# 5 · Inside the Normal Form

*Prerequisites: [4 · Differentiation](tutorial-differentiation.md).*

Every `proven` so far rested on one claim: two terms compile to the *same
node*. This tutorial opens that node up. The representation turns out to be
the one you already met in [tutorial 2](tutorial-numbers.md), with one part
removed.

## Constants first

```
zelph> .import math
zelph> <x> ~ polyring
zelph> ? :topoly $( 7 )
Answer: (:topoly &7) = (pos zint &7)
zelph> ? :topoly $( -7 )
Answer: (:topoly $( neg(&7) )) = (neg zint &7)
```

A constant polynomial is a signed integer, and a signed integer is the
ordinary term `(pos zint N)` or `(neg zint N)` with `N` a natural numeral
of whatever substrate you loaded. No new machinery: `pos`, `neg` and `zint`
are plain atoms, and the pair is a fact node like any other.

That layer, [`integer-arithmetic`](integers.md), is a *façade*. It owns no
recursion; each of its rules delegates the magnitude work to the natural
module by asserting ordinary `+`, `-`, `*` and `cmp` facts. What it buys is
totality — the gap [tutorial 2](tutorial-numbers.md#where-partiality-lives)
left open:

```
zelph> .import integer-arithmetic
zelph> ? &3 - &5
zelph>
zelph> ? (pos zint &3) - (pos zint &5)
Answer: ((pos zint &3) - (pos zint &5)) = (neg zint &2)
```

The natural subtraction is still partial — nothing was patched. A *second*
set of rules recognises zint-shaped operands and routes them to the signed
implementation, and the natural rules simply never fire there.

## One variable

```
zelph> ? :topoly $( x )
Answer: (:topoly x) = (x poly <(pos zint &0) (pos zint &1)>)
zelph> ? :topoly $( 3*x^2 - 5 )
Answer: (:topoly $( &3 * x ^ &2 - &5 )) = (x poly <(neg zint &5) (pos zint &0) (pos zint &3)>)
```

A non-constant polynomial with main variable `V` is `(V poly L)`, where `L`
is a cons list of coefficients — **least significant first**, so the V⁰
coefficient is the outermost cell. Read the second answer: ⟨−5, 0, 3⟩ is
−5 + 0·x + 3·x². 

Least-significant-first is the same orientation as the digits of `&42`, and
that is not a coincidence. **A number in base b is a polynomial evaluated
at b.** The rules that add two coefficient lists in
[`polynomial.zph`](polynomial.md) are literally the digit recursion of
[`common-arithmetic.zph`](arithmetic.md) with the carry removed — that is
the whole difference between ℤ[x] and base-b arithmetic.

## Two invariants

**Collapse.** A polynomial that happens to be constant in V is stored as
its constant coefficient directly, never as `(V poly (C cons nil))`. So a
canonical coefficient list always has length ≥ 2.

**No trailing zero.** The last element — the leading coefficient — is never
the zero polynomial. Inner zeros are value-relevant and kept, which is why
⟨−5, 0, 3⟩ keeps its middle cell.

Together these give the property everything depends on: **one polynomial,
one node**. The zero polynomial at every nesting level is the single node
`(pos zint &0)`. Which is why cancellation just happens:

```
zelph> ? :topoly $( x - x )
Answer: (:topoly (x - x)) = (pos zint &0)
```

[`symbolic-minus`](symbolic.md#subtraction-and-negation) deliberately has
*no* rewrite rule for `X - X`. Cancelling equal symbolic terms is not a
local rewrite's job — it is what a normal form is for.

## Several variables

Coefficients are themselves polynomials, in strictly *inner* variables:

```
zelph> <x y> ~ polyring
zelph> ? :topoly $( x*y )
Answer: (:topoly (x * y)) = (x poly <(pos zint &0) (y poly <(pos zint &0) (pos zint &1)>)>)
```

Outer variable `x`; the x⁰ coefficient is 0, the x¹ coefficient is the
polynomial `y`. So this is 0 + (y)·x = xy.

A larger one:

```
zelph> ? :topoly $( (x+y)^2 )
Answer: (:topoly ((x + y) ^ &2)) = (x poly <(y poly <(pos zint &0) (pos zint &0) (pos zint &1)>) (y poly <(pos zint &0) (pos zint &2)>) (pos zint &1)>)
```

Three x-coefficients: y², 2y, 1. That is y² + 2yx + x². The tag `x` on the
outer list and `y` on the inner ones is what keeps the representation
unambiguous — without it, the list for `y` as a constant-in-`x` polynomial
would be node-identical to the list for `x`, and node identity would stop
meaning equality.

### Variable order

The nesting order is *declared*, and it is what `<x y> ~ polyring` was for.
The rules in [`math.zph`](frontend.md) walk the list and emit, for each
adjacent pair, a fact `x pouter y` — "x is strictly outer than y".
Transitivity is provided by the polynomial layer, so adjacent pairs suffice.

Change the declaration and the normal form changes with it:

```
zelph> <x y> ~ polyring
zelph> ? :topoly $( y*x )
Answer: (:topoly (y * x)) = (x poly <(pos zint &0) (y poly <(pos zint &0) (pos zint &1)>)>)
```
```
zelph> <y x> ~ polyring        # a fresh session
zelph> ? :topoly $( y*x )
Answer: (:topoly (y * x)) = (y poly <(pos zint &0) (x poly <(pos zint &0) (pos zint &1)>)>)
```

Same polynomial, two normal forms — which is fine, because *within one
session* the order is fixed and the form is unique. What is not optional is
that the order be a **strict total order** on the variables in use.
Declaring both `x pouter y` and `y pouter x` would break single-valuedness,
and two composites with no declared order derive nothing at all — partiality
by absence, once again.

## The operations

The data layer offers five, each exposed under `=` in the standard idiom:

```
(P padd Q) = R      addition
(:pneg P) = R       negation
(P psub Q) = R      subtraction, via pneg + padd
(P pmul Q) = R      multiplication
(P ppow N) = R      exponentiation, N a natural numeral
```

They are worth a look because of what is *not* in them. Addition of two
polynomials in the same main variable is elementwise list addition — the
digit recursion minus the carry — followed by a strip stage that removes
trailing zeros and re-collapses. Multiplication is schoolbook: split off
the head coefficient, multiply the tail, shift by consing a zero onto the
front, and delegate the accumulation back to `padd`. Exponentiation is the
naive recursion with `pmul` in place of `*`.

There is no clever algorithm here. What makes it fast enough for the
[Jacobian case study](tutorial-jacobian.md) is the engine: hash-consing
means every repeated subterm is computed once and shared, and every
intermediate is memoised as a fact for the rest of the session.

## Why this is a proof

Assemble the pieces:

- every canonical polynomial is a unique node (the two invariants plus the
  variable tag),
- every compilation produces a canonical result (leaves are canonical, and
  the operations preserve canonicity),
- so *equality of polynomials is identity of nodes*.

Which is why the identity rule in [`topoly.zph`](topoly.md) is one line and
contains no comparison:

```
(A ≡ B, (:topoly A) = P, (:topoly B) = P) => ((A ≡ B) = proven)
```

Unification does the work. Both conditions mention the same variable `P`;
it binds only if both compilations landed on the identical node.

## Exercises

1. Compute `? :topoly $( (x+y)^3 )` and read the four coefficients off the
   answer. Check them against the binomial theorem.
2. `? :topoly $( (x - y) * (x + y) )` and `? :topoly $( x^2 - y^2 )` — the
   same node. Confirm it with `zelph/exists` rather than by eye.
3. Why is `(V poly (C cons nil))` never produced? Construct a term whose
   compilation would build it if the collapse invariant were dropped, and
   say which later comparison would then fail.
4. Declare `c ~ symconst` alongside `<x> ~ polyring` and compile
   `$( c*x )`. What sort does the polynomial layer give `c` — and why does
   it need to appear in the `pouter` order like any variable?

## Next

[6 · Refuting the Jacobian Conjecture](tutorial-jacobian.md) runs the whole
stack — digits, integers, terms, derivatives, normal forms — on a result
from July 2026.
