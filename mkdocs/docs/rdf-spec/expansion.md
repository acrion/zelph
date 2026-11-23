# Expanding Values on Request

A [value node](literals.md) holds a text and a datatype and nothing beyond that. The form the value takes in ordinary graph structure – a number as a numeral, a string as a list of its characters, a date as an interval of day numbers – is its **expansion**. An expansion is constructed exclusively when it is requested, and it is linked to the value node via a standard statement that rules treat identically to any other fact. This page specifies the statements involved, who builds an expansion, how one is requested, and what happens to all of it when a network is saved and loaded. Nothing on this page has been implemented yet; a sentence about the present engine says "today", and a transcript outside a box is a genuine execution of the present engine.

## Why on Request

An import expands nothing. Wikidata holds hundreds of millions of date and number values, and the full network, even without any literal value, already requires approximately 200 GiB of memory (computed, [Costs](costs.md#the-node-map); 223.7 GiB were measured before the engine moved to a single node map, [Binary Data Files](../binaries.md)). A numeral consisting of seven digits comprises seven `cons` cells, a date interval involves two such numerals with a term positioned around them. Building all of this for every value would incur a cost vastly exceeding that of the values themselves, and almost none of it would ever be read. With expansion on request, the cost materializes only when a value is actually required: for birth and death dates during a check’s comparison, and for nothing else ([Costs](costs.md)).

## Expansion Statements

An expansion statement features the node being expanded as its subject, one of three predefined predicates, and the resulting expansion as its object:

!!! example "Specified – not yet implemented"

    ```
    "+113"^^xsd:decimal   zelph:value     &113
    "Berlin"@de           zelph:chars     <n i l r e B>
    "1990"^^xsd:gYear     zelph:interval  (&2447893 interval &2448257)
    ```

| Predicate | Object | For |
|---|---|---|
| `zelph:value` | a number from the standard library, or one of the special values | numeric datatypes ([Numbers](numbers.md)) |
| `zelph:chars` | a list of characters | strings |
| `zelph:interval` | an interval term | dates and times ([Dates](dates.md)), and nodes with bounds such as Wikidata’s quantities |

The three predicates constitute part of the engine’s vocabulary: names in the `zelph` language that are generated upon their initial use and reached from any session language via the predeclared prefix. A module may register an expander under a predicate it defines in addition to these three, as `wikibase` does for the components of a coordinate ([Wikidata](wikidata-mapping.md#coordinates)). The statements themselves are ordinary facts. A rule employs them as conditions, a query finds them, `.save` writes them, and `.explain` shows an expansion as derived from the request that caused it.

Two outcomes arise from the expansion being a second node adjacent to the value node:

- **Values that are equal expand to the same node.** `"01"^^xsd:integer` and `"1"^^xsd:integer` are two value nodes sharing the identical expansion `&1`. A rule assessing whether two values are equal compares their expansions.
- **The kind of expansion is in the predicate.** Within `decimal-arithmetic`, the character list of the string `"123"` and the numeral `&123` are a single node, as both are the list of digits 3, 2, and 1 ([Strings](#strings)). A rule tells the string from the number based on the predicate via which it arrived at that node: `zelph:chars` for the string `"123"^^xsd:string`, `zelph:value` for the number `"123"^^xsd:integer`. No rule is required to equate the two, and none does unless the predicates are deliberately conflated.

## Expanders

An **expander** is a function within a module, written in Janet, that turns a value into a graph structure. The surrounding framework handles the remainder: it verifies the lexical form, calls the expander, and writes the expansion statement.

!!! example "Specified – not yet implemented"

    ```
    (zelph/register-expander "http://www.w3.org/2001/XMLSchema#gYear" "zelph:interval"
      (fn [lexical lang] ...))
    (zelph/register-class-expander "http://wikiba.se/ontology#TimeValue" "zelph:interval"
      (fn [node] ...))
    ```

- **A datatype expander** is registered for a datatype and an expansion predicate. It receives the lexical form and the tag and returns the node of the expansion, or nothing. Before invocation, the framework checks the lexical form against the lexical space registered for the datatype; a literal that fails this validation is considered ill-typed and gets no expansion.
- **A class expander** is registered for a class and an expansion predicate. It receives a node that is an instance of the class via an `rdf:type` statement, reads the statements associated with it, and returns the expansion, or nothing as long as any required statement is absent. This mechanism enables the expansion of a value distributed across multiple statements: Wikidata’s full time values carry timestamp, precision, and calendar information in three separate statements at a single node ([Wikidata](wikidata-mapping.md)).
- **Nothing is an answer.** An expander returning nothing leaves the node without an expansion. An expander that fails does not stop the session: the failure is counted and reported alongside the pass.
- **Registrations are part of the session state**, as the registration of the digits of the numerals is today. They are keyed by the text of the IRI and do not generate a node. A subsequent registration for the same key replaces the prior one. `.new` and a `.load` that replaces the graph forget them, just as they forget the digits today; importing the modules reinstates the registrations.
- **The standard library registers** the expanders for the decimal family and floating-point types in `common-arithmetic`, which every arithmetic substrate imports, the string expander in `rdf`, those for the date and time types in `calendar`, and the class expanders for Wikidata, along with the three expanders for the components of a coordinate, in `wikibase`.

An expander responsible for constructing numbers calls the substrate’s digit reader, `zelph/number`, during execution rather than at registration time, thereby generating numerals according to the substrate currently imported by the session ([The Substrate](#the-substrate)). A program that embeds zelph via C or Rust cannot register an expander, as expanders are written in Janet code; instead, it imports the modules using `zelph_import` and reads the expansions.

## Name Readers

The same mechanism reads the **name** of a node as a value. A name reader is registered for a datatype and a list of languages:

!!! example "Specified – not yet implemented"

    ```
    (zelph/register-name-reader "zelph:decimal-integer"
      "http://www.w3.org/2001/XMLSchema#integer" ["mul" "en"])
    ```

Asked for a node, it takes the node’s name in the first of its languages possessing one, strictly in that language and without invoking a fallback chain, reads it as a lexical form of its datatype, and establishes a link from the node to the expansion via a statement of the same type that an expander writes: for the item whose name is `113`, the statement indicating the item has the `zelph:value` `&113`.

- **The link performs no merging.** The item and the numeral remain as two distinct nodes, and no fact is copied from one to the other. What the standard library derives regarding `&113` arrives at the item via rules that treat the linking statement as a condition.
- **A name reader starts from selected nodes, never from every node whose name happens to read as a value.** Many items share the same English label, and a name made of digits belongs to years, albums, and films: 1999 is a prime number, and a check for missing primality claims across every node named by digits would incorrectly flag the year 1999. The selection is governed by a rule over a class ([Checks](checks.md#which-items-which-numbers)).
- **The reader of whole numbers**, `zelph:decimal-integer`, is registered in `common-arithmetic`, so it is available regardless of which substrate the session imports. It accepts digits with an optional sign. Wikidata writes whole numbers with a sign, `+113`, which `zelph/int` refuses today; `zelph/int` moves into `common-arithmetic` and accepts the `+`.
- **Where a value is imported, no name is read.** An item with a "numeric value" statement has the literal `"+113"^^xsd:decimal`, which expands directly to `&113`.

A node whose name was read before being renamed is read once more when requests are next answered, and it may then carry two values: the statement written under the old name is a derived fact and stays.

## Requests

No content is expanded unless a **request** calls for it. A request is a fact.

| Request | Meaning |
|---|---|
| `P zelph:expand-objects E` | a standing request: expand, using the expansion predicate `E`, every object within a statement bearing the predicate `P`, now and at any future time a new one is added |
| `V zelph:expand E` | expand, using the expansion predicate `E`, the node `V` |
| `X zelph:read-name R` | read the name of `X` via the name reader `R` |

Each request has a single object. All three are saved with the graph, ensuring the network maintains awareness of what it expands.

A standing request is a single fact for an entire property and incurs no cost per value. The other two forms are usually the **consequence of a rule**, which makes any condition a selection:

!!! example "Specified – not yet implemented"

    ```
    zelph> .import decimal-arithmetic
    zelph+> tower height "+113"^^xsd:decimal
    zelph> height zelph:expand-objects zelph:value
    zelph> tower height V, V zelph:value N
    Answer: tower height "+113"^^xsd:decimal, "+113"^^xsd:decimal zelph:value &113
    zelph> (X instance-of city, X founded V) => (V zelph:expand zelph:interval)
    ```

A request derived by a rule is a derived fact accompanied by a justification, and the expansion it causes is itself derived from that fact, so `.explain` provides the reason a value was expanded. From Janet, `(zelph/expand nodes predicate)` performs the expansion immediately, leaving no request within the graph.

### When Requests Are Answered

Requests are answered by a pass that runs on the session’s thread, where Janet runs:

- following each import;
- following each input line, before automatic inference runs;
- after each fixpoint within a run, and then again with the run until a pass writes nothing new;
- when an expander or a name reader is registered.

Within a module, the pass remains idle until the module has been read. On a network that was partially loaded, it does not execute, accompanied by a note.

**The pass counts as a rule.** A rule may negate a condition based on an expansion predicate, and the conclusion it reaches must not be overtaken by a subsequent expansion. Inference therefore treats the pass as a rule in its own right: it executes before each level of negation, at the end of the positive rules beneath it, and an expander indicates during registration which predicates it writes. A rule that derives a request and negates over the predicates that request will write is not stratifiable, just as any rule that negates its own consequence is: `.strata` names it ([Stratified Evaluation](../logic.md#stratified-evaluation)).

**A request lacking an expander remains idle.** Following a `.load`, the requests reside within the graph while the registrations are gone. The requests remain unresolved, the pass indicates the count, and importing the modules registers the expanders and answers them.

**The pass reports**, by datatype: number of values expanded, count of ill-typed values, number of values lacking an expander, and instances where an expander failed.

The pass incurs no cost as long as the graph holds no request: it returns after a single check. Once requests are present, it reads a record of the statements added for the requested predicates since its previous execution, ensuring its operation follows what is newly introduced rather than the graph’s overall size ([Engine Requirements](engine.md)).

## Strings

The expansion of a string is the list of its characters as zelph builds one today, using `zelph/list-chars` or the compact `<abc>` form:

- **One node per character**, each the node that carries the character as its name within the `zelph` language, regardless of the current session language.
- **Stored in reverse**: the outermost cell holds the last character. `"abc"` is the list `<c b a>`.
- **Ended by `nil`**, the empty list, as every list does.

The last point is a deliberate choice and not a coincidence. A string might conclude with a node of its own, and no string would then share cells with anything else. It concludes at `nil` because the list built from `cons` and `nil` is one of the foundations of zelph, equally foundational for numerals, for the arguments of terms, and for the collections of RDF; introducing a second kind of list for strings would trade that unity for a convenience. The consequence is accepted intentionally: the character list of a string of digits is, cell by cell, a numeral.

```
zelph> .import decimal-arithmetic
zelph+> %(zelph/fact (zelph/list-chars "123") "tag" "fromstring")
<zelph/node &123 tag fromstring>
zelph> &123 tag X
Answer: &123 tag fromstring
...
```

The specific numeral involved hinges on the substrate: within `binary-arithmetic`, the character list of `"101"` is `&5`, whereas that of `"123"` is not a numeral at all. Value nodes remain distinct in every case, and the predicate in the expansion statement says which reading a rule holds ([Expansion Statements](#expansion-statements)).

**Display.** A character list is displayed as zelph displays lists today, in one of three ways:

```
zelph> .import decimal-arithmetic
zelph+> %(zelph/fact "s1" "v" (zelph/list-chars "abc"))
<zelph/node s1 v <c b a>>
zelph> s1 v X
Answer: s1 v <c b a>
zelph> %(zelph/fact "s2" "v" (zelph/list-chars "123"))
<zelph/node s2 v &123>
zelph> s2 v X
Answer: s2 v &123
zelph> %(zelph/fact "s3" "v" (zelph/list-chars "0123"))
<zelph/node s3 v <0123>>
zelph> s3 v X
Answer: s3 v <0123>
...
```

- as a list of its elements in stored order, `<c b a>`, in general, and always when the session has registered no digits;
- as a numeral, `&123`, when every element is a registered digit and the most significant one is not zero; the string `"0"` is `&0`. The numeral displayed is the value represented by the digits within the session’s substrate;
- in the compact form and in written order, `<0123>`, when every element is a registered digit and the first one is zero.

**Characters that are core spellings.** The character `~` is the node denoted by the name `~`, and that is the core node of the instance relation; `!` is the core node of contradiction. A character list containing them includes these core nodes as elements. Nothing follows from it regarding the list, whose elements are only held, not stated; a rule that collects everything a core node occurs in will meet such lists.

**Characters that XML does not allow.** The value space of `xsd:string` comprises the characters permitted by XML, and the mapping takes the `Char` production from XML 1.0 for these, as allowed by XSD 1.1. The string expander checks that: a string containing a character outside this set – U+0000, any other character below U+0020 except tab, line feed, and carriage return, U+FFFE, or U+FFFF – is ill-typed and gets no expansion. Its value node remains present, retaining every character from its text.

**The empty string** expands to the empty list, `nil`: a list containing no characters is the empty list. It is the identical node as `<>`, as the empty set `{}` and as the empty RDF collection, and a rule over the expansion of strings must account for this. Two details of today’s engine shift accordingly: `(zelph/list-chars "")` returns Janet’s nil today, upon which `zelph/fact` silently writes nothing, and it will answer the node `nil`; and `nil` accepts memberships today (`x in nil`), which it will refuse, just as a non-empty set constant refuses them. The arithmetic of the standard library reads `nil` as zero; that the expansion of the empty string and the number zero share a node is an additional justification for the two predicates.

**A side remark on the ACE front end.** The specified [ACE front end](../ace-spec/decisions.md#dec-data) writes strings as character lists as well, and it writes the empty string as a distinct node, `ace:empty-string`. That node is a concession to the front end’s shapes rather than the general answer, which is the empty list, as here: in its shapes, `nil` serves as the object of a verb lacking an object, meaning that "John says." and a sentence expressing the empty string would represent the same fact, so the front end can take the empty list only once its shapes mark a missing object in another way. Adjusting the front end falls beyond the scope of the mapping, which leaves it unchanged; until then, a bridge from RDF strings to the front end’s shapes maps the empty string to `ace:empty-string` and never to `nil`.

## The Substrate

An expansion incorporating numbers is built from the numerals of the session’s arithmetic **substrate**: `&113` is the list `<113>` when using `decimal-arithmetic` and the list `<1110001>` when using `binary-arithmetic`, and with no substrate imported, numerals do not exist at all. A stored expansion statement therefore means a number only in conjunction with the substrate under which it was constructed.

Today nothing records it. A `.bin` carries the digit tables and the rules of its substrate as facts, yet does not indicate which substrate it is; a session that loaded a network built under a different substrate computes with incorrect digits and says nothing. The mapping records the substrate:

- **The record.** The graph notes which module provides its arithmetic, and the `.bin` file embeds this information in its header ([Storage](storage.md#records-in-the-header)). It survives `.cleanup` and a save of selected predicates.
- **`.import`** of the recorded substrate is permitted, even multiple times. A different substrate is refused at the top level. Multiple modules from the standard library import `binary-arithmetic` unless a substrate is already present (`integer-arithmetic`, `primes`, `symbolic-core`, and others); such nested imports are fulfilled via the recorded substrate, with a note, exactly as nested imports are skipped today when a substrate has already been imported.
- **`.load`** of a network whose recorded substrate matches the session’s causes no change. A network recorded under a different substrate is loaded with a warning, and the session’s digit reader is withdrawn until the recorded substrate is imported: `&113` is an error in between, whereas today it would silently construct an incorrect numeral. A network lacking a record, every network saved prior to the mapping, loads as it does today.

When handling dates, the order of imports is crucial: `decimal-arithmetic` must come first, followed by `integer-arithmetic`. Reversing this sequence introduces the binary substrate, where a day number consists of 22 digits, compared to 7 digits in decimal.

## Save and Load

| | `.save` | After `.load` |
|---|---|---|
| value nodes | saved, along with the value store | there |
| expansion statements | saved, just as every fact | there; they mean numbers under the recorded substrate |
| requests | saved, just as every fact | there, and pending until the expanders are registered |
| registrations of expanders, name readers, displays | not saved | absent following a `.load` that replaces the graph; importing the modules restores them |
| the substrate record | saved | read before the graph is replaced |

A value whose expansion was performed retains its expansion and undergoes no further expansion. A value that a request covers and that was added during a period when no expander was registered is expanded upon the import of modules.

## Reading Without Expanding

Three aspects of a value require neither expansion nor a request: its language tag, its datatype, and its lexical form. The engine answers these directly from the value store, as conditions within rules and queries ([Querying Terms](querying.md#conditions-the-engine-answers)). A rule that selects all values of a specific datatype, or all strings in a particular language, costs no node.
