# Collections and Lists

RDF writes a list as a chain of cells: each cell is a resource featuring an `rdf:first`, its element, and an `rdf:rest`, pointing to the subsequent cell, and the chain ends at `rdf:nil`. zelph maintains its own lists: a list is a term built from `cons` facts and terminates at `nil`, and two lists sharing identical elements are a single node. This page specifies how the first form becomes the second. The conversion has not been implemented yet. Since blank nodes are implemented, what the page says about a list that holds a blank node already holds today for what a script writes. A sentence regarding the present engine says "today".

## One Kind of List

A well-formed RDF collection becomes a zelph list. The rationale for this is not a convenience: the list built from `cons` cells is integral to zelph’s architecture, the very structure Lisp popularized, and is used by numerals, character lists, and the arguments of terms alike. RDF must not bring a second kind of list into the graph beside it. The price is also detailed on this page: a zelph list is a value, whereas an RDF collection is a resource, meaning certain distinctions from the source do not survive.

The correspondence is direct:

| RDF | zelph |
|---|---|
| a cell possessing `rdf:first` E and `rdf:rest` R | the term `(E cons R)` |
| `rdf:nil` | the core node `nil`, the empty list |
| the collection `( a b c )` | the list `<a b c>`, which is `(a cons (b cons (c cons nil)))` |
| the empty collection `( )` | `nil` |

`rdf:nil` is the node `nil` in every reader, even in contexts where no list is involved. Today, `nil`, the empty node list `<>`, the empty set `{}`, and the empty collection literal `@{}` are already unified as a single node; the empty RDF collection becomes part of this unity.

The elements of the list are the nodes of the collection’s elements within the rest of the mapping: an IRI node, a blank node, a value node, or a different list. A collection of numbers written in Turtle, `( 1 2 )`, is therefore a list comprising two value nodes, `"1"^^xsd:integer` and `"2"^^xsd:integer`, rather than a list of two numerals.

## The Conversion Is a Step in the Graph

A list can be constructed solely from its final element toward its initial one, since the node of a `cons` cell is determined by its element and its rest. A reader that built lists independently would need to retain every chain until it has seen its end, and in N-Triples the cells of a chain may be dispersed throughout the whole document. The conversion is therefore not included within any reader:

1. **Readers write what they read.** Each reader writes `rdf:first` and `rdf:rest` triples as ordinary facts, using blank nodes as cells; its memory remains constant, and it is capable of reading from a pipe.
2. **One step converts.** At the end of an import, a step operating on the graph finds the well-formed chains among the facts written by this import, constructs the list for each, and puts the list in the position previously occupied by the head of the chain.

So there is a single implementation for all syntaxes, and it also serves chains that were written via a script or through a rule.

**The shortcut.** The collection syntax of Turtle and TriG, `( … )`, and `@list` in JSON-LD deliver a fully formed list at the moment of the closing token. At that point, the reader constructs the list directly, starting from its last element, and writes no cells whatsoever. This occurs exclusively where the result cannot differ from what the step would give: the collection is not the subject of an `rdf:first` or `rdf:rest` statement within the same block, is not the object of an `rdf:rest`, and holds no labelled blank node. In all other cases, it writes cells and defers their processing to the step. A differential test ensures both approaches align: across the W3C test suites and within zelph’s own corner tests, the graph must remain identical whether the shortcut is enabled or disabled (`shortcut=off`, [Parameters](import.md#parameters)).

### When the Step Runs

- At the end of every successful RDF import and of every [`turtle` block](notation.md#the-turtle-block): subsequent to the reader’s last write and before the import’s reports, before requests are answered and before any inference runs. It judges the chains that hold at least one fact written by this import.
- On request: `.lists` (Janet `(zelph/lists)`, C `zelph_lists`, Rust `Engine::lists`) judges every chain within the graph.
- Not when an import specifies `lists=defer`. A document imported in multiple segments requires this: a chain might terminate in a subsequent segment, thus each segment is imported using `lists=defer`, and a `.lists` following the final one performs the conversion.

The step never executes independently following a typed line or preceding a run, so a chain written by a script through `rdf:first` and `rdf:rest` statements stays as statements until `.lists` is called. It does not run after a failed import, whose input might be truncated, and it is refused on a network that was partially loaded, where a statement naming a cell could reside in a part that was not loaded.

## Which Chains Are Converted

A **cell** is a blank node that serves as the subject of exactly one `rdf:first` statement and precisely one `rdf:rest` statement, each having a single object. No statement is made regarding either of these two statements, and the cell is never employed as a predicate.

A chain is identified starting from its terminal point. The step starts at every statement `C rdf:rest rdf:nil` where the subject is a cell and walks back: from a cell to the cell whose `rdf:rest` names it. It takes the step back from a cell `C` to its predecessor `P` only if

- `P` is a cell,
- the sole statement that names `C` as an object is `P`’s `rdf:rest`, and
- `C` serves as the subject of no statement except its own `rdf:first` and `rdf:rest`, and carries no name. One further statement is tolerated: `C rdf:type rdf:List`, which RDFS logically implies for every cell anyway; it is discarded along with the cell.

At the point where the walk stops, the chain’s **head** is located. The head must be a cell, and no additional requirements are imposed on it: any number of statements may name it, and it may serve as the subject in further statements. What gets converted is thus the longest well-formed segment at the end of a chain, the **maximal suffix**. JSON-LD’s transformation from RDF proceeds by walking back from `rdf:nil` in the same way; it enforces a stricter condition on the head, and the two differ regarding permissible input (for `( "1" "2" ) :p :o` JSON-LD converts only the cell of `"2"`, since the first cell carries another statement, whereas zelph converts the whole list).

Everything else stays as the statements it is:

- a chain failing to terminate at `rdf:nil`, or one that leads back into itself via `rdf:rest`;
- a cell that is an IRI and not a blank node;
- a cell possessing two `rdf:first` statements, or possessing none;
- the part of a chain preceding a cell that another statement names, or that carries a statement of its own. That cell becomes the head of the converted suffix, and the cell immediately before it keeps an `rdf:rest` statement whose object is now a list.

**Cycles through elements.** A list cannot contain itself: its node is determined by its elements. Should the element of a given cell be the head of a chain that in turn includes the first chain, no order exists in which the lists could be built. For such chains the step converts the longest suffix that is feasible to build, meaning it drops cells from the front until no element on the cycle serves as the head of a chain within the cycle; the cells at the front stay as statements.

## What the Conversion Does

For every chain, in this order:

1. **Check.** The list must not contain the node it is about to replace: a head appearing within its own list would necessitate replacement within the term used to replace it. Such a chain remains as statements and is tallied. This verification occurs first, since a chain that is dismantled and later cannot be rebuilt would lose asserted facts.
2. **Build.** Lists are constructed starting from the innermost level: when an element serves as the head of another convertible chain, the list of that chain is formed before.
3. **Remove.** The `rdf:first` and `rdf:rest` facts associated with the chain and its inner cells are removed.
4. **Replace the head.** The head is merged into its list: every statement that named the head now names the list instead, at any level of nesting, and equal statements that emerge are combined into a single instance. A head that no statement names is outright eliminated.

The replacement operation is what `.name` performs today when merging two nodes: the engine rebuilds every statement that depends on the merged node, and when an atom becomes a term it marks no predicate as one that may hold wider statements, because no sets of objects are combined, and leaves its fast paths armed. The conversion employs a distinct entry within that mechanism, which needs no name and accomplishes in one step what a merge does per node, thereby ensuring a conversion leaves no footprint on the cost of subsequent operations ([Engine Requirements](engine.md#list-conversion)).

Each RDF import declares `rdf:first` and `rdf:rest` as relations from its outset, ensuring that a conversion never removes the last fact that established either of them.

### The Report

The step reports, on the diagnostic channel: chains converted, cells removed, chain facts absorbed, heads replaced and heads dropped, dependent statements rebuilt and folded, and the chains it left, by reason (no `rdf:nil`, a cycle through `rdf:rest`, a cycle through elements, a head inside its own list, a cell that is not a blank node, a cell without exactly one `rdf:first` and one `rdf:rest`, a further statement with a cell as its subject or object, a named cell).

### Labels That a Conversion Used Up

In a script file, a blank-node label stays valid up to the point the file ends or a line changes the identity of the file ([Blank Nodes](blank-nodes.md#in-a-script)). If a `turtle` block converted the chain that `_:c` was a cell of, `_:c` ceases to exist, and any subsequent line writing `_:c` would silently create a new, unrelated node. Such a line is refused with a message that names the list the cell was incorporated into. A head keeps its meaning: its label now refers to the list.

## What Follows from a List Being a Value

These outcomes are integral to the decision, and anyone working with imported data must be aware of them:

- **Equal collections are one node.** Two collections sharing identical elements are the same list, within a single document and across multiple documents, provided no element is a blank node. "One list used in two places" and "two lists that happen to be equal" are indistinguishable thereafter.
- **A statement about a collection holds for every equal one.** The assertion `( 1 2 ) ex:source ex:a` applies to the list `<"1"^^xsd:integer "2"^^xsd:integer>` at every occurrence.
- **A triple with the same collection on both sides is a statement with itself as an object.** The triple `( 1 ) ex:p ( 1 ) .` becomes such a fact; zelph prints it in full, its predicate being an IRI, and with the self-fact prefix where the predicate is a name ([Display](notation.md#display)).
- **Tails are shared.** The list `<b c>` is the rest of both `<a b c>` and `<x b c>`. A cell from the source does not map to any node, and the number of `cons` cells may differ from the total number of cells in the source.
- **A list that holds a blank node is as local as that node.** Its identity incorporates that of the blank node, and thus that of the document.
- **`rdf:nil` as a predicate** makes `nil` a relation. It is harmless and mentioned for completeness.

A chain preserved as statements possesses none of these properties: its cells are blank nodes, and the graph holds it as RDF wrote it.

## Costs

The cost of the step was measured on a prototype from 5 November 2026, using the engine from that day: 400,000 cells across 100,000 chains required 21.9 s during the step and 1.04 s via the shortcut, with identical graphs; replacing a head took 134 to 139 µs in a graph featuring three relations and 7.6 to 8.3 ms in one with 12,000, as each substitution reconstituted the set of relation types. The figures highlight two essential aspects an implementation must address correctly: the shortcut for collection syntax, which Turtle and JSON-LD sources rely on almost exclusively, and a replacement mechanism that avoids rebuilding unchanged components. Wikidata’s dumps contain no RDF lists at all.

While a chain remains composed of statements, each of its triples is a fact in the same manner as any other ([Costs](costs.md)).

## Writing, Display, Storage and Interfaces

A list is written in a script according to how zelph writes lists today, `<a b c>` with spaces separating the elements, and each element appearing in its own form: `< <http://example.org/a> "1"^^xsd:integer _:b0 >` is a list comprising an IRI node, a value node, and a blank node ([Writing and Displaying Terms](notation.md#lists)). Within a `turtle` block, the Turtle form `( … )` is available. A list is displayed in the identical node-list form and reads back as the same node. A `.bin` file stores it as the `cons` facts it consists of, without containing any section of its own, and `zelph/list` builds one from Janet as is done today.

How the mechanisms of rules, native queries, and SPARQL reach the elements within a list is detailed in [Querying Terms](querying.md#lists).

## What the Graph Preserves

| Of the collection | Kept | Remark |
|---|---|---|
| The elements and their order | yes | |
| The statements that name the collection | yes | they name the list |
| The cells as resources | no | a converted chain has no cells |
| That two equal collections were two resources | no | one node |
| `rdf:type rdf:List` on a cell | no | dropped; RDFS entails it |
| Whether the source wrote `( … )` or explicit cells | no | both give the same graph |
| A chain that is not well-formed | yes, as statements | nothing of it is changed |
