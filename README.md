# zelph: A Sophisticated Semantic Network System

## Quick Start Guide

### Installation

Choose the method that matches your operating system:

#### 🐧 Linux (Arch Linux)

zelph is available in the [AUR](https://aur.archlinux.org/packages/zelph):

```bash
pikaur -S zelph
```

#### 🐧 Linux (Other Distributions)

Download the latest `zelph-linux.zip` from [Releases](https://github.com/acrion/zelph/releases), extract it, and run the binary directly.
Alternatively, see [Building zelph](#building-zelph) below to compile from source.

#### 🍏 macOS (via Homebrew)

```bash
brew tap acrion/zelph
brew install zelph
```

#### 🪟 Windows (via Chocolatey)

```powershell
choco install zelph
```

### Basic Usage

Once installed, you can run zelph in interactive mode simply by typing `zelph` in your terminal.
(If you downloaded a binary manually without installing, run `./zelph` from the extraction directory).

Let’s try a basic example:

```
Berlin "is capital of" Germany
Germany "is located in" Europe
(*{(X "is capital of" Y) (Y "is located in" Z)} ~ conjunction) => (X "is located in" Z)
```

After entering these statements, zelph will automatically infer that Berlin is located in Europe:

```
«Berlin» «is located in» «Europe» ⇐ {(«Germany» «is located in» «Europe») («Berlin» «is capital of» «Germany»)}
```

Note that none of the items used in the above statements are predefined, i.e. all are made known to zelph by these statements.
In section [Semantic Network Structure](#semantic-network-structure) you’ll find details about the core concepts, including syntactic details.

### Using Sample Scripts

zelph comes with sample scripts to demonstrate its capabilities:

```bash
# Run with the English examples script
./build/bin/zelph sample_scripts/english.zph

# Or try the Wikidata integration script
./build/bin/zelph sample_scripts/wikidata.zph
```

Within interactive mode, you can load a `.zph` script file using:

```
.import sample_scripts/english.zph
```

### Loading and Saving Network State

zelph allows you to save the current network state to a binary file and load it later:

```
.save network.bin          # Save the current network
.load network.bin          # Load a previously saved network
```

The `.load` command is general-purpose:

- If the file ends with `.bin`, it loads the serialized network directly (fast).
- If the file ends with `.json` (a Wikidata dump), it imports the data and automatically creates a `.bin` cache file for future loads.

### Data Cleanup Commands

zelph provides powerful commands for targeted data removal:

- `.prune-facts <pattern>` – Removes only the matching facts (statement nodes).  
  Useful for deleting specific properties without affecting the entities themselves.

- `.prune-nodes <pattern>` – Removes matching facts **and** all nodes bound to the single variable.  
  Requirements: exactly one variable (subject or single object), fixed relation.  
  **Warning**: This completely deletes the nodes and **all** their connections – use with caution!

- `.cleanup` – Removes all isolated nodes and cleans name mappings.

Example:

```
.lang wikidata
A P31 Q8054                 # Query all proteins
.prune-facts A P31 Q8054    # Remove only "instance of protein" statements
.prune-nodes A P31 Q8054    # Remove statements AND all protein nodes (with all their properties!)
.cleanup                    # Clean up any remaining isolated nodes
```

### Full Command Reference

Type `.help` inside the interactive session for a complete overview, or `.help <command>` for details on a specific command.

Key commands include:

- `.help [command]`          – Show help
- `.exit`                    – Exit interactive mode
- `.lang [code]`             – Show or set current language (e.g., `en`, `de`, `wikidata`)
- `.name <node|id> <new_name>` – Set node name in current language
- `.name <node|id> <lang> <new_name>` – Set node name in specific language
- `.delname <node|id> [lang]` – Delete node name in current (or specified) language
- `.node <name|id>`          – Show detailed node information (names, connections, representation, Wikidata URL)
- `.list <count>`            – List first N existing nodes (internal order, with details)
- `.clist <count>`           – List first N nodes named in current language (sorted by ID if feasible)
- `.out <name|id> [count]`   – List outgoing connected nodes (default: 20)
- `.in <name|id> [count]`    – List incoming connected nodes (default: 20)
- `.mermaid <name> [depth]`  – Generate Mermaid HTML file for a node (default depth 3)
- `.run`                     – Full inference
- `.run-once`                – Single inference pass
- `.run-md <subdir>`         – Inference + Markdown export
- `.run-file <file>`         – Inference + write deduced facts to file (compressed if wikidata)
- `.decode <file>`           – Decode a file produced by `.run-file`
- `.list-rules`              – List all defined rules
- `.list-predicate-usage [max]` – Show predicate usage statistics (top N most frequent)
- `.list-predicate-value-usage <pred> [max]` – Show object/value usage statistics (top N most frequent values)
- `.remove-rules`            – Remove all inference rules
- `.remove <name|id>`        – Remove a node (destructive: disconnects all edges and cleans names)
- `.import <file.zph>`       – Load and execute a zelph script
- `.load <file>`             – Load saved network (.bin) or import Wikidata JSON (creates .bin cache)
- `.save <file.bin>`         – Save current network to binary file
- `.prune-facts <pattern>`   – Remove all facts matching the query pattern (only statements)
- `.prune-nodes <pattern>`   – Remove matching facts AND all involved subject/object nodes
- `.cleanup`                 – Remove isolated nodes
- `.stat`                    – Show network statistics (nodes, RAM usage, name entries, languages, rules)
- `.auto-run`                – Toggle automatic execution of `.run` after each input (default: on)
- `.wikidata-constraints <json> <dir>` – Export property constraints as zelph scripts
- `.export-wikidata <json> <id1> [id2 ...]` – Extracts exact JSON lines for Q-IDs (no import)

### What’s Next?

- Explore the [Core Concepts](#core-concepts) to understand how zelph represents knowledge
- Learn about [Rules and Inference](#rules-and-inference) to leverage zelph’s reasoning capabilities
- Check out the [Example Script](#example-script) for a comprehensive demonstration

## Introduction

zelph is an innovative semantic network system that allows inference rules to be defined within the network itself.
This project provides a powerful foundation for knowledge representation and automated reasoning, with a special focus on efficiency and logical inference capabilities.
With dedicated import functions and specialized semantic scripts (like [wikidata.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/wikidata.zph)),
zelph offers powerful analysis capabilities for the complete Wikidata knowledge graph while remaining adaptable for any semantic domain.

### Community and Support

Development of zelph is supported by the [Wikimedia Community Fund](https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Wikidata_Contradiction_Detection_and_Constraint_Integration_(ID:_23553409)).

The project addresses real-world challenges in large-scale ontology management through direct collaboration with the [Wikidata Ontology Cleaning Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Cleaning_Task_Force) and the [Mereology Task Force](https://www.wikidata.org/wiki/Wikidata_talk:WikiProject_Ontology/Mereology_Task_Force).

### Components

The zelph ecosystem includes:

- A core C++ library providing both C++ and C interfaces
- A single command-line binary that offers both interactive usage (CLI) and batch processing capabilities
- API functions beyond what’s available in the command-line interface
- Integration options for languages like Go and Lua through the C interface

The key features of zelph include:

- Representation of knowledge in a semantic network structure
- Rules encoded within the same semantic network as facts
- Support for multi-language node naming
- Contradiction detection and resolution
- Memory-efficient data structures optimized at bit level
- A flexible scripting language for knowledge definition and querying
- Built-in import functionality for Wikidata JSON datasets and general binary save/load

## Core Concepts

### Semantic Network Structure

In zelph, knowledge is represented as a network of nodes connected by relations.
Unlike traditional semantic networks where relations are labeled edges,
zelph treats relation types as first-class nodes themselves.
This unique approach enables powerful meta-reasoning about relations.

### Predefined Core Nodes

zelph initializes with a small set of fundamental nodes that define the ontology of the system. These nodes are available in every language setting (though their names can be localized).

| Core Node                | Symbol        | Internal Name          | Description                                                                                                                              |
|:-------------------------|:--------------|:-----------------------|:-----------------------------------------------------------------------------------------------------------------------------------------|
| **RelationTypeCategory** | `->`          | `RelationTypeCategory` | The meta-category of all relations. Every relation predicate in zelph is an instance (`~`) of this node.                                 |
| **IsA**                  | `~`           | `IsA`                  | The fundamental categorical relation. Used for classification ("Socrates ~ Human") and to link proxies to concepts in compact sequences. |
| **Causes**               | `=>`          | `Causes`               | Defines inference rules. Connects a condition set to a consequence.                                                                      |
| **PartOf**               | `in`          | `PartOf`               | Defines membership in containers (Sets and Sequences).                                                                                   |
| **Cons**                 | `cons`        | `Cons`                 | The fundamental list-building relation (Lisp-style). The subject is the first element (car), the object is the rest of the list (cdr). |
| **Nil**                  | `nil`         | `Nil`                  | The empty list terminator. Marks the end of a cons list.                                                                               |
| **Conjunction**          | `conjunction` | `Conjunction`          | A tag used to mark a Set as a logical AND condition for rules.                                                                           |
| **Unequal**              | `!=`          | `Unequal`              | Used to define constraints (e.g., `X != Y`) within rules.                                                                                |
| **Negation**             | `negation`    | `Negation`             | Used to mark a condition in a rule as negative (match if the fact does *not* exist).                                                     |
| **Contradiction**        | `!`           | `Contradiction`        | The result of a rule that detects a logical inconsistency.                                                                               |

These nodes are the "axioms" of zelph's graph. For example, `~` is defined as an instance of `->` (i.e., "IsA" is a "Relation Type"). This self-referential bootstrapping allows zelph to reason about its own structure.

### Homoiconicity: The Executable Graph

A defining characteristic of zelph is its [homoiconicity](https://en.wikipedia.org/wiki/Homoiconicity): logic (code) and facts (data) share the exact same representation.

In many traditional semantic web stacks (like OWL/RDF), the ontology is *descriptive*. For example, an OWL "cardinality restriction" describes a constraint, but the actual logic to enforce that constraint resides hidden in the external reasoner's codebase (e.g., [HermiT](http://www.hermit-reasoner.com) or [Pellet](https://github.com/stardog-union/pellet)). The operational semantics are external to the data.

In zelph, **the logic is intrinsic to the data**.

* **Rules are Data:** Inference rules are not separate scripts; they are specific topological structures within the graph itself.
* **Math is Data:** Numbers are not opaque literals but Lisp-style cons lists of digit nodes that interact with semantic entities.

This means the graph doesn't just *describe* knowledge; it *structures the execution* of logic. The boundary between "data storage" and "processing engine" is effectively removed. Consequently, importing data (e.g., from Wikidata) can immediately alter the computational behavior or the arithmetic logic of the system, creating a system that is not just a database, but an **executable graph**.

### Facts and Relations

Facts in zelph are represented as triples consisting of a subject, relation type, and object.
The standard relation type is `~`, which represents a categorical relation (similar to "is a" or "instance of").
For example:

```
X ~ Y
```

This means "X is an instance of category Y" or "X is a Y".

#### Working with Custom Relations

zelph can work with any type of relation, not just the standard `~` relation.
Here’s how custom relations work:

```
zelph> bright "is opposite of" dark
 bright   is opposite of   dark
```

In this example, using the interactive REPL, we enter a subject-predicate-object triple.
Neither "bright", "dark" nor "is opposite of" is know to zelph prior this command.
It automatically creates the appropiate nodes and edges in the semantic network.
After doing so, in the second line this topology is parsed and printed to verify the process ran as expected.

Note that when a relation contains spaces, it must be enclosed in quotation marks.

### Nested Expressions and Sets

zelph supports advanced grouping and recursion using parentheses `()`, braces `{}`, and angle brackets `<>`.

#### Parentheses: Nested Facts

Triples can be nested within other triples. A parenthesized expression `(S P O)` evaluates to the **node representing that specific fact** (the relation node). This allows you to make statements about statements:

```
(bright "is opposite of" dark) "is a" "symmetric relation"
```

Here, the subject of the outer statement is the node representing the fact that bright is opposite of dark.

#### Braces: Sets

Braces `{...}` are used to create **unordered sets** of nodes or facts. This is primarily used for defining conditions in rules (see below).

```
{ (A "is part of" B) (B "is part of" C) }
```

Example session:

```
zelph> { elem1 elem2 elem3 }
{ elem1   elem2   elem3 }
zelph> A in { elem1 elem2 elem3 }
A  in  { elem1   elem2   elem3 }
Answer:   elem3    in  { elem1   elem2   elem3 }
Answer:   elem2    in  { elem1   elem2   elem3 }
Answer:   elem1    in  { elem1   elem2   elem3 }
zelph> (*{(A "is part of" B) (B "is part of" C) } ~ conjunction) => (A "is part of" C)
{B  is part of  C A  is part of  B} => (A  is part of  C)
zelph> earth "is part of" "solar system"
  earth    is part of  ( solar system )
zelph> "solar system" "is part of" universe
( solar system )  is part of    universe
  earth    is part of    universe   ⇐ {( solar system )  is part of    universe     earth    is part of  ( solar system )}
zelph>
```

##### Set Topology

A set `{A B C}` creates a **Super-Node** (representing the set itself).
The elements are linked to this super-node via the `in` (PartOf) relation.

* **Syntax:** `{A B}`
* **Facts created:**
    * `A in SetNode`
    * `B in SetNode`

#### Angle Brackets: Sequences

Angle brackets `<...>` create **ordered sequences**. Unlike sets, the order of elements is preserved using the `FollowedBy` (internally `..`) relation.

zelph distinguishes between two fundamental input modes for sequences, which result in different internal topologies:

1. **Node Sequences (Space-Separated):** `<item1 item2 item3>`

* **Syntax:** At least one whitespace between the brackets.
* **Semantics:** The existing nodes `item1`, `item2`, and `item3` become the direct elements of the sequence.
* **Facts created (Lisp-style cons list, built right-to-left):**
    * `Cell2 = B cons nil`
    * `Cell1 = A cons Cell2` (this IS the list)
* **Use Case:** Lists of known entities, e.g., `<Berlin Paris London>`.

2. **Compact Sequences (Continuous):** `<123>` or `<abc>`

* **Syntax:** No spaces between characters.
* **Semantics:** The input is split into individual characters. Each character is resolved to a named node (e.g., `node("1")`, `node("a")`), and these nodes become the direct elements of the cons list.
* **Wikidata Integration:** The element nodes map directly to external knowledge. For example, in a numeric sequence, `node("1")` corresponds exactly to the Wikidata item for the digit 1 ([Q715432](https://www.wikidata.org/wiki/Q715432)). This connects positions in a sequence directly to semantic knowledge about the character.

##### Digits vs. Numbers

A natural distinction between digits and numbers emerges from the cons-list representation:

- The **digit** "4" is the named node `node("4")`.
- The **number** 4 is the cons cell `4 cons nil` — a structurally different node.

This means that the digit concept and the number concept are automatically kept separate without any special mechanism. In a Wikidata context, `node("1")` maps to the Wikidata item for the digit 1 ([Q3450386](https://www.wikidata.org/wiki/Q3450386)), while the cons cell `1 cons nil` represents the number 1 ([Q199](https://www.wikidata.org/wiki/Q199).

This architecture connects the structural representation of numbers to semantic knowledge. A sequence like `<113>` is not just a string of digits — through its element nodes, it is linked to everything known about those digits (e.g., [Q715432](https://www.wikidata.org/wiki/Q715432) which represents the number 113 in Wikidata). Combined with inference rules that manipulate the cons structure, this enables the seamless integration of arithmetic and reasoning described in [Semantic Math](#semantic-math).

##### Sequence Topology

A compact sequence like `<11>` uses Lisp-style cons cells with direct concept node references.

**Topology of `<11>`:**

1. **Concept Node (Digit):** `1` (Named "1", e.g., Wikidata [Q715432](https://www.wikidata.org/wiki/Q715432)).
2. **Structure (Lisp-style cons list, built right-to-left, e.g. Wikidata [Q37136](https://www.wikidata.org/wiki/Q37136):**

* `Cell2 = 1 cons nil` (last element)
* `Cell1 = 1 cons Cell2` (first element — this IS the list)

Note that both cons cells reference the *same* concept node `1` as their car (subject). They are nevertheless distinct nodes because their cdr (object) differs: `Cell2` points to `nil`, while `Cell1` points to `Cell2`.

There is no separate container node — just as in Lisp, the outermost cons cell IS the list. Each cons cell's subject (car) holds an element, and its object (cdr) points to the rest of the list or `nil` for the final element.

#### The Focus Operator `*`

When defining complex structures, you often need to refer to a specific part of an expression rather than the resulting fact node. The `*` operator allows you to "focus" or "dereference" a specific element to be returned.

- `(A B C)` creates the fact and returns the relation node.
- `(*A B C)` creates the fact and returns node `A`.
- `(*{...} ~ conjunction)` creates the fact that the set is a conjunction, but **returns the set node itself**.

This operator is crucial for the rule syntax.

### Semantic Math

As described in [Angle Brackets: Sequences](#angle-brackets-sequences), zelph represents numbers as ordered sequences of digit nodes within the graph (e.g., `<123>`). Each sequence is linked to its Value Concept via `has_value`, connecting it to semantic knowledge about that number. This architecture has two powerful consequences:

1. **Symbolic Math:** Arithmetic operations can be defined as graph transformation rules rather than hard-coded calculations. Since numbers are topological structures (cons lists of digit nodes), you can write inference rules that manipulate them — effectively teaching the network to compute.

2. **Semantic Integration:** Because sequence elements are the same nodes used throughout the knowledge graph, semantic knowledge flows into arithmetic and vice versa. If Wikidata knows facts about the digit 1 ([Q715432](https://www.wikidata.org/wiki/Q715432)), those facts are accessible wherever `node("1")` appears in a sequence. The boundary between *calculating* numbers and *reasoning* about them is removed.

#### Example: Defining Addition (Peano)

In zelph, "math" is just a set of topological rules. Here is how you can teach the network to add 1 to a number, simply by defining a successor relationship and a logical rule:

```
zelph> <0> successor <1>
< 0 >  successor  < 1 >
zelph> <1> successor <2>
< 1 >  successor  < 2 >
zelph> <2> successor <3>
< 2 >  successor  < 3 >
zelph> <3> successor <4>
< 3 >  successor  < 4 >
zelph> <4> successor <5>
< 4 >  successor  < 5 >
... (defining up to 9) ...

zelph> (A successor B) => ((<1> + A) = B)
(A  successor  B) => < 1 >  +  A  =  B
```

The rule states: *If A is followed by B (in the number succession), then '1 + A' equals 'B'.*
Zelph immediately applies this rule to the facts we just entered:

```
< 1 >  +  < 5 >  =  < 6 > ⇐ < 5 >  successor  < 6 >
< 1 >  +  < 2 >  =  < 3 > ⇐ < 2 >  successor  < 3 >
< 1 >  +  < 3 >  =  < 4 > ⇐ < 3 >  successor  < 4 >
...
```

Note that `successor` is a user-defined relation here — it is not a predefined core node. The internal structure of sequences uses Lisp-style cons cells (`cons`/`nil`), while the succession relationship between numbers is expressed through domain-specific relations like `successor`.

#### Advanced Arithmetic with Fresh Variables and Negation

While the above example demonstrates basic Peano-style addition, zelph's advanced features—fresh variables and negation—enable more complex computations, such as digit-wise addition for arbitrary-sized numbers. These features are not specific to arithmetic; they are general-purpose tools for constructive reasoning and absence-based conditions. However, they shine in arithmetic applications by allowing the dynamic creation of new cons-list structures and recursive decomposition of number representations.

**Conceptual Implementation of Digit-Wise Addition:**

Since numbers are represented as Lisp-style cons lists of digit nodes (e.g., `<42>` is `4 cons (2 cons nil)`), digit-wise addition naturally decomposes into recursive processing of the cons structure.

1. **Define Digit Concepts:** Establish the digits and a basic lookup table for single-digit addition (including carry logic).

   ```
   <0> ~ digit
   <1> ~ digit
   ...
   (<0> + <1>) = <1> carry <0>
   ```

2. **Find the Last Digit:** Use the cons structure to find the element at the tail of the list — the cons cell whose cdr is `nil`:

   ```
   (A cons nil) => (A "is last digit of" *(A cons nil))
   (*{(B "is last digit of" _Rest) (A cons _Rest)} ~ conjunction) => (B "is last digit of" *(A cons _Rest))
   ```

   The first rule identifies the last element in a single-element list. The second rule propagates the result up the cons chain: if B is the last digit of the sublist `_Rest`, and `A cons _Rest` extends it, then B is also the last digit of the longer list.

3. **Compute and Generate Results:** Use **Fresh Variables** to create new result nodes. Variables that appear *only* in the consequence (e.g., `R` for a new cons cell) trigger the creation of new entities.

   ```
   (*{(A "is last digit of" _Num1) (B "is last digit of" _Num2) ( (A + B) = S )} ~ conjunction)
   => (S cons nil = R) (_Num1 + _Num2 = R)
   ```

To handle multi-digit numbers with carry, recursive rules would decompose the cons structure (separating car from cdr), apply the digit lookup table, handle carries, and construct new cons cells for the result using fresh variables. This is a natural fit for the Lisp-style representation: just as `car`/`cdr` decompose a list in Lisp, zelph's cons relation allows rules to structurally pattern-match on the head and tail of a number. This approach demonstrates how zelph can perform arbitrary precision arithmetic purely through graph transformations on cons-list structures.

#### Example: Querying Prime Numbers from Wikidata

The seamless integration of semantic knowledge and computation means that algorithms operating on numbers can leverage facts from external knowledge bases — without any special glue code. For instance, if the Wikidata graph is loaded, every number that Wikidata classifies as a [prime number (Q49008)](https://www.wikidata.org/wiki/Q49008) is already connected to the corresponding Value Concept nodes in zelph. A simple query is all it takes:

```
.lang wikidata
X P31 Q49008
```

With a Wikidata dataset loaded (for example, [wikidata-20251222-pruned.bin](https://huggingface.co/datasets/acrion/zelph/tree/main)), this query lists all prime numbers recorded in Wikidata — 10,018 in this dataset. The only requirement for an algorithm to work with prime numbers is to reference the correct node (`Q49008`); everything else works out of the box because the knowledge is already part of the graph. Since sequence elements are the same nodes as Wikidata entities, any arithmetic rule that produces a digit already inherits all semantic facts known about that digit.

This illustrates a key design principle of zelph: knowledge and computation are not separate layers. Any arithmetic rule that produces a number automatically inherits all semantic facts known about that number, and any semantic query can draw on structurally computed results.

### Internal Representation of facts

In a conventional semantic network, relations between nodes are labeled, e.g.

```mermaid
graph LR
    bright -->|is opposite of| dark
```

zelph’s representation of relation types works fundamentally differently.
As mentioned in the introduction, one of zelph’s distinguishing features is that it treats relation types as first-class nodes rather than as mere edge labels.

Internally, zelph creates special nodes to represent relations.
For example,when identifying "is opposite of" as a relation (predicate), this internal structure is created:

```mermaid
graph TD
    n_3["(3) ~"]
    n_1["(1) ->"]
    n_5688216769861436680["«is opposite of» «~» ->"]
    n_10["(10) is opposite of"]
    style n_10 fill:#FFBB00,stroke:#333,stroke-width:2px
    n_5688216769861436680 <--> n_10
    n_1 --> n_5688216769861436680
    n_5688216769861436680 --> n_3
```

The nodes `->` and `~` are predefined zelph nodes. `->` represents the category of all relations, while `~` represents a subset of this category, namely the category of categorical relations. Every relation that differs from the standard relation `~` (like "is opposite of") is linked to `->` via a `~` relation.

The node `is opposite of ~ ->` represents this specific relation (hence its name).
The relations to other nodes encode its meaning.

This approach provides several advantages:

1. It enables meta-reasoning about relations themselves
2. It simplifies the underlying data structures
3. It allows relations to participate in other relations (higher-order relations)
4. It provides a unified representation mechanism for both facts and rules

This architecture is particularly valuable when working with knowledge bases like Wikidata, where relations (called "properties" in Wikidata terminology) are themselves first-class entities with their own attributes, constraints, and relationships to other entities. zelph’s approach naturally aligns with Wikidata’s conceptual model, allowing for seamless representation and inference across the entire knowledge graph.

Similarly, when stating:

```
bright "is opposite of" dark
```

zelph creates a special relation node that connects the subject "white" bidirectionally, the object "black" in reverse direction, and the relation type "is opposite of" in the forward direction.

```mermaid
graph TD
    n_11["dark"]
    n_9["bright"]
    n_8445031417147704759["bright is opposite of dark"]
    n_10["is opposite of"]
    style n_10 fill:#FFBB00,stroke:#333,stroke-width:2px
    n_8445031417147704759 --> n_10
    n_9 <--> n_8445031417147704759
    n_11 --> n_8445031417147704759
```

The directions of the relations are as follows:

| Element       | Example        | Relation Direction |
|---------------|----------------|--------------------|
| Subject       | white          | bidirectional      |
| Object        | black          | backward           |
| Relation Type | is opposite of | forward            |

This semantics is used by zelph in several contexts, such as rule unification. It’s required because zelph doesn’t encode relation types as labels on arrows but rather as equal nodes. This has the advantage of facilitating statements about statements, for example, the statement that a relation is transitive.

zelph also supports **self-referential facts**, where subject and object are the same
node (e.g., `A cons A`). These arise rarely in practice — Wikidata contains a small
number of such entries, for example `South Africa (Q258) country (P17) South Africa
(Q258)`. Internally, the object connection is omitted because the subject is already
connected to the fact-node bidirectionally, which serves as the implicit object
connection. Detection is unambiguous: a fact-node whose left-neighbor set contains
only the subject node (no additional unidirectional incoming connection) is
self-referential.

## Creating a node graph

You can generate a node graph yourself using zelph’s `.mermaid` command, which outputs a Mermaid HTML format file. For example:

```
.mermaid name 3
```

In this example, `name` refers to the node identifier (in the currently active language specified via the `.lang` command) whose connections you want to visualise. The following number represents the depth of connections to include in the graph (default is 3).

To view the Mermaid graph, open the generated HTML file in a web browser.

## Rules and Inference

One of zelph’s most powerful features is the ability to define inference rules within the same network as facts. Rules are statements containing `=>` with conditions before it and a consequence after it.

### Rule Syntax

A rule in zelph is formally a statement where the subject is a **set of conditions** (marked as a conjunction) and the object is the **consequence**.

Example rule:

```
(*{(R ~ transitive) (X R Y) (Y R Z)} ~ conjunction) => (X R Z)
```

**Breakdown of the syntax:**

1. `{...}`: Creates a **Set** containing three fact templates:
    * `R` is a transitive relation.
    * `X` is related to `Y` via `R`.
    * `Y` is related to `Z` via `R`.
2. `~ conjunction`: Defines that this Set represents a logical "AND" (Conjunction). The inference engine only evaluates sets marked as conjunctions.
3. `(*...)`: The surrounding parentheses create the fact `Set ~ conjunction`.
4. `*`: The **Focus Operator** at the beginning ensures that the expression returns the **Set Node** itself, not the fact node `Set ~ conjunction`.
5. `=>`: The inference operator. It links the condition Set (Subject) to the consequence (Object).
6. `(X R Z)`: The consequence fact.

This rule states: *If there exists a set of facts matching the pattern in the conjunction, then the fact `X R Z` is deduced.*

### Negation in Rules

Negation allows rules to check for the **absence** of a fact pattern. This is achieved by linking a fact pattern to the `negation` core node using `~`.

**Syntax:** `(*(Pattern) ~ negation)`

The engine evaluates this by checking if **no** facts match the specified pattern given the current variable bindings.

**Example 1: Logical Negation**
"If the sun is yellow, and there is no fact stating it is green, deduce it is not green."

```
zelph> sun is yellow
 sun   is   yellow
zelph> (*(A is green) ~ negation) => (A "is not" green)
 negation  => (A  is not   green )
 sun   is not   green  ⇐  negation
```

**Example 2: Topological Querying (Finding the last element of a list)**
Negation combined with the cons structure enables analysis of lists. To find the last element,
we look for a cons cell whose cdr is `nil`:

```
zelph> <1 2 3>
< 1   2   3 >
zelph> (A cons nil) => (A "is last of" *(A cons nil))
(A  cons  nil) => (A  is last of  < A >)
 3   is last of  < 3 > ⇐ ( 3   cons   nil )
zelph> (*{(B "is last of" _Rest) (A cons _Rest)} ~ conjunction) => (B "is last of" *(A cons _Rest))
{(A  cons  _Rest) (B  is last of  _Rest)} => (B  is last of  < A  ... >)
 3   is last of  < 2   3 > ⇐ {( 2   cons  < 3 >) ( 3   is last of  < 3 >)}
 3   is last of  < 1   2   3 > ⇐ {( 1   cons  < 2   3 >) ( 3   is last of  < 2   3 >)}
```

The first rule handles the base case (single-element list), the second propagates the result
recursively up the cons chain. This is a general-purpose pattern for any cons-list analysis.

### Fresh Variables (Node Generation)

zelph supports **Generative Rules**, which create entirely new nodes (entities) in the graph.

**Mechanism:**
Variables that appear **only in the consequence** of a rule (and are not bound in the condition part) are treated as "fresh variables." During inference, these trigger the automatic creation of new nodes. This enables constructive reasoning, where rules build new structures dynamically.

**Termination Guarantee:**
The semantics ensure termination: Before creating a new node, zelph checks if the deduced facts (with the fresh variable as a wildcard) already exist in the network. If they do, no new deduction occurs. This check is performed within the live network, so it persists across serialization and loading.

**Example:**
"Every human has a name." (Where the 'name' is a distinct entity generated by the rule).

```
zelph> (A is human) => (B nameof A)
(A  is   human ) => (B  nameof  A)
zelph> tim is human
 tim   is   human
 ??   nameof   tim  ⇐  tim   is   human
```

In the output, `??` represents the newly generated anonymous node. This feature is fundamental for constructive logic, such as building new number sequences in arithmetic or simulating biological reproduction.

### Deep Unification (Nested Matching)

zelph's unification engine supports **Deep Unification**, meaning it can match patterns against arbitrarily nested structures. This is essential for advanced reasoning where statements themselves are the subjects of other statements.

Consider a rule that transforms an arithmetic structure:

```
((A + B) = C) => (test A B)
```

This rule matches any fact where the *subject* is itself a fact `(A + B)` and the relation is `=`.
If the network contains `(3 + 5) = 8`, zelph recursively unifies `A` with `3`, `B` with `5`, and `C` with `8`.

This capability allows zelph to perform symbolic manipulation and structural transformation of data, treating "code" (like mathematical expressions) as graph data that can be queried and transformed.

### Variables and Logic (A Predicate Logic Perspective)

zelph’s logic system can be viewed through the lens of first-order logic:

- **Variables:** Single uppercase letters (or words starting with `_`) act as variables.
- **Universal Quantification ($\forall$):** Variables appearing in the rule are implicitly universally quantified. The rule applies to *all* X, Y, Z, R that satisfy the pattern.
- **Existential Quantification ($\exists$):** A variable that appears *only* in the consequence acts as an existential quantifier (generates a fresh node).
- **Conjunction:** The `~ conjunction` tag explicitly defines the set as an AND-operation.
- **Negation:** The `~ negation` tag explicitly defines a condition as a NOT-operation.

### Examples

Here is a practical example of how this rule works in zelph (which you can also try out in interactive mode):

```
zelph> (*{(R ~ transitive) (X R Y) (Y R Z)} ~ conjunction) => (X R Z)
{(X R Y) (R  ~   transitive ) (Y R Z)} => (X R Z)
```

After the entered rule, we see zelph’s output, which in this case simply confirms the input of the rule.

Now, let’s declare that the relation `>` (greater than) is an instance of (`~`) transitive relations:

```
zelph> > ~ transitive
>  ~   transitive
```

Next, we provide three elements ("4", "5" and "6") for which the `>` relation applies:

```
zelph> 6 > 5
 6  >  5
zelph> 5 > 4
 5  >  4
 6  >  4  ⇐ {( 6  >  5 ) (>  ~   transitive ) ( 5  >  4 )}
zelph>
```

After entering `5 > 4`, zelph’s unification mechanism takes effect and automatically adds a new fact: `6 > 4`. This demonstrates the power of the transitive relation rule in action.

Rules can also define contradictions using `!`:

```
zelph> (*{(X "is opposite of" Y) (A ~ X) (A ~ Y)} ~ conjunction) => !
{(X  is opposite of  Y) (A  ~  X) (A  ~  Y)} =>  !
zelph> bright "is opposite of" dark
 bright   is opposite of   dark
zelph> yellow ~ bright
 yellow   ~   bright
zelph> yellow ~ dark
 yellow   ~   dark
 !  ⇐ {( bright   is opposite of   dark ) ( yellow   ~   bright ) ( yellow   ~   dark )}
Found one or more contradictions!
zelph>
```

This rule states that if X is opposite of Y, then an entity A cannot be both an instance of X and an instance of Y, as this would be a contradiction.

If a contradiction is detected when a fact is entered (via the scripting language or during import of Wikidata data), the corresponding relation (the fact) is not entered into the semantic network. Instead, a fact is entered that describes this contradiction (making it visible in the Markdown export of the facts).

### Internal representation of rules

Rules are not stored in a separate list; they are an integral part of the semantic network. The implication operator `=>` is treated as a standard relation node.

When you define:
`(*{A B} ~ conjunction) => C`

The following topology is created in the graph:

1. A node `S` is created to represent the set of conditions.
2. The conditions `A` and `B` are linked to `S` via `PartOf` relations.
3. A fact node represents `S ~ conjunction` (defining the logical AND).
4. A fact node represents `S => C` (the rule itself).

When the inference engine scans for rules, it looks for all facts involving the `=>` relation. It examines the subject (the set `S`), verifies that `S` is connected to `conjunction` via `~`, and if so, treats the elements of `S` as the condition patterns.

This means that **a rule is completely represented by standard subject-predicate-object triples**, with `=>` serving as a standard predicate.

### Facts and Rules in One Network: Unique Identification via Topological Semantics

A distinctive aspect of **zelph** is that **facts and rules live in the same semantic network**. That raises a natural question: how does the unification engine avoid confusing ordinary entities with statement nodes, and how does it keep rule matching unambiguous?

The answer lies in the network’s **strict topological semantics** (see [Internal Representation of facts](#internal-representation-of-facts) and [Internal representation of rules](#internal-representation-of-rules)). In zelph, a _statement node_ is not “just a node with a long label”; it has a **unique structural signature**:

- **Bidirectional** connection to its **subject**
- **Forward** connection to its **relation type** (a first-class node)
- **Backward** connection to its **object**

The unification engine is **hard-wired to search only for this pattern** when matching a rule’s conditions. In other words, a variable that ranges over “statements” can only unify with nodes that expose exactly this subject/rel/type/object wiring. Conversely, variables intended to stand for ordinary entities cannot accidentally match a statement node, because ordinary entities **lack** that tri-partite signature.

Two immediate consequences follow:

1. **Unambiguous matching.** The matcher cannot mistake an entity for a statement or vice versa; they occupy disjoint topological roles.
2. **Network stability.** Because statementhood is encoded structurally, rules cannot “drift” into unintended parts of the graph. This design prevents spurious matches and the sort of runaway growth that would result if arbitrary nodes could pose as statements.

## Performing Inference

By default, zelph triggers the inference engine immediately after every fact or rule is entered. You can toggle this behaviour using the `.auto-run` command.

**Performance Note:** When working with large datasets, continuous inference can be computationally expensive. Therefore, the `.load` command automatically **disables** auto-run mode to ensure efficient data loading. You can re-enable it manually at any time by typing `.auto-run`.

Queries containing variables (e.g., `A "is capital of" Germany`) are always evaluated immediately, regardless of the auto-run setting.

If auto-run is disabled, you can trigger inference manually:

```
.run
```

This performs full inference: rules are applied repeatedly until no new facts can be derived. New deductions are printed as they are found.

For a single inference pass:

```
.run-once
```

To export all deductions and contradictions as structured Markdown reports:

```
.run-md <subdir>
```

This command generates a tree of Markdown files in `mkdocs/docs/<subdir>/` (the directory `mkdocs/docs/` must already exist in the current working directory).  
It is intended for integrating detailed reports into an existing MkDocs site – this is exactly how the contradiction and deduction reports on <https://zelph.org> were produced.  
For normal interactive or script use, `.run` is the standard command.

### Exporting Deduced Facts to File

The command `.run-file <path>` performs full inference (like `.run`) but additionally writes every deduced fact (positive deductions and contradictions) to the specified file – one per line.

Key characteristics of the file output:

- **Reversed order**: The reasoning chain comes first, followed by `⇒` and then the conclusion (or `!` for contradictions).
- **Clean format**: No `«»` markup, no parentheses, no additional explanations – only the pure facts.
- **Console output unchanged**: On the terminal you still see the normal format with `⇐` explanations and markup.

The command is **general-purpose** and works with any language setting. It simply collects all deductions in a clean, machine-readable text file.

Example session:

```
zelph> .lang wikidata
wikidata> .auto-run
Auto-run is now disabled.
wikidata-> Q1 P279 Q2
 Q1   P279   Q2
wikidata-> Q2 P279 Q3
 Q2   P279   Q3
wikidata-> (*{(A P279 B) (B P279 C)} ~ conjunction) => (A P279 C)
{(B  P279  C) (A  P279  B)} => (A  P279  C)
wikidata-> .run-file /tmp/output.txt
Starting full inference in encode mode – deduced facts (reversed order, no brackets/markup) will be written to /tmp/output.txt (with Wikidata token encoding).
«Q1» «P279» «Q3» ⇐ {(«Q2» «P279» «Q3») («Q1» «P279» «Q2»)}
```

Content of `output.txt`:

```
丂 一丂 七, 七 一丂 丄 ⇒ 丂 一七 丄
```

When the current language is set to `wikidata` (via `.lang wikidata`), the output is **automatically compressed** using a dense encoding that maps Q/P identifiers to CJK characters.
This dramatically reduces file size and – crucially – makes the data highly suitable for training or prompting large language models (LLMs).
Standard tokenizers struggle with long numeric identifiers (Q123456789), often splitting them into many sub-tokens.
The compact CJK encoding avoids this problem entirely, enabling efficient fine-tuning or continuation tasks on Wikidata-derived logical data.

To read an encoded file back in human-readable form, use `.decode`, e.g.:

```
zelph> .decode /tmp/output.txt
Q2 P279 Q3 Q1 P279 Q2 ⇒ Q1 P279 Q3
```

`.decode` prints each line decoded (if it was encoded) using Wikidata identifiers.

## Example Script

Here’s a comprehensive example demonstrating zelph’s capabilities:

```
(*{(X "is a" Y)}  ~ conjunction) => (X ~ Y)
(*{(X "is an" Y)} ~ conjunction) => (X "is a" Y)

"is attribute of" "is opposite of" is
"is part of"      "is opposite of" "has part"
"is for example"  "is opposite of" "is a"

"has part"      is transitive
"has attribute" is transitive
~               is transitive

(*{(R is transitive)   (X R Y) (Y R Z)} ~ conjunction) => (X R Z)
(*{(X is E)               (E "is a" K)} ~ conjunction) => (X is K)
(*{(X "has part" P)       (P "is a" K)} ~ conjunction) => (X "has part" K)
(*{(K is E)               (X "is a" K)} ~ conjunction) => (X is E)
(*{(K "has part" P)       (X "is a" K)} ~ conjunction) => (X "has part" P)
(*{(X "is opposite of" Y) (X "is a" K)} ~ conjunction) => (Y "is a" K)
(*{(X "is opposite of" Y)}              ~ conjunction) => (Y "is opposite of" X)
(*{(R "is opposite of" S) (X R Y)}      ~ conjunction) => (Y S X)

(*{(X "is opposite of" Y) (A is X)         (A is Y)}         ~ conjunction) => !
(*{(X "is opposite of" Y) (A "has part" X) (A "has part" Y)} ~ conjunction) => !
(*{(X "is opposite of" Y) (A "is a" X)     (A "is a" Y)}     ~ conjunction) => !
(*{(X is E) (X "is a" E)}     ~ conjunction) => !
(*{(X is E) (E "is a" X)}     ~ conjunction) => !
(*{(X is E) (E "has part" X)} ~ conjunction) => !

"is needed by" "is opposite of" needs
"is generated by" "is opposite of" generates

"is needed by" "is opposite of" needs
"is generated by"  "is opposite of" generates

(*{(X generates energy)}   ~ conjunction) => (X "is an" "energy source")
(*{(A is hot)}             ~ conjunction) => (A generates heat)
(*{(A generates "oxygen")} ~ conjunction) => (A is alive)

chimpanzee "is an" ape
ape is alive

chimpanzee "has part" hand
hand "has part" finger

"green mint" "is an" mint

"water mint" "is a" mint

peppermint "is a" mint

mint "is a" lamiacea

catnip "is a" lamiacea

"green mint" is sweet

"is ancestor of" is transitive
peter "is ancestor of" paul
paul "is ancestor of" "pius"
A "is ancestor of" "pius"

```

When executed, the last line is interpreted as a query, because it contains a variable (single uppercase letter) and is no rule. Here are the results:

```
zelph> .import sample_scripts/english.zph
Importing file sample_scripts/english.zph...
[...skipped repetition of parsed commands..]
A  is ancestor of   pius
Answer:  paul   is ancestor of   pius
 peter   is ancestor of   pius  ⇐ {( peter   is ancestor of   paul ) ( is ancestor of   is   transitive ) ( paul   is ancestor of   pius )}
 chimpanzee   has part   finger  ⇐ {( chimpanzee   has part   hand ) ( has part   is   transitive ) ( hand   has part   finger )}
 needs   is opposite of   is needed by  ⇐ {( is needed by   is opposite of   needs )}
 has part   is opposite of   is part of  ⇐ {( is part of   is opposite of   has part )}
 is   is opposite of   is attribute of  ⇐ {( is attribute of   is opposite of   is )}
 is a   is opposite of   is for example  ⇐ {( is for example   is opposite of   is a )}
 generates   is opposite of   is generated by  ⇐ {( is generated by   is opposite of   generates )}
 peppermint   ~   mint  ⇐ {( peppermint   is a   mint )}
 water mint   ~   mint  ⇐ {( water mint   is a   mint )}
 mint   ~   lamiacea  ⇐ {( mint   is a   lamiacea )}
 catnip   ~   lamiacea  ⇐ {( catnip   is a   lamiacea )}
 chimpanzee   is a   ape  ⇐ {( chimpanzee   is an   ape )}
 green mint   is a   mint  ⇐ {( green mint   is an   mint )}
 chimpanzee   is   alive  ⇐ {( ape   is   alive ) ( chimpanzee   is a   ape )}
 water mint   ~   lamiacea  ⇐ {( water mint   ~   mint ) ( ~   is   transitive ) ( mint   ~   lamiacea )}
 peppermint   ~   lamiacea  ⇐ {( peppermint   ~   mint ) ( ~   is   transitive ) ( mint   ~   lamiacea )}
 lamiacea   is for example   mint  ⇐ {( mint   is a   lamiacea ) ( is a   is opposite of   is for example )}
 lamiacea   is for example   catnip  ⇐ {( catnip   is a   lamiacea ) ( is a   is opposite of   is for example )}
 mint   is for example   water mint  ⇐ {( water mint   is a   mint ) ( is a   is opposite of   is for example )}
 ape   is for example   chimpanzee  ⇐ {( chimpanzee   is a   ape ) ( is a   is opposite of   is for example )}
 mint   is for example   green mint  ⇐ {( green mint   is a   mint ) ( is a   is opposite of   is for example )}
 mint   is for example   peppermint  ⇐ {( peppermint   is a   mint ) ( is a   is opposite of   is for example )}
 transitive   is attribute of   has attribute  ⇐ {( has attribute   is   transitive ) ( is   is opposite of   is attribute of )}
 sweet   is attribute of   green mint  ⇐ {( green mint   is   sweet ) ( is   is opposite of   is attribute of )}
 transitive   is attribute of   is ancestor of  ⇐ {( is ancestor of   is   transitive ) ( is   is opposite of   is attribute of )}
 transitive   is attribute of   ~  ⇐ {( ~   is   transitive ) ( is   is opposite of   is attribute of )}
 alive   is attribute of   chimpanzee  ⇐ {( chimpanzee   is   alive ) ( is   is opposite of   is attribute of )}
 transitive   is attribute of   has part  ⇐ {( has part   is   transitive ) ( is   is opposite of   is attribute of )}
 alive   is attribute of   ape  ⇐ {( ape   is   alive ) ( is   is opposite of   is attribute of )}
 finger   is part of   chimpanzee  ⇐ {( chimpanzee   has part   finger ) ( has part   is opposite of   is part of )}
 finger   is part of   hand  ⇐ {( hand   has part   finger ) ( has part   is opposite of   is part of )}
 hand   is part of   chimpanzee  ⇐ {( chimpanzee   has part   hand ) ( has part   is opposite of   is part of )}
 green mint   ~   mint  ⇐ {( green mint   is a   mint )}
 chimpanzee   ~   ape  ⇐ {( chimpanzee   is a   ape )}
 green mint   ~   lamiacea  ⇐ {( green mint   ~   mint ) ( ~   is   transitive ) ( mint   ~   lamiacea )}
zelph>
```

The results demonstrate zelph’s powerful inference capabilities.
It not only answers the specific query about who is an ancestor of pius, but it also derives numerous other facts based on the rules and base facts provided in the script.

## Multi-language Support

zelph allows nodes to have names in multiple languages. This feature is particularly useful when integrating with external knowledge bases. The preferred language can be set in scripts using the `.lang` command:

```
.lang zelph
```

This capability is fully utilized in the Wikidata integration, where node names include both human-readable labels and Wikidata identifiers. An item in zelph can be assigned names in any number of languages, with Wikidata IDs being handled as a specific language ("wikidata").

## Scripting with Janet

zelph embeds [Janet](https://janet-lang.org), a lightweight functional programming language, as its scripting layer. Janet serves as the programmatic backbone behind zelph's syntax: every zelph statement is parsed into a Janet expression before execution. This integration enables users to go beyond zelph's declarative syntax and use loops, conditionals, macros, and data structures to generate facts, rules, and queries programmatically.

Importantly, Janet operates exclusively at *input time* — it generates graph structures that are then processed by zelph's reasoning engine. During inference, only zelph's native engine runs. Think of Janet as a powerful macro system: it constructs the graph, then steps aside.

### Entering Janet Code

There are three ways to write Janet code in zelph:

#### Inline Prefix `%`

Prefix a line with `%` to execute it as Janet:

```
%(print "Hello from Janet!")
```

Whitespace after `%` is optional. If the expression spans multiple lines (i.e., has unbalanced delimiters), zelph automatically accumulates subsequent lines until the expression is complete:

```
%(defn make-facts [items relation target]
   (each item items
     (zelph/fact item relation target)))
```

All four lines are collected and executed as a single Janet expression.

#### Block Mode `%`

A bare `%` on its own line toggles between zelph mode and Janet mode. In Janet mode, all lines are accumulated and executed together when the block is closed:

```
%
(def cities ["Berlin" "Paris" "London"])
(def relation "is capital of")
(def countries ["Germany" "France" "England"])

(each i (range (length cities))
  (zelph/fact (cities i) relation (countries i)))
%
```

This is convenient for longer scripts with multiple definitions and function calls. When closing a Janet block, zelph automatically triggers the reasoning engine (if [auto-run](#full-command-reference) is enabled), so any rules created in the block take effect immediately.

#### Comments and Commands

Lines starting with `#` (comments) and `.` (commands like `.lang`, `.run`, `.save`) work identically in both modes. They are never interpreted as Janet or zelph statements.

### The zelph API for Janet

zelph registers a set of functions in the Janet environment that mirror zelph's syntactic constructs. These functions operate directly on the semantic network, creating nodes, facts, sequences, and sets.

#### Nodes and Names: `zelph/resolve`

Every named entity in zelph's graph is a *node*. The function `zelph/resolve` takes a string and returns the corresponding node in the current language (as set by `.lang`), creating it if it does not yet exist:

```
%(def berlin (zelph/resolve "Berlin"))
%(def germany (zelph/resolve "Germany"))
```

The returned value is a `zelph/node` abstract type — an opaque handle to the internal node. This is the Janet equivalent of simply writing `Berlin` in zelph syntax.

**When to use `zelph/resolve`:** Whenever you need to refer to a node by name from Janet code. The node is resolved in the currently active language, which matters when working with Wikidata IDs vs. human-readable names.

#### Facts: `zelph/fact`

`zelph/fact` creates a subject–predicate–object triple in the graph and returns the relation node. It accepts three or more arguments (multiple objects create multiple facts with the same subject and predicate):

```
%(zelph/fact "Berlin" "is capital of" "Germany")
```

This is equivalent to the zelph statement:

```
Berlin "is capital of" Germany
```

String arguments are automatically resolved as node names (identical to `zelph/resolve`). You can also pass `zelph/node` values directly:

```
%(def city (zelph/resolve "Berlin"))
%(zelph/fact city "~" "city")
```

The function also accepts quoted Janet symbols (`'X`, `'_Var`) for zelph variables — single uppercase letters or underscore-prefixed identifiers. This is used when building rules and queries (see below).

#### Programmatic Query Results: `zelph/query`

When called from Janet, `zelph/query` returns its results as a Janet array of tables rather than printing them. Each table represents one match, mapping variable symbols to their bound `zelph/node` values:

```
%(def results (zelph/query (zelph/fact 'X "is located in" 'Y)))
```

If the graph contains `Berlin "is located in" Germany` and `Paris "is located in" France`, the return value is:

```
@[@{X <zelph/node 11> Y <zelph/node 13>}
  @{X <zelph/node 14> Y <zelph/node 16>}]
```

Access individual bindings with `get` using the same symbol that was passed to `zelph/fact`:

```
%(each r results
   (printf "%v is located in %v" (get r 'X) (get r 'Y)))
```

Iterate over all bindings in a match with `eachp`:

```
%(each r results
   (eachp [var node] r
     (printf "  %v = %v" var node)))
```

**Note:** The table keys are symbols (e.g. `'X`), and the values are `zelph/node` abstract types — opaque handles to the internal graph nodes. This ensures unambiguous identity even when multiple nodes share the same name.

When a query is entered in **zelph syntax** (not via `zelph/query`), results are printed to the console — `zelph/query`'s return-value behavior only applies when called from Janet code.

#### Filtering and Transforming Query Results

Since results are standard Janet arrays and tables, all of Janet's collection functions work naturally:

```
%
(def results (zelph/query (zelph/fact 'X "is located in" 'Y)))

# Extract just the X bindings
(def cities (map (fn [r] (get r 'X)) results))

# Count results
(printf "Found %d matches" (length results))

# Filter: find results where Y is bound to a specific node
(def germany (zelph/resolve "Germany"))
(def in-germany
  (filter (fn [r] (= (get r 'Y) germany))
    results))

(printf "Found %d cities in Germany" (length in-germany))
%
```

**Important:** `zelph/query` is designed for pattern matching with variables. To check whether a specific fact exists, filter the returned bindings directly rather than calling `zelph/query` with a fully concrete pattern. Note that `zelph/fact` always *creates* a fact as a side effect — passing concrete nodes to `zelph/fact` inside a filter would unintentionally add facts to the graph.

Each `zelph/query` call resets the variable scope, so consecutive queries produce independent results with fresh variable bindings.

#### Using Query Results in Rules and Facts

Query results can feed back into graph construction:

```
%
(def german-cities (zelph/query (zelph/fact 'X "is located in" "Germany")))

(each r german-cities
  (zelph/fact (get r 'X) "~" "German city"))
%
```

#### Rules in Janet: The `let` Pattern

In zelph syntax, the [focus operator `*`](#the-focus-operator-) controls what a parenthesized expression returns. For example, `(*{...} ~ conjunction)` creates the conjunction fact but returns the **set node** itself, which is then used as the subject of `=>`. In Janet, this is achieved naturally using `let` bindings:

```
# zelph syntax:
(*{(X "is capital of" Y) (Y "is located in" Z)} ~ conjunction) => (X "is located in" Z)

# Janet equivalent:
%
(let [condition
      (zelph/set
        (zelph/fact 'X "is capital of" 'Y)
        (zelph/fact 'Y "is located in" 'Z))]
  (zelph/fact condition "~" "conjunction")
  (zelph/fact condition "=>" (zelph/fact 'X "is located in" 'Z)))
%
```

The `let` binding stores the set node in `condition`, then uses it in two separate facts — once to mark it as a conjunction, and once to connect it to the consequence via `=>`. This mirrors exactly what the `*` operator does in zelph syntax. The reasoning engine is triggered automatically when the Janet block closes (via [auto-run](#full-command-reference)).

#### Sequences: `zelph/seq` and `zelph/seq-chars`

zelph has two sequence syntaxes, each with a Janet counterpart:

**Node sequences** (`< a b c >` in zelph) create an ordered sequence of existing nodes:

```
%(zelph/seq "Berlin" "Paris" "London")
```

Equivalent to:

```
< Berlin Paris London >
```

**Compact sequences** (`<abc>` in zelph) split a string into individual characters, resolve each to a named node, and build a cons list from them:

```
%(zelph/seq-chars "42")
```

Equivalent to:

```
<42>
```

This is the foundation of zelph's [Semantic Math](#semantic-math) system, where numbers are topological structures within the graph.

#### Sets: `zelph/set`

`zelph/set` creates an unordered set of nodes, returning the set's super-node:

```
%(zelph/set "red" "green" "blue")
```

Equivalent to:

```
{ red green blue }
```

### Referencing Janet Variables in zelph: Unquote `,`

The `,` (comma) operator bridges the two languages in the opposite direction: it allows zelph statements to reference values defined in Janet. Prefix any Janet variable name with `,` inside zelph syntax:

```
%(def my-city (zelph/resolve "Berlin"))
%(def my-relation "is capital of")

,my-city ,my-relation Germany
```

This is equivalent to writing `Berlin "is capital of" Germany`, but the subject and predicate come from Janet variables.

#### How Unquote Works

The unquoted variable name is emitted directly into the generated Janet code. At runtime, zelph's argument resolver handles the value based on its Janet type:

- **`zelph/node`** — used directly as a graph node (language-independent, unambiguous).
- **String** — resolved as a node name in the current language (identical to writing the name in zelph syntax).

This means you can use either form depending on your needs:

```
%(def node-a (zelph/resolve "Berlin"))   # zelph/node: precise, language-independent
%(def node-b "Berlin")                    # string: resolved at use time

,node-a ~ city    # Uses the node directly
,node-b ~ city    # Resolves "Berlin" in current .lang
```

For Wikidata work where IDs are language-independent, both forms are equivalent. For multilingual scenarios, `zelph/resolve` gives you explicit control over *when* the name is resolved.

#### Unquote in Complex Structures

The `,` operator works anywhere a value is expected — in facts, sets, sequences, and nested expressions:

```
%(def pred "P31")
%(def obj (zelph/resolve "Q5"))

# Query: find all instances of Q5 (human)
X ,pred ,obj

# In a set
{ ,node-a ,node-b ,node-c }

# In a nested expression
(,subject ,pred ,obj)
```

### Practical Patterns

#### Generating Facts from Data

A common pattern is defining data in Janet and generating zelph facts programmatically:

```
%
(def taxonomy
  [["Brontosaurus" "Apatosaurinae"]
   ["Apatosaurus" "Apatosaurinae"]
   ["Diplodocus" "Diplodocinae"]
   ["Apatosaurinae" "Diplodocidae"]
   ["Diplodocinae" "Diplodocidae"]])

(each [child parent] taxonomy
  (zelph/fact child "parent taxon" parent))
%

# Now use zelph's inference:
(*{(X "parent taxon" Y) (Y "parent taxon" Z)} ~ conjunction) => (X "parent taxon" Z)
```

After inference, zelph deduces that Brontosaurus and Apatosaurus have Diplodocidae as an ancestor — entirely from data generated by a Janet loop.

#### Parameterized Rules

Janet functions can encapsulate common rule patterns:

```
%
(defn transitive-rule [rel]
  (let [condition
        (zelph/set
          (zelph/fact 'X rel 'Y)
          (zelph/fact 'Y rel 'Z))]
    (zelph/fact condition "~" "conjunction")
    (zelph/fact condition "=>" (zelph/fact 'X rel 'Z))))

(transitive-rule "is part of")
(transitive-rule "is ancestor of")
(transitive-rule "is located in")
%
```

A single function generates a transitive inference rule for any relation. The `let` pattern captures the condition set and reuses it — the Janet equivalent of the focus operator `*` in zelph syntax.

#### Parameterized Queries

Similarly, queries can be wrapped in reusable functions:

```
%
(defn find-all [relation target]
  (zelph/query (zelph/fact 'X relation target)))

(find-all "is located in" "Europe")
(find-all "parent taxon" "Diplodocidae")
%
```

Each `zelph/query` call resets the variable scope, so the two calls produce independent results.

#### Wikidata Query Helpers

Janet functions can provide a higher-level query interface for Wikidata:

```
%
(defn wikidata-query [& clauses]
  "Generate and execute a conjunction query from S-P-O triples."
  (let [facts (map (fn [[s p o]] (zelph/fact s p o)) clauses)
        condition-set (zelph/set ;facts)]
    (zelph/fact condition-set "~" "conjunction")
    (zelph/query condition-set)))

# Find fossil taxa at genus rank
(wikidata-query ["X" "P31" "Q23038290"]
                ["X" "P105" "Q34740"])
%
```

This generates and executes the equivalent of:

```
(*{(X P31 Q23038290) (X P105 Q34740)} ~ conjunction)
```

#### Read-Only Graph Inspection

The following functions inspect the graph without modifying it. Unlike `zelph/fact`, they never create nodes or facts as a side effect. If a referenced name does not exist in the graph, the functions return `false`, `nil`, or an empty array as appropriate.

##### Existence Check: `zelph/exists`

`zelph/exists` checks whether a fact is present in the graph:

```
%(zelph/exists "Berlin" "is located in" "Germany")   # → true
%(zelph/exists "Berlin" "is located in" "France")     # → false
%(zelph/exists "Tokyo" "is located in" "Japan")       # → false (if Tokyo was never added)
```

This is the read-only counterpart to `zelph/fact`. While `zelph/fact` creates facts as a side effect (and is therefore unsuitable for conditional checks), `zelph/exists` purely queries the graph.

Like `zelph/fact`, it accepts strings (resolved in the current language), `zelph/node` values, or a mix:

```
%(def berlin (zelph/resolve "Berlin"))
%(zelph/exists berlin "~" "city")
```

##### Node Names: `zelph/name`

`zelph/name` returns the name of a node as a string, or `nil` if the node has no name:

```
%(def results (zelph/query (zelph/fact 'X "is located in" 'Y)))
%(each r results
   (printf "%s is located in %s"
     (zelph/name (get r 'X))
     (zelph/name (get r 'Y))))
```

An optional second argument specifies the language:

```
%(zelph/name some-node)          # current language (as set by .lang)
%(zelph/name some-node "en")     # English
%(zelph/name some-node "wikidata") # Wikidata ID
```

If no name exists in the requested language, `zelph/name` falls back through English, zelph, and other available languages before returning `nil`.

##### Graph Traversal: `zelph/sources` and `zelph/targets`

These functions traverse the graph along a specific relation, returning arrays of `zelph/node` values:

- **`zelph/sources`** finds all **subjects** connected to a target via a predicate.
- **`zelph/targets`** finds all **objects** connected from a subject via a predicate.

```
# Given: Berlin "is located in" Germany, Potsdam "is located in" Germany
%(zelph/sources "is located in" "Germany")   # → @[<Berlin> <Potsdam>]
%(zelph/targets "Berlin" "is located in")    # → @[<Germany>]
```

**Common patterns with sets and sequences:**

```
# Elements of a set (elements are linked via "in")
%(zelph/sources "in" my-set)        # → all elements of the set

# Decompose a cons cell (Lisp-style list node)
%(zelph/car cons-cell)              # → the first element (car)
%(zelph/cdr cons-cell)              # → the rest of the list (cdr)

# Instances of a concept
%(zelph/sources "~" "city")         # → all nodes that are instances of "city"

# What concept an instance represents
%(zelph/targets inst-node "~")      # → @[<concept-node>]

# Which set a node belongs to
%(zelph/targets elem-node "in")     # → @[<set-node>]
```

##### List Decomposition: `zelph/car` and `zelph/cdr`

These functions decompose cons cells (Lisp-style list nodes), mirroring the classic Lisp `car`/`cdr` operations:

- **`zelph/car`** returns the first element (subject) of a cons cell.
- **`zelph/cdr`** returns the rest of the list (object) of a cons cell.

Both return `nil` for invalid input. `zelph/cdr` returns the `nil` node for the last cell in a list.

``​`
%(def list-42 (zelph/seq-chars "42"))
%(zelph/car list-42)                    # → <zelph/node> for "4"
%(zelph/cdr list-42)                    # → <zelph/node> for the sublist <2>
%(zelph/car (zelph/cdr list-42))        # → <zelph/node> for "2"
%(zelph/cdr (zelph/cdr list-42))        # → <zelph/node> for nil
``​`

**Important:** `zelph/sources` and `zelph/targets` do *not* work for decomposing cons cells, because cons cells are relation nodes (fact nodes) in the graph, not entities that appear as subjects or objects in higher-level facts. Use `zelph/car` and `zelph/cdr` instead.


##### Practical Example: Inspecting a Sequence

Combining `zelph/car` and `zelph/cdr` to walk a cons-list sequence:

```
zelph> <42>
< 4   2 >
%
(def list-42 (zelph/seq-chars "42"))
(def nil-node (zelph/resolve "nil"))

# Walk the cons list using car/cdr
(var current list-42)
(while (and current (not= current nil-node))
  (let [element (zelph/car current)]
    (when element
      (prin (zelph/name element))))
  (set current (zelph/cdr current)))
(print) # newline
%
# Output: 42
```

Note: `zelph/car` and `zelph/cdr` mirror the classic Lisp operations. `zelph/sources` and `zelph/targets` cannot be used for cons cell decomposition because cons cells are relation nodes in the graph, not entities.

##### Combining with Query Results

Read-only functions are especially useful for processing query results:

```
%
(def results (zelph/query (zelph/fact 'X "is located in" 'Y)))

# Filter using zelph/exists (no side effects!)
(def germany (zelph/resolve "Germany"))
(def in-germany
  (filter (fn [r] (= (get r 'Y) germany)) results))

# Alternatively, check a different relation for each result
(def cities-that-are-capitals
  (filter (fn [r] (zelph/exists (get r 'X) "~" "capital"))
    results))

# Display with names
(each r in-germany
  (printf "%s" (zelph/name (get r 'X))))
%
```

#### Building a SPARQL-like Interface

Combining Janet's macro system with zelph's API, you can create domain-specific query languages. Here is a sketch of a `SELECT ... WHERE` syntax:

```
%
(defmacro sparql-select [vars & where-clauses]
  ~(wikidata-query ,;(map (fn [clause] clause) where-clauses)))

# Usage:
# "Select ?x where { ?x P31 Q5 . ?x P27 Q183 }"
# becomes:
(sparql-select [X]
  ["X" "P31" "Q5"]
  ["X" "P27" "Q183"])
%
```

The macro translates a SPARQL-inspired syntax into zelph conjunction queries. Since Janet is a full programming language, this can be extended with `OPTIONAL` (using negation), `FILTER`, and other SPARQL features — each mapped to the appropriate zelph construct.

#### Rule Construction: `zelph/rule` and `zelph/negate`

These functions simplify the creation of inference rules from Janet. While rules can always be built manually using `zelph/set`, `zelph/fact`, and `let` bindings (see [Rules in Janet: The `let` Pattern](#rules-in-janet-the-let-pattern)), `zelph/rule` encapsulates the entire pattern in a single call.

##### `zelph/negate`

Marks a fact pattern as a negation condition. Returns the pattern node itself (equivalent to the focus operator `*` in `(*(pattern) ~ negation)`):

```
%(zelph/negate (zelph/fact 'A ".." 'X))
```

This is equivalent to the zelph syntax fragment `(*(A .. X) ~ negation)` inside a condition set.

##### `zelph/rule`

Creates a complete inference rule: a conjunction of conditions linked to one or more consequences via `=>`.

```
(zelph/rule conditions consequence1 consequence2 ...)
```

- **conditions**: An array or tuple of fact nodes (the conjunction).
- **consequences**: One or more fact nodes to deduce when conditions match.
- **Returns**: The condition set node (the rule's identity in the graph).

**Example — Transitivity rule:**

```
# zelph syntax:
(*{(X R Y) (Y R Z) (R ~ transitive)} ~ conjunction) => (X R Z)

# Janet equivalent using zelph/rule:
%(zelph/rule
   [(zelph/fact 'X 'R 'Y)
    (zelph/fact 'Y 'R 'Z)
    (zelph/fact 'R "~" "transitive")]
   (zelph/fact 'X 'R 'Z))
```

**Example — Negation (finding the last element of a sequence):**

```
# zelph syntax:
(*{(A in _Num) (*(A .. X) ~ negation)} ~ conjunction) => (A "is last digit of" _Num)

# Janet equivalent:
%(zelph/rule
   [(zelph/fact 'A "in" '_Num)
    (zelph/negate (zelph/fact 'A ".." 'X))]
   (zelph/fact 'A "is last digit of" '_Num))
```

**Example — Multiple consequences:**

```
%(zelph/rule
   [(zelph/fact 'A "~" "human")]
   (zelph/fact 'A "has" "consciousness")
   (zelph/fact 'A "has" "mortality"))
```

##### Parameterized Rules with `zelph/rule`

Combined with Janet functions, `zelph/rule` enables concise parameterized rule generation:

```
%
(defn transitive-rule [rel]
  (zelph/rule
    [(zelph/fact 'X rel 'Y)
     (zelph/fact 'Y rel 'Z)]
    (zelph/fact 'X rel 'Z)))

(transitive-rule "is part of")
(transitive-rule "is ancestor of")
(transitive-rule "is located in")
%
```

Compare this to the manual `let` pattern from [Parameterized Rules](#parameterized-rules) — `zelph/rule` eliminates the boilerplate of creating the set, tagging it as a conjunction, and linking the consequence.

##### Setting Up Digit-Wise Addition

As a concrete example of combining `zelph/rule`, `zelph/negate`, and Janet loops to set up a domain for the reasoning engine, here is the setup for single-digit arithmetic. All reasoning happens purely within zelph's inference engine — Janet only generates the initial facts and rules:

```
%
# Digit successor relationships
(for i 0 9
  (zelph/fact (zelph/seq-chars (string i)) ".." (zelph/seq-chars (string (+ i 1)))))

# Mark all single digits
(for i 0 10
  (zelph/fact (zelph/seq-chars (string i)) "~" "digit"))

# Single-digit addition lookup table (100 entries)
(for a 0 10
  (for b 0 10
    (let [sum (+ a b)
          d (% sum 10)
          c (math/floor (/ sum 10))
          addition-fact (zelph/fact (zelph/seq-chars (string a)) "+" (zelph/seq-chars (string b)))]
      (zelph/fact addition-fact "digit-sum" (zelph/seq-chars (string d)))
      (zelph/fact addition-fact "digit-carry" (zelph/seq-chars (string c))))))

# Rule: find the last element of any cons list
# Base case: the car of a cell ending in nil
(let [base-cell (zelph/fact 'A "cons" "nil")]
  (zelph/rule [base-cell]
    (zelph/fact 'A "is last of" base-cell)))

# Recursive case: propagate through the cons chain
(zelph/rule
  [(zelph/fact 'B "is last of" '_Rest)
   (zelph/fact 'A "cons" '_Rest)]
  (zelph/fact 'B "is last of" (zelph/fact 'A "cons" '_Rest)))

# The first element of a cons list is trivially the car of the outermost cell,
# accessible via (zelph/car list-node) — no inference rule needed.
%
```

The rules above are general-purpose (they work on any sequence, not just numbers). The lookup table encodes digit arithmetic as graph facts. From here, additional rules can process multi-digit numbers by walking sequences from right to left, extracting digits, applying the lookup table, handling carries, and constructing new result sequences using fresh variables — all within zelph's reasoning engine.

### Summary: zelph Syntax and Janet Equivalents

| zelph Syntax | Janet Equivalent | Description |
|:---|:---|:---|
| `Berlin` | `(zelph/resolve "Berlin")` | Resolve a name to a node |
| `X`, `_Var` | `'X`, `'_Var` | Variable (single uppercase letter or `_`-prefixed) |
| `sun is yellow` | `(zelph/fact "sun" "is" "yellow")` | Create a fact (triple) |
| `(sun is yellow)` | `(zelph/fact "sun" "is" "yellow")` | Nested fact (returns relation node) |
| `{ red green blue }` | `(zelph/set "red" "green" "blue")` | Unordered set |
| `< Berlin Paris >` | `(zelph/seq "Berlin" "Paris")` | Ordered cons-list sequence |
| `<abc>` | `(zelph/seq-chars "abc")` | Compact character cons-list sequence |
| `*expr` | `let` binding to capture and reuse a sub-expression | Focus operator |
| `,var` in zelph | Direct variable reference in generated code | Unquote a Janet value |
| `% code` | — | Execute Janet inline |
| `%` (bare) | — | Toggle Janet block mode |
| `X ~ human` | `(zelph/query (zelph/fact 'X "~" "human"))` | Query — returns array of `@{symbol node}` tables |
| *(no equivalent)* | `(zelph/exists "sun" "is" "yellow")` | Check if a fact exists (read-only) |
| *(no equivalent)* | `(zelph/name node)` | Get the name of a node as a string |
| *(no equivalent)* | `(zelph/sources "~" "city")` | Find all subjects for a predicate–object pair |
| *(no equivalent)* | `(zelph/targets "Berlin" "is located in")` | Find all objects for a subject–predicate pair |
| `(*(P) ~ negation)` | `(zelph/negate (zelph/fact ...))` | Mark a pattern as negation condition |
| `(*{...} ~ conjunction) => ...` | `(zelph/rule [conditions] consequences...)` | Create inference rule |

## Project Status

The project is currently in **Version 0.9.4 (Beta)**. Core functionality is operational and has been rigorously tested against the full Wikidata dataset.

Current focus areas include:

- **REPL and parser refinement**: The REPL interface and the zelph language parser require architectural improvements.
- **Enhancement of semantic rules**: The [wikidata.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/wikidata.zph) script serves as a base, but the strategy has shifted from generic deductions to targeted contradiction detection. See the [Grant Report](grant-report.md) for details on this approach.
- **Potential Wikidata integration**: Exploring pathways for integration with the Wikidata ecosystem, e.g. the [WikiProject Ontology](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology).

Regarding potential Wikidata integration and the enhancement of semantic scripts, collaboration with domain experts would be particularly valuable. Expert input on conceptual alignment and implementation of best practices would significantly accelerate development and ensure optimal compatibility with existing Wikidata infrastructure and standards.

## Building zelph

You need:

- C++ compiler (supporting at least C++20)
- CMake 3.25.2+
- Git

### Build Instructions

1. Clone the repository with all submodules:

```bash
git clone --recurse-submodules https://github.com/acrion/zelph.git
```

2. Configure the build (Release mode):

```bash
cmake -D CMAKE_BUILD_TYPE=Release -B build src
```

3. Build the project (for MSVC, add `--config Release`):

```bash
cmake --build build
```

### Verifying the Build

Test your installation by running the CLI:

```bash
./build/bin/zelph
```

or

```bash
./build/bin/zelph sample_scripts/english.zph
```

## Licensing

zelph is dual-licensed:

1. **AGPL v3 or later** for open-source use,
2. **Commercial licensing** for closed-source integration or special requirements.

We would like to emphasize that offering a dual license does not restrict users of the normal open-source license (including commercial users).
The dual licensing model is designed to support both open-source collaboration and commercial integration needs.
For commercial licensing inquiries, please contact us at [https://acrion.ch/sales](https://acrion.ch/sales).

# Querying in zelph

zelph provides powerful querying capabilities directly in its scripting language and interactive CLI. Queries allow you to search the semantic network for matching patterns, supporting variables, multiple conditions, and integration with inference rules. This page covers general queries first (applicable to any domain), followed by Wikidata-specific examples.

Queries are statements that contain variables (single uppercase letters) but no `=>` (which would make them rules). They are evaluated immediately without needing `.run`, though inference can expand the graph beforehand to reveal more matches.

## Key Features

- **Variables**: Single uppercase letters (A-Z) or words starting with an underscore `_`, scoped to the query.
- **Multi-Conditions (Conjunctions)**: Use sets marked as conjunctions `({...} ~ conjunction)` to filter results by multiple criteria.
- **Wildcards**: Use variables for subjects, relations, or objects (e.g., `X R Y` matches any triple).
- **Inference Integration**: Automatically performed after each command (also see command `.auto-run`).
- **Output**: Matches are printed with bound values. No matches: Just the query echoed.
- **Limitations**: No multi-line queries.

## General Queries

These examples use a simple geography graph. Load them in zelph (`.lang zelph` mode) for testing:

```
zelph> Berlin "is capital of" Germany
zelph> Germany "is located in" Europe
zelph> Europe "has part" Germany
zelph> (*{(X "is capital of" Y) (Y "is located in" Z)} ~ conjunction) => (X "is located in" Z)
 Berlin   is located in   Europe  ⇐ {( Germany   is located in   Europe ) ( Berlin   is capital of   Germany )}
zelph>
```

### Single-Condition Queries

Basic pattern matching.

- Find capitals: `X "is capital of" Y`  
  Output:
  ```
  X  is capital of  Y
  Answer:  Berlin   is capital of   Germany
  ```

- Find locations in Europe: `A "is located in" Europe`  
  Output (post-inference):
  ```
  A  is located in   Europe
  Answer:  Germany   is located in   Europe
  Answer:  Berlin   is located in   Europe
  ```

### Multi-Condition Queries

Combine for intersections.

- Capitals in Europe: `(*{(X "is located in" Europe) (X "is capital of" Germany)} ~ conjunction)`  
  Output:
  ```
  {(X  is capital of   Germany ) (X  is located in   Europe )}
  Answer: {( Berlin   is capital of   Germany ) ( Berlin   is located in   Europe )}
  ```

## Wikidata-Specific Queries

For Wikidata, switch to `.lang wikidata` after loading a dump (`.wikidata path/to/dump.json` or `.load cached.bin`). Queries use Q/P IDs or names (if set). Examples from paleontology (e.g., Brontosaurus Q3222766).

### Single-Condition Queries

- Instances of fossil taxon: `X P31 Q23038290`  
  Output: Many answers, e.g., `Answer: Q3222766 P31 Q23038290` (Brontosaurus).

- Parent taxa: `X P171 Q3222766`  
  Output: Taxa with Brontosaurus as parent (if any).

### Multi-Condition Queries

Combine for targeted searches.

- Fossil taxa in genus rank: `(*{(X P31 Q23038290) (X P105 Q34740)} ~ conjunction)`  
  Output: Matches like Brontosaurus/Apatosaurus.

- Synonyms with parent taxon: `(*{(X P460 Q14326) (X P171 Q2544161)} ~ conjunction)` (Apatosaurus synonyms in Diplodocidae)  
  Output:
  ```
  {(X  P171   Q2544161 ) (X  P460   Q14326 )}
  Answer: {( Q3222766   P171   Q2544161 ) ( Q3222766   P460   Q14326 )}
  ```
  Since `Q3222766` is [Brontosaurus](https://www.wikidata.org/wiki/Q3222766), this answer means "The [parent taxon](https://www.wikidata.org/wiki/Property:P171) (P171) of [Brontosaurus](https://www.wikidata.org/wiki/Q3222766) is [Apatosaurinae](https://www.wikidata.org/wiki/Q2544161) (Q2544161), which is [said to be the same as](https://www.wikidata.org/wiki/Property:P460) [Apatosaurus](https://www.wikidata.org/wiki/Q14326) (Q14326).

## Tips and Advanced Usage

- **Debugging**: Use `.node`, `.out`, `.in` to inspect before querying.
- **Patterns**: Fixed parts in quotes if spaces; variables anywhere.
- For complex logic, define rules first, then query the inferred graph.

See [Rules and Inference](#rules-and-inference) for synergy with queries.

# zelph and Wikidata: Finding Logical Connections and Contradictions

## Wikidata as an Ideal Use Case for zelph

Wikidata represents an excellent application case for zelph’s capabilities.
It contains over 113 million entries interconnected by relations, all subject to logical constraints.
This complex web of knowledge presents two key opportunities for zelph:

1. **Finding contradictions**: Identifying logical inconsistencies in the data
2. **Making deductions**: Deriving new facts through logical inference

For example, if class `A` is the opposite of class `B` (such as [successor](https://www.wikidata.org/wiki/Q106110771) and [predecessor](https://www.wikidata.org/wiki/Q106110777)), then no entity `X` can belong to both classes (like [replacing entity](https://www.wikidata.org/wiki/Q45025415)).

Similarly, inferences can be made. Example: If X is related to Y and Y is related to Z through the same relation (e.g., X=[Canada](https://www.wikidata.org/wiki/Q16), Y=[American continent](https://www.wikidata.org/wiki/Q828), Z=[Earth's surface](https://www.wikidata.org/wiki/Q1349417), relation=[is part of](https://www.wikidata.org/wiki/Property:P361)), and the relation is [transitive](https://www.wikidata.org/wiki/Q64861), then X must also be related to Z in the same way.

### Architectural Synergy with Wikidata

zelph’s architecture of treating relations as first-class nodes creates a perfect alignment with Wikidata’s data model.
In Wikidata, properties (P-entities) are not merely labels on edges but are themselves entities with their own attributes, constraints, and relationships to other entities.
This fundamental similarity enables zelph to:

1. **Naturally represent Wikidata’s property hierarchy**: Properties in Wikidata can have subproperties, domains, ranges, and other metadata - all of which are directly representable in zelph’s relation-as-node approach.

2. **Reason about properties themselves**: zelph can apply inference rules to properties just as it does to regular entities, enabling powerful meta-reasoning capabilities essential for working with Wikidata’s complex property structure.

3. **Enforce property constraints**: Wikidata’s property constraints (symmetry, transitivity, inverse relationships) map directly to zelph’s rule system, allowing automatic validation and inference.

This structural compatibility makes zelph well-suited for analyzing and enriching Wikidata’s knowledge graph while maintaining its semantic integrity.

## Technical Implementation

### Memory Efficiency

The scale of Wikidata is massive: the JSON dump is approximately 1.7 TB in size, containing over 113 million entries. zelph has been optimized to handle this scale effectively.

The system is capable of importing the **entire** Wikidata graph into memory, a significant achievement that enables non-iterative, complete contradiction detection. After processing, the complete semantic network is serialized to disk in a highly efficient format (~100 GB).

While the serialized footprint is compact given the data volume (99 GB), loading the graph for active reasoning (where all relationships and structures must be accessible) requires significant memory. In practice, a system with **256 GB of RAM** is recommended for full-speed operation. Systems with 128 GB can process the graph by utilizing aggressive swap and compression (ZRAM), though at reduced performance.

### Processing Performance

Running the inference process on Wikidata data is computationally intensive but highly optimized:

- **Parallel Processing:** Both the data import and the unification/reasoning engine are multi-threaded, utilizing all available CPU cores to speed up processing.
- **Performance:** A complete inference pass on the full dataset takes approximately 2.5 hours on high-end hardware (e.g., Intel Core i9 with 24 cores), though this depends heavily on available RAM and the specific rules being applied.
- **Workflow:** Users can run targeted scripts to find specific classes of contradictions (see [Grant Report](grant-report.md) for examples like Split Order Violations).

## Wikidata Integration Script

The following script demonstrates how zelph connects with Wikidata data:

```
.lang zelph

.name !                wikidata Q363948
.name ~                wikidata P31
.name "is subclass of" wikidata P279
.name "is facet of"    wikidata P1269
.name =>               wikidata Q374182
.name ->               wikidata Q130901
.name "is part of"     wikidata P361
.name "has part"       wikidata P527
.name "is opposite of" wikidata P461
.name "is inverse of"  wikidata P1696
.name "has quality"    wikidata P1552
.name "is for example" wikidata Q21514624
.name "transitive relation" wikidata Q18647515

# The following facts are part of wikidata:
#"is subclass of" ~ transitive relation
#"has part"       ~ transitive relation
#"is facet of"    ~ transitive relation
#"is part of"     ~ transitive relation
#"is part of"     is inverse of "has part"

# The following facts are not part of wikidata:
"has quality" ~ transitive relation

(*{(X "is facet of" Y) (Y ~ C)}                ~ conjunction) => (X ~ C)
(*{(X "is facet of" Y) (Y "is subclass of" C)} ~ conjunction) => (X "is subclass of" C)
(*{(X "is facet of" Y) (Y "has part" P)}       ~ conjunction) => (X "has part" P)
(*{(X "is facet of" Y) (Y "is part of" P)}     ~ conjunction) => (X "is part of" P)
(*{(X "is facet of" Y) (Y "has quality" Q)}    ~ conjunction) => (X "has quality" Q)

# The following fact is not part of wikidata. Wikidata only includes the fact "is subclass of" "subject item of this property" "is for example"
"is for example"  is inverse of "~"

(*{(R ~ "transitive relation") (X R Y) (Y R Z)}         ~ conjunction) => (X R Z)
(*{(P ~ "transitive relation") (P "is inverse of" Q)}   ~ conjunction) => (Q ~ "transitive relation")
(*{(X ~ K) (K "is subclass of" U)}                      ~ conjunction) => (X ~ U)

(*{(X "has quality" E) (E ~ K)}                ~ conjunction) => (X "has quality" K)
(*{(X "has quality" E) (E "is subclass of" K)} ~ conjunction) => (X "has quality" K)
(*{(K "has quality" E) (X ~ K)}                ~ conjunction) => (X "has quality" E)
(*{(K "has quality" E) (X "is subclass of" K)} ~ conjunction) => (X "has quality" E)
(*{(X "has part" P)    (P ~ K)}                ~ conjunction) => (X "has part" K)
(*{(K "has part" P)    (X "is subclass of" K)} ~ conjunction) => (X "has part" P)

(*{(X "is opposite of" Y) (X ~ K)}                ~ conjunction) => (Y ~ K)
(*{(X "is opposite of" Y) (X "is subclass of" K)} ~ conjunction) => (Y "is subclass of" K)
(*{(X "is inverse of" Y)  (X ~ K)}                ~ conjunction) => (Y ~ K)
(*{(X "is inverse of" Y)  (X "is subclass of" K)} ~ conjunction) => (Y "is subclass of" K)

# Single rules (no conjunction needed for 1 condition)
(X "is opposite of" Y) => (Y "is opposite of" X)
(X "is inverse of" Y)  => (Y "is inverse of" X)
(*{(R "is opposite of" S) (X R Y)} ~ conjunction) => (Y S X)
(*{(R "is inverse of" S)  (X R Y)} ~ conjunction) => (Y S X)

(*{(X "is opposite of" Y) (A "has quality" X) (A "has quality" Y)} ~ conjunction) => !
(*{(X "is inverse of" Y)  (A "has quality" X) (A "has quality" Y)} ~ conjunction) => !
(*{(X "is opposite of" Y) (A "has part" X)    (A "has part" Y)}    ~ conjunction) => !
(*{(X "is inverse of" Y)  (A "has part" X)    (A "has part" Y)}    ~ conjunction) => !

(*{(X "is opposite of" Y) (A ~ X)              (A ~ Y)}              ~ conjunction) => !
(*{(X "is opposite of" Y) (A "is subclass of" X) (A "is subclass of" Y)} ~ conjunction) => !
(*{(X "is inverse of" Y)  (A ~ X)              (A ~ Y)}              ~ conjunction) => !
(*{(X "is inverse of" Y)  (A "is subclass of" X) (A "is subclass of" Y)} ~ conjunction) => !

(*{(X "has quality" E) (X ~ E)}              ~ conjunction) => !
(*{(X "has quality" E) (X "is subclass of" E)} ~ conjunction) => !
(*{(X "has quality" E) (E ~ X)}              ~ conjunction) => !
(*{(X "has quality" E) (E "is subclass of" X)} ~ conjunction) => !
(*{(X "has quality" E) (E "has part" X)}       ~ conjunction) => !

(*{(X "has part" E) (X ~ E)}              ~ conjunction) => !
(*{(X "has part" E) (X "is subclass of" E)} ~ conjunction) => !
(*{(X "has part" E) (E ~ X)}              ~ conjunction) => !
(*{(X "has part" E) (E "is subclass of" X)} ~ conjunction) => !

# The following contradiction requires that X cannot be at the same time an instance and a subclass:
(*{(X ~ A) (X "is subclass of" B)} ~ conjunction) => !

(*{(A ~ B) (B ~ A)}                               ~ conjunction) => !
(*{(A "is subclass of" B) (B "is subclass of" A)} ~ conjunction) => !
(*{(A "is facet of" B) (B "is facet of" A)}       ~ conjunction) => !
(*{(A ~ B) (B "is subclass of" A)}                ~ conjunction) => !
(*{(A ~ B) (B "is facet of" A)}                   ~ conjunction) => !
(*{(A "is subclass of" B) (B "is facet of" A)}    ~ conjunction) => !
```

This script maps zelph’s relation types to Wikidata properties and items, defines inference rules, and establishes contradiction checks.

## Understanding the Script

### Relation Mapping

The script begins by mapping zelph’s internal names to Wikidata entities:

- `~` is mapped to Wikidata’s [instance of (P31)](https://www.wikidata.org/wiki/Property:P31)
- `is subclass of` is mapped to [subclass of (P279)](https://www.wikidata.org/wiki/Property:P279)
- `is facet of` is mapped to [facet of (P1269)](https://www.wikidata.org/wiki/Property:P1269)

This careful mapping ensures that zelph can interpret Wikidata’s relational structure correctly.

### Handling "is a" Relations

Wikidata makes a granular distinction between different types of category relations:

1. [instance of (P31)](https://www.wikidata.org/wiki/Property:P31)
2. [subclass of (P279)](https://www.wikidata.org/wiki/Property:P279)
3. [facet of (P1269)](https://www.wikidata.org/wiki/Property:P1269)

zelph’s flexible design accommodates these distinctions.
The idea of the script is to follow the [Wikidata usage guidelines](https://www.wikidata.org/wiki/Property:P2559).
It can be easily adapted or extended for further improvements.

Notably, Wikidata only marks "subclass of" as transitive, not the other two relations.
This makes sense for "instance of" (since an instance is not a class), but the script adds transitivity for "facet of" along with additional rules that reflect its documented meaning:
if X is a "facet of" Y, then X inherits all properties of Y.

For this case, the following rules are included in the script:

- If `Y` is an [instance of](https://www.wikidata.org/wiki/Property:P31) `C`, then `X` must also be an [instance of](https://www.wikidata.org/wiki/Property:P31) `C`.
- If `Y` is a [subclass of](https://www.wikidata.org/wiki/Property:P279) `C`, then `X` must also be a [subclass of](https://www.wikidata.org/wiki/Property:P279) `C`.
- If `Y` [has part](https://www.wikidata.org/wiki/Property:P527) `P`, then `X` must also [have part](https://www.wikidata.org/wiki/Property:P527) `P`.
- If `Y` is [part of](https://www.wikidata.org/wiki/Property:P361) `P`, then `X` must also be [part of](https://www.wikidata.org/wiki/Property:P361) `P`.
- If `Y` has a [characteristic](https://www.wikidata.org/wiki/Property:P1552) `Q`, then `X` must also have a [characteristic](https://www.wikidata.org/wiki/Property:P1552) `Q`.

### Example Inference Process

Here’s a step-by-step example of zelph’s inference process when working with Wikidata:

1. According to Wikidata, the property [greater than (P5135)](https://www.wikidata.org/wiki/Property:P5135) is an instance of [transitive Wikidata property (Q18647515)](https://www.wikidata.org/wiki/Q18647515).
2. Wikidata also states that [transitive Wikidata property (Q18647515)](https://www.wikidata.org/wiki/Q18647515) is a [facet of (P1269)](https://www.wikidata.org/wiki/Property:P1269) [transitive relation (Q64861)](https://www.wikidata.org/wiki/Q64861).
3. The script contains the rule: `(*{(X "is facet of" Y) (Y ~ C)} ~ conjunction) => (X ~ C)`
4. Therefore, zelph infers that [greater than (P5135)](https://www.wikidata.org/wiki/Property:P5135) is also an instance of [transitive relation (Q64861)](https://www.wikidata.org/wiki/Q64861).

## Rules in the Semantic Network

Rules in zelph are encoded in the same semantic network as facts, using the special relation `=>` (which corresponds to [logical consequence (Q374182)](https://www.wikidata.org/wiki/Q374182) in Wikidata).

This innovative approach enables tight integration between the fact base and the rules, allowing rules to be reasoned about in the same way as facts.
This makes zelph particularly powerful for applications like Wikidata, where the knowledge base itself contains statements about relations, including properties like [transitivity](https://www.wikidata.org/wiki/Q18647515).

A rule is just a special case of a fact that uses the relation `=>`. In the case of the application of zelph to Wikidata data, this relation corresponds to [logical consequence](https://www.wikidata.org/wiki/Q374182).

## Loading and Processing Wikidata

To download the compressed JSON file, browse to https://dumps.wikimedia.org/wikidatawiki/entities/. You may need to
search through the subdirectories to find a download link for `wikidata-*-all.json.bz2`.

After uncompression, you may start zelph with the provided `wikidata.zph` script:

```bash
zelph sample_scripts/wikidata.zph
```

### Basic Import

To import Wikidata data (or load a previously saved network), use the `.load` command:

```
.wikidata download/wikidata-20250127-all.json
```

This command is general-purpose:

- For a Wikidata JSON dump, it imports the data and automatically creates a `.bin` cache file in the same directory for faster future loads.
- For a `.bin` file (created by `.save`), it loads the serialized network directly.

### Advanced Commands

zelph provides several additional commands for working with Wikidata:

* **Export Constraints:** Extract constraints from the dump and generate zelph scripts for them:
  ```
  .wikidata-constraints download/wikidata-20250127-all.json constraints_output_dir
  ```

Inference is performed using the general `.run`, `.run-once`, `.run-md`, and `.run-file` commands (see the [Performing Inference](#performing-inference) section above).
