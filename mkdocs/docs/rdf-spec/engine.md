# Engine Requirements

The mapping is built into an engine whose first application is mathematics: rules that derive arithmetic, polynomial identities, and the refutation of a conjecture, on workloads whose cost is measured down to the instruction. This page states what the mapping may presuppose about that engine, what its implementation must preserve unchanged, and through which tests its completion is checked. The requirements are specified; the section detailing what the engine provides describes today’s engine.

## What the Engine Provides

A number of properties upon which the mapping depends were developed ahead of it. They are part of zelph today.

- **A fully bound condition incurs a single probe.** A condition in which the subject, predicate, and all objects are bound is answered by one lookup of the corresponding statement, and a condition with a single bound end is matched from the bound node with the smaller neighbourhood ([Performance Architecture](../internals/performance.md#candidate-sets-anchoring-and-semi-naive-seeding)). This is significant for the mapping since each expanded number introduces cells attached to the few digit nodes: previously, a comparison by rules traversed the entire neighbourhood of a digit for each test, causing its cost to rise with every numeral present in the graph.
- **Wider statements are recorded per predicate.** A statement possessing multiple objects increases the expense of matching patterns over its predicate, and the engine maintains a record of which predicates may hold such statements instead of assuming this universally. This record is set by a statement with two or more objects and by the merge of two statements into one, and it is stored within the `.bin`. Merging an atom into a term, as the specified [list conversion](collections.md#what-the-conversion-does) will do, leaves no mark.
- **Comparison as a condition.** `A zelph:lt B` and its five counterparts are answered by the engine, order every pair of numbers by value, derive nothing, and are held to the rules of `cmp` through a property test ([Numeric Comparisons](../logic.md#numeric-comparisons)).
- **One node map.** Both sides of a node’s adjacency reside within a single entry of one map. The map’s bucket array undergoes its final doubling at 1,717,986,945 nodes, which adds 16 GiB simultaneously (computed, [Costs](costs.md#the-bucket-step-and-the-cap)); `.node-map-load-factor` adjusts this threshold, and a load reserves precisely the amount announced by the chunk directory of a file ([Binary Data Files](../binaries.md)).
- **The fast paths survive loads and merges.** The stores enabling the engine to read a statement’s parts without walking the graph remain armed through a load, an import, and the merging of two names, and the store of variables is saved with the network ([Performance Architecture](../internals/performance.md#authoritative-bits-and-the-disarm-funnel)).
- **Threads.** The number of worker threads aligns with the number of processors available to the process, `.threads` and `ZELPH_THREADS` define this value, and the outcomes of a parallel scan are merged in a fixed sequence, ensuring that two executions of the same input produce identical output.

## A Session Without RDF Stays as It Is

The fundamental requirement: **a session that uses no feature of the mapping performs precisely the same operations it performs today.** It is validated against the Jacobian workload, the established benchmark for zelph’s measurements ([Measurement Methodology](../internals/measurement.md#the-reference-protocol)); the criterion is located at the end of this section. The rules that make it hold:

1. **No new node.** A session that uses no RDF feature generates no node beyond those it creates today. The vocabulary of the mapping (`zelph:value`, `zelph:lang`, the requests, the special values, the constructors `dec`, `interval` and `at`) is created upon first use, just as the engine’s vocabulary is created today. Registrations of expanders, name readers, displays, order keys, namespace rows, and the constructors’ display are keyed by text and generate no node; today a display registration by name creates the node it names, and this behaviour is modified. A node created during start-up or during the import of the mathematics modules would renumber every node following it, and the work of a run depends on the numbering.
2. **A counter of their own for literals.** Sequence numbers for value nodes never originate from the node counter; blank nodes do not use any counter; the substrate record is an annotation and not a node ([Storage](storage.md#id-classes)).
3. **Atoms to every existing test.** Value nodes and blank nodes lie in the counter class, so no existing class test changes its result, and nothing new enters the functions that unify nodes or read the structure of a fact.
4. **Tests on hot paths are cheap by construction.** Whatever the mapping adds to the evaluation of a condition, to the ordering of a plan, to the creation of a fact or a node, is a test of an id class or of a cached id or flag that remains zero while the feature is unused: never a name lookup, a string hash, or a lock.
5. **Engine-answered conditions are recognized by cached ids** behind one flag, per run and per query. What they bind is created on the thread that evaluates the rule, never by a worker of the parallel scan.
6. **The request pass returns after one test** while the graph holds no request. With requests, it reads its own record of statements added for the requested predicates, filled through the single hook the engine has for new facts and only while a request exists; after a bulk import, which bypasses that hook, and after the record overflowed, it scans the requested predicates once.
7. **One object per fact.** Every route writes one RDF triple as one fact with one object, and every request and expansion statement has one object, so the mapping does not mark any predicate as holding wider statements.
8. **Core spellings first, without a cost per lookup.** The check whether a token is a core spelling is performed behind a test of its length or first character; no lookup pays a string hash for it.
9. **Fallbacks only on a miss.** A fallback chain is invoked solely when the session language lacks the name.
10. **Refusals where nodes are created.** That a value node takes no name and no merge, and that it and a blank node are not predicates, is a single class test on the branch that would create the name, the merge, or the fact.
11. **The value store sleeps.** Store and content index are not touched while the graph holds no literal.
12. **The grammar pays one character.** The new forms are attempted only behind a test of the first character of a token. A constant of digits behind `&` produces exactly today’s call, and the scanners that look for islands of other syntax within a line skip only spans that are IRIs, so that a list containing such an island keeps it.
13. **Opt-in by module.** The `turtle` keyword, the bridge from `rdf:type` to `~`, the interval relations, and the rules of the checks come from modules that the mathematics does not import.
14. **Substrates.** Refusing a second substrate keeps today’s quiet skip of the default import inside a module, on which every non-binary mathematics session relies. Negative zero adds no rule to the mathematics modules.

**The criterion.** For each alteration that realizes a part of the mapping, relative to the engine before the change: the counters from the sequential Jacobian run match exactly, counter by counter ([Identities and Invariants](../internals/measurement.md#identities-and-invariants)), and its user-space instructions increase by no more than 0.05 %. The performance guard passes. Two guard cases are added: a session that imports a small RDF document containing an explicit list of `rdf:first` and `rdf:rest` statements and subsequently executes the Jacobian workload, which must do the same work as the plain session; and a pin of the sequential counters extracted directly from the build itself.

## List Conversion

The [conversion of collections](collections.md) replaces the head of a chain with its list via the mechanism that merges two nodes. Today’s merge already performs most of the operations required by the conversion: when an atom becomes a term, it marks no predicate, and it leaves the fact-path stores armed. What it continues to execute in every single merge is excessive for a step that might substitute thousands of heads:

- it clears the predicate index and switches the index’s sidecar files off until the subsequent full load;
- it drops the memo of relation types and rebuilds the indexes for rule patterns, refuted facts, and opaque predicates.

The conversion therefore uses an entry of its own. It needs no name and does not resolve any name conflict; it keeps the check that a node is not part of the term it becomes; it rebuilds the dependent statements in the manner a merge does; and it performs invalidations once per step, and only those that a rebuilt node requires. The statements it rebuilds are recorded for the next incremental run, which an ordinary merge does not perform today.

Below the scale of a dump, readers write facts via the standard creation process; the bulk path is designated for dumps. Today, both refuse a triple where the predicate equals its object: the standard creation process does so with an error, the bulk path by leaving the triple unwritten while the import continues, reporting the first such triple encountered during an import and their total count upon completion. Every route in the mapping verifies this condition before writing ([Statements](iris.md#statements)).

## The Reasoner

- **The request pass is scheduled with the rules.** It runs at the end of the positive rules at a level and before each level of negation, as a rule would whose consequences correspond to the predicates declared by its expanders ([When Requests Are Answered](expansion.md#when-requests-are-answered)).
- **Names reach the incremental run.** A name modification, via `.name`, `.delname`, or a label import, is logged in the record of what is new, and a rule with a condition that is answered from the names is fully evaluated in a run where the record holds such an entry. Today a name change reaches no run.
- **A bulk import drops the record of what is new**, thereby ensuring the next incremental run does not rely on a record that the import bypassed.
- **Provisional values.** What a condition answered by the engine provides is bound by content and becomes a node only when a consequence writes it ([Querying Terms](querying.md#conditions-the-engine-answers)).

## The Parser

- The new forms, the code that turns them into calls, and every walker of the syntax tree are introduced simultaneously; a kind of term that the emitter lacks knowledge of triggers an error and is never written as `nil`, which is what an unknown kind silently becomes today.
- The message of a syntax error names the text that failed.
- `_:` leaves every test for a variable.
- The commands that take a node as an argument (`.node`, `.name`, and the rest) gain understanding of the three forms and the `&` constants.
- A text enclosed in quotation marks is transmitted with its length across every layer ([Storage](storage.md#programming-interfaces)).

## Tests

The implementation is complete when these are satisfied, in addition to the existing suite:

| Test | Holds |
|---|---|
| display, then read | across the W3C suites and a fixed sample of Wikidata: every term printed according to the [guaranteed forms](notation.md#the-re-read-guarantee) is re-read as the identical node, under two session languages |
| order keys and comparison against `cmp` | the existing property test, extended to include the order keys of SPARQL and `dec` terms, under both decimal and binary |
| list conversion | zelph’s unique corner cases expressed as scripts of `_:label` statements and `.lists`; identical graphs with the shortcut enabled and disabled |
| namespace table | for each namespace in the table: mapping a full IRI yields the same node that the split form gives |
| constants | `&-0` and `&0.0` are `&0`; `(neg zint &0)` prints in full; display then read under decimal, binary, and the NAND substrate; `&1\x2e5` is an error |
| value store | a lexical form with U+0000 through script, Janet, C, Rust, save and load; the content index after a load; a partial view excluding the section |
| blank nodes | fixed inputs and their identifiers, ensuring the derivation cannot shift without notice |
| names | core spellings first in every language; chains; the label rules, independent of input order; a regenerated fixture for `mul` |
| requests | a pass per level of negation; a renamed node re-read; requests pending after a load and answered after the import |
| substrate | a load under a different substrate withdraws the reader; nested default imports served by the record |
| `turtle` block | `.end`, an open long string, parameters, all or nothing |
| Wikidata routes | the differential test over pairs of JSON and RDF exports of pinned revisions ([Wikidata](wikidata-mapping.md)) |
| conformance | the W3C suites with the accounting of triples ([Conformance](conformance.md)) |
| the criterion above | on every change |
