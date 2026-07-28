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

An integer is the ordinary term

```
(pos zint N)        (neg zint N)
```

with `N` a natural-number cons list and `pos`/`neg` plain atoms. No new
machinery — the term is a fact node like any other, built by `fact()` and
matched by deep unification.

**Canonical form.** Zero is `(pos zint &0)`. The term `(neg zint &0)` must
never be produced: one value, one node, the invariant the naturals pin with
`canonnum`. All rules yield canonical results for canonical operands;
`pos` magnitudes may be any canonical natural, `neg` magnitudes must be
nonzero.

A Janet input helper is provided for signed literals:

```
zelph> %(print (zelph/int "-0"))
«pos» «zint» &0
```

`zelph/int` delegates the magnitude to `zelph/number`, so a `-0` literal
canonicalises. It is input convenience only — the same node can be built
with `zelph/fact` or typed verbosely.

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
zelph> ? (pos zint &7) z+ (neg zint &10)
Answer: ((pos zint &7) z+ (neg zint &10)) = (neg zint &3)
zelph> ? (pos zint &7) z- (neg zint &10)
Answer: ((pos zint &7) z- (neg zint &10)) = (pos zint &17)
zelph> ? (neg zint &7) zx (neg zint &6)
Answer: ((neg zint &7) zx (neg zint &6)) = (pos zint &42)
zelph> ? (neg zint &7) zcmp (neg zint &6)
Answer: ((neg zint &7) zcmp (neg zint &6)) = lt
```

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
zelph> ? (pos zint &0) z- (pos zint &0)
Answer: ((pos zint &0) z- (pos zint &0)) = (pos zint &0)
```

## The uniform operator façade

Rules gated on zint-shaped operands route `+`, `-`, `*` and `cmp` to their
`z`-counterparts and re-expose the results under the natural predicate:

```
((G zint A) + (H zint B)) => ((G zint A) z+ (H zint B))
((G zint A) + (H zint B), ((G zint A) z+ (H zint B)) = R)
=> (((G zint A) + (H zint B)) = R)
```

so the query idiom is the same over ℕ and ℤ:

```
zelph> ? (pos zint &2) + (neg zint &5)
Answer: ((pos zint &2) + (neg zint &5)) = (neg zint &3)
```

The payoff is elsewhere: [`symbolic-core`](symbolic.md)'s knowledge-folding
bridge `(T red C, C = R) => (T rw R)` consumes these `=` facts with **no
ℤ-specific rule at all**. Constant folding over ℤ is this façade plus a rule
that was already there.

The price is that the naturals' triggers also fire on these facts and
create dead internal states such as `((zintA add zintB) ci 0)`. They are
harmless — the digit recursion cannot decompose a fact whose predicate is
`zint` rather than `cons` — and are the cost of shared predicates.

**Division and mod are deliberately not routed.** Euclidean division on ℤ
is a genuine design choice (floor versus truncation), not an oversight; a
zint `/` fact derives nothing until that choice is made.

## Completing natural partiality

Natural subtraction is partial, and loading this module does not change
that — it adds a second, disjoint set of rules:

```
zelph> ? &3 - &5
zelph>
zelph> ? (pos zint &3) - (pos zint &5)
Answer: ((pos zint &3) - (pos zint &5)) = (neg zint &2)
```

The bridge that lets a *term* containing natural numerals fall through to ℤ
lives one layer up, in [`symbolic-integers`](symbolic.md#integers-as-leaves).

## Testing

`src/test/test_integers.cpp` runs the operation matrix against all three
arithmetic substrates and both parallelism modes, with structural probes
rather than string comparisons.
