# Term-to-Polynomial Compiler

Module: [`stdlib/topoly.zph`](https://github.com/acrion/zelph/blob/main/stdlib/topoly.zph)
· Prerequisite: [`polynomial`](polynomial.md)

Compiles symbolic terms to the canonical normal forms of the polynomial
layer, and decides polynomial identities on the result. Pure
forward-chaining rules, in the standard Trigger / Decompose / Assemble /
Connect shape.

[`symbolic-core`](symbolic.md) is deliberately **not** a prerequisite. This
module consumes only the shared term *vocabulary* — the binary operators,
the application `(neg of U)`, sort declarations and numerals — all of which
are ordinary facts that exist with or without the simplifier. The simplifier
and `diff` are the typical *producers* of the terms being compiled, not a
dependency.

## Request idiom

```
(:topoly T) = P
```

repeatable, like every result query in the standard library.

```
zelph> .import math
zelph> <x> ~ polyring
zelph> ? :topoly $( (x+1)^2 )
Answer: (:topoly ((x + &1) ^ &2)) = (x poly <(pos zint &1) (pos zint &2) (pos zint &1)>)
```

## Vocabulary

| Term | Compiles to |
|---|---|
| `X ~ symvar` | the polynomial X: `(X poly <(pos zint &0) (pos zint &1)>)` |
| `C ~ symconst` | the same — an opaque constant is an **indeterminate** here |
| zint numeral | itself, a constant polynomial |
| natural numeral | promoted to `(pos zint N)` |
| `(U + V)`, `(U - V)`, `(U * V)` | `padd` / `psub` / `pmul` |
| `(U ^ N)`, `N` a natural numeral | `ppow` |
| `(neg of U)` | `pneg` |

Anything else — division, other function applications, undeclared atoms —
gets no result, and the request stays silently unanswered.

Two consequences worth stating.

**Constants are indeterminates.** `c ~ symconst` compiles like a variable,
because an identity that holds in ℤ[c, x, …] holds for every value of c.
Like any variable, `c` must appear in the `pouter` order.

**Promotion makes subtraction total.** Natural subtraction is partial, but
here the operands are promoted to ℤ first:

```
zelph> ? :topoly (&3 - &5)
Answer: (:topoly (&3 - &5)) = (neg zint &2)
```

This also closes the loop that
[`symbolic-minus`](symbolic.md#subtraction-and-negation) deliberately left
open: `(:topoly (x - x))` is `(pos zint &0)`. Cancelling equal symbolic
terms is the polynomial layer's job, and here it happens.

## Architecture

```
(T topoly T) => (T needstopoly T)                         Trigger
((U + V) needstopoly (U + V)) => (U needstopoly U)        Decompose
…
(X needstopoly X, X ~ symvar) => (X aspoly (X poly …))    Base
((U + V) needstopoly (U + V), U aspoly P, V aspoly Q)
=> (P padd Q)                                             Assemble (seed)
((U + V) needstopoly (U + V), U aspoly P, V aspoly Q,
 (P padd Q) = R) => ((U + V) aspoly R)                    Assemble (adopt)
(T topoly T, T aspoly P) => ((T topoly T) = P)            Connect
```

The assemble stage is the cross-module cascade used throughout the standard
library: seed the operation as an ordinary fact, let the data layer answer
it, adopt the `=` result.

**Canonicity** needs no separate argument: leaves compile to canonical
polynomials, and the delegated operations preserve canonicity, so every
`aspoly` result is canonical. `=` is single-valued because each term shape
is matched by exactly one assemble family.

**Display sugar** needs no registration here. `(T topoly T)` is a genuine
request, `needstopoly` a genuine marker, and `(P aspoly P)` for a leaf
genuinely reads "P is its own polynomial form".

## Identity checking

Canonical forms are unique hash-consed nodes, so proving a polynomial
identity is comparing node identity — expressed through unification:

```
(A ≡ B) => (:topoly A)
(A ≡ B) => (:topoly B)
(A ≡ B, (:topoly A) = P, (:topoly B) = P) => ((A ≡ B) = proven)
(A ≡ B, (:topoly A) = P, (:topoly B) = Q, P != Q) => ((A ≡ B) = disproven)
```

The *same* variable `P` in both proof conditions binds only if both
compilations reached the identical node. There is no equality checker.

### Three outcomes

| Answer | Means |
|---|---|
| `proven` | both sides compiled to the same normal form |
| `disproven` | both sides compiled, to different normal forms |
| *(no answer)* | at least one side did not compile |

```
zelph> ? $( (x+1)^2 ) ≡ $( x^2 + 2*x + 1 )
Answer: … = proven
zelph> ? $( (x+1)^2 ) ≡ $( x^2 + 1 )
Answer: … = disproven
```

The third case is not a weakness to be engineered away — it is the honest
reading. An undeclared atom, a missing `pouter` pair, or an operator
outside the vocabulary means the question was never posed to the polynomial
layer, and *"I could not compile this" is not "these differ"*. Ask for the
normal forms directly (`? :topoly A`) to see which side is missing.

Note also what `disproven` does **not** need: no negation-as-failure, no
dependence on the order in which the compile state saturates. Once both
normal forms exist, `!=` decides in the positive stratum.

## Extending the vocabulary

An operator the compiler does not know can be taught by **delegation** —
seed its defining term and adopt whatever normal form that reaches. Two
rules suffice; the [terms tutorial](tutorial-terms.md#teaching-zelph-a-new-operator)
walks through an example, and [`eml`](eml.md) applies the same pattern to a
whole macro chain.

## Testing

`src/test/test_topoly.cpp` runs against all three arithmetic substrates,
with structural probes. `symbolic-core` is deliberately not imported there:
the compiler must work from the vocabulary alone.
