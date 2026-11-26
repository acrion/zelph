# RDF Terms in zelph

RDF identifies a resource through an IRI. It sets blank nodes apart from all other terms and IRIs apart from literals, and it distinguishes literals based on their datatype and language tag. zelph identifies a node via a name within a naming language. The two models do not align with one another inherently. This section specifies how they are reconciled: for each type of RDF term, which zelph node it becomes.

Of this, blank nodes are implemented within the engine: their id class, the derivation of their ids, the forms `_:b0` and `_:@` with the digits of an id, the identity of a script file and that of a session, the action a firing performs when a blank node would serve as a predicate, and the associated functions ([Blank Nodes](blank-nodes.md)). Likewise implemented are the function that answers the kind of a node and the cap on the node counter set at 2^58 ([Storage](storage.md)). Everything else has not been implemented yet. The readers for the RDF syntaxes will be constructed based on these pages, and the engine will be enhanced with literal values as described. The pages are written so that anyone can derive what an import produces, and so that the developers constructing it can locate every name, form, and rule they need.

## The Mapping at a Glance

| RDF term | Becomes | Identity | In a script | Stored |
|---|---|---|---|---|
| IRI | a named node, in the IRI language `<iri>` or via the namespace table in an existing language | its name | `<http://example.org/a>`, a declared prefix | as every named node |
| literal | a value node | datatype, lexical form and tag | `"1952-03-11"^^xsd:date`, `"Berlin"@de`, `"Berlin"^^xsd:string` | in a value store of its own |
| label | a name of its resource | the name | read via `rdfs:label` | as every name |
| blank node | a blank node | document and label | `_:b0` | as an id |
| collection | a zelph list | its elements | `<a b c>` | as the facts of the list |
| triple | one fact with one object | its parts | `s p o` | as every fact |
| graph name | nothing; counted | – | – | – |

Each row has a page: [IRIs](iris.md), [literals](literals.md), [labels](names.md), [blank nodes](blank-nodes.md), [collections](collections.md), [statements](iris.md#statements), and [named graphs](import.md#named-graphs). Each of these pages says what the node is, what determines when two terms refer to the same node, how the node is displayed and stored, and what the graph retains from the term that a reader met.

## What Is Wrong Today

zelph today can hold values exclusively as names or as character lists, and both methods fail:

- A value stored as a name collides with anything else spelled identically. The string `"Q5"` of a Wikidata qualifier is the item `Q5`, and a value written as `nil` or `in` resolves to a core node.
- A string possesses no independent identity. As a character list, `"123"` is the node of the numeral `&123` under decimal arithmetic.
- SPARQL compares numbers using the floating-point numbers of the host language and treats all other data as text, hence dates are ordered as strings and large numbers that differ are considered equal.
- The documentation refers to relationships between computed numbers and the numbers within a knowledge base that have not yet materialized ([Semantic Math](../logic.md#semantic-math-computation-as-graph-rewriting)). The mapping enables these connections, upon request and through rules.

## Seven Principles

1. **A literal is a value node**: a node of a kind of its own, where identity is determined by its datatype, precise lexical form, and tag, in accordance with RDF’s definition of term equality. It is never an entity, a name, or a core node, and its text is stored only once and in a compact manner ([Literals as Value Nodes](literals.md); a label is the one exception, [Labels Are the Exception](literals.md#labels-are-the-exception)).
2. **A datatype is a node, and the meaning of a value is registered for it**: a module registers an expander that turns a value into ordinary graph structure and links it to the value node through a regular statement ([Expanding Values on Request](expansion.md)).
3. **The same mechanism reads names**: a name reader reads the name of selected nodes as a value and associates the node with the same structure, without performing any merging ([Name Readers](expansion.md#name-readers)).
4. **Numbers and dates are numbers of the standard library**, and their comparisons are those defined by the library. There is no dedicated date type ([Numbers](numbers.md)).
5. **A date is the interval of days it may represent**, expressed as Julian Day Numbers, and with the second of the day where a value carries a time, across all precision levels Wikibase defines ([Dates and Intervals](dates.md)).
6. **A string is a value node**, and its character list is an expansion identical in nature to any other ([Strings](expansion.md#strings)).
7. **Blank nodes are anonymous nodes whose scope is confined to a document** ([Blank Nodes](blank-nodes.md)).

The choices underpinning the principles, each with its reason, are listed on [Design Decisions](decisions.md); what remains to be decided, on [Open Decisions](open-decisions.md).

## A Session

The session below shows the mapping in action across three triples. It is specified behaviour.

!!! example "Specified – not yet implemented"

    ```
    zelph> .import decimal-arithmetic
    zelph+> .import turtle
    zelph> turtle labels=de
    @prefix ex: <http://example.org/> .
    @prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
    @prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .

    ex:tower rdfs:label "Fernsehturm"@de ;
             ex:height "368.03"^^xsd:decimal ;
             ex:opened "1969-10-03"^^xsd:date .
    .end
    Imported 3 triples: 2 facts written, 1 name assigned.
    zelph> .lang de
    de> Fernsehturm P O
    Answer: Fernsehturm <http://example.org/height> "368.03"^^xsd:decimal
    Answer: Fernsehturm <http://example.org/opened> "1969-10-03"^^xsd:date
    de> X rdfs:label L
    Answer: Fernsehturm rdfs:label "Fernsehturm"@de
    de> <http://example.org/height> zelph:expand-objects zelph:value
    de> Fernsehturm <http://example.org/height> V, V zelph:value N
    Answer: Fernsehturm <http://example.org/height> "368.03"^^xsd:decimal, "368.03"^^xsd:decimal zelph:value &368.03
    de> (X <http://example.org/height> V, V zelph:value N, N zelph:gt &300) => (X ~ landmark)
    ...
    de> X ~ landmark
    Answer: Fernsehturm ~ landmark
    ```

- The [`turtle` block](notation.md#the-turtle-block) holds a Turtle document. Its three triples become two facts and a name: the label is the name of the node in the language `de` and no statement ([Names](names.md)).
- The two literals are value nodes. They are printed as they are written, and a line that zelph prints is read back as the identical statement ([Display](notation.md#display)).
- The label remains accessible as a literal. `X rdfs:label L` is answered from the names ([Labels](querying.md#labels)).
- No expansion occurs before a [request](expansion.md#requests) asks for it. Here, a standing request asks for the numbers of all heights; the value `368.03` is then the number `&368.03`, the term `(&36803 dec &2)`.
- A rule compares the number using the engine’s [numeric comparison](../logic.md#numeric-comparisons) and derives an ordinary fact.

On a large scale, the same procedures enable validations on Wikidata that zelph cannot run today: a date of death occurring before the date of birth, a work that cites a later one, a number item whose assertion of being prime is wrong ([Three Checks on Wikidata](checks.md)).

## The Pages of This Section

| Page | Holds |
|---|---|
| [IRIs and the Namespace Table](iris.md) | IRI nodes, the IRI language, the mapping table that links namespaces to existing names, `rdf:type`, statements |
| [Literals as Value Nodes](literals.md) | identity, the value store, refusals, datatypes |
| [Blank Nodes](blank-nodes.md) | document identity, ids, blank nodes within scripts |
| [Collections and Lists](collections.md) | the process by which a collection becomes a list, and the implications of a list being a value |
| [Writing and Displaying Terms](notation.md) | the forms used in a script, what changes for existing scripts, display, the `turtle` block |
| [Labels, Names and Languages](names.md) | labels as names, core spellings, shared labels, `mul` and fallback chains |
| [Expanding Values on Request](expansion.md) | expansion statements, expanders, name readers, requests, strings, the substrate |
| [Numbers](numbers.md) | the decimal family, floating-point types, constants, comparison |
| [Dates and Intervals](dates.md) | day numbers, the XSD types, Wikibase’s precision levels, the three relations |
| [Querying Terms](querying.md) | rules, native queries and SPARQL over literals, labels and lists |
| [Importing RDF](import.md) | the import command, its parameters and report, named graphs, JSON-LD |
| [Wikidata under the Mapping](wikidata-mapping.md) | the worked example: every kind of Wikidata value on both routes |
| [Three Checks on Wikidata](checks.md) | the three-check rules, executed on today’s engine |
| [Costs](costs.md) | the memory and time costs of the mapping, each figure accompanied by its basis |
| [Profile and Conformance](conformance.md) | RDF 1.1, the W3C test suites and the criteria used to evaluate a test |
| [Storage and Programming Interfaces](storage.md) | ID classes, the `.bin` file, the Janet, C and Rust functions |
| [Engine Requirements](engine.md) | what the engine provides, what must remain unchanged, the tests |
| [Design Decisions](decisions.md) | every decision paired with its reason |
| [Open Decisions](open-decisions.md) | what remains unresolved, including alternatives and associated costs |

## How to Read These Pages

**Two kinds of transcripts.** A code block enclosed in a box titled "Specified – not yet implemented" shows behaviour that this specification requires and that no implementation currently fulfils. A code block featuring zelph prompts, situated outside such a box, is a real run of the present zelph: it shows what the engine does today – a capability that the mapping builds on, the behaviour it supersedes, or a part of the mapping that is implemented. Both follow the convention outlined in the [Quick Start Guide](../quickstart.md#basic-usage): whatever comes after a prompt is input, and a line of `...` denotes omitted lines. The wording of a message within a specified session serves as an example; its content is binding.

**Today, implemented, and specified.** Running text marks the current engine using "today". A part of the mapping that the engine already implements indicates "implemented", either in the opening paragraph of its page or at its current location. All other content on these pages represents specified behaviour.

**Figures.** A number on these pages is accompanied by its basis and says whether it is computed or measured; a tally retrieved from Wikidata names its source and its date.

## Relation to the NLnet Project

This specification is the first task of the project funded by [NLnet funds](../nlnet-status.md). Subsequent tasks build from it: a streaming reader for N-Triples and N-Quads, a Turtle reader, blank nodes as a pool of anonymous nodes, literals as terms, a JSON-LD reader, and a conformance report over the W3C test suites.

One of these tasks is "literals as terms rather than as names", and labels become names regardless. The two align: every literal is a term, a value node, except for the specific kind that zelph has always employed as its names and that scripts rely on, and that kind can be read as a term wherever a rule or a query asks for it ([Labels Are the Exception](literals.md#labels-are-the-exception)).
