# Blank Nodes

A blank node in RDF is a resource lacking an IRI: it is recognized solely through the statements involving it, and its label, if the syntax assigns one, holds significance only within the document that uses it. This page specifies the node that such a resource becomes, where its identity comes from, and how a script writes and reads it. Nothing on this page has been implemented yet; a sentence regarding the present engine says "today".

## A Pool of Anonymous Nodes

A blank node becomes a node belonging to its own class, a **blank node**: an atom lacking a name. Much like a [value node](literals.md), it resides within an id class inside the range of the atoms ([Storage](storage.md#id-classes)), so every statement may hold it as a subject or as an object, and rules match it just as they match any atom.

Its scope is a document. Two rules follow from this:

- Within a single document, the same label refers to the same node, and an anonymous node (`[]` or `[ … ]` in Turtle) is a distinct node at every location the syntax writes one.
- Two documents never share a blank node, even when both write `_:b0`.

To keep the second rule without remembering every label ever read, the id of a blank node is derived from two things: the **identity of its document** and the label, or, in the case of a node lacking a label, its position in the document. Importing the same document twice thus results in the same nodes, and because facts are found by their content, yields the same graph: an import is idempotent.

The engine refuses any attempt to transform a blank node into something different. A blank node is never merged with another node, thus a name conflict cannot cause it to merge with a named node, and a statement having a blank node as its predicate is refused. A blank node may carry a label: `_:b rdfs:label "Ada"@en` establishes `Ada` as its name in `en` according to the rules that apply to every label ([Names](names.md)).

The cells of an RDF collection are blank nodes within the source. Once a collection is turned into a zelph list, its cells no longer exist: the list is a term, and no blank node stands for it anymore ([Collections and Lists](collections.md)).

## The Identity of a Document

A reader must be able to assign identifiers to blank nodes in a single pass and using constant memory, even when its input comes from a pipe. The identity of the document is thus established before the reading of the first statement:

1. **Given by the user.** Each import takes an identity as an argument (`identity=` in `.import-rdf`, [Importing RDF](import.md#parameters)). It is a freely chosen string; an IRI is the most suitable option.
2. **Otherwise the source.** In the case of a regular file, it is the file IRI corresponding to its canonical absolute path (RFC 8089); for a source retrieved via an IRI, it is that IRI as given. A base IRI declared within the document (`@base`) does not override it.
3. **A source without a name needs one.** For a pipe, a FIFO, a device, or standard input, no default exists: the import is refused until an identity is given.

What is implied by the default is expressed directly, since it adheres to the name of the source rather than to its content:

- A copy of a file imported from a different location is a distinct document and generates separate blank nodes.
- A file that underwent modification and is re-imported under the identical name remains the same document: a label it continues to employ refers to the node it was previously, and an anonymous node takes the identifier of whatever entity occupied its position before.

Anyone requiring something different provides the identity: the identical string for two copies, a new string for a new version.

**Data from Wikidata** possesses a fixed identity: `http://www.wikidata.org/`. The JSON route always uses it, and an RDF import of Wikidata’s data passes it as `identity=`. Only in this case does the node of an unknown value remain identical across both routes ([Wikidata](wikidata-mapping.md)); the import report issues a warning when it sees Wikibase’s ontology within a document imported under a different identity.

## Ids

The identifier for a blank node consists of its class combined with 58 bits derived from a hash. The hash is generated using BLAKE2b, producing an 8-byte digest, with the personalization `zelph:blank:v1`, applied to the following bytes, in which every length and every ordinal is a 64-bit little-endian number:

| Part | Bytes |
|---|---|
| the identity | its length, followed by its UTF-8 bytes |
| a labelled node | `0x00`, the label’s length, then the label’s UTF-8 bytes |
| an anonymous node | `0x01`, its ordinal within the document’s anonymous nodes, starting from 0 |
| a collection cell | `0x02`, its ordinal within the document’s collection cells, starting from 0 |

The digest is read as a little-endian number, with its least significant 58 bits constituting the payload. The tag byte distinguishes among the three types: no label a document may write ever reaches the identifier of an anonymous node. Collection cells are counted independently, ensuring that the cells within a collection – most of which never become nodes – do not move the ordinals of anonymous nodes.

The ordinal of an anonymous node within the order in which the syntax introduces such nodes is determined by: in Turtle and TriG, the order of the `[` tokens; in JSON-LD, the order in which zelph’s own traversal of the expanded document meets a node object lacking `@id` ([Importing RDF](import.md#json-ld)).

**Collisions.** A 58-bit hash may assign identical identifiers to two different blank nodes, with no detection possible: the node stores neither its label nor its document. The expected number of colliding pairs across n blank nodes equals n²/2^59: 0.00017 for ten million (computed). The expense of maintaining data capable of identifying such collisions would demand a record for each blank node, which directly violates the one-pass, constant-memory requirement.

The 58 bits make no use of a counter. A session that uses no feature of the mapping creates no blank node and takes no identifier from any source.

## In a Script

Today `_:b0` is a variable in zelph: every token beginning with an underscore is one, thus a line that holds `_:b0` is a query and asserts nothing. Under the mapping the Turtle form is a blank node:

!!! example "Specified – not yet implemented"

    ```
    zelph> _:b0 knows bob
    zelph> X knows bob
    Answer: _:@06c41f6e9a3b7d15 knows bob
    ```

- **`_:label`** is the blank node associated with that label within the document to which the line belongs. In the case of a script file, the document is the file itself, identified by its canonical file IRI, regardless of whether the file is run from the command line or imported; a [`turtle` block](notation.md#the-turtle-block) contained within the file is part of the same document, so a label refers to the same node both inside and outside the block. For lines typed at the prompt, received via piped input, or submitted through an interface, the document is the session.
- **The identity of a session** is generated randomly, using 128 bits drawn at session initiation, ensuring that blank nodes from two distinct sessions never meet in a network where one session’s data is saved and later loaded by another. The directive `.identity <iri>` establishes this identity; a script requiring consistent identifiers across runs must set this identity upfront.
- **`_:@` followed by sixteen hexadecimal digits** is the display form and the reference form. A blank node is always rendered in this manner: its label is not stored, and a label would not indicate the document it belongs to. When typed, this form refers to an existing blank node with that specific identifier; it does not create a new node, and using an identifier that does not correspond to any blank node in the graph results in an error. This form is not valid Turtle syntax, so it cannot be used within a `turtle` block.

A label that has been exhausted by a list conversion ceases to be a node. A subsequent line in the same document that attempts to write the label is refused, accompanied by a message that names the list ([Collections and Lists](collections.md#labels-that-a-conversion-used-up)).

From Janet, `(zelph/blank identity label)` and `(zelph/blank-at identity ordinal)` find or create a blank node, while `(zelph/blank-id hex)` finds an existing one; the C functions `zelph_blank` and `zelph_blank_at` and the Rust methods `Engine::blank` and `Engine::blank_at` perform equivalent operations ([Programming Interfaces](storage.md#programming-interfaces)). A `.bin` file stores a blank node just as it stores any atom, via its id within the facts that hold it; it needs no dedicated section.

## What the Graph Preserves

| Of the blank node | Kept | Remark |
|---|---|---|
| That two occurrences in a document are one resource | yes | one node |
| That it is not an IRI | yes | the id class |
| Its label | no | a writer invents labels, for example from the ids |
| The document it came from | no | the identity enters the id and is not stored |
| That two documents kept their blank nodes apart | yes | different identities give different ids, up to the collision probability above |
| The cells of a collection that became a list | no | the list stands for them ([Collections](collections.md#what-the-graph-preserves)) |

A conformance test compares two graphs modulo a renaming of their blank nodes ([Conformance](conformance.md)); the ids themselves never matter to it.
