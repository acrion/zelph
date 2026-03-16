## Logic and Computation

zelph is a logic programming system, but not in the traditional sense.
Where Prolog operates on terms and Datalog on flat relations, zelph stores _everything_ — facts, rules, predicates, even numbers — as structures in a single semantic network.
Computation does not happen outside the graph; it _emerges from_ the graph, through rule-driven inference over its topology.

This page introduces zelph's reasoning capabilities from a logic programming perspective.
For the basic syntax of facts, sets, and lists, see the [main documentation](index.md).
For a hands-on quick start, see the [Quick Start Guide](quickstart.md).

### Positioning: Forward Chaining over Graphs

zelph's inference engine performs **forward chaining** (bottom-up evaluation), similar to Datalog or production rule systems.
When a new fact is asserted or a rule is defined, the engine immediately checks whether any rule conditions are newly satisfied and derives all possible consequences.
This process repeats until a fixed point is reached — no further facts can be derived.

This is fundamentally different from Prolog's top-down, goal-driven search with backtracking.
zelph does not search for proofs; it _materializes all derivable facts_ in the graph.
The trade-off is deliberate: forward chaining integrates naturally with knowledge graphs (where facts arrive incrementally, e.g. from Wikidata imports) and guarantees termination for Datalog-safe rules.

What sets zelph apart from both Prolog and Datalog is the _representation_: rules, predicates, and even the concept of conjunction are themselves nodes in the graph.
This [homoiconicity](#the-executable-graph) enables meta-reasoning, self-referential structures, and a seamless boundary between knowledge and computation.

## The Executable Graph

A defining characteristic of zelph is its [homoiconicity](https://en.wikipedia.org/wiki/Homoiconicity): logic (code) and facts (data) share the exact same representation.

In many traditional semantic web stacks (like OWL/RDF), the ontology is _descriptive_. For example, an OWL "cardinality restriction" describes a constraint, but the actual logic to enforce that constraint resides hidden in the external reasoner's codebase (e.g., [HermiT](http://www.hermit-reasoner.com) or [Pellet](https://github.com/stardog-union/pellet)). The operational semantics are external to the data.

In zelph, **the logic is intrinsic to the data**.

- **Rules are Data:** Inference rules are not separate scripts; they are specific topological structures within the graph itself. The implication operator `=>` is a standard relation node; the conditions form a set tagged as a conjunction — all represented by ordinary subject–predicate–object triples.
- **Predicates are Nodes:** Every relation type (including user-defined ones) is a first-class node in the graph, not an edge label. This means you can write rules _about_ predicates — declaring symmetry, transitivity, or functional constraints as graph-level properties.
- **Math is Data:** Numbers are not opaque literals but Lisp-style cons-lists of digit nodes that interact with semantic entities through the same rule mechanism.

This means the graph doesn't just _describe_ knowledge; it _structures the execution_ of logic. The boundary between "data storage" and "processing engine" is effectively removed. Consequently, importing data (e.g., from Wikidata) can immediately alter the computational behavior of the system.

## Rules Over Graphs

### Basic Rules and Conjunction

A rule in zelph connects a set of **conditions** to a **consequence** via the `=>` operator.
The conditions form a conjunction: all must match simultaneously under a shared variable binding.

The idiomatic syntax uses commas to separate conditions:

```
(cond1, cond2, cond3) => consequence
```

Variables are single uppercase letters (`A`–`Z`) or identifiers starting with `_` (e.g. `_Var`).
They are scoped to the current rule and universally quantified: the rule fires for **all** bindings that satisfy the conditions.

A classic example — transitive closure:

```
zelph> (R is transitive, A R B, B R C) => (A R C)
zelph> > is transitive
zelph> 6 > 5
zelph> 5 > 4
 6  >  4  ⇐ {( 6  >  5 ) (>  is   transitive ) ( 5  >  4 )}
```

After entering `5 > 4`, the engine finds that the three conditions are jointly satisfiable with `R = >`, `A = 6`, `B = 5`, `C = 4`, and derives `6 > 4`.

### Meta-Rules: Predicates as First-Class Nodes

Notice that `R` in the transitive rule is a **variable ranging over predicates**.
This is possible because predicates in zelph are nodes, not edge labels.
Any relation declared `is transitive` automatically benefits from this single rule — no separate rule per predicate is needed.

This enables a class of rules that are difficult or impossible to express in standard Datalog or Prolog: rules that _reason about relations themselves_.

**Example — Symmetric relations:**

```
zelph> (R is symmetric, X R Y) => (Y R X)
zelph> friend is symmetric
zelph> alice friend bob
( bob   friend   alice ) ⇐ ...
```

A single rule declares the semantics of symmetry for _any_ relation.
Declaring `friend is symmetric` is a fact about the predicate `friend`; the rule matches it and closes over all instances.

**Example — Opposite relations:**

```
zelph> (R "is opposite of" S, X R Y) => (Y S X)
zelph> "has part" "is opposite of" "is part of"
zelph> chimpanzee "has part" hand
( hand   is part of   chimpanzee ) ⇐ ...
```

Declaring that `"has part"` is opposite of `"is part of"` causes every `has part` fact to automatically generate its inverse.
The rule is generic: it works for any pair of opposite relations without modification.

### Deep Unification

zelph's unification engine matches patterns against **arbitrarily nested** structures.
This is essential for reasoning about statements-about-statements — a natural consequence of zelph's graph topology where fact nodes can themselves appear as subjects or objects of other facts.

```
zelph> ((A + B) = C) => (test A B)
zelph> (4 + 5) = 9
( test   4   5 ) ⇐ ...
```

The rule's condition pattern `((A + B) = C)` requires a fact whose _subject_ is itself a fact matching `(A + B)`.
The engine recursively walks the graph topology, binding `A = 4`, `B = 5`, `C = 9`.

This extends to arbitrary depth:

```
zelph> (subj pred (obj is (subj2 A (b test C)))) => (success A C)
zelph> subj pred (obj is (subj2 a_val (b test c_val)))
( success   a_val   c_val ) ⇐ ...
```

Deep unification also works within conjunction conditions, enabling rules that combine structural decomposition with multi-condition reasoning:

```
zelph> ((A + B) = C) => (test A B)
zelph> (4 + 5) = 9
zelph> (*{ ((A + B) = C) (B followed-by D) (C followed-by E) } ~ conjunction) => ((A + D) = E)
zelph> 5 followed-by 42
zelph> 9 followed-by 43
(( 4   +   42 )  =   43 ) ⇐ ...
```

This rule decomposes the nested equation `(A + B) = C`, uses the bound values to look up successor relationships, and assembles a new equation — all in a single inference step.

### Facts with Multiple Objects

A fact in zelph connects a subject to an object via a predicate.
When a fact has **multiple objects** (e.g. `f maps 1 2`), these objects form an **unordered set** — there is no defined ordering among them.
The statement `f maps 1 2` means: `f` is related via `maps` to both `1` and `2`, but it does **not** encode which is "first" and which is "second".

This is a deliberate design choice: zelph's internal topology represents objects as a set of incoming connections to the fact node, not as a sequence.

When order matters — for example, to represent a mapping from a domain element to a codomain element — you must use **lists** (cons-lists via angle brackets `<...>`), which provide an explicit structural ordering.
See [Angle Brackets: Lists](index.md#angle-brackets-lists) for details on the list syntax.

**Example — unordered objects in rules:**

```
zelph> alice parent_of bob charlie
zelph> (A parent_of B) => (B child_of A)
( bob   child_of   alice ) ⇐ ...
( charlie   child_of   alice ) ⇐ ...
```

The rule matches each object independently. Whether `bob` or `charlie` was written first is irrelevant — both are equally "objects of" the `parent_of` fact.

This distinction becomes critical in more complex rules. Consider a naïve attempt at function composition using multiple objects:

```
zelph> (F maps A B, G maps B C) => ((G compose F) maps A C)
zelph> f maps 1 2
zelph> g maps 2 3
```

This produces not only `(g compose f) maps 1 3` but also `(f compose g) maps 1 3` and further spurious compositions — because the engine cannot distinguish "domain" from "codomain" when both are unordered objects of the same fact.

**Example — Function composition with lists:**

The correct approach uses **ordered lists** to encode the domain–codomain relationship:

```
zelph> (F maps <A B>, G maps <B C>) => ((G compose F) maps <A C>)
zelph> f maps <item1 item2>
zelph> g maps <item2 item3>
(( g   compose   f )  maps  < item1   item3 >) ⇐ ...
```

The list `<A B>` is a cons-list with a defined structure: `A` is the first element (car) and `B` is the rest (cdr).
Deep unification matches this structure precisely, so `A` unambiguously binds to the domain element and `B` to the codomain element.

The consequence `(G compose F)` creates a _new fact node_ representing the composition, which then becomes the subject of `maps`.
This is higher-order reasoning expressed as first-order graph topology — with the structural ordering provided by cons-lists rather than by positional assumptions about objects.

### Negation as Failure

Negation in zelph follows **negation-as-failure** (NAF) semantics, familiar from Datalog with stratified negation and Prolog's `\+`.
A negated condition succeeds when **no** fact in the graph matches the pattern under the current variable bindings.

The idiomatic syntax uses `¬`:

```
zelph> (A is yellow, ¬(A is green)) => (A "is not" green)
zelph> plant is green
zelph> plant is yellow
zelph> plant2 is yellow
( plant2   is not   green ) ⇐ ...
```

`plant` is both yellow and green, so `¬(plant is green)` fails and the rule does not fire for `plant`.
`plant2` is yellow but not green, so the rule fires.

Negation is particularly powerful for structural queries — finding elements that _lack_ a specific connection:

```
zelph> elem1 --> elem2
zelph> elem2 --> elem3
zelph> elem3 --> elem4
zelph> elem4 --> elem5
zelph> elem1 partoflist mylist
zelph> elem2 partoflist mylist
zelph> elem3 partoflist mylist
zelph> elem4 partoflist mylist
zelph> elem5 partoflist mylist
zelph> (A partoflist L, ¬(A --> X)) => (A "is last of" L)
( elem5   is last of   mylist ) ⇐ ...
```

The negated condition `¬(A --> X)` succeeds only when `A` has no outgoing `-->` link — identifying the last element purely declaratively.

The explicit (ASCII-only) equivalent of `¬(pattern)` is `*(pattern) ~ negation`, using the [focus operator `*`](index.md#the-focus-operator-).

### Inequality Constraints

The `!=` operator is a **built-in guard constraint** — not a fact lookup.
It filters variable bindings after the involved variables are bound by positive conditions.

**Key design decision:** In zelph, two different variable names **may bind to the same node**.
This is consistent with standard first-order logic where `∀x ∀y. P(x,y)` does not exclude `x = y`.
To require distinct bindings, an explicit `!=` constraint is needed.

```
zelph> (A prop X, A prop Y, X != Y) => (A has_pair X Y)
zelph> a prop v
```

With only one value `v`, the binding `X = v, Y = v` is blocked by `!=`, so the rule does not fire.

```
zelph> a prop v1
zelph> a prop v2
( a   has_pair   v1   v2 ) ⇐ ...
```

Once distinct values exist, the rule fires for the distinct pairings.

**Practical use case — detecting functional-property violations** (a pattern from Wikidata ontology work):

```
zelph> (P is functional, A P X, A P Y, X != Y) => !
zelph> date_of_birth is functional
zelph> alice date_of_birth 1990
zelph> alice date_of_birth 1991
 !  ⇐ ...
Found one or more contradictions!
```

Without `!=`, the rule would also fire when the same value is entered redundantly, which is not a real conflict.

**Why `!=` matters — preventing spurious deductions:**

Without `!=`, rules that quantify over pairs can produce false positives from reflexive bindings:

```
(X opposite Y, A ~ X, A ~ Y) => !
bright opposite bright
yellow ~ bright
```

Without `!=`, the engine binds `X = bright, Y = bright`, satisfies all conditions, and fires the contradiction — even though `yellow` is merely classified under the same category twice.
Adding `X != Y` blocks this reflexive binding and prevents the false positive.

**Constraint checking — Graph coloring:**

Multiple `!=` constraints can enforce pairwise distinctness, enabling constraint-checking patterns:

```
zelph> (A adjacent B, A color X, B color X) => !
zelph> r1 adjacent r2
zelph> r2 adjacent r3
zelph> r1 color red
zelph> r2 color blue
zelph> r3 color red
```

No contradiction — the coloring is valid. But assigning `r2 color red` instead would trigger the contradiction, since adjacent regions `r1` and `r2` would share the same color.

### Fresh Variables: Generative Rules

Variables that appear **only in the consequence** of a rule are treated as fresh: the engine generates new anonymous nodes for them during inference.

```
zelph> (A is human) => (B nameof A)
zelph> tim is human
 ??   nameof   tim  ⇐  tim   is   human
```

The `??` represents a newly created node — an existential witness materialized in the graph.

**Termination guarantee:** Before creating a new node, zelph checks whether the deduced facts (with the fresh variable as wildcard) already exist. If they do, no new deduction occurs. This ensures that generative rules converge.

This mechanism is fundamental for constructive reasoning, such as building new cons-list structures during arithmetic (see below).

## A Predicate Logic Perspective

For readers with a background in formal logic, here is how zelph's constructs map to first-order logic.

### Universal Quantification

Variables in rule conditions behave like universally quantified variables.
The transitive-closure rule

```
(R ~ transitive, X R Y, Y R Z) => (X R Z)
```

reads as:

$$
\forall R\, \forall X\, \forall Y\, \forall Z.\; \bigl(\text{transitive}(R) \wedge R(X,Y) \wedge R(Y,Z)\bigr) \to R(X,Z)
$$

with the caveat that quantification ranges over the current fact base (closed-world evaluation), not over all possible interpretations.

### Existential Quantification

Fresh variables (those appearing only in the consequence) correspond to constructive existential quantification.
In Skolem-function terms, each fresh variable is implicitly Skolemized relative to the universally quantified condition variables.

### Conjunction

The comma-separated condition syntax `(cond1, cond2, cond3)` is a conjunction: all conditions must match under a **shared** variable assignment. The conditions are evaluated as a joint constraint (the semantics are order-independent, even if the engine may internally choose an evaluation order for efficiency).

### Negation

`¬(Pattern)` corresponds to **negation-as-failure** (NAF) — the same semantics as in Datalog with stratified negation or Prolog's `\+`.
It tests the _absence of evidence_ in the current graph state, not the _evidence of absence_ in the model-theoretic sense.
Readers familiar with Answer Set Programming (ASP) or well-founded semantics will recognize this as a form of default negation operating over the graph's Herbrand base.

### Inequality

The `!=` operator corresponds to `dif/2` in Prolog or disequality constraints in CLP(FD).
It is a guard, not a fact pattern.

### Disjunction

zelph currently supports conjunction but not explicit disjunction in rule conditions.
As in Datalog, disjunction is expressed through **multiple rules with the same consequence pattern**:

```
(A is bird) => (A can fly)
(A is bat) => (A can fly)
```

This is equivalent to `(bird(A) ∨ bat(A)) → can_fly(A)`.

## Semantic Math: Computation as Graph Rewriting

Numbers in zelph are not opaque primitives.
They are **cons-lists of digit nodes** — topological structures within the graph, built from the same `cons` and `nil` predicates used for any list.
Arithmetic is not hard-coded; it is defined by inference rules that transform these structures.

This architecture has a remarkable consequence: _calculating_ numbers and _reasoning about_ numbers happen in the same system.
If a knowledge base (e.g. Wikidata) records that 13 is a prime number, that semantic fact is directly accessible wherever the list `<13>` appears in a computation.

### Numbers as Cons-Lists

The list `<42>` is internally stored as `2 cons (4 cons nil)` — least significant digit first.
The display reverses the order for human readability, so results appear in conventional notation.
For details on the list syntax, see [Angle Brackets: Lists](index.md#angle-brackets-lists).

This representation means digits are ordinary named nodes.
The node `"4"` is the same node everywhere in the graph — in a number, in a Wikidata entity, in a classification.

### Peano-style Successor Addition

The simplest possible addition rule uses a successor table:

```
zelph> <0> followed-by <1>
zelph> <1> followed-by <2>
zelph> <2> followed-by <3>
zelph> ...

zelph> (A followed-by B) => ((<1> + A) = B)
```

This states: if `A` is followed by `B` in the number succession, then `1 + A = B`.
The engine derives `(<1> + <0>) = <1>`, `(<1> + <1>) = <2>`, etc.

The key point: `followed-by` is a user-defined relation. zelph has no arithmetic kernel. The rule works because the graph contains the corresponding facts.

### Rule-based Multi-digit Addition

zelph can perform **arbitrary-precision addition** purely via graph rules.
The reference implementation lives in [sample_scripts/arithmetic.zph](https://github.com/acrion/zelph/blob/main/sample_scripts/arithmetic.zph).

The algorithm consists of three parts:

**1. A digit-level lookup table** (generated programmatically via [Janet](janet.md)):

For all digits `a,b ∈ {0..9}` and carry-in `c ∈ {0,1}`, two facts encode the sum and carry-out:

- `((a d+ b) ci c) sum s` where `s = (a + b + c) mod 10`
- `((a d+ b) ci c) co e` where `e = ⌊(a + b + c) / 10⌋`

This turns digit arithmetic into ordinary facts in the network — 200 entries total.

**2. Base cases** for the recursion endpoint (when both operands are `nil`):

- `((nil add nil) ci 0) sum nil`
- `((nil add nil) ci 1) sum <1>`

**3. Eight inference rules** that decompose, propagate carries, assemble results, and connect to the user-facing `=` predicate.

The rules handle three cases each for decomposition (both operands non-nil, left nil, right nil) and assembly, plus a trigger rule and a connection rule.

#### A Worked Example

```
.import sample_scripts/arithmetic.zph
<12345> + <98765>
```

**Trigger (Rule A0):** Seeds the internal addition state with carry-in 0:

```
((<12345>  add  <98765>)  ci   0 )
```

**Decomposition (Rules D1–D3):** Peels off least-significant digits, propagates carry:

```
((<1234>  add  <9876>)  ci   1 )
((<123>  add  <987>)  ci   1 )
((<12>  add  <98>)  ci   1 )
((<1>  add  <9>)  ci   1 )
```

**Base case:** The recursion ends at `nil + nil` with carry-in 1:

```
((( nil   add   nil )  ci   1 )  sum  <1>)
```

**Assembly (Rules As1–As3):** Constructs the result on the way back up, prepending digits via `cons`:

```
(((<1>  add  <9>)  ci   1 )  sum  <11>)
(((<12>  add  <98>)  ci   1 )  sum  <111>)
...
(((<12345>  add  <98765>)  ci   0 )  sum  <111110>)
```

**Connection (Rule C0):** Exposes the result under the user-facing `=` predicate:

```
((<12345>  +  <98765>)  =  <111110>)
```

Nothing in the engine is hard-coded for addition.
The computation emerges from the same topological primitives used for ordinary knowledge representation — facts, conjunctions, cons-lists — plus eight generic inference rules.

### Semantic Integration with Knowledge Graphs

Because list elements are ordinary graph nodes, any arithmetic rule that produces a digit automatically inherits all semantic facts known about that digit.
If the Wikidata graph is loaded, querying all prime numbers is a standard pattern query:

```
.lang wikidata
X P31 Q49008
```

This lists all 10,018 prime numbers recorded in Wikidata — the same nodes that would appear in a cons-list produced by arithmetic rules.
Knowledge and computation are not separate layers.

## Beyond Arithmetic

The techniques demonstrated by the addition algorithm — deep unification, recursive decomposition via cons-lists, fresh variable generation, and digit-level lookup tables — are general-purpose.
They apply whenever computation can be expressed as structure transformation over a graph.

### Statements About Statements

Because fact nodes can themselves appear as subjects or objects, zelph naturally supports **higher-order assertions**.
For example, declaring a relation to be symmetric, transitive, or functional is a statement about a predicate — and the inference engine treats it as an ordinary fact that conditions can match against.

This enables concise, generic rules that would require meta-interpreters or reflection mechanisms in traditional logic programming systems.

### Contradiction Detection

Rules with `!` as the consequence detect logical inconsistencies:

```
zelph> (X "is opposite of" Y, A ~ X, A ~ Y, X != Y) => !
zelph> bright "is opposite of" dark
zelph> yellow ~ bright
zelph> yellow ~ dark
 !  ⇐ ...
Found one or more contradictions!
```

When a contradiction is detected during fact assertion, the contradictory fact is _not_ entered into the graph.
Instead, a record of the contradiction is stored, making it visible in reports.
This mechanism is central to zelph's [Wikidata ontology work](wikidata.md), where thousands of constraint violations in the knowledge graph are detected automatically.

## The Programmatic Layer: Janet

zelph embeds [Janet](https://janet-lang.org), a lightweight functional programming language, as its scripting layer.
Janet serves as a powerful macro system: it constructs graph structures that are then processed by zelph's reasoning engine.
During inference, only zelph's native engine runs.

Janet is used for:

- **Generating facts programmatically** — e.g. the 200-entry digit addition lookup table is generated by a Janet loop, not entered manually.
- **Parameterized rules** — functions that create rule topologies for any relation.
- **Querying and inspecting the graph** — `zelph/query` returns results as Janet data structures.
- **External integration** — Janet's standard library provides file I/O, networking, and data processing, enabling zelph to exchange data with external systems.

For the full Janet API reference and examples, see the [Janet scripting documentation](janet.md).

**A quick taste** — generating a transitive rule for any relation:

```
%
(defn transitive-rule [rel]
  (zelph/rule
    [(zelph/fact 'X rel 'Y)
     (zelph/fact 'Y rel 'Z)]
    (zelph/fact 'X rel 'Z)))

(transitive-rule "is part of")
(transitive-rule "is ancestor of")
%
```

A single function call creates the entire rule topology — conjunction set, conditions, consequence, and the `=>` link — for any named relation.

## Getting Started

To try zelph yourself, see the [Quick Start Guide](quickstart.md) for pre-compiled binaries on all major platforms.
The interactive REPL lets you enter facts and rules and observe inference in real time — every example on this page can be entered directly.

The project is open source (AGPL v3 / commercial dual license) and hosted on [GitHub](https://github.com/acrion/zelph).
