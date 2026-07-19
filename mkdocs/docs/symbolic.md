zelph performs symbolic mathematics — term simplification, symbolic
differentiation, and compilation into a single-operator basis — purely inside
its reasoning engine. As with [arithmetic](arithmetic.md), there is no
computer-algebra code in the C++ core: terms are ordinary graph structure,
rewrite rules are ordinary forward-chaining rules, and every simplified form
and every derivative arrives as an ordinary fact carrying its derivation
(`⇐`). This page delivers the first slice of the symbolic half of the
mathematics-engine roadmap announced in
[Semantic Arithmetic](arithmetic.md#why-this-is-more-than-a-demo).

The modules live in the standard library and stack on top of any arithmetic
module (the import order matters: `&`-literals in the symbolic scripts are
parsed by the loaded `zelph/number` at import time):

- [`stdlib/symbolic-core.zph`](https://github.com/acrion/zelph/blob/main/stdlib/symbolic-core.zph) — term sorts, the simplification core, knowledge folding
- [`stdlib/diff.zph`](https://github.com/acrion/zelph/blob/main/stdlib/diff.zph) — symbolic differentiation
- [`stdlib/eml.zph`](https://github.com/acrion/zelph/blob/main/stdlib/eml.zph) — case study: the EML Sheffer operator

```
zelph> .import arithmetic       # or binary-arithmetic / binary-nand-arithmetic
zelph> .import symbolic-core
zelph> .import diff
zelph> x ~ symvar
zelph> (x * x) diffby x
zelph> ((x * x) diffby x) = D
Answer: (( x * x ) diffby x ) = ( x + x )
```

The derivative was not computed by a differentiation routine; it was _derived_,
the same way `Berlin is located in Europe` follows from a transitivity rule —
and like every derived fact, it persists: repeating the query answers from the
graph.

## Terms Are Graph Structure

A symbolic term is nothing new. Binary operations use the **same predicates as
the numeric modules** — a symbolic `(x + y)` and a numeric `(&2 + &3)` are
knowledge about the same `+` node. Symbolic atoms are named nodes with
declared sorts:

```
x ~ symvar        # variables
c ~ symconst      # opaque base constants
```

Unary functions have no dedicated fact arity in zelph; they are **application
facts** `(exp of x)` with the function symbol as a first-class node. This is
the meta-rule pattern from [Logic and Computation](logic.md#meta-rules-predicates-as-first-class-nodes)
transplanted to function symbols: properties of functions are ordinary facts,

```
exp inverseof ln
ln inverseof exp
```

and a _single_ generic rule collapses `f(f⁻¹(u))` for **all** declared pairs.
New function symbols work out of the box — undeclared ones simply have no
rewrites.

Numerals — the cons-lists of the loaded arithmetic module — are opaque leaves
here. The symbolic layer is representation-agnostic: its `&0`/`&1` leaves are
whatever the loaded module builds, which is why the test suite runs every
symbolic test against all three arithmetic modules.

Declaring sorts is not optional politeness: an undeclared atom gets no normal
form at all, and the simplification request stays silently unanswered.
Partiality by absence — silence, never a wrong answer.

## The Simplification Core

The user-facing idiom mirrors `testprime`: a self-fact seeds the work, a
query retrieves the result repeatably.

```
zelph> (x + &0) simplify (x + &0)
zelph> ((x + &0) simplify (x + &0)) = X
Answer: (( x + &0 ) simplify ( x + &0 )) = x
```

Internally the module follows the arithmetic architecture, with one new stage:

1. **Trigger** — `(T simplify T)` marks the term (`needssimp`, the
   [`needscanon`](arithmetic.md) pattern generalized to terms).
2. **Decompose** — markers propagate to all subterms.
3. **Base cases** — declared atoms and numerals are their own normal forms.
4. **Congruence** — once the children are normal, the context-reduced form is
   built: `(T red (P + Q))`. Thanks to hash-consing, a term whose children
   were already normal reduces to _itself_ — a self-fact `(T red T)`.
5. **Rewrite** — directed rules on reduced forms produce `(T rw S)`: neutral
   and absorbing elements, the generic inverse-pair rule, knowledge folding
   (below), and any operator extensions such as `eml.zph`.
6. **Connect** — the rewrite result if one exists, otherwise the reduced form
   itself; exposed under `=`.

Two design decisions deserve emphasis.

**No free commutativity or associativity.** A monotonic engine never deletes:
freely commuting rules would double every term, and associativity plus
congruence would blow up the term space. Instead, every rewrite rule is
directed and measure-reducing, both orientations of symmetric identities are
spelled out explicitly (`X + &0` _and_ `&0 + X`), and the whole machinery is
gated by markers so it never touches numeric facts. Equality saturation in
the e-graph style — which is, at heart, forward chaining over equalities — is
a natural future experiment, not the foundation.

**The normal-form contract.** Every rewrite right-hand side must already be a
normal form — a leaf, or built from normal children of the reduced form. All
shipped rules satisfy this, which is why one bottom-up pass suffices and
`simp` is single-valued. Rules violating the contract (distributivity, say)
need an iterated design and must not be added casually.

The module's only negation is the **identity fallback**: a term whose reduced
form has no rewrite is its own normal form,

```
(T red C, ¬(T rw S)) => (T simp C)
```

deferred by [stratified evaluation](logic.md#stratified-evaluation) until the
positive rules — including the entire numeric cascade feeding knowledge
folding — have reached quiescence. Fallback results feed parent congruences,
which re-open the positive stratum; the alternating schedule makes chains
like `exp(ln(x + 0)) → x` sound across multiple deferred rounds.

## Knowledge Folding

Constant folding needs no dedicated machinery, because the reduced form
**is** an ordinary `+`/`*`/`/` fact: the arithmetic module's triggers fire on
it automatically, derive its `=` result, and one bridge rule adopts it:

```
(T red C, C = R) => (T rw R)
```

```
zelph> ((&2 + &3) * (&4 + &6)) simplify ((&2 + &3) * (&4 + &6))
zelph> (((&2 + &3) * (&4 + &6)) simplify ((&2 + &3) * (&4 + &6))) = X
Answer: ((...) simplify (...)) = &50
```

The inner sums fold to `&5` and `&10`; congruence then materializes the fresh
fact `(&5 * &10)` _mid-simplification_, whose numeric cascade the bridge
consumes — the same cross-module mechanism by which
[multiplication delegates to addition](arithmetic.md#the-four-operations).

The rule is deliberately more general than constant folding: it consumes
_any_ equational fact about the reduced form — computed by the arithmetic
modules, or declared. An equation imported from a knowledge graph drives
simplification exactly like a computed one; knowledge and computation remain
one substrate. (Declared equations must respect the normal-form contract on
their right-hand side, and conflicting declarations would break
single-valuedness.)

Confluence with the plain rewrites holds on every overlap because numeric
results of canonical operands are canonical — addition and multiplication by
construction, division via `canonnum`. And partiality composes: `&5 / &0`
derives no `=` fact, matches no rewrite, and falls back to itself —
undefinedness stays visible instead of folding to a wrong value.

## Symbolic Differentiation

[`stdlib/diff.zph`](https://github.com/acrion/zelph/blob/main/stdlib/diff.zph)
implements sum, product, and chain rule as Trigger / Decompose / Assemble /
Connect blocks. Function derivatives are again _facts about function
symbols_ (`exp hasderivative exp`), consumed by one generic chain rule; `ln`
has a dedicated rule because its derivative `1/u` is not of the form `g(u)`
for a named symbol.

Constancy is the textbook definition, executable: a positive containment
recursion plus negation-as-failure — the
[`primes-naf`](arithmetic.md#from-arithmetic-to-number-theory-primality)
pattern:

```
(T dstate X, X ~ symvar, ¬(T contains X)) => ((T wrt X) deriv &0)
```

Numerals need no special rule: nothing derives `(&n contains X)`, so they are
constants by absence. Constant composites are reached by _two_ derivation
paths — the NAF short-circuit and the structural rules over `&0`-children —
and both collapse to the same `&0` through the simplifier, keeping the
exposed result single-valued.

That pipeline is the module's connect stage: the raw derivative is not the
exposed result. It is pushed through an ordinary `(D simplify D)` request —
another cross-module cascade — so `d(x + c)/dx` answers `&1` (not
`&1 + &0`), and `d(x + x)/dx` answers `&2` (the raw `&1 + &1` folded through
the numeric module):

```
zelph> (x + c) diffby x
zelph> ((x + c) diffby x) = D
Answer: (( x + c ) diffby x ) = &1
zelph> ((ln of x) diffby x) = D
Answer: (( ln of x ) diffby x ) = (&1 / x )
```

## Case Study: The EML Sheffer Operator

In _All elementary functions from a single binary operator_
([arXiv:2603.21852](https://arxiv.org/abs/2603.21852)), Odrzywołek shows that
the single operator `eml(x, y) = exp(x) − ln(y)`, together with the constant
1, generates the entire repertoire of a scientific calculator — a Sheffer
stroke for continuous mathematics, as NAND is for Boolean logic. (The NAND
analogy is taken literally elsewhere in the standard library:
[`binary-nand-arithmetic.zph`](https://github.com/acrion/zelph/blob/main/stdlib/binary-nand-arithmetic.zph)
derives all digit tables of binary arithmetic from a single NAND axiom.)

[`stdlib/eml.zph`](https://github.com/acrion/zelph/blob/main/stdlib/eml.zph)
makes `eml` a first-class symbolic operator via the **operator extension
protocol** — the three contributions any new operator makes to the
simplifier: decompose rules, a congruence rule, and rewrite rules on reduced
forms. The connect stage and the identity fallback work unchanged.

The rewrite rules are an **identity table** taken from the paper: they
eliminate `eml` in favor of named functions. The flagship is Eq. (5),
`ln z = eml(1, eml(eml(1, z), 1))` — and zelph derives it:

```
zelph> .import eml
zelph> x ~ symvar
zelph> (&1 eml ((&1 eml x) eml &1)) simplify (&1 eml ((&1 eml x) eml &1))
zelph> ((&1 eml ((&1 eml x) eml &1)) simplify (&1 eml ((&1 eml x) eml &1))) = X
Answer: (...) = ( ln of x )
```

The identity is not checked numerically and not assumed: it is _derived_, as
a chain of ordinary deductions by the same fixpoint engine that reasons over
Wikidata — with the full `⇐` provenance attached.

**Identity patterns match normalized forms.** A subtlety worth recording for
future identity-table authors: because simplification is bottom-up, the
literal Eq.-(5) tree never reaches the top-level rewrite stage intact — its
inner `(… eml &1)` has already been reduced to `(exp of …)` by the exp rule.
Table patterns must therefore be stated over bottom-up-normalized forms; the
ln rule recognizes `(&1 eml (exp of (&1 eml X)))`, matched as a single deep
condition by [deep unification](logic.md#deep-unification).

**Single-pass semantics.** Rewrite results are not re-processed within a
request, so `e = eml(1, 1)` reduces in two requests: `(&1 eml &1)` answers
`(exp of &1)`, and simplifying _that_ answers `e`. `simp` stays
single-valued throughout; one-shot deep normalization is the future
iterated/e-graph design.

**The compiler.** The expansion direction, `(T emlcompile T)`, builds
pure-EML forms bottom-up under the same architecture and answers the
repeatable query `((T emlcompile T) = F)`. Compile-and-reduce round trips
close for `exp` and `ln`:

```
zelph> (ln of x) emlcompile (ln of x)
zelph> ((ln of x) emlcompile (ln of x)) = F
Answer: (( ln of x ) emlcompile ( ln of x )) = (&1 eml ((&1 eml x ) eml &1))
```

Coverage is deliberately strict — `exp`, `ln`, `eml` itself, and leaves;
anything else yields no `emlform` rather than a form falsely claimed to be
pure EML. Extending it along the paper's verified discovery chain
(Supplementary Information, Table S2) is mechanical per-constructor work and
is the module's roadmap. A structural bonus falls out of the engine: since
all terms are hash-consed, EML trees are canonical DAGs — repeated
subexpressions are stored, matched, and rewritten once.

## Coexistence with the Numeric Substrate

Sharing predicates has a price, paid deliberately: the numeric triggers fire
on symbolic facts and create dead internal states (`((x add y) ci 0)`), which
are harmless — digit recursion cannot decompose atoms — and small. In return,
the derivative scaffolding of `diff.zph` consists of real `+`/`*` facts whose
numeric results appear in the same graph, which is what makes knowledge
folding a one-rule bridge instead of an integration layer.

## Scope and Honest Limitations

Identities are **formal**: `exp inverseof ln` assumes the principal branch
and `u > 0` over the reals, `&0 / X` ignores division by zero; side
conditions are not tracked yet. Knowledge folding computes over the naturals
(the current numeric substrate). Rewriting is single-pass per request, and
the operator set is young — subtraction, negation, and powers are next on the
list, driven by the EML compiler extension.

## Testing

`src/test/test_symbolic.cpp` runs every case against all three arithmetic
modules and both parallelism modes, permanently in `.semi-naive check` mode —
so the stratified schedule of the identity fallback is continuously verified
against classic evaluation. Assertions are structural (`zelph/exists` probes
on graph nodes), not string comparisons: symbolic results are checked as the
nodes they are.
