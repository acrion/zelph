## Scripting with Janet

zelph embeds [Janet](https://janet-lang.org), a lightweight functional programming language, as its scripting layer. Janet serves as the programmatic backbone behind zelph's syntax: every zelph statement is parsed into a Janet expression before execution. This integration enables users to go beyond zelph's declarative syntax and use loops, conditionals, macros, and data structures to generate facts, rules, and queries programmatically.

Importantly, Janet operates exclusively at _input time_ — it generates graph structures that are then processed by zelph's reasoning engine. During inference, only zelph's native engine runs. Think of Janet as a powerful macro system: it constructs the graph, then steps aside.

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

This is convenient for longer scripts with multiple definitions and function calls. When closing a Janet block, zelph automatically triggers the reasoning engine (if [auto-run](quickstart.md#full-command-reference) is enabled), so any rules created in the block take effect immediately.

#### Comments and Commands

Lines starting with `#` (comments) and `.` (commands like `.lang`, `.run`, `.save`) work identically in both modes. They are never interpreted as Janet or zelph statements.

### The zelph API for Janet

zelph registers a set of functions in the Janet environment that mirror zelph's syntactic constructs. These functions operate directly on the semantic network, creating nodes, facts, lists, and sets.

#### Nodes and Names: `zelph/resolve`

Every named entity in zelph's graph is a _node_. The function `zelph/resolve` takes a string and returns the corresponding node in the current language (as set by `.lang`), creating it if it does not yet exist:

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

**Important:** `zelph/query` is designed for pattern matching with variables. To check whether a specific fact exists, filter the returned bindings directly rather than calling `zelph/query` with a fully concrete pattern. Note that `zelph/fact` always _creates_ a fact as a side effect — passing concrete nodes to `zelph/fact` inside a filter would unintentionally add facts to the graph.

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

In zelph syntax, the [focus operator `*`](index.md#the-focus-operator) controls what a parenthesized expression returns. For example, `(*{...} ~ conjunction)` creates the conjunction fact but returns the **set node** itself, which is then used as the subject of `=>`. In Janet, this is achieved naturally using `let` bindings:

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

The `let` binding stores the set node in `condition`, then uses it in two separate facts — once to mark it as a conjunction, and once to connect it to the consequence via `=>`. This mirrors exactly what the `*` operator does in zelph syntax. The reasoning engine is triggered automatically when the Janet block closes (via [auto-run](quickstart.md#full-command-reference)).

#### Lists: `zelph/list` and `zelph/list-chars`

zelph has two list syntaxes, each with a Janet counterpart:

**Node lists** (`< a b c >` in zelph) create an ordered list of existing nodes:

```
%(zelph/list "Berlin" "Paris" "London")
```

Equivalent to:

```
< Berlin Paris London >
```

**Compact lists** (`<abc>` in zelph) split a string into individual characters, resolve each to a named node, and build a cons-list from them:

```
%(zelph/list-chars "42")
```

Equivalent to:

```
<42>
```

This is the foundation of zelph's [Semantic Math](logic.md#semantic-math-computation-as-graph-rewriting) system, where numbers are topological structures within the graph.

#### Sets: `zelph/set`

`zelph/set` creates an unordered set of nodes, returning the set's super-node:

```
%(zelph/set "red" "green" "blue")
```

Equivalent to:

```
{ red green blue }
```

#### Janet API Reference (zelph/\*)

The embedded Janet environment exposes the following functions. Unless stated otherwise, functions accept either strings (resolved as node names in the current `.lang`) or `zelph/node` values.

##### Graph construction (mutating)

- **`(zelph/resolve name)`**  
  Resolve (and create if needed) the node named `name` in the current language.

- **`(zelph/fact s p o & more-objects)`**  
  Create a fact node for `s p o...` and return the statement node.

- **`(zelph/set nodes...)`**  
  Create a set super-node from the given elements and return it.

- **`(zelph/list nodes...)`**  
  Create a cons list from existing nodes; the **first argument becomes the outermost cons cell**.

- **`(zelph/list-chars str)`**  
  Create a cons list from the characters of `str`. Characters are reversed before cons construction, matching the `<...>` compact list syntax.

- **`(zelph/negate pattern)`**  
  Mark a fact pattern as a negation condition and return the **pattern node** (equivalent to `*(pattern) ~ negation` in zelph syntax). In zelph syntax, this is also what `¬(pattern)` desugars to.

- **`(zelph/rule conditions & consequences)`**  
  Convenience constructor for rules.  
  `conditions` must be a non-empty array/tuple of fact (pattern) nodes; `consequences` are one or more fact nodes.  
  Returns the conjunction set node.

##### Querying (read-only)

- **`(zelph/query pattern-node)`**  
  Execute a query and return an array of tables, mapping variable symbols (e.g. `'X`) to bound `zelph/node` values.  
  The argument is typically the return value of `(zelph/fact 'X ... 'Y)`.

- **`(zelph/exists s p o & more-objects)`**  
  Check whether a fact exists **without creating** nodes/facts. Returns boolean.

- **`(zelph/name node &opt lang)`**  
  Return the node’s name as a string (or `nil` if unnamed). Optional `lang` selects the naming language.

- **`(zelph/sources predicate target)`**  
  Return all subjects `S` such that `S predicate target` exists (read-only traversal).

- **`(zelph/targets subject predicate)`**  
  Return all objects `O` such that `subject predicate O` exists (read-only traversal).

##### Cons cell inspection (read-only)

- **`(zelph/car cell)`**  
  Return the car (first element) of a cons cell, or `nil` if `cell` is not a cons cell.

- **`(zelph/cdr cell)`**  
  Return the cdr (rest of list) of a cons cell. Returns the `nil` list terminator node for the last cell; returns `nil` if `cell` is not a cons cell.

### Referencing Janet Variables in zelph: Unquote `,`

The `,` (comma) operator bridges the two languages in the opposite direction: it allows zelph statements to reference values defined in Janet. Prefix any Janet variable name with `,` inside zelph syntax:

```
%(def my-city (zelph/resolve "Berlin"))
%(def my-relation "is capital of")

,my-city ,my-relation Germany
```

This is equivalent to writing `Berlin "is capital of" Germany`, but the subject and predicate come from Janet variables.

> Important: unquoting is written as `,name` **without whitespace**.  
> A comma that is followed by whitespace (or `)`) is interpreted as a **conjunction separator** inside `(cond1, cond2, ...)`.
>
> - `,pred` → unquote the Janet variable `pred`
> - `, pred` → **not** unquote; inside conjunction parentheses it acts as a separator, and outside it is simply invalid syntax

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

For Wikidata work where IDs are language-independent, both forms are equivalent. For multilingual scenarios, `zelph/resolve` gives you explicit control over _when_ the name is resolved.

#### Unquote in Complex Structures

The `,` operator works anywhere a value is expected — in facts, sets, lists, and nested expressions:

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
(X "parent taxon" Y, Y "parent taxon" Z) => (X "parent taxon" Z)
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

**Common patterns with sets and lists:**

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
%(def list-42 (zelph/list-chars "42"))
%(zelph/car list-42)                    # → <zelph/node> for "4"
%(zelph/cdr list-42)                    # → <zelph/node> for the sublist <2>
%(zelph/car (zelph/cdr list-42))        # → <zelph/node> for "2"
%(zelph/cdr (zelph/cdr list-42))        # → <zelph/node> for nil
``​`

**Important:** `zelph/sources` and `zelph/targets` do _not_ work for decomposing cons cells, because cons cells are relation nodes (fact nodes) in the graph, not entities that appear as subjects or objects in higher-level facts. Use `zelph/car` and `zelph/cdr` instead.

##### Practical Example: Inspecting a List

Combining `zelph/car` and `zelph/cdr` to walk a cons-list:

```
zelph> <42>
< 4   2 >
%
(def list-42 (zelph/list-chars "42"))
(def nil-node (zelph/resolve "nil"))

# Walk the cons-list using car/cdr
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

**Example — Negation (finding the last element of a list):**

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
  (zelph/fact (zelph/list-chars (string i)) ".." (zelph/list-chars (string (+ i 1)))))

# Mark all single digits
(for i 0 10
  (zelph/fact (zelph/list-chars (string i)) "~" "digit"))

# Single-digit addition lookup table (100 entries)
(for a 0 10
  (for b 0 10
    (let [sum (+ a b)
          d (% sum 10)
          c (math/floor (/ sum 10))
          addition-fact (zelph/fact (zelph/list-chars (string a)) "+" (zelph/list-chars (string b)))]
      (zelph/fact addition-fact "digit-sum" (zelph/list-chars (string d)))
      (zelph/fact addition-fact "digit-carry" (zelph/list-chars (string c))))))

# Rule: find the last element of any cons-list
# Base case: the car of a cell ending in nil
(let [base-cell (zelph/fact 'A "cons" "nil")]
  (zelph/rule [base-cell]
    (zelph/fact 'A "is last of" base-cell)))

# Recursive case: propagate through the cons chain
(zelph/rule
  [(zelph/fact 'B "is last of" '_Rest)
   (zelph/fact 'A "cons" '_Rest)]
  (zelph/fact 'B "is last of" (zelph/fact 'A "cons" '_Rest)))

# The first element of a cons-list is trivially the car of the outermost cell,
# accessible via (zelph/car list-node) — no inference rule needed.
%
```

The rules above are general-purpose (they work on any list, not just numbers). The lookup table encodes digit arithmetic as graph facts. From here, additional rules can process multi-digit numbers by walking lists from right to left, extracting digits, applying the lookup table, handling carries, and constructing new result lists using fresh variables — all within zelph's reasoning engine.

### Summary: zelph Syntax and Janet Equivalents

| zelph Syntax                    | Janet Equivalent                                    | Description                                                                 |
| :------------------------------ | :-------------------------------------------------- | :-------------------------------------------------------------------------- |
| `Berlin`                        | `(zelph/resolve "Berlin")`                          | Resolve a name to a node                                                    |
| `X`, `_Var`                     | `'X`, `'_Var`                                       | Variable (single uppercase letter or `_`-prefixed)                          |
| `sun is yellow`                 | `(zelph/fact "sun" "is" "yellow")`                  | Create a fact (triple)                                                      |
| `(sun is yellow)`               | `(zelph/fact "sun" "is" "yellow")`                  | Nested fact (returns relation node)                                         |
| `{ red green blue }`            | `(zelph/set "red" "green" "blue")`                  | Unordered set                                                               |
| `< Berlin Paris >`              | `(zelph/list "Berlin" "Paris")`                     | Ordered cons-list (first element is the head/outermost cons cell)           |
| `<abc>`                         | `(zelph/list-chars "abc")`                          | Compact char cons-list (LSB-first: rightmost char = outermost)              |
| `*expr`                         | `let` binding to capture and reuse a sub-expression | Focus operator                                                              |
| `,var` in zelph                 | Direct variable reference in generated code         | Unquote a Janet value (no whitespace after comma)                           |
| `% code`                        | —                                                   | Execute Janet inline                                                        |
| `%` (bare)                      | —                                                   | Toggle Janet block mode                                                     |
| `X ~ human`                     | `(zelph/query (zelph/fact 'X "~" "human"))`         | Query — returns array of `@{symbol node}` tables                            |
| _(no equivalent)_               | `(zelph/exists "sun" "is" "yellow")`                | Check if a fact exists (read-only)                                          |
| _(no equivalent)_               | `(zelph/name node)`                                 | Get the name of a node as a string                                          |
| `A ~ city`                      | `(zelph/sources "~" "city")`                        | Find all subjects for a predicate–object pair                               |
| `Berlin "is located in" L`      | `(zelph/targets "Berlin" "is located in")`          | Find all objects for a subject–predicate pair                               |
| `Berlin R city`                 | _(no equivalent)_                                   | Find all predicates that connect a given Berlin and city                    |
| `S P O`                         | _(no equivalent)_                                   | List all facts in the network (use with caution on large databases)         |
| `(*(P) ~ negation)`             | `(zelph/negate (zelph/fact ...))`                   | Mark a pattern as negation condition (evaluates to the pattern node)        |
| `¬(P)`                          | `(zelph/negate P)`                                  | Negation sugar for patterns (evaluates to the pattern node)                 |
| `(*{...} ~ conjunction) => ...` | `(zelph/rule [conditions] consequences...)`         | Create inference rule                                                       |
| `(cond1, cond2, cond3)`         | _(desugars to)_ set + `~ conjunction`               | Conjunction expression (comma sugar), evaluates to the conjunction set node |
| `(cond1, cond2) => cons`        | `(zelph/rule [cond1 cond2] cons)`                   | Rule using a conjunction of conditions                                      |
