# Arithmetic Substrates

zelph performs arithmetic — addition, subtraction, comparison, multiplication and division with remainder of arbitrarily large natural numbers — purely inside its reasoning engine. There is no arithmetic code in the C++ core: digits are ordinary named nodes, numbers are ordinary cons-lists, digit tables are ordinary facts, and the algorithms are ordinary forward-chaining rules. When you type `&12 * &34`, the engine does not call a multiplication routine; it _derives_ the fact `((&12 * &34) = &408)` the same way it derives `Berlin is located in Europe` from a transitivity rule.

This page describes the complete arithmetic system: the number representation, the shared architecture of the four rule modules, the base-independence property, and the engine machinery — bound-pattern grounding, cost-based condition ordering, semi-naive evaluation — that makes rule-based computation fast enough to be practical. It complements [Logic and Computation](../logic.md#semantic-math-computation-as-graph-rewriting), which builds up the addition module step by step.

The modules live in the standard library. Three of them supply the digit level, and one supplies the recursion that runs on top of any of them:

- [`stdlib/decimal-arithmetic.zph`](https://github.com/acrion/zelph/blob/main/stdlib/decimal-arithmetic.zph) – internal base 10, constructed from 2,700 digit-table facts generated automatically
- [`stdlib/binary-arithmetic.zph`](https://github.com/acrion/zelph/blob/main/stdlib/binary-arithmetic.zph) – internal base 2, built from 52 digit-table facts written by hand (full adder 16, full subtractor 16, digit multiplication 16, digit comparison 4)
- [`stdlib/binary-nand-arithmetic.zph`](https://github.com/acrion/zelph/blob/main/stdlib/binary-nand-arithmetic.zph) – internal base 2, derived from a **single NAND axiom**
- [`stdlib/common-arithmetic.zph`](https://github.com/acrion/zelph/blob/main/stdlib/common-arithmetic.zph) – the base-agnostic recursions, reused across all three

All three digit-level modules claim the module ID `arithmetic` via [`.provides`](../modules.md#interchangeable-implementations-provides), so anything built on top of arithmetic — [integers](integers.md), [polynomials](polynomial.md), the [symbolic layer](symbolic.md) — imports whichever substrate you loaded first and is otherwise indifferent.

They expose the identical user interface:

```
zelph> .import decimal-arithmetic
zelph+> ? &128 + &53
Answer: (&128 + &53) = &181
zelph+> ? &42 cmp &9
Answer: (&42 cmp &9) = gt
zelph+> ? &105 - &98
Answer: (&105 - &98) = &7
zelph+> ? &12 * &34
Answer: (&12 * &34) = &408
zelph+> ? &17 / &5
Answer: (&17 / &5) = &3
zelph+> ? &17 mod &5
Answer: (&17 mod &5) = &2
zelph+> ? &3 ^ &4
Answer: (&3 ^ &4) = &81
```

The [`?` prefix](../quickstart.md#two-statement-prefixes) ensures that each of those seven computations produces just one line of output: it asserts the request, runs the inference quietly and asks for the result. Written as a plain statement and a query, the identical calculation prints the derived fact, along with the conditions that led to it; `.deductions all` prints every intermediate step as well:

```
zelph> .import decimal-arithmetic
zelph+> (&128 + &53) = X
((&128 + &53) = &181) ⇐ {(((&128 add &53) ci 0) sum &181) (&128 + &53)}
zelph+>
```

A computation in zelph is not a black box returning a value – it is a set of ordinary facts, each carrying the conditions that produced it. Comparison additionally leaves the relational facts behind: `&42 cmp &9` derives `&42 > &9`, which is what makes computed order usable by [meta-rules](#the-four-operations).

## Numbers Are Graph Structure

A number is a cons-list of digit nodes, stored least-significant digit first: `<42>` is the structure `2 cons (4 cons nil)`. Cons cells are relation nodes — triples with `cons` as the predicate — so a number is nothing but nested statements, the same S-P-O material everything else in zelph is made of (see the [Lisp comparison](../logic.md#lisp-and-s-expressions)). The LSB-first order is an algorithmic choice: carries and borrows propagate from the least significant digit, so the recursion of every arithmetic rule simply follows the list. It also makes multiplication by the base a single cons: in LSB-first representation, `base * X` is just `(0 cons X)` — the "shift" of schoolbook multiplication is free.

The engine core knows exactly one convention about numbers: the `&` prefix means _decimal_, on input and on output. Everything else is defined by scripts. On input, the parser turns `&42` into the Janet call `(zelph/number "42")` — a hook each arithmetic script redefines to build its internal representation. The decimal script maps the literal one-to-one onto digit nodes; the binary script converts by pure string arithmetic (so arbitrarily large literals work) into a base-2 list. On output, the script registers its digit alphabet via `(zelph/set-number-digits ...)`; `node_to_string` subsequently renders any nil-terminated cons-list in canonical form (without a leading zero) composed exclusively of registered digit nodes as a decimal `&`-literal – precisely the reverse operation of the input conversion. Any other list keeps the generic `<...>` display, so cons-lists remain general-purpose and nothing globally reinterprets them.

The consequence: a binary session reads and writes decimal (`&5` in, `&5` out) while internally computing on `<101>`. Lists with leading zeros are tolerated as non-canonical values — `&105 - &98` internally yields the list `<007>`, which the subtraction module associates with its canonical form, resulting in `&7`, and which the comparison module treats as equal to `<7>` by value.

Only digits constitute a number, whether according to the rules or in the display. The digit tables contain no entry for a cell that is not a digit, hence `+`, `-`, `*`, `/`, and `mod` yield no result for such a list, and comparison checks every individual cell (see [The Four Operations](#the-four-operations)). The sole exception is exponentiation by zero: `X ^ &0` returns `&1` for any base, thus `<x 1> ^ &0` answers `&1` as well.

For the modules built on top, `common-arithmetic` offers the criterion as a numeral test upon request: a rule preparing to read a list as a number marks it with `(L needsnumeral L)` and reads `(L isnumeral true)`. A list meets the criteria of a numeral when it is non-empty, ends with nil, and each element is a digit. The output applies the same condition during the rendering of a list as an `&`-literal, and additionally requires canonical form: `<007>` is recognized as a numeral, though it prints as `<007>`. Only lists that are explicitly marked are subject to this verification, thereby maintaining cons lists as general-purpose. [Differentiation](symbolic.md#differentiation), the [simplifier](symbolic.md#the-simplification-core) for its list leaves, [`topoly`](topoly.md), [`symbolic-integers`](symbolic.md#integers-as-leaves), and the [integer façade](integers.md#the-uniform-operator-facade), for the magnitude of every operand of `+`, `-`, `*` and `cmp` it routes, apply the test before interpreting a list as a numeric value:

```
zelph> .import decimal-arithmetic
zelph+> :needsnumeral &123
(&123 isnumeral true) ⇐ {(3 isdigit true) (:needsnumeral &123) (&12 isnumeral true)}
zelph+> :needsnumeral <x 1>
zelph> L isnumeral true
Answer: &12 isnumeral true
Answer: &1 isnumeral true
Answer: &123 isnumeral true
```

The test proceeds by examining the suffixes in the list, which is why `&12` and `&1` yield responses; `<x 1>` does not, even though its suffix `&1` qualifies as a numeral.

The empty list `nil` (typed `<>`) does not qualify as a numeral and falls beyond the scope of the arithmetic contract. Since it represents the exhausted tail at which every digit recursion ends, certain operations read it as zero regardless: `+`, `-`, `cmp`, the exponent in `^`, and `*` when `nil` appears as the second factor and the first operand contains more than a single digit in the base of the substrate (`(&3 - nil) = &3`, `(nil cmp &3) = lt`, `(&2 ^ nil) = &1`, `(&12 * nil) = &0`). When `nil` serves as the first factor of `*`, as either operand in `/` and `mod`, or when viewed through the integer façade, it derives nothing. Likewise, a result that would otherwise be the empty list does not become `&0`: `(nil + nil)` answers `nil`, and `(nil - nil)` derives nothing.

Janet's role ends at load time: it generates the digit tables, a macro-like input-time job (see [Scripting with Janet](../janet.md)). During inference, only the reasoning engine runs.

## The Anatomy of an Arithmetic Module

The same architectural design underpins all four operations, clearly evident within the scripts.

**Digit knowledge as facts.** All single-digit arithmetic lives in lookup tables of ordinary facts. For base 10 they are generated by short Janet loops at load time; the largest is the multiplication table `((a dx b) tci c) pd d` / `((a dx b) tci c) mco e`, which covers all digit pairs with a running carry `c ∈ {0..8}` — 1,800 facts, and the carry range is closed since `floor((81+8)/10) = 8`. For base 2 the tables are written by hand and are recognizable classics: the 16-fact full-adder truth table, the full subtractor, and an AND gate with increment.

**Trigger → Decompose → Assemble → Connect.** Each module seeds an internal recursion from the user-facing fact:

```
(N + M) => ((N add M) ci 0)
```

Decomposition rules then peel the operand lists digit by digit while threading the carry (or borrow) state; assembly rules build the result list bottom-up once the inner subproblem is solved; a final connect rule exposes the result under the user-facing predicate:

```
(N + M, ((N add M) ci 0) sum T) => ((N + M) = T)
```

The internal state facts — `((<A> add <B>) ci C)` and friends — are themselves ordinary facts: you can query them, and they persist as reusable knowledge. Computing `&99 + &1` leaves behind every intermediate carry state, and future computations that reach the same state reuse it instead of re-deriving it.

**Table space vs. state space.** The digit tables are keyed by dedicated predicates (`tci` for the carry, `tbi` for the borrow) that differ from the recursion-state predicates (`ci`, `bi`, `mci`). This schema separation guarantees that table facts are only ever reached through fully bound lookups and never appear in the extensions the recursion rules scan — a design decision that turned out to matter enormously for performance (see below).

**Partiality by absence.** Subtraction over the natural numbers forms a partial function, and the module encodes this trait without relying on error-handling mechanisms: there is deliberately no base fact for a net borrow (`((nil sub nil) bi 1)` is absent), hence `&5 - &7` derives nothing. The lack of a fact encodes undefinedness – the natural failure mode in a forward-chaining system.

## The Four Operations

**Addition** is the archetype, developed in detail in [Logic and Computation](../logic.md#semantic-math-computation-as-graph-rewriting): a full-adder cascade over the digit lists, with zero-extension for operands of unequal length.

**Comparison** (`N cmp M`) is an LSB-first state machine. Decomposition rules derive an `lcmp` state for every suffix pair; compute-upward rules then resolve them from the inside out — the more significant rest dominates unless it is `eq`, in which case the current digit pair decides via the `dcmp` table. Missing digits of the shorter operand count as 0, so non-canonical lists compare correctly by value. The results are _relational_ facts — `N < M`, `N > M`, `N == M` — which compose with meta-rules like any declared knowledge:

```
zelph> .import decimal-arithmetic
zelph+> (R is transitive, A R B, B R C) => (A R C)
((R is transitive), (B R C), (A R B)) => (A R C)
zelph> > is transitive
zelph> &30 cmp &20
(&30 > &20) ⇐ {((&30 lcmp &20) res gt) (&30 cmp &20)}
zelph+> &20 cmp &10
(&20 > &10) ⇐ {((&20 lcmp &10) res gt) (&20 cmp &10)}
(&30 > &10) ⇐ {(> is transitive) (&20 > &10) (&30 > &20)}
```

(Each `cmp` additionally outputs the states generated by its own recursive
calls along with its `= gt` result; only the lines relevant to this
context are shown.)

The derived `&30 > &10` was never computed digit-wise; it follows from the transitivity meta-rule applied to two computed facts. Computation and reasoning are literally the same operation.

The more significant rest exerts control, though it does not decide independently: passing its outcome upward remains contingent upon the current cells being digits. Consequently, comparison interprets a non-empty list as a number only if every constituent cell holds a digit, and derives nothing for any other non-empty list, no matter what the more significant cells hold (the empty list constitutes a unique scenario [as detailed here](#numbers-are-graph-structure)). `<x 1>` represents the list `x cons (1 cons nil)`, where `x` resides in the least significant cell:

```
zelph> .import decimal-arithmetic
zelph+> ? <x 1> cmp &0
zelph+> ? &123 cmp &45
Answer: (&123 cmp &45) = gt
```

**Subtraction** mirrors addition with borrow in place of carry — the rule blocks are the structural mirror image — plus the deliberate partiality described above.

**Multiplication** is schoolbook recursion on the first operand's digits: `(A cons R) * M = A*M + base * (R * M)`, where the base-shift is a free cons. The digit-times-number stage threads a running carry through the `dx` table. The accumulation stage does something worth pausing on: one rule _asserts an ordinary `+` fact_, the addition module derives its `=` result, and a follow-up rule consumes it. The modules know nothing about each other — they communicate exclusively through the shared fact space. This cross-module cascade is the pattern the whole system scales by: any rule, including user-defined ones, can consume computed results and trigger further computations.

**Division** (`N / M`) and **remainder** (`N mod M`) complete Euclidean
division on the naturals -- and they are the deepest cross-module cascade in
the system. The module contributes no digit table of its own: candidate
products come from the multiplication module's digit-times-number engine
(`dmul`), candidate differences from the subtraction module, and
quotient-digit selection from the comparison module. Its only base-specific
data is the digit alphabet itself, stated as facts (`0 isdigit true`, ...).

Long division looks like the wrong fit for LSB-first lists -- it works on the
most significant digit first -- but the representation pays off a third time.
For `N = (A cons R)`, i.e. `N = A + base*R`, the recursion descends into the
more significant part `R` first. If `R = QR*M + RR` with `RR < M`, then the
current step must solve `t = q*M + r` for `t = A + base*RR` -- and in
LSB-first representation, `t` **is** the cons cell `(A cons RR)`. The "bring
down the next digit" step of schoolbook long division is a single cons, just
as the base-shift of multiplication was.

The quotient digit is selected without backtracking and without any
digit-ordering knowledge, exploiting the fixpoint engine's natural
parallelism over candidates: all products `q*M` are derived up front (shared
across all recursion steps -- and, by hash-consing, across every computation
involving `M`), all differences `t - q*M` are asserted as ordinary `-` facts,
and the unique candidate with `t - q*M < M` is selected via `cmp`. Uniqueness
is an arithmetic theorem the rules simply inherit: the invariant `rem < M`
bounds `t` below `base*M`, so exactly one digit satisfies the constraint.
Candidates with `q*M > t` need no handling at all -- subtraction is partial,
their difference facts simply never come into existence.

Partiality composes: division by zero requires no dedicated rule. Every
candidate difference equals `t` by value, `t < 0` is unsatisfiable, no
quotient digit is ever selected -- `&5 / &0` derives nothing, exactly as
`&5 - &7` derives nothing. Undefinedness remains encoded as absence.

**Exponentiation** (`N ^ M`) is the naive recursion — repeated multiplication, delegating to the multiplication module the way multiplication delegates to addition. `X ^ &0` is `&1` for every base, including `&0`: the empty-product convention, shared with the [polynomial layer's](polynomial.md) `ppow`.

## One Rule Set, Any Base

The decimal and binary substrates lack recursion rules: each imports the same module, `common-arithmetic.zph`, and varies solely in its digit tables (and the input conversion); the NAND module’s own rules merely derive its tables from a single gate fact and the digit alphabet. This makes a coherent insight: recursion rules are base-agnostic theorems concerning digit sequences, and the tables are the sole locations where "ten" or "two" occurs. Loading the binary module yields full adders, full subtractors, and AND gates as facts, with the same algorithms operating atop – a semantic network performing computations akin to digital hardware, while interfacing with decimal at the boundary.

## Asserting and Querying

`&A + &B` is an _assertion_: it states the fact and lets the fixpoint derive everything that follows, including `((&A + &B) = ...)`. Repeating the same assertion after the fixpoint correctly produces no output — there is nothing new to derive. The repeatable retrieval idiom is the _query_ form with a variable, uniform across all operations:

```
(&12 + &34) = X
(&42 - &9) = X
(&42 cmp &9) = X
(&12 * &34) = X
```

Queries are always evaluated; once the result fact exists, they answer from the graph (`Answer: ...`), repeatably. For `cmp`, a bridge rule additionally exposes the outcome under `=` (answering `gt`, `lt`, or `eq`), while the relational facts remain the primary, composable output.

## Arbitrary Precision for Free

Because numbers are lists, there is no word size:

```
zelph+> ? &3495734893 * &92348793847
Answer: (&3495734893 * &92348793847) = &322826900977421603371
```

That is an exact 21-digit result. For comparison, asking the embedded Janet runtime — a full scripting language — for the same product yields `3.22826900977422e+20`, a double-precision approximation. The reasoning engine is more precise than the programming language living in the same binary, because structural numbers have no width limit.

## Why This Is More Than a Demo

The comparisons in [Logic and Computation](../logic.md#comparisons-with-other-systems) position zelph relative to Prolog and Datalog, Lean, Gödel numbering, and Lisp. Arithmetic is where those comparisons stop being philosophical.

In [Lean](../logic.md#lean-and-curry-howard), numerical values, proofs, and the mechanisms governing inference exist on distinct levels; reasoning *concerning* these mechanisms necessitates ascending to a meta-level. In zelph, there is no meta-level to ascend into: the number `<42>`, the rule responsible for its addition, the fact documenting the sum, the facts from which `.explain` rebuilds its derivation, and a meta-rule that quantifies over the predicates in play are all nodes within a single graph, processed by one engine. When the multiplication module asserts a `+` fact for the addition module to answer, the distinction between object level and meta level dissolves into plain fact flow.

Where Gödel numbering encodes formulas _as_ numbers to make arithmetic self-referential, zelph runs the arrow in the other direction and makes numbers _structural_: no encoding, no decoding — the digit list _is_ the number, and it participates in statements directly. And against Datalog: computed facts are indistinguishable from declared ones, predicates are first-class, and therefore arithmetic results feed meta-rules (`> is transitive`) that standard Datalog can only represent via an encoding.

These modules are the ground floor of a mathematics engine that is now several storeys tall: [signed integers](integers.md), [multivariate polynomial normal forms](polynomial.md), a [symbolic term layer](symbolic.md) with differentiation, and a [compiler](topoly.md) that decides polynomial identities by node identity. Every one of them is rules over this same graph — which was the bet: because terms, rules and equations share one substrate, algebraic rewriting is just more rules.

The [Jacobian case study](tutorial-jacobian.md) runs the whole stack, from these digit tables up to a 2026 counterexample to a 1939 conjecture, in under a second.

## From Arithmetic to Number Theory: Primality

The four operations invite a first genuinely number-theoretic question: is
N prime? The standard library answers it twice, with two deliberately
different designs on top of the same arithmetic modules:

- [`stdlib/primes.zph`](https://github.com/acrion/zelph/blob/main/stdlib/primes.zph) — negation-free, via a positive fold
- [`stdlib/primes-naf.zph`](https://github.com/acrion/zelph/blob/main/stdlib/primes-naf.zph) — the textbook rule, via negation-as-failure

Both load on top of either arithmetic module and expose the same repeatable
query idiom:

```
.import decimal-arithmetic            # or: .import binary-arithmetic
.import primes                # or: .import primes-naf
? :testprime &113
Answer: (:testprime &113) = prime
```

Both perform trial division with the square bound (candidates stop once
E*E > N), and the primes modules contribute **no arithmetic of their own**:
candidate successors are asserted as ordinary `+` facts, the bound as `*`
and `cmp` facts, divisibility as `mod` facts — the arithmetic modules answer
them all. A single primality test is the deepest cross-module cascade in the
standard library, with division itself internally cascading through
multiplication, subtraction, and comparison.

**The version excluding negation: the fold serves as the scheduler.** "N is prime" constitutes a universally quantified statement – _every_ candidate produces a remainder – a claim that cannot be resolved by any single lookup among positive facts. `primes.zph` builds the universal from positive facts: a fold `(N nodivupto D)` progressively gathers one verified non-divisor at a time, and the next candidate E = D+1 only appears after D has been verified. The sequence forming the proof simultaneously controls the search: for composite N, recursion halts at the smallest divisor – no additional computation takes place past the verdict – and `hasdivisor` detects exactly one witness, which must be the smallest prime factor.

**The NAF version: the definition itself, executable.** Under
[stratified evaluation](../logic.md#stratified-evaluation) the textbook
formulation is sound as written:

```
(N testprime N, &2 < N, ¬(N hasdivisor D)) => (N isprime N)
```

The rule remains deferred until the positive rules – the full candidate scan – achieve quiescence, ensuring that negation tests absence against the complete scan, and nothing its conclusion leads to can add a divisor afterwards. [Primality](primality.md#primes-naf-the-textbook-formulation) explains why `.strata` continues to classify the rule as not stratifiable. The trade-offs mirror the fold version: enumeration proceeds eagerly (composites pay the full scan, without early exit), and in exchange, `hasdivisor` produces a list of _all_ divisors up to the square bound.

Shared properties: 0 and 1 receive no verdict at all — neither prime nor
composite, partiality by absence as everywhere in the stdlib. And the
verdicts are ordinary relational facts (`N isprime N`, `N hasdivisor D`)
plus a result bridge under `=`, so they compose with meta-rules and further
computations like any declared knowledge — a computed `isprime` fact can,
for instance, feed a rule that cross-checks Wikidata's prime-number claims.

## Making It Fast

Naively, "arithmetic as rules" sounds hopeless: a fixpoint engine re-evaluates rules until nothing new appears, and a single multiplication spawns hundreds of intermediate facts. Three engine mechanisms make it practical — none of them arithmetic-specific.

**Bound-pattern grounding.** A structured condition subject whose variables are all bound — such as the table lookup `((A d+ B) tci C)` once `A`, `B`, and `C` are known — denotes exactly one fact node. The engine resolves it with a single hash lookup instead of any scan; if the denoted fact does not exist, the condition fails immediately. This is what makes the digit tables behave like actual lookup tables even though they are ordinary facts: 1,800 multiplication-table entries, and the engine touches exactly the one it needs.

**Cost-based condition ordering.** Before evaluating a rule, the engine orders its conditions so that cheap, binding-rich conditions run first and every later condition profits from the accumulated bindings — ideally becoming groundable. The scoring estimates the actual scan each condition would cause (bound anchors, relation cardinalities) rather than guessing from syntax alone.

### Semi-naive Evaluation

The engine’s default fixpoint strategy is _delta-driven_. Following a single classic pass across the whole graph, each subsequent iteration is driven by the facts created in the previous iteration: for every newly introduced fact, the engine identifies which rule conditions might match it, directly binds that condition to the singular fact – entirely eliminating the need for scanning – and then evaluates only the remaining conditions of the rule, which, thanks to the newly established bindings, typically become direct lookups. Two kinds of rule are not processed in this seeded manner. Rules that include a negated condition are excluded from the first pass and instead execute as classic passes whenever the delta has completely drained, progressing incrementally through each negation level ([Stratified Evaluation](../logic.md#stratified-evaluation)). A limited set of rules whose conditions cannot be entirely seeded – nested conditions, path conditions like `P279⁺`, neural conditions, or conditions lacking exactly one predicate – are processed via classic passes in every iteration.

This is a general evaluation strategy, not an arithmetic feature — it is how mature Datalog engines operate. Its payoff is largest for deeply _iterative_ rule systems: workloads with many fixpoint iterations that each add only a few facts while the internal state relations keep growing. Naive evaluation re-scans all of that state in every pass, so its cost per new fact grows as the computation proceeds. Arithmetic recursion is the extreme case of this shape, which is why the effect is dramatic here: multi-digit multiplications run several times faster than under naive evaluation, and the gap widens super-linearly with operand size — a ten-by-eleven-digit binary multiplication completes in seconds semi-naively, while the equivalent naive run had to be aborted after minutes. One-shot workloads that reach their fixpoint in a pass or two — applying a constraint rule once over an imported Wikidata graph, say — see correspondingly less benefit.

The strategy is exposed as a command:

```
.semi-naive on     (default) delta-driven evaluation
.semi-naive off    classic naive evaluation
.semi-naive check  delta-driven, followed by classic verification passes
```

The objective of the modes is to produce identical results, with `check` verifying the segment managed by the delta path. Once the delta has fully drained, it re-runs classic passes repeatedly until no further changes occur, and turns any fact overlooked by the delta path into a hard error. Likewise, it raises an error if a fact rests on a negated premise that has become true and is not derived by any other means ([Stratified Evaluation](../logic.md#stratified-evaluation)). It does not separately execute the classic evaluator and then contrast the outcomes. The test binary sets every engine to `check` mode by default, implying that every test that does not explicitly choose a mode also checks the delta-driven fixpoint against classic passes.
