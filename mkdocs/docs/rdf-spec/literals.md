# Literals as Value Nodes

An RDF literal becomes a node of a distinct kind: a **value node**. This page specifies what such a node is, what determines when two literals are the same node, where its text is kept, what actions the engine refuses to perform on it, and what aspects of the literal a reader met the graph preserves. Nothing on this page has been implemented yet; a sentence regarding the present engine says "today".

## What a Value Node Is

A value node stands for a single literal: a lexical form, a datatype, and, for a language-tagged string, a language tag. It is an atom, akin to a named node, so every statement may hold it as a subject or as an object, and a rule matches it in the same manner as it matches any other atom. It differs from a named node in three ways:

- It carries no name. Its text is not an entry in any naming language, so it can never be found by typing a name, and it can never collide with an entity, a core node, or a word from a script.
- Its identity is its content. The same datatype, lexical form, and tag always point to the same node within a network, regardless of who creates it: a reader, a script, a rule, or a host program.
- Its text is stored once and compactly, in a dedicated store: not as a name, and not as a structure of facts.

Today zelph lacks such a node. A value is either a name or a character list, and each carries a defect that the value node removes:

- A value stored as a name collides with any other node sharing that spelling. The qualifier layer in the Wikidata import stores values as names within the language `wikidata`, so the string `"Q5"` of a qualifier is the item `Q5`, and a value spelled `nil`, `in`, or `cons` lands on the core node associated with that spelling.
- A string kept as a character list lacks independent identity. Under `decimal-arithmetic`, the character list of `"123"` is the node of the numeral `&123`, and under `binary-arithmetic`, the character list of `"101"` is `&5`.

Following the mapping, a string and a number are two value nodes. What they stand for within ordinary graph structure, their [expansion](expansion.md), might still coincide; the value nodes themselves never do.

## Identity

Two literals are the same node exactly when they are identical RDF terms according to RDF 1.1, with language tags written in lowercase as RDF 1.1 permits: their datatype IRIs are equal character by character, their lexical forms are equal character by character, and, for language-tagged strings, their tags are equal after being converted to lowercase.

- `"01"^^xsd:integer` and `"1"^^xsd:integer` are two distinct nodes. They share the same expansion, the numeral `&1`, and a rule comparing values compares the expansions ([Numbers](numbers.md)).
- A simple literal is an `xsd:string`, as defined by RDF 1.1: `"abc"` in Turtle and `"abc"^^xsd:string` are a single node.
- A language-tagged string carries the datatype `rdf:langString`. Its tag is converted to lowercased form upon node creation, a practice allowed by RDF 1.1, so `"Berlin"@DE` and `"Berlin"@de` are one node, and `zelph:lang` always answers the lowercase tag. This is the sole normalization applied to a value node.

The lexical form is never brought into a canonical form at the time of node creation. Whether `01` and `1` are the same value is information tied to a datatype, and what a session knows about a datatype depends on what it has registered: selecting a canonical form during creation would cause a node’s identity to rely on the modules a session happened to import. It would also depart from RDF, where those two are different terms that denote a single value.

**Interning.** A value node is located via its content using a content index, just as a named node is located through its name using the name index, and it is created when the index holds no matching entry. Its id is the id class assigned to value nodes combined with a sequence number taken from a counter specific to it ([Storage](storage.md#id-classes)). The id consequently does not include a hash: two different literals can never result in a collision, no hash function is frozen into the files, and there is no scheme to version. What follows from this:

- Within a single network, the same content is always the same node, across all graphs and in every session that loads it.
- The numeric id of a literal differs between two networks constructed independently, just as the id of a named node does. Nothing in zelph relies on equal ids across networks: a `.load` replaces the graph, and a program seeking the node of a literal requests it by content.
- The content index is not saved. It is reconstructed upon loading a network, as the name index is.

## The Value Store

The texts live in a **value store** beside the graph:

- One record per value node, stored in a dense array indexed by the node’s sequence number: the position of its text encoded in 48 bits and its length in 32, the index of its datatype and the index of its tag each in 24 bits. A record takes 16 bytes. These widths bound a lexical form to less than 2^32 bytes, the texts of a network to 2^48 bytes, and a network to 2^24 datatypes and an equal number of tags; an import that would surpass any of these limits is refused with a message.
- A text arena that holds every lexical form exactly once, as bytes with an explicit length. A lexical form may therefore contain any Unicode character, including U+0000, and every layer that hands a lexical form along does so with its length: the `.bin` format, the Janet functions, the C functions with a pointer and a length, the Rust functions with a slice.
- A table of datatype IRIs and a table of language tags, enabling a record to name its datatype and its tag via a small index. A datatype is identified by the text of its IRI here and in every registration ([Datatypes](#datatypes)).
- The content index: a set of value-node ids whose hash and equality operations read datatype, tag, and lexical form from the store. It grows without copying its entries and is rebuilt with an exact reserve upon loading a network.

The adjacency of a value node, the facts it participates in, is stored within a second dense array sharing the same index rather than in the node map that holds every other node. Two outcomes arise. A value node costs a fraction of the cost of a named node ([Costs](costs.md)), and literal values never fill the node map, whose bucket array expands by doubling in size in a single step once it is full enough. A node lookup tells the two apart based solely on the id class.

The store and the index remain untouched when a graph holds no literal: a session utilizing no RDF feature performs no work for them ([Engine Requirements](engine.md)).

## What the Engine Refuses

A value node remains as defined in the first section, since the engine refuses any action that would alter its nature. Each refusal is a test of the id class on the branch that creates or changes a node:

- **A name.** `.name`, `zelph/name`, and any other naming path refuse attempts to assign a name to a value node, and no label import assigns one to it.
- **A merge.** A value node is never merged with another node, whether due to a name clash or through any other route.
- **The predicate position.** A statement with a value node in its predicate role is refused by the parser, by `zelph/fact`, by `zelph_fact` and by every reader. In RDF, a predicate must be an IRI.
- **A statement about its text.** `zelph:lang`, `zelph:datatype`, and `zelph:lexical` are conditions that the engine answers from the store ([Querying Terms](querying.md#conditions-the-engine-answers)). When written as a statement or as the consequence of a rule, they are refused, ensuring that no stored fact can contradict the store.

A reader never writes a literal as the subject of a triple, since RDF 1.1 provides no such thing there. zelph itself does so: the statement that links a value node to its expansion has the value node as its subject.

## Datatypes

A datatype is a node: the [IRI node](iris.md) corresponding to the IRI of the datatype. It is a standard node, so a statement can make claims about `xsd:date`, and `V zelph:datatype D` binds `D` to that node. The engine creates the node of a datatype alongside the first value node that carries the datatype, not at initialization.

What a session knows beyond that is registered, through the text of the datatype’s IRI: a lexical check, an [expander](expansion.md#expanders) that constructs ordinary graph structure from a value upon request, an order key for SPARQL ([Querying Terms](querying.md#comparing-and-ordering)). A datatype is considered **recognized** once an entry is registered for it. The table lists what the modules of the standard library register; the last column names the page containing the details.

| Datatype | Lexical check | Expansion on request | Page |
|---|---|---|---|
| `xsd:string`, `rdf:langString` | none; the character-list expander checks XML 1.0 characters | character list (`zelph:chars`) | [Expansion](expansion.md#strings) |
| `xsd:normalizedString`, `xsd:token`, `xsd:language`, `xsd:Name`, `xsd:NCName`, `xsd:NMTOKEN` | the type’s lexical space, exactly | character list | [Expansion](expansion.md#strings) |
| `xsd:decimal`, `xsd:integer` and the twelve integer types derived from it | the type’s lexical space and value range | number (`zelph:value`) | [Numbers](numbers.md#the-decimal-family) |
| `xsd:double`, `xsd:float` | the type’s lexical space | exact number or special value (`zelph:value`) | [Numbers](numbers.md#doubles-and-floats) |
| `xsd:dateTime`, `xsd:dateTimeStamp`, `xsd:date`, `xsd:gYearMonth`, `xsd:gYear` | the type’s lexical space | interval (`zelph:interval`) | [Dates](dates.md) |
| `xsd:boolean` | `true`, `false`, `1`, `0` | none | [Querying Terms](querying.md#comparing-and-ordering) |
| `xsd:time`, `xsd:gMonth`, `xsd:gDay`, `xsd:gMonthDay` | none | none: a recurring value has no interval of days | [Dates](dates.md#types-without-an-interval) |
| `xsd:duration`, `xsd:yearMonthDuration`, `xsd:dayTimeDuration` | none | none; left to a later module | – |
| `xsd:hexBinary`, `xsd:base64Binary` | none | none; left to a later module | – |
| `xsd:anyURI` | none | none; such a literal is a value node and never passes the [namespace table](iris.md#the-namespace-table) | – |
| `rdf:XMLLiteral`, `rdf:HTML`, `rdf:JSON` | none | none | [Conformance](conformance.md) |
| `geo:wktLiteral`, MathML and the other datatypes of Wikidata | none | per datatype | [Wikidata](wikidata-mapping.md) |
| any other IRI | none | none | – |

For each row, three rules are in effect:

- **An ill-typed literal is a value node just like any other.** RDF 1.1 mandates that an implementation must accept a literal whose lexical form lies beyond the lexical space of its datatype. A reader never rejects one. It only gets no expansion: an expansion request counts it as ill-typed, per datatype, beside the values it expanded and those without an expander.
- **An unknown datatype is accepted.** Its values are value nodes, written and presented using the full IRI of the datatype.
- **A literal typed `rdf:langString` lacking a tag is not an RDF 1.1 term.** Every reader and the script notation refuse it as a syntax error.

## Writing, Display, Storage and Interfaces

The remaining pages hold these; in short:

- **In a script** a literal is written as in Turtle: `"1952-03-11"^^xsd:date`, `"Berlin"@de`, and `"Berlin"^^xsd:string` for a plain string, since a quoted `"Berlin"` on its own remains the name it is today ([Writing and Displaying Terms](notation.md#literals)).
- **On display** a value node always prints in that form, including its escapes, ensuring the printed term reads back as the same node in every session language ([Display](notation.md#display)).
- **In a `.bin`** the value store is a distinct section, and the header saves the counter of sequence numbers ([Storage](storage.md#the-value-section)).
- **From Janet, C and Rust** a value node is created or found using `zelph/literal`, `zelph_literal` and `Engine::literal`, and taken apart using `zelph/literal-parts`, `zelph_literal_parts` and `Engine::literal_parts` ([Programming Interfaces](storage.md#programming-interfaces)).

## Labels Are the Exception

One kind of literal does not become a value node by default. A label, a literal that a label predicate such as `rdfs:label` attaches to a resource, becomes a **name** of that resource in the naming language of its tag, just as the JSON import of Wikidata makes names of labels today ([Labels, Names and Languages](names.md)). The node count of an import therefore does not grow with its labels, and scripts that address nodes by their labels continue to function. A rule or a query still reads such a label as a literal: the condition `X rdfs:label L` is answered from the names, and `L` is bound to the value node the label would be ([Querying Terms](querying.md#labels)). A label that cannot become a name stays a value node with a standard statement, just as every description and every alias does.

## Clusters

Every node generated during a cluster’s active phase is recorded by the cluster, and discarding it eradicates those nodes alongside every fact that names them. A value node is found through its content, so a node initially created within a scratch cluster may be reused beyond it later. Whether a node that is rediscovered from outside the cluster that originally recorded it leaves that record remains an [open decision](open-decisions.md#clusters-and-nodes-found-by-content); this applies equally to numerals and lists and is advised in that context.

## What the Graph Preserves

For a literal encountered by a reader, the graph retains all information a writer would need to write the identical RDF term again, and loses what belongs to the surrounding syntax.

| Of the literal | Kept | Remark |
|---|---|---|
| Lexical form | every character | no trimming, no Unicode normalization, no canonical form |
| Datatype IRI | in full | whether the source used a prefix is not kept |
| Language tag | in lowercase | the original case is lost; RDF 1.1 compares tags after lowercasing in their value space |
| `"abc"` against `"abc"^^xsd:string` | one node | the same term in RDF 1.1 |
| Escapes of the source | no | `"\u0041"` and `"A"` are a single lexical form |
| Quoting style of the source | no | whether quotes were long or short, single or double |
| A label that became a name | text and tag | which of several label predicates carried it is not kept ([Names](names.md#what-the-graph-preserves)) |

That a value has been read at all is kept by the statements that hold its node. A value node no longer held by any statement is removed by `.cleanup`, just as any other node lacking facts is.
