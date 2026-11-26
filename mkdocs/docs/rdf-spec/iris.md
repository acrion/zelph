# IRIs and the Namespace Table

RDF identifies a resource via an IRI. zelph identifies a node through a name within a naming language. This page specifies how the two meet: each IRI is a named node, within a naming language of its own unless a table maps its namespace onto names already used by zelph networks. Of this page, the positions a blank node takes in a statement are implemented for the statements written by a script, a rule, and a program; everything else is specified but not yet implemented. A sentence about the present engine says "today".

## An IRI Is a Named Node

An IRI becomes an **IRI node**: a standard named node, an atom bearing a counter id like every node a script creates by typing a name. What is new is solely the location where its name lives.

- **The IRI language.** zelph gains an additional naming language, `<iri>`. A node for which no table row applies is named by its full IRI in that language: `http://example.org/alice` is the node that carries the name `http://example.org/alice` in `<iri>`. The name of the language is selected to ensure it cannot ever be a language tag, since a language tag names a [label language](names.md#label-languages).
- **The namespace table.** When a row in the [namespace table](#the-namespace-table) applies to the namespace of an IRI, the IRI is the node that carries a more concise name in a different naming language. `http://www.wikidata.org/entity/Q42` is the node named `Q42` in the language `wikidata`, the identical node that the JSON import of Wikidata creates today, and `http://www.wikidata.org/prop/P2738` is the node named `p:P2738` in that context.

The table enables two distinct worlds to share their nodes. An import of Wikidata’s RDF is placed upon the nodes of a network built from the JSON dump, and a script that names items by their identifiers operates on both.

IRIs are compared character by character, just as RDF 1.1 does: no case folding, no percent-decoding, no additional normalization. A reader resolves a relative IRI against its base before mapping the outcome, ensuring that only absolute IRIs ever arrive at the graph; the notation for scripts accepts absolute IRIs exclusively ([Writing and Displaying Terms](notation.md#iris)).

An IRI node carries no special id class. It needs none: every existing graph reader processes it as the named atom it is, and its identity is its name. Two networks constructed in isolation assign different ids to the same IRI, just as they assign distinct ids to every named node; a `.bin` stores the names, and a network reloaded later resolves the same IRI to the same node.

Naming paths guard the IRI language: `.name`, `zelph/name`, and every other path refuse a name in `<iri>` that is not an absolute IRI. An IRI node may carry additional names, such as a label in `en`; these are names assigned to the same node.

## The Namespace Table

Each row within the table consists of four parts:

| Part | Meaning |
|---|---|
| namespace | an IRI prefix, matched against the beginning of an IRI |
| naming language | the language in which the name is formed; never a label language |
| name prefix | the initial segment of the name; may be empty |
| pattern | the local names that the row accepts |

An IRI is mapped through three steps:

1. **The longest namespace takes precedence.** From among the rows where the namespace is a prefix of the IRI, the one with the longest namespace is selected, regardless of the order of the rows. `http://www.wikidata.org/entity/statement/Q42-…` therefore belongs to the row of statement nodes rather than to the row of entities.
2. **The local name must be compatible.** The rest of the IRI behind the namespace is the local name. If it matches the row’s pattern, the node is the one named by the combination of name prefix and local name within the row’s language.
3. **Otherwise, the IRI language applies.** In the absence of a matching row, or when the local name is rejected by the row, the node is the one named by the full IRI in `<iri>`. The same holds whenever the resulting name would be empty or correspond to the spelling of a core node (`in`, `cons`, `nil`, `~`, and others): an IRI never lands on a core node.

Step 2 is responsible for maintaining separation among IRIs. `http://www.wikidata.org/entity/p:P2738` fails to conform to the entity id pattern, thus it does not become the name `p:P2738`, which instead belongs to `http://www.wikidata.org/prop/P2738`.

Today the table is present solely within `stdlib/sparql.zph`, as two lists that the SPARQL front end examines, with the first matching entry taking precedence: `http://www.wikidata.org/entity/statement/X` becomes the name `statement/X`. An IRI not found within the lists becomes a name in the current session language, spelled identically to the IRI. The specified table replaces both lists. It lives in the standard-library module `rdf`, which `sparql`, `turtle`, and the command `.import-rdf` import, and it registers its rows with the engine, ensuring that the C++ readers, the script parser, and the SPARQL front end all map every IRI through the same set of rows.

### The Rows

The `rdf` module registers these rows. Wikidata’s rows use the prefixes from Wikidata’s native Turtle output as name prefixes, ensuring the names are what a reader of Wikidata’s documentation would anticipate, and match the ones the qualifier layer already writes.

| Namespace | Language | Name prefix | Pattern |
|---|---|---|---|
| `http://www.wikidata.org/entity/` | `wikidata` | (empty) | an entity identifier: `Q…`, `P…`, `L…`, `L…-F…`, `L…-S…`, `E…` |
| `http://www.wikidata.org/prop/direct/` | `wikidata` | (empty) | a property identifier |
| `http://www.wikidata.org/prop/direct-normalized/` | `wikidata` | `wdtn:` | a property identifier |
| `http://www.wikidata.org/prop/` | `wikidata` | `p:` | a property identifier |
| `http://www.wikidata.org/prop/statement/` | `wikidata` | `ps:` | a property identifier |
| `http://www.wikidata.org/prop/statement/value/` | `wikidata` | `psv:` | a property identifier |
| `http://www.wikidata.org/prop/statement/value-normalized/` | `wikidata` | `psn:` | a property identifier |
| `http://www.wikidata.org/prop/qualifier/` | `wikidata` | `pq:` | a property identifier |
| `http://www.wikidata.org/prop/qualifier/value/` | `wikidata` | `pqv:` | a property identifier |
| `http://www.wikidata.org/prop/qualifier/value-normalized/` | `wikidata` | `pqn:` | a property identifier |
| `http://www.wikidata.org/prop/reference/` | `wikidata` | `pr:` | a property identifier |
| `http://www.wikidata.org/prop/reference/value/` | `wikidata` | `prv:` | a property identifier |
| `http://www.wikidata.org/prop/reference/value-normalized/` | `wikidata` | `prn:` | a property identifier |
| `http://www.wikidata.org/prop/novalue/` | `wikidata` | `wdno:` | a property identifier |
| `http://www.wikidata.org/entity/statement/` | `wikidata` | `wds:` | letters, digits, `_` and `-` |
| `http://www.wikidata.org/reference/` | `wikidata` | `wdref:` | lowercase hexadecimal digits |
| `http://www.wikidata.org/value/` | `wikidata` | `wdv:` | lowercase hexadecimal digits |
| `http://wikiba.se/ontology#` | `wikidata` | `wikibase:` | letters and digits |
| `https://www.wikidata.org/wiki/Special:EntityData/` | `wikidata` | `data:` | an entity identifier |
| `https://zelph.org/ns/wikidata/full-value/` | `wikidata` | `fv:` | a property identifier |
| `https://zelph.org/ns#` | `zelph` | `zelph:` | letters, digits and `-` |

An entity id is one of the letters followed by a number that lacks a leading zero, and for a form or a sense of a lexeme additionally `-F` or `-S` followed by such a number; a property id is `P` followed by such a number. The last two rows are zelph’s own. The row labelled `fv:` names the predicates of direct full-value facts ([Wikidata](wikidata-mapping.md)); the row labelled `zelph:` gives the engine’s vocabulary an IRI, enabling a comparison such as `zelph:lt`, which exists today under exactly that name, and the specified `zelph:lang` or `zelph:value` to be written as IRIs and through the predeclared prefix `zelph:` in any session language. The whole prefix is reserved for the engine: a script does not introduce any name of its own under `zelph:`, ensuring that a name later added by a new version – such as the condition via which rules over rules will read the conditions of a rule – cannot be assigned to a node created by a script.

Every vocabulary lacking a row keeps its full IRIs: `rdf:`, `rdfs:`, `xsd:`, `owl:`, `skos:`, `schema:`, `prov:`, `geo:` and any additional ones a source uses are nodes within the IRI language.

A module registers a row via `(zelph/namespace namespace language name-prefix pattern)`. The language assigned to a row is reserved by this registration: no label may become a name within it ([Names](names.md#labels-that-do-not-become-names)).

### Statement Nodes

Wikibase names the node of a statement based on the statement’s id, substituting every character that is not a letter, a digit, `_`, or `-` with `-`: the JSON id `Q42$F078E5B3-…` becomes `wds:Q42-F078E5B3-…` in RDF. Wikibase does not ensure any specific id format, the case of the first letter varies (`wds:q42-…` occurs), and the replacement cannot be undone with certainty. Therefore, the RDF form is the canonical name: the row accepts every local name composed of the permitted characters exactly as provided, preserving its case, while the JSON route applies Wikibase’s replacement before naming a statement node.

Today, the qualifier layer of the JSON import names statement nodes after the JSON id, with `$`. Networks that hold the layer are regenerated with the release introducing the mapping, ensuring no network keeps the old names ([Wikidata](wikidata-mapping.md)).

### Two Namespaces, One Name

Two rows map onto the bare id: `wd:P31` (`…/entity/P31`) and `wdt:P31` (`…/prop/direct/P31`) are both the node `P31`. That is intended: a network built from the JSON dump features a single node per property, used as a predicate and described as an entity, with scripts naming it `P31`.

What gets forfeited is the namespace previously utilized by the source. A writer recovers it from the node’s location:

- as the predicate in a statement, as the object of `wikibase:directClaim` or `owl:onProperty`, and as the subject of `rdf:type owl:ObjectProperty` or `rdf:type owl:DatatypeProperty`, the node of a property id is written `wdt:`;
- in all other contexts it is written `wd:`.

This reproduces every use of `wdt:` that Wikibase itself writes. A source that uses `wdt:P31` in a different role, for example as the subject of a statement it makes, comes back as `wd:P31`; that loss is accepted and stated herein.

No pair of rows other than the one in question shares names. For each remaining row, the reverse direction is unique: a name in the language with the row’s prefix equals the row’s namespace concatenated with the rest of the name. `zelph/iri-of` answers this canonical IRI for a node, and yields no result for a node that lacks one.

### What a Network Records

The rows via which at least one node of a network was mapped are preserved within the network and saved in its `.bin` ([Storage](storage.md#records-in-the-header)). A loaded network therefore carries its rows along, and a session whose module `rdf` registers an additional row for a recorded namespace receives notification only once; the recorded row remains active for that network, since the same IRI must continue to denote the same node.

## `rdf:type`

`rdf:type` is an IRI node just like any other: the node corresponding to `http://www.w3.org/1999/02/22-rdf-syntax-ns#type`. A reader writes `X rdf:type C` as a standard fact using that predicate, including Turtle’s `a`, and all mechanisms that depend on a node’s class depend on these facts: the [class expanders](expansion.md#expanders), the rule that tells a sitelink’s title from a label ([Names](names.md#which-predicates-make-a-name)), and the `wdno:` classes of Wikidata.

It is deliberately not zelph’s own `~`. The core relation `~` carries the engine’s declarations: every predicate gets the fact `P ~ ->` with its first statement, so a query `X ~ Y` over imported data would answer those declarations in addition to the classes of the data, and an unnamed subject of a `~` statement appears as its class. A session aiming for RDF classes to take part in rules written over `~` imports the module `rdf-types`, which holds the single bridging rule:

!!! example "Specified – not yet implemented"

    ```
    (X rdf:type C) => (X ~ C)
    ```

Without that import, the two relations remain separate.

## Statements

One RDF triple becomes one zelph fact having precisely one object. A reader never gathers the objects from multiple triples into a single fact, regardless of the syntax offered (an object list in Turtle, an array in JSON-LD), since a fact with several objects is a different statement in zelph and marks its predicate as one that may hold wider statements, incurring a cost for every later match on it.

What may occupy a position:

| Position | IRI node | Blank node | Value node |
|---|---|---|---|
| subject | affirmative | affirmative | not derived from a reader (RDF 1.1 does not permit literal subjects) |
| predicate | affirmative | refused; a firing results in the statement remaining unwritten | refused |
| object | affirmative | affirmative | affirmative |

A triple having a blank node as its predicate is not RDF 1.1; a reader encountering one in a generalized source counts it and refrains from writing it. A typed statement with a blank node as its predicate is refused with an error, just as is a call to a Janet, C, or Rust function that would write one. A rule whose firing would write one, whether at the top level of a consequence or nested within it, causes that consequence to stay unwritten while its other consequences are written; this is not a contradiction, and the run names the rule and the blank node in a note at its end, once in a session ([Blank Nodes](blank-nodes.md#a-firing-that-would-make-one-a-predicate)). A triple **whose predicate is also its object**, for instance `P1269 P1659 P1659`, cannot be written either: zelph refuses such a statement today, because a fact is identified by its predicate, making it impossible for one node to serve as both the predicate and an object within the same statement. Every route counts such triples and reports them at the end of the import rather than writing them ([Importing RDF](import.md#the-import-report)); Wikidata’s truthy data held twelve of them on 4 November 2026 (counted on the Wikidata Query Service).

## Writing, Display and Interfaces

- **In a script** an IRI is written as in Turtle, `<http://example.org/alice>`, or as a prefixed name where the prefix has been declared by the script or is predeclared (`rdf:`, `rdfs:`, `xsd:`, `zelph:`). A prefixed token whose prefix is not declared stays the name it is today, so `wd:Q42` typed under `.lang wikidata` continues to name the node it names now ([Writing and Displaying Terms](notation.md#iris)).
- **On display** a node named solely in the IRI language prints as `<IRI>`, or using one of the predeclared prefixes, in every session language; a node that additionally carries a name in a label language is rendered according to the order in which names are displayed ([How a Name Is Found](names.md#how-a-name-is-found)). A node that the table maps onto a name is a named node and prints as named nodes do ([Display](notation.md#display)).
- **From Janet, C and Rust** `zelph/iri`, `zelph_iri` and `Engine::iri` find or create the node corresponding to an IRI via the table; `zelph/iri-of` answers the canonical IRI of a node ([Programming Interfaces](storage.md#programming-interfaces)).
- **In rules and queries** `X zelph:lexical L` assigns `L` the full IRI of an IRI node as a string, and a pattern tells an IRI from a string with identical characters based on the node’s kind ([Querying Terms](querying.md#an-iri-or-a-string)).

## What the Graph Preserves

| Of the IRI | Kept | Remark |
|---|---|---|
| The IRI, character by character | yes | as the name within `<iri>`, or as namespace combined with local name via the row |
| `wd:` versus `wdt:` | by position | refer to [Two Namespaces, One Name](#two-namespaces-one-name) |
| A relative form and the base it was resolved against | no | only the absolute IRI arrives at the graph |
| A prefix from the source and its declaration | no | syntax |
| A resource that only labels describe | until `.cleanup` | a name is not a fact: a node lacking facts is removed by `.cleanup` alongside its names |
