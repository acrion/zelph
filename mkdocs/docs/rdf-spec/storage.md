# Storage and Programming Interfaces

This page defines the location of the nodes of the mapping: the id classes they take, what a `.bin` stores of them, what a network records about itself, and the functions via which Janet, the C interface, and the Rust layer create and read them. Nothing on this page has been implemented yet; a sentence about the present engine says "today".

## Id Classes

A node is a 64-bit id, and its four highest bits select a class. Today all sixteen patterns are taken:

| Bits 63..60 | Class | Holds |
|---|---|---|
| `0000` | counter | named nodes, the ten core nodes, and any other node that gets the subsequent value from the node counter |
| `0001` | set pattern | a set pattern created beyond the scope of a rule |
| `0010` | recipe | the term a firing rule builds for a collection of its text |
| `0011` | template | a collection written within a rule, and the collections constructed by a rule-writing rule |
| `0100` to `0111` | hash | facts and set constants: the identifier is derived from a hash of the parts |
| `1000` to `1111` | variable | variables, enumerated in descending order from the highest identifier |

The counter class is the sole one with room: the counter for the full Wikidata network stands near 2^27 (the network contains 119,231,266 names in `wikidata`, [Binary Data Files](../binaries.md)), and the class holds 2^60 ids. Value nodes and blank nodes take two distinct ranges within it:

| Range | Class | Payload |
|---|---|---|
| `0000 1…` (bits 63..59 equal `00001`) | **literal** | a 59-bit sequence number drawn from an independent counter |
| `0000 01…` (bits 63..58 equal `000001`) | **blank** | 58 bits derived from the document and label ([Blank Nodes](blank-nodes.md#ids)) |
| under 2^58 | counter, as today | the node counter, which is specified to be refused at 2^58, whereas it is refused at 2^60 today |

The decision emerges from how the engine already handles the classes:

- **Every existing test reads both as atoms.** No test for a hash, a variable, a recipe, or a template applies to them, so no reader of the graph needs to acquire any knowledge to treat a value node or a blank node as the plain atom it is for matching.
- **They stay on the fast path.** The engine skips its bookkeeping for variables in a statement where the subject, predicate, and objects are all ids of the counter class, which holds true for every data triple in a bulk import. A literal or a blank node within a triple maintains this condition.
- **The counters remain separate.** A sequence number for a value node is taken from the literal counter and never from the node counter. A session that creates no literal therefore numbers its nodes exactly as it does today, which is a [requirement](engine.md#a-session-without-rdf-stays-as-it-is) and not a minor detail: the work of a run depends on the numbering.

IRI nodes need no class. They are named nodes belonging to the counter class ([IRIs](iris.md#an-iri-is-a-named-node)).

## What a `.bin` Stores

A `.bin` file today comprises a header, a directory listing its chunks, and five sections of chunks: the two sides of the adjacency, the names in both directions, and the annotations ([Binary Data Files](../binaries.md)). The mapping contributes an additional section and three records.

### The Value Section

The value store ([Literals](literals.md#the-value-store)) is saved as a distinct section, following the annotations, and the chunk directory lists it alongside every other section, specifying offset, length, and entry count per chunk.

| Field of a value chunk | Content |
|---|---|
| chunk index | the position of the chunk within its section |
| datatype | the full IRI of the datatype of every value contained in the chunk |
| tags | the language tags present in the chunk; empty unless the datatype is `rdf:langString` |
| nodes | the ids of the value nodes, ordered in ascending sequence |
| tag index | for each node, the index of its associated tag |
| ends | for each node, the endpoint of its text within the text block |
| text | the lexical forms, encoded as bytes, sequentially concatenated |

- A chunk holds values of a single datatype, up to a maximum of one million such values and up to 256 MiB of text.
- The text is stored as bytes with explicit ends, never as text ended by a zero byte, thus allowing a lexical form to include U+0000.
- The facts involving a value node are stored where all facts are, within the adjacency sections.
- The content index is not stored. A load operation reads the value chunks into the store and builds the index from them, using a reserve taken from the directory’s entry counts.

`.stat-file` and `.index-file` list the section; a manifest for partial and remote loading names its chunks under the key `values`, and `.load-partial` retrieves them using `values=`. A save of selected predicates keeps the value nodes that the statements it keeps hold.

### Records in the Header

| Record | Content | Why |
|---|---|---|
| literal counter | the next sequence number | to ensure that a loaded network continues numbering from where it left off |
| substrate | the name of the module supplying the network’s arithmetic | to preserve the meaning of stored expansions ([The Substrate](expansion.md#the-substrate)) |
| namespace rows | the rows of the namespace table via which nodes of this network were mapped | to maintain the same node reference for an IRI ([What a Network Records](iris.md#what-a-network-records)) |

The header is read before a load replaces the graph, which enables `.load` to issue a warning about a conflicting substrate in a timely manner.

In the session the substrate record is an annotation attached to the core node `nil`, keyed as `zelph:substrate`. An annotation is neither a node nor a fact, so writing the record changes no numbering, `.cleanup` cannot remove it, and no cluster records it; the copy in the header supports loads that do not read annotations.

The marker indicating how labels were allocated is not included in these records: the label import writes it as a named node, just as it does today, and it is saved alongside the names. The name of this marker within the mapping remains an [open decision](open-decisions.md#a-marker-for-the-label-allocation).

### Partial Views

A network may be partially loaded: specific chunks within certain sections. For the mapping this means:

- A view missing the value section holds value nodes without their texts. The load counts them; they are displayed with a mark that does not read back, and no expander is run for them.
- The conditions that the engine answers from the value store answer nothing for such a node.
- An RDF import into a partial view is refused unless the names it maps into are loaded completely ([Importing RDF](import.md#how-an-import-proceeds)), and neither the list conversion nor the request pass runs on one.

### Older Files

The format carries no version, and the mapping preserves this state: a file says what it holds via the fields within its header.

- **A file written before the mapping** holds no value section and none of the three records. It loads as it does today, as a network devoid of literals, without a recorded substrate and without recorded namespace rows.
- **A file with a value section opened by an engine lacking the mapping** does not fail at that point: such an engine ignores what it does not recognize, displays the value nodes as unnamed nodes, and a save from it writes a file without the texts. Files that hold value nodes are intended for engines that possess the mapping; [Binary Data Files](../binaries.md) will name the release.
- **No code is kept for superseded layouts.** Every published network is regenerated with the release that brings the mapping, which is also when the names change that the mapping changes: `mul` as a language in its own right, the labels that spell core nodes, the names of statement nodes ([Names](names.md), [Wikidata](wikidata-mapping.md)).

## Programming Interfaces

The functions below are specified; the last column says what is available today. In each of the three layers, a lexical form is provided alongside its length: a Janet string carries it, a C function takes a pointer and a length, a Rust method a slice. Nodes are passed as node handles, never as host language numbers, since the host language’s floating-point numbers cannot hold every identifier.

| Purpose | Janet | C | Rust | Today |
|---|---|---|---|---|
| find or create a value node | `(zelph/literal lexical datatype &opt lang)` | `zelph_literal` | `Engine::literal` | – |
| the lexical form, datatype, and tag of a value node | `(zelph/literal-parts node)` | `zelph_literal_parts` | `Engine::literal_parts` | – |
| find or create the node associated with an IRI | `(zelph/iri iri)` | `zelph_iri` | `Engine::iri` | names only: `zelph/resolve`, `zelph_resolve`, `Engine::resolve` |
| the canonical IRI of a node | `(zelph/iri-of node)` | `zelph_iri_of` | `Engine::iri_of` | – |
| find or create a blank node | `(zelph/blank identity label)`, `(zelph/blank-at identity ordinal)` | `zelph_blank`, `zelph_blank_at` | `Engine::blank`, `Engine::blank_at` | – |
| an existing blank node by its identifier | `(zelph/blank-id hex)` | – | – | – |
| the kind of a node | `(zelph/node-kind node)` | `zelph_node_kind` | `Engine::kind` | – |
| build a list, read its elements | `(zelph/list …)`, `(zelph/car l)`, `(zelph/cdr l)` | `zelph_list`, `zelph_list_elements` | `Engine::list`, `Engine::elements` | exists |
| declare a prefix, register a namespace row | `(zelph/prefix p iri)`, `(zelph/namespace …)` | – | – | – |
| import an RDF document | `(zelph/import-rdf source options)`, `(zelph/read-turtle text options)` | `zelph_import_rdf` | `Engine::import_rdf` | – |
| import a module from the standard library | `.import` within a script | `zelph_import` | `Engine::import` | Janet and the REPL only |
| register an expander, a name reader, a display | `zelph/register-expander`, `zelph/register-class-expander`, `zelph/register-name-reader`, `zelph/register-datatype-display` | – | – | – |
| expand nodes at once | `(zelph/expand nodes predicate)` | `zelph_expand` | `Engine::expand` | – |
| convert lists | `(zelph/lists)` | `zelph_lists` | `Engine::lists` | – |
| names, languages, and fallback chains | see [Names](names.md#interfaces) | | | |

`zelph/node-kind` answers one of `:named`, `:literal`, `:blank`, `:fact`, `:variable`, or the kinds of rule text that exist today.

**From C and Rust.** A host program can perform every action listed above that does not involve Janet code: create and read terms, import documents and modules, expand, convert lists. It is unable to register an expander, a name reader, or a display, as these are functions of a module; it imports the module that carries out those duties. Today a C or Rust host cannot import a module at all, because the C interface leaves `.import` to scripts ([The C ABI](../capi.md)); `zelph_import` closes this gap, as `zelph_load` wraps `.load`.

**Threads.** The rule of the C interface remains unchanged: the graph is tied to the thread that created the engine. Every function responsible for creating a node, registering an entry, or performing an import operates exclusively on that thread. The functions that only read (`zelph_literal_parts`, `zelph_node_kind`, `zelph_iri_of`, `zelph_name_in`, `zelph_lookup`) may be called from any thread, as reading functions may be today.

**Text.** Today, multiple layers separating a script from the graph end a text upon encountering the first zero byte: the bridge between Janet and the engine, the C interface, and the names writer. A lexical form including U+0000 would be cut by each of these layers. The mapping transmits length information across every layer for lexical forms; names are not impacted, as a text containing U+0000 never becomes a name ([Names](names.md#labels-that-do-not-become-names)).
