# The Symbolic Layer

Modules:
[`symbolic-core`](https://github.com/acrion/zelph/blob/main/stdlib/symbolic-core.zph),
[`symbolic-minus`](https://github.com/acrion/zelph/blob/main/stdlib/symbolic-minus.zph),
[`symbolic-pow`](https://github.com/acrion/zelph/blob/main/stdlib/symbolic-pow.zph),
[`symbolic-integers`](https://github.com/acrion/zelph/blob/main/stdlib/symbolic-integers.zph),
[`diff`](https://github.com/acrion/zelph/blob/main/stdlib/diff.zph)
· Prerequisite: an [arithmetic substrate](arithmetic.md)

Term simplification and symbolic differentiation, purely inside the
reasoning engine. As with [arithmetic](arithmetic.md), there is no
computer-algebra code in the C++ core: terms are ordinary graph structure,
rewrite rules are ordinary forward-chaining rules, and every simplified
form and every derivative arrives as an ordinary fact carrying its
derivation (`⇐`).

The import order matters — the `&`-literals in these scripts are parsed by
the loaded `zelph/number` at import time — but [`math`](frontend.md) takes
care of it.

```
zelph> .import math
zelph> x ~ symvar
zelph> ? (x * x) diffby x
Answer: ((x * x) diffby x) = (x + x)
```

The derivative was not computed by a differentiation routine; it was
*derived*, the same way `Berlin is located in Europe` follows from a
transitivity rule — and like every derived fact, it persists.

## Terms Are Graph Structure

A symbolic term is nothing new. Binary operations use the **same predicates
as the numeric modules**: a symbolic `(x + y)` and a numeric `(&2 + &3)`
are knowledge about the same `+` node. Symbolic atoms are named nodes with
declared sorts:

```
x ~ symvar        # variables
c ~ symconst      # opaque base constants
```

Unary functions have no dedicated fact arity in zelph; they are
**application facts** `(exp of x)` with the function symbol as a
first-class node. This is the meta-rule pattern from
[Logic and Computation](../logic.md#meta-rules-predicates-as-first-class-nodes)
transplanted to function symbols: properties of functions are ordinary
facts,

```
exp inverseof ln
ln inverseof exp
```

and a *single* generic rule collapses f(f⁻¹(u)) for **all** declared pairs.
New function symbols work out of the box; undeclared ones simply have no
rewrites.

Numerals — the cons lists of the loaded arithmetic module — are opaque
leaves here. The layer is representation-agnostic: its `&0`/`&1` leaves are
whatever the loaded module builds, which is why the test suite runs every
symbolic test against all three arithmetic modules.

Declaring sorts is not optional politeness: an undeclared atom gets no
normal form at all, and the request stays silently unanswered. Partiality
by absence — silence, never a wrong answer.

## The Simplification Core

The user-facing idiom mirrors `testprime`: a request self-fact seeds the
work — entered with the [self-fact prefix `:`](../index.md#the-self-fact-prefix)
— and a query retrieves the result repeatably.

```
zelph> ? :simplify (x + &0)
Answer: (:simplify (x + &0)) = x
```

Internally the module follows the arithmetic architecture, with one new
stage:

1. **Trigger** — the request self-fact `(T simplify T)` marks the term
   (`needssimp`, the [`needscanon`](arithmetic.md) pattern generalised to
   terms).
2. **Decompose** — markers propagate to all subterms.
3. **Base cases** — declared atoms and numerals are their own normal forms.
4. **Congruence** — once the children are normal, the context-reduced form
   is built: `(T red (P + Q))`. Thanks to hash-consing, a term whose
   children were already normal reduces to *itself* — a self-fact
   `(T red T)`.
5. **Rewrite** — directed rules on reduced forms produce `(T rw S)`.
6. **Connect** — the rewrite result if one exists, otherwise the reduced
   form itself; exposed under `=`.

Two design decisions deserve emphasis.

**No free commutativity or associativity.** A monotonic engine never
deletes: freely commuting rules would double every term, and associativity
plus congruence would blow up the term space. Instead, every rewrite rule
is directed and measure-reducing, both orientations of symmetric identities
are spelled out explicitly (`X + &0` *and* `&0 + X`), and the whole
machinery is gated by markers so it never touches numeric facts. Equality
saturation in the e-graph style — which is, at heart, forward chaining over
equalities — is a natural future experiment, not the foundation.

**The normal-form contract.** Every rewrite right-hand side must already be
a normal form — a leaf, or built from normal children of the reduced form.
All shipped rules satisfy this, which is why one bottom-up pass suffices
and `simp` is single-valued. Rules violating the contract (distributivity,
say) need an iterated design and must not be added casually.

The module's only negation is the **identity fallback**: a term whose
reduced form has no rewrite is its own normal form,

```
(T red C, ¬(T rw S)) => (T simp C)
```

deferred by [stratified evaluation](../logic.md#stratified-evaluation)
until the positive rules — including the entire numeric cascade feeding
knowledge folding — have reached quiescence. Fallback results feed parent
congruences, which re-open the positive stratum; the alternating schedule
makes chains like exp(ln(x + 0)) → x sound across multiple deferred rounds.

### Knowledge Folding

Constant folding needs no dedicated machinery, because the reduced form
**is** an ordinary `+`/`*`/`/` fact: the arithmetic module's triggers fire
on it automatically, derive its `=` result, and one bridge rule adopts it:

```
(T red C, C = R) => (T rw R)
```

```
zelph> ? :simplify ((&2 + &3) * (&4 + &6))
Answer: (:simplify ((&2 + &3) * (&4 + &6))) = &50
```

The inner sums fold to `&5` and `&10`; congruence then materialises the
fresh fact `(&5 * &10)` *mid-simplification*, whose numeric cascade the
bridge consumes — the same cross-module mechanism by which
[multiplication delegates to addition](arithmetic.md#the-four-operations).

The rule is deliberately more general than constant folding: it consumes
*any* equational fact about the reduced form — computed by the arithmetic
modules, or declared. An equation imported from a knowledge graph drives
simplification exactly like a computed one; knowledge and computation
remain one substrate. (Declared equations must respect the normal-form
contract on their right-hand side, and conflicting declarations would break
single-valuedness.)

Confluence with the plain rewrites holds on every overlap because numeric
results of canonical operands are canonical — addition and multiplication
by construction, division via `canonnum`. And partiality composes:
`&5 / &0` derives no `=` fact, matches no rewrite, and falls back to itself
— undefinedness stays visible instead of folding to a wrong value.

### Constant Reassociation

The one associativity rule in the module, admitted as an exception:

```
(T red ((A cons R) * ((B cons S) * U)))     => ((A cons R) * (B cons S))
(T red ((A cons R) * ((B cons S) * U)),
 ((A cons R) * (B cons S)) = K, K != &0)    => (T rw (K * U))
```

It exists because iterated differentiation assembles its constants in
layers — the third derivative of x⁵ arrives as 5·(4·(3·x²)) — and every
factor being correct is no comfort if the result is unreadable. It is
measure-reducing (two products become one), its right-hand side is already
a normal form, and both factors are restricted to numerals **by pattern**,
so symbolic operands are left alone and no spurious facts are created.

The guard sits on the result: `K` is `&0` exactly when a factor is, which
is the case the absorbing rules already answer, and answer differently.
[`symbolic-integers`](#integers-as-leaves) carries the ℤ counterpart.

## Operator Modules

Each of the following contributes the three parts of the **operator
extension protocol** — decompose, congruence, rewrites — plus, where
differentiation applies, containment and assemble rules and a `ddom`
declaration. [`eml`](eml.md) is the reference application of the same
protocol from outside the core.

### Subtraction and Negation

`symbolic-minus` adds the binary `-` and unary negation.

```
zelph> ? :simplify $( x - 0 )
Answer: (:simplify (x - &0)) = x
zelph> ? :simplify $( -(-x) )
Answer: (:simplify $( neg(neg(x)) )) = x
zelph> ? :simplify (&5 - &3)
Answer: (:simplify (&5 - &3)) = &2
```

`neg` needs no decompose or congruence rules of its own — `(neg of U)` is
an ordinary application fact, covered by the core's generic `(F of U)`
rules. Its involution needs no rule either: one `neg inverseof neg`
declaration feeds the existing generic inverse-pair rule.

Numeric folding is inherited, not implemented. `(&3 - &5)` derives nothing
over the naturals and falls back to itself — until `symbolic-integers` is
loaded, which completes exactly that partiality.

**Deliberate omission:** there is no `(T red (X - X)) => (T rw &0)` rule.
With the ℤ façade loaded, `((pos zint A) - (pos zint A))` folds to the zint
zero; an `X - X` rule would additionally rewrite to the *natural* zero —
two `rw` values for one reduced form. Cancelling equal symbolic terms is
the [polynomial layer's](topoly.md) job, not a local rewrite's.

### Exponentiation

`symbolic-pow` adds `^`.

```
zelph> ? :simplify $( x^1 )
Answer: (:simplify (x ^ &1)) = x
zelph> ? :simplify $( x^0 )
Answer: (:simplify (x ^ &0)) = &1
zelph> ? :simplify (&2 ^ &10)
Answer: (:simplify (&2 ^ &10)) = &1024
```

**The exponent is not a subterm.** `(U ^ N)` decomposes only its base: `N`
is a numeral literal, and rewriting it would be meaningless. That also
keeps the congruence rule single-conditioned.

**x² and x·x are different nodes.** Local rewrites deliberately do *not*
expand powers into products — that would reintroduce the blow-up `^` exists
to avoid. Their equality is a statement of the [polynomial layer](topoly.md).

The power rule d(uⁿ)/dx = n·uⁿ⁻¹·du/dx lives here too. The exponent-zero
case gets its own rule: n−1 has no natural value there, so the general rule
stays silent, and the constancy fallback cannot step in either, because
`(x ^ &0)` *does* contain x structurally. An in-system comparison makes the
two cases mutually exclusive, so `deriv` stays single-valued. n = 1 needs
no special rule: it composes.

### Integers as Leaves

`symbolic-integers` makes ℤ numerals first-class symbolic leaves and adds
the ℕ → ℤ promotions that complete natural partiality.

```
zelph> ? :simplify (x + (pos zint &0))
Answer: (:simplify (x + (pos zint &0))) = x
zelph> ? :simplify (neg of (pos zint &3))
Answer: (:simplify (neg of (pos zint &3))) = (neg zint &3)
zelph> ? :simplify (&3 - &5)
Answer: (:simplify (&3 - &5)) = (neg zint &2)
```

Numeric folding over ℤ needs no rules here at all: a reduced form over two
zint leaves *is* an ordinary `+`/`-`/`*` fact, the
[façade](integers.md#the-uniform-operator-facade) routes it and re-exposes
the result under `=`, and the knowledge-folding bridge adopts it. This
module only contributes what the façade cannot — leaf status,
neutral/absorbing identities against *symbolic* operands, negation of
numerals, and the promotions.

**Mixed worlds coexist.** The naturals' identity rules (`X + &0`, `X * &1`)
and the zint rules live side by side — different zero and one nodes for
different number worlds. That is what lets `diff`'s natural `&0`/`&1` seeds
work unchanged over ℤ coefficients: d((neg zint &3)·x)/dx assembles
`((&0 * x) + ((neg zint &3) * &1))`, and the *natural* identities reduce it
to `(neg zint &3)`.

## Differentiation

`diff` implements sum, product and chain rule as Trigger / Decompose /
Assemble / Connect blocks.

```
zelph> ? (x * c) diffby x
Answer: ((x * c) diffby x) = c
zelph> ? ((exp of x) * x) diffby x
Answer: ($( exp(x) * x ) diffby x) = $( exp(x) * x + exp(x) )
zelph> ? ((ln of x) diffby x)
Answer: ($( ln(x) ) diffby x) = (&1 / x)
```

Function derivatives are again *facts about function symbols*
(`exp hasderivative exp`), consumed by one generic chain rule; `ln` has a
dedicated rule because its derivative 1/u is not of the form g(u) for a
named symbol.

The raw derivative is deliberately **not** the exposed result: the connect
stage asserts an ordinary `:simplify` request on it, the core answers that,
and the simplified form is exposed under `=`. That is why d(x + c)/dx
answers `&1` and not `&1 + &0`.

### Constancy, and Its Guard

Constancy is the textbook definition, executable — a positive containment
recursion plus negation-as-failure, the
[`primes-naf`](primality.md#primes-naf-the-textbook-formulation) pattern:

```
(T dstate X, X ~ symvar, T ddom T, ¬(T contains X)) => ((T wrt X) deriv &0)
```

Numerals need no special rule: nothing derives `(&n contains X)`, so they
are constant by absence. Constant composites are reached by *two*
derivation paths — the NAF short-circuit and the structural rules over
`&0`-children — and both collapse to the same `&0` through the simplifier,
keeping the exposed result single-valued.

`T ddom T` is the **shape domain**, and it is what makes the negation
sound. `contains` walks exactly the forms the decompose rules cover, while
the trigger hands a `dstate` fact to *whatever* was requested. Ungated, a
shape `diff` knows nothing about would fail the containment test for the
trivial reason that no rule could ever have derived it — and be declared
constant. So membership is asserted positively, one rule per known form,
and the fallback is gated on it:

```
(T dstate X, T ~ symvar)   => (T ddom T)
(T dstate X, T ~ symconst) => (T ddom T)
((A cons R) dstate X)      => ((A cons R) ddom (A cons R))
((U + V) dstate X)         => ((U + V) ddom (U + V))
((U * V) dstate X)         => ((U * V) ddom (U * V))
((F of U) dstate X)        => ((F of U) ddom (F of U))
```

with `-` and `neg` declared by `symbolic-minus`, `^` by `symbolic-pow` and
zint leaves by `symbolic-integers`. This is the protocol's fourth
contribution: an operator joining differentiation declares its shape here,
exactly as it declares its decompose, containment and assemble rules.

The effect is that `(x / c)` and any foreign predicate stay **silent**
rather than answering a confident `&0`.

Note the variable positions. A `dstate` fact is `(TERM dstate VARIABLE)`,
so the leaf rules must not be written as `(X dstate X, X ~ symvar)` — that
self-fact form matches only a leaf differentiated with respect to *itself*,
and every other leaf would lose its constancy answer.

### Iterated Differentiation

```
(T diffalong <x y>) = E     d/dy (d/dx T), reading order
(T diffalong <x x>) = E     the second derivative
(T diffalong nil)   = T
```

Four rules, and no new machinery: each step is an ordinary `diffby` request
whose `=` result feeds the next. A cons list rather than extra objects on
the fact, because a zelph fact carries a **set** of objects — `(T d x y)`
and `(T d y x)` would be the same node, and order has to live in the data.

Partiality composes: if any step is silent, the chain is. Mixed partials in
both orders reach the same answer node, so the Schwarz/Clairaut symmetry is
observable as node identity rather than assumed.

## Coexistence with the Numeric Substrate

Sharing predicates has a price, paid deliberately: the numeric triggers
fire on symbolic facts and create dead internal states (`((x add y) ci 0)`),
which are harmless — digit recursion cannot decompose atoms — and small. In
return, the derivative scaffolding consists of real `+`/`*` facts whose
numeric results appear in the same graph, which is what makes knowledge
folding a one-rule bridge instead of an integration layer.

## Scope and Honest Limitations

Identities are **formal**: `exp inverseof ln` assumes the principal branch
and u > 0 over the reals, and `&0 / X` ignores division by zero. Side
conditions are not tracked — declare only directions you accept.

Rewriting is single-pass per request: a rewrite result is not re-processed
within the same request, which is why e = eml(1, 1) takes
[two requests](eml.md#the-identity-table) to reduce.

There is no quotient rule, so d(u/v)/dx is silent — an operator extension
away, but not shipped.

What is *not* a limitation of this layer: canonicalisation. Distributivity,
cancellation of equal symbolic terms, and the equality of x² with x·x
belong to [the polynomial layer](topoly.md), which decides them by
construction rather than by rewriting. Reaching for a distributivity
rewrite here would break the normal-form contract; reaching for `topoly`
costs one request.

## Testing

`src/test/test_symbolic.cpp` runs every case against all three arithmetic
modules and both parallelism modes, permanently in `.semi-naive check` mode
— so the stratified schedule of the identity fallback is continuously
verified against classic evaluation. Assertions are structural
(`zelph/exists` probes on graph nodes), not string comparisons: symbolic
results are checked as the nodes they are.

The engine machinery that turned these workloads from intractable into
routine — the layered fact-structure lookup, the acceleration stores, and
candidate-set anchoring — is documented for contributors in
[Internals: Performance Architecture](../internals/performance.md).
