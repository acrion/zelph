# 6 · Refuting the Jacobian Conjecture

*Prerequisites: [5 · Inside the Normal Form](tutorial-polynomials.md).*

This tutorial verifies a counterexample from July 2026 to a conjecture open
since 1939. Everything in it runs on the stack built in the previous five
tutorials — digit tables, positional arithmetic, signed integers, symbolic
terms, differentiation, polynomial normal forms — with no step performed
outside zelph.

## The conjecture

Let F : ℂⁿ → ℂⁿ be a polynomial map. Its Jacobian determinant det J_F is
again a polynomial. If F has a polynomial inverse, then det J_F must be a
nonzero constant — that direction is elementary. The **Jacobian
conjecture** (Keller, 1939) asserts the converse: a constant nonzero
Jacobian determinant forces F to be invertible, in particular injective.

In July 2026, Alpöge and Fable exhibited a map F : ℂ³ → ℂ³ with constant
Jacobian determinant −2 that is *not* injective: three distinct points share
one image. That refutes the conjecture for n ≥ 3.

The map is

- F₁ = (1 + xy)³z + y²(1 + xy)(4 + 3xy)
- F₂ = y + 3x(1 + xy)²z + 3xy²(4 + 3xy)
- F₃ = 2x − 3x²y − x³z

## Clearing the denominators

zelph's arithmetic is over ℤ, and that is not a limitation to work around
here — it is an opportunity. Substituting x = a, y = b/2, z = c/4 and
scaling the components by 32, 16 and 4 gives a map **G with integer
coefficients**:

- G₁ = (2 + ab)³c + 2b²(2 + ab)(8 + 3ab)
- G₂ = 8b + 3a(2 + ab)²c + 6ab²(8 + 3ab)
- G₃ = 8a − (6a²b + a³c)

The substitution is invertible and the scaling is by nonzero constants, so
nothing is lost. The collision points become P₁ = (0, 0, −1),
P₂ = (1, −3, 26), P₃ = (−1, 3, 26), all mapping to (−8, 0, 0). By the chain
rule,

det J_G = det diag(32,16,4) · det J_F · det diag(1, ½, ¼) = 2048 · (−2) / 8 = −512

and conversely, det J_G = −512 as a polynomial identity implies
det J_F = −2. So both halves of the refutation can be checked over the
integers, with no fractions anywhere.

## Part 1 — the collision

Evaluation needs no special machinery. A ground term is just a term whose
leaves are numerals, and `topoly` compiles it to a constant polynomial —
which is exactly its value.

```
zelph> .import math
zelph> ? :topoly $( (2+0*0)^3*(-1) + 2*0^2*(2+0*0)*(8+3*0*0) )
Answer: … = (neg zint &8)
zelph> ? :topoly $( 8*0 + 3*0*(2+0*0)^2*(-1) + 6*0*0^2*(8+3*0*0) )
Answer: … = (pos zint &0)
zelph> ? :topoly $( 8*0 - (6*0^2*0 + 0^3*(-1)) )
Answer: … = (pos zint &0)
```

G(P₁) = (−8, 0, 0). Now P₂ = (1, −3, 26):

```
zelph> ? :topoly $( (2+1*(-3))^3*26 + 2*(-3)^2*(2+1*(-3))*(8+3*1*(-3)) )
Answer: … = (neg zint &8)
zelph> ? :topoly $( 8*(-3) + 3*1*(2+1*(-3))^2*26 + 6*1*(-3)^2*(8+3*1*(-3)) )
Answer: … = (pos zint &0)
zelph> ? :topoly $( 8*1 - (6*1^2*(-3) + 1^3*26) )
Answer: … = (pos zint &0)
```

and P₃ = (−1, 3, 26):

```
zelph> ? :topoly $( (2+(-1)*3)^3*26 + 2*3^2*(2+(-1)*3)*(8+3*(-1)*3) )
Answer: … = (neg zint &8)
zelph> ? :topoly $( 8*3 + 3*(-1)*(2+(-1)*3)^2*26 + 6*(-1)*3^2*(8+3*(-1)*3) )
Answer: … = (pos zint &0)
zelph> ? :topoly $( 8*(-1) - (6*(-1)^2*3 + (-1)^3*26) )
Answer: … = (pos zint &0)
```

Three points, one image. And the points are genuinely distinct for the same
reason the images are equal: canonical numerals of different values are
different nodes. `(pos zint &1)` and `(neg zint &1)` are not the same node,
so P₂ ≠ P₃ needs no separate argument.

Note what did *not* happen. There was no evaluator, no substitution
mechanism, no numeric mode. The same `topoly` that proves symbolic
identities compiled a ground term, and a ground term's normal form is its
value.

## Part 2 — det J_G is the constant −512

This is the harder half: nine partial derivatives, a 3×3 determinant, and
the claim that the result is a constant *as a polynomial identity* — not at
sample points.

The derivatives are ordinary `diffby` requests:

```
zelph> <a b c> ~ polyring
zelph> ? $( (2+a*b)^3*c + 2*b^2*(2+a*b)*(8+3*a*b) ) diffby a
Answer: … = $( &3 * (&2 + a * b) ^ &2 * b * c + (&2 * b ^ &2 * b * (&8 + &3 * a * b) + &2 * b ^ &2 * (&2 + a * b) * (&3 * b)) )
zelph> ? $( 8*a - (6*a^2*b + a^3*c) ) diffby c
Answer: … = $( &0 - a ^ &3 )
```

Typing the cofactor expansion by hand from nine such results would be
error-prone and would prove nothing about zelph. Instead, let a **rule**
assemble it. The whole construction is:

```
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (F diffby X)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (F diffby Y)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (F diffby Z)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (G diffby X)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (G diffby Y)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (G diffby Z)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (H diffby X)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (H diffby Y)
((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) => (H diffby Z)

((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil))),
 (F diffby X) = _P, (F diffby Y) = _Q, (F diffby Z) = _R,
 (G diffby X) = _S, (G diffby Y) = _T, (G diffby Z) = _U,
 (H diffby X) = _V, (H diffby Y) = _W, (H diffby Z) = _N)
=> (((F cons (G cons (H cons nil))) jac3 (X cons (Y cons (Z cons nil)))) detis
    ((( _P * ((_T * _N) - (_U * _W)))
      - (_Q * ((_S * _N) - (_U * _V))))
      + (_R * ((_S * _W) - (_T * _V)))))

(M detis D) => (D topoly D)
(M detis D, (D topoly D) = P) => (M jdet P)
```

The first nine rules request the partials. The tenth is the first-row
cofactor expansion, written exactly as in a linear algebra text, with the
nine derivative results bound by their `=` facts. The last two compile the
assembled determinant to a normal form.

`(F cons (G cons (H cons nil)))` is how the list `<F G H>` looks to a rule
— matching three cells deep is ordinary
[deep unification](../logic.md#deep-unification), the same shape the
polynomial recursion uses. A list is needed rather than three objects on
one fact because a zelph fact carries a *set* of objects, and the row order
matters.

State the map, and run:

```
zelph> < $( (2+a*b)^3*c + 2*b^2*(2+a*b)*(8+3*a*b) ) $( 8*b + 3*a*(2+a*b)^2*c + 6*a*b^2*(8+3*a*b) ) $( 8*a - (6*a^2*b + a^3*c) ) > jac3 <a b c>
zelph> .run
zelph> _M jdet _P
Answer: (<($( (&2 + a * b) ^ &3 * c + &2 * b ^ &2 * (&2 + a * b) * (&8 + &3 * a * b) )) ($( &8 * b + &3 * a * (&2 + a * b) ^ &2 * c + &6 * a * b ^ &2 * (&8 + &3 * a * b) )) ($( &8 * a - (&6 * a ^ &2 * b + a ^ &3 * c) ))> jac3 <a b c>) jdet (neg zint &512)
```

**`(neg zint &512)`.** A constant polynomial — the normal form collapsed to
a bare integer, which is precisely the statement that every non-constant
term cancelled. Not "cancelled up to the precision I sampled at": the
normal form of the determinant *is* the node `(neg zint &512)`, and that
node is not any polynomial in a, b or c.

The sign matters, so check it rather than reading it:

```
zelph> _M jdet (neg zint &512)
Answer: …                              # one answer
zelph> _M jdet (pos zint &512)
zelph>                                 # none
```

Total: about 1.8 seconds, cold start included, on a laptop.

The whole construction ships with zelph, so you do not have to retype it:

```
zelph> .import examples/math/jacobian
zelph> _M jdet _P
Answer: … jdet (neg zint &512)
```

## What was and was not assumed

Worth being explicit, because a verification is only as good as its
premises.

**Assumed:** the three components of G as written above, the three points,
and the cofactor expansion of a 3×3 determinant. Those are the mathematics
you are asking zelph to check *against*, and you can read all of them in
the session.

**Derived:** every digit operation, every carry, the signed-integer
arithmetic, the nine partial derivatives, the products and differences of
the cofactor expansion, and the polynomial normal forms of both halves.
`.explain` reaches all of it. Nothing was computed by a routine that zelph
could not show you as rules.

**Not shown here:** that G and F are related by the stated substitution —
that is a two-line hand calculation, and it is the one step of the argument
that lives outside the session. Likewise, the reduction of "not injective"
to "three points collide" is the definition.

## Why this is a good stress test for zelph

The workload is unforgiving in a specific way: symbolic differentiation
produces large unsimplified terms, and the determinant multiplies three of
them together. The intermediate polynomials have hundreds of terms.

It runs because of two properties that have nothing to do with mathematics:

**Hash-consing.** The subterms `(2 + ab)`, `(8 + 3ab)` and their powers
appear many times across the nine derivatives. They are one node each,
computed once, and every rule that touches them hits the same node.

**Monotone memoisation.** Every intermediate result is a fact, and facts
are never recomputed. The determinant's three 2×2 minors share operands;
those products are derived once and read twice.

Both are properties of a knowledge-graph engine, not of a CAS — which is
the theme of this whole section. The
[performance internals](../internals/performance.md) document what had to
be built to make workloads of this size routine rather than intractable.

## Exercises

1. Run part 2 under `binary-arithmetic` instead of the default and confirm
   the answer is the same node. Then time both.
2. Replace G₃ by `$( 8*a - (6*a^2*b + a^3*c) + a )` and re-run. The
   determinant should no longer be constant — what comes back, and how do
   you read it?
3. Use `.explain` on the `jdet` fact and find the point at which the last
   non-constant coefficient cancels.
4. The nine seeding rules differ only in which component and which variable
   they name. Rewrite them as fewer rules using `diffalong` and a rule that
   walks the two lists in step.

## Further reading

- [`src/test/test_jacobian.cpp`](https://github.com/acrion/zelph/blob/main/src/test/test_jacobian.cpp)
  — the same verification as a regression test, with the map built by a
  Janet constructor parameterised over its leaves, so the symbolic map and
  the three ground evaluations come from one definition.
- [Module and Predicate Index](reference.md) — every predicate used above.
