# Profile and Conformance

This page defines the RDF that the mapping covers, its profile, and the method by which zelph’s readers are evaluated against the W3C’s test suites. None of this is implemented yet: zelph lacks an RDF reader and a conformance runner today, and all content on this page is specified behaviour, written in the present tense; a sentence about the present engine says "today". The status of the RDF 1.2 documents was read on w3.org on 23 November 2026, and every test count was recorded on that day, based on the two commits pinned below.

## The Profile

The mapping covers **RDF 1.1, the W3C Recommendations dated 25 February 2014**: Concepts and Abstract Syntax, Semantics, Schema, N-Triples, N-Quads, Turtle, and TriG ([D8](decisions.md#d8)), amended by the two technical errata from the RDF 1.1 errata page that pertain to document reading. Erratum 24 explicitly defines white space and comments within the N-Triples grammar; erratum 30 removes `:` from blank node labels in N-Triples and N-Quads (production `[158s] PN_CHARS_U`), as Turtle and TriG have consistently excluded it. The errata page lists both as fixed in RDF 1.2. The remaining technical errata concern canonical N-Triples and N-Quads (32, 33), which only a writer produces, and dataset isomorphism (27), which the judgment below does not rely on.

The fifth syntax is **JSON-LD 1.1, the W3C Recommendations issued on 16 July 2020**, read through the algorithm Deserialize JSON-LD to RDF, referred to as toRdf for brevity, using `processingMode` `json-ld-1.1`, `rdfDirection` null, and `produceGeneralizedRdf` set to false ([D28](decisions.md#d28), [Importing RDF](import.md)). The first two settings are the default values specified in the Recommendation; with the second, the base direction of a string is lost, as explicitly stated in section 10 of JSON-LD 1.1. The third setting deviates from the default `true`: a triple featuring a blank node as predicate is not valid RDF 1.1, hence the reader drops it and counts it, in accordance with the expectations of the toRdf test `te068`.

**RDF/XML lies outside.** It would need no new rule for any term, yet no reader for it is planned, and its suite (166 entries at the pinned commit) is not run. JSON-LD embedded in HTML resides outside due to the absence of an HTML reader.

**Why a version and a date.** "The latest Recommendation" names RDF 1.1 today, and unversioned references like `https://www.w3.org/TR/rdf-concepts/` serve the 2014 documents. When RDF 1.2 is issued as a Recommendation, the same words name RDF 1.2, whose novelties reside outside, and the profile and every verdict in a report would shift in meaning while this page remains unchanged. A version and a date name one fixed set of documents.

### Named Graphs

N-Quads, TriG, and JSON-LD embed triples within named graphs, while zelph operates with a single graph. A reader thus constructs the **union** of the default graph and each named graph: a triple becomes a fact regardless of which graph holds it, a triple held by two graphs is a single fact, and the graph name is dropped and counted ([D3](decisions.md#d3), [Named Graphs](import.md#named-graphs)). A blank node label refers to one node across all graphs in a document, within the scope that TriG and N-Quads give it ([D9](decisions.md#d9)).

This departs, by decision ([D28](decisions.md#d28)), from JSON-LD 1.1, in which section 10 says that a consumer using a JSON-LD document as a graph source "MUST only use the default graph and ignore all named graphs", and from the identical expectation that RDF 1.1 Concepts makes regarding every dataset syntax in its non-normative section 4.2. Dropping the named graphs would lose most of what many datasets hold, and the import report counts the quads that carried a graph name, ensuring that nothing enters the union without being counted.

## RDF 1.2

### Status on 23 November 2026

No document in the RDF 1.2 series is a Recommendation. The RDF & SPARQL Working Group lists these eleven RDF documents in parallel with those of SPARQL 1.2; their status and date correspond to those of their respective pages under `https://www.w3.org/TR/` and of their publication histories:

| Document | Status | Date |
|---|---|---|
| RDF 1.2 Concepts and Abstract Data Model | Candidate Recommendation Draft | 16 November 2026 |
| RDF 1.2 Semantics | Candidate Recommendation Draft | 24 September 2026 |
| RDF 1.2 N-Triples | Working Draft | 22 November 2026 |
| RDF 1.2 N-Quads | Working Draft | 20 November 2026 |
| RDF 1.2 Turtle | Working Draft | 11 November 2026 |
| RDF 1.2 TriG | Working Draft | 13 November 2026 |
| RDF 1.2 Schema | Working Draft | 12 November 2026 |
| RDF 1.2 XML Syntax | Working Draft | 25 October 2026 |
| RDF 1.2 Primer | Group Note Draft | 19 November 2026 |
| RDF 1.2 Interoperability | Group Note Draft | 23 July 2026 |
| What’s New in RDF 1.2 | Group Note Draft | 14 July 2026 |

For Concepts and Semantics, the most recent Candidate Recommendation Snapshot is the one from 7 April 2026.

### The Novelties, Each Outside the Profile

The appendices titled "Changes between RDF 1.1 and RDF 1.2" in Concepts, N-Triples, N-Quads, Turtle, TriG, Schema, and XML Syntax, along with the change log in Semantics, list these novelties. Each one resides outside the profile, except for `rdf:JSON`:

1. **Triple terms**: `<<( s p o )>>` serving as the object within a triple in the four text syntaxes, `rdf:parseType="Triple"` in RDF/XML, and their associated semantics.
2. **Reifiers**: `rdf:reifies` and `rdfs:Proposition`; the reified triple `<< s p o ~ r >>` and the annotation syntax `{| … |}` in Turtle and TriG, `rdf:annotation` and `rdf:annotationNodeID` in RDF/XML.
3. **Base direction**: `"…"@ar--rtl` (the terminal `LANG_DIR`), the datatype `rdf:dirLangString`, `its:dir` in RDF/XML.
4. **Version announcement**: `VERSION` and `@version`, `rdf:version` in RDF/XML, a `profile` parameter within media types, and the rule that a later version of N-Triples must require a `VERSION` directive for any document that is not N-Triples 1.2.
5. **Tags compared regardless of case**: two literals whose tags vary solely in case are a single literal. RDF 1.1 permits lowercasing tags, and the mapping does so ([Identity](literals.md#identity)), thus getting the nodes of the RDF 1.2 rule under the ground of RDF 1.1.
6. **`rdf:JSON`**, the exception detailed below.
7. **`rdf:HTML` and `rdf:XMLLiteral`** made normative, `rdf:HTML` admitted in RDF/XML and the canonical mapping of `rdf:XMLLiteral` removed; in the profile, both are datatypes without an expander ([Datatypes](literals.md#datatypes)).
8. **Semantics**: the entailment rule `rdfD1a` added, the meaning of `rdf:PlainLiteral` dropped.
9. **Canonical forms**, newly defined for N-Quads and changed for N-Triples, produced exclusively by a writer.

The remaining elements reside within the profile already or result in no alteration to it: the errata 24 and 30, which the profile applies, clarifications (surrogate code points are illegal in documents and escapes, a string consists of Unicode scalar values, the result of parsing a non-conforming document is undefined) and editorial changes (terminology, explicit definitions of a quad and of term equality, a vocabulary registry, media type registrations, removed and reorganized sections).

**Why triple terms lie outside.** A triple term is not asserted: `:x :says <<( :a :b :c )>>` does not state `:a :b :c`. In zelph a statement functioning as a term within another statement is a fact in its own right, asserted just like any other, unless it appears within an argument of an [opaque predicate](../logic.md#mentioning-a-statement-opaque-predicates); mapping a triple term onto a nested statement would result in asserting what the document merely mentions. Additionally, an opaque predicate cannot host triple terms: opacity is inherent to a predicate, not to a single instance, so every fact of the predicate would mention its arguments, including the lists of [D10](decisions.md#d10) among them, since a `cons` cell within an opaque argument becomes a mention as well; and the declaration is refused once a fact of the predicate holds a statement, so a reader meeting the first triple term mid-stream may no longer be able to declare it.

**The exception: `rdf:JSON`.** JSON-LD 1.1 defined this datatype first; RDF 1.2 adopted it from section 10.2 of JSON-LD 1.1 and updated its value space. The profile adheres to the definition of JSON-LD 1.1: the datatype `http://www.w3.org/1999/02/22-rdf-syntax-ns#JSON`, whose lexical space comprises every string accepted by the JSON grammar outlined in RFC 8259. toRdf produces such a literal from a value typed `@json`, in a canonical lexical form that JSON-LD 1.1 describes itself; the reader writes the form of the JSON Canonicalization Scheme (RFC 8785), which meets that description and which the suite assumes for the 23 entries marked with `useJCS`, all within the profile. Without an expander, such a literal is a value node whose identity is its lexical form.

## The Test Suites

### Two Repositories at Fixed Commits

| Repository | Directories | Pinned commit | Committed |
|---|---|---|---|
| `w3c/rdf-tests` | `rdf/rdf11/rdf-n-triples`, `rdf-n-quads`, `rdf-turtle`, `rdf-trig` | `56076c0efec4c353efcae2971be8f6005294ac64` | 18 November 2026 |
| `w3c/json-ld-api` | `tests/`, including `toRdf-manifest.jsonld` | `fd4f7eb41f679fb73a54923143c473c3a1d06093` | 18 November 2026 |

Both served as the heads of the default branch `main` on 23 November 2026.

**Why pinned.** The test suites change under identical names. In 2026 alone, `rdf-tests` retyped and renamed the negative evaluation tests for Turtle and TriG (`turtle-eval-bad-01` is now the negative syntax test `turtle-syntax-bad-uri-escape-01`), removed its `rdft:Proposed` annotations, modified `IRI-resolution-07`, and renamed two TriG test types, while `json-ld-api` turned its Editor’s Draft into that of JSON-LD 1.2 and added a toRdf entry. A runner tracking the heads would shift verdicts without any change in zelph. A new pin constitutes an update to this page, with every count and class taken again.

### Entries per Suite and Type

Approval is no longer encoded as a mark within the manifests: on 21 October 2026 `rdf-tests` removed every `rdft:Proposed` mark, as approval is becoming implicit through the review of the pull request introducing a test and the marks are deemed "no longer relevant". Thus, every entry listed in a manifest contributes to the count; 740 out of the 827 entries across the four suites still bear `rdft:Approved`, while the remaining 87 carry no mark. The JSON-LD manifests carry no marks whatsoever. Counted from the manifests at the pinned commits:

| Suite | Positive syntax | Negative syntax | Evaluation | Negative evaluation | Entries |
|---|---:|---:|---:|---:|---:|
| N-Triples | 41 | 29 | – | – | 70 |
| N-Quads | 53 | 34 | – | – | 87 |
| Turtle | 74 | 94 | 145 | – | 313 |
| TriG | 99 | 115 | 143 | – | 357 |
| JSON-LD toRdf | 16 | – | 346 | 106 | 468 |

N-Triples and N-Quads feature syntax tests exclusively, whereas Turtle and TriG include evaluation tests as well. A negative evaluation entry in JSON-LD expects an error code.

### The JSON-LD Baseline

The JSON-LD suite is judged against JSON-LD 1.1 as the Recommendation specifies, incorporating the corrections the Editor’s Draft made up to the pinned commit, to the extent that the suite examines them. As of 4 June 2026, that draft is the one of JSON-LD 1.2, whose processors default to `processingMode` `json-ld-1.2`; the profile continues to use `json-ld-1.1`, and no entry at the pinned commit refers to `json-ld-1.2`. Compared to the suite at the Recommendation (460 entries, 437 within the profile), the manifest includes eight additional entries, two duplicate ids have been renamed, and `ter53` is now limited to JSON-LD 1.1; no file from an earlier test has undergone modification.

The options associated with the entries, an entry carrying several at times:

| Option | Entries | Inside the profile |
|---|---:|---|
| none | 185 | yes |
| `specVersion` `json-ld-1.1` | 266 | yes |
| `specVersion` `json-ld-1.0` | 11 | no: the entry tests JSON-LD 1.0, whose behaviour was modified in 1.1 |
| `processingMode` `json-ld-1.0` | 8 | no: the reader operates exclusively in `json-ld-1.1` mode |
| `produceGeneralizedRdf` `true` | 2 | no: not RDF 1.1; both entries also stand in a prior row |
| `rdfDirection` `i18n-datatype` (3) or `compound-literal` (2) | 5 | no: the profile sets `rdfDirection` to null; all five are non-normative |
| `useJCS` | 23 | yes: `rdf:JSON` |
| `base` | 8 | yes |
| `expandContext` | 1 | yes |

Thus, 24 entries lie outside the profile and 444 are within it: 335 for evaluation, 93 for negative evaluation, and 16 for positive syntax. Eighteen of the 444 entries load a context via IRI (`@context`, `@import`, `expandContext`): seventeen using IRIs relative to the entry under `https://w3c.github.io/json-ld-api/tests/`, which the runner’s document loader serves from the clone, meaning the suite needs no network access, and `ter04` using one that cannot be dereferenced, expecting the error `loading remote context failed`.

Two additional manifests reach RDF: the HTML manifest featuring 20 toRdf entries, and the remote-document manifest containing 18 expansion tests involving retrieval by content type, redirect, and `Link` header. The 20, along with three of the 18, need HTML script extraction and lie outside. The remaining 15 are evaluated via toRdf using a loader that responds according to the entry’s options, with the expected expanded document converted by the same reader.

## How a Test Is Judged

### Syntax Tests

A **positive syntax test** passes if the reader reads the file without encountering an error and leaves no triple unwritten. A **negative syntax test** passes when the reader refuses the file; what the import wrote before the error is irrelevant. JSON-LD’s positive syntax entries are assessed identically, and its negative evaluation entries pass upon refusal, with the expected error code noted alongside.

A triple is unwritten when the import report counts it as not written due to its predicate being identical to its object, or because it is a label, description, or alias in a language that was not selected ([The Import Report](import.md#the-import-report)); the runner selects every language (`labels=all`, `descriptions=all`, `aliases=all`). The report’s counts of graph names dropped by [D3](decisions.md#d3) and of triples featuring a blank node as predicate, which toRdf excludes when `produceGeneralizedRdf` is false, serve as informational data and never cause a test to fail.

### Evaluation Tests

An **evaluation test** passes when the reader reads the action without an error, leaves no triple unwritten, passes the accounting described [below](#what-the-comparison-cannot-see), and builds a graph that is isomorphic to the graph built from the expected file, an N-Triples file for Turtle and an N-Quads file for TriG and JSON-LD. Both are zelph graphs, each built by zelph’s own reader in a distinct process; isomorphic signifies equality modulo a relabelling of blank nodes.

Ids cannot be compared: [D9](decisions.md#d9) derives a blank node’s id based on the identity of its document, and the id of a list that holds a blank node changes with it, since the node of a `cons` cell is determined by its element and its rest. The runner writes each graph as an **id-free dump of terms**:

- an IRI node is its IRI, retrieved via the [namespace table](iris.md#the-namespace-table);
- a value node is its datatype IRI, lexical form, and lowercase tag ([Identity](literals.md#identity));
- a blank node is a variable;
- `nil` is the empty list, and a list is expressed as `LIST(e1, …, en)` with its elements dumped by identical rules;
- a name within a label language is a label tuple comprising node, text, and language ([Names](names.md));
- a fact is the triple of its serialized components;
- the declarations `P ~ ->` that the engine writes for every predicate are omitted.

Isomorphism is determined through **colour refinement and backtracking** using the dumps: all variables start with a single colour, and during each round a variable is recoloured based on the multiset of its places, a place being a spot within a triple, label tuple or `LIST` together with the contents filling the other positions. Once the colours stabilize, a variable is mapped if its colour is unique on each side; for variables sharing a colour, the runner tests each possibility in sequence, refining after each choice and backtracking upon failure. The test passes when a mapping results in the dumps being identical as sets, since a dataset is a set; `toRdf/e036-out.nq` lists two statements twice.

### Through Lists

[D10](decisions.md#d10) turns every well-formed collection into a list on both sides, causing most chains of `rdf:first` and `rdf:rest` to arrive at the comparison as `LIST` terms. In a Turtle test authored using the collection syntax, the action’s list comes from the reader’s shortcut where it applies, and the expected one stems from the conversion step applied to the triples within the `.nt` file ([Collections and Lists](collections.md)), thus the comparison also holds the two ways against each other.

Two lists match if they share the same length and their elements match index by index. A blank node appearing within the elements is a variable just like any other, and its position within the list is part of its place, so refinement and backtracking reach into lists and into nested lists. A blank node that stands in a list and in statements of its own maps to a single node everywhere. In `turtle-eval-lists-06`, `:a :b ([ a :c ]) .`, the list’s element is such a node, and both files give this dump:

!!! example "Specified – not yet implemented"

    ```
    <http://www.w3.org/2013/TurtleTests/a> <http://www.w3.org/2013/TurtleTests/b> LIST(?1)
    ?1 <http://www.w3.org/1999/02/22-rdf-syntax-ns#type> <http://www.w3.org/2013/TurtleTests/c>
    ```

A chain that fails to be well-formed stays as statements on both sides, its cells compared as variables. In the pinned suites, this occurs exclusively in the toRdf entries `tli12` and `tli14`, where the sole cell omits `rdf:first` due to its element expanding into an invalid IRI. Collections occur in 12 Turtle, 7 TriG and 25 in-profile toRdf expectations, nested lists within lists in 14 of those, and a blank node that is not a list stands as an element in two cases, `turtle-eval-lists-06` and `t0036`.

### Dataset Tests

A TriG or JSON-LD evaluation test compares datasets as zelph reads them, as [union graphs](#named-graphs), and two counts judge what the union hides: the distinct quads that carry a graph name and the distinct graph names within them, both counted by an import with `accounting=on` ([Parameters](import.md#parameters)), must be equally numerous on both sides. In the pinned suites, 128 out of 143 TriG evaluation tests and 44 out of 335 in-profile toRdf evaluation entries contain such quads, and in 104 of the TriG tests a triple appears across more than one graph, which the union holds as a single fact.

## What the Comparison Cannot See

Each side of an evaluation test traverses the identical mapping, so the common actions of the two readers remain concealed. `turtle-subm-26`, "Variations on decimal canonicalization", illustrates this: its `.ttl` and its `.nt` hold the same 22 triples, featuring the subject `<http://example.org/foo>`, the predicate `<http://example.org/bar>`, and an `xsd:decimal` literal as object, with identical lexical forms, four of which (`"1"`, `"1.0"`, `"1."`, `"1.000000000"`) denote the value 1. A reader that rewrote every lexical form into its canonical version would write 19 facts from each file, the dumps would become identical, and the test would succeed, even though the reader merged four literals and changed lexical forms. Shared decoding errors, lowercased tags, the conversion of collections, and a triple both readers discard pass alike. Furthermore, readers of N-Triples and N-Quads, which build every expected graph, are assessed solely through syntax tests.

The argument that a literal’s identity is its exact lexical form therefore rests on RDF 1.1, not on the suites: two literals are one term solely when their lexical forms, datatype IRIs, and tags compare equal on a character-by-character basis (RDF 1.1 Concepts, section 3.3), meaning `"1"^^xsd:decimal` and `"1.0"^^xsd:decimal` are two distinct terms sharing one value ([Identity](literals.md#identity)).

Four additions see what the comparison fails to capture:

- **An accounting of the triples read.** With `accounting=on`, a parameter of the import that the runner sets ([Parameters](import.md#parameters)), each import counts the distinct triples it parsed, over the RDF terms as the syntax delivers them, before any mapping, with graph names dropped and tags converted to lowercase (the one normalization applied by the mapping). The runner mandates that for each import, the facts from its dump, its label tuples, the triples absorbed into lists (the `rdf:first` and `rdf:rest` triples, regardless of whether the conversion step or the reader’s shortcut took them, and the dropped `rdf:type rdf:List` statements) and the statements the conversion folded into equal ones sum to that count. The count does not include a triple that toRdf leaves out due to its predicate being a blank node. A canonicalizing reader fails in `turtle-subm-26` with 19 facts for 22 triples, and so does a triple both readers drop.
- **zelph’s own tests of the corners of D10** ([Which Chains Are Converted](collections.md#which-chains-are-converted)): shared tails, heads referenced by other statements, typed cells, cycles via `rdf:first`, chains lacking `rdf:nil`, named cells, the longest well-formed end of a chain. The suites only barely reach these, every chain in their expectations being well-formed except in `tli12` and `tli14`. The tests are zelph scripts composed of `_:label` statements and `.lists`.
- **Tests that a simple literal is its `xsd:string` form**: `"abc"` and `"abc"^^xsd:string` are a single term (section 3.3) and a single value node. In the suites, an explicit `xsd:string` occurs in seven syntax tests exclusively, never in an evaluation expectation.
- **The N-Triples expectations read by the Turtle reader.** N-Triples is a subset of Turtle, so the Turtle reader must construct the identical graph as the N-Triples reader from each of the 145 `.nt` files in the Turtle evaluation tests, and the reference reader is assessed by a second one.

## Tests Outside the Profile or Expected to Fail

| Class | Entries | Verdict | Reason |
|---|---:|---|---|
| a triple where the predicate is the object | 7 | expected to fail | the triple is counted, not written |
| blank node labels containing `:` (erratum 30) | 4 | refused, as expected | included within the profile via the erratum |
| RDF 1.2 features present in the pinned directories | 0 | – | – |
| toRdf, `specVersion` `json-ld-1.0` | 11 | outside | JSON-LD 1.0 |
| toRdf, `processingMode` `json-ld-1.0` | 8 | outside | a processing mode not part of the profile |
| toRdf, `rdfDirection` set | 5 | outside | base direction, non-normative |
| HTML manifest, toRdf entries | 20 | outside | HTML script extraction |
| remote-document manifest, HTML entries | 3 | outside | HTML script extraction |
| RDF/XML | 166 | outside | no reader |

**A triple where the predicate is its object** stands in the Turtle tests `turtle-syntax-prefix-05` and `turtle-subm-14` (`: : : .`) and `turtle-subm-25` (`foo:blah foo:blah foo:blah .`) and the TriG tests `trig-syntax-minimal-whitespace-01`, `trig-syntax-prefix-05`, `trig-subm-14` and `trig-subm-25`: three positive syntax tests and four evaluation tests. Today the engine refuses such a statement, and under the mapping each route counts the triple and reports it rather than writing it ([Statements](iris.md#statements)). In the four evaluation tests the comparison would alone succeed, as the expected graph also omits the triple; however, the accounting fails them. Whether the engine ought to accept such statements remains undecided ([Open decisions](open-decisions.md#triples-whose-predicate-is-their-object)); until resolution, the seven are treated as expected failures.

**Erratum 30.** `nt-syntax-bad-bnode-01` (`_::a`) and `nt-syntax-bad-bnode-02` (`_:abc:def`) appear within both the N-Triples and the N-Quads suite, and are considered negative solely due to the erratum: the 2014 specification permits both labels. The readers follow the erratum; the report marks the four entries as contingent upon it.

**RDF 1.2.** None of the four pinned suites uses a feature of RDF 1.2. Its tests lie in `rdf/rdf12/`, 320 entries for the four syntaxes at the pinned commit, and are not run.

## Literals the Readers Must Accept

The readers accept every literal that RDF 1.1 admits ([Datatypes](literals.md#datatypes)). An ill-typed literal such as `"abc"^^xsd:integer` is a value node just like any other, because RDF 1.1 Concepts mandates that implementations must accept ill-typed literals (section 3.3); it merely undergoes no expansion. A literal with a datatype that nothing recognizes is also accepted, as RDF using an unknown datatype should not be rejected (section 5.4). A literal typed `rdf:langString` lacking a tag is not an RDF 1.1 literal, and every reader refuses it as a syntax error.

## The Runner

The runner produces the conformance report for the W3C suites outlined in the NLnet project’s plans ([RDF Terms in zelph](index.md)). It

- **dispatches on the manifests’ own type IRIs**, exclusively and not on a test’s name. The TriG types are written with a lowercase `g` throughout, such as `rdft:TestTrigEval`; before 4 November 2026, the vocabulary `ns/rdftest.ttl` used the two syntax types `TestTriG…`. An entry of an unrecognized type fails the count guard;
- **takes the base IRI** of an action from `mf:assumedTestBase` and the action’s file name, the IRI from which the file is fetched (as per RFC 3986, section 5.1.3, as the TriG README mandates), and forwards it as `base=`; N-Triples and N-Quads rely solely on absolute IRIs. For JSON-LD the base is derived from the manifest’s `baseIri` combined with the input’s path, or from the entry’s option `base`;
- **builds the actual and the expected graph in independent processes**, ensuring no residual data from one import influences the other; today a process holds one engine;
- **guards the counts** of this page at the pinned commits and stops when the suites before it diverge, thereby detecting a modified suite instead of allowing verdicts to shift silently;
- **resolves relative IRIs by RFC 3986, section 5.2, within zelph’s own code.** The tests `IRI-resolution-01`, `-02`, `-07`, and `-08` of Turtle and TriG check the removal of dot segments, which serdi 0.32.10, run on them at the pinned commit, misinterprets in all eight cases (`g/.` instead of the expected `g/`);
- **writes the report**: the pinned commits, each test with its verdict (passed, failed, expected to fail along with its class), every class outside the profile with its count and reason, and the counts of each import.
