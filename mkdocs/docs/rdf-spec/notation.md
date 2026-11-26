# Writing and Displaying Terms

An RDF term needs to be writable in a zelph script and must be printed in a form that reads back as the identical node. This page specifies both: the forms a script uses for literals, IRIs, and blank nodes, the meaning these forms mean today and therefore what changes, how each type of node is displayed, the two number constants included with the mapping, and the `turtle` block for whole documents. Of this page, the two forms of a blank node are implemented, in a script, on display, and in the export of derivations; all other elements are specified but not yet implemented. A sentence about the present engine says "today", and a transcript outside a box is a genuine execution of the present engine.

## The Forms at a Glance

The forms of literals, IRIs, and blank-node labels are taken from Turtle: each is a spelling that Turtle reads as the identical term, even though not every Turtle spelling is adopted. zelph and Turtle stay two distinct languages throughout: only the forms of single terms are shared, and a whole Turtle document belongs within a [`turtle` block](#the-turtle-block).

| Term | Form in a script | What this spelling is today |
|---|---|---|
| literal with a datatype | `"1952-03-11"^^xsd:date` | a syntax error within a statement, two elements in a set or a spaced list |
| string with a language tag | `"Berlin"@de` | the same |
| plain string | `"Berlin"^^xsd:string` | a syntax error; a quoted `"Berlin"` alone is a name, and continues to be one |
| IRI | `<http://example.org/alice>` | a list of the characters enclosed within the brackets |
| IRI through a prefix | `rdfs:label`, and `ex:alice` after `.prefix ex: …` | a name |
| blank node | `_:b0`; on display `_:@` and sixteen hexadecimal digits | a blank node: this form is implemented |
| list | `<a b c>` | unchanged |
| negative and decimal number | `&-5`, `&13.05` | errors |

## Literals

A literal consists of a quoted string immediately succeeded, without a blank, by a language tag or by a datatype:

!!! example "Specified – not yet implemented"

    ```
    hamburg population "1910683"^^xsd:integer
    hamburg motto "Tor zur Welt"@de
    hamburg founded "0808"^^xsd:gYear
    hamburg nickname "HH"^^xsd:string
    hamburg website "https://www.hamburg.de"^^<http://www.w3.org/2001/XMLSchema#anyURI>
    ```

- **The suffix makes the literal.** A quoted string lacking a suffix is a name, as it is today: `"Berlin"` and `Berlin` refer to the same named node. A plain string is therefore written with its datatype, `"Berlin"^^xsd:string`, which RDF 1.1 specifies as the same term as Turtle’s `"Berlin"`.
- **The tag** takes the form Turtle assigns: letters, followed by sequences of letters and digits joined by hyphens. It is converted to lowercase upon node creation ([Literals](literals.md#identity)).
- **The datatype** is an IRI enclosed in angle brackets or a prefixed name where the prefix is either predeclared or declared ([Prefixes](#prefixes)). `"…"^^rdf:langString` without a tag is refused.
- **Escapes.** Within the quotes of a literal, Turtle’s escape sequences apply: `\t`, `\b`, `\n`, `\r`, `\f`, `\"`, `\'`, `\\`, and `\u` and `\U` with four and eight hexadecimal digits, respectively. An unrecognized escape is an error, as is an escape that names a surrogate code point. A quoted name, in contrast, recognizes only the two escapes `\"` and `\\` and keeps every other backslash: today `"a\nb"` is a name consisting of four characters, and it remains so, whereas `"a\nb"^^xsd:string` holds a line feed. The suffix determines whether a quoted string is a name or a literal.
- **Where a literal ends.** A tag or a datatype terminates at the point where a bare name would terminate. Within braces `{"a"@en "b"@de}` is a set of two literals. A literal commits: after the parser has read the quote and the `@` or `^^`, any malformed content that follows is treated as an error that names the text, never as a second silent reading.
- **Only double quotes.** Turtle’s single-quoted and long strings are accessible within a `turtle` block; on a script line, `'v'@de` stays the name it is today.
- **No variable inside.** `"v"@X` is the tag `x`, not a pattern. A rule or query seeking the tag, datatype, or text as a variable uses the conditions `zelph:lang`, `zelph:datatype` and `zelph:lexical` ([Querying Terms](querying.md#conditions-the-engine-answers)).

Today these spellings are refused in any context where values are separated by blanks, and silently read as two distinct elements whenever separation is not required:

```
zelph> y city "Berlin"@de
Error in line "y city "Berlin"@de": Syntax error: Could not parse statement.
```

```
zelph> x p {"v"@en}
x p {v @en}
zelph> x p X
Answer: x p {v @en}
zelph> M in O
Answer: @en in {v @en}
Answer: v in {v @en}
```

The set holds two members: the name `v` and the name `@en`.

## IRIs

An IRI is written within angle brackets, in the manner of Turtle. The brackets mean an IRI when the text enclosed within them contains no blank and begins with a scheme followed by a colon, a scheme being a letter and then any number of letters, digits, `+`, `-`, or `.`, as specified in RFC 3986:

!!! example "Specified – not yet implemented"

    ```
    <http://example.org/alice> <http://xmlns.com/foaf/0.1/knows> <http://example.org/bob>
    ```

- Such a text must be a valid absolute IRI according to the grammar of Turtle’s `IRIREF`, supporting only the `\u` and `\U` escapes; otherwise, the line is a syntax error. There is no relative IRI in a script line and no base.
- **Everything else within angle brackets keeps its meaning.** `<a b c>` containing blanks is a node list. `<foo>` lacking a scheme remains the compact character list it is today, as does `<"v"@en>`.
- `<wd:Q42>` is the IRI with the scheme `wd`, as Turtle reads it. It is not the item: the item is `Q42` under `.lang wikidata`, `<http://www.wikidata.org/entity/Q42>`, or `wd:Q42` following a prefix declaration.
- A transitive path marker may appear after an IRI just as it may follow a name: `X <http://example.org/partOf>⁺ Y` and `X wdt:P279∗ Y`.

Today each such text is a list composed of its characters, stored in reversed sequence and printed in the order that was originally stored:

```
zelph> x p <http://example.org/a>
x p <a / g r o "." e l p m a x e / / ":" p t t h>
zelph> x p X
Answer: x p <a / g r o "." e l p m a x e / / ":" p t t h>
```

### Prefixes

A prefixed name like `ex:alice` is an IRI solely when its prefix is recognized. In all other cases, it remains what it is today, a name that happens to contain a colon: `wd:Q42`, `p:P2738`, and `wikibase:rank` typed under `.lang wikidata` continue to keep naming the nodes they name now.

- **Four prefixes are predeclared** in every script and every session: `rdf:`, `rdfs:` and `xsd:` with the IRIs assigned by the W3C, and `zelph:` for the engine’s vocabulary ([The Rows](iris.md#the-rows)). Thus, `rdfs:label` and `zelph:lang` refer to the same node regardless of the session language. This corrects a flaw in today’s engine: the comparison `zelph:lt` is identified by its node, which is named in the language `zelph`, and a rule that spells it under `.lang en` generates a distinct English name and silently never fires.
- **A script establishes additional prefixes** using `.prefix ex: <http://example.org/>`, or `(zelph/prefix "ex" "http://example.org/")` in Janet. A declaration remains in effect from its line to the end of the file it appears in; at the prompt it remains in effect for the session. Declarations within a module do not extend to the script that imports the module.
- **The empty prefix** may be declared, yet a token starting with a colon stays the [self-fact prefix](../concepts.md#the-self-fact-prefix) of zelph. The empty prefix therefore only applies after `^^`.
- A prefixed name is first expanded into its IRI, and then mapped like any IRI, via the [namespace table](iris.md#the-namespace-table).

An engine-answered condition checks the kind of the input it receives. `L zelph:lang "de"` names the node called de, which is not a literal, whereas `L zelph:lang "de"^^xsd:string` was meant; such a rule is refused when it is written, accompanied by a suggestion toward the form that was probably meant ([Querying Terms](querying.md#conditions-the-engine-answers)).

## Blank Nodes

`_:b0` refers to the blank node labelled `b0` within the document containing the line, and `_:@` followed by sixteen hexadecimal digits identifies an existing blank node by its unique identifier; both are specified on [Blank Nodes](blank-nodes.md#in-a-script). The Turtle constructs `[]` and `[ … ]` exist exclusively within a `turtle` block.

Both forms are implemented. An underscore immediately followed by a colon begins a blank node, while every other token beginning with an underscore continues to function as a variable. Among the two lines containing an underscore below, the first asserts and the second asks:

```
zelph> .identity http://example.org/doc
zelph> alice knows bob
zelph> _:b0 knows bob
zelph> _b0 knows bob
Answer: _:@068feb5243a0018d knows bob
Answer: alice knows bob
```

## Lists

A list is written as today, `<a b c>` with spaces separating its elements, and each element takes its own form:

!!! example "Specified – not yet implemented"

    ```
    recipe ingredients < <http://example.org/flour> "250"^^xsd:integer _:b0 >
    ```

This is the form of an RDF collection after it has become a list ([Collections and Lists](collections.md)).

## Number Constants

Two forms of the `&` constant are provided with the mapping, since the numbers that literals expand to include negative and fractional ones, and a rule must be able to name them:

| Constant | Node | |
|---|---|---|
| `&-5` | `(neg zint &5)` | a negative integer |
| `&13.05` | `(&1305 dec &2)` | 1305 · 10⁻² |
| `&13.0`, `&13.`, `&+13` | `&13` | a whole value is the numeral |
| `&-0`, `&0.0` | `&0` | |

What comes after `&` is the lexical form of an `xsd:decimal`: an optional sign, followed by digits with an optional decimal point positioned before, within, or after them. There is no exponent; `&1e3` is an error with a hint. The token passes through the same normalization as the expansion of a decimal literal, so a constant and the expansion of a literal with that value are one node ([Numbers](numbers.md#constants)). Today both forms are refused, by the substrate’s reader of digits:

```
zelph> .import decimal-arithmetic
zelph+> x v &-5
Error in line "x v &-5": invalid decimal literal &-5
```

## What Changes for Existing Scripts

The new forms take spellings that zelph accepts today. Each alteration, and the corresponding result discovered:

| Spelling | Today | Under the mapping |
|---|---|---|
| `{"v"@en}`, `@{"v"@en}`, `< "v"@en >` | two members or elements: `v` and `@en` | a single literal |
| `{"1"^^xsd:integer}` | two members: `1` and `^^xsd:integer` | a single literal |
| `<http://example.org/a>` | a compact character list | an IRI |
| `_:b0` | a blank node, as this form is implemented | a blank node |
| `rdf:…`, `rdfs:…`, `xsd:…`, `zelph:…` when interpreted under a session language other than `zelph` | a name in the session language | the IRI node or the engine’s node |
| `&-5`, `&13.05` | errors | numbers |
| `&1\x30` | read as `&10`, via an escape from the host language | an error |

In a statement, within a parenthesized group, and inside a rule, literal forms are syntax errors today, so no alteration in meaning occurs in those contexts. With a blank preceding the tag, `y city "Berlin" @de` persists as the statement with two objects it is today; the glued form is the literal.

No script is known that any of this breaks. On 23 November 2026 the tests, the documentation, and the standard library of the repository were searched for the changed spellings: none occurs outside SPARQL queries, which are read by the SPARQL front end and never arrive at the grammar of zelph. A check of the same kind on 4 November 2026 ran an extended grammar over every statement text of the repository and found the syntax tree of each one unchanged. Therefore, no exception is specified for any particular case, and the two languages are not separated by a mode.

## Turtle Constructs That Keep Their zelph Meaning

A line of Turtle typed as a zelph statement is typically a valid zelph statement that conveys a different meaning. These constructs keep the meaning they hold today:

| Typed | What zelph stores today, and after the mapping |
|---|---|
| `x p o ; q r .` | a single fact containing the five objects `o`, `;`, `q`, `r`, and `.` |
| `x p y .` | a fact comprising the two objects `y` and `.` |
| `x p ( 1 2 3 )` | a fact where the nested fact `1 2 3` serves as the object |
| `x p [ q r ]` | four distinct objects: `[`, `q`, `r`, `]` |
| `x p 'v'@de` | a node bearing the exact name `'v'@de` |
| `x p true`, `x p 42`, `x p -5`, `x p 1.5e3` | nodes named precisely these names |
| `@prefix ex: <http://example.org/> .` | a statement whose subject is `@prefix` |
| `x p << a b c >>` | a list in which the sole element is the list `<a b c>` |

Anyone who writes Turtle does so within a `turtle` block, where all of this carries Turtle’s intended meaning.

## Display

Each node that the mapping brings into existence is printed in the form a script writes it in:

| Node | Printed as |
|---|---|
| value node | `"…"^^datatype` or `"…"@tag`; a plain string as `"…"^^xsd:string` |
| the datatype behind `^^` | `xsd:`, `rdf:` or `rdfs:` along with the local name when the IRI allows it, otherwise `<IRI>` |
| IRI node of the IRI language | `<IRI>`, or `rdf:`, `rdfs:`, `xsd:` and the local name when the IRI allows it |
| node that the namespace table maps onto a name | as every named node is printed today |
| blank node | `_:@` followed by the sixteen lowercase hexadecimal digits of its identifier |
| list | `<a b c>`, with each element in its respective form |
| negative integer, decimal number | `&-5`, `&13.05` within a term ([Numbers](numbers.md#constants)) |

- **Escapes.** Within the quotation marks of a printed literal, `\\`, `\"`, `\n`, `\r`, and `\t` appear as these escape sequences, and every other control character, including U+0000, as `\u` followed by four digits. The two guillemets `«` and `»`, which the engine internally employs to mark names in its output, are written as `\u00AB` and `\u00BB`.
- **Class before name.** A value node and a blank node are identified by their id class before any other consideration. Therefore, they never print as `??`, as an unnamed node does today, nor as the class they are an instance of, which is the manner in which today’s display shows an unnamed subject in a `~` statement. For a blank node this is implemented.
- **In every language.** The forms remain independent of the session language: a node named solely in the IRI language is rendered as `<IRI>` under `.lang en` just as it is under any other.
- **Lists whose elements begin with `<`.** A list is rendered with a space inside its brackets whenever the initial element begins with `<` or the final element ends with `>`, ensuring that `< <http://example.org/a> x >` reads back; an element that would otherwise print as one of `<`, `>`, `<=`, `>=`, or `<=>` is enclosed in quotes. Today, a list composed of two one-element lists is output as `<<a> <b>>`, which is a syntax error if re-typed.
- **Statements with themselves as an object.** A statement where the subject is its object is today displayed using the self-fact prefix, `:p o` for `o p o`. When the predicate is a node that prints as `<IRI>` or with a prefix, the statement is printed in full.
- **A lone term waits.** A line that holds only a literal, an IRI, or a blank node serves as the start of a statement and awaits the remainder, just as a line consisting solely of a name does today. A line that holds only a blank node does so already.
- **Exports.** `.run-export` writes a value node as an object of its own, `{"literal": {"lexical": …, "datatype": …, "lang": …}}`. It also outputs a blank node as a standalone object, `{"blank":"068feb5243a0018d"}` using the sixteen digits of its identifier, regardless of any names associated with the node; this behaviour is implemented.

### Registered Displays

A module may register, for a given datatype, how its values are shown to a reader: `(zelph/register-datatype-display datatype-IRI handler)`, where the handler receives the lexical form and the tag and returns a text, such as `11 March 1952` for `"1952-03-11"^^xsd:date`. This text reads back as nothing. It is therefore employed exclusively in contexts where output is prose rather than notation: within the lines of `.explain`, in the diagrams of `.node`, and in reports generated by a script. Answer lines, deduction lines, and the echo of a statement always print the Turtle form. The handler runs on the session’s thread only; every other place that prints a node, including worker threads, uses the Turtle form.

### The Re-Read Guarantee

A printed term reads back as the same node. The guarantee covers:

- the forms of this page: literals, IRIs enclosed in angle brackets or using a predeclared prefix, blank nodes identified by their id, number constants;
- lists;
- names, within the language that holds them.

It does not cover what zelph prints for a reader today and what the mapping leaves unchanged:

- **A Wikidata item under a different session language** prints as its identifier and its label in a single token. On the published class hierarchy:

    ```
    zelph> .load wikidata-20260309-all-pruned-small-P279.bin
    ...
    zelph-> .lang en
    en-> %(def prof (zelph/lookup "Q215627" "wikidata"))
    <zelph/node Q215627>
    en-> %(def p279 (zelph/lookup "P279" "wikidata"))
    <zelph/node P279>
    en-> ,prof ,p279 X
    Answer: Q215627 P279 "Q3778211 - legal person"
    Answer: Q215627 P279 Q24229398
    Answer: Q215627 P279 Q795052
    ```

    `"Q3778211 - legal person"` is a report format and not a name. The label within it is the first name found in the session language and then, with the mapping, along its chain; an item lacking such a name prints as its bare id, which under `.lang en` is no English name either.

- **A label that several nodes share** prints for each of them and reads back as the first carrier ([Names](names.md#labels-that-several-nodes-share)).
- **A name found through a fallback chain** may read back as a different node: under `.lang en` a node displayed via its `mul` name reads back as the English carrier of that text, if such a carrier exists ([Names](names.md#fallback-chains)).
- **A character list** is printed as a list of its characters, each being a name in the language `zelph`; when entered under a different session language, the same line names other nodes.

Two methods for expanding the guarantee, a notation that specifies a name within a stated language and a concise display of character lists, modify the published output and are [open decisions](open-decisions.md#a-display-that-always-reads-back).

## The `turtle` Block

A script holds Turtle text verbatim within a `turtle` block, which the module `turtle` supplies, just as `sparql` provides the block for queries ([SPARQL Queries](../sparql.md#why-an-import-is-required)):

!!! example "Specified – not yet implemented"

    ```
    zelph> .import turtle
    zelph> turtle
    @prefix ex: <http://example.org/> .
    @prefix xsd: <http://www.w3.org/2001/XMLSchema#> .

    ex:alice ex:knows ex:bob ;
             ex:born "1990-05-01"^^xsd:date ;
             ex:nick "Al"@en .
    .end
    Imported 3 triples: 3 facts written.
    zelph> <http://example.org/alice> P O
    Answer: <http://example.org/alice> <http://example.org/knows> <http://example.org/bob>
    Answer: <http://example.org/alice> <http://example.org/born> "1990-05-01"^^xsd:date
    Answer: <http://example.org/alice> <http://example.org/nick> "Al"@en
    ```

The three predicates behind the semicolons become three distinct facts, each with a single object, rather than forming one fact containing three objects.

**The frame.** A line whose first token is `turtle` opens the block. All content up to a line that holds nothing except `.end` is treated as Turtle text and is not looked at by zelph: blank lines, `#` comments, lines starting with `.` or `%`. A blank line does not terminate the block, unlike in a `sparql` block, since blank lines are ordinary Turtle; therefore, the keyword is registered with an end line via `(zelph/register-keyword "turtle" handler {:end-line true})`, and `.end` is the sole line that no Turtle document can hold outside a long string. Within an open long string, `.end` is considered text, and the block continues; a second `.end` immediately following it ends the block and reports the open string. Using `.end` with no block currently open results in an error. At the prompt, the continuation prompt remains visible until `.end`.

**Parameters.** The rest of the opening line either begins the Turtle text or comprises parameters for the import, which are never valid Turtle: `turtle labels=de,en base=http://example.org/`. The parameters are those defined in [`.import-rdf`](import.md#parameters), except `format=` and `identity=`: the syntax of a block is Turtle, and its document is the script.

**The function of the block** is identical to that of importing a Turtle file, executed via the same reader (`(zelph/read-turtle text options)`), with these differences:

- **All or nothing.** The entire text must be parsed completely before anything is written; a syntax error results in the graph remaining unchanged. The error names the line and column within the block.
- **The document is the script.** A blank-node label within the block refers to the same node that the same label names in the surrounding lines ([Blank Nodes](blank-nodes.md#in-a-script)).
- **Prefixes stay inside.** The block’s `@prefix` and `@base` directives conclude with the block, and the `.prefix` declarations from the script are not accessible within it, ensuring the block functions as an independent Turtle document that can be extracted from the script.
- **No base by default.** A relative IRI lacking `@base` and without `base=` is an error; an imported file uses its fetch location as its base, but a block has no such location, and thus its IRIs must not depend on the script’s position.

At the end of the block, lists are converted, the report is printed, requests are answered, and when automatic inference is enabled, it runs, each occurring precisely once ([Importing RDF](import.md#how-an-import-proceeds)).

**The name.** Following `.import turtle`, a statement line beginning with the token `turtle` opens a block, and a node named `turtle` is output within quotation marks, just as a node named `sparql` is after `.import sparql`. Wikidata contains an item whose English label is "turtle"; in a session where the module has been imported, that item is written as `"turtle"` at the start of a line.
