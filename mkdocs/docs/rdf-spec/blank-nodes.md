# Blank Nodes

A blank node in RDF is a resource lacking an IRI: it is recognized solely through the statements involving it, and its label, if the syntax assigns one, holds significance only within the document that uses it. This page specifies the node that such a resource becomes, where its identity comes from, and how a script writes and reads it. Of this page, the engine implements the class for blank nodes, the derivation of their ids, the two forms within a script, the identity of a script file and that of a session together with the directive `.identity`, the behaviour during a firing when a blank node would serve as a predicate, and the functions for Janet, C, and Rust. What depends on a reader of an RDF syntax is specified but not yet implemented: the identity of an imported document, the count of anonymous nodes and of collection cells, the label that turns into a name, the fixed identity of Wikidata’s data, and the labels that a list conversion consumes. Each of these parts indicates its status in place.

## A Pool of Anonymous Nodes

A blank node becomes a node belonging to its own class, a **blank node**: an atom lacking a name. Much like a [value node](literals.md), it resides within an id class inside the range of the atoms ([Storage](storage.md#id-classes)), so every statement may hold it as a subject or as an object, and rules match it just as they match any atom.

Its scope is a document. Two rules follow from this:

- Within a single document, the same label refers to the same node, and an anonymous node (`[]` or `[ … ]` in Turtle) is a distinct node at every location the syntax writes one.
- Two documents never share a blank node, even when both write `_:b0`.

To keep the second rule without remembering every label ever read, the id of a blank node is derived from two things: the **identity of its document** and the label, or, in the case of a node lacking a label, its position in the document. Importing the same document twice thus results in the same nodes, and because facts are found by their content, yields the same graph: an import is idempotent.

The engine refuses any attempt to transform a blank node into something different. A blank node is never merged with another node, thus a name conflict cannot cause it to merge with a named node, and a statement having a blank node as its predicate is refused. When the firing of a rule would write such a statement, the firing leaves it unwritten instead ([A Firing That Would Make One a Predicate](#a-firing-that-would-make-one-a-predicate)). A blank node may carry a label: `_:b rdfs:label "Ada"@en` establishes `Ada` as its name in `en` according to the rules that apply to every label ([Names](names.md)). The refusals are implemented, and `.name` assigns a name to a blank node; that a label obtained via `rdfs:label` becomes this name is not yet implemented.

The cells of an RDF collection are blank nodes within the source. Once a collection is turned into a zelph list, its cells no longer exist: the list is a term, and no blank node stands for it anymore ([Collections and Lists](collections.md)).

## The Identity of a Document

This section concerns a document that an import reads, and it is not yet implemented. The identity of a script file and that of a session, which the engine implements, are described [below](#in-a-script).

A reader must be able to assign identifiers to blank nodes in a single pass and using constant memory, even when its input comes from a pipe. The identity of the document is thus established before the reading of the first statement:

1. **Given by the user.** Each import takes an identity as an argument (`identity=` in `.import-rdf`, [Importing RDF](import.md#parameters)). It is a freely chosen string; an IRI is the most suitable option.
2. **Otherwise the source.** In the case of a regular file, it is the file IRI corresponding to its canonical absolute path (RFC 8089), in the single spelling that a file IRI possesses ([The File IRI of a Script File](#the-file-iri-of-a-script-file)): an imported document gets the IRI that a script file at the same path gets. For a source retrieved via an IRI, it is that IRI as given. A base IRI declared within the document (`@base`) does not override it.
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

The engine implements this derivation for each of the three kinds of reference, and its tests hold it against identifiers computed by another implementation of BLAKE2b. The count of the ordinals belongs to the readers and is not yet implemented.

The ordinal of an anonymous node within the order in which the syntax introduces such nodes is determined by: in Turtle and TriG, the order of the `[` tokens; in JSON-LD, the order in which zelph’s own traversal of the expanded document meets a node object lacking `@id` ([Importing RDF](import.md#json-ld)).

**Collisions.** A 58-bit hash may assign identical identifiers to two different blank nodes, with no detection possible: the node stores neither its label nor its document. The expected number of colliding pairs across n blank nodes equals n²/2^59: 0.00017 for ten million (computed). The expense of maintaining data capable of identifying such collisions would demand a record for each blank node, which directly violates the one-pass, constant-memory requirement.

The 58 bits make no use of a counter. A session that uses no feature of the mapping creates no blank node and takes no identifier from any source.

## In a Script

An underscore immediately followed by a colon begins a blank node, while every other token starting with an underscore is a variable. The Turtle form therefore makes an assertion where a variable would pose a query:

```
zelph> .identity http://example.org/doc
zelph> _:b0 knows bob
zelph> X knows bob
Answer: _:@068feb5243a0018d knows bob
```

- **`_:label`** is the blank node associated with that label within the document to which the line belongs. In the case of a script file, the document is the file itself, regardless of whether the file is run from the command line or imported; a [`turtle` block](notation.md#the-turtle-block) contained within the file is part of the same document, so a label refers to the same node both inside and outside the block (the block is not yet implemented). For lines typed at the prompt, received via piped input, or submitted through an interface, the document is the session. The label must conform to the format accepted by Turtle’s `BLANK_NODE_LABEL`, and the token terminates at the same point a bare name does; a token beginning with `_:` that is neither a valid label nor the `_:@` form described below is invalid and is treated as an error that names the token, never as a variable. The functions `zelph/blank`, `zelph_blank`, and `Engine::blank` do not validate the label: they take its bytes as given.
- **The identity of a script file** is its [file IRI](#the-file-iri-of-a-script-file), unless a line within the file establishes a different one. `.identity <iri>` in a script file sets the file’s identity from that line onward until the end of the file, just as a `.lang` in a module applies only to the module’s own lines. A file that sets its identity before its first label thus refers to the same blank nodes regardless of where it is stored. A label written before such a line and the identical label written afterwards constitute two distinct nodes: the label remains the same, yet the identity is not. The line changes nothing else: neither the identity of the session nor that of a file importing this one.
- **The identity of a session** is formed from 128 random bits, written as 32 lowercase hexadecimal digits, and drawn when the session first needs them, ensuring that blank nodes from two distinct sessions never meet in a network where data from one session is saved and later loaded by another. Only a line originating from the session itself, typed at the prompt or piped in, establishes this identity via `.identity <iri>`; a script file, whether a module or a session script, does not change it. `.new` keeps the identity of the session, as well as the identity that a session script has set for itself. A piped script requiring consistent identifiers across runs sets the identity before its first label.
- **`.identity` without an argument** shows the identity currently in force for its line, formatted as `The current identity is '…'`, and performs no setting; an empty string enclosed in quotation marks constitutes no argument, thus `.identity ""` has the same effect.
- **`_:@` followed by sixteen hexadecimal digits** is the display form and the reference form. A blank node is always rendered in this manner: its label is not stored, and a label would not indicate the document it belongs to. zelph prints the digits in lowercase. When typed, this form accepts the digits in either case and refers to an existing blank node with that specific identifier; it does not create a new node, and using an identifier that does not correspond to any blank node in the graph results in an error. This form is not valid Turtle syntax, so it cannot be used within a `turtle` block.
- **In a command**, a blank node may appear in either form, and the command creates none: a bare `_:label` for which the graph holds no node is classified as `Unknown node`. Within quotes, the identical text functions as a name: `.node "_:b0"` refers to the node named `_:b0`, and `.name _:b0 "_:b0"` assigns the name `_:b0` to the blank node, while `.name` refuses a bare `_:…` as a new name.

A module sets an identity of its own and leaves the identity of the session unchanged. Here a file `notes.zph` holds two lines:

```
.identity http://example.org/notes
_:b0 written-in notes
```

A session that imports it reads:

```
zelph> .identity http://example.org/doc
zelph> .import notes.zph
zelph> .identity
The current identity is 'http://example.org/doc'
zelph> _:b0 written-in session
zelph> X written-in Y
Answer: _:@068feb5243a0018d written-in session
Answer: _:@057f8a2add1418ab written-in notes
```

A label that has been exhausted by a list conversion ceases to be a node. A subsequent line in the same document that attempts to write the label is refused, accompanied by a message that names the list ([Collections and Lists](collections.md#labels-that-a-conversion-used-up)). The conversion and this refusal are not yet implemented.

From Janet, `(zelph/blank identity label)` and `(zelph/blank-at identity ordinal)` find or create a blank node, whereas `(zelph/blank-id hex)` finds an existing one and answers `nil` if the graph does not hold the id, just as `zelph/lookup` answers for a name; using digits that are not the identifier of a blank node is an error. The C functions `zelph_blank` and `zelph_blank_at` and the Rust methods `Engine::blank` and `Engine::blank_at` perform equivalent operations ([Programming Interfaces](storage.md#programming-interfaces)); all of them are implemented. A `.bin` file stores a blank node just as it stores any atom, via its id within the facts that hold it; it needs no dedicated section.

### The File IRI of a Script File

The canonical file IRI enters the id of every blank node a file writes via a label, provided the file has not defined its own identity, thus its spelling is fixed and must never be altered:

1. `file://` with an empty host appears first, then the canonical absolute path of the file: symbolic links resolved, `/` separating its components, encoded in UTF-8.
2. Within the path, each ASCII character not included in `A–Z a–z 0–9 - . _ ~ ! $ & ' ( ) * + , ; = : @ /` is transformed into a percent escape using uppercase hexadecimal digits, while every character outside the ASCII set remains unchanged.
3. A Windows drive path gets a `/` preceding its drive letter, and a network path brings its host along: the host of the path corresponds to the host of the IRI.

| Path of the script file | File IRI |
|---|---|
| `/home/x/a.zph` | `file:///home/x/a.zph` |
| `/home/x/my notes/50%.zph` | `file:///home/x/my%20notes/50%25.zph` |
| `/home/x/über#1.zph` | `file:///home/x/über%231.zph` |
| `C:\x\a.zph` | `file:///C:/x/a.zph` |
| `\\host\share\a.zph` | `file://host/share/a.zph` |

A path that cannot be made canonical, such as when the file is absent, enters as its absolute path, lexically normalized, and is expressed according to the same rules. A path provided relatively is made absolute upon file opening, so a script that changes the working directory before its first label keeps the identity of the file it is.

## A Firing That Would Make One a Predicate

A binding may place a blank node in the position where a consequence has its predicate: `(X p Y) => (a Y b)` with a blank node assigned to `Y`. The firing leaves that statement unwritten, as it leaves a statement whose predicate is identical to its object, and writes the other consequences arising from the same application. Nothing about that statement stays in the graph: no fact, no record of a contradiction, nothing indicating the blank node serves as a predicate, and no witness for a fresh variable that every run would make anew ([Fresh Variables](../logic.md#fresh-variables-generative-rules)). It is not a contradiction: the run prints no `! ⇐` line, reports no "Found one or more contradictions!" for it, does not count it among its contradictions, and `.run-export` produces no entry for it. The same applies when the blank node would function as a predicate only further down: within a statement nested in the consequence, `(X p Y) => (k says (a Y b))`, or in a rule that the consequence derives, `(R is transitive) => ((X R Y, Y R Z) => (X R Z))` after `_:b0 is transitive`. This applies solely to the firing of a rule: a typed statement and the functions of Janet, C, and Rust refuse a blank node as a predicate with their respective error.

The run ends with a solitary line specifying what it left unwritten (a single line, wrapped here for reading):

```
zelph> .identity http://example.org/doc
zelph> (X p Y) => (X seen Y) (a Y b)
(X p Y) => (a Y b) (X seen Y)
zelph> k p _:b0
(k seen _:@068feb5243a0018d) ⇐ (k p _:@068feb5243a0018d)
Note: statements whose predicate would be a blank node were not written, for 1
pair of rule and blank node: (X p Y) => (a Y b) (X seen Y) with
_:@068feb5243a0018d.
zelph> m p _:b0
(m seen _:@068feb5243a0018d) ⇐ (m p _:@068feb5243a0018d)
```

- **Pairs, not firings.** The line tallies every pair of a rule and a blank node, so its total remains unaffected by the number of threads, by the frequency with which a run meets the same application, or by the state of `.semi-naive` – whether it is on, off, or in check mode. A rule stands in the line once, followed by the blank nodes of its pairs: `<rule> with _:@…, _:@…; <rule> with _:@…`. The rules are ordered by their text and the blank nodes of a rule are ordered by their id; for more than ten pairs, the line outputs the first ten and ends with `; and N more`. When the binding would assign multiple blank nodes as predicates within a single consequence, the run names the consequence using one of them – the first such blank node encountered during the firing. The predicate of the consequence appears first, followed by the statements contained within the consequence, ordered according to the order in which the firing builds them: each statement comes after its subject, its predicate, and its objects.
- **Once in a session.** A run names only the pairs for which no prior run during the session has written the line, and the count includes only these; a run that met no new pair writes no line. This holds for each run, whether triggered automatically after an input line or entered manually via `.run`: above, the firing for `m` meets the same pair again, and the line does not reappear.
- **A warning, not a deduction.** The line is written in every `.deductions` mode, and also by the first pass of a `?` line. A quiet run, `.run quiet` or `(zelph/run :quiet)`, writes no line and names nothing; from Janet, it returns all pairs from that run under the key `:unwritten` ([Running quietly](../janet.md#running-quietly)).
- **No part of it is stored.** Neither the graph nor a saved `.bin` holds any of it: following a `.load` or a `.new`, and in a fresh instance that loads such a file, a run encountering a pair names it again.

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
