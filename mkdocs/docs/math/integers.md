# Integers over ℤ

Module: [`stdlib/integer-arithmetic.zph`](https://github.com/acrion/zelph/blob/main/stdlib/integer-arithmetic.zph)
· Prerequisite: an [arithmetic substrate](arithmetic.md) (a default is
imported if you do not choose one)

Signed integers on top of the natural-number modules. The module owns no
recursion of its own: every rule keys on a user-facing fact and delegates
the magnitude work to the naturals by asserting ordinary `+`, `-`, `*` and
`cmp` facts. It is therefore base-agnostic and runs unchanged on all three
substrates.

## Representation

The `z`-operations of this module work on the ordinary term

```
(pos zint N)        (neg zint N)
```

with `N` a natural-number cons list and `pos`/`neg` plain atoms. No new
machinery — the term is a fact node like any other, built by `fact()` and
matched by deep unification.

**One world for the nonnegative integers.** A nonnegative integer is the natural numeral in its own right: `&3`, never `(pos zint &3)`. A zint term appears as a value exclusively for a negative integer, `(neg zint N)`. `(pos zint N)` serves as the *operand form* of `z+`, `z-`, `zx`, and `zcmp`: these operations accept it and return it, while the [façade](#the-uniform-operator-facade) lifts a natural into this form and lowers a nonnegative outcome back into the numeral. It remains permissible as input, within the façade beside another zint term, and throughout the [simplifier](symbolic.md#integers-as-leaves), which normalizes it to `N`. One value, one node: the neutral and absorbing rules of [`symbolic-core`](symbolic.md) yield the natural zero and one, so a façade returning `(pos zint &1)` would supply two responses to every rule interpreting `=` facts at points where the two forms meet.

**Canonical form.** In the context of `z`-operations, the representation of zero is `(pos zint &0)`. The term `(neg zint &0)` must never be produced: one value, one node, the invariant the naturals pin with `canonnum`. All rules yield canonical results for canonical operands; `pos` magnitudes may be any canonical natural, `neg` magnitudes must be nonzero. When typed as input, `(neg zint &0)` denotes zero bearing a sign it cannot possess: the [simplifier](symbolic.md#integers-as-leaves) normalizes it to `&0`, similarly to how it normalizes `(pos zint N)` to `N`, [`topoly`](topoly.md) compiles it into the zero polynomial `(pos zint &0)`, so that `((neg zint &0) ≡ &0)` is proven, and the [façade](#the-uniform-operator-facade) routes it as the zero. All three read the magnitude by value, hence `(neg zint <00>)` is also zero.

One location intentionally retains `(pos zint N)`. The [polynomial layer](polynomial.md#representation) uses it as its coefficient form and calls `z+` and `zx` directly, ensuring that a constant polynomial persists as a zint term throughout the entire layer, and [`topoly`](topoly.md) returns outcomes originating from that layer: `(:topoly &7)` equals `(pos zint &7)`, and `(:topoly (x - x))` equals `(pos zint &0)`. The exponent of `^` is not such a place: it is [a subterm just like any other](symbolic.md#exponentiation), hence `:simplify (x ^ (pos zint &2))` produces `(x ^ &2)`.

A Janet input helper is provided for signed literals:

```
zelph> .import integer-arithmetic
zelph+> %(print (zelph/int "-0"))
&0
zelph> %(print (zelph/int "5"))
&5
zelph> %(print (zelph/int "-5"))
neg zint &5
```

`zelph/int` is defined by the module, not by the engine, thus the import is responsible for placing it there. It yields the natural numeral corresponding to a nonnegative literal and `(neg zint N)` for a negative one; the magnitude is delegated to `zelph/number`, so a negative zero such as `-0` or `-00` is normalized to the natural zero. It is input convenience only — the same node can be built with `zelph/fact` or typed verbosely.

## Operations

Results are exposed under `=`, the uniform query idiom.

| Request | Meaning |
|---|---|
| `(X z+ Y) = Z` | addition |
| `(X z- Y) = Z` | subtraction — **total** on ℤ, unlike natural `-` |
| `(X zx Y) = Z` | multiplication |
| `(X zcmp Y) = Z` | comparison; `lt`, `gt`, `eq` |

```
zelph> .import integer-arithmetic
zelph+> ? (pos zint &7) z+ (neg zint &10)
Answer: ((pos zint &7) z+ (neg zint &10)) = (neg zint &3)
zelph+> ? (pos zint &7) z- (neg zint &10)
Answer: ((pos zint &7) z- (neg zint &10)) = (pos zint &17)
zelph+> ? (neg zint &7) zx (neg zint &6)
Answer: ((neg zint &7) zx (neg zint &6)) = (pos zint &42)
zelph+> ? (neg zint &7) zcmp (neg zint &6)
Answer: ((neg zint &7) zcmp (neg zint &6)) = lt
```

The `z`-operations yield results in their operand form, as demonstrated by `(pos zint &17)` and `(pos zint &42)` above; under the shared predicates, the [façade](#the-uniform-operator-facade) exposes the same values as naturals.

`zx` rather than `z*`, for the same reason the digit table is `dx`: `*` is
parser-reserved inside atom names.

Comparison additionally derives the relational facts `<`, `>`, `==` — the
**same predicates** the naturals use. A meta-rule quantifying over them
("`>` is transitive") therefore spans ℕ and ℤ without knowing that either
exists.

## How partiality composes

The design worth studying is mixed-sign addition. Both candidate
differences are asserted; natural subtraction silently kills the invalid
one; the `cmp` guards select the matching connect rule:

```
((pos zint A) z+ (neg zint B)) => (A cmp B)
((pos zint A) z+ (neg zint B)) => (A - B)
((pos zint A) z+ (neg zint B)) => (B - A)

((pos zint A) z+ (neg zint B), A > B, (A - B) = D) => (… = (pos zint D))
((pos zint A) z+ (neg zint B), A == B)             => (… = (pos zint &0))
((pos zint A) z+ (neg zint B), A < B, (B - A) = D) => (… = (neg zint D))
```

Nothing tests which branch is "valid". The invalid subtraction simply
derives nothing, and the rule that would have consumed it never fires.

Zero guards follow the same principle. Negating a positive subtrahend is
guarded by `B > &0` so that `(neg zint &0)` is never materialised, not even
as an operand; subtracting zero has its own direct rule:

```
zelph+> ? (pos zint &0) z- (pos zint &0)
Answer: ((pos zint &0) z- (pos zint &0)) = (pos zint &0)
```

## The uniform operator façade

Rules that are conditional on signed operands route `+`, `-`, `*`, and `cmp` to their respective `z`-counterparts and re-expose the results under the natural predicate, lowering a nonnegative result into its corresponding natural numeral. Every operand is first lifted via `zoperand`:

```
((G zint A) + (H zint B)) => ((G zint A) needszoperand (G zint A))
((G zint A) + (H zint B)) => ((H zint B) needszoperand (H zint B))
((G zint A) + (H zint B), (G zint A) zoperand P, (H zint B) zoperand Q) => (P z+ Q)
((G zint A) + (H zint B), (G zint A) zoperand P, (H zint B) zoperand Q,
 (P z+ Q) = (pos zint N))
=> (((G zint A) + (H zint B)) = N)
((G zint A) + (H zint B), (G zint A) zoperand P, (H zint B) zoperand Q,
 (P z+ Q) = (neg zint N))
=> (((G zint A) + (H zint B)) = (neg zint N))
```

The lift passes an operand solely when its magnitude is a numeral, and it uses the magnitude in canonical form: a natural numeral `N` is transformed into `(pos zint N)`, a zint stays a zint, and a signed zero is represented as `(pos zint &0)` (refer to the explanation provided below). The shapes of routed operands consist of two zint terms, including an explicit `(pos zint N)`, and a natural number positioned next to a negative integer, in either order:

```
((A cons R) + (neg zint B), (A cons R) zoperand P, (neg zint B) zoperand Q) => (P z+ Q)
```

so the query idiom is the same over ℕ and ℤ:

```
zelph> .import integer-arithmetic
zelph+> ? (pos zint &2) + (neg zint &5)
Answer: ((pos zint &2) + (neg zint &5)) = (neg zint &3)
zelph+> ? &2 + (neg zint &5)
Answer: (&2 + (neg zint &5)) = (neg zint &3)
zelph+> ? (neg zint &2) - (neg zint &5)
Answer: ((neg zint &2) - (neg zint &5)) = &3
zelph+> ? (neg zint &3) * (neg zint &4)
Answer: ((neg zint &3) * (neg zint &4)) = &12
zelph+> ? (neg zint &2) * &3
Answer: ((neg zint &2) * &3) = (neg zint &6)
```

Comparison of two zint terms occurs via `zcmp`. When a natural number is contrasted with a negative integer, the outcome hinges solely on the sign, as `zcmp` decides mixed signs, provided the [numeral test](arithmetic.md#numbers-are-graph-structure) in `common-arithmetic` has confirmed that the natural component constitutes a number: `(<x c> cmp (neg zint &1))` derives nothing. Two naturals remain under the natural module, including their partiality (see [below](#completing-natural-partiality)).

**A signed zero is the zero.** `(neg zint &0)`, or any negative zint whose magnitude equals zero, like `(neg zint <00>)`, arises from no rule but is permitted as input. The `z`-operations take a negative operand as non-zero, thus, if processed directly, it would be treated as a negative value. Instead, the lift passes it as `(pos zint &0)`, a natural compared with it is compared with `&0`, and the relational fact is restated for the operands as written, thus `(&0 cmp (neg zint &0))` derives `(&0 == (neg zint &0))`:

```
zelph> .import integer-arithmetic
zelph+> ? &0 cmp (neg zint &0)
Answer: (&0 cmp (neg zint &0)) = eq
zelph+> ? &3 * (neg zint &0)
Answer: (&3 * (neg zint &0)) = &0
zelph+> ? (neg zint &0) + (neg zint &0)
Answer: ((neg zint &0) + (neg zint &0)) = &0
```

The payoff is elsewhere: [`symbolic-core`](symbolic.md)'s knowledge-folding
bridge `(T red C, C = R) => (T rw R)` consumes these `=` facts with **no
ℤ-specific rule at all**. Constant folding over ℤ is this façade plus a rule
that was already there.

The price is that the naturals' triggers also fire on these facts and create internal states such as `((zintA add zintB) ci 0)`. They pose no danger, as none results in an incorrect outcome, and they remain finite. Upon encountering a zint operand, digit recursion stalls immediately: it cannot break down a fact whose predicate is `zint` instead of `cons`. A natural operand is traversed, however, in two positions: as the first factor of `*` and as the dividend of `/` and `mod`. `(&123 * (neg zint &5))` produces a `mul` state for each suffix of `&123` and a stalled `dmul` state for each unique digit in its representation, amounting to six states in base 10; `(&123 / (pos zint &5))` generates a `dvd` state for every suffix of `&123` beside one stalled `dmul` seed per digit of the alphabet, totalling 13 states in base 10. Additionally, the natural `/` and `mod` triggers fire on a `/` or `mod` fact with a zint dividend and a numeral divisor, thereby completing the divisor’s table of multiples, yielding accurate facts that persist as reusable knowledge. These states represent the cost of shared predicates.

**Division and mod are not routed.** The choice among floor, truncation, and Euclidean division arises solely when dealing with negative operands. For operands that are nonnegative, all three methods produce the same result, and because a nonnegative integer constitutes a natural numeral, the quotient and remainder precisely match those of the natural module: `&7 / &2` yields `&3`, `&7 mod &2` yields `&1`. When either the dividend or the divisor is negative, the three conventions no longer align in general (−7 / 2 equals −4 under floor and Euclidean division, yet −3 under truncation), and this decision is deliberately left open: a `/` or `mod` fact involving a zint operand derives nothing – partiality by absence, as consistently observed throughout the standard library.

```
zelph> .import integer-arithmetic
zelph+> ? &7 / &2
Answer: (&7 / &2) = &3
zelph+> ? (neg zint &7) / &2
zelph+> ? &7 mod (neg zint &2)
zelph+> ? (pos zint &7) / (pos zint &2)
zelph+>
```

The [simplifier](symbolic.md#integers-as-leaves) normalizes an explicit `(pos zint N)` to `N` before folding, meaning that there `((pos zint &7) / (pos zint &2))` simplifies to `&3` through natural division, whereas a quotient involving a negative operand remains unchanged:

```
zelph> .import symbolic-integers
zelph+> ? :simplify ((pos zint &7) / (pos zint &2))
Answer: (:simplify ((pos zint &7) / (pos zint &2))) = &3
zelph+> ? :simplify ((neg zint &7) / &2)
Answer: (:simplify ((neg zint &7) / &2)) = ((neg zint &7) / &2)
```

## Completing natural partiality

Natural subtraction is partial, and loading this module does not change
that — it adds a second, disjoint set of rules:

```
zelph+> ? &3 - &5
zelph+> ? (pos zint &3) - (pos zint &5)
Answer: ((pos zint &3) - (pos zint &5)) = (neg zint &2)
```

The façade intentionally leaves two naturals to the natural module: the mixed-sign rules above assert both candidate differences as ordinary `-` facts, and a façade that made `-` total over ℕ would also yield a result for the invalid one.

The bridge that lets a *term* containing natural numerals fall through to ℤ
lives one layer up, in [`symbolic-integers`](symbolic.md#integers-as-leaves).

## Testing

`src/test/test_integers.cpp` runs the operation matrix across all three arithmetic substrates and both parallelism modes. The `z`-operations are checked by `zelph/exists` probes, which verify the presence of anticipated `=` and relational facts, as well as the absence of wrong ones; the façade cases additionally compare normalized answer lines and exact answer counts.
