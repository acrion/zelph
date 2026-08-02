# Polynomial Normal Forms

Module: [`stdlib/polynomial.zph`](https://github.com/acrion/zelph/blob/main/stdlib/polynomial.zph)
· Prerequisite: [`integer-arithmetic`](integers.md)

Multivariate polynomial normal forms over ℤ, as pure forward-chaining
rules. This is a **data layer**, like the arithmetic modules: it operates on
normal forms and knows nothing about symbolic terms. The compiler that
bridges the two is [`topoly`](topoly.md).

Its reason for existing is one property: *equality of polynomials is
identity of nodes*. Everything below serves that.

## Representation

Recursive, dense, collapsed, variable-tagged.

- A **constant** polynomial is a zint term: `(pos zint N)` / `(neg zint N)`.
- A **non-constant** polynomial with main variable `V` is `(V poly L)`,
  where `L` is a cons list of canonical polynomials, **least significant
  first**: the V⁰ coefficient is the outermost cell.

The orientation is the numeral orientation, and deliberately so. A number
in base *b* **is** a polynomial evaluated at *b*, and the rules below are
the digit recursions of [`common-arithmetic`](arithmetic.md) with the carry
removed.

```
zelph> .import math
zelph> <x> ~ polyring
zelph> ? :topoly $( 3*x^2 - 5 )
Answer: (:topoly $( &3 * x ^ &2 - &5 )) = (x poly <(neg zint &5) (pos zint &0) (pos zint &3)>)
```

The `<x> ~ polyring` line is what fixes the variable ORDER, and without it the
query answers nothing at all — see [Variable order](#variable-order) below.

⟨−5, 0, 3⟩ is −5 + 0·x + 3·x².

## The invariants

**Collapse.** A polynomial that is constant in `V` is represented by its
constant coefficient directly, never as `(V poly (C cons nil))`. Canonical
coefficient lists therefore have length ≥ 2.

**Nonzero leading coefficient.** The last list element is never the zero
polynomial. Inner zeros are value-relevant and kept.

Together: the zero polynomial at *every* nesting level is the single node
`(pos zint &0)` — which is what turns the strip stage's zero test into a
pattern match instead of a structural recursion. And it is what makes
cancellation fall out:

```
zelph> ? ((x poly <(pos zint &1) (pos zint &2)>) padd (x poly <(pos zint &3) (neg zint &2)>))
Answer: … = (pos zint &4)
```

(1 + 2x) + (3 − 2x): the x-coefficients cancel, the surviving list has one
cell, and collapse turns it back into a bare constant.

**The variable tag** resolves an ambiguity an untagged collapsed
representation would have across nesting levels: the list for `y` as a
constant-in-`x` polynomial would be node-identical to the list for `x`.
With the tag, every canonical polynomial is a unique hash-consed node.

## Variable order

Composite coefficients inside `(V poly L)` have main variables strictly
**inner** to `V`. The order is declared by the user as `pouter` facts —
`V pouter W` reads "V is strictly outer" — and transitivity is provided by
the module, so adjacent pairs suffice:

```
(A pouter B, B pouter C) => (A pouter C)
```

[`math.zph`](frontend.md) derives these from a `<x y z> ~ polyring`
declaration, outermost first.

The order must be a **strict total order** on the variables in use.
Declaring both `V pouter W` and `W pouter V` would break single-valuedness,
exactly like conflicting declared equations in
[`symbolic-core`](symbolic.md). Two composites with no declared order
derive nothing — partiality by absence.

A different order is not wrong, only different; within one session the
normal form is unique either way:

```
<x y> ~ polyring   →  (:topoly (y * x)) = (x poly <(pos zint &0) (y poly <…>)>)
<y x> ~ polyring   →  (:topoly (y * x)) = (y poly <(pos zint &0) (x poly <…>)>)
```

## Operations

| Request | Meaning |
|---|---|
| `(P padd Q) = R` | addition |
| `(:pneg P) = R` | negation |
| `(P psub Q) = R` | subtraction, via `pneg` + `padd` |
| `(P pmul Q) = R` | multiplication |
| `(P ppow N) = R` | exponentiation, `N` a **natural** numeral |

```
zelph> ? (:pneg (x poly <(pos zint &1) (pos zint &2)>))
Answer: … = (x poly <(neg zint &1) (neg zint &2)>)
zelph> ? ((x poly <(pos zint &1) (pos zint &2)>) ppow &2)
Answer: … = (x poly <(pos zint &1) (pos zint &4) (pos zint &4)>)
```

(1 + 2x)² = 1 + 4x + 4x².

**Addition** is a flat case analysis on operand shapes. Constant + constant
delegates to the ℤ façade. Same main variable is elementwise list addition —
the digit recursion minus the carry — followed by a strip/collapse stage.
Head adjustment (different main variables, or a constant into a composite)
touches only the V⁰ coefficient; the tail passes through untouched.

**Multiplication** needs no strip stage of its own: over ℤ, an integral
domain, products of nonzero coefficients are nonzero, so elementwise
scaling preserves canonicity, and the only accumulation — the schoolbook
recursion — delegates to `padd`, which owns its canonicalisation.

**Exponentiation** is the naive recursion with `pmul` in place of `*`. The
exponent is not a polynomial; a zint, symbolic or composite exponent
derives nothing.

## The zero test, positively

The strip rules must distinguish zero from nonzero coefficients. Zero is
matched directly as the ground pattern `(pos zint &0)`. Nonzero is
**derived** as a positive marker from the three positive shapes:
`(neg zint N)`; `(pos zint N)` with `N > &0` via an in-system `cmp`; and
`(W poly K)` composites, which are never zero by the leading-coefficient
invariant.

This keeps the module entirely inside the positive stratum — no negation,
no `!=` against structured nodes.

## Canonicity

All rules yield canonical results for canonical operands. Only
same-main-variable addition can produce a non-canonical raw list
(cancellation, possibly of the leading coefficient); its results run
through the strip/collapse stage. Negation maps nonzero coefficients to
nonzero coefficients elementwise. Applying an operation to non-polynomial
or non-canonical operands derives nothing.

## Display note

The module excludes `padd`, `psub`, `ladd` and `pmul` from the
[self-fact display sugar](../index.md#the-self-fact-prefix): operand value
coincidences like `(P padd P)` are ordinary facts about equal operands, not
request markers, and must render verbosely. The genuine marker predicates
(`pneg`, `lneg`, `needsplcanon`, `plcanon`, `pzcheck`, `pnonzero`) stay
registered, because they *are* self-facts.

## Testing

`src/test/test_polynomial.cpp` pins the representation, the invariants and
every operation across all three arithmetic substrates.
