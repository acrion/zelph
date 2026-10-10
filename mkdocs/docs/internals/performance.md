# Performance Architecture

This section of the documentation is for contributors, not users. zelph's
performance machinery is invisible by design: every mechanism described here
is semantically neutral, so results, deductions, and command behavior are
identical with and without it. That is exactly why it appears nowhere in the
user-facing pages — and why it is collected here instead, together with the
soundness arguments that license it. The companion page
[Measurement Methodology](measurement.md) documents how changes to this
machinery are validated; treat the two pages as one contract.

To provide context, a snapshot from July 2026 shows the performance gains: the symbolic-mathematics case study – involving nine partial derivatives, simplification, and polynomial compilation of a 3×3 Jacobian determinant ([test_jacobian.cpp](https://github.com/acrion/zelph/blob/main/src/test/test_jacobian.cpp)) – took 23 and 9 minutes for its two phases at the start of this effort, but now completes in 0.9 and 1.2 seconds respectively (approximately 1500× and 480× faster), with every semantic counter remaining bit-identical. Each subsection below played a role in achieving this improvement factor.

## The Identity Foundation

Everything on this page is a corollary of one design decision: **a fact
node's ID _is_ the hash of its triple**, `create_hash(predicate, subject,
objects)` — see [Internal Representation of Facts](../rules.md#internal-representation-of-facts)
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

Fact 3 does not apply in reverse: the identifier is a 62-bit hash of the data, implying that two separate structures share the same ID if their hashes coincide in a collision. For n structured nodes, the birthday bound predicts the expected number of colliding pairs to be about n²/2⁶³: roughly 10⁻⁷ when dealing with a million nodes, no more than approximately 0.0014 in the [`-medium` Wikidata artefact](../binaries.md) containing 1.1·10⁸ nodes, and ranging from 0.1 to 0.3 for a complete import of 10⁹ to 1.6·10⁹ triples. The system refrains from comparing the actual structures, so while a collision is detected, it is not resolved. The `fact()` function detects an ID already assigned to a node that does not hold the new statement and rejects the statement, naming both. During inference, a derived statement rejected in this manner is marked as a contradiction, including the cause; if a nested component of a derived statement is rejected, the run stops with the same notification. In bulk import, such a triple is left unwritten; the system reports the first occurrence during import and delivers the overall count once the process finishes. Either way, the second statement is lost. The `fact()` function determines "does not hold" by checking the subject and every object against the node's edges; regarding the predicate, it only examines the weight of the node's edge to it, which defaults to 0 if no such edge exists. A colliding statement sharing the same subject, where all its objects are part of the node's own set, is thus deemed known. If the predicate matches the node's own, or is any other node the node has an edge to – such as its subject – it goes undetected. For any other predicate, it is treated as known to be false. The `fact()` function then rejects it with the message "this fact is known to be wrong" instead of referencing the collision; during inference, a derived statement becomes a contradiction without an explicit reason stated, and a nested component stops the run with this refusal. The bulk import also performs a lookup on the predicate. Collections that rules construct possess their own identifiers, whose collisions are entirely overlooked; refer to the following section.

## Rule-Built Collections: The Id Is the Recipe

This section stands apart on this page: the subject it describes is semantics, not acceleration. It rests on the identity foundation, though, and it decides what several readers below may assume regarding a node id.

A collection is the sole type of node for which the identifier is not its content – writing `@{a b}` on two separate occasions yields two distinct containers. When a rule builds a collection, the identifier is determined through computation instead of enumeration, and the higher-order bits indicate the origin of the collection:

| Bits 63..60 | Class | Built by |
| --- | --- | --- |
| `000x` | counter | `create()`: atoms, witnesses, the core nodes, the conjunction sets of typed rules, each collection written as data |
| `0010` | value recipe | a firing: the term it builds for a collection of the rule’s text |
| `0011`, bit 59 clear | written template | a collection written during rule writing: the parser, `zelph/build-rule`, `zelph/rule-text` |
| `0011`, bit 59 set | construction’s recipe | a construction: the collections of a rule that a generator writes |
| `01xx` | hash | facts and set constants |
| `1xxx` | variable | `var()` |

**Authorship constitutes the id class.** A collection is part of a rule's text – it is a _template_ – exactly when its id resides within the template class, making `Zelph::is_rule_template` one bit test, and nothing written later (a claim, a mark, a name, a membership) changes the result. Every other collection is a value, retained uniformly across all instantiations. The sole exception involves a conjunction set: although it is rule text, it is distinguished by its `~ conjunction` tag, not by its id – a typed rule's has a counter id. A firing keeps it, except where it writes a membership into it, which goes to the set's data term; a construction rebuilds it, except where a variable bound to the set occupies the position where a membership is written (refer to _A rule's text is fixed once the rule is written_ below). A recipe is not a hash, so every gate interpreting "not a hash" as "an atom or a collection" reads a recipe as a collection, and neither a fact nor a set constant can land on one.

**The key.** A firing builds `recipe_id(T, key)` for a template `T`, where `key` hashes the binding of variables that the instantiated statement meets (the consequence in a firing, the written rule in a construction), represented as sorted (variable, value) pairs; any variable left unbound by the binding does not appear in it. The collection that a consequence writes to uses the empty key, ensuring all firings of the rule target a single term (`Zelph::bucket_term_id`). `container_plan` (`reasoning.cpp`) is the sole location determining how an instantiation handles a container. The key walk (`statement_variables`) and the prediction of the existential check follow it, meaning the check looks for the node that the firing builds.

**Reuse never compares members.** `Zelph::recipe_collection` creates the node upon detecting a non-existent ID and asserts every missing membership. This mechanism enables a subsequent firing with the same binding to rediscover the collection – explaining why a collision between two recipes remains undetected: the two share one collection. For n terms, approximately n²/2⁶¹ pairs are expected to collide (4·10⁻⁷ at a million rule-built collections, 4·10⁻³ at 10⁸), while among the collections of constructions, n²/2⁶⁰. A collision involving a fact, a set constant, a counter, or the alternate class is impossible by range. During a firing, `recipe_collection` also claims a membership that a rule has specified as a pattern, as a firing claims every fact it derives.

**The counter is protected by guards.** IDs from bit 61 onward are assigned to recipes, so `Network::create` rejects requests once the counter reaches 2⁶¹, and `create_written_template`, which uses the next counter value as payload (`0x3000000000000000 | counter`), refuses once it reaches 2⁵⁹. Neither threshold is reachable in actual use, which is also why no test ever triggers them. `.save` and `.load` keep the counter state, ensuring that a template written after a load gets a fresh payload.

**A construction executes within a scratch cluster** (`ConstructionScratch`, `reasoning_deduce.cpp`). It builds the parts of the rule a generator writes, and keeps them, merged into the active cluster, precisely when the rule governing them is the one it returns. If a rule currently in force already says the identical assertion, the construction returns that rule – the _claim on creation_ – and discards any constructed elements, regardless of exit path, including upon exception throw. This mechanism also applies when a generator settles after a name merge: its next construction under the merged binding claims the rule reconstructed by the merge and leaves no residual artefacts. The rule a construction returns, whether claimed or kept, is remembered (`_claims`: statement and key → rule, one entry per unique construction), ensuring subsequent passes refrain from rebuilding the components; the record is cleared each time the fingerprint index becomes outdated.

**A rule's text is fixed once the rule is written.** A rule governing rules can bind a collection of another rule's text – via rule structure, as in `(G => (S q C)) => ((X r Y) => (X in C))`, which binds the collection of a ground rule, or through an engine marking fact – and a membership it states there would change what that rule says. The variable represents the collection's _data term_ instead (`Zelph::bucket_term_id`), the term a firing of a ground rule writes for that collection: a rule a construction writes names it wherever the collection appears within it, and a firing writes into it wherever it writes a membership. A conjunction set stands for its data term only where a membership is written into it, and a condition `X in C` that a construction writes in the rule is precisely such a membership: the condition is a membership fact, which, if included in the set, would make X a condition of the bound rule. A firing that writes into a conjunction set the rule holds by itself – where the relation was a variable at the time the rule was written, as in `(X R C)` – also writes into the set's data term. The term comes into existence at the first statement that names it, and remains empty until the rule whose collection it represents fires: the members of the rule's literal, and the one its statement writes, arrive with that firing. Three configurations arise depending on where substitution happens and are documented rather than changed: the data term of a collection whose rule contains variables is the term under no binding, which none of that rule's firings ever writes to, so a rule over the engine's marking facts that binds such a collection writes into a term the rule never encounters; a rule re-expressed from its bound parts, `(G => (S q C)) => (G => (S q C))`, is a second rule, over the term, beside the first; and when placed under a switch, that restated rule leaves the typed rule active. A membership written into a rule's own collection or conjunction set while no rule is being written – through Janet, the C ABI, or a name given to the collection – is refused (`Zelph::fact`), also when the collection already contains the member. Within the scope in which a rule is being written – the parser's, `zelph/build-rule`'s, `zelph/rule-text`'s, and a construction's – such a write is how the rule's text is formed. The scope is open only on the thread that opened it, and only on its network: a collection created by another thread meanwhile is considered data. Within the scope, a rule currently being written can still write into another rule's collection, when a Janet program holds the node of that collection; in such a case, the other rule's text is modified.

**The fingerprint index** (`zelph_maintenance.cpp`) maps a fingerprint common to alpha-equivalent rules to the set of rules possessing it, one entry per `=>` fact. It serves both `zelph/dedup-rule` and the claim. A newly introduced rule acquires its fingerprint during the subsequent lookup. Outside the scope a rule is written in, nothing writes into a rule's text after the rule is written (as previously noted), thus ensuring a fingerprint remains accurate for the lifetime of the rule, unless a name merge, deletion from a rule's text, or a load operation alters the text without creating a new rule node; each such event invalidates the whole index, prompting the next lookup to re-fingerprint every rule, performed once per batch of such changes. A membership that a rule being written inserts into another rule's collection is not noted: the target rule keeps its old fingerprint, and if typed again, it is treated as a second rule.

**The rule-text variables of `var_in_closure`** (`_rule_text_vars`, located adjacent to the template-variable store below). The store's closure does not enter a container, thus omitting a variable that exists solely within a rule's condition set, or exclusively among the members of a rule's own collection referenced by a membership. `var_in_closure` reads this set beside the store; `collect_variables` does not, because a firing keeps a condition set and performs no substitutions within it. It maintains a single entry for each such rule and for each fact written over one – a mention, a membership, those a firing writes included – hence its size increases with the firings of rules that write such facts, not merely with the rules themselves. It is reset alongside the store, via `.fact-stores off` among other paths.

**Networks saved by an earlier engine.** Every file generated by this engine includes `templateIds` in its header, with the exception of a network file whose load triggered the message below: the older engine's rules preserve their collections across a save, causing the next load to emit the same message again. `.remove-rules` walks the rules again, and once no fragment of the older engine's rule text remains, a save writes the field again. A rule eliminated via another method, such as using `.remove`, results in the field being omitted until a subsequent load of the saved file walks the rules. If a rule is introduced before that save over a collection that was originally constructed as data and holds a variable, the collection assumes the appearance of the older engine's literal, and every future load will produce the message. A full-file `.load` operation on a file missing the field walks the rule texts and the memberships of counter collections encountered therein exactly once, issuing a message when one of those collections holds a membership that the older engine wrote as the rule's literal – specifically, one marked as a rule pattern or holding a variable – and which is not itself a node within a rule's text. The statements a rule writes into a data collection it names are such nodes, so no message appears for that collection provided its other members hold no variable, nor does any message appear for a rule whose collection holds only what its own statement writes into it, as `@{Y}` does in `(X reported Y) => (Y in @{Y})`. A data collection that also holds a rule, or any other member with a variable, takes on the form of a literal and triggers the message, even though it is treated as data by both engines and a rebuild from the scripts makes no difference to it. No re-identification occurs, so each such collection is a value here, either bearing the message or not. For a data collection, a container a rule writes to, and a literal with no substitutions to make – that is what the older engine accomplished; a literal whose members a binding makes ground it rebuilt for each binding, and a firing here refers to that literal, including any variable (see [A literal a rule derives](../concepts.md#a-literal-a-rule-derives)). A partial view produces no output, as it might lack the elements the walk reads. An older engine that loads a file from this one reads its templates and terms as unnamed collections and writes into a rule's own collection again; when reloaded here, what was written becomes part of that rule's text. Therefore, one network should not switch back and forth between the two engines.

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

One asymmetry of the topology has to be undone by hand. `fact()` draws
`F → P` for the predicate and `O → F` for an object, so both the objects of
a node and the facts that _use_ it as their predicate arrive in its incoming
set, on edges that are indistinguishable locally. A node in predicate
position would therefore read back with its own users appended to its
objects. What separates them is the candidate's own reading — a user is a
fact whose predicate is this very node — and that test is only run for nodes
that are relation types to begin with, which no ordinary data node is.

This reconstruction is correct but expensive — O(deg²) on hub neighborhoods
— and the engine consults structures on its hottest paths (unification,
template rejection, grounding, anchoring). The layered lookup below exists
to make the walk the _exception_.

## The Layered Structure Lookup

`get_fact_structures` answers through four layers, cheapest first:

1. **Structureless bit gate (lock-free).** Atoms (sequential IDs) and variables are incapable of decomposing, so `!is_hash(n) || is_var(n)` returns a shared empty list before any lock acquisition or cache lookup – just two bit tests. Soundness: a structure must have a declared relation type among the node's right neighbours; every edge departing a non-hash node is directed to a hash fact node due to the design of `connect()`, and hash nodes enter the relation-type set exclusively via an explicit `(hashnode ~ ->)` declaration, which is not generated by the parser, the stdlib, or any import. This qualifies as an accepted exotic divergence class, backstopped by `.semi-naive check`. The gate is _static_ – it remains valid after binary loading as well, meaning that on a loaded Wikidata graph every Q/P atom answers without touching a lock.
2. **The fact-structure cache (`_fs_cache`).** A promotion cache that maps node → immutable shared structure list (`FactStructurePtr`). A successful lookup incurs one shared-lock pair and one atomic reference count increment – no deep cloning – and the acquired pointer remains valid during invalidations, pointing to a consistent snapshot. All empty results share a single static instance, so the most common lookups along the unify recursion path (negative entries) trigger no allocation.
3. **The genuine-structure store (`_genuine`).** `Zelph::fact()` records the precise triple of every node it creates as a one-element immutable list, at the moment the triple is known and final (foundation facts 1 and 2). Cache misses consult this store before initiating a walk; hits are promoted into the fs_cache. Self-facts maintain `objects == {subject}`, aligning precisely with the walk's self-referential repair mechanism. Since a node's ID pins its triple, entries in the store can never become outdated due to graph _growth_; only topology destruction disarms them (next section).
4. **The reconstruction walk.** The historical semantics, kept verbatim. On a store-armed workload, it serves only hash nodes that `fact()` did not create, such as a set constant; the Jacobian reference workload looks up none, which explains why `genuine walks` reports zero in that context ([Measurement Methodology](measurement.md)). After the store is disarmed, it serves every node – and it now operates swiftly, executing under a single `ReadScope` (below) rather than incurring a lock pair and an adjacency copy for each neighbourhood query.

### Per-node cache invalidation

The fs_cache previously underwent a complete reset on _every_ newly created fact, keeping it near-permanently empty during rule-intensive workloads. `fact()` now calls `invalidate_fact_structures_for`, which erases only those elements that growth can actually influence: the newly introduced relation node and its associated parts, along with a single _bidirectional_ adjacency level surrounding both subject and objects (the neighbourhood examined by the child-fact heuristic). The justification for correctness relies on monotonicity: growth can only _introduce_ new reconstruction candidates, and hash verification prunes any ambiguous set, restoring it to the genuine reading. Two exceptions revert to the full reset behaviour: declarations of relation types (`P ~ ->`), which may alter predicate detection for _any_ entry and also invalidate the memoized relation-type set, and neighbourhoods surpassing a fixed stale budget (hubs). The concept of a budget recurs throughout the engine: any degradation remains sound and is never worse than the prior semantic behaviour. One residual risk is deliberately accepted – entries that fail to verify via hash are not re-checked during subsequent deeper-level growth; however, the suite-wide `.semi-naive check` serves as a safety net.

## The Template-Variable Store

`_template_vars` associates each node generated whose structural closure includes variables with its **exact variable set**, updated in a bottom-up manner by `fact()` using the actual triple arguments. Records are present _only_ when the set is nonempty; hence, while the store is authoritative, **absence means "provably no variables"** – `var_in_closure(n)` requires just one map probe, and `collect_variables` operates in O(1). This criterion distinguishes rule-template nodes from data nodes, and is applied in `extract_bindings` during deep template rejection, in determining anchor eligibility, and in grounding bound patterns. In contrast to the prior reconstruction-driven walk, the store cannot be misled by ambiguous adjacency readings.

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

**Semi-naive evaluation** builds a static index for each execution run (`IndexedRule`): the set of seedable leaf conditions for every rule, a mapping from predicate to (rule, leaf) index, a list of wildcards applicable to variable-predicate leaves, and – extracted from the `Unification` constructor – the rule-static **pattern decomposition** (`PatternInfo`: relation, subject, objects, subject-predicate hint), which relies exclusively on the immutable condition node and is shared across all seeds. Work contingent upon binding (relation-variable resolution, grounding, boundness analysis, anchoring, snapshot initiation) remains per-instance. The delta is monitored through the fact-creation observer, and a seeded `Unification` maintains a candidate set containing precisely one fact. Rules whose seeding cannot be established as exhaustive (nested conjunction elements, ambiguous predicates, neural conditions, transitive path conditions, rules lacking a seedable condition) are marked _delta-unsafe_ and execute in the classic manner during every iteration; rules involving negation form deferred strata, excluded from the first classic pass and run classically at each negation level. The `check` mode appends classic verification passes and names any fact overlooked by the delta path – the completeness net used to assess all prior components.

**Join ordering** (`optimize_order`) carries a connectivity term: a
condition sharing _no_ variable with the current bindings starts an
unconstrained cross-product scan and must lose against every connected
condition, whatever the cardinalities. Variables at any structural depth
count as connecting — the decisive case is a bound variable sitting inside
a nested pattern, invisible to subject/object boundness scores. Guard
conditions (`!=`, neural, negation) are pushed last via tier penalties that
dominate the connectivity term.

## Smaller Fast Paths

A handful of performance-critical rewrites merit awareness before modifying their call sites. `check_fact` examines every edge membership associated with the precise triple within a single lock scope on references (`Network::fact_edges_hold`) and nothing beyond: a node existing without the subject and object edges of the triple remains unrecognized by it, and `fact()` is where such a scenario is refused ([The Identity Foundation](#the-identity-foundation)). `create_hash` bypasses the copy+sort normalization for an object set whenever the set’s storage mode inherently traverses in ascending order (small sets – nearly all of them); large unordered storage keeps the normalization to ensure the hash stays a pure function of the element _set_. `parse_relation` filters right neighbours via the memoized relation-type set before executing the exact probe; its `ReadScope` variant `parse_relation_scoped` uses membership (is-known) semantics, which are documented as strictly equivalent during reconstruction. Finally, output streams route their flush operations through the print mutex (`locked_stream`) – a correctness contract, not a performance enhancement: pool workers emit logs simultaneously, and an unserialized stateful output handler introduces a data race.

## What a Save Costs

Everywhere else on this page the budget is time. For `.save` it is memory,
because the operation that matters — writing a network freshly imported from
a Wikidata dump — runs on a machine that is already deep into swap.

A saved network is a stream of Cap'n Proto messages: a small header, then
the adjacency and name maps in chunks of `chunk_entries` (1M) each. Cap'n
Proto allocates the **first segment of a message up front**, so the size
handed to `MallocMessageBuilder` is a floor on what the write needs
resident, not a hint — and under mimalloc, which the zelph binary links, the
pages are there immediately. A flat first segment of 64 Mi words therefore
cost 512 MiB per message regardless of content: measured, saving an 11 kB
network moved process RSS from 0.0 to 0.5 GiB, and the same half gigabyte
was charged on top of every large save.

`serialization_layout.hpp` sizes it from the entry count of the chunk being
written instead — eight words per entry, which covers a node plus a list
header plus a typical adjacency, or a key plus a short label. Where the
estimate falls short Cap'n Proto **appends** a segment rather than copying,
each new one as large as all previous together, so a miss costs at most a
factor of two in memory and never a `memcpy`. It does grow the file a little
(a segment table entry per segment, and cross-segment references become far
pointers), which is why the estimate is generous enough to keep the common
chunk in one segment.

On a 2.6M-node network (0.7 GiB resident, 169 MB on disk) this takes the
peak RSS of the save from 1.22 GiB to 0.84 GiB at unchanged wall time, and
the file comes out byte-identical. No off switch accompanies this: unlike
the acceleration stores, nothing here trades memory for speed — the segment
is simply as large as the data going into it.

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
[`serialization_layout.hpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/serialization_layout.hpp)
(chunk size and first-segment sizing, i.e. what a save costs);
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
