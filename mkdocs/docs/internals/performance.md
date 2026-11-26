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

Fact 3 does not apply in reverse: the identifier is a 62-bit hash of the data, implying that two separate structures share the same ID if their hashes coincide in a collision. For n structured nodes, the birthday bound predicts the expected number of colliding pairs to be about n²/2⁶³: roughly 10⁻⁷ when dealing with a million nodes, no more than approximately 0.0014 in the [`-medium` Wikidata artefact](../binaries.md) containing 1.1·10⁸ nodes, and ranging from 0.1 to 0.3 for a complete import of 10⁹ to 1.6·10⁹ triples. The system refrains from comparing the actual structures, so while a collision is detected, it is not resolved. The `fact()` function detects an ID already assigned to a node that does not hold the new statement and rejects the statement, naming both. During inference, a derived statement rejected in this manner is marked as a contradiction, including the cause, just as a derived statement is if one of its nested components is rejected; the run continues. In bulk import, such a triple is left unwritten; the system reports the first occurrence during import and delivers the overall count once the process finishes. Either way, the second statement is lost. The `fact()` function determines "does not hold" by checking the subject and every object against the node's edges; regarding the predicate, it only examines the weight of the node's edge to it, which defaults to 0 if no such edge exists. A colliding statement sharing the same subject, where all its objects are part of the node's own set, is thus deemed known. If the predicate matches the node's own, or is any other node the node has an edge to – such as its subject – it goes undetected. For any other predicate, it is treated as known to be false. The `fact()` function then rejects it with the message "this fact is known to be wrong" instead of referencing the collision; during inference, a derived statement becomes a contradiction without an explicit reason stated, and one whose nested component is rejected becomes a contradiction with this refusal as its reason. The bulk import also performs a lookup on the predicate. Collections that rules construct possess their own identifiers, whose collisions are entirely overlooked; refer to the following section.

## Rule-Built Collections: The Id Is the Recipe

This section stands apart on this page: the subject it describes is semantics, not acceleration. It rests on the identity foundation, though, and it decides what several readers below may assume regarding a node id.

A collection is the sole type of node for which the identifier is not its content – writing `@{a b}` on two separate occasions yields two distinct containers. When a rule builds a collection, the identifier is determined through computation instead of enumeration, and the higher-order bits indicate the origin of the collection:

| Bits 63..60 | Class | Built by |
| --- | --- | --- |
| `0000`, below 2⁵⁸ | counter | `create()`: atoms, witnesses, the core nodes, the conjunction sets of typed rules, each collection written as data |
| `0000`, bits 59 and 58 set to `01` | blank node | `Zelph::blank`: the 58 bits below are derived from a hash of the document’s identity and the label, or the ordinal, and not from any counter ([Blank Nodes](../rdf-spec/blank-nodes.md#ids)) |
| `0001` | set pattern | `set()` invoked beyond the template scope, for a set that holds a variable (`create_value_set_pattern`) |
| `0010` | value recipe | a firing: the term it builds for a collection of the rule’s text |
| `0011`, bit 59 clear | written template | a collection written during rule writing: the parser, `zelph/build-rule`, `zelph/build-rule-parts` (the `zelph/rule` macro), `zelph/rule-text` |
| `0011`, bit 59 set | construction’s recipe | a construction: the collections of a rule that a generator writes |
| `01xx` | hash | facts and set constants |
| `1xxx` | variable | `var()` |

**Authorship constitutes the id class.** A collection is part of a rule's text – it is a _template_ – exactly when its id resides within the template class, making `Zelph::is_rule_template` one bit test, and nothing written later (a claim, a mark, a name, a membership) changes the result. Every other collection is a value, retained uniformly across all instantiations. The sole exception involves a conjunction set: although it is rule text, it is distinguished by its `~ conjunction` tag, not by its id – a typed rule's has a counter id. A firing keeps it, except where it writes a membership into it, which goes to the set's data term; a construction rebuilds it, except where a variable bound to the set occupies the position where a membership is written (refer to _A rule's text is fixed once the rule is written_ below). A recipe is not a hash, so every gate interpreting "not a hash" as "an atom or a collection" reads a recipe as a collection, and neither a fact nor a set constant can land on one.

**The key.** A firing builds `recipe_id(T, key)` for a template `T`, where `key` hashes the binding of variables that the instantiated statement meets (the consequence in a firing, the written rule in a construction), represented as sorted (variable, value) pairs; any variable left unbound by the binding does not appear in it. The collection that a consequence writes to uses the empty key, ensuring all firings of the rule target a single term (`Zelph::bucket_term_id`). `container_plan` (`reasoning.cpp`) is the sole location determining how an instantiation handles a container. The key walk (`statement_variables`) and the prediction of the existential check follow it, meaning the check looks for the node that the firing builds.

**Reuse never compares members.** `Zelph::recipe_collection` creates the node upon detecting a non-existent ID and asserts every missing membership. This mechanism enables a subsequent firing with the same binding to rediscover the collection – explaining why a collision between two recipes remains undetected: the two share one collection. For n terms, approximately n²/2⁶¹ pairs are expected to collide (4·10⁻⁷ at a million rule-built collections, 4·10⁻³ at 10⁸), while among the collections of constructions, n²/2⁶⁰. A collision involving a fact, a set constant, a counter, or the alternate class is impossible by range. During a firing, `recipe_collection` also claims a membership that a rule has specified as a pattern, as a firing claims every fact it derives.

**The counter is protected by a guard.** IDs from 2⁵⁸ onward belong to the other classes, with the closest being blank nodes, thus `Network::create` rejects a request once the counter reaches 2⁵⁸, as do `create_written_template` and `create_value_set_pattern`, which use the next counter value as payload (`0x3000000000000000 | counter` and `0x1000000000000000 | counter`). The range from 2⁵⁹ to 2⁶⁰ remains unused; the [RDF term mapping](../rdf-spec/storage.md#id-classes) keeps it for literal value nodes. Node creation does not reach the threshold in practical scenarios, so its test loads a network whose header holds the counter at that value. `.save` and `.load` keep the counter state, ensuring that a template written after a load gets a fresh payload.

**A construction executes within a scratch cluster** (`ConstructionScratch`, `reasoning_deduce.cpp`). It builds the parts of the rule a generator writes, and keeps them, merged into the active cluster, precisely when the rule governing them is the one it returns. If a rule currently in force already says the identical assertion, the construction returns that rule – the _claim on creation_ – and discards any constructed elements, regardless of exit path, including upon exception throw. This mechanism also applies when a generator settles after a name merge: its next construction under the merged binding claims the rule reconstructed by the merge and leaves no residual artefacts. The rule a construction returns, whether claimed or kept, is remembered (`_claims`: statement and key → rule, one entry per unique construction), ensuring subsequent passes refrain from rebuilding the components; the record is cleared each time the fingerprint index becomes outdated.

**A rule's text is fixed once the rule is written.** A rule governing rules can bind a collection of another rule's text – via rule structure, as in `(G => (S q C)) => ((X r Y) => (X in C))`, which binds the collection of a ground rule, or through an engine marking fact – and a membership it states there would change what that rule says. The variable represents the collection's _data term_ instead (`Zelph::bucket_term_id`), the term a firing of a ground rule writes for that collection: a rule a construction writes names it wherever the collection appears within it, and a firing writes into it wherever it writes a membership. A conjunction set stands for its data term only where a membership is written into it, and a condition `X in C` that a construction writes in the rule is precisely such a membership: the condition is a membership fact, which, if included in the set, would make X a condition of the bound rule. A firing that writes into a conjunction set the rule holds by itself – where the relation was a variable at the time the rule was written, as in `(X R C)` – also writes into the set's data term. The term comes into existence at the first statement that names it, and remains empty until the rule whose collection it represents fires: the members of the rule's literal, and the one its statement writes, arrive with that firing. Three configurations arise depending on where substitution happens and are documented rather than changed: the data term of a collection whose rule contains variables is the term under no binding, which none of that rule's firings ever writes to, so a rule over the engine's marking facts that binds such a collection writes into a term the rule never encounters; a rule re-expressed from its bound parts, `(G => (S q C)) => (G => (S q C))`, is a second rule, over the term, beside the first; and when placed under a switch, that restated rule leaves the typed rule active. A membership written into a rule's own collection or conjunction set while no rule is being written – through Janet, the C ABI, or a name given to the collection – is refused (`Zelph::fact`), also when the collection already contains the member. Within the scope in which a rule is being written – the parser's, `zelph/build-rule`'s, that of `zelph/build-rule-parts`, which the `zelph/rule` macro expands into, `zelph/rule-text`'s, and a construction's – such an insertion is the means by which the rule's text is formed. The scope is open only on the thread that opened it, and only on its network: a collection created by another thread meanwhile is considered data. Within the scope, a rule currently being written can still write into another rule's collection, when a Janet program holds the node of that collection; in such a case, the other rule's text is modified.

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
3. **The genuine-structure store (`_genuine`).** `Zelph::fact()` records the precise triple of every node it creates as a one-element immutable list, at the moment the triple is known and final (foundation facts 1 and 2). Cache misses consult this store before initiating a walk; hits are promoted into the fs_cache. Self-facts maintain `objects == {subject}`, aligning precisely with the walk's self-referential repair mechanism. Since a node's ID pins its triple, entries in the store can never become outdated due to graph _growth_; only topology destruction disarms them (next section). A node written without `fact()` – read during a load, written via a trusted import, rebuilt through a name merge – lacks an entry, and a cache miss on it initiates the walk. A walk that retrieves a single reading which hashes back to the node has read the node's own triple, the one `fact()` would have recorded, and the store keeps it, meaning such a node is walked only once rather than at every cache miss: over a math stdlib loaded from a file, the Jacobian workload walks 2,800 structures instead of 178,010, and runs 7.5 G user instructions instead of 11.0 G (7.4 G in a fresh session). The store does not keep a walked reading while the template-variable store is incomplete (below), where an entry here also says that the stores recorded the node's variables, nor during a bulk pass that suspends the structure cache, which reads each node once.
4. **The reconstruction walk.** The historical semantics, kept verbatim. On a store-armed workload, it serves only hash nodes that `fact()` did not create, such as a set constant or a node a load, a trusted import or a name merge wrote; the Jacobian reference workload looks up none, which accounts for why `genuine walks` reports zero in that context ([Measurement Methodology](measurement.md)), even following a `.load`, an import, or a merge of something the workload does not read. After the store is disarmed, it serves every node – and it now operates swiftly, executing under a single `ReadScope` (below) rather than incurring a lock pair and an adjacency copy for each neighbourhood query.

### Per-node cache invalidation

The fs_cache previously underwent a complete reset on _every_ newly created fact, keeping it near-permanently empty during rule-intensive workloads. `fact()` now calls `invalidate_fact_structures_for`, which erases only those elements that growth can actually influence: the newly introduced relation node and its associated parts, along with a single _bidirectional_ adjacency level surrounding both subject and objects (the neighbourhood examined by the child-fact heuristic). The justification for correctness relies on monotonicity: growth can only _introduce_ new reconstruction candidates, and hash verification prunes any ambiguous set, restoring it to the genuine reading. Two exceptions revert to the full reset behaviour: declarations of relation types (`P ~ ->`), which may alter predicate detection for _any_ entry and also invalidate the memoized relation-type set, and neighbourhoods surpassing a fixed stale budget (hubs). The concept of a budget recurs throughout the engine: any degradation remains sound and is never worse than the prior semantic behaviour. One residual risk is deliberately accepted – entries that fail to verify via hash are not re-checked during subsequent deeper-level growth; however, the suite-wide `.semi-naive check` serves as a safety net.

## The Template-Variable Store

`_template_vars` associates each node generated whose structural closure includes variables with its **exact variable set**, updated in a bottom-up manner by `fact()` using the actual triple arguments. Records are present _only_ when the set is nonempty; hence, while the store is authoritative, **absence means "provably no variables"** – `var_in_closure(n)` requires just one map probe, and `collect_variables` operates in O(1). This criterion distinguishes rule-template nodes from data nodes, and is applied in `extract_bindings` during deep template rejection, in determining anchor eligibility, and in grounding bound patterns. In contrast to the prior reconstruction-driven walk, the store cannot be misled by ambiguous adjacency readings.

Absence can mean "no variables" solely when the store holds an entry for _every_ node that has variables, so every path which writes a hash node without `fact()` keeps it complete:

- **A trusted import** enters each triple it writes as `fact()` would, based on what its three components hold. A data triple holds nothing and costs a few bit tests.
- **A name merge** rebuilds every node constructed upon the vanished one under a fresh identifier, and each rebuilt node inherits the entries of the node it supersedes. Neither of the merged nodes holds a variable – a statement or a set constant holding one is rejected, as is a variable paired with anything other than a variable – thus the rebuilt statement holds exactly the variables of the old one. A merge of two variables alters the name of a variable within nodes that keep their identifier, and a merge involving a set pattern built outside a rule changes the variables of what is rebuilt over it: both disarm the stores (next section).
- **`.save` writes the store into the file** (`variableRecord` in the header: each node with its variables, and the `_rule_text_vars` entries), and **a load enters it**, for every chunk selection of the file. A network of data records nothing; the record grows with rule text, as the store does. The same header says whether the network may hold a rule’s own collection or a set pattern constructed externally to a rule (`patternKinds`), which `may_hold_set_patterns` and the components `fact()` reads need; a file lacking that field has them read from the ids the load read, a few dozen instructions per node.

A file lacking the record also loads successfully. When it is missing `patternKinds` as well, as is the case with every file of an engine without the record, and the load has read the whole file and none of its nodes is a variable, no node within it holds one, and the store is complete even though it is empty: the pass over the node IDs, which such a file gets for `patternKinds` in any case, resolves that question en route, and every network of data loads in this manner. In any other case the loaded nodes did not undergo a `fact()` call and the store holds none of them, so absence applies only to a node that the genuine-structure store holds, which is every fact written since the load: any other hash node is read during the walk. That test costs one probe of the genuine store per store miss, roughly 1 % of the instructions in a workload that touches no loaded node, and the walks incur higher cost when reading loaded rules: the Jacobian workload over a math stdlib loaded from a file without the record runs 13.8 G user instructions, compared to 7.5 G from the same network with the record. A session stays in this state until the next load that leaves the store complete, until an action disarms the stores, or until `.new`. A save writes the entries of the store solely when the store is complete; a session that loaded a file without the record saves one without it too, and so does a session whose stores are off. The exception applies to a file where no node is a variable: it gets the record, without an entry, regardless of the store’s state, ensuring that a network of data saved after a removal loads with the store complete. A view of a file is considered whole in neither case, because its nodes may name nodes it does not hold.

The readers that _enumerate_ the store instead of probing it – `unasserted_snapshot`, behind `.list-predicate-usage`, `.list-predicate-value-usage`, `get_fact_objects` and `get_fact_subjects`, the transitive closures, `.explain` and `.save-predicates` – rely on its completeness: a node that holds a variable without an entry is treated as a claim there. Following a load of a file lacking the record, the loaded rule patterns are considered data for these readers, and once the stores are disarmed, every pattern is.

## Authoritative Bits and the Disarm Funnel

Each store carries a state indicating whether it is _authoritative_, with a
one-way discipline: **disarmed stores are never re-armed**. Absence of an entry is meaningful
while a store is authoritative, and no retroactive scan could soundly
recreate that property (`.new` re-arms by creating a fresh engine). The
disarm funnel consists of the single shared implementation
`disable_fact_stores()`, accessed via `invalidate_fact_structures_cache` –
node deletions, merges of two variables or with a set pattern built outside a
rule, meaning the paths that destroy topology or change which variables a
statement holds – and through the explicit `.fact-stores off` command. Growth-only full clears (relation-type declarations, stale-budget
degradation) deliberately do _not_ touch the stores: they are growth-immune
by the identity foundation.

A load, a trusted import, and any other name merge keep the stores armed.
They write nodes without using `fact()`, which the genuine-structure store
simply does not hold, and they keep the template-variable store complete
wherever the previous section says they can. A load replaces the graph, thus initially
discarding what both stores retained regarding the prior graph. Consequently,
the variable store resides in one of three states, one word that readers on
the hot path load once: complete, complete exclusively for the nodes the
genuine store holds (following a load of a file lacking the record, unless
it read the whole of a network without a variable), or disarmed. The checks
for the second state are out of line, ensuring that a session in the first
state incurs no cost for them.

The compromise managed by the switch concerns memory: roughly 150 bytes per
node created via `fact()`, even following a `.load`. A trusted import adds no
entry to the genuine store, and only into the variable store when a triple
holds a variable, thus keeping bulk imports neutral; the variable store grows
with rule text, never with data. Every `fact()` write occurring after a load
is recorded, however, likewise every fact a derivation writes, and every
loaded, imported, or rebuilt node whose structure a walk reads is retained at
roughly 70 bytes (a query over 200,000 loaded facts raised the resident size
by 12 to 14 MB more than when the stores were off): before bulk derivation on
a large loaded graph, `.fact-stores off` frees the stores and prevents their
growth, at the price of the walks and of the readers that enumerate the
store, which thereafter treat patterns as data. Rules typed onto a loaded billion-node
graph function normally either way, and the walk path itself is quicker than
it was before this project.

## ReadScope: One Lock Pair per Read Region

`Network::ReadScope` acquires both adjacency locks in shared mode (left
before right — the writer order of `connect()`) and hands out _references_
into the node map for its lifetime, replacing sequences of `get_right`/`get_left`
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
This type of anchor reads the node's whole adjacency, even when the predicate
selects only a few of its facts, and `.prof` counts these entries as
`anchors: walked`. A condition with a fixed relation and all concrete subject
and objects is therefore answered by a single **exact-triple probe**
(`check_fact`, counted as `exact_probes`) instead: read from its subject, the
digit test `A isdigit true` from the stdlib's comparison rules walked every
list cell containing the digit, so that each comparison grew with the numbers
present in the graph. When both ends are concrete and no probe is applicable,
the anchor is the end with the smaller adjacency.
**Bound-pattern grounding** resolves a fully bound structured pattern to the
single node it denotes via pure hash lookups — the fact that contains
precisely the pattern’s objects — and can fail a condition outright when
the denoted fact is missing. **Partial-pattern anchoring** handles the
partially bound case: any concrete node inside the pattern must appear _identically_ in
every matching graph fact (foundation fact 3), so climbing the adjacency
levels from the lowest-degree anchor, filtered by the pattern's predicate
chain, yields a complete candidate superset. A concrete statement stands
apart from foundation fact 3: a fact carrying more objects states it as
well (refer to [Facts with Multiple Objects](../logic.md#facts-with-multiple-objects)).
If such a fact could exist for the statement’s predicate, or for the predicate
of a statement contained within it, neither grounding, nor an anchor, nor the
probe considers the statement the sole node matching it, and the climb starts
at its atoms. The engine keeps the predicates for which such a fact could
exist (`Zelph::may_hold_wider_statements`): `fact()` marks the predicate of a
ground fact having multiple objects, a name merge applies the mark solely when
combining the objects from two statements, and a merge passes the mark from
each node it relocates to the node that takes its place. The record travels in
the `.bin` header; a file from an older
engine, which carries none, is interpreted as marking every predicate, and
`.stat` indicates how many predicates might have such a fact. This approach
entails a cost: `.explain` of the documentation’s polynomial identity performs
approximately tenfold more work when every predicate counts, surpassing the
built-in budget. Whether a statement holds a marked predicate at any depth is
decided by a walk across the statements inside it, which conditions use based
on the values they are bound to once any predicate is marked: one fact with
two objects whose predicate is not held by any term causes the Jacobian
example to execute roughly a tenth more instructions, even though every
condition is read as if it were without it. When a fact with more objects
could match it, a concrete statement serving as the predicate in a condition
is interpreted as a pattern there is: the candidates are the relations sharing
its own predicate, not the statement in isolation. Predicate positions never
qualify as anchors — a fact points _to_ its predicate, so the predicate's
incoming side is the full extent, exactly the scan being avoided. All
anchoring is budgeted, and an exceeded budget falls back to the full scan,
never to a truncated candidate set: soundness is unconditional (candidates
still pass structural unification) and completeness is budget-independent.
Turning off anchors with `.anchors off` reverts to the anchor-free naive
reference, operating independently of `.parallel`. It also disables the
subject-predicate hint, which normally bypasses a candidate if its
statement subject does not include the predicate found in the pattern’s
subject, so tests use it as an independent completeness check for both
anchor lookups and that hint.

**Semi-naive evaluation** builds a static index for each execution run (`IndexedRule`): the set of seedable leaf conditions for every rule, a mapping from predicate to (rule, leaf) index, a list of wildcards applicable to variable-predicate leaves, and – extracted from the `Unification` constructor – the rule-static **pattern decomposition** (`PatternInfo`: relation, subject, objects, and the subject-predicate hint unless that predicate holds a variable), which relies exclusively on the immutable condition node and is shared across all seeds. Work contingent upon binding (relation-variable resolution, grounding, boundness analysis, anchoring, snapshot initiation) remains per-instance. The delta is monitored through the fact-creation observer, and a seeded `Unification` maintains a candidate set containing precisely one fact. Rules whose seeding cannot be established as exhaustive (ambiguous predicates, neural conditions, a path condition applied to a predicate variable, rules lacking a seedable condition) are marked _delta-unsafe_ and execute in the classic manner during every iteration; a rule with a transitive path condition is seeded via its other conditions and runs classically in an iteration where the delta holds a fact of the predicate it walks; rules involving negation form deferred strata, excluded from the first classic pass and run classically at each negation level – except during a seeded run (`.run-delta`), which seeds them at that level using facts the level has not yet seen, unless a predicate they negate has lost a fact since the last run, they negate under a predicate variable, or their component negates what it derives. A seeded run gives a rule that is newly introduced since the last run one classic pass in the run’s first iteration. The `check` mode appends classic verification passes and names any fact overlooked by the delta path – the completeness net used to assess all prior components.

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

A saved network is a stream of Cap’n Proto messages: a small
header, the chunk directory, followed by the adjacency and name maps
organized into chunks of `chunk_entries` (1M) each. Cap'n Proto allocates
the **first segment of a message up front**, so the size handed to
`MallocMessageBuilder` is a floor on what the write needs resident, not a
hint — and under mimalloc, which the zelph binary links, the pages are
there immediately. A flat first segment of 64 Mi words therefore
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

The chunk directory incurs a cost of three words per chunk. Being unpacked, its size remains unaffected by the content it records: the save operation writes it with zeros before the chunks, records each chunk’s offset, length, and entries during chunk writing, and later overwrites the zeros with the directory at the end.

## Reading the Code

The map, in dependency order:
[`network.hpp`](https://github.com/acrion/zelph/blob/main/src/lib/network/network.hpp)
(the node map, `connect`, hashing, `ReadScope`, `fact_edges_hold`,
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
