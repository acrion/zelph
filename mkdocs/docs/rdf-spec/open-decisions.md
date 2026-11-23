# Open Decisions

The pages of this specification settle technical specifics: the names of predicates and functions, the section of the `.bin`, the id classes. This page gathers what falls outside their authority: the decisions that change the meaning of existing scripts or of published networks, or that weigh a meaning against a cost. Each entry states the question, what the specification assumes until a decision is made, the alternatives along with their respective costs, and a recommendation.

## A Marker for the Label Allocation

**Question.** What marker is conveyed by a network whose labels were allocated under the mapping, and what action does the ACE front end take regarding it?

Today the Wikidata import writes the node `wikidata:labels-v2` into a network, indicating that each entity kept its label and that a shared label resolves to its first-ranked carrier. Under the mapping, labels are allocated differently: a `mul` label is a name in `mul` and is no longer copied into `en` ([Fallback Chains](names.md#fallback-chains)). The specified [ACE front end](../ace-spec/decisions.md#dec-wikidata) relies on the marker and on the copy: it requests all English carriers of a word and expects an entity with only a `mul` label, such as Douglas Adams, among them.

**Until decided.** An import under the mapping writes a marker of its own, `wikidata:labels-v3`, and writes no `wikidata:labels-v2`.

| Variant | What it means | Cost |
|---|---|---|
| (a) | The front end requires the new marker and requests the carriers for each stage in the chain: the English carriers alongside the `mul` carriers lacking an English name, ordered as today. | The front end resolves each word as it does today ("Douglas Adams" is `Q42`). Its specification changes in the decision, the page on words, and the tests; no answer changes. |
| (b) | The front end adheres to the lookup order of the mapping: the initial language in the chain possessing the name. | Simpler, and the same rule as applied to scripts. "Douglas Adams" becomes a disambiguation page wherever an English carrier for the text is present; the front end’s tutorial session and its tests yield different outcomes. |
| (c) | The prior marker remains in use for both allocations. | No method distinguishes a network with copies from one without; the front end would misread one of them. |

**Recommendation.** (a).

## The Naming Languages of the ACE Front End

**Question.** The ACE front end names its content in the naming languages `ace` and `ace-lex`. `ace` also serves as the language code for Acehnese, and Wikidata includes labels in that language (Berlin, `Q64`, possesses one). What keeps the two apart?

**Until decided.** `ace` and `ace-lex` are reserved: no label becomes a name within these, and a label tagged with `ace` stays a value node ([Label Languages](names.md#label-languages)).

| Variant | What it means | Cost |
|---|---|---|
| (a) | The reservation remains in place. | Acehnese labels never become names. |
| (b) | The front end’s languages are converted into spellings that cannot function as language tags, using a colon for example. | Modifications across the entire ACE specification, which currently lacks implementation. Acehnese becomes a label language indistinguishable from any other. |
| (c) | The import refuses `ace` when used in its language parameters. | Same as (a), but triggers an error where (a) remains silent. |

**Recommendation.** (b).

## A Display That Always Reads Back

**Question.** The [re-read guarantee](notation.md#the-re-read-guarantee) covers the forms of the mapping, lists, and names within their respective language. Should it cover every printed node?

**Until decided.** It does not. A Wikidata item under a different session language displays its ID alongside its label, a non-first carrier of a shared label appears using that label, and a character list prints as a list of `zelph` characters; none of these reads back across every session.

| Variant | What it means | Cost |
|---|---|---|
| (a) | The guarantee as specified. | The cases above remain excluded from it. |
| (b) | A node whose shown name does not resolve back to it in the session language is printed in a form that does: its IRI if it possesses one, otherwise a new notation for a name in a stated language, such as `"Q42"@@wikidata`. | Published output is altered: the display of items on the Wikidata page, exports of derivations, the transcripts of the ACE specification. One lookup per printed node. A notation differing by a single character from a language-tagged string. |
| (c) | A character list is printed in the compact form and in written order, `<abc>`, which reads back in every language. | It departs from the display in stored order that strings are specified to have, and it changes the output shown on the page for lists. |

**Recommendation.** (a) immediately. (b) is the one to take up if scripts end up requiring printed output as input across languages.

## Day Numbers in Two Parts

**Question.** A bound of a date interval is a single number, the Julian Day Number. Should a bound also be expressed in two parts, the day number divided by 1000 and the remainder, in order to enable comparisons by rules to reuse more of their computational effort?

A comparison according to the rules of `cmp` derives facts for each pair of numbers it compares, and two day numbers that share their leading digits also share the comparison of those digits. When written in two parts and compared part by part, the comparisons of the high parts are common to all dates within a 1000-day span. On a prototype of 5 November 2026, the derived facts per compared pair dropped from approximately 10 to 2.5 under the decimal substrate and from roughly 24 to 3 under the binary one, at 16,000 pairs, and the execution time fell by a factor ranging from 1 to 2 (measured).

**Until decided.** One number per bound.

| Variant | What it means | Cost |
|---|---|---|
| (a) | One number per bound. | Comparisons by rules derive more facts. Comparisons by the [numeric conditions](dates.md#the-three-relations) derive none either way. |
| (b) | Both forms: the day number for display and for the conditions, the two-part term for `cmp`. | A second expansion per date; rules must compare the high parts as terms and must not join items on them. |

**Recommendation.** (a). As the engine answers comparisons as conditions, the checks derive nothing per pair, and the two-part form would fulfil only a check that wants every comparison to be derived within the graph.

## Three Points of the SPARQL Front End

**List patterns.** A pattern `?l rdf:first ?x` with no other bindings matches every `cons` cell within the graph, and in a graph with expanded values these are predominantly the cells of numerals and character lists.

| Variant | What it means | Cost |
|---|---|---|
| (a) | Every cell answers. | A query over lists lists the digits of numbers. |
| (b) | A list pattern must be anchored: its list is bound, or reached from a statement that is not a list statement. An unanchored one is refused with a message. | A query seeking all lists within a graph must name the predicate that holds them. |
| (c) | Cells reachable exclusively through expansion statements are excluded. | A test of reachability for every cell. |

**Recommendation.** (b), which the specification assumes until decided.

**`true` and `false`.** SPARQL reads them as boolean values regardless of case; today the front end reads them as names in the session language.

| Variant | What it means | Cost |
|---|---|---|
| (a) | SPARQL’s reading: the two literals of `xsd:boolean`. | A query that used them as names undergoes changes; none is known. In native zelph `true` stays a name, which the standard library relies upon. |
| (b) | Names, as today. | A departure from SPARQL at a point every user expects. |

**Recommendation.** (a), which the specification takes as given until a decision is reached.

**A declared `PREFIX` in opposition to a built-in one.** Today, a built-in prefix prevails: following `PREFIX p: <http://example.org/>`, `p:x` continues to denote Wikidata’s `p:x`.

| Variant | What it means | Cost |
|---|---|---|
| (a) | The declared prefix wins. | A query that declared a prefix it never meant to use undergoes alteration; no such case is known. |
| (b) | The built-in prefix takes precedence, and the page on SPARQL states this explicitly. | A query copied from another source silently means different nodes. |

**Recommendation.** (a), which the specification assumes until decided.

## Clusters and Nodes Found by Content

**Question.** Every node generated during a cluster’s active phase is recorded by the cluster, and discarding it eradicates those nodes alongside any fact that names them. What happens to a node initially produced within a cluster and subsequently found again, based on its content, from beyond its scope?

Today’s behaviour regarding numerals is this: a numeral initially constructed within a cluster goes when the cluster is dropped, along with a fact that was stated outside the cluster afterwards. Value nodes, lists, and the terms of expansions are found by content in the same manner, making this scenario widespread through the mapping. The SPARQL front end and the specified ACE front end both operate within a scratch cluster that they subsequently drop.

**Until decided.** Today’s behaviour.

| Variant | What it means | Cost |
|---|---|---|
| (a) | Record as today. | A value initially encountered within a scratch question vanishes alongside it, and with it facts asserted afterwards. |
| (b) | Nodes found by content are never recorded. | A scratch question leaves its values and numerals permanently; "a query leaves nothing in the graph" ceases to be true. |
| (c) | Adopted on reuse: upon rediscovering a node from outside the cluster that originally recorded it, it leaves that record. | One record lookup per hit while clusters hold records. `.cluster-drop` removes less than today in scripts that reuse nodes across clusters. |

**Recommendation.** (c).

## Number Terms in the ACE Front End

**Question.** The mapping prints `(neg zint &5)` as `&-5` within a term ([Constants](numbers.md#constants)). The specified ACE front end identifies a number through the printed forms `&` followed by digits and `neg zint &` followed by digits. With the new display it would take every negative number as no number.

**Until decided.** The display remains as specified; the ACE specification is not changed by this work.

| Variant | What it means | Cost |
|---|---|---|
| (a) | The front end’s rule learns the pattern `&-` accompanied by digits. | A minor adjustment, and the subsequent modification of a display breaks it again. |
| (b) | The front end reads number terms via their structure, using a function that answers the decimal text of a numeral, of `(neg zint N)` and of `(M dec K)`. | A single function in the standard library; the front end ceases to rely on any display. |

**Recommendation.** (b).

## Engine-Answered Predicates Written as Statements

**Question.** `zelph:lang`, `zelph:datatype`, and `zelph:lexical` are refused as statements and as consequences ([Querying Terms](querying.md#conditions-the-engine-answers)). `!=` and the six numeric comparisons are also answered by the engine, and today each of them may be written as a statement: `&3 zelph:lt &5` asserts a fact that no condition ever reads. Should they be refused as well?

**Until decided.** They stay ordinary facts, as documented today.

| Variant | What it means | Cost |
|---|---|---|
| (a) | As today. | Two kinds of engine-answered predicate, one that is refusable and one that is not; a statement like `&7 zelph:lt &6` can be stored and means nothing. |
| (b) | The six comparisons are refused as statements and consequences. | They are new in the release currently being prepared; one paragraph on the logic page and the tests that confirm them undergo modification. |
| (c) | `!=` is refused as well. | Scripts that assert `a != b` as a fact cease to load. |

**Recommendation.** (b).

## Triples Whose Predicate Is Their Object

**Question.** zelph refuses a statement in which the predicate serves as its own object, and each route in the mapping counts such triples without writing them ([Statements](iris.md#statements)). Should the engine support them?

**Until decided.** They are counted and reported.

| Variant | What it means | Cost |
|---|---|---|
| (a) | Count and report. | Seven tests from the W3C suites that hold such a triple are intentionally failing ([Conformance](conformance.md)), and the twelve such triples that Wikidata’s truthy data held on 4 November 2026 (counted on the Wikidata Query Service) are missing. |
| (b) | The engine admits such statements. | A change to the way a fact is identified from its parts, in both the engine and its bulk path. |

**Recommendation.** (a).

## Not Open

Two aspects a reader might anticipate at this juncture are settled.

- **DEC-DATA of the ACE front end remains unchanged.** Strings continue to be character lists there, precisely the [expansion of strings](expansion.md#strings); real numbers keep having no route until a module for decimal arithmetic is present; and the empty string of the front end keeps its own node for as long as its shapes use `nil` for a missing object, as the side remark beside the expansion of the empty string says.
- **No instance was identified where a script must choose between Turtle’s meaning of a spelling and zelph’s.** The forms the mapping takes from Turtle were either free or unused ([What Changes for Existing Scripts](notation.md#what-changes-for-existing-scripts)).
