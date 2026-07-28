# Module and Predicate Index

Every public request idiom of the mathematical standard library, in one
place. The pages linked from the module column explain the machinery; this
one is for looking things up.

## How to read a request

The standard library speaks one idiom throughout. A request is an ordinary
fact you assert; the answer arrives as another ordinary fact, exposed under
`=`, and stays in the graph:

```
zelph> ? (&12 * &34)
Answer: (&12 * &34) = &408
```

Many requests are **self-facts** — `(T topoly T)`, `(N testprime N)` — for
which zelph has the prefix shorthand `:`, so `:topoly T` and `(T topoly T)`
are the same statement. The `?` prefix asserts, infers and queries in one
line.

Where a request has no answer, that is deliberate: **partiality is
expressed by absence**, never by a wrong or default value. Every "silent
when" column below is a design decision, not a gap.

## Loading

| Import | Pulls in | Use when |
|---|---|---|
| `math` | everything below except `primes*` and `eml` | you want mathematics and no decisions |
| `binary-arithmetic` | `common-arithmetic` | you need naturals only, base 2 (the default substrate) |
| `decimal-arithmetic` | `common-arithmetic` | base 10 — cheapest for large coefficients |
| `binary-nand-arithmetic` | `common-arithmetic` | base 2 derived from a single NAND axiom |
| `integer-arithmetic` | an arithmetic substrate | you need ℤ |
| `polynomial` | `integer-arithmetic` | you work on normal forms directly |
| `topoly` | `polynomial` | you compile terms to normal forms |
| `symbolic-core` | an arithmetic substrate | you build and simplify terms |
| `symbolic-minus`, `symbolic-pow`, `symbolic-integers` | `symbolic-core` | you need `-`/`neg`, `^`, or ℤ leaves in terms |
| `diff` | `symbolic-core` | you differentiate |
| `math-syntax` | an arithmetic substrate | you want `$( … )` notation |
| `primes` / `primes-naf` | an arithmetic substrate | primality |
| `eml` | `symbolic-core` | the EML case study |

All three arithmetic substrates claim the module ID `arithmetic` via
`.provides`. Import the one you want **before** anything that depends on a
substrate; dependants import a default and are otherwise indifferent.

## Naturals — [`common-arithmetic`](arithmetic.md)

| Request | Answer | Silent when |
|---|---|---|
| `(A + B) = X` | sum | — |
| `(A - B) = X` | difference | `A < B` (no natural result) |
| `(A * B) = X` | product | — |
| `(A / B) = X` | quotient, truncated | `B = &0` |
| `(A mod B) = X` | remainder | `B = &0` |
| `(A ^ B) = X` | power; `X ^ &0` is `&1` | — |
| `(A cmp B) = X` | `lt`, `gt` or `eq` | — |
| `A < B`, `A > B`, `A == B` | relational facts, derived by `cmp` | — |

Numerals are cons lists of digit nodes, least significant first; `&42` is
input sugar for one. Precision is unbounded.

## Integers — [`integer-arithmetic`](integers.md)

Representation: `(pos zint N)` / `(neg zint N)`, `N` a natural numeral.
Canonical zero is `(pos zint &0)`; `(neg zint &0)` is never produced.

| Request | Answer | Silent when |
|---|---|---|
| `(X z+ Y) = Z` | addition | operands not canonical zints |
| `(X z- Y) = Z` | subtraction, total on ℤ | ” |
| `(X zx Y) = Z` | multiplication | ” |
| `(X zcmp Y) = X` | `lt`, `gt`, `eq`; also derives `<`, `>`, `==` | ” |
| `(X + Y) = Z`, `-`, `*`, `cmp` | the same, through the natural predicates | mixed natural/zint operands |

The last row is the **uniform operator façade**: zint-shaped operands are
routed to the `z`-operations and the result re-exposed under the natural
predicate, so `=` queries look the same over ℕ and ℤ. Division is
deliberately not routed — Euclidean division on ℤ is a design choice, not
an oversight.

## Polynomials — [`polynomial`](polynomial.md)

Representation: a constant is a zint; otherwise `(V poly L)` with `L` a
cons list of coefficients, least significant first, whose main variables are
strictly inner to `V`.

| Request | Answer | Silent when |
|---|---|---|
| `(P padd Q) = R` | addition | operands not canonical polynomials |
| `(:pneg P) = R` | negation | ” |
| `(P psub Q) = R` | subtraction | ” |
| `(P pmul Q) = R` | multiplication | ”, or no `pouter` order for two composites |
| `(P ppow N) = R` | power, `N` a natural numeral | ”, or `N` not a natural numeral |
| `A pouter B` | declares `A` strictly outer than `B`; transitive | — |

## Terms — [`symbolic-core`](symbolic.md) and its operator modules

| Request | Answer | Silent when |
|---|---|---|
| `(:simplify T) = S` | normal form of `T` | a leaf of `T` has no declared sort |
| `X ~ symvar` | declares an indeterminate | — |
| `X ~ symconst` | declares an opaque constant | — |
| `F inverseof G` | declares f(g(u)) = u for the generic rewrite | — |

Operators understood by the simplifier: `+ - * / ^`, application `(F of U)`,
and `neg`. `-`, `neg` come from `symbolic-minus`, `^` from `symbolic-pow`,
ℤ leaves from `symbolic-integers`. Adding your own is the
[operator extension protocol](tutorial-terms.md#extending-the-simplifier-instead).

## Differentiation — [`diff`](symbolic.md#differentiation)

| Request | Answer | Silent when |
|---|---|---|
| `(T diffby X) = D` | simplified derivative | `T`'s shape is outside the known vocabulary, or an operand has no derivative rule |
| `(T diffalong L) = E` | iterated derivative along a cons list of variables | any step is silent |
| `F hasderivative G` | declares d f(u)/du = g(u) for the generic chain rule | — |

`diffalong <x y>` differentiates by `x`, then by `y`; `diffalong nil`
answers `T`. Mixed partials in both orders reach the same answer node.

## Compilation and identity — [`topoly`](topoly.md)

| Request | Answer | Silent when |
|---|---|---|
| `(:topoly T) = P` | canonical polynomial normal form of `T` | `T` uses an operator or leaf the compiler does not know |
| `(A ≡ B) = proven` | the two terms are the same polynomial | — |
| `(A ≡ B) = disproven` | both compiled, to different normal forms | — |
| `(A ≡ B)` — no answer | **a side did not compile at all** | see above |

Vocabulary the compiler accepts: `+ - *`, `^` with a natural exponent,
`(neg of U)`, sorts `~ symvar` / `~ symconst`, natural numerals (promoted to
ℤ) and zint numerals. Division is not among them.

## Front end — [`math`](frontend.md) and [`math-syntax`](frontend.md#notation)

| Request | Effect |
|---|---|
| `<x y z> ~ polyring` | declares each element `~ symvar` and each adjacent pair `pouter`, outermost first |
| `$( … )` | infix term island: `+ - * / ^`, `f(u)`, unary minus, integer literals, parentheses |
| `%(math-syntax/operator "name" prec [assoc])` | adds a binary infix operator to the island grammar **and** its display scheme |

## Number theory — [`primes`](primality.md)

| Request | Answer | Silent when |
|---|---|---|
| `(N testprime N) = X` | `prime` or `composite` | `N` is `&0` or `&1` |
| `N isprime N` | derived for every proven prime | — |
| `N hasdivisor D` | the smallest divisor ≥ 2 (`primes`), or all divisors ≤ √N (`primes-naf`) | `N` prime |

`primes` uses a positive fold and halts at the first divisor; `primes-naf`
uses negation-as-failure and scans the full bound. Same answers, opposite
techniques — the pair is the standard library's worked comparison.

## Case study — [`eml`](eml.md)

| Request | Answer |
|---|---|
| `(:emlcompile T) = F` | `T` rewritten into pure EML form |
| `(U eml V)` | the operator itself, wired into the simplifier |

## Engine commands used throughout

| Command | Purpose |
|---|---|
| `? <statement>` | assert, infer quietly, report the `=` result |
| `.explain [<pattern>] [depth]` | reconstruct a proof tree; depth `0` is unlimited |
| `.import <module>` | load a module once, by ID |
| `.provides <id>` | claim a module ID, so this file satisfies dependants' imports |
| `.run` | infer to the fixpoint |
| `.deductions off` | silence the deduction echo |

See the [Quick Start Guide](../quickstart.md) for the full command
reference.
