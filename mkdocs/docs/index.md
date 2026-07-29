## Introduction

zelph is an innovative semantic network system that allows inference rules to be defined within the network itself.
This project provides a powerful foundation for knowledge representation and automated reasoning, with a special focus on efficiency and logical inference capabilities.
With dedicated import functions and specialized semantic scripts (like [stdlib/examples/wikidata/wikidata.zph](https://github.com/acrion/zelph/blob/main/stdlib/examples/wikidata/wikidata.zph)),
zelph offers powerful analysis capabilities for the complete Wikidata knowledge graph while remaining adaptable for any semantic domain.

For an in-depth exploration of zelph's reasoning capabilities, including deep unification, negation, inequality constraints, and semantic arithmetic, see [Logic and Computation](logic.md).

### Try It Live 🕹️

<p>
  <strong><a href="play/" target="_blank">Open the zelph playground</a></strong> —
  the complete reasoning engine compiled to WebAssembly, running entirely in your
  browser. Nothing is installed and nothing is sent to a server. Guided demos cover
  rule-based big-number arithmetic, a primality test via negation as failure,
  SPARQL over derived facts, and neural networks inside the graph.
</p>

### Video: Logic and Computation

Watch this video walkthrough of zelph's reasoning capabilities — including live demonstrations of rules, meta-reasoning, semantic arithmetic, and Wikidata analysis. For section navigation and the full technical reference, visit [Logic and Computation](logic.md).

<video controls width="100%">
    <source src="https://zelph.org/assets/2026-03-21-zelph.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

A separate [presentation video](presentation.md) covers zelph's application to Wikidata as part of the Ontology Cleaning Task Force.

### Quick Start Guide 🚀

see [this page](quickstart.md)

### Community and Support

Development of zelph is supported by the Wikimedia Community Fund, through two Rapid Fund projects: [Wikidata Contradiction Detection and Constraint Integration](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Wikidata_Contradiction_Detection_and_Constraint_Integration_(ID:_23553409)>) and [Transitive Reasoning, Qualifier Support, and SPARQL-Subset Integration](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Transitive_Reasoning,_Qualifier_Support,_and_SPARQL-Subset_Integration_(ID:_23759260)>).

The project addresses real-world challenges in large-scale ontology management through direct collaboration with the [Wikidata Ontology Cleaning Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Cleaning_Task_Force) and the [Mereology Task Force](https://www.wikidata.org/wiki/Wikidata_talk:WikiProject_Ontology/Mereology_Task_Force).

### Components

The zelph ecosystem includes:

- A core C++ library providing both C++ and C interfaces
- A single command-line binary that offers both interactive usage (CLI) and batch processing capabilities
- API functions beyond what's available in the command-line interface
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

| Core Node                | Symbol        | Internal Name          | Description                                                                                                                            |
| :----------------------- | :------------ | :--------------------- | :------------------------------------------------------------------------------------------------------------------------------------- |
| **RelationTypeCategory** | `->`          | `RelationTypeCategory` | The meta-category of all relations. Every relation predicate in zelph is an instance (`~`) of this node.                               |
| **IsA**                  | `~`           | `IsA`                  | The fundamental categorical relation. Used for classification, e.g. to classify a Set as a Conjunction.                                |
| **Causes**               | `=>`          | `Causes`               | Defines inference rules. Connects a condition set to a consequence.                                                                    |
| **PartOf**               | `in`          | `PartOf`               | Defines membership in Sets.                                                                                                            |
| **Cons**                 | `cons`        | `Cons`                 | The fundamental list-building relation (Lisp-style). The subject is the first element (car), the object is the rest of the list (cdr). |
| **Nil**                  | `nil`         | `Nil`                  | The empty list terminator. Marks the end of a cons-list.                                                                               |
| **Conjunction**          | `conjunction` | `Conjunction`          | A tag used to mark a Set as a logical AND condition for rules.                                                                         |
| **Unequal**              | `!=`          | `Unequal`              | Used to define constraints (e.g., `X != Y`) within rules.                                                                              |
| **Negation**             | `negation`    | `Negation`             | Used to classify a condition in a rule as negative (match if the fact does _not_ exist).                                               |
| **Contradiction**        | `!`           | `Contradiction`        | The result of a rule that detects a logical inconsistency.                                                                             |

These nodes are the "axioms" of zelph's graph. For example, `~` is defined as an instance of `->` (i.e., "IsA" is a "Relation Type"). This self-referential bootstrapping allows zelph to reason about its own structure.

### Homoiconicity: The Executable Graph

A defining characteristic of zelph is its [homoiconicity](https://en.wikipedia.org/wiki/Homoiconicity): logic (code) and facts (data) share the exact same representation.
Rules are not separate scripts; they are topological structures within the graph.
Math is not hard-coded; numbers are cons-lists of digit nodes that interact with semantic entities through the same rule mechanism.

This means the graph doesn't just _describe_ knowledge; it _structures the execution_ of logic.
For a detailed exploration of this concept and its implications for computation, see [Logic and Computation](logic.md).

### Facts and Relations

A _fact_ in zelph is a statement node created from a **subject**, a **predicate**, and an _object_:

```
subject predicate object
```

A predefined predicate is `~`, used for classification:

```
X ~ Y
```

This can be read as "X is an instance of class Y", but depends on the context of your dataset. It is used in zelph's [internal topology](#internal-representation-of-facts) — there is no need to actually use it in your scripts.

#### Working with Custom Relations

zelph can use any predicate node, not just `~`:

```
zelph> bright "is opposite of" dark
bright "is opposite of" dark
```

In this example, using the interactive REPL, we enter a subject-predicate-object triple.
Neither "bright", "dark" nor "is opposite of" is know to zelph prior this command.
It automatically creates the appropiate nodes and edges in the semantic network.
After doing so, in the second line this topology is parsed and printed to verify the process ran as expected.

Note that when a relation contains spaces, it must be enclosed in quotation marks.

Predicates are completely generic: symbolic predicates such as `..`, `-->`, or `<=` are treated in the same way as word-like predicates such as `followed-by` or `is capital of`.

### Nested Expressions and Sets

zelph supports advanced grouping and recursion using parentheses `()`, braces `{}`, and angle brackets `<>`, plus a shorthand prefix `:` for [self-facts](#the-self-fact-prefix).

#### Parentheses: Nested Facts

A parenthesised statement `(s p o)` creates the fact and evaluates to the **statement node** (i.e. the relation/fact node). This lets you make statements _about_ statements:

```
(bright "is opposite of" dark) "is a" "symmetric relation"
```

Here, the subject of the outer statement is the node representing the inner fact.

> Note: A line consisting of only a bare nested fact like `(subject rel object)` is not a valid _top-level_ statement in the REPL; nested facts are meant to be used _as parts_ of a larger statement.

#### Braces: Sets

Braces `{...}` are used to create **unordered sets** of nodes or facts. This is primarily used for defining conditions in rules (see below).

```
{ (A "is part of" B) (B "is part of" C) }
```

Example session:

```
zelph> { elem1 elem2 elem3 }
{elem1 elem2 elem3}
zelph> A in { elem1 elem2 elem3 }
A in {elem1 elem2 elem3}
Answer: elem1 in {elem1 elem2 elem3}
Answer: elem3 in {elem1 elem2 elem3}
Answer: elem2 in {elem1 elem2 elem3}
zelph> (*{(A "is part of" B) (B "is part of" C) } ~ conjunction) => (A "is part of" C)
{(A "is part of" B) (B "is part of" C)} => (A "is part of" C)
zelph> earth "is part of" "solar system"
earth "is part of" "solar system"
zelph> "solar system" "is part of" universe
"solar system" "is part of" universe
(earth "is part of" universe) ⇐ {(earth "is part of" "solar system") ("solar system" "is part of" universe)}
zelph>
```

##### Set Topology

A set `{A B C}` creates a **Super-Node** (representing the set itself).
The elements are linked to this super-node via the `in` (PartOf) relation.

- **Syntax:** `{A B}`
- **Facts created:**
  - `A in SetNode`
  - `B in SetNode`

The empty set `{}` is the node `nil` — there is nothing for a super-node to
collect, and `nil` is what an empty collection is throughout zelph.

#### Angle Brackets: Lists

Angle brackets `<...>` create **ordered lists** implemented as classic Lisp-style cons lists using the core predicates `cons` and `nil`.

A list is represented by the **outermost cons cell**. There is **no separate container node**: the node returned by list construction _is_ the list (exactly as in Lisp).

The empty list `<>` is `nil`, exactly as in Lisp: the terminator every
non-empty list ends at. `q p <>`, `q p {}` and `q p nil` are therefore the
same fact.

---

##### Two list syntaxes

zelph supports two input modes that both create cons lists:

1. **Node Lists (space-separated):** `<item1 item2 item3>`
   - **Syntax:** At least one whitespace between elements.
   - **Semantics:** The elements are existing nodes (`item1`, `item2`, …) and the list preserves this order.
   - **Construction:** Built right-to-left:
     - `Cell3 = item3 cons nil`
     - `Cell2 = item2 cons Cell3`
     - `Cell1 = item1 cons Cell2` ← this outermost cons cell **is the list**

2. **Compact Lists (continuous characters):** `<abc>`
   - **Syntax:** No spaces inside the brackets.
   - **Semantics:** The input is split into individual characters. Each character is resolved to a named node (e.g. `"a"`, `"b"`, `"c"`), and these become the list elements.
   - **Construction detail:** Before building the cons list, the character sequence is **reversed**.  
     So `"abc"` becomes the element vector `["c","b","a"]`, yielding: - `Cell3 = "a" cons nil` - `Cell2 = "b" cons Cell3` - `Cell1 = "c" cons Cell2`

This reversal is **not** "numeric logic" — it is simply the definition of the compact syntax and is useful for many right-to-left processing rules (including, but not limited to, arithmetic scripts).

Because of this rule, `<abc>` is internally identical to the explicit node list `<c b a>`.

---

##### Referring to the _same_ cons list in different ways

The list node is the **outermost cons cell**. You can refer to the same list topology using any of these equivalent notations:

1. **Explicit cons chain (nested facts)**

```
(3 cons (1 cons nil))
```

2. **Compact list syntax** (character splitting + reversal rule)

```
<13>
```

3. **Node-list syntax** (space-separated elements)

```
<3 1>
```

Example session:

```
zelph> (3 cons (1 cons nil)) is prime
<3 1> is prime
zelph> <13> is prime
<3 1> is prime
zelph> <3 1> is prime
<3 1> is prime
```

Why this works: `<13>` is parsed as a compact list of characters `"1"` and
`"3"`, **reversed before cons construction**, so it becomes the same internal
cons chain as `<3 1>` — and the same node as the explicit cons chain.

> To avoid confusion: `<13>` is list syntax, **not** a numeric node ID. Numeric IDs are shown elsewhere in parentheses (e.g. `(10)` in Mermaid graphs).

---

##### Display: storage order, and the numeral exception

A cons list echoes in **storage order, space-separated** — the notation that
re-reads as the same list. `<abc>` therefore prints as `<c b a>`, which is
exactly the chain the compact syntax built; the display does not undo the
input-side reversal, because nothing about three character nodes says they
are a numeral.

The one exception is the numeral case, and it is opt-in: a script that
registers its digit alphabet with `zelph/set-number-digits` tells the
formatter which nodes are digits. A `nil`-terminated list consisting solely
of registered digits is then printed MSB-first as a decimal `&`-literal —
the inverse of the `&`-input syntax, not a spacing variant of `<...>`:

```
zelph> .import decimal-arithmetic
zelph> <3 1> is prime
&13 is prime
zelph> <a b c> is x
<a b c> is x
```

See [Number Literals](logic.md#number-literals) for the mechanism. Either
way the topology is untouched: display never imposes numeric meaning, it
only reports the meaning a script has declared.

---

##### Digits vs. numbers (emergent, not built-in)

The cons-list representation naturally distinguishes between:

- a **character node** such as `"4"` (a normal named node), and
- a **single-element list** `( "4" cons nil )` (a different node: a cons cell)

Whether you interpret `"4"` as a digit, or `( "4" cons nil )` as the number four, is entirely up to your rule system (e.g. [stdlib/decimal-arithmetic.zph](https://github.com/acrion/zelph/blob/main/stdlib/decimal-arithmetic.zph)) and any external naming/mapping you choose to apply. For a detailed exploration of how rules can define arithmetic over these structures, see [Semantic Math](logic.md#semantic-math-computation-as-graph-rewriting). For convenient input, the parser additionally supports `&`-prefixed number literals (e.g. `&42`), which delegate to the redefinable function `zelph/number` — so you can always type decimal even when the loaded arithmetic script uses a different internal base. See [Number Literals](logic.md#number-literals). The inverse direction — displaying digit lists as decimal &-literals — works through the same opt-in mechanism: scripts register their digit alphabet via zelph/set-number-digits.

#### The Focus Operator `*`

When defining complex structures, you often need to refer to a specific part of an expression rather than the resulting fact node. The `*` operator allows you to "focus" or "dereference" a specific element to be returned.

- `(A B C)` creates the fact and returns the relation node.
- `(*A B C)` creates the fact and returns node `A`.
- `(*{...} ~ conjunction)` creates the fact that the set is a conjunction, but **returns the set node itself**.

This operator is crucial for the rule syntax.

**Example — typing a node via a nested fact:**

The focus operator lets you create a fact and use its subject in an outer statement in a single expression:

```
zelph> (*tim ~ human) ~ male
tim ~ male
zelph> tim _predicate _object
Answer: tim ~ male
Answer: tim ~ human
```

The inner expression `(*tim ~ human)` creates the fact `tim ~ human` and — thanks to the `*` prefix — returns the node `tim` rather than the statement node. That returned node becomes the subject of the outer `~ male` relation, so `tim ~ male` is created as well.

Querying `tim _predicate _object` (where leading underscores indicate variables, equivalent to using single uppercase letters) confirms that both facts are in the graph.

#### The Self-Fact Prefix `:`

Several standard-library modules use **self-facts** — facts whose subject and
object are the same node — as request markers: `(N testprime N)` triggers the
primality test, `(T simplify T)` a simplification. Because writing the term
twice is noisy (and error-prone for large terms), zelph provides a prefix
shorthand:

```
:pred X
```

is pure syntax sugar for

```
X pred X
```

The desugaring happens at parser level, so the shorthand works at any nesting
level: as a rule condition or consequence, inside conjunctions, and in the
repeatable query idiom. The operand is evaluated exactly once and used as both
subject and object, so both sides are guaranteed to be the _same_ node — even
when the operand has side effects (a focus `*`, nested fact creation).

> The term _self-fact_ is zelph's own: graph-theoretically a loop, relationally
> a point on the diagonal of the predicate — the classic way to encode unary
> predicates over a binary-relation substrate. For the logic-side perspective,
> and for why zelph does not assign operators a fixed arity instead, see
> [Unary Predicates and Self-Facts](logic.md#unary-predicates-and-self-facts).

```
zelph> .import decimal-arithmetic
zelph> .import primes
zelph> :testprime &13
zelph> (:testprime &13) = X
Answer: (:testprime &13) = prime
zelph> (:isprime N, N hasdivisor D) => !
```

A lone `:pred` on a line is an incomplete statement; the operand may follow on
the next line, so multi-line input works as usual. A variable token as
predicate keeps variable semantics: the rule pattern `(:R X)` matches any
self-fact, whatever its predicate.

**Display.** The formatter uses the same shorthand in the other direction:
any fact whose subject and object coincide is printed as `:pred subject`.
Because all terms are hash-consed, this includes facts that merely _happen_ to
be self-facts, such as `(&1 + &1)` — printed `:+ &1`. A manual expansion of
such output back to the verbose `S P S` form parses to the identical node.
The sugar form is only used where it round-trips through the parser: the
predicate needs a single-token name free of reserved characters (`*`, `<`,
`>`, `,`, quotes, `¬`) that is not shaped like a variable. Facts such as
`(&9 * &9)` or `x "is opposite of" x` therefore keep the verbose form.

Modules can additionally exclude predicates from the display sugar via
[`zelph/no-selffact-sugar`](janet.md#redefinable-hooks): the arithmetic and EML modules register their
term-forming operators, so a hash-consed coincidence like `(&1 + &1)` or
`(&1 eml &1)` prints verbose, while request markers (`:testprime`,
`:simplify`) keep the compact form. The registration only affects display;
`:+ &1` remains valid input.

The colon is **not** a reserved character: atoms with an _inner_ colon (URLs,
`wd:Q5`-style names) are unaffected; only a leading colon in value position
triggers the sugar.

## Creating a node graph

You can generate a node graph yourself using zelph's `.mermaid` command, which outputs a Mermaid HTML format file. For example:

```
.mermaid name 3
```

In this example, `name` refers to the node identifier (in the currently active language specified via the `.lang` command) whose connections you want to visualise. The following number represents the depth of connections to include in the graph (default is 3).

To view the Mermaid graph, open the generated HTML file in a web browser.

## Rules and Inference

One of zelph's most powerful features is the ability to define inference rules within the same network as facts. Rules are statements containing `=>` with conditions before it and a consequence after it.

For an in-depth treatment of zelph's rule system — including deep unification, negation as failure, inequality constraints, fresh variables, and the formal connection to predicate logic — see [Logic and Computation](logic.md).

### Rule Syntax

A rule in zelph is formally a statement where the subject is a **set of conditions** (marked as a conjunction) and the object is the **consequence**.

Example rule:

```
(*{(R ~ transitive) (X R Y) (Y R Z)} ~ conjunction) => (X R Z)
```

**Breakdown of the syntax:**

1. `{...}`: Creates a **Set** containing three fact templates:
   - `R` is a transitive relation.
   - `X` is related to `Y` via `R`.
   - `Y` is related to `Z` via `R`.
2. `~ conjunction`: Defines that this Set represents a logical "AND" (Conjunction). The inference engine only evaluates sets marked as conjunctions.
3. `(*...)`: The surrounding parentheses create the fact `Set ~ conjunction`.
4. `*`: The **Focus Operator** at the beginning ensures that the expression returns the **Set Node** itself, not the fact node `Set ~ conjunction`.
5. `=>`: The inference operator. It links the condition Set (Subject) to the consequence (Object).
6. `(X R Z)`: The consequence fact.

This rule states: _If there exists a set of facts matching the pattern in the conjunction, then the fact `X R Z` is deduced._

#### Syntax Sugar for Conditions

A parenthesised group that contains commas is parsed as **conjunction syntax sugar**:

```
(cond1, cond2, cond3)
```

Each comma-separated condition is itself a normal zelph statement fragment (either a fact pattern like `X R Y`, or a nested expression). The whole parenthesised expression evaluates to a **set node** that is automatically tagged as a conjunction internally (i.e. it desugars to the same topology as `(*{...} ~ conjunction)`).

Practical consequence: you can write the above example rule as

```
(R ~ transitive, X R Y, Y R Z) => (X R Z)
```

without using the set syntax `{...}` or the `conjunction` core node.

### Examples

Here is a practical example of how a transitive-closure rule works in zelph (which you can also try out in interactive mode):

```
zelph> (R is transitive, A R B, B R C) => (A R C)
{(A R B) (R is transitive) (B R C)} => (A R C)
```

After the entered rule, we see zelph's output, which in this case simply confirms the input of the rule.

Now, let's declare that the relation `>` (greater than) is an instance of transitive relations:

```
zelph> > is transitive
> is transitive
```

Next, we provide three elements ("4", "5" and "6") for which the `>` relation applies:

```
zelph> 6 > 5
6 > 5
zelph> 5 > 4
5 > 4
(6 > 4) ⇐ {(6 > 5) (> is transitive) (5 > 4)}
zelph>
```

After entering `5 > 4`, zelph's unification mechanism takes effect and automatically adds a new fact: `6 > 4`. This demonstrates the power of the transitive relation rule in action. Note that the rule uses `R` as a variable for the predicate itself — this is possible because predicates are first-class nodes in the graph, not edge labels. Any relation that is declared `is transitive` will automatically benefit from this single rule.

Rules can also define contradictions using `!`:

```
zelph> (X "is opposite of" Y, A ~ X, A ~ Y, X != Y) => !
{(X "is opposite of" Y) (A ~ X) (X != Y) (A ~ Y)} => !
zelph> bright "is opposite of" dark
bright "is opposite of" dark
zelph> yellow ~ bright
yellow ~ bright
zelph> yellow ~ dark
yellow ~ dark
! ⇐ {(bright "is opposite of" dark) (yellow ~ bright) (bright != dark) (yellow ~ dark)}
Found one or more contradictions!
zelph>
```

This rule states that if X is opposite of Y and X ≠ Y, then an entity A cannot be both an instance of X and an instance of Y, as this would be a contradiction. The `X != Y` guard is essential here: without it, a reflexive fact like `bright "is opposite of" bright` could cause a spurious contradiction when `yellow ~ bright` is entered, because `X` and `Y` would both bind to `bright` (see [Inequality Constraints](logic.md#inequality-constraints) for a detailed discussion).

A contradiction is **reported, not enforced**. The facts that triggered it stay in the graph — zelph is built to audit inconsistent real-world data, and deleting the evidence would defeat that. Nothing is written for the contradiction either: `!` is the one consequence that materialises no fact, which is also why a contradiction rule is always safe in the [deferred stratum](logic.md#stratified-evaluation) (it can derive nothing that another rule could then negate).

What you get instead is a report — on the console, and in the [derivation export](#exporting-derivations) as a record with `"kind":"contradiction"` and the premises that produced it. Each distinct rule instantiation is reported **once**, however many derivation paths reach it, so the count is a count of violations rather than of discoveries.

### Internal Representation of facts

In a conventional semantic network, relations between nodes are labeled, e.g.

```mermaid
graph LR
    bright -->|is opposite of| dark
```

zelph's representation of relation types works fundamentally differently.
As mentioned in the introduction, one of zelph's distinguishing features is that it treats relation types as first-class nodes rather than as mere edge labels.

Internally, zelph creates special nodes to represent relations.
For example,when identifying "is opposite of" as a relation (predicate), this internal structure is created:

```mermaid
graph TD
    n_3["~"]
    n_1["->"]
    n_5688216769861436680["is opposite of ~ ->"]
    n_10["is opposite of"]
    style n_10 fill:#8a5c00,stroke:#666666,stroke-width:2px,color:#e0e0e0
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

This architecture is particularly valuable when working with knowledge bases like Wikidata, where relations (called "properties" in Wikidata terminology) are themselves first-class entities with their own attributes, constraints, and relationships to other entities. zelph's approach naturally aligns with Wikidata's conceptual model, allowing for seamless representation and inference across the entire knowledge graph.

Similarly, when stating:

```
bright "is opposite of" dark
```

zelph creates a special relation node that connects the subject "bright" bidirectionally, the object "dark" in reverse direction, and the relation type "is opposite of" in the forward direction.

```mermaid
graph TD
    n_11["dark"]
    n_9["bright"]
    n_8445031417147704759["bright is opposite of dark"]
    n_10["is opposite of"]
    style n_10 fill:#8a5c00,stroke:#666666,stroke-width:2px,color:#e0e0e0
    n_8445031417147704759 --> n_10
    n_9 <--> n_8445031417147704759
    n_11 --> n_8445031417147704759
```

The directions of the relations are as follows:

| Element       | Example        | Relation Direction |
| ------------- | -------------- | ------------------ |
| Subject       | white          | bidirectional      |
| Object        | black          | backward           |
| Relation Type | is opposite of | forward            |

This semantics is used by zelph in several contexts, such as rule unification. It's required because zelph doesn't encode relation types as labels on arrows but rather as equal nodes. This has the advantage of facilitating statements about statements, for example, the statement that a relation is transitive.

zelph also supports **self-referential facts**, where subject and object are the same
node (e.g., `A cons A`). These arise rarely in practice — Wikidata contains a small
number of such entries, for example `South Africa (Q258) country (P17) South Africa
(Q258)`. On input and output, such facts are covered by the
[self-fact prefix `:`](#the-self-fact-prefix): the Wikidata example prints as
`:P17 Q258`. Internally, the object connection is omitted because the subject is already
connected to the fact-node bidirectionally, which serves as the implicit object
connection. Detection is unambiguous: a fact-node whose left-neighbor set contains
only the subject node (no additional unidirectional incoming connection) is
self-referential.

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

The answer lies in the network's **strict topological semantics** (see [Internal Representation of facts](#internal-representation-of-facts) and [Internal representation of rules](#internal-representation-of-rules)). In zelph, a _statement node_ is not "just a node with a long label"; it has a **unique structural signature**:

- **Bidirectional** connection to its **subject**
- **Forward** connection to its **relation type** (a first-class node)
- **Backward** connection to its **object**

The unification engine is **hard-wired to search only for this pattern** when matching a rule's conditions. In other words, a variable that ranges over "statements" can only unify with nodes that expose exactly this subject/rel/type/object wiring. Conversely, variables intended to stand for ordinary entities cannot accidentally match a statement node, because ordinary entities **lack** that tri-partite signature.

Two immediate consequences follow:

1. **Unambiguous matching.** The matcher cannot mistake an entity for a statement or vice versa; they occupy disjoint topological roles.
2. **Network stability.** Because statementhood is encoded structurally, rules cannot "drift" into unintended parts of the graph. This design prevents spurious matches and the sort of runaway growth that would result if arbitrary nodes could pose as statements.

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

To record everything a run derived, for further processing:

```
.run-export <file>
```

See [Exporting Derivations](#exporting-derivations). For normal interactive
or script use, `.run` is the standard command.

### Deduction Output Modes

zelph performs forward chaining: every derivable consequence is materialized
in the graph (see [Logic and Computation](logic.md#positioning-forward-chaining-over-graphs)).
For rule libraries that implement computations — the arithmetic modules are
the prime example — this is a double-edged sword: a single input like
`&10 - &3` triggers a long cascade of internal derivations (recursion
states, canonicalization steps) that are essential to the computation but
rarely interesting to read. In a goal-driven system like Prolog this
question does not arise, because only the proof of the asked goal is ever
constructed; in a forward chainer, filtering the _trace_ is the natural
counterpart.

The `.deductions` command controls which derived facts are printed:

    .deductions all      # print every deduction (full derivation trace)
    .deductions focus    # print only deductions about your input (default)
    .deductions off      # print no deductions

In `focus` mode, a deduction is printed when its subject stems from an
interactively entered statement: the subject is the entered fact itself, or
its subject, or one of its objects. Anchors accumulate over the session, so
a rule entered later still surfaces conclusions about earlier inputs.
Imported scripts (`.import`) do not contribute anchors — a loaded arithmetic
library stays silent about its internals.

The filter affects printing only: **all facts are derived and stored
regardless of the mode**, and query answers, contradictions and warnings are
always printed. If a result you are interested in is not shown, query it
(e.g. `&7 > X`) or switch to `.deductions all`. Filtered deductions are
counted in the "(skipped N deductions)" summary. As a side effect, heavy
computations run several times faster in focus/off mode, because rendering
large derived terms dominates the cost.

## Node Clusters: Transactional Workspaces

When experimenting on a large loaded network — say, a full Wikidata dump — you often want to undo an entire experiment without reloading everything. Clusters provide exactly that:

```
.cluster my-experiment
... enter facts and rules, .run ...
.cluster-drop my-experiment      # roll back everything the experiment created
```

While a cluster is active, every node created is recorded in it: entities, relation nodes, rule definitions, the variables those rules are made of, and facts deduced by `.run`. Facts that already existed beforehand are never recorded, so dropping a cluster can never destroy pre-existing knowledge. `.cluster-merge <from> <to>` commits a cluster's bookkeeping into another one (merging into `default` simply turns its nodes into ordinary nodes), and `.cluster default` deactivates tracking. Clusters are session state and are not persisted by `.save`.

The [neural network demo](neural.md) uses a cluster so that the entire experiment — layers, synapses, rules, and all deductions — can be removed with a single command, leaving the loaded dump untouched.

### Exporting Derivations

`.run-export <file>` performs full inference like `.run` and writes every
derived fact and every contradiction to `<file>` — one JSON object per line
(JSON Lines):

```json
{"kind":"deduction","conclusion":[SEG,...],"premises":[[SEG,...],...]}
```

A `SEG` is either a JSON string — literal text of the rendering, brackets
and spacing — or one of

```json
{"names":{"wikidata":"Q5","en":"human"}}
{"core":"!"}
```

the first naming one node in every language it is known by, the second one
of zelph's own vocabulary (`!`, `~`, `=>`, …).

Two properties are worth spelling out, because they are the point of the
format:

- **Nothing in it is about a target format.** Which of a node's names to
  display, which of them is a URL, whether identifiers should be
  italicised, which file a line belongs in — those are decisions of the
  consumer. zelph does not know about Wikidata, and it does not know about
  MkDocs either.
- **The premises are separate.** The console prints the condition _set_,
  `⇐ {(a p b) (b p c)}`, because that set is what the rule's subject is.
  The export hands over its elements, so no one has to take braces apart
  again.

Deduction printing is off during the run: rendering large derived terms
dominates the wall-clock time, and the file is the point.

```
zelph> .lang wikidata
wikidata> .auto-run
Auto-run is now disabled.
wikidata-> Q1 P279 Q2
Q1 P279 Q2
wikidata-> Q2 P279 Q3
Q2 P279 Q3
wikidata-> (*{(A P279 B) (B P279 C)} ~ conjunction) => (A P279 C)
{(B P279 C) (A P279 B)} => (A P279 C)
wikidata-> .run-export /tmp/derivations.jsonl
Running full inference; derivations are written to /tmp/derivations.jsonl as JSON Lines.
```

Content of `/tmp/derivations.jsonl` (one line, wrapped here for reading):

```json
{"kind":"deduction",
 "conclusion":["(",{"names":{"wikidata":"Q1"}}," ",{"names":{"wikidata":"P279"}}," ",{"names":{"wikidata":"Q3"}},")"],
 "premises":[["(",{"names":{"wikidata":"Q2"}}," ",{"names":{"wikidata":"P279"}}," ",{"names":{"wikidata":"Q3"}},")"],
             ["(",{"names":{"wikidata":"Q1"}}," ",{"names":{"wikidata":"P279"}}," ",{"names":{"wikidata":"Q2"}},")"]]}
```

#### Converting the export

`dev_scripts/zelph-derivations.py` is the reference converter and reproduces
what zelph used to write itself:

```bash
# The MkDocs tree behind the reports on https://zelph.org: one page per
# Wikidata identifier occurring in a conclusion, with links between pages.
dev_scripts/zelph-derivations.py /tmp/derivations.jsonl --format md --out mkdocs/docs/report

# One flat line per derivation, premises first.
dev_scripts/zelph-derivations.py /tmp/derivations.jsonl --format text --out /tmp/derivations.txt
```

```
Q2 P279 Q3, Q1 P279 Q2 => Q1 P279 Q3
```

The `text` form is also the starting point for tokenizer-friendly training
data. Long numeric identifiers (`Q123456789`) are expensive for standard
tokenizers, which split them into many sub-tokens; because every identifier
arrives in the export as a discrete token rather than as a substring of a
sentence, substituting a compact encoding for it is a dictionary lookup and
not a parse. zelph itself no longer ships such an encoding — that was an
experiment, and it is exactly the kind of decision that belongs to the
consumer of the data.

## Example Script

Here's an example demonstrating zelph's capabilities:

```
(X "is a" Y) => (X ~ Y)
(X "is an" Y) => (X "is a" Y)

"is attribute of" "is opposite of" is
"is part of"      "is opposite of" "has part"
"is for example"  "is opposite of" "is a"

"has part"      is transitive
"has attribute" is transitive
~               is transitive

(R is transitive, X R Y, Y R Z) => (X R Z)
(X is E, E "is a" K) => (X is K)
(X "has part" P, P "is a" K) => (X "has part" K)
(K is E, X "is a" K) => (X is E)
(K "has part" P, X "is a" K) => (X "has part" P)
(X "is opposite of" Y, X "is a" K) => (Y "is a" K)
(X "is opposite of" Y) => (Y "is opposite of" X)
(R "is opposite of" S, X R Y) => (Y S X)

(X "is opposite of" Y, A is X, A is Y) => !
(X "is opposite of" Y, A "has part" X, A "has part" Y) => !
(X "is opposite of" Y, A "is a" X, A "is a" Y) => !
(X is E, X "is a" E) => !
(X is E, E "is a" X) => !
(X is E, E "has part" X) => !

"is needed by" "is opposite of" needs
"is generated by" "is opposite of" generates

"is needed by" "is opposite of" needs
"is generated by" "is opposite of" generates

(X generates energy) => (X "is an" "energy source")
(A is hot) => (A generates heat)
(A generates "oxygen") => (A is alive)

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

An imported script does not contribute deduction anchors, so in the default
`focus` mode only the query answer is printed and the derived facts are
counted (see [Deduction Output Modes](#deduction-output-modes)):

```
zelph> .import examples/english
Importing file examples/english.zph...
Answer: paul "is ancestor of" pius
 (skipped 35 deductions)
```

`.deductions all` shows what those 35 are. The order in which they appear
is not fixed -- the reasoner is parallel, and the fixpoint is a set:

```
zelph> .deductions all
Deduction printing mode: all
zelph> .import examples/english
Importing file examples/english.zph...
Answer: paul "is ancestor of" pius
(peppermint ~ mint) ⇐ (peppermint "is a" mint)
(catnip ~ lamiacea) ⇐ (catnip "is a" lamiacea)
(mint ~ lamiacea) ⇐ (mint "is a" lamiacea)
("water mint" ~ mint) ⇐ ("water mint" "is a" mint)
(peppermint ~ lamiacea) ⇐ {(peppermint ~ mint) (~ is transitive) (mint ~ lamiacea)}
("water mint" ~ lamiacea) ⇐ {("water mint" ~ mint) (~ is transitive) (mint ~ lamiacea)}
(peter "is ancestor of" pius) ⇐ {(peter "is ancestor of" paul) ("is ancestor of" is transitive) (paul "is ancestor of" pius)}
(chimpanzee "has part" finger) ⇐ {(chimpanzee "has part" hand) ("has part" is transitive) (hand "has part" finger)}
(chimpanzee "is a" ape) ⇐ (chimpanzee "is an" ape)
("green mint" "is a" mint) ⇐ ("green mint" "is an" mint)
(needs "is opposite of" "is needed by") ⇐ ("is needed by" "is opposite of" needs)
(is "is opposite of" "is attribute of") ⇐ ("is attribute of" "is opposite of" is)
("has part" "is opposite of" "is part of") ⇐ ("is part of" "is opposite of" "has part")
(generates "is opposite of" "is generated by") ⇐ ("is generated by" "is opposite of" generates)
("is a" "is opposite of" "is for example") ⇐ ("is for example" "is opposite of" "is a")
(finger "is part of" chimpanzee) ⇐ {(chimpanzee "has part" finger) ("has part" "is opposite of" "is part of")}
(chimpanzee ~ ape) ⇐ (chimpanzee "is a" ape)
(chimpanzee is alive) ⇐ {(chimpanzee "is a" ape) (ape is alive)}
(ape "is for example" chimpanzee) ⇐ {(chimpanzee "is a" ape) ("is a" "is opposite of" "is for example")}
("green mint" ~ mint) ⇐ ("green mint" "is a" mint)
(mint "is for example" "green mint") ⇐ {("green mint" "is a" mint) ("is a" "is opposite of" "is for example")}
(transitive "is attribute of" "has attribute") ⇐ {("has attribute" is transitive) (is "is opposite of" "is attribute of")}
(transitive "is attribute of" ~) ⇐ {(~ is transitive) (is "is opposite of" "is attribute of")}
(transitive "is attribute of" "is ancestor of") ⇐ {("is ancestor of" is transitive) (is "is opposite of" "is attribute of")}
(alive "is attribute of" chimpanzee) ⇐ {(chimpanzee is alive) (is "is opposite of" "is attribute of")}
(sweet "is attribute of" "green mint") ⇐ {("green mint" is sweet) (is "is opposite of" "is attribute of")}
(transitive "is attribute of" "has part") ⇐ {("has part" is transitive) (is "is opposite of" "is attribute of")}
(alive "is attribute of" ape) ⇐ {(ape is alive) (is "is opposite of" "is attribute of")}
(hand "is part of" chimpanzee) ⇐ {(chimpanzee "has part" hand) ("has part" "is opposite of" "is part of")}
(finger "is part of" hand) ⇐ {(hand "has part" finger) ("has part" "is opposite of" "is part of")}
(lamiacea "is for example" mint) ⇐ {(mint "is a" lamiacea) ("is a" "is opposite of" "is for example")}
(mint "is for example" "water mint") ⇐ {("water mint" "is a" mint) ("is a" "is opposite of" "is for example")}
(lamiacea "is for example" catnip) ⇐ {(catnip "is a" lamiacea) ("is a" "is opposite of" "is for example")}
(mint "is for example" peppermint) ⇐ {(peppermint "is a" mint) ("is a" "is opposite of" "is for example")}
("green mint" ~ lamiacea) ⇐ {("green mint" ~ mint) (~ is transitive) (mint ~ lamiacea)}
```

The results demonstrate zelph's powerful inference capabilities.
It not only answers the specific query about who is an ancestor of pius, but it also derives numerous other facts based on the rules and base facts provided in the script.

## Multi-language Support

zelph allows nodes to have names in multiple languages. This feature is particularly useful when integrating with external knowledge bases. The preferred language can be set in scripts using the `.lang` command:

```
.lang zelph
```

This capability is fully utilized in the Wikidata integration, where node names include both human-readable labels and Wikidata identifiers. An item in zelph can be assigned names in any number of languages, with Wikidata IDs being handled as a specific language ("wikidata").

## Importing Scripts: Module IDs and Interchangeable Implementations

`.import <script>` loads and executes a zelph (`.zph`) or Janet (`.janet`)
script, resolving first against the working directory and then the standard
library (the `.zph` extension is optional).

### Import once

Every imported `.zph` script is registered under a **module ID** — by default
its lowercase file name without extension (`binary-arithmetic` for
`binary-arithmetic.zph`). A script whose ID is already registered is skipped,
much like `#pragma once` in C++. Scripts can therefore declare their
prerequisites with plain `.import` lines at the top; shared dependencies are
never loaded twice, and import cycles terminate. `.new` clears the registry.
Janet scripts are exempt: they are runnable programs and may be executed
repeatedly, e.g. with different arguments.

The registry is session state and is deliberately **not** part of a `.save`
file — see [Rules Say Themselves Only Once](#rules-say-themselves-only-once)
for why re-importing a module after `.load` is both necessary and free.

### Rules Say Themselves Only Once

Facts are hash-consed: the node _is_ its structure, so asserting the same
fact twice does nothing. Rules used to be the exception. A rule contains
variables, variables are allocated fresh for each statement, and a node
built from fresh variables is a fresh node — so entering the same rule
twice gave you two rules deriving the same consequences at twice the
unification cost.

zelph therefore recognises a rule it already has. A `... => ...` statement
is compared against the existing rules **up to a renaming of its
variables** (alpha-equivalence, as in the lambda calculus); if one matches,
the newly built rule is rolled back and the statement evaluates to the rule
that was already there. What counts as "the same rule" is exactly what the
reasoner reads — set membership, the conjunction and negation tags, and each
condition's subject, predicate and objects — so two rules that survive the
check are guaranteed to behave differently.

This matters most in a place where it is easy to miss. `.load` restores the
graph but not the Janet side of a module (`zelph/number`, digit alphabets,
display schemes), so working with a saved arithmetic network means
re-importing the module:

```
.load math.bin
.import math          # brings back &-literals -- and adds no second rule set
```

Without rule identity that sequence doubled every rule in the file.

Two boundaries: the check runs on parsed `=>` statements, which covers
`.import` and the REPL, but not [`zelph/rule`](janet.md) — a Janet program
building rules programmatically owns the nodes it passes in, and zelph does
not second-guess it. And a rule whose conditions mention a literal set
(`{...}` in a term position) is never recognised as a duplicate, because
each such set is a fresh node; the outcome is the old behaviour, never a
wrong one.

### Interchangeable implementations: `.provides`

A script can claim **additional** module IDs with

    .provides <id> [<id2> ...]

placed at the top of the file. Scripts claiming the same ID are
interchangeable — and mutually exclusive — implementations of one capability:
whichever is imported first wins, and later providers of the ID are skipped.

The three arithmetic substrates all declare `.provides arithmetic`. Dependent
modules simply import the default substrate (`.import binary-arithmetic`); to
compute on a different substrate, import it **before** anything that depends
on arithmetic:

    .import binary-nand-arithmetic   # claims "arithmetic" first
    .import symbolic-core            # its ".import binary-arithmetic" is skipped

If a directly requested script is skipped because a _different_ script
already provides one of its IDs, zelph prints a warning — you asked for a
specific implementation, but an alternative is already active.

## Project Status

The project is currently in beta phase. The core functionality has been rigorously tested against the full Wikidata dataset and is operational.
Comprehensive automated tests are run with every commit, see https://github.com/acrion/zelph/blob/main/src/test/CMakeLists.txt. Contributor-facing documentation of the engine's internal performance architecture — and of the measurement methodology used to develop it — lives
in the [Internals](internals/performance.md) section.

Current focus areas include:

- **Graph-based arithmetic and symbolic mathematics**: no longer a proof of concept. A complete stack is in the standard library, every layer of it ordinary zelph rules: positional arithmetic over interchangeable digit substrates — one of which derives its entire digit level from a single NAND axiom — then signed integers, multivariate polynomial normal forms over ℤ, a terminating term simplifier, symbolic differentiation, and a compiler that decides polynomial identities by node identity. It proves Euler's four-square identity in a fifth of a second, and reproduces the July-2026 counterexample to the [Jacobian conjecture](math/tutorial-jacobian.md) — nine symbolic partial derivatives and a 3×3 determinant over ℤ — in under two. Every answer carries a reconstructible proof down to the digit tables. See [Mathematics](math/index.md); the comparison with [Lean](https://lean-lang.org) still holds, except that here the foundation is a graph-native, homoiconic representation rather than a type theory.
- **Transitive reasoning and Wikidata integration**: A [second Wikimedia Rapid Fund project](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Transitive_Reasoning,_Qualifier_Support,_and_SPARQL-Subset_Integration_(ID:_23759260)>) delivered transitive reasoning over Wikidata's subclass hierarchy ([native closures with a cached adjacency index](sparql.md#performance-and-the-adjacency-index)), [qualifier support](qualifiers.md), and [SPARQL-subset integration](sparql.md) in release 0.9.6 — capabilities that also serve as building blocks for more general symbolic computation. The next direction it opens up is qualifier-dependent property-constraint checking.
- **Neural networks in the graph**: Since 0.9.7, zelph embeds a neural substrate directly in the semantic network — weighted edges as synapses, layers as ordinary sets, and rule conditions that consult trained networks via the `≈` operator. See [Neural Networks in the Graph](neural.md).
- **Potential Wikidata integration**: Exploring pathways for integration with the Wikidata ecosystem, e.g. the [WikiProject Ontology](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology).

Regarding potential Wikidata integration and the enhancement of semantic scripts, collaboration with domain experts would be particularly valuable. Expert input on conceptual alignment and implementation of best practices would significantly accelerate development and ensure optimal compatibility with existing Wikidata infrastructure and standards.

### Where the logic goes next

The reasoning engine has its own open directions, independent of the
mathematical stack built on top of it:

- **Negation over a group of conditions.** `¬` applies to one fact pattern.
  `¬(A, B)` — "not both" — is [rejected](logic.md#negation-as-failure)
  rather than approximated, because the honest implementation is a nested
  negation-as-failure over a conjunctive subgoal: run the subgoal's search
  and succeed only if it yields no binding. The interesting part is not the
  search but the [stratification](logic.md#stratified-evaluation): a
  negated group must be deferred exactly like a negated pattern, and its
  own conditions may themselves be negated. Until then, De Morgan plus
  several rules expresses the same thing.

### Where the mathematics goes next

The mathematical stack will keep growing, and which way it grows should depend on what mathematicians actually want from it. Several directions are open, and none of them is committed:

- **Side conditions.** Identities in the symbolic layer are currently *formal*: `exp inverseof ln` silently assumes the principal branch and u > 0. Tracking conditions as ordinary facts — so that a rewrite carries its domain with it — fits the architecture, but the right granularity is a mathematical question, not an engineering one.
- **Beyond a single polynomial ring.** Ideal membership and Gröbner bases are the obvious next layer above the existing normal forms, and Buchberger's algorithm is a fixpoint computation — an unusually good match for forward chaining. Modular arithmetic and finite fields would be cheaper and would open number theory.
- **Rationals, and with them division.** The quotient rule is missing from differentiation for exactly one reason: there is no field to divide in yet.
- **One-shot normalisation.** Rewriting is single-pass per request today. Equality saturation in the e-graph style is, at heart, forward chaining over equalities — the natural experiment, and one the current design deliberately left room for.
- **Proofs that leave the system.** `.explain` already reconstructs a full justification from the saturated graph. Exporting it in a form another proof checker accepts would turn zelph's answers into externally verifiable ones.

If one of these matters to your work — or if the interesting direction is one not listed here — that is precisely the feedback that would shape the roadmap. Issues and discussions are on [GitHub](https://github.com/acrion/zelph).

## Project History

zelph has been in continuous development since 2012, when it began as a C# application called "NeoCortex" ([archived project page from 2012, in German](https://web.archive.org/web/20120826111106/http://www.zipproth.de/entwicklung_einer_neuartigen_inferenzmaschine.html)). The core idea was already the same: the inference engine is not external to the semantic network but part of it — rules are network structures, and the system can make statements about itself. The current C++ engine is a from-scratch realization of that idea.

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
cmake -D CMAKE_BUILD_TYPE=Release -B build .
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
./build/bin/zelph stdlib/examples/english.zph
```

## Licensing

zelph is dual-licensed:

1. **AGPL v3 or later** for open-source use,
2. **Commercial licensing** for closed-source integration or special requirements.

We would like to emphasize that offering a dual license does not restrict users of the normal open-source license (including commercial users).
The dual licensing model is designed to support both open-source collaboration and commercial integration needs.
For commercial licensing inquiries, please contact us at [https://acrion.ch/sales](https://acrion.ch/sales).
