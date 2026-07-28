# 3 · Terms and Rewriting

*Prerequisites: [2 · Numbers from Nothing](tutorial-numbers.md).*

So far every term was consumed by `≡` and vanished into a normal form.
This tutorial works with terms as objects: how they are built, how the
simplifier reduces them, and — the payoff — how to teach zelph an operator
it has never heard of, in two rules.

## A term is graph structure, and sorts are facts

There is no expression type. A term is built from the same facts as
everything else, and its leaves need a declared **sort**:

```
zelph> .import math
zelph> x ~ symvar        # an indeterminate
zelph> c ~ symconst      # an opaque constant
```

Binary operations reuse the *same predicates as the numeric modules*. A
symbolic `(x + y)` and a numeric `(&2 + &3)` are knowledge about the same
`+` node. That sharing is not an accident; it is what makes constant
folding free, as you will see in a moment.

Unary functions are **application facts** `(f of u)`, with the function
symbol as a first-class node. `math-syntax` writes them `f(u)` in both
directions:

```
zelph> $( exp(x) ) ~ probe
```

builds exactly `((exp of x) ~ probe)`.

Declaring sorts is not politeness. An undeclared atom gets no normal form
at all, and the request stays unanswered:

```
zelph> ? :simplify undeclared
zelph>
```

Silence, never a wrong answer — the same discipline as everywhere in the
standard library.

## The simplifier

`:simplify T` is the request; the answer comes back under `=`:

```
zelph> ? :simplify $( (x + 0) * 1 )
Answer: (:simplify ((x + &0) * &1)) = x
zelph> ? :simplify $( x / 1 + 0 * c )
Answer: (:simplify $( x / &1 + &0 * c )) = x
zelph> ? :simplify $( exp(ln(x)) )
Answer: (:simplify $( exp(ln(x)) )) = x
```

The last one is worth pausing on. There is no table of function inverses in
the engine. There are two facts,

```
exp inverseof ln
ln inverseof exp
```

and *one* generic rule in
[`symbolic-core.zph`](symbolic.md) that consumes them:

```
(F inverseof G, T red (F of (G of U))) => (T rw U)
```

Declare a new inverse pair and it collapses too. Declare none, and nothing
breaks — an unknown function symbol simply has no rewrites.

The pipeline is bottom-up: mark the term and all its subterms, reduce
leaves to themselves, rebuild each node from its already-normal children
(the *reduced form*), apply rewrite rules to that, and expose the result.
Because zelph hash-conses, a node whose children were already normal
reduces to *itself* — the machinery notices that for free.

## Constant folding without an arithmetic bridge

```
zelph> ? :simplify $( (2+3) * (4+6) )
Answer: (:simplify ((&2 + &3) * (&4 + &6))) = &50
```

No arithmetic was added to the simplifier to make this work. The reduced
form of `(&2 + &3)` **is** an ordinary `+` fact, so the arithmetic module's
trigger fires on it and derives `= &5` — and one bridge rule adopts
whatever the graph knows:

```
(T red C, C = R) => (T rw R)
```

Then congruence materialises the fresh fact `(&5 * &10)` *mid-simplification*,
the numeric cascade answers that too, and the bridge fires again.

The rule is deliberately more general than constant folding: it consumes
**any** equational fact about the reduced form. An equation imported from a
knowledge graph drives simplification exactly like a computed one. Knowledge
and computation are one substrate — this single line is where that stops
being a slogan.

Partiality composes through it unchanged:

```
zelph> ? :simplify $( 5 / 0 )
Answer: (:simplify (&5 / &0)) = (&5 / &0)
```

`&5 / &0` derives no `=` fact, matches no rewrite rule, and falls back to
itself. Undefinedness stays visible instead of folding to a wrong value.

## Why there is no commutativity rule

A forward-chaining engine is **monotonic**: it never deletes. A rule
`(X + Y) => (Y + X)` would therefore not *normalise* anything — it would
double the term space, permanently. Associativity plus congruence would be
worse.

So every rewrite rule in the standard library is directed and
measure-reducing, both orientations of a symmetric identity are spelled out
explicitly (`X + &0` *and* `&0 + X`), and the whole machinery is gated
behind markers so it never touches numeric facts.

The contract for anyone adding a rule: **every rewrite right-hand side must
already be a normal form** — a leaf, or built from normal children of the
reduced form. That is what makes one bottom-up pass sufficient and the
result single-valued. Distributivity violates it and is deliberately absent.

Which raises the obvious question: if the simplifier will not expand
products, how was `(1+x)(1−x) ≡ 1−x²` proven in tutorial 1? It was not
proven by rewriting at all. Canonicalisation is the polynomial layer's job,
and it reaches it by a different route — see
[5 · Inside the Normal Form](tutorial-polynomials.md).

## Teaching zelph a new operator

Here is the part that a specialised CAS cannot offer.

Take the **circle operation** of ring theory, x ∘ y = x + y + xy. It is the
operation under which the elements of the Jacobson radical form a group,
and it is not built into anything.

First give it notation — one call, which registers it with the island
grammar *and* the island printer:

```
zelph> .import math
zelph> <x y z> ~ polyring
zelph> %(math-syntax/operator "circ" 15)
```

`15` is the precedence: between `+` (10) and `*` (20). Then give it
meaning, in two rules:

```
zelph> ((U circ V) needstopoly (U circ V)) => (((U + V) + (U * V)) needstopoly ((U + V) + (U * V)))
zelph> ((U circ V) needstopoly (U circ V), ((U + V) + (U * V)) aspoly P) => ((U circ V) aspoly P)
```

The first rule says: when asked to compile `U ∘ V`, also ask for its
defining term. The second: whatever normal form the defining term reached,
adopt it. This is *delegation* — the pattern
[`eml.zph`](eml.md) uses for its whole macro chain — and it is all the
polynomial layer needs.

zelph now knows the operation, in your notation:

```
zelph> ? :topoly $( x circ y )
Answer: (:topoly (x circ y)) = (x poly <(y poly <(pos zint &0) (pos zint &1)>) (y poly <(pos zint &1) (pos zint &1)>)>)
```

Read the normal form: the x⁰ coefficient is `(y poly ⟨0, 1⟩)` = y, the x¹
coefficient is `(y poly ⟨1, 1⟩)` = 1 + y. So x ∘ y = y + x(1 + y) =
x + y + xy — which you can also just ask:

```
zelph> ? $( x circ y ) ≡ $( x + y + x*y )
Answer: ((x circ y) ≡ $( x + y + x * y )) = proven
```

And the laws come out as theorems, not assumptions:

```
zelph> ? $( (x circ y) circ z ) ≡ $( x circ (y circ z) )
Answer: ($( x circ y circ z ) ≡ (x circ (y circ z))) = proven
zelph> ? $( x circ y ) ≡ $( y circ x )
Answer: ((x circ y) ≡ (y circ x)) = proven
zelph> ? $( x circ 0 ) ≡ $( x )
Answer: ((x circ &0) ≡ x) = proven
```

Associativity, commutativity, and a neutral element — a commutative monoid,
verified over ℤ[x, y, z], from three lines you typed into a REPL. Nothing
was recompiled and no plug-in interface was involved, because there is no
interface: rules about `circ` are the same kind of object as rules about
`+`.

Notice the first answer: you wrote `$( (x circ y) circ z )` and zelph
printed `$( x circ y circ z )`, dropping the parentheses its own precedence
declaration makes redundant — and that output parses back to the same node.

!!! note "One table, two directions"
    The island grammar and the island display scheme are generated from a
    single operator table, which is why `math-syntax/operator` is the way
    to extend the notation. Registering an operator for display alone —
    with `zelph/set-infix-display` on the `math-syntax` scheme — would let
    zelph print island syntax its own parser refuses to read.

    Word-shaped operators are matched with an identifier boundary, so
    `circ` never matches inside `circle`, and they need surrounding
    whitespace. Operators sharing a precedence must share an
    associativity; mixing them is an error rather than a silent choice.
    Prefix and postfix operators are not user-definable.

## Extending the simplifier instead

The two rules above taught the *polynomial compiler*. To teach the
*simplifier* — so that `:simplify` reduces terms containing your operator —
you contribute three things instead: decompose rules that propagate the
`needssimp` marker to subterms, a congruence rule that builds the reduced
form, and rewrite rules on reduced forms. `symbolic-core`'s connect stage
and identity fallback then work unchanged.

That is the **operator extension protocol**, and the standard library uses
it four times: for `-` and unary negation
([`symbolic-minus`](symbolic.md#subtraction-and-negation)), for `^`
([`symbolic-pow`](symbolic.md#exponentiation)), for ℤ numerals
([`symbolic-integers`](symbolic.md#integers-as-leaves)), and for the EML
operator ([`eml`](eml.md)). Any of the four is a readable template.

## Exercises

1. Declare `sq inverseof sqrt` and check that `:simplify $( sq(sqrt(x)) )`
   collapses. Then think about what you have actually asserted — for which
   x is it true? (The standard library calls this out: identities are
   **formal**, side conditions are not tracked.)
2. Teach zelph the operation x ⋄ y = x + y − xy — notation with
   `math-syntax/operator`, meaning with the same two-rule delegation — and
   prove it is associative and commutative with neutral element 0. What is
   its relation to `circ`?
3. Why does `? :simplify $( (x + y) * (x - y) )` not answer `x² − y²`?
   Which rule would you have to add, and which contract would it break?
4. `? :simplify $( 0 * (5 / 0) )` — predict the answer before running it.

## Next

[4 · Differentiation](tutorial-differentiation.md) puts the term layer to
work, and shows how far you can push it with rules of your own.
