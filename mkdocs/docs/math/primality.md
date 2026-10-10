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
zelph+> ? :testprime &97
Answer: (:testprime &97) = prime
zelph+> ? :testprime &91
Answer: (:testprime &91) = composite
zelph+> &91 hasdivisor _D
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

The statement "N is prime" is universally quantified – *all* potential divisors yield a non-zero remainder – which might superficially imply using negation-as-failure with `hasdivisor`. This would be incorrect here. Negation-as-failure evaluates absence within the **current** graph state, yet `hasdivisor` facts continue to be derived across multiple fixpoint iterations. Forward chaining never retracts a fact once established: thus, an `isprime` fact derived too early would stand.

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

`.strata` lists the rule as not stratifiable nonetheless, together with the
whole arithmetic. The analysis compares predicates, and the rule delivering
the verdict, `(N testprime N, N isprime N) => ((N testprime N) = prime)`,
creates an `=` fact – the predicate for every arithmetic result the scan
reads – and contains `(N testprime N)`, which starts the scan. Neither can
produce a fact that the negation relies upon: the `=` fact has a
`testprime` term as its subject, never one of the arithmetic terms the scan
reads, and `(N testprime N)` is already established, being the rule’s own
premise. Thus, every `hasdivisor` fact for N is derived before the negation
is tested, and `.semi-naive check` verifies this in every run.

It scans eagerly up to √N, because the negation needs the full scan. In
return it finds **all** divisors ≤ √N, not just the smallest:

```
zelph> .import primes-naf
zelph+> ? :testprime &60
Answer: (:testprime &60) = composite
zelph+> &60 hasdivisor _D
Answer: &60 hasdivisor &3
Answer: &60 hasdivisor &6
Answer: &60 hasdivisor &5
Answer: &60 hasdivisor &4
Answer: &60 hasdivisor &2
```

## Choosing between them

| | `primes` | `primes-naf` |
|---|---|---|
| Technique | positive fold | negation-as-failure |
| Defers a negation | no | yes |
| Composite N | halts at the smallest divisor | examines up to √N |
| `hasdivisor` | one witness | every divisor ≤ √N |
| Resembles the definition | no | yes |

Neither is the "right" one. `primes` is the better computation; `primes-naf`
is the better statement of the mathematics. That both are expressible, in
the same language, over the same arithmetic, is the point of having both in
the standard library.

## Node-identity guards

The guards `R != &0`, `&2 == N`, `P == N` and the bound comparisons compare **nodes** using the relational facts from the comparison module. This approach is sound since every number involved is canonical, and canonical numbers are hash-consed: a single value corresponds to a single node, and distinct values produce distinct nodes, barring a hash collision, which zelph refuses ([details](../internals/performance.md#the-identity-foundation)).

## Cross-module cascade

No module performs computation independently. The expressions `(N mod D)`, `(D + &1)`, `(E * E)`, and `(P cmp N)` are ordinary facts asserted for the arithmetic modules to resolve. When `.explain` is applied to a `primes` verdict, or to a composite verdict from `primes-naf`, it therefore traverses the division and multiplication recursions down to the digit level. At this level, the leaves consist of digit-table entries and alphabet facts under `decimal-arithmetic` and `binary-arithmetic`, while under `binary-nand-arithmetic` they include the single NAND axiom, the two alphabet facts, and the `[absent]` leaves resulting from gate completion. On every substrate, the leaves also include the `:testprime N` request and structural base cases such as `(nil lcmp nil) res eq`. A prime verdict from `primes-naf` does not go through division at all: the scan stands behind its leaf `¬(N hasdivisor D) [absent]`.

## Testing

`src/test/test_primes.cpp` runs both modules against all three arithmetic
substrates. `src/test/test_stratified.cpp` covers the scheduling that
`primes-naf` depends on.
