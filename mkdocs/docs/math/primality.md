# Primality

Modules: [`stdlib/primes.zph`](https://github.com/acrion/zelph/blob/main/stdlib/primes.zph)
and [`stdlib/primes-naf.zph`](https://github.com/acrion/zelph/blob/main/stdlib/primes-naf.zph)
· Prerequisite: an [arithmetic substrate](arithmetic.md)

Trial division, twice. The two modules solve the same problem with opposite
techniques — a positive fold and negation-as-failure — and the pair is the
standard library's worked comparison of the two.

## Request idiom

```
zelph> .import primes
zelph> ? :testprime &97
Answer: (:testprime &97) = prime
zelph> ? :testprime &91
Answer: (:testprime &91) = composite
zelph> &91 hasdivisor _D
Answer: &91 hasdivisor &7
```

| Request | Answer | Silent when |
|---|---|---|
| `(N testprime N) = X` | `prime` or `composite` | `N` is `&0` or `&1` |
| `N isprime N` | derived for every proven prime | — |
| `N hasdivisor D` | a divisor witness — see below | `N` prime |

0 and 1 are neither prime nor composite, and the test derives nothing for
them. Partiality by absence, as everywhere in the arithmetic standard
library.

The trigger is the self-fact `(N testprime N)`. Entering the query form
directly suffices: parsing it materialises the inner fact as a side effect,
which seeds the whole computation — exactly like `(&12 + &34) = X`.

## `primes` — the positive fold

"N is prime" is universally quantified — *all* candidates leave a remainder
— which naively suggests negation-as-failure over `hasdivisor`. It would be
wrong here. NAF tests absence in the **current** graph state, while
`hasdivisor` facts are still being derived over many fixpoint iterations,
and forward chaining is monotonic: a prematurely derived `isprime` fact
could never be retracted.

So primality is built from positive facts only. A fold `(N nodivupto D)`
grows one verified non-divisor at a time:

```
(N testprime N, &2 divisorcand N, (N mod &2) = R, R != &0) => (N nodivupto &2)
(N nodivupto D, (D + &1) = E, E divisorcand N, (N mod E) = R, R != &0)
=> (N nodivupto E)
```

**The fold is the scheduler.** Candidate E = D+1 only comes into existence
after D has been *verified* as a non-divisor. Two consequences:

- For a composite N the search halts at the smallest divisor. No work is
  performed past the verdict, and `hasdivisor` names exactly one witness —
  the smallest prime factor.
- Candidates stop at E·E > N, so the scan is O(√N) divisions.

The `P == N` boundary rule is essential: without it, perfect squares like
`&9` would pass as prime, because candidate 3 would never be created.

## `primes-naf` — the textbook formulation

The same problem, stated the way a textbook states it: N is prime iff no
candidate divides it. This needs
[stratified evaluation](../logic.md#stratified-evaluation) — the negated
rule is deferred until the positive rules, candidate enumeration and all
`mod` computations, have reached quiescence, so the negation tests absence
against the complete divisor scan.

It scans eagerly up to √N, because the negation needs the full scan. In
return it finds **all** divisors ≤ √N, not just the smallest:

```
zelph> .import primes-naf
zelph> &60 testprime &60
zelph> .run
zelph> &60 hasdivisor _D
Answer: &60 hasdivisor &2
Answer: &60 hasdivisor &3
Answer: &60 hasdivisor &4
Answer: &60 hasdivisor &5
Answer: &60 hasdivisor &6
```

## Choosing between them

| | `primes` | `primes-naf` |
|---|---|---|
| Technique | positive fold | negation-as-failure |
| Needs stratification | no | yes |
| Composite N | halts at the smallest divisor | scans to √N |
| `hasdivisor` | one witness | all divisors ≤ √N |
| Reads like the definition | no | yes |

Neither is the "right" one. `primes` is the better computation; `primes-naf`
is the better statement of the mathematics. That both are expressible, in
the same language, over the same arithmetic, is the point of having both in
the standard library.

## Node-identity guards

The guards `R != &0`, `&2 == N`, `P == N` and the bound comparisons compare
**nodes**, via the comparison module's relational facts. This is sound
because all involved numbers are canonical and canonical numbers are
hash-consed: one value, one node.

## Cross-module cascade

Neither module computes anything itself. `(N mod D)`, `(D + &1)`,
`(E * E)`, `(P cmp N)` are ordinary facts asserted for the arithmetic
modules to answer. `.explain` on a verdict therefore descends through the
division and multiplication recursions all the way to the digit tables —
and, under `binary-nand-arithmetic`, to a single NAND axiom.

## Testing

`src/test/test_primes.cpp` runs both modules against all three arithmetic
substrates. `src/test/test_stratified.cpp` covers the scheduling that
`primes-naf` depends on.
