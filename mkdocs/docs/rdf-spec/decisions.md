# Design Decisions

Wherever the mapping presented more than one plausible outcome, the choice was taken as a decision. This page lists them: what was settled, the rationale behind it, and the page that carries the full account. The first thirteen are the decisions the specification began with; the remaining ones were made during the process of working out its consequences. What remains unresolved can be found on [Open Decisions](open-decisions.md).

## The Thirteen Decisions

### D1 – An IRI is a named node {#d1}

**Decision.** Each IRI is a node named by its complete IRI within a naming language for IRIs, except when a namespace table maps its namespace to a naming language and a name prefix: `http://www.wikidata.org/entity/Q42` is the node named `Q42` in `wikidata`.

**Why.** Importing Wikidata’s RDF shares its nodes with the networks built from the JSON dump, and scripts relying on identifiers function across both. The table expands upon the one the SPARQL front end maintains; there is no second such table.

**Where.** [IRIs and the Namespace Table](iris.md).

### D2 – Values are expanded on request {#d2}

**Decision.** A value is turned into numbers, character lists, or intervals exclusively when a statement or a call asks for it, typically for the values of selected properties.

**Why.** The cost emerges exclusively in contexts where a value is needed. A full import of Wikidata holds hundreds of millions of dates and numbers, and the network, even without them, already demands approximately 200 GiB (computed, [Costs](costs.md#the-node-map); 223.7 GiB were measured before the engine moved to a single node map, [Binary Data Files](../binaries.md)).

**Where.** [Expanding Values on Request](expansion.md).

### D3 – A graph name is dropped and counted {#d3}

**Decision.** The fourth element of a quad is discarded, and the import reports the number of graph names it discarded. Retaining the name as a statement about the statement is described as a possibility for future use, along with its associated costs.

**Why.** zelph maintains a single graph. Under an ordinary predicate, a statement that refers to another statement asserts the inner statement, meaning graphs preserved in that manner would function as annotations rather than distinct graphs; under an opaque predicate, they would remain distinct, and rules would reach their triples exclusively via the graph predicate. Either form adds one more fact per quad.

**Where.** [Named Graphs](import.md#named-graphs).

### D4 – Literals are written as in Turtle {#d4}

**Decision.** Typed and language-tagged literals take Turtle’s notation, `"1952-03-11"^^xsd:date` and `"Berlin"@de`. The forms of plain strings, IRIs, and blank nodes, whose Turtle spellings zelph already employs, are settled separately ([D20](#d20)). Each printed term is read back as the identical node.

**Why.** This kind of literal is written in zelph as it is written in Turtle. The change this makes to existing spellings was checked against the tests, the documentation, and the standard library, and no use of them was found; the two languages are not separated by a mode.

**Where.** [Writing and Displaying Terms](notation.md).

### D5 – A string expands to a character list that ends in `nil` {#d5}

**Decision.** The expansion of a string is the character list zelph builds today: reversed, ended by `nil`. The empty string expands to `nil`. `"123"^^xsd:string` and `"123"^^xsd:integer` stay two value nodes, even though their expansions might be a single node under certain substrates.

**Why.** The list built from `cons` and `nil` is integral to zelph’s design, and it is not weakened for a practical advantage: a list of digits is a numeral, and this conflict is accepted deliberately. The ACE specification’s approach to strings remains valid without alteration.

**Where.** [Strings](expansion.md#strings).

### D6 – Wikidata’s dates get their intervals from full values {#d6}

**Decision.** Wikidata’s RDF writes each date as an `xsd:dateTime`, with the export having already adjusted it according to precision and calendar, or as a plain string in the few cases where its cleaner gives up. An expander can be registered for a class of nodes as well as for a datatype; the one designated for Wikidata’s full time values reads value, precision, and calendar together.

**Why.** Precision and calendar are present exclusively at the full value nodes. A network constructed solely from the truthy triples lacks intervals for Wikidata’s dates, and stating this is preferable to computing wrong ones.

**Where.** [Wikidata under the Mapping](wikidata-mapping.md), [Wikibase’s Precisions](dates.md#wikibase-precisions).

### D7 – Labels become names {#d7}

**Decision.** A label becomes a name within the language associated with its tag, according to the current rules governing labels shared among several entities. A label that cannot become a name stays a value node, just as all descriptions and aliases do.

**Why.** Scripts under `.lang en`, the class-hierarchy page, and the label handling of SPARQL continue to function; an import from RDF yields identical names as one from JSON; and English labels carry the same cost they cost today.

**Where.** [Labels, Names and Languages](names.md).

### D8 – The profile is RDF 1.1 {#d8}

**Decision.** The mapping covers RDF 1.1, not RDF 1.2. Every innovation introduced in RDF 1.2 is listed and declared to fall beyond the scope of the profile.

**Why.** RDF 1.2 has not yet reached Recommendation status. Its triple terms are not asserted, whereas zelph asserts every nested statement located outside an opaque position.

**Where.** [Profile and Conformance](conformance.md).

### D9 – A document has an identity {#d9}

**Decision.** The ids of blank nodes are determined by the identity of their document, which is established before reading the first statement: a string provided by the user, otherwise the source of the import.

**Why.** A reader can assign identifiers to blank nodes in a single pass using constant memory, even when reading from a pipe; the same document imported twice produces an identical graph; two distinct documents never share a blank node.

**Where.** [The Identity of a Document](blank-nodes.md#the-identity-of-a-document).

### D10 – A collection becomes a list {#d10}

**Decision.** A well-formed RDF collection becomes a zelph list, in every reader, regardless of whether the source wrote the collection syntax or the chain of `rdf:first` and `rdf:rest`. A chain that is not well-formed stays as statements. `rdf:nil` is `nil`.

**Why.** RDF must not create a parallel universe within zelph: the `cons` list is deliberate and has been validated. That a collection thereby becomes a value, with equal collections being a single node, is stated along with its consequences.

**Where.** [Collections and Lists](collections.md).

### D11 – One node per number {#d11}

**Decision.** A value from the decimal family expands to exactly one node that captures it without approximation: a whole value to the existing number within the standard library, any other to a term `(M dec K)`. `xsd:double` and `xsd:float` expand to their exact value in identical formats. For Wikidata’s quantities, the amount and bounds expand in this way, the unit is an item, with the unit `"1"` of the JSON becoming `Q199` as in Wikidata’s RDF, and an interval arises solely when bounds are stated.

**Why.** Every value is exact under every substrate, and a number is one node across all datatypes. The separation of floating-point values from decimal ones, as maintained by XSD, is intentionally disregarded.

**Where.** [Numbers](numbers.md); for quantities, [Wikidata under the Mapping](wikidata-mapping.md#quantities).

### D12 – The languages of an import are a parameter {#d12}

**Decision.** The set of languages an import keeps for labels, descriptions, and aliases is a parameter, and the import reports what it omitted. By default, it preserves labels in `en` and `mul`, with no descriptions and no aliases.

**Why.** An RDF import of Wikidata constitutes a selection in any case; descriptions in all languages alone would bring the full network to the limit of the node map. The default does not consume memory for further languages, descriptions, or aliases, since it keeps the labels that the JSON import keeps, so nobody is taken aback by it.

**Where.** [Parameters](import.md#parameters), [Costs](costs.md).

### D13 – `mul` is a language of its own {#d13}

**Decision.** `mul` stays a naming language; `en` falls back to `mul` when retrieving a name or displaying a node. The JSON import operates identically and no longer copies a `mul` label into `en`.

**Why.** A writer can determine if the source wrote `@en` or `@mul`.

**Where.** [Fallback Chains](names.md#fallback-chains); generalized by [D30](#d30).

## Identity and Storage

### D14 – Value nodes are interned {#d14}

**Decision.** The identifier of a value node consists of its class combined with a sequence number. A content index finds an existing node, just as the name index does for names, and undergoes reconstruction upon loading. The `.bin` format owes older files no compatibility, and no code is kept for a superseded layout: all published networks are regenerated.

**Why.** Absence of a hash in the identifier eliminates collisions, prevents fixation on a specific hash function, and removes any mechanism for versioning. The alternative – using a content hash – would have left 59 bits and required a collision policy.

**Where.** [Identity](literals.md#identity), [Storage](storage.md).

### D15 – Requests are facts {#d15}

**Decision.** A request for an expansion is a standing fact, saved with the graph, and may be the consequence of a rule. Requests are answered following each import, each input line, each fixpoint of a run, and upon registration of an expander.

**Why.** A request that is a fact persists through a save, catches values that arrive afterwards, and causes `.explain` to display the reason a value was expanded.

**Where.** [Requests](expansion.md#requests).

### D16 – The engine answers tag, datatype, text and labels {#d16}

**Decision.** The conditions `zelph:lang`, `zelph:datatype`, and `zelph:lexical` are answered by the engine, just as `!=` is, and `X rdfs:label L` is answered from the name tables. Facts derived from a label are not withdrawn when the name is altered.

**Why.** Text, tag, and datatype are not facts and must not incur a cost of one each. Labels read from names do not require label statements, which would otherwise amount to between 16 and 19 GiB, and the texts, for the English labels across the full network alone (computed, [Costs](costs.md#labels-and-languages)).

**Where.** [Conditions the Engine Answers](querying.md#conditions-the-engine-answers).

### D17 – The substrate is recorded {#d17}

**Decision.** The graph records which module provides its arithmetic. `.import` refuses a second substrate; `.load` of a network recorded under a different substrate issues a warning and withdraws the reader of digits until the recorded substrate is imported.

**Why.** A stored expansion means a number exclusively within the substrate it was constructed on; today a session computes using incorrect digits and says nothing.

**Where.** [The Substrate](expansion.md#the-substrate).

## Names

### D18 – Core spellings come first {#d18}

**Decision.** A label that is the spelling for a core node is excluded upon import, and a core spelling is looked up before the names of any language.

**Why.** In published networks `~`, `!` and `nil` denote Wikidata items under `.lang en`. Exclusion ensures such names are omitted from regenerated networks; the lookup order repairs the existing ones.

**Where.** [Core Spellings](names.md#core-spellings).

### D29 – Which labels make names {#d29}

**Decision.** `rdfs:label` and `skos:prefLabel` serve as sources for names; `schema:name` does so exclusively for Wikidata entities. When multiple labels exist for a single language at one node, the name is selected based on rank, independent of the sequence in which the input appears.

**Why.** Outside Wikidata `schema:name` does not serve as a label predicate; data relying solely on `skos:prefLabel` must still get names; and two imports of the same data must name alike.

**Where.** [Which Predicates Make a Name](names.md#which-predicates-make-a-name).

### D30 – Fallback chains are data {#d30}

**Decision.** Each label language possesses an explicit, ordered fallback chain, read as written and never followed transitively. Each chain ends with `mul` unless otherwise declared. A minor manually authored module declares chains for a limited set of regional languages.

**Why.** Wikidata appends `mul` to each chain and relies on this mechanism. Explicit lists are the sole model compatible with its language codes; a table generated from MediaWiki’s files would raise a licensing question.

**Where.** [Fallback Chains](names.md#fallback-chains).

### D31 – `.name` looks at the target language only {#d31}

**Decision.** `.name` performs checks for a conflict within the language it names in. A newly assigned name hides a node that was previously accessible through the chain, and a note names that node.

**Why.** Merging via a fallback would reverse the separation between `mul` and `en`, and a merge is irreversible.

**Where.** [How a Name Is Found](names.md#how-a-name-is-found).

## Notation and Display

### D19 – What the re-read guarantee covers {#d19}

**Decision.** The guarantee covers the forms of the mapping, lists, and names within the language that holds them. Answer lines always present values in Turtle form; a registered display of a datatype is employed solely in explanations, diagrams, and reports.

**Why.** As initially formulated, the guarantee proved ineffective for every Wikidata item outside `.lang wikidata`. Expanding its scope alters the published output and constitutes an open decision.

**Where.** [The Re-Read Guarantee](notation.md#the-re-read-guarantee).

### D20 – The three forms {#d20}

**Decision.** A plain string is written `"Berlin"^^xsd:string`, while a quoted `"Berlin"` stays a name. An `<…>` form with a scheme and no blank is an IRI; a relative `<foo>` continues to be a character list. A blank node is written as `_:label`, valid within its script file or session, and displayed as `_:@` followed by sixteen hexadecimal digits.

**Why.** Each variant adheres to Turtle’s form or comes as near as zelph’s existing syntax allows, and none changes the meaning of a spelling employed by the repository.

**Where.** [Writing and Displaying Terms](notation.md).

### D32 – Four prefixes are predeclared {#d32}

**Decision.** `rdf:`, `rdfs:`, `xsd:` and `zelph:` are declared within every script. A condition answered by the engine checks the kind of its object and refuses a name when a literal or a datatype is expected.

**Why.** Without it, a rule typed under `.lang en` that spells `rdfs:label` or `zelph:lang` generates distinct English names and silently matches nothing.

**Where.** [Prefixes](notation.md#prefixes).

### D33 – Negative and decimal constants {#d33}

**Decision.** `&-5` and `&13.05` are constants, read via the normalization that the expanders apply. Within a term `(neg zint &5)` is printed `&-5` and a `dec` term as a decimal number.

**Why.** A rule must be able to name the numbers that literals expand to, across every session language. The display changes published output, which is accepted.

**Where.** [Constants](numbers.md#constants).

### D34 – The `turtle` block {#d34}

**Decision.** A script holds Turtle text within a `turtle` block that ends with `.end`, read by the reader that imports Turtle files.

**Why.** Only the forms of single terms are common across the two languages; a document belongs in a place where all of Turtle adheres to Turtle’s meaning.

**Where.** [The `turtle` Block](notation.md#the-turtle-block).

## Terms and Values

### D21 – `rdf:type` is not `~` {#d21}

**Decision.** `rdf:type` is an IRI node of its own. A module holds the optional bridge `(X rdf:type C) => (X ~ C)`.

**Why.** `~` carries the engine’s own declarations regarding predicates; RDF classes and these declarations must not mix within queries.

**Where.** [`rdf:type`](iris.md#rdftype).

### D22 – A time is a day and a second {#d22}

**Decision.** When a value carries a time of day, each bound of its interval is a day number and a second of that day; a value lacking an offset is read as UTC. The Julian Day Number stays the expansion of a day.

**Why.** Day numbers by themselves are insufficient to sequence two instants occurring on the same day, and an offset has the capacity to shift an instant to a different day.

**Where.** [The Model](dates.md#the-model).

### D23 – Three relations, and `=` means overlap {#d23}

**Decision.** On dates `<` means certainly before, `>` indicates certainly after, and `=` signifies that the intervals overlap. The three mutually exclude one another.

**Why.** At day precision, overlap is "possibly the same day" and with bounds that carry seconds, any alternative reading would let `<` and `=` hold together.

**Where.** [The Three Relations](dates.md#the-three-relations).

### D24 – How SPARQL compares {#d24}

**Decision.** SPARQL compares using order keys per datatype that a test holds to the standard library’s `cmp`. Decimals and doubles are compared exactly. A literal in a query is a value node. An IRI not present in the namespace table is looked up in the IRI language and subsequently as a name in the session language, never created.

**Why.** Evaluating via the rules of `cmp` leaves derived facts for each pair and requires approximately a quarter of a second per thousand pairs under the decimal substrate (measured, [Costs](costs.md#comparisons)); assessing via the host language’s doubles is incorrect when dealing with large numbers or dates.

**Where.** [SPARQL](querying.md#sparql).

### D36 – Comparison is also a condition {#d36}

**Decision.** The engine answers a numeric comparison as a condition within a rule, using a single comparator that orders every pair of numbers by value and that is shared by SPARQL’s order keys. `cmp` continues to serve as the semantics for rules that use it.

**Why.** A check across hundreds of millions of pairs cannot bear the cost of a derivation for each pair. The mathematics remains unaltered: it uses none of these conditions.

**Where.** [Comparing Numbers](numbers.md#comparing-numbers); the condition exists today ([Numeric Comparisons](../logic.md#numeric-comparisons)).

## Collections, Profile and Wikidata

### D27 – The longest well-formed end of a chain, converted in the graph {#d27}

**Decision.** A chain is converted from `rdf:nil` in reverse, extending as far as it remains well-formed. A cell marked with type `rdf:List` is tolerated, and the type statement is dropped. The conversion constitutes a step within the graph, identical for every reader, and is not a part of any reader.

**Why.** A single implementation serves all syntaxes while maintaining constant memory in the readers, and also handles chains that were written via scripts.

**Where.** [Which Chains Are Converted](collections.md#which-chains-are-converted).

### D28 – JSON-LD is in the profile {#d28}

**Decision.** The profile incorporates JSON-LD 1.1 via its transformation into RDF. Named graphs are handled according to the specifications of D3. A conformance run tracks each triple read, and triples where the predicate is identical to the object are counted and reported.

**Why.** A JSON-LD reader is included in the plan and needs conformance evidence; comparing two zelph graphs in isolation would not see a triple that both sides drop.

**Where.** [Profile and Conformance](conformance.md).

### D25 – Wikidata on two routes {#d25}

**Decision.** Values arrive at a network via Wikidata’s RDF or through the JSON dump, and each route yields the identical value node for the same value. Full values are attached as direct facts with rank indicators; the JSON route retains every rank, writes entity-valued statements of all types, and replicates the forms of Wikidata’s RDF.

**Why.** Intervals require full values, and the statement layer of Wikidata’s RDF would incur a cost ranging from three to four times that of the direct fact (computed, [Direct Full-Value Facts](wikidata-mapping.md#direct-full-value-facts)).

**Where.** [Wikidata under the Mapping](wikidata-mapping.md).

### D35 – One mapping for both routes, held by a test {#d35}

**Decision.** The JSON import emits the terms of Wikidata’s RDF via the code employed by RDF readers. A differential test over revision-pinned exports of the same entities in both formats holds the two routes together.

**Why.** Equal terms then are one node by construction, and the test makes the remaining requirement – equal terms – executable.

**Where.** [Wikidata under the Mapping](wikidata-mapping.md).

### D26 – The prime check has a bound {#d26}

**Decision.** The check of primality claims tests the numbers under a bound, widened by rules that perform division exclusively by primes.

**Why.** Primality through rules involves a derivation for each divisor; without a bound, the check is unable to conclude.

**Where.** [Primes among the Number Items](checks.md#primes-among-the-number-items).

## The Engine

### D37 – The mathematics does not notice {#d37}

**Decision.** A session that uses no feature of the mapping performs precisely the same work as it does today, verified via the Jacobian workload counter for counter. The engine properties required by the mapping were constructed prior to its development.

**Why.** Performance decides whether the engine is usable for its first purpose; a mapping that slowed down comparison or the reference workload would not be acceptable.

**Where.** [Engine Requirements](engine.md).
