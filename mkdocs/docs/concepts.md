# Core Concepts

## Semantic Network Structure

In zelph, knowledge is represented as a network of nodes connected by relations.
Unlike traditional semantic networks where relations are labeled edges,
zelph treats relation types as first-class nodes themselves.
This unique approach enables powerful meta-reasoning about relations.

## Predefined Core Nodes

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

## Homoiconicity: The Executable Graph

A defining characteristic of zelph is its [homoiconicity](https://en.wikipedia.org/wiki/Homoiconicity): logic (code) and facts (data) share the exact same representation.
Rules are not separate scripts; they are topological structures within the graph.
Math is not hard-coded; numbers are cons-lists of digit nodes that interact with semantic entities through the same rule mechanism.

This means the graph doesn't just _describe_ knowledge; it _structures the execution_ of logic.
For a detailed exploration of this concept and its implications for computation, see [Logic and Computation](logic.md).

## Facts and Relations

A _fact_ in zelph is a statement node created from a **subject**, a **predicate**, and an _object_:

```
subject predicate object
```

A predefined predicate is `~`, used for classification:

```
X ~ Y
```

This can be read as "X is an instance of class Y", but depends on the context of your dataset. It is used in zelph's [internal topology](rules.md#internal-representation-of-facts) — there is no need to actually use it in your scripts.

### Working with Custom Relations

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

## Nested Expressions and Sets

zelph supports advanced grouping and recursion using parentheses `()`, braces `{}`, and angle brackets `<>`, plus a shorthand prefix `:` for [self-facts](#the-self-fact-prefix).

### Parentheses: Nested Facts

A parenthesised statement `(s p o)` creates the fact and evaluates to the **statement node** (i.e. the relation/fact node). This lets you make statements _about_ statements:

```
(bright "is opposite of" dark) "is a" "symmetric relation"
```

Here, the subject of the outer statement is the node representing the inner fact.

> Note: A line consisting of only a bare nested fact like `(subject rel object)` is not a valid _top-level_ statement in the REPL; nested facts are meant to be used _as parts_ of a larger statement.

### Braces: Set Constants and Collections

zelph distinguishes two kinds of unordered grouping, because mathematics and
programming mean different things by "a set".

| Literal | What it is | Identity | Can membership grow? |
| --- | --- | --- | --- |
| `{a b c}` | **set constant** | its members | no |
| `@{a b c}` | **collection** | its own | yes |

A mathematical set is determined by its members — that is the axiom of
extensionality — so `{a b}` written twice denotes one and the same set, and
there is no operation that adds an element to it; you form a new set. A
collection is a container: it has an identity of its own, and putting
something into it is exactly what one does with it.

The `@` marker follows [Janet](https://janet-lang.org/), the language zelph
embeds, where `{...}` is the immutable struct and `@{...}` the mutable table.
It costs no reserved character: `@` stays an ordinary character inside names,
and only `@{` is special.

#### Set constants

```
zelph> A in { elem1 elem2 elem3 }
A  in  { elem1   elem2   elem3 }
Answer:   elem2    in  { elem1   elem2   elem3 }
Answer:   elem1    in  { elem1   elem2   elem3 }
Answer: elem3 in {elem1 elem2 elem3}
```

Two occurrences of the same literal are the same node, and the order inside
the braces does not matter:

```
zelph> p q {a b}
zelph> r s {a b}
zelph> S q O
Answer: p q {a b}
zelph> S s O
Answer: r s {a b}
```

That identity is what makes a set literal usable in a **rule**: the set in the
condition is the set in the data, so quantifying over its members works.

```
zelph> (X in {red green blue}) => (X is-a-colour yes)
(green is-a-colour yes) ⇐ (green in {red green blue})
(red is-a-colour yes) ⇐ (red in {red green blue})
(blue is-a-colour yes) ⇐ (blue in {red green blue})
zelph> S is-a-colour yes
Answer: green is-a-colour yes
Answer: blue is-a-colour yes
Answer: red is-a-colour yes
```

Membership in a constant is definitional rather than asserted, so extending
one is refused — and the message names the literal that can be extended:

```
zelph> x in {a b}
Error in line "x in {a b}": fact(): a set constant cannot be extended -- {a b} IS its members. Write the collection literal @{...} for a container that membership can grow.
```

Saying what already holds is not an extension: `a in {a b}` is true by
construction and is simply a no-op.

A **rule consequence** is the one place where the refusal arrives during
reasoning rather than while the line is read: `X in {a b}` is how a rule
quantifies over the members, so the pattern is fine and only its instances
are refused.

```
zelph> z rel {a b}
zelph> q p r
zelph> (X p Y) => (X in {a b})
! ⇐ (q p r)
   └─ refused: a set constant cannot be extended -- {a b} IS its members. Write the collection literal @{...} for a container that membership can grow.
Found one or more contradictions!
```

#### Collections

A collection is built fresh by each literal, so two of them are two containers
— and each takes its own members:

```
zelph> x in @{a b}
zelph> y in @{a b}
zelph> S in O
Answer: a in @{a b y}
Answer: x in @{a b x}
Answer: y in @{a b y}
Answer: b in @{a b y}
Answer: a in @{a b x}
Answer: b in @{a b x}
```

`a` and `b` appear twice because each literal built its own container and put
them into it. A set constant would have collapsed the two into one node.

This is what a rule can put things **into**. The container the rule writes is
named by every fact it derives, and it grows as the run proceeds:

```
zelph> alice reported bug1
zelph> bob reported bug2
zelph> (X reported Y) => (Y in @{X})
(X reported Y) => (Y in @{Y X})
zelph> S in O
Answer: bug1 in @{bug1 bug2}
Answer: bug2 in @{bug1 bug2}
```

(The two deductions are printed as well; which of them comes first varies with
the run, and so does the order of the answers – see
[Querying](queries.md#key-features).)

The rule and the facts it derives name **one and the same** container, so what
counts as its members depends on which of the two you are looking at. Inside
the rule the variables are the statement; in the data the gathered members
are. The rule therefore keeps saying what it says, however much it has
gathered:

```
zelph> .list-rules
(X reported Y) => (Y in @{Y X})
```

(`Y` is a member because `Y in @{X}` puts it there.)

Note the asymmetry, and that it is the point: quantify over a **set constant**
to read its members, write into a **collection** to gather results.

One consequence is worth knowing before it surprises you: a printed collection
literal **cannot be pasted back into a command**. `@{a b x}` in a `.explain` or
`.prune-facts` pattern builds yet another container, so it never names the one
the answer came from — and the command says so:

```
zelph> x in @{a b}
x in @{a b x}
zelph> a in O
Answer: a in @{a b x}
zelph> .explain (a in @{a b x})
Fact is not asserted -- nothing to explain.
A collection literal @{...} builds a NEW container, so it cannot name an existing one. Address the fact by its ID (.node without an argument reports the last answer's node), or use a set constant {...}, whose identity IS its members.
zelph> .explain
a in @{a b x}  [axiom]
```

The route that works is the argument-less form in the last line: it takes the
node of the last answer, so no literal has to be re-parsed.

A set constant has no such restriction — its identity *is* its members, so
`.explain (c in {c d})` addresses exactly the fact it names.

#### Which one a literal builds

Extensionality needs *known* members. A literal carrying a variable denotes a
different set for every binding, so it cannot be hash-consed and is a
collection:

```
zelph> a p b
zelph> (X p Y) => (X in {Y})
(X p Y) => (X in @{Y X})
(a in @{a}) ⇐ (a p b)
zelph> S in O
Answer: a in @{a}
```

The echo responds to the question the section asks: what was typed as `{Y}`
reappears as `@{Y X}`, a collection.

This is also what keeps the engine's own conjunction form working, whose
members are condition patterns and therefore never ground — see
[the rule section](rules.md#rules-and-inference) for `*{...} ~ conjunction`.

#### A literal a rule derives

While the rule is being read its members are unknown, but a *binding* makes
them known — so a container in a **consequence** is rebuilt from the
substituted members, as a set constant. The rule keeps showing its own
collection; each derived fact carries a set of its own:

```
zelph> a p b
zelph> c p d
zelph> (X p Y) => (X likes @{Y})
(a likes {b}) ⇐ (a p b)
(c likes {d}) ⇐ (c p d)
zelph> S likes O
Answer: c likes {d}
Answer: a likes {b}
```

Set constants are hash-consed, which is what makes this safe: re-deriving
lands on the same node, so the run reaches a fixpoint. A fresh collection per
binding would be a new node on every run and never converge.

Two containers are deliberately **not** rebuilt. One is the container a rule
writes *into*: `Y in @{X}` says something about that container, so its
identity survives substitution and the accumulator above keeps naming one
bucket. The other is a literal with nothing to substitute — `{red green}` or
`@{bucket}` — which is already what it will be.

#### Topology

Both kinds create a **super-node** representing the grouping itself, and link
the elements to it via the `in` (`PartOf`) relation.

- **Facts created for `{A B}` and for `@{A B}` alike:**
  - `A in SuperNode`
  - `B in SuperNode`

They differ only in how the super-node gets its identity: a set constant hashes
its members, a collection is a fresh node. Nothing else in the graph
distinguishes them, which is why the printed form does: `{...}` versus `@{...}`.

The empty literal of either kind — `{}` and `@{}` — is the node `nil`. There is
nothing for a super-node to collect, and `nil` is what an empty collection is
throughout zelph.

### Angle Brackets: Lists

Angle brackets `<...>` create **ordered lists** implemented as classic Lisp-style cons lists using the core predicates `cons` and `nil`.

A list is represented by the **outermost cons cell**. There is **no separate container node**: the node returned by list construction _is_ the list (exactly as in Lisp).

The empty list `<>` is `nil`, exactly as in Lisp: the terminator every
non-empty list ends at. `q p <>`, `q p {}` and `q p nil` are therefore the
same fact.

---

#### Two list syntaxes

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
     So `"abc"` becomes the element vector `["c","b","a"]`, yielding:
     - `Cell3 = "a" cons nil`
     - `Cell2 = "b" cons Cell3`
     - `Cell1 = "c" cons Cell2` ← this outermost cons cell **is the list**

This reversal is **not** "numeric logic" — it is simply the definition of the compact syntax and is useful for many right-to-left processing rules (including, but not limited to, arithmetic scripts).

Because of this rule, `<abc>` is internally identical to the explicit node list `<c b a>`.

---

#### Referring to the _same_ cons list in different ways

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

#### Display: storage order, and the numeral exception

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

#### Digits vs. numbers (emergent, not built-in)

The cons-list representation naturally distinguishes between:

- a **character node** such as `"4"` (a normal named node), and
- a **single-element list** `( "4" cons nil )` (a different node: a cons cell)

Whether you interpret `"4"` as a digit, or `( "4" cons nil )` as the number four, is entirely up to your rule system (e.g. [stdlib/decimal-arithmetic.zph](https://github.com/acrion/zelph/blob/main/stdlib/decimal-arithmetic.zph)) and any external naming/mapping you choose to apply. For a detailed exploration of how rules can define arithmetic over these structures, see [Semantic Math](logic.md#semantic-math-computation-as-graph-rewriting). For convenient input, the parser additionally supports `&`-prefixed number literals (e.g. `&42`), which delegate to the redefinable function `zelph/number` — so you can always type decimal even when the loaded arithmetic script uses a different internal base. See [Number Literals](logic.md#number-literals). The inverse direction — displaying digit lists as decimal &-literals — works through the same opt-in mechanism: scripts register their digit alphabet via zelph/set-number-digits.

### The Focus Operator `*`

When defining complex structures, you often need to refer to a specific part of an expression rather than the resulting fact node. The `*` operator allows you to "focus" or "dereference" a specific element to be returned.

- `(A B C)` creates the fact and returns the relation node.
- `(*A B C)` creates the fact and returns node `A`.
- `(*{...} ~ conjunction)` creates the fact that the set is a conjunction, but **returns the set node itself**.

This operator is crucial for the rule syntax.

**Example — typing a node via a nested fact:**

The focus operator lets you create a fact and use its subject in an outer statement in a single expression:

```
zelph> (*tim ~ human) ~ male
  tim    ~   male
zelph> tim _predicate _object
Answer: tim ~ male
Answer:   tim    ~   human
```

The inner expression `(*tim ~ human)` creates the fact `tim ~ human` and — thanks to the `*` prefix — returns the node `tim` rather than the statement node. That returned node becomes the subject of the outer `~ male` relation, so `tim ~ male` is created as well.

Querying `tim _predicate _object` (where leading underscores indicate variables, equivalent to using single uppercase letters) confirms that both facts are in the graph.

### The Self-Fact Prefix `:`

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

In the predicate position, it also carries a
[path marker](logic.md#transitive-path-conditions): `:pred⁺ X` is `(X pred⁺ X)`
and inquires whether `X` reaches itself, which is the manner in which a cycle
is detected.

> The term _self-fact_ is zelph's own: graph-theoretically a loop, relationally
> a point on the diagonal of the predicate — the classic way to encode unary
> predicates over a binary-relation substrate. For the logic-side perspective,
> and for why zelph does not assign operators a fixed arity instead, see
> [Unary Predicates and Self-Facts](logic.md#unary-predicates-and-self-facts).

```
zelph> .deductions off
Deduction printing mode: off
zelph> .import decimal-arithmetic
zelph> .import primes
zelph> :testprime &13
 (skipped 328 deductions)
zelph> (:testprime &13) = X
Answer: (:testprime &13) = prime
zelph> (:isprime N, N hasdivisor D) => !
((:isprime N), (N hasdivisor D)) => !
```

`.deductions off` is what keeps this transcript short: the primality test is a
computation, and the 328 derivations it takes are printed line by line in the
default mode (see [Deduction Output Modes](rules.md#deduction-output-modes)).

A lone `:pred` on a line is an incomplete statement; the operand may follow on
the next line, so multi-line input works as usual. A variable token as
predicate keeps variable semantics: the rule pattern `(:R X)` matches any
self-fact, whatever its predicate.

**Display.** The formatter uses the same shorthand in the other direction:
any fact whose subject and object coincide is printed as `:pred subject`.
Because all terms are hash-consed, this includes facts that merely _happen_ to
be self-facts, such as `(&1 + &1)` — printed `:+ &1`. A manual expansion of
such output back to the verbose `S P S` form parses to the identical node.
The sugar form is only used where it round-trips through the parser, and
that is a property of the predicate's NAME: it needs a single token free of
reserved characters (`*`, `<`, `>`, `,`, quotes, `¬`), and it has to print
without quotes, since the sugar has no place to put them. So a predicate
named `&12` or `≈net` — which the parser reads as a number literal and as a
neural condition — keeps the verbose form, as do `(&9 * &9)` and
`x "is opposite of" x`.

Modules can additionally exclude predicates from the display sugar via
[`zelph/no-selffact-sugar`](janet.md#redefinable-hooks): the arithmetic and EML modules register their
term-forming operators, so a hash-consed coincidence like `(&1 + &1)` or
`(&1 eml &1)` prints verbose, while request markers (`:testprime`,
`:simplify`) keep the compact form. The registration only affects display;
`:+ &1` remains valid input.

The colon is **not** a reserved character: atoms with an _inner_ colon (URLs,
`wd:Q5`-style names) are unaffected; only a leading colon in value position
triggers the sugar. A node whose NAME begins with a colon is therefore
printed quoted (`":foo" rel x`), because bare it would open the sugar
instead — as a subject that is an arity error, and inside a multi-object
fact the sugar would swallow the next object without a word. The colon of
the sugar itself is not part of the name and is emitted beside it, which is
also what lets `.run-export` report the predicate as the node it is.

## Creating a node graph

You can generate a node graph yourself using zelph's `.mermaid` command, which outputs a Mermaid HTML format file. For example:

```
.mermaid name 3
```

In this example, `name` refers to the node identifier (in the currently active language specified via the `.lang` command) whose connections you want to visualise. The following number represents the depth of connections to include in the graph (default is 3).

To view the Mermaid graph, open the generated HTML file in a web browser.
