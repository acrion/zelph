# Querying Terms

This page specifies how rules, native queries, and SPARQL queries reach what the mapping brings into the graph: how a literal is chosen via its lexical form, its datatype, or its language tag; how an IRI is told from a string; how labels are read even though they are names; how the elements of a list are reached; and what the comparison and ordering of values mean in SPARQL. Nothing on this page has been implemented yet; a sentence about the present engine says "today", and a transcript outside a box is a real run of the present engine.

## Terms in Patterns

A term written within a rule or a query refers to its node, and a pattern matches through nodes:

!!! example "Specified – not yet implemented"

    ```
    zelph> X born "1952-03-11"^^xsd:date
    Answer: <http://example.org/adams> born "1952-03-11"^^xsd:date
    zelph> X homepage <http://example.org/adams>
    zelph> X note "http://example.org/adams"^^xsd:string
    ```

- A literal within a pattern matches precisely that RDF term and nothing else: `"1952-03-11"^^xsd:date` fails to match `"1952-03-11Z"^^xsd:date`, a distinct literal whose expansion yields the same day, or the string composed of the same characters. Values are compared via their [expansions](expansion.md), not through the value nodes themselves.
- A variable positioned in place of a literal binds the value node, like any node.
- No variable can reside within a literal. Its parts are reached through the conditions outlined in the next section.

### An IRI or a String

`<http://example.org/adams>` and `"http://example.org/adams"^^xsd:string` are two nodes of distinct kinds, an [IRI node](iris.md) and a [value node](literals.md). A pattern that names the one never matches the other: the second and third query shown above name two different nodes. When a variable has bound a node of unknown kind, the conditions below tell: only a value node possesses a datatype.

## Conditions the Engine Answers

Text, tag, and datatype of a literal are not facts. They live in the [value store](literals.md#the-value-store), and labels are stored in the name tables. The engine answers four conditions from there, in rules and in queries:

| Condition | Subject | Object the engine supplies |
|---|---|---|
| `V zelph:lang T` | a language-tagged string | the tag, in lowercase, as a plain string |
| `V zelph:datatype D` | a value node | the IRI node corresponding to its datatype |
| `V zelph:lexical L` | a value node or an IRI node | the lexical form, or the full IRI, as a plain string |
| `X rdfs:label L` | a node possessing a name in a label language | the label as a language-tagged string; see [Labels](#labels) |

They are evaluated by the engine in the same way as `!=` and the [numeric comparisons](../logic.md#numeric-comparisons) today: by the engine itself, without a fact behind them, and without writing one.

!!! example "Specified – not yet implemented"

    ```
    (X motto V, V zelph:lang "de"^^xsd:string) => (X has-german-motto yes)
    X born V, V zelph:datatype xsd:date
    X born V, V zelph:datatype D
    X P V, V zelph:lexical "42"^^xsd:string
    X rdfs:label L, L zelph:lang "de"^^xsd:string
    ```

The rule selects strings based on their tag. The first query lists the `xsd:date` values associated with a property, the second lists every value of it along with its datatype, the tag or the datatype treated as a variable just like any other. The third finds every statement whose object is written `42`, regardless of its datatype. The last requests the German label of every node.

- **Test or generate.** When both sides are bound, a condition acts as a test. When the subject is bound and the object is a variable, it generates the object. `X rdfs:label L` also functions by retrieving every node that carries a bound label. No condition enumerates all values in the graph: when neither side is bound, it awaits another condition to bind one, and a rule or query that binds neither is refused at its first evaluation, once and with a message, just as a comparison with an unbound term is refused today ([Numeric Comparisons](../logic.md#numeric-comparisons)).
- **Not through a variable predicate.** A pattern `S P O` with a variable predicate retrieves only stored statements. It never invents `zelph:lang` or `rdfs:label` statements from the stores.
- **The kind of the object is checked.** The object of `zelph:lang` and `zelph:lexical` must be a plain string, while that of `zelph:datatype` must be an IRI node. A rule that writes a name there, such as `L zelph:lang "de"` or `V zelph:datatype date`, can never hold and is refused upon being written, with a hint toward the likely intended form: `"de"^^xsd:string`, `xsd:date`.
- **Not as statements.** `zelph:lang`, `zelph:datatype`, and `zelph:lexical` are refused as statements and as consequences of rules: a stored fact could conflict with the store. `rdfs:label` differs, since a label that did not become a name is a stored statement ([Names](names.md#labels-that-do-not-become-names)); the condition answers from the names and from such statements together, each label once.
- **Objects that are no nodes yet.** The string `"de"` provided by `zelph:lang`, or the label of a node that is a name, need not yet exist as a value node. The engine binds such an object provisionally, based on its content: it is compared, joined, and printed like a value node, and only becomes one when a consequence of a rule writes it into a fact. A query therefore leaves no node behind for the values it merely shows.
- **In explanations** a premise answered in this way is marked as read from the store or from the names, as a comparison is marked `[compared]` today.

### Labels

`X rdfs:label L` reads names. For a node `X` it supplies, for each label language in which `X` possesses a name, the label as a language-tagged string, strictly per language and never via a [fallback chain](names.md#fallback-chains); with a second condition on the tag, it chooses a single language. The languages `zelph`, `wikidata`, and the other reserved ones are not label languages and never appear.

This is how labels maintain a cost of only names: a rule applied to the English labels of the full network reads its 83,261,799 names in `en` ([Binary Data Files](../binaries.md)) and creates no node for any of them, whereas storing each label as a statement would cost a node and a fact per label ([Costs](costs.md)).

Two consequences are explicitly outlined:

- **A fact derived from a label is not withdrawn when the name is updated.** Derived facts are never retracted in zelph. A run following `.name`, `.delname`, or a label import sees the new names, and rules concerning labels are re-evaluated for nodes whose names were altered.
- **Names are found in a single language only.** A rule seeking the label "in German, or else in any language" must specify this explicitly, using two conditions or two rules; the engine does not guess.

## Lists

A list is a term formed from `cons` facts ([Collections and Lists](collections.md)), and a pattern takes it apart just like any other term: `(E cons R)` matches a list where the initial element is `E` and the remaining part is `R`. Three rules reach every element. This process operates today, and it is the method by which the elements of an imported RDF collection are reached as well:

```
zelph> cake ingredients <flour sugar eggs>
zelph> (X ingredients L) => (X tail L)
(cake tail <flour sugar eggs>) ⇐ (cake ingredients <flour sugar eggs>)
zelph> (X tail (E cons R)) => (X ingredient E)
(cake ingredient flour) ⇐ (cake tail <flour sugar eggs>)
zelph> (X tail (E cons R)) => (X tail R)
(cake tail <sugar eggs>) ⇐ (cake tail <flour sugar eggs>)
(cake ingredient sugar) ⇐ (cake tail <sugar eggs>)
(cake tail < eggs >) ⇐ (cake tail <sugar eggs>)
(cake ingredient eggs) ⇐ (cake tail < eggs >)
(cake tail nil) ⇐ (cake tail < eggs >)
zelph> cake ingredient E
Answer: cake ingredient flour
Answer: cake ingredient sugar
Answer: cake ingredient eggs
```

The rules walk only the lists that a statement of the chosen predicate holds. That matters, because a graph with expanded values is full of lists: each numeral and each character list is one.

## SPARQL

zelph provides answers to SPARQL via the module `sparql`, which translates a query into zelph’s native primitives ([SPARQL Queries](../sparql.md)). The mapping alters the semantics of the query’s terms.

### Today

The SPARQL front end contains no literals. A string within a query is the node bearing that name in the session language, with any language tag discarded; a typed literal constitutes a syntax error; `lang()` always answers the empty string, and `datatype()` does not exist:

```
zelph> .lang wikidata
wikidata> Q1 P1 Berlin
wikidata> Q1 P1 Q2
wikidata> .import sparql
SPARQL subset loaded. Type 'sparql', paste your query, finish with an empty line.
wikidata> sparql
SELECT ?s WHERE { ?s wdt:P1 "Berlin"@de }

?s
Q1
-- 1 result(s) --
wikidata> sparql
SELECT ?o WHERE { wd:Q1 wdt:P1 ?o . FILTER(lang(?o) = "en") }

No results.
wikidata> sparql
SELECT ?o WHERE { wd:Q1 wdt:P1 ?o . FILTER(str(?o) = "Q2") }

?o
Q2
-- 1 result(s) --
```

`FILTER` and `ORDER BY` perform comparisons in the host language: two values that both read as numbers are compared as doubles, while all other values are treated as text. Consequently, dates are ordered as strings, `+9007199254740993` is considered equal to `9007199254740994`, and since these two comparison types are combined, the ordering produced by an `ORDER BY` is not even internally consistent. An IRI not included in the built-in prefixes is a name in the session language, written as the IRI, and a `PREFIX` declared within a query is overridden by a built-in prefix with the same name.

### Terms

- **A literal in a query is an RDF term.** `"Berlin"@de` is the value node bearing the tag `de`; it fails to match the node named Berlin, and it fails to match `"Berlin"@fr`. A number written in a query is the typed literal SPARQL specifies it to be, `42` as an `xsd:integer` and `1.5` as an `xsd:decimal`. Queries that relied on strings being names change their meaning: the front end’s tests that pin today’s interpretation are rewritten with the mapping, and the networks whose qualifier layer stores values as names are regenerated ([Wikidata](wikidata-mapping.md)).
- **An IRI** is mapped via the [namespace table](iris.md#the-namespace-table), just as in all other contexts. An IRI not covered by the table is checked in the IRI language, and if no such node exists, it is treated as a name in the session language, preserving compatibility for queries on networks where nodes were previously named using full IRIs. A query never creates a new node.
- **`lang()`, `datatype()` and `str()`** are answered through the three conditions mentioned above: `lang()` of a string lacking a tag returns the empty string, in accordance with SPARQL’s prescription, where the condition `zelph:lang` returns no answer; `str()` of an IRI node yields its IRI, and of a literal its lexical form.
- **`rdfs:label`, `skos:prefLabel` and `schema:name`** in a pattern are answered from names and stored label statements, just as the condition `X rdfs:label L` is; `schema:name` exclusively when the subject is a Wikidata entity, because elsewhere it does not yield a name and only matches its stored statements ([Which Predicates Make a Name](names.md#which-predicates-make-a-name)). The common idiom used in queries for the Wikidata Query Service – optionally matching a label with `FILTER (lang(?l) = "en")` – then selects the English name.
- **A variable predicate** retrieves only stored statements: `?s ?p ?o` does not include labels that are names.
- **A query leaves nothing behind**, as it does today. It never expands a value and never needs to: what it compares, it compares via order keys.

Whether `true` and `false` are read as booleans, whether a declared `PREFIX` overrides a built-in one, and whether a list pattern must be anchored are [open decisions](open-decisions.md#three-points-of-the-sparql-front-end), each accompanied by a recommendation. Until they are decided, the specification assumes the recommended variant of each: `true` and `false` are booleans, a declared `PREFIX` wins, and a list pattern must be anchored.

### Comparing and Ordering

SPARQL comparison must be fast and must leave no facts, hence it does not run the rules of `cmp`. Each datatype features an **order key**, computed from the lexical form through the function responsible for normalization during expansion: a whole number and a scale for the numeric types, the bounds for the date types. A test holds the order of the keys against the standard library’s `cmp` on random pairs, the identical test that already holds the engine’s comparison condition to it, so the three ways of comparing numbers cannot diverge.

- **Families.** Comparisons occur within a family: numeric, date and time, string, boolean, and one family for every other datatype. A comparison across families is a type error, which, within a `FILTER`, drops the row.
- **Numbers are compared exactly**, across all numeric datatypes. `"0.1"^^xsd:decimal = "0.1"^^xsd:double` is false, since the double is a different number ([Numbers](numbers.md#doubles-and-floats)). SPARQL 1.1 converts the decimal to a double first and answers true; the departure is deliberate, and the draft of XPath and XQuery Functions and Operators 4.0 compares the same way (as of 3 November 2026).
- **Identity is something else.** Joins, `DISTINCT`, and `GROUP BY` work on terms: two literals are the same term if their value nodes are identical. `=` within a `FILTER` compares values within a family, and nodes by identity.
- **An IRI is never compared as a number**, regardless of its character sequence.
- **Other datatypes** perform comparison via `=` and `!=` based on terms; the operators `<` and `>` are type errors.
- **`ORDER BY` establishes a total order**: unbound values first, then blank nodes, IRIs, and literals, in accordance with SPARQL’s specification; literals are ordered by family, then by value within a family, and for literals with identical value, by datatype IRI and lexical form. `DESC` produces the precise reverse. Among floating-point values, `NaN` comes first, then `-INF`, the finite numbers, and finally `INF`.

### Dates in SPARQL

The meaning of the comparison operators applied to date and time literals corresponds to the three [relations of intervals](dates.md#the-three-relations):

| Operator | Meaning |
|---|---|
| `<` | certainly before |
| `>` | certainly after |
| `=` | the intervals overlap |
| `<=`, `>=`, `!=` | not `>`, not `<`, not `=` |

The same holds when a `FILTER` compares two interval terms, the objects of `zelph:interval` statements, which is the method used to compare the full values of Wikidata within a query. `ORDER BY` arranges intervals according to their first bound, and then by their last, which is consistent with "certainly before".

This differs from the SPARQL endpoints of Wikidata when a date is less precise than a day: they compare the literal `1990-01-01T00:00:00Z` that stands for the year 1990 as that instant. And `=` is not transitive: 1990 overlaps May 1990 and December 1990, which do not overlap each other. Joins are unaffected, since they rely on the identity of terms.

### Lists in SPARQL

`rdf:first` and `rdf:rest` within a pattern or a path are answered from `cons` cells: `?l rdf:first ?x` matches the term `(?x cons ?r)`, and a path `rdf:rest*/rdf:first` walks a list starting from its head. The patterns also match chains that remained as statements due to being malformed. A backward walk through `rdf:rest` may yield multiple answers, as lists share their tails. A list pattern that is not anchored is refused until the [open decision](open-decisions.md#three-points-of-the-sparql-front-end) regarding it is taken.
