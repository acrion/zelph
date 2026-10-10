# Module and Predicate Index

Every public request idiom of the mathematical standard library, in one
place. The pages linked from the module column explain the machinery; this
one is for looking things up.

## How to read a request

The standard library speaks one idiom throughout. A request is an ordinary
fact you assert; the answer arrives as another ordinary fact, exposed under
`=`, and stays in the graph:

```
zelph> .import math
zelph+> ? (&12 * &34)
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
| `math` | every component beneath except the decimal and NAND substrates, `primes*` and `eml` | you want mathematics and no decisions |
| `binary-arithmetic` | `common-arithmetic` | you need naturals only, base 2 (the default substrate) |
| `decimal-arithmetic` | `common-arithmetic` | base 10 — cheapest for large coefficients |
| `binary-nand-arithmetic` | `common-arithmetic` | base 2 derived from a single NAND axiom |
| `integer-arithmetic` | an arithmetic substrate | you need ℤ |
| `polynomial` | `integer-arithmetic` | you work on normal forms directly |
| `topoly` | `polynomial` | you compile terms to normal forms |
| `symbolic-core` | an arithmetic substrate | you build and simplify terms |
| `symbolic-minus`, `symbolic-pow` | `symbolic-core` | you need `-`/`neg` or `^` in terms |
| `symbolic-integers` | `symbolic-minus`, `integer-arithmetic` | you need ℤ leaves in terms |
| `diff` | `symbolic-core` | you differentiate |
| `math-syntax` | an arithmetic substrate | you want `$( … )` notation |
| `primes` / `primes-naf` | an arithmetic substrate | primality |
| `eml` | `symbolic-core`, `symbolic-minus` | the EML case study |

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

Numerals are constructed from cons lists composed of digit nodes, arranged so that the least significant digit appears first; `&42` serves as syntactic shorthand for such a structure. The precision involved is unbounded. A cons list is considered a numeral exactly when it is non-empty, terminates with `nil`, and every individual element within it is a digit. The operations `+`, `-`, `*`, `/`, `mod`, and `cmp` yield no outcome if either operand is a non-empty list that does not meet the criteria of being a numeral: requests such as `(<x 1> cmp &0)` and `(<x c> + &1)` go unanswered. Modules that read a list as a numeric value carry out the same verification on demand: `(L needsnumeral L)` derives `(L isnumeral true)` exactly when `L` constitutes a valid numeral.

The empty list `nil` (typed `<>`) does not qualify as a numeral and falls beyond the contract of any request on this page. Because it also serves as the endpoint where digit recursions terminate, certain requests interpret it as zero: `+`, `-`, and `cmp` (`(nil + &3) = &3`); every exponent processed via `cmp`, which covers `^`, `ppow`, `topoly`, and the power rule in `diffby` (`(&2 ^ nil) = &1`); and `*` when `nil` appears as the second factor and the first operand contains more than one digit (`(&13 * nil) = &0`). The operations `/`, `mod`, `*` when `nil` is the first factor, and the integer façade derive nothing for it.

## Integers — [`integer-arithmetic`](integers.md)

Representation: a nonnegative integer is expressed as its natural numeral `N`; a negative integer is represented as `(neg zint N)` where `N > &0`, and `(neg zint &0)` is never generated. `(pos zint N)` serves as the operand form for the `z`-operations that follow and remains valid as input; the polynomial layer maintains it as its coefficient form.

| Request | Answer | Silent when |
|---|---|---|
| `(X z+ Y) = Z` | addition | an operand fails to be a zint term |
| `(X z- Y) = Z` | subtraction, total over ℤ | ”, with the exception that `(X z- (pos zint &0))` yields `X` for any `X` |
| `(X zx Y) = Z` | multiplication | ” |
| `(X zcmp Y) = Z` | `lt`, `gt`, `eq`; furthermore derives `<`, `>`, `==` | ”, unless a fact `X < Y`, `X > Y`, or `X == Y` already holds, whether declared or derived through another request such as `(X cmp Y)` |
| `(X + Y) = Z`, `-`, `*`, `cmp` | the same, through the natural predicates; a nonnegative result transforms into the natural numeral | two naturals, with the natural module silent; a natural next to `(pos zint N)`; an operand that is not a numeric value |

The last row presents the **uniform operator façade**: the operators `+`, `-`, and `*`, when applied to two zint operands or to a natural paired with a negative one, are routed to the `z`-operations, and the result is re-exposed under the natural predicate, a nonnegative one as the natural numeral: `((neg zint &2) - (neg zint &5)) = &3`. Comparison via `cmp` between two zints is routed to `zcmp`; when comparing a natural with a negative, the decision hinges solely on sign. The façade interprets each magnitude in its canonical form, and treats a signed zero such as `(neg zint &0)` as the zero. Thus, `=` queries appear identical across ℕ and ℤ. Division and `mod` are not routed: a fact involving `/` or `mod` with a zint operand derives nothing. Floor, truncated, and Euclidean division vary exclusively for negative operands, and the choice among them is deliberately left open. For nonnegative operands, all three methods produce the same result, and a nonnegative integer is a natural: the simplifier normalizes `(pos zint N)` into `N`, so `(:simplify ((pos zint &7) / (pos zint &2)))` evaluates to `&3`.

The outcomes of `z`-operations are defined solely for canonical operands: although a signed zero like `(neg zint &0)` or a non-canonical magnitude might still elicit a response – possibly incorrect, as in `((neg zint &0) zcmp (pos zint &0)) = lt` – such inputs fall within the façade’s domain, which reads every operand in canonical form.

## Polynomials — [`polynomial`](polynomial.md)

Representation: a constant is a zint; otherwise `(V poly L)` with `L` a
cons list of coefficients, least significant first, whose main variables are
strictly inner to `V`.

| Request | Answer | Silent when |
|---|---|---|
| `(P padd Q) = R` | addition | an operand is neither a zint nor a `(V poly L)` term, or it combines two composites in distinct variables lacking a `pouter` order between their variables |
| `(:pneg P) = R` | negation | `P` does not qualify as a zint or a `(V poly L)` term |
| `(P psub Q) = R` | subtraction | identical to the condition for `padd` |
| `(P pmul Q) = R` | multiplication | same as above |
| `(P ppow N) = R` | power, `N` a natural numeral | `P` is not a zint nor a `(V poly L)` term, or it combines two composites in separate variables without a `pouter` order between their variables – except when `N` is zero, yielding `(pos zint &1)` regardless of `P`; or when `N` is not a natural numeral, other than a negative zero like `(neg zint &0)`, which the façade compares as `&0` and thus behaves as exponent zero; `(pos zint &0)` stays silent |
| `A pouter B` | asserts that `A` is strictly outer relative to `B`; transitive | – |

As with the `z`-operations, results are defined solely for canonical operands: a non-canonical input – such as a signed zero, a magnitude that is not a numeral, or a coefficient list that is not canonical – might still yield a response, and that response need not be canonical.

## Terms — [`symbolic-core`](symbolic.md) and its operator modules

| Request | Answer | Silent when |
|---|---|---|
| `(:simplify T) = S` | normal form of `T` | a leaf of `T` has no declared sort |
| `X ~ symvar` | declares an indeterminate | — |
| `X ~ symconst` | declares an opaque constant | — |
| `F inverseof G` | declares f(g(u)) = u for the generic rewrite | — |

The simplifier recognizes these operators: `+ - * / ^`, application `(F of U)`, and `neg`. The operators `-` and `neg` come from `symbolic-minus`, `^` is sourced from `symbolic-pow`, and ℤ leaves come from `symbolic-integers`. To include user-defined operators, use the [operator extension protocol](tutorial-terms.md#extending-the-simplifier-instead). Alternatively, an operator may be brought in via its [definition](symbolic.md#definitional-extension): its congruence rule infers `(T expandsto R)` based on the normal forms of the operands, `R` is simplified as a standard term, and `T` takes its normal form ([`eml`](eml.md) is defined in this way).

`simp` is single-valued. If a declared equation conflicts with a current rewrite, or if one is introduced after the request has already been fulfilled, it assigns a second normal form to the term; likewise, an added rule that overlaps a shipped one with a distinct outcome does the same. In either way, this is reported as a contradiction, explicitly naming both answers.

## Differentiation — [`diff`](symbolic.md#differentiation)

| Request | Answer | Silent when |
|---|---|---|
| `(T diffby X) = D` | simplified derivative | a subterm of `T` resides beyond the vocabulary (`/`, a foreign predicate, an undeclared atom, a list or zint magnitude that is not a numeral), an exponent fails to be a natural numeral (except a negative-signed zero such as `(neg zint &0)`, which the façade equates to `&0` and which thus functions as the exponent zero; `(pos zint &0)` stays silent), or a function lacks a `hasderivative` declaration |
| `(T diffalong L) = E` | iterated derivative along a cons list of variables | any step is silent |
| `F hasderivative G` | declares d f(u)/du = g(u) for the generic chain rule | – |

`diffalong <x y>` performs differentiation with respect to `x`, followed by differentiation with respect to `y`; `diffalong nil` answers `T`. When mixed partials yield results in both orders, their values match, yet they converge to the same answer node solely when their simplified forms are identical: the simplifier does not enforce normalization of commutativity or associativity. In some cases, one order may remain silent while the other produces a result, due to the derivative of `ln` being expressed as a quotient. Each term possesses no more than a single raw derivative per variable; a second one would be reported as a contradiction.

## Compilation and identity — [`topoly`](topoly.md)

| Request | Answer | Silent when |
|---|---|---|
| `(:topoly T) = P` | the canonical polynomial normal form of `T` | `T` uses an operator or leaf that the compiler is unaware of |
| `(A ≡ B) = proven` | the two terms are the same polynomial | – |
| `(A ≡ B) = disproven` | both terms were compiled into distinct normal forms – they are different as polynomials; declared equations are not consulted | – |
| `(A ≡ B)` – no answer | **one side did not compile at all** | see above |

The compiler recognizes `+ - *`, `^` with a natural exponent, `(neg of U)`, sorts `~ symvar` / `~ symconst`, natural numerals (promoted to ℤ), and zint numerals; `(neg zint &0)` translates into the zero polynomial `(pos zint &0)`. Division is not included. A constant outcome is represented as a zint term, the coefficient form of the polynomial layer: `(:topoly &7)` equals `(pos zint &7)`.

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
| `(U eml V)` | the operator exp(U) − ln(V), simplified according to this definition; normal forms exclude any `eml` |

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
