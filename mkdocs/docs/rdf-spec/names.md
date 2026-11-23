# Labels, Names and Languages

RDF attaches a label to a resource as a literal equipped with a language tag. zelph gives a node a name in a naming language, and scripts, queries, and display operations all function via these names. This page specifies how labels become names, which labels do not, how a name is found when the session language lacks it, and what the name-related interfaces look like. Nothing on this page has been implemented yet; a sentence about the present engine says "today", and a transcript outside a box is a real run of the present engine.

## Labels Become Names

A label becomes the **name** of its resource within the naming language specified by its tag. `wd:Q64 rdfs:label "Berlin"@de` assigns the name `Berlin` to the node `Q64` in the language `de`, without writing any statement or value node for it. The tag is converted to lowercase to determine the language: `@en-GB` corresponds to the naming language `en-gb`.

This is the function of the JSON import from Wikidata today, with the one label it takes, and the rationale remains identical:

- Scripts written under `.lang en`, the page on the [class hierarchy](../class-hierarchy.md), and the label handling within the SPARQL front end continue to function, across a network imported from RDF as well as one imported from JSON.
- An import from RDF yields identical names to an import from JSON.
- English labels cost, in an RDF import, the same as they do in the JSON import today: a name entry in each direction, rather than a node and a fact each ([Costs](costs.md)).

A label that has become a name can still be read as a literal. The condition `X rdfs:label L` is answered by the engine using the names, with `L` bound to the value node that the label would be; there is no need for an existing label statement to support it ([Querying Terms](querying.md#labels)).

### Label Languages

A naming language is a **label language** if its name conforms to the shape of a lowercase language tag and is not classified as reserved. Reserved names are:

- `zelph`, the language used by the engine’s and the standard library’s vocabulary;
- `wikidata` and every other language that a row of the [namespace table](iris.md#the-namespace-table) maps into;
- `ace` and `ace-lex`, the naming languages of the [ACE front end](../ace-spec/index.md), until their renaming is decided ([Open Decisions](open-decisions.md#the-naming-languages-of-the-ace-front-end)).

The IRI language `<iri>` cannot serve as a tag in any way. No information is stored regarding which languages are label languages: the rule reads the language’s name, so each `.bin` describes itself, and a language introduced by a user via `.lang` counts if its name matches the required format.

Only label languages are involved in what this page defines: labels become names within them, fallback chains run through them, and `rdfs:label` is answered from them.

### Which Predicates Make a Name

| Predicate | Becomes |
|---|---|
| `rdfs:label` | a name |
| `skos:prefLabel` | a name |
| `schema:name` | a name when the subject is a Wikidata entity; elsewhere an ordinary statement |
| `schema:description` | never a name: a value node bearing its tag, provided the language is selected |
| `skos:altLabel` | never a name: a value node carrying its tag, provided the language is selected |

Wikidata’s RDF writes every label of an entity threefold, under `rdfs:label`, `skos:prefLabel`, and `schema:name`; all three give the identical name, once. Outside Wikidata, `schema:name` is employed for numerous purposes unrelated to labels, so there it persists as a statement. A sitelink illustrates the subject’s significance: `schema:name` at the node of a Wikipedia article is the article’s title, and that node is not a Wikidata entity.

Descriptions and aliases cannot be names. A node carries precisely one name per language, and its label takes that name: a description or an alias would replace the label, and there are typically multiple aliases within a single language.

The set of languages an import keeps at all, for labels, descriptions, and aliases, constitutes a parameter of the import, with the labels in `en` and `mul` as the default ([Importing RDF](import.md#parameters)).

### Several Labels in One Language

A node may arrive with multiple labels in a language, originating from two predicates or two imports. Precisely one becomes the name, and which one is chosen is not influenced by the input order:

1. a name provided via `.name` or `zelph/name` is positioned above every imported label;
2. followed by `skos:prefLabel`, then `rdfs:label`, then `schema:name`;
3. among labels sharing the same predicate, the one whose text appears earliest in the order of its UTF-8 byte values.

Every additional label of that language at that node stays a value node, in a standard statement with its predicate. A later import can therefore substitute the name of a node with a label of higher rank, causing a script that referenced the node by its former name to no longer find it; an import reports how many names it has superseded.

### Labels That Do Not Become Names

A label stays a value node, in a standard statement with its predicate and with its tag if present, when it

- is an additional label of its language at its node, not chosen by the rule above;
- lacks a language tag;
- includes a tag that names a reserved language, since a node with the label `"Q5"@wikidata` would otherwise take the name of the item `Q5`;
- features a text that is the spelling of a core node, such as `in`, `nil`, or `!` ([Core Spellings](#core-spellings));
- has an empty text, or a text containing U+0000;
- belongs to a core node: a label of `rdf:nil` must not alter the name of the empty list.

Only one of these cases appears in Wikidata. On 4 November 2026, the Wikidata Query Service listed 24 English labels and 5 `mul` labels that spell a core node: `!` (8), `in` (5), `conjunction` (4), `negation` (2), `~` (2), `cons`, `nil`, and `!=` in English, along with `!` (2), `~` (2), and `in` in `mul`.

## Core Spellings

zelph comprises ten core nodes, and each is written by a fixed spelling: `~`, `=>`, `->`, `!=`, `!`, `cons`, `nil`, `in`, `conjunction`, `negation`. A label bearing such a text must never appear in place of the core node. Two rules enforce this:

- **Excluded at import.** Both imports, from RDF and from JSON, leave such a label as a value node. The few Wikidata items concerned lose that name; they stay reachable by their id.
- **Core spellings first, in every language.** A token is looked up as a core spelling before the names in the session language are consulted.

Today neither applies to imported labels. `.name` refuses to give a core spelling to another node, yet the JSON import assigns labels without performing that check, and a lookup consults the names of the language first. In the published network `wikidata-20260309-all-pruned-small.bin` under `.lang en`, `~` is therefore the item `Q87496336`, `!` is `Q98684635`, `!=` is `Q12357630`, `nil` is `Q1783389`, `cons` is `Q3687178` and `negation` is `Q1478451` (probed on 4 November 2026): a rule typed there with `=> !` has an item as its consequence. The second rule repairs such networks as they are; the first keeps the names out of regenerated ones.

## Labels That Several Nodes Share

Numerous resources carry the same label. zelph’s rule for this situation exists today, for the labels of the JSON import ([Labels that several entities share](../wikidata.md#labels-that-several-entities-share)), and remains: every carrier has the label as its own name, so each of them is displayed by it, and the name, when typed, resolves to the carrier ranked first. The ranking is completed for nodes that are not Wikidata entities:

1. according to the name in `wikidata`: a property preceding an item, the smaller number first, any other name following those, a node lacking one last (as today);
2. then according to the name in the IRI language, in the order of its bytes;
3. then according to the node id.

The second step is new. Today, two nodes lacking a Wikidata id are ranked by their node ids, which for nodes generated by scripts corresponds to the order of their creation; an import that reads its input in a different sequence would then name differently. With the IRI in between, the first carrier of a label among IRI nodes remains consistent regardless of the input’s order.

## Fallback Chains

### `mul`

Wikidata keeps a label that remains identical across all languages just once, in the language `mul`. Douglas Adams (`Q42`) has had no label in `en` since that label was moved there. `mul` is a naming language in its own right: a label in `mul` becomes a name within `mul` and in no other language, enabling a writer to determine whether the source wrote `@en` or `@mul`.

Today the JSON import copies a `mul` label into `en` when the entity has no English label, and a name that the session language lacks is found in no other language:

```
zelph> .lang en
en> x p q
en> .name q mul Douglas
Node 'q' ('en') exists, assigned name 'Douglas' in 'mul'.
en> %(nil? (zelph/lookup "Douglas"))
true
...
```

Under the mapping, the copy ends on both routes, and the lookup proceeds along a chain.

### A Chain Is a List of Languages

Each label language possesses a **fallback chain**: an ordered list of label languages that are consulted, in this order, when the language itself lacks a name.

- **Every chain ends with `mul`** unless its declaration specifies differently. In the absence of a declaration, the chain for `en` defaults to `mul`, as does the chain for `de` and for every other label language. `mul` itself possesses no chain.
- **A chain is read as written.** It is not followed transitively: if `de-ch` falls back to `de`, the chain of `de` does not influence a lookup under `de-ch`. Chains are explicit lists because the codes employed by Wikidata do not support alternative structures: among the 318 codes with a chain in MediaWiki, the software underlying Wikidata, 270 have a first fallback that is not a prefix of their own code (`gsw` falls back to `de`, `sco` to `en`), and 60 pairs mutually fall back to one another, meaning a rule that followed chains or cut codes at the hyphen would either loop or miss (counted on 23 November 2026 in the message files of MediaWiki).
- **Declared in the session.** `.fallback de-ch de` sets the chain of `de-ch` to `de`, then `mul`; `.fallback de-ch de -` assigns it to `de` exclusively, and `.fallback de-ch -` assigns it to nothing; `.fallback de-ch` displays it, and `.fallback` presents all. From Janet, `(zelph/fallbacks lang)` reads it and `(zelph/fallbacks lang steps)` modifies it. Both sides must be label languages; a chain that names its own language, names a language twice, or exceeds sixteen steps is refused. The table of chains belongs to the session: a `.load` preserves it, a `.new` clears it, and it is not saved, as it names languages rather than nodes.
- **Readers and partial views.** A change to the table takes effect as a whole: a lookup executing on a different thread at that moment reads either the earlier table or the new one, never a mixture of both. While the names of a network are loaded partially, `.lang` and `.fallback` print a warning: a name that a language seems to lack might reside in a section that was not loaded, and a subsequent step in the chain would then answer in its place.
- **The module `label-chains`** declares a small set that is manually authored: `de-ch`, `de-at`, `gsw`, and `rm` fall back to `de`; `en-gb`, `en-ca`, and `sco` to `en`; `pt-br` and `pt` to each other.

### How a Name Is Found

A token typed under the session language `L` is resolved in this order:

1. as the core spelling;
2. within the names of `L`;
3. within the names of each language in `L`’s chain, in order;
4. and if no such name exists, a new node is created and named in `L`, where the path may create; a path that solely looks up answers nothing.

The first language possessing the name answers, accompanied by its first-ranked carrier; carriers from distinct steps are never mixed. A lookup tagged `:strict` terminates following step 2.

The order applies to every path that turns a name into a node: the tokens of a script, `zelph/lookup` and `zelph/resolve`, the C function `zelph_resolve`, and the new, non-creating `zelph_lookup`, the node arguments in commands, and the plain names within a SPARQL query. `zelph/lookup-all` answers the carriers of the step where `zelph/lookup` arrived at its answer; with `:strict`, only those of `L`, and with `:chain`, the carriers of each step, each associated with its language.

**Never through a chain** proceed the paths that must signify a single language exactly: the assignment of labels via an import, the condition `X rdfs:label L` and the functions based upon it, the name readers ([Expansion](expansion.md#name-readers)), `.delname`, and `.name`.

**`.name` considers only the target language.** `.name X Berlin` gives `X` the name `Berlin` in the session language, regardless of whether a node accessible via the chain bears that name; the new name then precedes that node for lookups under this language, and a note names the node and its language, so that merging the two, if intended, requires one further command. Within a single language `.name` keeps today’s rule: a name carried by another node of that language merges the two.

**Display.** A node is shown using its name in the session language `L`; without such a name, by the first name found in this order: its core spelling, the chain of `L`, `en`, the chain of `en`, `zelph`, the remaining label languages ordered by their names, the namespace languages, the IRI language. Today the final steps follow the order in which a network initially acquired names in each language, a sequence determined by its history. A node bearing a Wikidata id keeps today’s form under a session language different from `wikidata`, its id and its label combined into one token ([The Re-Read Guarantee](notation.md#the-re-read-guarantee)): the label is the first name found in `L` and then in the chain of `L`, and if none exists, the id appears by itself.

### What a Chain Costs and What It Changes

- **Cost.** A chain is consulted solely when the session language lacks the name: one probe per step in case of a miss, none in case of a hit. On a prototype from 5 November 2026, importing 100,000 lines of new names ran 0.56 % more instructions with a single step (measured).
- **Words bind to `mul`.** A word entered under `.lang de` that lacks a German name but possesses a `mul` name refers to the `mul` carrier and does not generate a new node. That is the intended behaviour, and it represents a shift: such a line creates a node today.
- **An earlier step hides a later one.** With `de-ch` and the chain `de`, `mul`, a name that `de-ch` itself has takes precedence over the German one. In the first 200,000 entities from the dump of 9 March 2026, between 0.21 and 0.26 % of base language names resolved to a different item via a regional chain, and approximately 2.2 % of the names shown through the base step failed to read back as the node they were shown for, primarily because several nodes of that step share the same label (counted on 5 November 2026).
- **English names of Wikidata items move.** Today, an entity with only a `mul` label serves as a carrier of that text in `en` and competes based on rank; under the mapping, an English carrier of the text is found first. Typing "Douglas Adams" under `.lang en` resolves to `Q42` in a network imported today and will resolve to a disambiguation page bearing that English label, with `Q42` found only when no English carrier exists. Tests, the Wikidata page, and the ACE specification state today’s outcome and evolve with it; the marker a network carries so that the ACE front end can rely on the new allocation remains an [open decision](open-decisions.md#a-marker-for-the-label-allocation).
- **Memory.** `mul` as a language in its own right incurs a name entry in each direction for every entity that has both an English and a `mul` label ([Costs](costs.md)).

## Removal

A name is not a fact. A resource known to an import solely through its labels becomes a node bearing names but lacking statements, and `.cleanup`, which removes every node devoid of statements, eliminates it along with its names. That is today’s behaviour and it remains unchanged; the published pruned networks are cut by a process concluding with `.cleanup`.

## Interfaces

Today no programming interface can list the naming languages of a network or retrieve the names associated with a single language, only the Wikidata import can assign a label according to the shared labels rules, and the C function `zelph_name` consistently falls back to alternative languages. The mapping adds:

| Janet | C | Rust | Purpose |
|---|---|---|---|
| `(zelph/languages)`, with `:labels` for label languages only | `zelph_languages` | `Engine::languages` | the naming languages of the network |
| `(zelph/names-in lang)`, in pages | – | – | the names of a single language |
| `(zelph/assign-label node text lang)` | `zelph_assign_label` | `Engine::assign_label` | assign a label in the manner an import does: applying ranking, shared labels, and the exclusions mentioned above |
| `(zelph/lookup name lang :strict)` | `zelph_lookup` with a flag | `Engine::lookup` | find without creating, with or without the chain |
| `(zelph/name node lang :strict)` (exists) | `zelph_name_in` with a flag | `Engine::name_in` | the name in a specific language, with or without the display order |
| `(zelph/fallbacks lang)` | `zelph_fallbacks`, `zelph_set_fallbacks` | `Engine::fallbacks`, `Engine::set_fallbacks` | read and declare chains |

## What the Graph Preserves

| Of a label | Kept | Remark |
|---|---|---|
| Text | every character | as the name, or as the lexical form of a value node |
| Language tag | in lowercase | as the naming language, or as the tag of the value node |
| `@en` against `@mul` | yes | `mul` is a language of its own |
| The predicate of a label that became a name | no | a writer writes `rdfs:label`, and for a Wikidata entity the three predicates Wikidata writes |
| The predicate of a label that stayed a value node | yes | it is the predicate of its statement |
| Labels, descriptions and aliases of languages not selected | no | dropped and counted by the import |

The published networks from 2026 were constructed before the existence of the present label import; the information on this page regarding today’s names applies to networks built by the present importer. All published networks are regenerated for the release that brings the mapping ([Storage](storage.md#older-files)).
