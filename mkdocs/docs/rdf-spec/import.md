# Importing RDF

This page specifies the command that reads an RDF document into the graph, its parameters, the information it reports, the fate of named graphs, and the rules governing the JSON-LD reader. Nothing on this page has been implemented yet; a sentence about the present engine says "today".

## The Command

!!! example "Specified – not yet implemented"

    ```
    .import-rdf <source> [format=<syntax>] [identity=<string>] [base=<IRI>]
                [labels=<languages>] [descriptions=<languages>] [aliases=<languages>]
                [predicates=<predicate>,…] [lists=convert|defer]
                [accounting=on|off] [shortcut=on|off]
    ```

`.import-rdf` reads a single document and **adds** its statements to the session’s graph. It never replaces the graph, it writes no cache file, and what it has written is preserved just like everything else, via `.save`. The source is either a path or an IRI; a path may name a pipe.

The syntax is taken from the suffix of the source, compression included:

| Suffix | Syntax |
|---|---|
| `.nt` | N-Triples |
| `.nq` | N-Quads |
| `.ttl` | Turtle |
| `.trig` | TriG |
| `.jsonld` | JSON-LD 1.1 |

Each may be followed by `.gz`, `.bz2`, or `.zst`. A source lacking such a suffix, a pipe for example, specifies its syntax using `format=nt`, `nq`, `ttl`, `trig`, or `jsonld`.

The command brings in the module `rdf` if the session has not already done so, since the [namespace table](iris.md#the-namespace-table) decides which node an IRI is and must be the same for every import.

The identical import is accessible via `(zelph/import-rdf source options)` in Janet, as `zelph_import_rdf` with an options structure in C, and as `Engine::import_rdf` in Rust. Turtle text embedded within a script is read by the same reader through the [`turtle` block](notation.md#the-turtle-block).

`.load` keeps its meaning: it loads a `.bin` file or imports a Wikidata JSON dump. Today it sends every source ending in `.json` or `.bz2` to the Wikidata importer and determines the cache file’s name by dropping the suffixes, so a compressed N-Triples file named alongside a JSON dump would share the same cache. With the mapping, `.load` treats only `.json` and `.json.bz2` as a Wikidata dump, and the cache of a dump records the format of the importer that wrote it; a cache written by a different importer is not utilized.

## Parameters

**`identity=`** denotes the document’s identity, from which the ids of its blank nodes are derived. Its default value is the source; when a source lacks a name, this identity is required ([Blank Nodes](blank-nodes.md#the-identity-of-a-document)).

**`base=`** is the base IRI used to resolve relative IRIs, following the rules defined in RFC 3986. Its default value is the IRI from which the document was obtained: the file IRI of a path, or the source IRI. It is separate from the identity, enabling a document to be read from a temporary copy while preserving its original base. A source lacking a name that uses relative IRIs needs it. N-Triples and N-Quads do not include relative IRIs and ignore it.

**`labels=`, `descriptions=`, `aliases=`** determine which languages an import retains for each of the three kinds of term text. Each takes a list of language tags separated by commas, or `all`, or `none`. A tag is matched exactly: `en` does not include `en-gb`. The defaults are those of the JSON import of Wikidata today:

| Parameter | Default | What a selected text becomes |
|---|---|---|
| `labels=` | `en,mul` | a name in its language ([Names](names.md)) |
| `descriptions=` | `none` | a value node bearing its tag, within an ordinary statement |
| `aliases=` | `none` | a value node bearing its tag, within an ordinary statement |

A text in a language that is not selected is dropped, and the report includes it in its tally. The default costs no memory for further languages, descriptions, or aliases, ensuring that nobody importing Wikidata is surprised by this; anyone desiring more explicitly indicates so. The rationale behind the default stems from scale: an RDF import of Wikidata constitutes a selection regardless, and descriptions across every language alone would bring the node count of the full network to the limit of the node map ([Costs](costs.md)). Which predicates count as labels, descriptions, and aliases is detailed in [Names](names.md#which-predicates-make-a-name); every other statement with a language-tagged string is read irrespective of its language.

**`predicates=`** makes the reader retain only the triples that feature the listed predicates, each given as an IRI within angle brackets, as a prefixed name, or as the name assigned by the namespace table. This is the method for incorporating the values of chosen properties into a network that already holds the entities ([Wikidata](wikidata-mapping.md)). Triples involving different predicates are skipped and counted.

**`lists=defer`** leaves chains of `rdf:first` and `rdf:rest` as statements for a later conversion; the default `convert` converts well-formed chains at the end of the import ([Collections and Lists](collections.md#when-the-step-runs)).

**`accounting=on` and `shortcut=off`** serve tests. With `accounting=on`, the import keeps the set of distinct triples it has parsed and the set of distinct quads among them that carried a graph name, and reports both counts; these sets grow with the input, hence the default is `off` ([Conformance](conformance.md#what-the-comparison-cannot-see)). With `shortcut=off`, a reader writes the cells of every collection and hands them over to the conversion step, where the default `on` enables it to construct a list at the closing token ([Collections](collections.md#the-conversion-is-a-step-in-the-graph)).

## How an Import Proceeds

1. **The reader streams.** The readers for N-Triples, N-Quads, Turtle and TriG work in one pass with constant memory, excluding the set of graph names that the report counts, enabling a dump to be read from a pipe. Each triple is mapped term by term ([IRIs](iris.md), [literals](literals.md), [blank nodes](blank-nodes.md)) and written as one fact having one object ([Statements](iris.md#statements)).
2. **Term texts are routed.** A triple whose predicate is a label, a description or an alias predicate undergoes language selection; a selected label is handed to the single primitive that assigns names, which both the RDF import and the JSON import utilize, ensuring the two do not assign names differently.
3. **Lists are converted**, provided they are not deferred.
4. **The report is printed**, to the diagnostic channel.
5. **Requests are answered** once for what was added by the import ([Expansion](expansion.md#when-requests-are-answered)), and when automatic inference is enabled, it runs once.

An import is not atomic. If the reader meets a syntax error, the statements it wrote prior remain in the graph, the report says how many exist and specifies the error’s location, and no list conversion takes place, since the input might be truncated. A `turtle` block differs: its text is parsed completely before any writing occurs.

An import into a network that has been partially loaded is refused unless each naming language it maps into is loaded in both directions – the languages from the namespace table, the IRI language, and the label languages it selects: with an incomplete name index the reader would not find the existing nodes and would create them again. On such a view, steps 3 and 5 do not run ([Collections](collections.md#when-the-step-runs), [Expansion](expansion.md#when-requests-are-answered)).

## The Import Report

The report details how the import interacted with what it read. Its counts convey information, not errors: an import that dropped graph names or labels has succeeded.

| Count | Meaning |
|---|---|
| triples read | following the `predicates=` selection; the triples skipped by it are tallied in parallel |
| facts written, facts already present | a triple whose fact was already established contributes nothing |
| names assigned; labels kept as value nodes | per reason ([Names](names.md#labels-that-do-not-become-names)) |
| names superseded | a label of higher rank took the place of a name assigned earlier ([Names](names.md#several-labels-in-one-language)) |
| labels, descriptions, aliases discarded | by category, according to the language selection |
| quads with a graph name; distinct graph names | refer to [Named Graphs](#named-graphs) |
| triples where the predicate equals the object | not written ([Statements](iris.md#statements)) |
| triples featuring a blank node or a literal as predicate | not written; generalized RDF lies beyond the profile |
| chains converted and chains left | the report of the list conversion ([Collections](collections.md#the-report)) |
| remote documents loaded | JSON-LD contexts, each with its IRI and its ETag or a hash of its content |
| distinct triples parsed; distinct quads with a graph name | with `accounting=on` only |

The number of triples written and the number of triples not written always sum to the number of triples read. The [conformance runner](conformance.md#what-the-comparison-cannot-see) keeps a more rigorous account, of the distinct triples parsed against what the graph holds afterwards, in which a triple that was not written fails the test. The report also warns when it sees Wikibase’s ontology in a document imported under an identity other than `http://www.wikidata.org/` ([Blank Nodes](blank-nodes.md#the-identity-of-a-document)).

## Named Graphs

N-Quads, TriG, and JSON-LD permit embedding a triple within a named graph. zelph maintains a single graph. The name of the graph, serving as the fourth element of a quad, is thus omitted: the triple is written as though it stood in the default graph, and the report counts the quads that carried a graph name as well as the distinct graph names present within them. The graph constructed by an import from a dataset is the union of its default graph and all of its named graphs.

This departs from what the standards expect of a consumer that takes a dataset as a single graph. JSON-LD 1.1 says that such a consumer must use only the default graph and ignore the named graphs, and RDF 1.1 Concepts describes the same expectation for every dataset syntax. zelph performs a union instead, since discarding the content of named graphs would lose most of what many datasets hold, and the report says how much of the input resided within named graphs.

**A possibility for later** consists in retaining the graph name as a statement concerning the statement: for the quad `s p o g`, the fact `(s p o) in-graph g`, using a predicate fixed for this intent. It is not included in this specification; its two forms and their associated costs are documented here to ensure the choice is prepared.

- Using an ordinary predicate, the outer statement asserts the inner one, as each nested statement in zelph is asserted. A triple originating from a named graph is then a fact everywhere, and a triple present in two graphs is one fact with two statements about it. The graphs are annotations; they are not separate.
- With an [opaque predicate](../logic.md#mentioning-a-statement-opaque-predicates), the inner statement is a mention: `s p o` does not yield an answer on its own, a pattern `(X p Y) in-graph G` matches it, and the triples of the default graph remain as data. That is the semantics of a dataset. Rules therefore access the content of a named graph solely via the graph predicate, and a list within such a triple also becomes a mention.

Either form incurs an additional fact per quad ([Costs](costs.md)).

## JSON-LD

The JSON-LD reader implements the algorithm "Deserialize JSON-LD to RDF" (toRdf) as defined in JSON-LD 1.1 and hands the triples it emits to the identical mapping used by every other reader. Its options are set in stone:

| Option | Value | Consequence |
|---|---|---|
| `processingMode` | `json-ld-1.1` | |
| `produceGeneralizedRdf` | false | a triple with a blank node as predicate is dropped and counted |
| `rdfDirection` | null | the base direction of a string is not represented in the output; this loss is stated |
| `base` | the parameter `base=` | |

Further rules:

- **Native numbers.** A JSON number is read as an IEEE double. A number having an integral value whose absolute value is less than 10^21 is written with all its digits, unless a datatype of `xsd:double` is asked for; every other number is written in the exponent form prescribed by the algorithm, featuring one digit before the decimal point and up to fifteen after it, rounded half away from zero based on the exact binary value, and with trailing zeros reduced to a minimum of one fraction digit, as seen in `5.3E0` and `1.0E21`. The datatype is `xsd:integer` for the first form and `xsd:double` for the second exclusively when the context does not coerce any: a coerced datatype remains unchanged, so that `5.1` under the type `http://example.com/datatype` becomes `"5.1E0"` with that datatype. This format reflects the algorithm’s output, not XSD’s canonical form; the value node keeps it as its lexical form, just like any other.
- **`rdf:JSON`.** A JSON literal becomes a value node with the datatype `rdf:JSON`, where the lexical form is the canonical representation defined by RFC 8785: keys ordered by UTF-16 code units, numbers expressed in ECMAScript form. It is the sole datatype the profile takes from beyond RDF 1.1 ([Conformance](conformance.md)).
- **Blank nodes.** A node object lacking an `@id` is assigned the ordinal position of its location within zelph’s internal traversal: the document is expanded with its keys in order, and nodes are numbered as the node map generation algorithm meets them ([Blank Nodes](blank-nodes.md#ids)).
- **Lists.** `@list` is built as a zelph list immediately when the shortcut is applicable ([Collections](collections.md#the-conversion-is-a-step-in-the-graph)).
- **Not one pass.** A JSON-LD context may appear after the properties it defines, so the reader buffers one top-level object at a time. It is the only reader that does not stream triple by triple.
- **Remote contexts.** A context referenced by an IRI in a document is retrieved once, with a request for `application/ld+json`, and kept in a local catalogue; the report lists each remote document along with its ETag, or, if the server sends none, a hash of its content. When a context changes behind its IRI, the IRIs of the triples change, even though the document itself remains unchanged: the list in the report is what enables a user to detect such changes.
- **What toRdf does not carry** is discarded before the mapping begins: the context and its prefixes, `@index` entries, and the sequence of keys. The preservation tables on the other pages apply only to the triples emitted by the algorithm.

## What an Import Does Not Do

- It does not expand any value. A value node costs a store record; what it stands for as numbers or character lists is built [on request](expansion.md).
- It does not run inference unless automatic inference is enabled, and then only once, following the import.
- It does not interpret vocabularies. `rdf:type`, `rdfs:subClassOf`, `owl:sameAs`, and all others function as predicates just like any other; a session that wants consequences from them writes or imports the rules.
