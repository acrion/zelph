# Performance Architecture

This section of the documentation is for contributors, not users. zelph's
performance machinery is invisible by design: every mechanism described here
is semantically neutral, so results, deductions, and command behavior are
identical with and without it. That is exactly why it appears nowhere in the
user-facing pages — and why it is collected here instead, together with the
soundness arguments that license it. The companion page
[Measurement Methodology](measurement.md) documents how changes to this
machinery are validated; treat the two pages as one contract.

For orientation, a snapshot of what the machinery buys (July 2026): the
symbolic-mathematics case study — nine partial derivatives, simplification,
and polynomial compilation of a 3×3 Jacobian determinant
([test_jacobian.cpp](https://github.com/acrion/zelph/blob/main/src/test/test_jacobian.cpp))
— took 23 and 9 minutes for its two phases when this work began, and takes
0.9 and 1.2 seconds today (roughly 1500× and 480×), with every semantic
counter bit-identical and the full test suite permanently running in
`.semi-naive check` mode. Every subsection below contributed to that factor.

## The Identity Foundation

Everything on this page is a corollary of one design decision: **a fact
node's ID _is_ the hash of its triple**, `create_hash(predicate, subject,
objects)` — see [Internal Representation of Facts](../index.md#internal-representation-of-facts)
for the topology this identifies. Four consequences carry all the soundness
arguments that follow:

1. A node's genuine triple is **immutable from creation**: the ID pins it.
   Graph growth can change how a triple is _reconstructed_ from adjacency,
   never what the triple _is_.
2. Hash-consing materializes **children before parents**, so per-node
   bookkeeping computed bottom-up at creation time is final — there is no
   "later update" case to handle.
3. Two equal, fully concrete structures are **the same node**. Concrete
   nodes therefore unify only via identity, which is what makes anchoring
   (below) complete.
4. Hash-consed structures are **acyclic**: no node contains itself.

## The Reconstruction Problem

zelph stores no triples; it stores topology. `get_fact_structures`
(`fact_structure.hpp`) reconstructs a node's `(subject, predicate, objects)`
readings from its adjacency: the predicate is a right neighbor in the
declared relation-type set, the subject is a bidirectional neighbor, objects
are pure incoming edges. The hard part is disambiguation — a fact node that
is itself the _subject_ of further facts acquires bidirectional neighbors
that masquerade as subjects, and a heuristic that inspects up to three
adjacency hops separates genuine subjects from such "child facts". Ambiguous
candidate sets are pruned by **hash verification**: a candidate reading is
genuine iff `create_hash` over it reproduces the node's ID (foundation
fact 1 at work).

This reconstruction is correct but expensive — O(deg²) on hub neighborhoods
— and the engine consults structures on its hottest paths (unification,
template rejection, grounding, anchoring). The layered lookup below exists
to make the walk the _exception_.

## The Layered Structure Lookup

`get_fact_structures` answers through four layers, cheapest first:

1. **Structureless bit gate (lock-free).** Atoms (sequential IDs) and
   variables can never decompose, so `!is_hash(n) || is_var(n)` answers with
   a shared empty list before any lock or cache probe — two bit tests.
   Soundness: a structure requires a declared relation type among the node's
   right neighbors; every edge out of a non-hash node leads to a hash fact
   node by construction of `connect()`, and hash nodes enter the
   relation-type set only through an explicit `(hashnode ~ ->)` declaration,
   which neither the parser, the stdlib, nor any import produces. This is an
   accepted exotic divergence class, backstopped by `.semi-naive check`. The
   gate is _static_ — it holds after binary loads too, so on a loaded
   Wikidata graph every Q/P atom answers without touching a lock.
2. **The fact-structure cache (`_fs_cache`).** A promotion cache mapping
   node → immutable shared structure list (`FactStructurePtr`). A hit costs
   one shared-lock pair plus one atomic refcount increment — no deep copy —
   and a held pointer stays valid across invalidations, referencing a
   consistent snapshot. All empty results share one static instance, so
   negative entries (the most frequent lookups on the unify recursion path)
   allocate nothing.
3. **The genuine-structure store (`_genuine`).** `Zelph::fact()` records the
   exact triple of every node it creates as a one-element immutable list,
   at the moment the triple is known and final (foundation facts 1 and 2).
   Cache misses consult it before walking; hits are promoted into the
   fs_cache. Two deliberate exclusions: facts with `subject == predicate`
   are **not** stored — the reconstruction walk yields _empty_ for those,
   and unification's atom treatment of them is pinned behavior — and
   self-facts store `objects == {subject}`, matching the walk's
   self-referential repair exactly. Because a node's ID pins its triple,
   store entries can never go stale through graph _growth_; only topology
   destruction disarms them (next section).
4. **The reconstruction walk.** The historical semantics, kept verbatim. In
   normal operation it serves only `subject == predicate` facts; after a
   store disarm it serves everything — and it is itself fast now, running
   under a single `ReadScope` (below) instead of paying a lock pair and an
   adjacency copy per neighborhood probe.

### Per-node cache invalidation

The fs_cache formerly suffered a wholesale clear on _every_ created fact,
keeping it near-permanently empty on rule-heavy workloads. `fact()` now
calls `invalidate_fact_structures_for`, which erases only what growth can
actually affect: the new relation node and its components, plus one
_bidirectional_ adjacency level around subject and objects (the neighborhood
the child-fact heuristic inspects). The correctness argument rests on
monotonicity: growth can only _add_ reconstruction candidates, and hash
verification prunes any ambiguous set back to the genuine reading. Two
escape hatches degrade to the wholesale clear: relation-type declarations
(`P ~ ->`), which can change predicate detection for _any_ entry and also
invalidate the memoized relation-type set, and neighborhoods exceeding a
fixed stale budget (hubs). The budget philosophy recurs throughout the
engine: degradation is never unsound and never worse than the old
semantics. One residual risk is consciously accepted — entries that no
candidate hash-verifies (e.g. `subject == predicate` readings) are not
re-checked on deeper-level growth; the suite-wide `.semi-naive check` net
backstops it.

## The Template-Variable Store

`_template_vars` maps every created node whose structural closure contains
variables to its **exact variable set**, maintained bottom-up by `fact()`
from the actual triple arguments. Entries exist _only_ for nonempty sets, so
while the store is authoritative, **absence means "provably no variables"**
— `var_in_closure(n)` is a single map probe, and `collect_variables` is
O(1). This is the criterion separating rule-template nodes from data nodes,
consumed by the deep template rejection in `extract_bindings`, by anchor
eligibility, and by bound-pattern grounding. Unlike the former
reconstruction-based walk, the store cannot be misled by ambiguous adjacency
readings; it deliberately covers `subject == predicate` facts, whose closure
variables the walk cannot see — the exact answer is the safer one for
template-leak prevention (an accepted, documented divergence).

## Authoritative Bits and the Disarm Funnel

Both stores carry an _authoritative_ flag with a one-way discipline:
**disarmed stores are never re-armed**. Absence of an entry is meaningful
while a store is authoritative, and no retroactive scan could soundly
recreate that property (`.new` re-arms by creating a fresh engine). The
disarm funnel is the single shared implementation `disable_fact_stores()`,
reached through `invalidate_fact_structures_cache` — trusted imports, binary
loads (`.load`), node removals, merges, and name merges, i.e. every path
that either bypasses triple-level construction or destroys topology — and
through the explicit `.fact-stores off` command. Growth-only full clears
(relation-type declarations, stale-budget degradation) deliberately do _not_
touch the stores: they are growth-immune by the identity foundation.

The trade-off the switch controls is memory: roughly 150 bytes per
`fact()`-created node. Wikidata-scale graphs are neutral _by construction_ —
the first trusted import or `.load` disarms the stores before they could
grow — and rules typed onto a loaded billion-node graph still work normally,
on the walk path, which is itself faster than it was before this project.

## ReadScope: One Lock Pair per Read Region

`Network::ReadScope` acquires shared locks on both adjacency maps (left
before right — the writer order of `connect()`) and hands out _references_
into the maps for its lifetime, replacing sequences of `get_right`/`get_left`
calls that each paid a rwlock pair plus a full `adjacency_set` copy. Its
hard rules are absolute for any code running under a live scope: never write
to the network, never take another network lock (no nested scope), and never
call the locking API — `get_right`, `get_left`, `exists`, `check_fact`,
`parse_relation`, `format`, `log`, or any output stream.
`std::shared_mutex` shared-locking is not guaranteed reentrant, and a writer
queued between two shared acquisitions deadlocks the process. Prefetch
everything that locks (e.g. the relation-type memo) _before_ opening the
scope. Consumers today: the whole reconstruction walk, `check_fact`'s edge
probe (`fact_edges_hold`), anchored-candidate collection, and the
partial-anchor climb.

A layering rule guards its construction: `zelph_impl.hpp` is included
_only_ by `zelph.cpp` (Cap'n-Proto layering), so `Zelph::read_scope()` is
declared in `zelph.hpp` but defined in `zelph.cpp`, and `ReadScope` itself
lives in `Network` (`network.hpp`). Never name `Impl`-nested types or
dereference `_pImpl` in other headers — this is a recurring, build-breaking
mistake.

## Candidate Sets: Anchoring and Semi-Naive Seeding

The user-facing semantics of these features live elsewhere —
[Semantic Arithmetic](../math/arithmetic.md) introduces bound-pattern grounding
and semi-naive evaluation, [Stratified Evaluation](../logic.md#stratified-evaluation)
covers the negation schedule, and `.help .anchors` / `.help .semi-naive`
document the switches. This section records the engineering invariants.

**Anchoring** replaces full-relation scans with adjacency lookups from a
concrete node. Subject/object-driven anchors collect a candidate's adjacency
under one lock scope, rejecting rule-topology nodes via `var_in_closure`.
**Bound-pattern grounding** resolves a fully bound structured pattern to the
single node it denotes via pure hash lookups — with deliberate exact
object-set semantics — and can fail a condition outright when the denoted
fact is missing. **Partial-pattern anchoring** handles the partially bound
case: any concrete node inside the pattern must appear _identically_ in
every matching graph fact (foundation fact 3), so climbing the adjacency
levels from the lowest-degree anchor, filtered by the pattern's predicate
chain, yields a complete candidate superset. Predicate positions never
qualify as anchors — a fact points _to_ its predicate, so the predicate's
incoming side is the full extent, exactly the scan being avoided. All
anchoring is budgeted, and an exceeded budget falls back to the full scan,
never to a truncated candidate set: soundness is unconditional (candidates
still pass structural unification) and completeness is budget-independent.
`.anchors off` restores the anchor-free naive reference, decoupled from
`.parallel`; tests use it as an independent completeness check.

**Semi-naive evaluation** builds a static per-run index (`IndexedRule`):
each rule's seedable leaf conditions, a predicate → (rule, leaf) index, a
wildcard list for variable-predicate leaves, and — hoisted out of the
`Unification` constructor — the rule-static **pattern decomposition**
(`PatternInfo`: relation, subject, objects, subject-predicate hint), which
is a pure function of the immutable condition node and is reused by every
seed. Binding-dependent work (relation-variable resolution, grounding,
boundness analysis, anchoring, snapshot launches) stays per-instance. The
delta is captured by the fact-creation observer, and a seeded `Unification`
has a candidate set of exactly one fact. Rules whose seeding cannot be
proven complete (nested conjunction elements, ambiguous predicates, neural
conditions) are classified _delta-unsafe_ and run classically each
iteration; rules with negation form the deferred stratum. The `check` mode
appends classic verification passes and names any fact the delta path
missed — the completeness net everything above is measured against.

**Join ordering** (`optimize_order`) carries a connectivity term: a
condition sharing _no_ variable with the current bindings starts an
unconstrained cross-product scan and must lose against every connected
condition, whatever the cardinalities. Variables at any structural depth
count as connecting — the decisive case is a bound variable sitting inside
a nested pattern, invisible to subject/object boundness scores. Guard
conditions (`!=`, neural, negation) are pushed last via tier penalties that
dominate the connectivity term.

## Smaller Fast Paths

A few hot-path rewrites are worth knowing before touching their call sites.
`check_fact` probes all edge memberships of the exact triple under one lock
scope on references (`Network::fact_edges_hold`); the expensive
hash-collision diagnostics are a cold branch that fetches its own copies.
`create_hash` over an object set skips the copy+sort normalization whenever
the set's storage mode already iterates ascending (small sets — nearly all
of them); large unordered storage keeps the normalization so the hash stays
a pure function of the element _set_. `parse_relation` prefilters right
neighbors through the memoized relation-type set before running the exact
probe; its `ReadScope` variant `parse_relation_scoped` uses membership
(is-known) semantics, documented as exactly equivalent within
reconstruction. Finally, output streams route their flush through the print
mutex (`locked_stream`) — a correctness contract, not an optimization: pool
workers log concurrently, and an unserialized stateful output handler is a
data race.

## Reading the Code

The map, in dependency order:
[`network.hpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/network.hpp)
(adjacency maps, `connect`, hashing, `ReadScope`, `fact_edges_hold`,
`collect_anchored_facts`);
[`zelph.hpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/zelph.hpp) /
[`zelph.cpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/zelph.cpp)
(the stores, the fs_cache and its invalidation, the relation-type memo,
`read_scope`);
[`zelph_impl.hpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/zelph_impl.hpp)
(store members — included only by `zelph.cpp`, see the layering rule above);
[`fact_structure.hpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/fact_structure.hpp)
(the layered lookup and the reconstruction walk);
[`unification.cpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/unification.cpp) /
[`unification.hpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/unification.hpp)
(grounding, anchoring, `PatternInfo`, the scan loops);
[`reasoning_seminaive.cpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/reasoning_seminaive.cpp)
(`IndexedRule`, the delta loop, strata, the check mode);
[`reasoning.cpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/reasoning.cpp)
(`optimize_order`);
[`reasoning_profiler.hpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/reasoning_profiler.hpp)
(every counter the [measurement page](measurement.md) relies on). The
regression tests pinning this machinery live in
[`src/test`](https://github.com/acrion/zelph/tree/main/src/test) —
`test_check_fact.cpp`, `test_fact_cache.cpp`, `test_genuine_structure.cpp`,
`test_var_closure.cpp`, `test_partial_anchor.cpp`, and `test_seminaive.cpp`;
their comments are primary sources for _why_ each pin exists.
