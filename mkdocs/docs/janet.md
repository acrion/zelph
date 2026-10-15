# Scripting with Janet

zelph embeds [Janet](https://janet-lang.org), a lightweight functional programming language, as its scripting layer. Janet serves as the programmatic backbone behind zelph's syntax: every zelph statement is parsed into a Janet expression before execution. This integration enables users to go beyond zelph's declarative syntax and use loops, conditionals, macros, and data structures to generate facts, rules, and queries programmatically.

Importantly, Janet operates exclusively at _input time_ — it generates graph structures that are then processed by zelph's reasoning engine. During inference, only zelph's native engine runs. Think of Janet as a powerful macro system: it constructs the graph, then steps aside. It may decide _when_ the engine runs — see [Running the engine](#running-the-engine) — but not how.

## Installing External Packages

zelph ships with an **embedded Janet runtime** — you do not need a separate Janet installation to use Janet scripting in zelph. All built-in Janet functions (including `slurp`, `spit`, `string/split`, and the full standard library) work out of the box.

External Janet packages are only needed for specific functionality such as JSON parsing (`spork/json`) or advanced CSV handling (`spork/csv`). These packages are installed via `jpm` (Janet's package manager), which _does_ require a Janet installation on the host system.

### Checking the embedded Janet version

Use the `.licenses` command to see which Janet version is embedded in your zelph build:

```
zelph> .licenses
...
zelph incorporates the following third-party software:
------------------------------------------------------
Janet (v1.42.1) - MIT License
...
```

Packages installed via `jpm` should match this major version. Packages written exclusively in Janet remain compatible across minor versions. A package containing native code (such as a `.so` or `.dll`, like the one behind `spork/json`) can only be loaded into the exact major and minor version it was compiled for: Janet rejects all others with the message `native needs to be recompiled!`, and reinstalling the package using a Janet version matching the one specified above resolves the issue.

### Installing jpm

`jpm` is typically bundled with the Janet distribution:

- **Arch Linux**: `jpm` is part of the `janet-lang` package (`pacman -S janet-lang`).
- **macOS (Homebrew)**: `brew install janet` includes `jpm`.
- **Windows (Chocolatey / Scoop)**: `jpm` is included with the Janet installation.
- **Other Linux distributions**: If your package does not include `jpm`, see the [Janet documentation](https://janet-lang.org/docs/index.html) for manual installation.

### Setting up the module path

Janet needs to know where to find installed packages. Set the following environment variables (example for Linux/macOS — adapt paths for your system):

```bash
export JANET_TREE="$HOME/.local/jpm_tree"
export JANET_PATH="$JANET_TREE/lib:/usr/lib/janet"
```

Add these to your shell profile (e.g. `~/.bashrc`, `~/.zshrc`) so they persist across sessions. On Windows, set the corresponding environment variables via the system settings.

### Installing packages

To install the `spork` library (which provides JSON, CSV, and other utilities):

```bash
jpm install spork
```

After installation, you may employ its modules within zelph, either in the REPL as demonstrated or within a script:

```
zelph> %(use spork/json)
@{% @{:doc "(% & xs)\n\nReturns the remainder of dividing the first value of xs by each remaining value." :value <function %>} … (4934 bytes, cut)
zelph> %(pp (decode "{\"name\": \"Alice\", \"age\": 30}"))
@{"age" 30 "name" "Alice"}
```

The first line represents the value of `use`, specifically the session’s environment table, which the echo trims according to the method detailed in [Inline Prefix `%`](#inline-prefix).

## Entering Janet Code

There are three ways to write Janet code in zelph:

### Inline Prefix `%`

Prefix a line with `%` to execute it as Janet:

```
%(print "Hello from Janet!")
```

`%` is a **line** escape and nothing else. It is recognized only as the first
character of a line, and it hands zelph the WHOLE line as Janet code — so it
can never stand as a term inside a statement:

```
x knows %(zelph/resolve "Berlin")     # not a statement
```

To use a value computed in Janet as a term, bind it and unquote its name with
`,name` (see [Referencing Janet Variables in zelph](#referencing-janet-variables-in-zelph-unquote) below).
Whitespace after `%` is optional. If the expression spans multiple lines (i.e., has unbalanced delimiters), zelph automatically accumulates subsequent lines until the expression is complete:

```
%(defn make-facts [items relation target]
   (each item items
     (zelph/fact item relation target)))
```

All four lines are collected and executed as a single Janet expression.

### Block Mode `%`

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

The outcome of a `%` line or block is echoed, unless it is `nil`. A struct, a table, a tuple, or an array is rendered as `(pp value)` would display it, all on one line: a dictionary shows its first 30 entries, an array with more than 160 elements shows its first and last three, and a line surpassing 1000 bytes is cut and ends with the total size, formatted as `... (4934 bytes, cut)`. A value whose textual representation exceeds 64 KiB – which a value holding the same part in many places reaches swiftly – is rendered no deeper than the level at which it first exceeds the limit: subsequent levels appear as `...`, and the line ends with `... (more than 65536 bytes, cut)`. `(printf "%m" value)` prints a value without any restriction. Every other value is presented exactly as Janet describes it: a string enclosed in quotation marks, a node as `<zelph/node …>`.

An error triggered by a `%` line or block is reported following its line, as seen in `Error in line "%(error {:a 1})": {:a 1}`: a string exactly as it was raised, any other value as the echo prints it. The same applies to an error raised by a script executed via `zelph/run-script`. A signal other than an error stops the code as well, because nothing resumes it, and the report lists both the signal and its value: `Error in line "%(yield 1)": signal :yield with the value 1, not an error: nothing resumes the code of a line, so it stopped there`.

### Comments and Commands

Lines starting with `#` (comments) and `.` (commands such as `.lang`, `.run`, `.save`) function the same way in both modes. They are never interpreted as Janet or zelph statements. A comment can also end a zelph statement or a command; within Janet code, Janet’s own comment rule remains in effect.

## The zelph API for Janet

zelph registers a set of functions in the Janet environment that mirror zelph's syntactic constructs. These functions operate directly on the semantic network, creating nodes, facts, lists, and sets.

### Nodes and Names: `zelph/resolve`

Every named entity in zelph's graph is a _node_. The function `zelph/resolve` takes a string and returns the corresponding node in the current language (as set by `.lang`), creating it if it does not yet exist:

```
%(def berlin (zelph/resolve "Berlin"))
%(def germany (zelph/resolve "Germany"))
```

The returned value is a `zelph/node` abstract type — an opaque handle to the internal node. This is the Janet equivalent of simply writing `Berlin` in zelph syntax.

**When to use `zelph/resolve`:** Whenever you need to refer to a node by name from Janet code. The node is resolved in the currently active language, which matters when working with Wikidata IDs vs. human-readable names.

### Facts: `zelph/fact`

`zelph/fact` creates a subject–predicate–object triple in the graph and returns the relation node. It accepts three or more arguments (additional arguments serve as extra objects for the same fact: a single relation node whose objects form an unordered set):

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

### Programmatic Query Results: `zelph/query`

When called from Janet, `zelph/query` returns its results as a Janet array of tables rather than printing them. Each table represents one match, mapping variable symbols to their bound `zelph/node` values:

```
%(def results (zelph/query (zelph/fact 'X "is located in" 'Y)))
```

If the graph contains `Berlin "is located in" Germany` and `Paris "is located in" France`, the return value is:

```
@[@{X <zelph/node Berlin> Y <zelph/node Germany>}
  @{X <zelph/node Paris> Y <zelph/node France>}]
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

A query pattern is an ordinary node, so it can be kept in a binding and asked repeatedly — which is the natural shape for a program that builds its patterns once and queries them per unit of data:

```
%(def q (zelph/fact 'A "hits" 'B))
%(each r (zelph/query q) (printf "%s -> %s" (zelph/name (get r 'A)) (zelph/name (get r 'B))))
%(each r (zelph/query q) (printf "%s -> %s" (zelph/name (get r 'A)) (zelph/name (get r 'B))))
```

Both loops print the same matches. The bindings are labelled from the names the variable nodes carry in the graph, so it does not matter whether the pattern was built by the statement that runs the query.

When a query is entered in **zelph syntax** (not via `zelph/query`), results are printed to the console — `zelph/query`'s return-value behavior only applies when called from Janet code.

#### A question that leaves nothing behind

What `zelph/query` is provided remains within the graph: the pattern is part of the program, which can ask it again, as with `q` above, and a conjunction set that the program tagged answers `S ~ conjunction` just like any statement the program wrote. A question that the program asks once and no longer needs leaves no trace when the program builds and answers it within a dedicated cluster, then discards that cluster after the answer. The drop also takes the relation-type declaration that the query’s pattern wrote for a node it first employed as a predicate. The name of that cluster must be one not currently held by any cluster: since the drop operation eliminates all nodes belonging to the cluster it names, using a name the caller already employs would result in the caller’s own nodes being removed as well. `(zelph/clusters)` provides a list of clusters that currently exist, and `unused?` checks this list before the switch, just as `stdlib/sparql.zph` does, which refuses to execute if a cluster bearing its scratch name is present. `(zelph/cluster)` with no argument returns the active cluster, thus the caller’s cluster is reinstated via `(zelph/cluster prev)` instead of being deactivated with `nil`:

```
zelph> john enters card1
zelph> card1 ~ card
zelph> %(defn ask [] (let [s (zelph/set (zelph/fact 'X "enters" 'C) (zelph/fact 'C "~" "card"))] (zelph/fact s "~" "conjunction") (map |(zelph/name (get $ 'X)) (zelph/query s))))
<function ask>
zelph> .cluster mine
Active cluster: mine
zelph> %(def prev (zelph/cluster))
"mine"
zelph> %(defn unused? [name] (not (some (fn [[n _]] (= n name)) (zelph/clusters))))
<function unused?>
zelph> %(unused? "mine")
false
zelph> %(unused? "question")
true
zelph> %(zelph/cluster "question")
"question"
zelph> %(def answer (ask))
@["john"]
zelph> %(zelph/cluster prev)
"mine"
zelph> %(zelph/cluster-drop "question")
8
zelph> S ~ conjunction
```

The drop removes the eight nodes associated with the question – the two conditions, their set, its tag, the two memberships, and the two variables – and `S ~ conjunction` finds nothing. When asked outside a cluster, `(ask)` leaves them, and `S ~ conjunction` answers `{(C ~ card) (X enters C)} ~ conjunction`. A program that might fail between the two invocations restores the cluster and discards it within a `defer`, just as `stdlib/sparql.zph` does for each query.

### Filtering and Transforming Query Results

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

### Using Query Results in Rules and Facts

Query results can feed back into graph construction:

```
%
(def german-cities (zelph/query (zelph/fact 'X "is located in" "Germany")))

(each r german-cities
  (zelph/fact (get r 'X) "~" "German city"))
%
```

### Rules in Janet: The `let` Pattern

In zelph syntax, the [focus operator `*`](concepts.md#the-focus-operator) controls what a parenthesized expression returns. For example, `(*{...} ~ conjunction)` creates the conjunction fact but returns the **set node** itself, which is then used as the subject of `=>`. In Janet, this is achieved naturally using `let` bindings:

```
# zelph syntax:
(*{(X "is capital of" Y) (Y "is located in" Z)} ~ conjunction) => (X "is located in" Z)

# Janet equivalent:
%
(zelph/build-rule
  (fn []
    (let [condition
          (zelph/collection
            (zelph/fact 'X "is capital of" 'Y)
            (zelph/fact 'Y "is located in" 'Z))]
      (zelph/fact condition "~" "conjunction")
      (zelph/fact condition "=>" (zelph/fact 'X "is located in" 'Z)))))
%
```

The `let` binding stores the set node within `condition`, subsequently applying it in two separate facts – first to mark it as a conjunction, and second to connect it to the consequence using `=>`. Within `zelph/build-rule`, this behaviour precisely replicates the action of the `*` operator in zelph syntax. The reasoning engine activates automatically when the Janet block closes (through [auto-run](quickstart.md#full-command-reference)).

**Why `zelph/build-rule`.** zelph writes a parsed rule within a scope of its own, and `zelph/build-rule` gives a manually constructed rule the same scope: whatever the thunk generates inside the rule becomes the rule’s text. Any collection the thunk inserts into the rule becomes the rule’s own collection, just as if it were a literal typed directly into the rule, and any ground fact it constructs within the rule becomes a pattern of the rule, not a claim. A fact the thunk builds that the rule does not hold is treated as a claim once the call succeeds, and any node that existed before the thunk’s execution remains what it was. Without this scope, identical calls would produce data instead. Each `zelph/collection` then becomes a value that the rule only references: every firing of the rule names that exact node in what it derives, and if a member of the collection holds a variable, that variable enters the data unaltered, with every binding of the rule sharing the same node. Each ground fact then becomes a claim, just like any other `zelph/fact` within a Janet block. [`zelph/rule`](#zelphrule) opens the scope by itself; a rule crafted manually using `zelph/fact`, as demonstrated here, requires `zelph/build-rule` surrounding it. Absent this wrapping, the rule remains subject to verification for a consequence predicate that no condition binds, upon the block’s conclusion (refer to [`zelph/rule`](#zelphrule)).

The distinction becomes apparent as soon as a variable is contained within a collection in the consequence. The identical function builds a rule once outside the scope and once within it:

```
zelph> %(defn likes-rule [p] (let [condition (zelph/collection (zelph/fact 'X p 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/collection (zelph/fact 'Z "q" 'Y))))))
<function likes-rule>
zelph> %(likes-rule "p")
<zelph/node (*{(X p Y)} ~ conjunction) => (X likes @{(Z q Y)})>
zelph> %(zelph/build-rule (fn [] (likes-rule "r")))
<zelph/node (*{(X r Y)} ~ conjunction) => (X likes @{(Z q Y)})>
zelph> a p k
(a likes @{(Z q Y)}) ⇐ {(a p k)}
zelph> b p m
(b likes @{(Z q Y)}) ⇐ {(b p m)}
zelph> c r k
(c likes @{(Z q k)}) ⇐ {(c r k)}
zelph> d r m
(d likes @{(Z q m)}) ⇐ {(d r m)}
```

The rule built without the scope names the single collection in both facts, `Y` included, meaning `a` and `b` like the same node. The rule constructed within `zelph/build-rule` builds a collection for each binding, just as the typed rule `(X r Y) => (X likes @{(Z q Y)})` does.

### The Scope of a Variable Symbol: One Block

That `let` is not only a matter of style. **A variable symbol is scoped to one evaluation of a Janet block**, exactly as a variable in zelph syntax is quantified by the statement it appears in. Two blocks that both write `'B` mean two *different* variables, so conditions built in separate blocks are not joined by their shared symbol — they are multiplied:

```
zelph> %(zelph/fact "Berlin" "is located in" "Germany")
<zelph/node Berlin "is located in" Germany>
zelph> %(zelph/fact "Lyon" "is located in" "France")
<zelph/node Lyon "is located in" France>
zelph> %(zelph/fact "Germany" "is member of" "EU")
<zelph/node Germany "is member of" EU>
zelph> %(def joined (let [s (zelph/set (zelph/fact 'A "is located in" 'B) (zelph/fact 'B "is member of" 'K))] (zelph/fact s "~" "conjunction") s))
<zelph/node {(B "is member of" K) (A "is located in" B)}>
zelph> %(length (zelph/query joined))
1
zelph> %(def c1 (zelph/fact 'A "is located in" 'B))
<zelph/node A "is located in" B>
zelph> %(def c2 (zelph/fact 'B "is member of" 'K))
<zelph/node B "is member of" K>
zelph> %(def crossed (let [s (zelph/set c1 c2)] (zelph/fact s "~" "conjunction") s))
<zelph/node {(B "is member of" K) (A "is located in" B)}>
zelph> %(length (zelph/query crossed))
2
```

Both conjunctions print identically — the variable nodes carry the same display names — and both answer without a warning. Only the counts differ: the first joins on `B` and reports the one match (Berlin / Germany / EU), the second reports every combination of the two conditions.

The expense of getting this wrong increases alongside the graph, not with the program. Two conditions each containing 400 facts already generate 160,000 rows rather than just a few, and at the scale of Wikidata, the cross product consumes all available memory before completion. **Construct all conditions belonging to one pattern within a single block**, a structure naturally supported by the `let` form shown above. A pattern *node* can of course be stored and queried later – refer to [Programmatic Query Results](#programmatic-query-results-zelphquery); the symbol, not the node, loses its meaning once the block ends.

Inside a block, a new scope starts at a `zelph/query` and at a `zelph/import` that reads a file, because every line within that file constitutes an independent block: a symbol written after either invocation names a fresh variable. `zelph/run` and `zelph/run-script` keep the existing scope.

When one block is not where the program wants to build them — a pattern assembled step by step, or by several functions — use `zelph/var`, which returns the variable as an ordinary node. Then the caller's own binding decides how far it reaches:

```
zelph> %(def B (zelph/var "B"))
<zelph/node B>
zelph> %(def c1 (zelph/fact (zelph/var "A") "is located in" B))
<zelph/node A "is located in" B>
zelph> %(def c2 (zelph/fact B "is member of" (zelph/var "K")))
<zelph/node B "is member of" K>
zelph> %(def joined (let [s (zelph/set c1 c2)] (zelph/fact s "~" "conjunction") s))
<zelph/node {(B "is member of" K) (A "is located in" B)}>
zelph> %(length (zelph/query joined))
1
```

Every call to `zelph/var` makes a *new* variable, whatever name it is given — otherwise a program building many patterns in one loop would join them all by accident. The name is display only; it is what lets the binding be read back as `(get r 'B)`.

### Lists: `zelph/list` and `zelph/list-chars`

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

The characters serve as nodes bearing that name within the `zelph` language, regardless of what `.lang` specifies, meaning `<42>` denotes the identical list across all languages; a node list resolves its names using the current language, just as every other name does.

This is the foundation of zelph's [Semantic Math](logic.md#semantic-math-computation-as-graph-rewriting) system, where numbers are topological structures within the graph.

### Sets and collections: `zelph/set`, `zelph/collection`

Both create an unordered grouping and return its super-node. They differ in
identity, exactly as the two literals do (see
[Set Constants and Collections](concepts.md#braces-set-constants-and-collections)):
`zelph/set` hashes its members, so the same elements always yield the same node
and membership cannot be extended; `zelph/collection` returns a fresh container
that `(member in container)` adds to.

```
%(zelph/set "red" "green" "blue")
```

Equivalent to:

```
{ red green blue }
```

```
%(zelph/collection "red" "green" "blue")
```

Equivalent to:

```
@{ red green blue }
```

A rule's conjunction of conditions is a collection, not a set constant: its
members are condition patterns, and the rule needs a container of its own.

`zelph/set` applied to an element containing a variable builds a collection, as the `{...}` literal does: wherever a pattern holds it, whether within the conditions of a rule or in a pattern given to `zelph/query`, it matches a set constant by its members (refer to [A literal with a variable in a pattern](concepts.md#a-literal-with-a-variable-in-a-pattern)). After a `.save` and a `.load`, it persists as a set pattern, because its node id says what it is: a statement over it holds its variables and answers no question as data, regardless of whether the program wrote that statement using `zelph/fact` or typed it over variables that the program binds.

A collection is a rule's own exactly when it was written with the rule: entered directly in the rule's text, constructed within the argument forms of [`zelph/rule`](#zelphrule) or within `zelph/build-rule`, or built by a rule generator for the rule it produces. No subsequent writing alters this status. A thread initiated by the program is unable to form a collection in any way: just as every write, `zelph/collection` is restricted to the main thread only (refer to [Threads](#threads)). A collection constructed by the program before the rule, or passed to `zelph/rule*`, also counts as a value: the rule refers to it, and any data the rule writes into it is stored in that node. In contrast, data a rule writes into a collection of its own is placed in the collection's term (refer to [Collections](concepts.md#collections)), meaning a program seeking to read what the rule accumulates must either build the collection before the rule or retrieve it via a query like `S in O`.

Once a rule has been formulated, its text becomes immutable. A program that keeps the node of a collection it constructed within `zelph/build-rule` and subsequently attempts to insert a member into it – via `zelph/fact`, or as the consequence of a rule created with `zelph/rule*` – is rejected, issuing an error that names the collection, and no data is recorded; similarly, a member that the collection already contains, which `zelph/fact` would otherwise claim as data, is also disallowed, and the same restriction applies to a rule's condition set. Within the argument forms of `zelph/rule` or the thunk of `zelph/build-rule`, writing into a collection is the mechanism by which a rule's text is written.

### Janet API Reference (zelph/\*)

The embedded Janet environment exposes the following functions. Unless stated otherwise, functions accept either strings (resolved as node names in the current `.lang`) or `zelph/node` values. A string names an atom, not a rule’s variable: within a function that performs reads without generating, `"X"` refers to nothing, even in contexts where a rule’s variable displays that name, such as in `zelph/lookup`; a variable is accessed via its node.

#### Threads

A program can initiate threads independently using `ev/thread`, `ev/spawn-thread`, or `ev/do-thread`, each hosting its own Janet VM. While such a thread is permitted to examine the graph and utilize a compiled network, any action that writes the graph, builds a rule, or changes the engine’s state must occur exclusively on the main thread – the one responsible for running the REPL, scripts, and statements. Attempting these operations from any thread other than the main one triggers an error that names the function and says "main thread only"; every entry below that describes such a function includes the sentence "Main thread only." The state underlying a statement, rule build, and the engine’s scratch clusters is singular per process, meaning that concurrent writes from a second thread would disrupt the main thread’s ongoing operations. Instead, a thread transmits its computed results to the main thread via a channel made with `ev/thread-chan`, and the main thread performs the write. `zelph/resolve` performs both reading and writing operations: if invoked from a thread other than the main thread, it only looks up a name, and any name lacking a corresponding node is refused with the message "zelph/resolve: '<name>' has no node, and creating one is main thread only". For details on compiled networks, refer to the threading note under [Neural network functions](#neural-network-functions). A thread that names a node via a string, or omits the language when a function takes one, reads the language set last by the main thread, and a single invocation resolves all its strings in that language, even as the main thread switches `.lang`.

#### Graph construction (mutating)

- **`(zelph/resolve name)`**  
  Resolve (and create if needed) the node named `name` in the current language. On any thread distinct from the main thread, this operation solely performs a lookup of the name, and a name lacking an associated node is rejected in that context.

- **`(zelph/var &opt name)`**  
  Create a **fresh** variable node and return it. Every call yields a new variable, whatever it is named — the name is display only, and is what makes the binding readable as `(get r 'name)`; an unnamed variable still matches but contributes no column. Use it when the conditions of one pattern are built in separate blocks, where a variable *symbol* would mean a different variable each time: see [The Scope of a Variable Symbol](#the-scope-of-a-variable-symbol-one-block). Main thread only.

- **`(zelph/fact s p o & more-objects)`**  
  Create a fact node from `s p o...` and return the corresponding statement node. A membership (`p` being `in`) within a collection of a rule's own text or within a rule's condition set is disallowed when not within the process of writing a rule: a rule's text becomes immutable once the rule is written. A refuted marking of a ground fact, `(zelph/fact F "~" "refuted")`, is rejected beyond the scope of a rule build, and the rejection names `zelph/refute`, which writes that claim; a marking whose fact holds a variable is classified as a pattern and continues to be permitted, enabling a program to request refutations through queries. Main thread only.

- **`(zelph/refute s p o & more-objects)`**  
  Claim that the fact `s p o...` does not hold, and return its associated node – what `¬(s p o)` and `(s p o) ~ refuted` mean on a line of their own. The node corresponds to what `zelph/fact` would generate, featuring a probability of 0 on the edge leading to its predicate, and the marking `(fact ~ refuted)` is placed adjacent to it. This action is denied if the graph claims the fact ("this fact is known to be true"); a fact that a rule or query merely specifies as a pattern is not a claim and thus is refuted. What the call claims is the marking rather than the fact: a fact that a rule only wrote down remains a pattern, whereas a fact generated by the call is written as a mention, ensuring it does not respond to any query after the marking is removed (see [Negation as Failure](logic.md#negation-as-failure)). Attempting to refute a fact that is already refuted has no effect. Main thread only.

- **`(zelph/mention s p o & more-objects)`**, **`(zelph/mention thunk)`**  
  Write a statement without claiming it, and yield its node: the first form constructs a single fact, the second runs `thunk`, which constructs the statement and yields it. What the invocation creates and what the returned statement reaches is a mention – marked as written down, not asserted, precisely like a rule’s ground pattern – thus `zelph/exists`, queries, and rule conditions do not take it as data until a claim of the same statement makes it data. What existed before keeps its status: a mention of an asserted statement preserves its assertion, and one of a refuted statement retains its refutation. A rule that the call returns – a `=>` fact it creates as the statement itself – functions as a mention: it is not in force, `.list-rules` excludes it from its output, and a [rule over rules](logic.md#mentioning-a-rule-is-not-asserting-it) does not read it, until a claim of the identical rule puts it in force – a typed rule, `zelph/fact` inside a Janet block, `zelph_fact`, `zelph/dedup-rule`, or `zelph/build-rule`. A claim via `zelph/fact` or `zelph_fact` is checked for a consequence predicate that no condition binds, as a `=>` fact produced by the call is (refer to [`zelph/rule`](#zelphrule)), and if the claim is denied, the rule stays a mention. A rule that was present before the call stays in force. The thunk runs as `zelph/rule-text`’s thunk does, within a scratch cluster and without claiming anything, even within a Janet block, yet outside the template scope: a rule it writes via `zelph/rule-text` is rule text, a collection it creates is a value, and a refutation marking it writes does not refute anything until it is claimed. In a parsed statement, the marking waits until the statement concludes, and a statement that asks marks nothing. A thunk returning no node is refused, and nothing it constructed is retained. The parser wraps every argument of an [opaque predicate](logic.md#mentioning-a-statement-opaque-predicates) that builds something in `zelph/mention`. A prune of a claimed statement that a mention holds withdraws the claim while preserving the statement as part of the mention, just as a rule’s pattern remains intact. Main thread only.

- **`(zelph/prune-fact fact)`**, **`(zelph/prune-fact s p o & more-objects)`**  
  Withdraw the claim of a single fact, mirroring how the ground form of `.prune-facts` withdraws it, and report the outcome. The fact is resolved without generating any new entity and is accepted precisely as provided, thus `(zelph/prune-fact "a" "p" "b")` leaves `a p b c` alone. The answer is `:removed` if the node was eliminated, along with the statements that nest it, which `zelph/holders` identifies in advance; `:unclaimed` if the node stays because a rule or a mention holds it, while the statements that hold it outside the rule’s text are discarded; and `nil` if nothing claims the fact: it is missing, merely a rule’s pattern or a mention, or refuted. The relation-type declaration of a core predicate, like `~ ~ ->`, belongs to the engine and is retained: the answer is `nil`, and the function prints the same line that `.prune-facts` prints for it. Anything derived from the fact remains, as zelph does not track derivation; `zelph/dependents` enumerates it ahead of time. A removed node loses its annotations, whereas a node that endures retains them. A removal constitutes no creation, so dropping the active cluster does not bring the fact back. A node that is not a fact is rejected, as is a fact containing a variable, since removing what a pattern matches falls under the business of `.prune-facts`. This operation is blocked when the graph is only partially loaded, as `.prune-facts` is. Rejected during rule construction. Main thread only.

- **`(zelph/set nodes...)`**  
  Create a SET CONSTANT from the given elements and return its super-node.
  Identified by its members, so the same elements always yield the same node
  and membership cannot be extended. An element that is a variable makes the
  members unknown, and the call then behaves as `zelph/collection`. Main thread only.

- **`(zelph/collection nodes...)`**  
  Create a COLLECTION from the given elements and return its super-node. A
  container with its own identity: two calls with the same elements yield two
  different nodes, and `(member in container)` adds to it. This is what a
  rule's conjunction of conditions is. When there are no elements present, the invocation yields `nil`, which serves as the core node of an empty list, just as `zelph/set` and `zelph/list` do, and does not generate a new container: two such invocations produce the identical node, and `@{}` within a statement evaluates to `nil`. Main thread only.

- **`(zelph/conjunction conditions...)`**  
  Create a rule’s condition set from two or more conditions and return it: a collection tagged `~ conjunction`, representing the comma list `(cond, cond, ...)`. Each condition must qualify as a fact pattern or a nested condition set; any other input is rejected, among it the node that a focus makes a condition evaluate to, since a rule holding it could never fire. Formed by a statement located outside a rule build, the set is owned by the statement just as a collection literal is, meaning a ground comma list on a standalone line asserts its members. Main thread only.

- **`(zelph/list nodes...)`**  
  Create a cons list from existing nodes; the **first argument becomes the outermost cons cell**. Main thread only.

- **`(zelph/list-chars str)`**  
  Create a cons list from the characters of `str`. Characters are reversed before cons construction, matching the `<...>` compact list syntax. Every character serves as a node bearing that name within the `zelph` language, regardless of the setting specified by `.lang`. Main thread only.

- **`(zelph/negate pattern)`**  
  Generate the negated condition over a fact pattern and return it: the fact `((pattern) ~ negation)`, which a rule takes as its condition instead of the original pattern, while `pattern` remains an ordinary positive pattern. In zelph syntax, `¬(pattern)` expands into this form. A node that is not a statement, or a node that is already a negation, is refused. Main thread only.

- **`(zelph/path pattern mode)`**  
  Tag a one-step fact pattern as a transitive path condition and return the tag fact `(pattern closure mode)`, which serves as the condition that a rule processes. `mode` must be `"one-or-more"` when representing `P⁺` and `"zero-or-more"` when representing `P∗`; any other mode is refused. In zelph syntax, `S P⁺ O` is expanded into this structure (refer to [Transitive Path Conditions](logic.md#transitive-path-conditions)). Main thread only.

- **`(zelph/path-guard from to)`**  
  Emitted by the parser prior to a path condition, `S P⁺ O` or `S P∗ O`, on a line that makes an assertion, before the pattern is built. It refuses the line when both ends are concrete: reachability is what the engine walks, not what a statement asserts, and building the pattern would assert its single step. A Janet symbol, or a node containing a variable, serves as an open endpoint, whereas a string represents a name. Within a rule build or inside a Janet block, it has no effect. Not needed in Janet code authored manually. Main thread only.

- **`(zelph/rule conditions & consequences)`**  
  A macro serving as a convenient way to construct rules.  
  `conditions` must be a non-empty array or tuple composed of fact (pattern) nodes; `consequences` consist of one or more fact nodes. The consequences serve as the objects of one `=>` fact, much like the typed `(A) => (B) (C)`: the rule fires once for each binding, meaning a fresh variable named by two consequences refers to the identical node in both instances.  
  Returns the conjunction set node.  
  The argument forms execute inside the rule’s build, the identical build that `zelph/build-rule` assigns to its thunk, meaning the rule is assembled in the same manner as a parsed rule: the content the forms build into the rule – within their dynamic extent, including any helper function they invoke – becomes the rule’s text. Any collection they write into it is the rule’s own, and a ground fact constructed within it functions as a pattern within the rule, not as a claim (refer to [Claimed or Merely Written Down](#claimed-or-merely-written-down)). A fact they build that the rule does not hold, such as one a helper asserts in addition to the fact it returns, constitutes a claim upon successful completion of the call, and any node created before the call remains unchanged. As a macro, `zelph/rule` cannot be used as a value by `apply` or `map`; instead, `zelph/rule*` can be.  
  In contrast to a parsed `... => ...` statement, this does **not** check whether the graph already contains an identical rule, up to variable renaming (see [Rules Say Themselves Only Once](modules.md#rules-say-themselves-only-once)): the decision of what to build rests with the program constructing the rules. If a program generates the same rule multiple times, it should either build it once or proceed through `zelph/import`.  
  `zelph/rule`, `zelph/rule*`, `zelph/build-rule`, `zelph/build-rule-parts`, and `zelph/dedup-rule` are disallowed during the creation of a rule: when appearing within the argument forms of another rule, or inside the thunk of `zelph/build-rule` or `zelph/rule-text`. Invoked in such contexts, `zelph/rule*` would build a rule of its own, in force by itself, and not as part of the outer rule. A rule nested within another is a part of the outer rule and is written there as a `=>` fact, `(zelph/fact condition "=>" consequence)` – which is the way Janet writes a [rule generator](rule-generators.md). A call that does not succeed – due to an error in its argument forms, or an argument that is not a node – results in no trace of the structure its forms attempted to create: neither a fragment of the rule, nor a rule they recorded alongside as a `=>` fact, nor a fact they asserted as a claim. Every builder – `zelph/rule`, `zelph/rule*`, `zelph/build-rule`, and `zelph/dedup-rule` – refuses a rule whose consequence writes a fact with a predicate that no condition binds, exactly as a typed rule is rejected (see [Fresh Variables](logic.md#fresh-variables-generative-rules)). A `=>` fact generated by `zelph/fact` is also examined: one written at the top of a block, one written within a builder’s thunk alongside the rule it returns, and one returned bare by a `zelph/rule-text` thunk. The call itself cannot reject it, since a generator’s inner rule maintains the same shape until the generator holds it, so the validation occurs when the outermost evaluation of Janet code ends – a `%` block, a statement together with its islands, a keyword handler, a `.janet` file loaded via `.import` – and before every run or save started within it. A `=>` fact the call produced that remains a rule in force is then removed, while its components persist, and the rejection becomes the error of the block, following any error the block itself incurred, or the error of the run or save, which then does not proceed. A rule written down by `zelph/mention` is not in force, and a `zelph/fact` call that claims it brings it into force: that call undergoes identical verification, and if rejected, only the claim is undone, leaving the rule as a mention. An inner rule held by the generator and a rule held by a statement are not in force, and thus pass. This is the price of the delayed check: an inner rule that only a later block would hold is refused when its own block ends, so a generator must be written within a single block, or within a single `zelph/build-rule` or `zelph/rule-text` thunk. During the construction of a rule, the following are rejected: `zelph/cluster` (even when invoked without an argument), `zelph/cluster-drop`, `zelph/import`, `zelph/load`, `zelph/save`, `zelph/run`, `zelph/run-once`, `zelph/run-delta`, and `zelph/query`. Each would attempt to act on a rule that remains unfinished, and a query would additionally reset the variable scope that binds the rule’s variables. Main thread only. On a different thread, `zelph/rule` is rejected before execution of its argument forms.

- **`(zelph/rule* conditions & consequences)`**  
  The plain function behind `zelph/rule`, accepting identical parameters and producing the same outcome. Before execution, its arguments are evaluated, so it cannot tell which of them the call built: it marks nothing, a ground fact within them stays a claim, and a collection among them is a value, just as one constructed before a `zelph/rule` invocation is. Before writing anything, it performs checks on every argument, meaning that an argument which is not a node results in no rule being left behind. A ground refutation marking `((F) ~ refuted)` among the conditions qualifies as a refutation if `zelph/fact` builds it outside a rule build, and is rejected there; express it using `zelph/mention`, which records it without claiming it, as shown in `(zelph/rule* [(zelph/mention (fn [] (zelph/fact (zelph/fact "john" "waits" "nil") "~" "refuted")))] (zelph/mention "mary" "sleeps" "nil"))`, and the rule fires once the refutation is claimed. Within the argument forms of `zelph/rule` and the thunk of `zelph/build-rule` or `zelph/dedup-rule`, `zelph/fact` writes such a marking as a condition. Main thread only.

- **`(zelph/build-rule thunk)`**  
  Run `thunk`, which constructs a single rule, as a parsed rule is built, and return what it returns. What the thunk builds into the rule is the rule’s text, and the ground components of the rule it returns – of every `=>` fact over the condition set it returns, or over the condition of the `=>` fact it returns – are patterns, not claims. A duplicate is not undone, and a thunk that does not succeed leaves no trace of its construction. Outside a parsed statement, the `=>` fact it returns is asserted, so a rule that `zelph/mention` wrote with identical text comes into force. `zelph/rule` builds its rule within the same context; a rule manually authored using `zelph/fact` must be enclosed by it (refer to [Rules in Janet: The `let` Pattern](#rules-in-janet-the-let-pattern)). Rejected if another rule is currently under construction, and also rejected when the thunk is awaiting the event loop, as `ev/sleep` does and `ev/take` does even on a channel that holds a value, or when yielding: other fibres would run within this scope meanwhile and write the rule’s text, so nothing of the rule is kept, and thus the value the thunk is waiting for must be determined before rule construction. Main thread only.

- **`(zelph/dedup-rule thunk &opt :focus)`**, **`(zelph/rule-text thunk)`**  
  Emitted by the parser around a typed `... => ...` statement and around a rule that another statement mentions. `zelph/dedup-rule` runs a thunk that constructs a single complete rule and returns its `=>` fact, with each `zelph/var` of the rule created inside the thunk, and returns the rule node: should the graph already contain an equivalent rule, differing only in variable names, the newly created rule is discarded and the existing node is provided instead. This mechanism serves as the parser's route for typed rules and stands as the sole method that eliminates duplicates when a rule is assembled in Janet. Outside a parsed statement, the rule it keeps is asserted, so a rule authored by `zelph/mention` using identical text comes into force; inside one, the statement claims its rule upon completion. Any thunk producing a result other than a `=>` fact is rejected, and none of its constructed elements are retained: this applies to a plain fact, and likewise to a condition set such as `zelph/rule*` returns. Using `:focus`, the thunk returns a tuple consisting of the `=>` fact and one component from the rule – either its condition, the `=>` itself, or one of its consequences – and `zelph/dedup-rule` returns that part of the rule which remains, the original rule’s when the new one is discarded. This is the parser’s form for a rule written with the [focus operator](concepts.md#the-focus-operator), for example `*{(X p Y)} => (X q Y)`, which evaluates to its condition set: the statement asserts the rule and asks nothing, regardless of which part it ultimately evaluates to. `zelph/rule-text` runs its thunk in the same manner as `zelph/build-rule` does, so the ground parts of each rule the thunk creates are patterns belonging to that rule, not claims, regardless of what the thunk returns; a mentioned rule stays unmarked itself, as a rule over rules could treat it as data, unless it is a part of another rule the thunk creates. A rule that `zelph/rule-text` returns within a Janet block, held by no statement, is therefore in force, while the identical rule formulated using `zelph/mention` constitutes a mention and is not. In a parsed statement, the marking waits until the statement concludes and is discarded if the statement is a query. A `=>` that is mentioned and asks about the existence of implications, such as `x says (S => O)`, makes the statement into a query. The same holds when its consequence is a Janet value that refers to no statement, as seen in `x says ((X p Y) => ,k)`, which is known solely after the line executes; once the query is answered, what the line wrote vanishes, just as a query’s text does. A failing thunk retains none of the structures it previously built. A mention authored manually using `zelph/fact` claims its parts unless it is contained within `zelph/rule-text` (see [Claimed or Merely Written Down](#claimed-or-merely-written-down)). Main thread only.

- **`(zelph/query-text thunk)`**, **`(zelph/operand thunk)`**  
  Emitted by the parser around every query, and surrounding each condition operator individually. Neither is required for Janet code authored manually. Inside a parsed statement, `zelph/query-text` runs the query’s code, and whatever the code creates under an operator – the operand, all nested elements, and the operator’s own fact – is marked as a pattern before query evaluation proceeds, while a node that existed prior to this process retains its initial state (see [Conditions in Queries](queries.md#conditions-in-queries)); if the thunk encounters failure, it retains none of the structures it built. Once the query is answered, every construct generated by the code that holds one of the query’s variables is eliminated, as is the relation-type declaration authored by the code for a predicate utilized for the first time; whatever else it created without a variable stays. Outside a parsed statement, such as within a Janet block, it merely executes the thunk. `zelph/operand` runs the code responsible for constructing an operator and its operand: a fact already present in the graph is retrieved rather than asserted, a refuted fact is located instead of being added, and any other case is constructed as `zelph/fact` would construct it, with the determination of whether it qualifies as a pattern made by the rule or the query in context. Main thread only.

- **`(zelph/claim-text thunk)`**  
  Emitted by the parser around a claimed statement that carries a refutation beside what else it writes, that takes a node by reference (a Janet variable `,name`, or the result of an inline keyword), or that writes a component before its own fact (such as a nested statement, a set, a collection, a list, a number, or the members of a comma list), and unnecessary in manually written Janet code. Within a parsed statement, it runs the statement’s code and the claim at the end of the line so that a refused statement keeps nothing: if either fails – a refutation refused for a fact that the graph or the statement itself claims, a rule’s marking pattern of a claimed fact handed in by reference, any other refusal of `fact()` after a component has been written – whatever it created is removed, and the rule patterns and refutations it altered return to their prior state. Outside a parsed statement, it only executes the thunk. Main thread only.

- **`(zelph/aside fact)`**  
  Emitted by the parser around the fact a focus writes beside the part it hands on, and redundant in code authored manually. It returns the fact. Inside a parsed statement, the statement claims the fact just as it claims what it writes, even if a rule previously wrote the fact as its pattern, and a typed rule that duplicates one the graph has keeps it. Within a rule or statement that another statement mentions, or within an argument of an opaque predicate, the fact is marked with what the mention wrote, and a pre-existing fact keeps its status. Not recorded are a fact holding a variable, a rule, and the tag of a comma list or of a condition operator. Outside a parsed statement, it simply returns the fact. Main thread only.

- **`(zelph/build-rule-parts thunk)`**  
  What `zelph/rule` expands to. `thunk` returns the rule’s conditions and consequences as a single tuple, `[conditions ;consequences]`, with the rule being assembled from that tuple during the identical build where the thunk executed, thus the expansion contains no standalone function responsible for rule construction. Denied when a different rule is currently being built. Not required for Janet code written by hand. Main thread only.

#### Querying (read-only)

- **`(zelph/query pattern-node)`**  
  Execute a query and return an array of tables, mapping variable symbols (e.g. `'X`) to bound `zelph/node` values.  
  The argument is typically the return value of `(zelph/fact 'X ... 'Y)`.  
  Disallowed during rule construction: executing a query resets the variable scope, which there binds the rule’s variables. Main thread only.  
  The pattern provided remains within the graph as the program’s own pattern; a question posed once leaves no lasting trace when the program builds and answers it in a cluster that it discards afterwards (see [A question that leaves nothing behind](#a-question-that-leaves-nothing-behind)).

- **`(zelph/exists s p o & more-objects)`**  
  Check whether the fact was **claimed** — asserted or derived — **without creating** nodes/facts. Returns boolean. A statement that occurs only as a rule's own condition or consequence is not claimed; see [Claimed or merely written down](#claimed-or-merely-written-down). The engine’s own bookkeeping does not appear either, just as it does not appear for a ground rule condition: the mark of a rule’s pattern, the marking of a contradiction record, and the memberships of a record that nothing else names (refer to [Contradiction Detection](logic.md#contradiction-detection)). `zelph/sources`, `zelph/targets`, and `.node` list them all: they constitute the structural read surface. A fact carrying **further** objects satisfies the question: `(zelph/exists "a" "p" "b")` is true when the graph says `a p b c`, which is what a rule with that condition matches. A statement among the arguments is interpreted identically, at each level: with `s` representing the node of `a p c`, `(zelph/exists s "bad" "k")` holds true when the graph says `(a p b c) bad k`, and a claimed wider fact answers also where the narrower statement is present solely as a rule’s pattern. When followed by `:exact`, only the fact as explicitly stated is recognized, so `(zelph/exists "a" "p" "b" :exact)` returns false in that case. The exact probe performs a single hash lookup, even if the answer is false, whereas the plain form subsequently matches the question as a ground rule condition is matched, once a fact involving multiple objects may exist (either one was written, two statements were merged, or a network was loaded): on a hub within an extensive graph, this can entail hundreds of thousands of entries. A keyword in the final position is always read as an option, and any keyword other than `:exact` is refused.

- **`(zelph/mentioned s p o & more-objects)`**  
  Check whether the fact **node** is present at all, whether or not anybody claimed it. True for a rule's own patterns, which `zelph/exists` reports as absent. Use it to inspect rule structure; use `zelph/exists` to ask about the data. Takes a trailing `:exact` in the identical manner.

- **`(zelph/rules)`**  
  The rules in force, as enumerated by `.list-rules`, including their parts, without running anything. Returns a tuple sorted by rule node, one struct per rule: `:rule` the rule node, `:conditions` its plain positive conditions, `:negated` the patterns that its negated conditions negate (`P` for the condition `¬(P)`), `:guards` its `!=` conditions, `:paths` its [path conditions](logic.md#transitive-path-conditions) and `:neural` its `≈` conditions – both of these last two expressed as the fact that their condition operator builds, which prints as the condition is typed, `S P279⁺ T` or `≈net(P)`, in every language – and `:heads` its consequences, the core node `!` for a denial. Each part is a tuple of nodes sorted by node. The conditions are organized into these categories precisely as the engine reads them for its [negation levels](#negation-levels), so a program analysing the rules in force needs no reading of its own. A rule that a statement merely mentions is not in force and does not appear in the list. Creates nothing. Main thread only.

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> (X ~ man, ¬(X ~ married)) => (X ~ bachelor)
zelph-> (X ~ man, X ~ woman) => !
zelph-> ((X p Y) => (X q Y)) is nice
zelph-> %(each r (zelph/rules) (zelph/out (string/format "%q" r)))
{:conditions (<zelph/node X ~ man> <zelph/node X ~ woman>) :guards () :heads (<zelph/node !>)
:negated () :neural () :paths () :rule <zelph/node ((X ~ man), (X ~ woman)) => !>}
{:conditions (<zelph/node X ~ man>) :guards () :heads (<zelph/node X ~ bachelor>) :negated
(<zelph/node X ~ married>) :neural () :paths () :rule <zelph/node (¬(X ~ married), (X ~ man))
=> (X ~ bachelor)>}
```

- **`(zelph/name node &opt lang :strict)`**  
  Return the node’s name as a string (or `nil` if unnamed). Optional `lang` selects the naming language. If no name exists in `lang`, the result defaults to a name in a different language (refer to [Node Names](#node-names-zelphname)); appending `:strict` disables this fallback, making `(zelph/name node "de" :strict)` return `nil` when the node has no German name. A core node answers with its spelling across every language.

- **`(zelph/lookup name &opt lang)`**  
  The node that already carries `name` in `lang` (default: the current language), or `nil`. In contrast to `zelph/resolve`, it never creates a new node, which makes it the way to ask whether a term refers to an entity. A variable that carries the name for display is not its node, and a core spelling like `"~"` gives the core node across all languages. Permitted on every thread.

- **`(zelph/lookup-all name &opt lang)`**  
  Each node that carries `name` in `lang`, expressed as a tuple, ordered according to the ranking system determining which node the name ultimately resolves to (refer to [Labels that several entities share](wikidata.md#labels-that-several-entities-share)): a property precedes an item, which precedes any other node type, followed by the smaller Wikidata number, then the smaller node identifier. A name assigned to exactly one node yields that node alone, a name assigned to no node returns an empty tuple, and a variable or a core node is never included in the results. Once `.name` has given a name to a different node, the node returned by `zelph/lookup` does not need to be the first one. While the name sections of the network are only partially loaded – due to a `.load-partial` operation that selected fewer name chunks than the file holds – the answer is the keyword `:incomplete`, in every language, until a new load replaces the graph or `.new` starts a new one. Creates nothing. Allowed on every thread.

- **`(zelph/lang &opt lang)`**  
  The language currently in use, as displayed by `.lang`. When provided with an argument, set the language just as `.lang` does and return the prior value, enabling a program to operate in a distinct language and later reinstate the session’s original language. An empty string is not permitted. Main thread only.

- **`(zelph/sources predicate target)`**  
  Return exactly those nodes S for which the claimed fact S predicate target exists — i.e. nodes in the subject role. target must be in the object role of the matching fact: given a R b and b R c, (zelph/sources R b) returns only a (not c), and (zelph/targets b R) returns only c (not a). A statement ABOUT such a fact is not a subject of it, and a rule's own pattern is not an answer.

- **`(zelph/targets subject predicate)`**  
  Return all objects `O` such that `subject predicate O` exists (read-only traversal).

- **`(zelph/degree node &opt side)`**  
  The number of edges of `node` within the graph, obtained by examining the size of its adjacency structure without copying it, thus ensuring the cost remains independent of the number: `:in` counts the incoming edges, `:out` the outgoing ones, and omitting `side` includes both. A fact references its subject and its predicate, while its subject and objects reference it, meaning `:in` counts the facts where the node serves as subject or predicate, and `:out` those where it functions as subject or object. A predicate additionally acts as the subject of the fact that declares it as a relation. Returns `0` when the node does not exist. Allowed on every thread.

- **`(zelph/closure start predicate &opt include-start)`**  
  Transitive closure following `predicate` forward (subject to object). `include-start` true gives the reflexive closure (the `*` of SPARQL property paths).

- **`(zelph/closure-sources target predicate &opt include-target)`**  
  Transitive closure following `predicate` backward (object to subject). `include-target` true gives the reflexive closure.

- **`(zelph/explain fact)`**, **`(zelph/explain s p o & more-objects)`**  
  The proof that `.explain` outputs, in data form (refer to [Proofs as Data](#proofs-as-data-zelphexplain)), or `nil` if the graph holds no such fact. The fact is resolved without creating anything and accepted precisely as provided, just as `.explain` takes it. Every step of the proof is a struct: `:fact` the fact; `:status` one of `:derived`, `:axiom`, `:unfounded`, `:rule-pattern`, `:mentioned` (the mark of a rule pattern in which a statement wrote the fact down as a mention, `.explain`’s `[mentioned; not asserted]`), `:rule-mentioned`, or `:truncated`; `:rule` the `=>` fact associated with a derived step; `:premises` the steps corresponding to its positive conditions; `:absent` the patterns of its negated conditions, identified as absent; `:walked` the patterns of its path conditions, and `:walk-edges`, for each path condition, the steps of the derived facts traversed during its walk; `:cycle-leaf`; `:more`, true if the root possesses a second justification; and `:bindings`, the rule’s variables as the step binds them. The proof is the complete one if the search stays within the session’s `.explain-budget`; otherwise, it is the proof found within the default depth of `.explain`, truncated at `:truncated` steps, and the root includes `:fallback true`. Main thread only.

- **`(zelph/holders fact)`**, **`(zelph/holders s p o & more-objects)`**  
  The statements that claim the fact by nesting it, represented as a tuple of nodes sorted by node, each appearing exactly once; the empty tuple if nothing holds the fact, and `nil` if the graph holds no such fact. The fact is resolved as `zelph/prune-fact` resolves it. These are the statements that `zelph/prune-fact` takes along with the fact, or withdraws where a holder is itself part of a rule, so a program that must not lose one asks first. Rule text and mentions hold the fact without claiming it and are excluded: a rule that names the fact, a statement that only a rule records, a statement that holds the fact within an [opaque position](logic.md#mentioning-a-statement-opaque-predicates), and one that `zelph/mention` wrote down. Also excluded are the engine’s bookkeeping, a pattern involving a variable, or the fact’s own markings. Only the direct holders are listed: a statement that encloses a holder goes with that holder. A membership of the fact within a container that the prune takes along with it is passed over, and instead the statements naming the container are listed: a set constant, which is its members, and an atom or collection not named by any rule or mention, which loses a member. The prune takes the container along with the membership, the container’s other memberships, and the statements naming the container; the container itself goes too, though it is not a fact and does not appear in the list. A membership within a container named by a rule or mention is listed, since that membership is all that goes. Likewise, a membership situated within such a container and within another that the prune takes along is also listed: it is what the first container loses, and it is listed alongside the statements that name the second. A membership that is itself asked about is passed over in the same fashion. The relation-type declaration of a core predicate, which `zelph/prune-fact` maintains together with whatever holds it, results in the empty tuple. Generates nothing. Refused, and blocked on a graph that is only partially loaded, as `zelph/prune-fact` is. Main thread only.

- **`(zelph/dependents fact)`**, **`(zelph/dependents s p o & more-objects)`**  
  The set of facts that a rule in force produces directly from the given fact in a single step, represented as a tuple of nodes sorted by node, each appearing once: each such fact’s justification relies on the original fact, either as a condition or as an edge that the walk of a [path condition](logic.md#transitive-path-conditions) steps over, provided the rule’s other conditions, guards, and negations are satisfied. An empty tuple results when no fact depends on the given one, and `nil` if the graph holds no such fact; the fact is resolved as `zelph/prune-fact` resolves it. The search is the one `.explain` performs, run one step forward, thus a fact that a rule with a fresh variable made is found as its witness, and a rule that a [generator](rule-generators.md) built is found too. Only immediate dependents are included: a fact that depends on the given one only via another dependent is excluded. A fact with a different justification is included, as is a fact that a statement also asserted, since zelph does not track derivation history. A denial yields no derived fact, and a rule whose negation is blocked by the fact produces nothing while the fact holds. The search is limited by the session’s `.explain-budget`: if the budget is exhausted first, the tuple ends with the keyword `:truncated` following the dependents found so far, ensuring that a search that could not complete never returns an empty tuple. Creates nothing. Refused, and blocked on a graph that is only partially loaded, as `zelph/prune-fact` is. Main thread only.

```
zelph> e f g
zelph> x says (e f g)
zelph> %(zelph/out (string/format "%q" (zelph/holders "e" "f" "g")))
(<zelph/node x says (e f g)>)
zelph> %(zelph/out (string/format "%q" (zelph/prune-fact "e" "f" "g")))
:removed
zelph> S says O
zelph> (X ~ man) => (X ~ mortal)
zelph> bob ~ man
(bob ~ mortal) ⇐ (bob ~ man)
zelph> %(zelph/out (string/format "%q" (zelph/dependents "bob" "~" "man")))
(<zelph/node bob ~ mortal>)
zelph> %(zelph/out (string/format "%q" (zelph/prune-fact "bob" "~" "man")))
:removed
zelph> bob ~ X
Answer: bob ~ mortal
zelph> (a p b) => (c q d)
zelph> a p b
zelph+> %(zelph/out (string/format "%q" (zelph/prune-fact "a" "p" "b")))
:unclaimed
zelph> .list-rules
Listing all rules:
------------------------
(a p b) => (c q d)
(X ~ man) => (X ~ mortal)
------------------------
```

#### Cons cell inspection (read-only)

- **`(zelph/car cell)`**  
  Return the car (first element) of a cons cell, or `nil` if `cell` is not a cons cell.

- **`(zelph/cdr cell)`**  
  Return the cdr (rest of list) of a cons cell. Returns the `nil` list terminator node for the last cell; returns `nil` if `cell` is not a cons cell.

- **`(zelph/parse fact)`**  
  The parts of any fact node represented as the tuple `[subject predicate objects]`, with `objects` being a tuple of nodes ordered by their node identifiers, ensuring that identical facts consistently produce the same tuple (refer to [Fact Decomposition](#fact-decomposition-zelphparse)). Returns `nil` when applied to an atom, a variable, or any entity that is not a fact. Allowed on every thread.

#### Script import

- **`(zelph/import path & args)`**  
  Load and execute a script as a **module**, through the same machinery as the `.import` command — not as a session, whatever the surrounding script is: `path` is resolved against the current working directory first, then the zelph standard library, and the `.zph` extension is optional. Any further arguments must be strings; they are passed to the imported script and available there via `(dyn :args)`. Returns `nil`.  
  This is the way to pull `.zph` files into the network from Janet code — for example `(zelph/import "decimal-arithmetic")` to load the decimal arithmetic rules before working with `&`-literals.  
  Two limitations are in place: `.janet` files are not allowed (instead, employ `zelph/run-script` as detailed below, or Janet’s built-in `import`, `use`, or `dofile` for loading Janet modules), and the function is prohibited while building a rule. Main thread only.

- **`(zelph/run-script path & args)`**  
  Run a Janet source file the way the `janet` CLI would, and the counterpart of `zelph/import` for `.janet` files: the file is evaluated in a fresh environment, and its `main` function — if it defines one — is then called with the script path followed by `args`. Returns `nil`.  
  This is the same runner the binary uses for `zelph <file.janet>`, so a script written for that invocation runs unchanged when called from Janet. What differs is the surroundings, not the runner: on the command line the file is a session, so what it asserts anchors the deduction filter and auto-run fires as usual, while a call to `zelph/run-script` inherits whatever surrounds it.  
  Two properties are worth knowing, because neither follows from the name:
    - Relative imports such as `(use ./helper)` resolve against the **script's** directory, not the process's working directory, so a script can be started from anywhere.
    - Every run starts from an empty module cache. Janet's `require` caches modules process-wide, so without this a second run of the same script would keep the first version of every dependency it pulled in — an edit to `helper.janet` would be invisible for the rest of the session.

#### Running the engine

- **`(zelph/run &opt :quiet :derived)`**  
  Run forward chaining to a fixed point, exactly like the `.run` command. Returns `nil`. When `:quiet` is specified, the run produces no output and instead returns what it met (refer to [Running quietly](#running-quietly)), and when `:derived` is also included, regardless of order, also the facts that were derived; any other argument is not accepted. Main thread only. Rejected when a rule is under construction.

- **`(zelph/run-once &opt :quiet :derived)`**  
  Run a single inference pass, exactly like the `.run-once` command: derives what one application of the rules yields instead of iterating to a fixed point. Returns `nil`. When `:quiet` is specified, it returns what `(zelph/run :quiet)` returns, and the same applies to `:derived`; a single pass never re-tests the negation records, hence its `:naf-final` is always empty. Main thread only. Blocked when a rule is under construction.

- **`(zelph/run-delta &opt :quiet :derived)`**  
  Run inference seeded by the facts created since the previous run, exactly like the `.run-delta` command. Returns `nil`. When `:quiet` is specified, it returns what `(zelph/run :quiet)` returns, and the same applies to `:derived`; the note about falling back to a full pass is omitted. Main thread only. Rejected when a rule is under construction. See [Reasoning incrementally](#reasoning-incrementally).

- **`(zelph/auto-run?)`**  
  Whether the host performs inference following the input currently being processed: the effective state of the `.auto-run` switch. It remains `false` during module import via `.import` or `zelph/import`, as the import executes just once after the module's last line, and also after `.load` or `.load-partial`, which disable auto-run until `.auto-run` reactivates it. A keyword handler executing inference internally checks this value to align with the session's setting. Main thread only.

- **`(zelph/defeated)`**  
  Returns the facts that the most recent execution determined to be no longer justified, presented as an array of `[fact rule]` tuples. Each fact was derived by its rule under a negated condition, the very pattern that the condition negates now holds, and nothing else derives the fact. These are the facts that the run’s warning identifies when `.negation-records` is active, or that its error names in check mode (refer to [Stratified Evaluation](logic.md#stratified-evaluation)). They remain within the graph. Following a run that identifies nothing, the array is empty, and a load that replaces the graph also empties it. Each name merge made since the run moves the array’s pairs to the nodes that were rebuilt during the merge. In a session, a Janet line accesses this list before the auto-run triggered after the line, which constitutes an independent run. Main thread only.

- **`(zelph/negation-records &opt on)`**  
  Fetches the current state of the `.negation-records` switch, or sets it to `true` or `false` to control whether a run logs every fact that a rule with a negated condition produces – enabling later runs to reference the facts whose negated condition has become satisfied. Returns the resulting state; if no argument is provided, no modification occurs. Any input other than a boolean is rejected, meaning that the string `"off"` does not activate the records merely by being truthy. Off by default, as each record consumes memory (refer to [Stratified Evaluation](logic.md#stratified-evaluation)); switching it off drops the records, unless check mode keeps them. This switch is a session-level configuration: when a load replaces the graph, the setting persists but the records are discarded, as they refer to the nodes of the replaced graph. Available only in the main thread.

Auto-run is tied to _processing an input line_: it fires after a statement, and after a Janet block closes. Code that only calls the Janet API — a program driving zelph as a library, or a long-running script that keeps asserting facts between queries — never triggers it, and so has to run the engine itself.

Given this script:

```
.auto-run
%(zelph/fact "socrates" "~" "human")
%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "mortal"))
%(zelph/out (string "derived before run: " (zelph/exists "socrates" "~" "mortal")))
%(zelph/run)
%(zelph/out (string "derived after run: " (zelph/exists "socrates" "~" "mortal")))
```

the session reads, with the script provided via `zelph < script.zph` (given as an argument, the identical lines are displayed, excluding the prompts):

```
zelph> Auto-run is now disabled.
zelph-> <zelph/node socrates ~ human>
zelph-> <zelph/node {(X ~ human)}>
zelph-> derived before run: false
zelph-> Starting reasoning with 24 worker threads.
(socrates ~ mortal) ⇐ {(socrates ~ human)}
Reasoning complete. Total unification matches processed: 1. Total contradictions
found: 0.
Reasoning summary: 1 matches processed, 0 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m0.000s – 1 matches processed, 0 contradictions found.
Ready.
zelph-> derived after run: true
zelph->
```

The rule is in the graph from the moment it is created, but its consequence only exists once the engine has run.

#### Running quietly

A program that translates its own language into zelph communicates the outcomes of a run in that language, whereas what the run prints – the banner, each deduction, the `!` line associated with a contradiction, the notice "Found one or more contradictions!", the note on hidden deductions, and the list of check mode – appears in zelph syntax within the program’s output. When using `:quiet` (entered as `.run quiet` at the prompt), a run prints none of it, not even `Ready.`, and instead returns the same findings as structured data, a struct `{:contradictions C :naf-final N}`:

- `C` is a tuple comprising one struct per distinct contradiction detected during the run, arranged according to initial appearance; if a contradiction arises in multiple passes, it appears only once. `:rule` refers to the rule’s `=>` node, `:conditions` lists the facts that satisfied its positive conditions, ordered by node (empty if all conditions are negations, guards, or paths), and `:conclusion` holds the core node `!` in the case of a denial, the fact that a consequence would have asserted but was refuted, or the claimed fact that a derived refutation met. `:record` says whether the run wrote the record of the contradiction (`:new`), found it already recorded (`:existing`), or failed to preserve any record (`:none`: records deactivated through `.contradiction-records off`, a rule whose conditions are exclusively negations, guards, or paths, or a fact that cannot be named). A derived refutation that meets a claimed fact counts that fact among the members of its record, hence a rule consisting solely of negations, guards, or paths is likewise recorded. `:bindings` is a table from each rule variable to its assigned value. A consequence that the engine refused to build omits `:conclusion`, and `:refused` gives the reason, as shown by a run that prints.
- `N` is a tuple of `{:fact F :rule R}`, one for each fact that the run determined is no longer supported – such facts are those derived via `R` under a negated condition whose pattern has since come to hold, and not derived through any other means – listed in the same sequence as presented by the check. These are the same pairs returned by `(zelph/defeated)`. `N` is populated solely when negation records are preserved: during check mode (`.semi-naive check`), which then does not raise for them, and when `.negation-records` is enabled. Since `.run-once` never re-tests these records, its `N` is always empty.

Each error that a run that prints would raise is still thrown, save for this single case: the negation check within check mode returns its findings as `N`. A condition the engine will not evaluate, like a negated `!=` guard or a path condition where both ends are free, is reported by the next run that prints. The memory of the return expands as the run meets contradictions, and a full pass meets every recorded contradiction once more.

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> ((X waits nil), (X ~ man)) => (X sleeps nil)
zelph-> ¬(john sleeps nil)
zelph-> john ~ man
zelph-> john waits nil
zelph-> %(each e ((zelph/run :quiet) :contradictions) (zelph/out (string/format "%q" e)))
{:bindings @{<zelph/node X> <zelph/node john>} :conclusion <zelph/node ¬(john sleeps nil)>
:conditions (<zelph/node john ~ man> <zelph/node john waits nil>) :record :new :rule
<zelph/node ((X ~ man), (X waits nil)) => (X sleeps nil)>}
```

The run wrote the record, so a later `.run` reports this contradiction as already recorded rather than printing it.

Using `:derived` as well (`.run quiet derived` at the prompt), the struct acquires `:derived D`, a tuple containing one struct per fact the run derived, in the order it derived them. `:fact` holds the derived fact, `:rule` refers to the `=>` node of the rule responsible for its derivation, `:premises` lists the facts that satisfied its positive conditions, ordered by node, `:absent` includes the patterns from its negated conditions that the run determined were missing, and `:bindings` captures the rule’s variables as bound during the firing, transforming those patterns into the actual tests performed. A premise is looked up, never created: when a condition matched a fact that carries more objects than the condition names, the premise is that fact. A full pass returns everything it derived, while `run-delta` returns what it derived from facts added since the last run. `:derived` without `:quiet` is refused, as a run that prints returns `nil`. The memory associated with `D` grows with each derived fact, and the call frees it upon return.

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> (X ~ man) => (X ~ mortal)
zelph-> (X ~ mortal, ¬(X ~ god)) => (X ~ doomed)
zelph-> socrates ~ man
zelph-> %(each e ((zelph/run :quiet :derived) :derived) (zelph/out (string/format "%q" e)))
{:absent () :bindings @{<zelph/node X> <zelph/node socrates>}
:fact <zelph/node socrates ~ mortal> :premises (<zelph/node socrates ~ man>)
:rule <zelph/node (X ~ man) => (X ~ mortal)>}
{:absent (<zelph/node X ~ god>) :bindings @{<zelph/node X> <zelph/node socrates>}
:fact <zelph/node socrates ~ doomed> :premises (<zelph/node socrates ~ mortal>)
:rule <zelph/node ((X ~ mortal), ¬(X ~ god)) => (X ~ doomed)>}
```

#### Reasoning incrementally

`zelph/run` always begins with one classic pass over the whole graph, because it cannot know what the graph looked like before. That pass costs time proportional to the graph — so a program that alternates between asserting a little and reasoning pays, every time, for everything it has ever asserted. This is the shape of most library use: a fact base per document, per position, per request.

`zelph/run-delta` removes that term for the rules eligible to be seeded. It seeds the fixpoint with the facts created since the last run and lets semi-naive evaluation proceed from that point, ensuring their expense aligns with the size of the new additions rather than the size of the graph.

```
%(zelph/fact "plato" "~" "human")
%(zelph/run-delta)
%(zelph/out (string "plato is mortal: " (zelph/exists "plato" "~" "mortal")))
```

```
zelph-> <zelph/node plato ~ human>
zelph-> Starting reasoning with 24 worker threads.
(plato ~ mortal) ⇐ {(plato ~ human)}
Reasoning complete. Total unification matches processed: 0. Total contradictions
found: 0.
Reasoning summary: 0 matches processed, 0 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m0.000s – 0 matches processed, 0 contradictions found.
Ready.
zelph-> plato is mortal: true
```

Measured on a graph holding fact bases of 72 facts each, with one rule, asserting one further fact base and running again:

| fact bases in the graph | `zelph/run` | `zelph/run-delta` |
| --- | --- | --- |
| 100 | 48.7 ms | 0.98 ms |
| 400 | 190.7 ms | 1.26 ms |
| 1200 | 532.3 ms | 1.02 ms |

Both derive identical facts, except in cases where two runs of the same input might diverge anyway: when using non-stratifiable rules, or under `.parallel` with a rule containing a fresh variable (refer to [Deduction Output Modes](rules.md#deduction-output-modes)). The complete run grows with the graph; the seeded variant does not. The figures correspond to a single positive rule.

This is only equivalent to a full run when the graph already is a fixpoint of the current rules — otherwise the skipped pass is exactly the one that would have found the older consequences. `zelph/run-delta` therefore checks, and falls back to a full pass (with a note on the diagnostic channel) unless all of the following hold:

- a previous run has happened and reached its fixpoint, which a `.run-once` that derived something has not, nor has a run interrupted by an error before completion (such as a `.run-export` whose file fails to open),
- no rule was removed since it, and no rule was created since it while a rule featuring a negated condition remains in force – a new rule may modify the [negation levels](logic.md#stratified-evaluation). Every other new rule must see facts that predate it, and the seeded run gives it a single classic pass for this purpose, while simultaneously seeding the rules that existed before. A statement that mentions a rule, like `john believes ((A p B) => (A q B))`, creates no rule in force and counts as a standard fact, whereas eliminating the final statement that mentions a rule brings the rule into force, which counts as its creation if the rule was mentioned at the previous run; a mention made and then eliminated again between two runs has no effect,
- fewer than a million facts have accumulated since, so the record is still kept (past that it is dropped rather than grown without bound, and anything of that size is a bulk load for which a classic pass is the right answer),
- no fact that a run may have derived has been removed since: neither through dropping a [cluster](#scoped-work-clusters) where a run generated derived facts, nor via a pruning or removal command (`.prune-facts`, `.prune-nodes`, `.remove`, `.cleanup`) that removed a fact. A derived fact whose premises still exist is not re-derived by a seeded run, and these commands are unable to distinguish a derived fact from an asserted one. Dropping a cluster that received only write operations maintains the run’s incremental nature, just as does a rule that was constructed and discarded alongside such a cluster,
- `.semi-naive` is on, since delta seeding *is* the semi-naive machinery.

Note that facts created by an imported script count as ordinary additions here. The record is not tied to interactive input, precisely so that a program driving zelph as a library — which never enters a statement — can use it.

The safe pattern is therefore: define the rules, `zelph/run` once, then assert and `zelph/run-delta` per unit of new data.

Certain rules persist in applying classic passes throughout a seeded run, their cost established by the facts that satisfy their conditions, instead of by the addition:

- A rule featuring a negated condition `¬(…)` undergoes evaluation at its designated [negation level](logic.md#stratified-evaluation), and is re-evaluated after each pass that introduces fresh derivations. During a seeded run, it is seeded there from the facts that level has not yet seen and tests the negation of each instance found: a newly introduced fact can only make a negation false, so seeding identifies every instance that might now trigger and could not have previously. The rule takes a classic pass over the facts that satisfy its positive conditions instead when a fact belonging to a predicate it negates has been removed since the last run (for example, due to a [cluster drop](#scoped-work-clusters), which also revokes the claims made within the cluster), as this may cause a negation to become true for an instance where all other conditions are older; when it negates under a predicate variable; and when it is part of rules that negate their own derived outcomes (`:stratifiable false` in `zelph/strata`).
- A rule featuring a [path condition](logic.md#transitive-path-conditions) such as `P279⁺` is seeded through its other conditions, and it is applied classically during an iteration where new facts include one of the predicate that the path walks.
- Every iteration applies a rule classically when its conditions cannot be seeded in any way – specifically, a neural `≈` condition, a nested condition set, a condition without exactly one predicate, or a lack of any positive condition that reads a fact.

By employing `.import math`, the simplifier’s identity fallback `(T red C, ¬(T rw S)) => (T simp C)` is part of rules that negate what they derive, implying that throughout a seeded run inside a `math` session, each reduced term remains subject to inspection.

#### Negation levels

- **`(zelph/strata)`**  
  The [negation levels](logic.md#stratified-evaluation) of the rules currently active, as revealed by `.strata`, without running anything. Returns an array containing one table for each rule, arranged in no specific sequence: `:rule` denotes the rule node, `:level` indicates its negation level, indexed starting at 1 just as `.strata` does, and omitted for rules lacking a negated condition, `:component` assigns a shared identifier to all rules within a single strongly connected component, and `:stratifiable` is set to `false` for any rule within a component that negates what it derives. A program that writes rules can therefore allow the engine to decide whether a valid ordering exists, while the engine also considers every other rule in force. A rule over predicates – one that only extends the predicates it reads, like `(R is transitive, A R B, B R C) => (A R C)` – also features `:transparent true`, and no other row includes this key. While such a rule is in force, the levels compare predicates alone, not predicate and object together, meaning two rules sharing a predicate depend on each other regardless of their objects. Available only on the main thread.

The first two rules below create a loop involving a negation – a bachelor wearing a ring is considered married, which the first rule negates – thus both report `stratifiable false`; the third waits for them at level 2:

```
zelph> (X ~ man, ¬(X ~ married)) => (X ~ bachelor)
zelph> (X ~ bachelor, X wears ring) => (X ~ married)
zelph> (X ~ man, ¬(X ~ bachelor)) => (X ~ husband)
zelph> %(each e (zelph/strata) (zelph/out (string (e :rule) " -- level " (or (e :level) "none") ", stratifiable " (e :stratifiable))))
(¬(X ~ bachelor), (X ~ man)) => (X ~ husband) -- level 2, stratifiable true
((X wears ring), (X ~ bachelor)) => (X ~ married) -- level none, stratifiable false
(¬(X ~ married), (X ~ man)) => (X ~ bachelor) -- level 1, stratifiable false
```

#### Scoped work: clusters

Apart from clusters, the Janet API does not eliminate anything: the graph expands exclusively. A program that asserts a fact base, reasons about it, and reads the conclusions cannot take the fact base out again, thus every question it ever poses remains – and the pattern "one fact base per document, per position, per request" accumulates without bound.

A **cluster** is the answer. It records the IDs of the nodes *created* while it is active, and dropping it removes exactly those. Nodes that already existed are never recorded, so a drop cannot reach them: a cluster is safe as scratch space over a graph loaded from disk. The nodes associated with the engine’s internal vocabulary (`refuted`, `opaque`, `closure`, and similar) are likewise never recorded, even if a cluster is active when they are first created: a later statement located outside the cluster might still reference them, and the drop leaves them in place. This is the same mechanism `.explain` uses internally to evaluate a pattern without asserting it.

- **`(zelph/cluster &opt name)`**  
  Activate the named cluster, creating it if needed. `nil` or `"default"` deactivates cluster tracking. Without an argument nothing changes. In every case the function returns the name of the cluster that is active afterwards, or `nil` for the default. A name beginning with `__` is refused: the engine’s internal scratch clusters use such naming, and the engine would either discard or merge a cluster with that name alongside its own. Main thread only. Prohibited during rule construction.

- **`(zelph/cluster-drop name)`**  
  Remove every node recorded in the cluster — with its edges and names — and return how many were removed. A fact located beyond the cluster boundary that refers to a cluster node is removed with it, as is every fact referencing any of those facts, and every rule where one of them serves as a condition or a conclusion (refer to `.help .remove`). The default cluster cannot be dropped; an unknown name removes nothing and returns 0. Main thread only. Prohibited during rule construction.

- **`(zelph/clusters)`**  
  An array of `[name node-count]` tuples, one per existing cluster. Main thread only.

The scratch-space pattern, which is what makes assert–reason–read–discard loops possible at all:

```janet
(zelph/cluster "scratch")
(zelph/fact "subject-of-this-request" "~" "something")
(zelph/run-delta)
(def answer (zelph/query ...))
(zelph/cluster nil)
(zelph/cluster-drop "scratch")
```

Unlike `.cluster` and `.cluster-drop`, these print nothing. That is deliberate: a caller that scopes one question per iteration of its own loop invokes them thousands of times, and the commands' status lines would be noise on the caller's own output channel rather than information. The return values carry what a program actually wants to know.

Clusters are session state and are not persisted by `zelph/save`, and a load that replaces the graph forgets them all.

#### Persistence

- **`(zelph/save file)`**  
  Save the current network to a binary file, exactly like the `.save` command. The filename must end with `.bin`. Returns `nil`. Main thread only. Blocked during rule construction.

- **`(zelph/load file)`**  
  Load a previously saved network, or import a dump, in the same manner as the `.load` command:
  - If `file` bears the `.bin` extension, the saved network replaces the graph (rapid): every node, name, rule, and cluster is purged, together with any reference to them, such as negation records, the data upon which `zelph/run-delta` depends, display registrations, the handles of compiled nets, and the import registry.
  - If `file` concludes with `.json` or `.json.bz2` (a Wikidata dump) and the graph is empty, the `.bin` cache located adjacent to it is read and replaces the graph, operating identically to a `.bin` file; without a cache, the data is ingested, and the resulting graph is saved as that cache. Into a non-empty graph, the data is ingested and merged, and no cache is read or written.  
    The Janet environment stays the same, as do the registered keywords. A Janet value that referred to a node before the load is not refreshed: it points to whatever entity has the same ID in the newly loaded graph, or to nothing, and a variable previously named by a symbol like `'X` within the same block becomes a new one after the load. In the interactive REPL, loading disables auto-run (large datasets). Inside a module (`.import`) auto-run is already suspended for the duration of the load and restored afterwards. A script named on the command line is a session, so there `.load` disables auto-run exactly as it does in the REPL — see [Scripts and Modules](modules.md). Returns `nil`. Main thread only. Rejected when a rule is currently under construction.

- **`(zelph/epoch)`**  
  An integer that increases monotonically throughout the process. Each creation of a fresh engine advances it (`.new`), each replacement of the graph during a load, and each Wikidata dump imported into the running session. A Janet value storing node ids or labels, like a cache built from the graph, remains valid as long as the epoch matches the one under which it was constructed; a standard statement does not alter the epoch. Permitted on every thread.

#### Node annotations

A front end that generates statements on behalf of its user frequently needs to preserve the origin of each one – the sentence that asserted a fact, the file and line number associated with a rule – and to quote it again later. Node annotations store such textual data: a node carries a list of values, ordered by their insertion, with each value appearing at most once, under a key selected by the caller. These annotations are not integrated into the graph. They do not produce a node, an edge, or a name, thus remaining invisible to queries and rules. Adding annotations to the node of a rule does not constitute a mention of it: the rule stays in force, unlike a fact that names it, which would turn it off (see [Mentioning a Rule Is Not Asserting It](logic.md#mentioning-a-rule-is-not-asserting-it)).

An annotation is associated with its node and follows it. Each deletion of the node erases its annotations – whether through `.prune-facts`, `.prune-nodes`, `.remove`, `.cleanup`, `.remove-rules`, or a dropped cluster – meaning a fact reasserted and restored under the same identifier begins without them. During a name merge (`.name`), the annotations of the vanishing node are transferred to the surviving node, with each key’s values combined behind the latter’s own, while a fact reconstructed under a new identifier during the merge retains its annotations. A cluster records no annotation: any annotation added within a cluster to a pre-existing node persists even after the cluster is dropped. The `.save` command outputs the annotations as a distinct section following the names, a slice (`.save-predicates`) preserves the annotations of the nodes it keeps, and a load operation that replaces the graph replaces them with the file’s. A partial load (`.load-partial`, also sourced from a manifest) reads none. An engine older than the section loads such a file without annotations and excludes them from its next `.save`.

- **`(zelph/annotate node key value)`**  
  Add the string `value` under the string `key` of `node`, following any values the key already contains. Returns `true` if the value was added, or `false` if the key already holds the value and no modification occurred. `node` must be a `zelph/node`, a node identifier, or a name, which is resolved without creating a new node; a name not associated with an existing node is rejected. Main thread only.

- **`(zelph/annotations node &opt key)`**  
  The collection of annotations associated with `node`: a struct mapping each key to the tuple of its corresponding values, maintaining the sequence in which they were inserted, or, when `key` is provided, the tuple of values linked to that specific key. Returns empty if no annotations are present, including in cases where the node is absent. Creates nothing. Permitted on any thread.

- **`(zelph/unannotate node key &opt value)`**  
  Remove `value` from the list of values associated with `key`, or if no `value` is provided, delete the entire `key` along with all its associated values. Returns the count of values that were removed, `0` if no removal occurred. Main thread only.

```
zelph> a p b
zelph> %(def f (zelph/fact "a" "p" "b"))
<zelph/node a p b>
zelph> %(zelph/annotate f "source" "notes.txt, line 1")
true
zelph> %(zelph/annotate f "source" "notes.txt, line 7")
true
zelph> %(zelph/annotate f "source" "notes.txt, line 1")
false
zelph> %(zelph/out (string/format "%q" (zelph/annotations f)))
{"source" ("notes.txt, line 1" "notes.txt, line 7")}
zelph> %(zelph/unannotate f "source" "notes.txt, line 1")
1
zelph> %(zelph/out (string/format "%q" (zelph/annotations f "source")))
("notes.txt, line 7")
```

#### Neural network functions

zelph 0.9.7 adds a neural substrate: raw weighted edges act as synapses, layers are ordinary sets, and sub-graphs compile into feed-forward networks that rules can access through the `≈` operator. The full documentation – including semantics, training workflow, and a Wikidata demonstration – resides on the dedicated page [Neural Networks in the Graph](neural.md). For completeness, the functions:

- **`(zelph/nn-connect from to &opt weight)`** – create a raw weighted edge (synapse); invisible to reasoning. Default weight 1. Main thread only.
- **`(zelph/weight from to)`** – weight of a directed node pair: the stored value of a synapse or fact probability, 1 for a real graph edge lacking a stored entry, `nil` if neither is present.
- **`(zelph/set-weight from to w)`** – assign a weight to an existing synapse or edge. Main thread only.
- **`(zelph/nn-compile layers)`** – compile a feed-forward view of a sub-graph (layer nodes, input first); returns an integer handle that becomes invalid upon a subsequent load that replaces the graph. Main thread only.
- **`(zelph/nn-nodes handle layer)`** – neurons within a compiled layer, ordered by index.
- **`(zelph/nn-eval handle inputs)`** – perform a forward pass using plain number vectors.
- **`(zelph/nn-train handle inputs targets &opt learning-rate)`** – execute one stochastic gradient descent step; returns the loss before the update.
- **`(zelph/nn-write-back handle)`** – transfer learned weights into the graph’s weight store (required for `.save` and for `≈` conditions).
- **`(zelph/nn-snapshot handle)`** – copy the weights out as an array of arrays of numbers.
- **`(zelph/nn-restore handle snapshot)`** – put a snapshot back; dimensions must align, missing synapses remain missing.
- **`(zelph/nn-connect-layers from-layer to-layer &opt scale seed)`** – fully interconnect two layers; idempotent, preserves existing weights. Main thread only.
- **`(zelph/nn-train-nodes handle inputs targets &opt learning-rate)`** – stochastic gradient descent step addressing neurons by graph node (multi-hot).
- **`(zelph/nn-eval-nodes handle inputs &opt top-k)`** – forward pass addressed by node; returns sorted `[node score]` tuples.
- **`(zelph/approx pattern net-name)`** – tag a fact pattern as a neural rule condition; desugared form of `≈net(pattern)`. Returns the tag fact. Main thread only.

**Threading.** Unlike most of the API above, a compiled network may be used from more than one thread. Any number of threads may **evaluate** concurrently — `nn-eval`, `nn-eval-nodes`, `nn-snapshot`, `nn-write-back` — while a **training** step (`nn-train`, `nn-train-nodes`) or `nn-restore` excludes them for its duration. So a program may evaluate a network from a worker thread while another thread trains it, which is what lets training run continuously alongside the work that uses the result. `nn-write-back` is the sole one permitted to modify the graph from any thread: it overwrites only the weights associated with the network’s own synapses, under the lock of the graph’s weight store, and introduces no new elements. Constructing a network must occur only on the main thread, just like all other write operations (refer to [Threads](#threads)): `nn-connect`, `nn-connect-layers`, `set-weight`, and `nn-compile` are refused on non-main threads, so build the network before sharing its handle. `zelph/load`, also restricted to the main thread only, must not execute while another thread writes a network back: it replaces the graph without taking the weight store’s lock. `zelph/save` reads the weights while holding that lock.

The two node-addressed calls price the input layer by the number of active neurons rather than by its width, because they are told which ones are non-zero; the dense pair cannot be. Same numbers, so prefer them whenever the input is a multi-hot encoding of a large domain — see [Neural networks](neural.md#node-addressed-training).

#### Output

- **`(zelph/out text)`**  
  Emit `text` through zelph's own output pipeline (the `Out` channel) rather
  than Janet's `stdout`. Use this in modules: it is the only form that
  reaches the REPL, `.log` capture, the WebAssembly playground and the test
  harness alike, and it respects the quieting that `?` applies to its
  inference pass. Janet's `print` writes to the process's standard output
  and is invisible to all of them. The standard library uses it for load
  banners, e.g. `(zelph/out "math-syntax loaded: $( ... ) term islands")`.

#### Display registration

A script can declare how *its own* notation is written, so that
`node_to_string` renders matching terms in it. The C++ side knows
precedence, associativity, delimiters, numeral prefix and leaf grammar —
never a concrete operator. A term is rendered under a scheme only when the
whole subtree is expressible in it; otherwise the default rendering is used.
The registrations name predicates by node, so when a graph is replaced during a
load, they are discarded alongside the schemes; importing the module again
reestablishes the registrations.

- **`(zelph/register-display-scheme name open close &opt options)`**  
  Declare a scheme. Main thread only. `open`/`close` are the delimiters wrapped around a
  rendered term (`"$( "` and `" )"` for the stdlib's term islands).
  `options` is a struct:

    - `:numeral-prefix` — the sigil numerals carry inside the scheme
      (`"&"` keeps the default, `""` drops it).
    - `:name-first`, `:name-chars` — the identifier grammar. A leaf is
      writable in the scheme only if its name matches; a scheme that
      declares no grammar can never deviate from the default rendering,
      which is the safe default rather than a limitation.

- **`(zelph/set-infix-display scheme entries)`**  
  Register infix operators into a declared scheme. `entries` is an array of
  `[predicate precedence associativity]`, with associativity `:left` or
  `:right`. Higher precedence binds tighter. Registration is additive across
  calls, but a predicate belongs to at most one scheme — re-registering it
  is an error. Main thread only.

- **`(zelph/set-application-display scheme predicates)`**  
  Register predicates whose facts render in call notation: `(S P O)` becomes
  `S(O)`. Application heads must be atomic names; a composite head falls
  back to the default rendering. Registering a predicate here also excludes
  it from the self-fact display sugar. Main thread only.

!!! warning "Display alone is half a notation"
    Registering an operator for display does **not** teach any parser to
    read it back, so zelph can end up printing syntax it cannot itself
    consume. For the stdlib's term islands there is a combined entry point,
    `math-syntax/operator`, which extends the island grammar and the display
    scheme from one table — see
    [The math Front End](math/frontend.md#adding-an-operator). Reach for
    `zelph/set-infix-display` directly only when you are registering a
    notation you do not intend to parse.

#### Redefinable hooks

- **`(zelph/number str)`**  
  Called by the parser for every `&`-prefixed number literal (e.g. `&42` becomes `(zelph/number "42")`). The default implementation raises an error; arithmetic scripts such as [`stdlib/decimal-arithmetic.zph`](https://github.com/acrion/zelph/blob/main/stdlib/decimal-arithmetic.zph) (decimal) or [`stdlib/binary-arithmetic.zph`](https://github.com/acrion/zelph/blob/main/stdlib/binary-arithmetic.zph) (binary) redefine it to build the cons-list representation of their choice. See [Number Literals](logic.md#number-literals).

- **`(zelph/set-number-digits digits)`**  
  Register the digit alphabet of the loaded number representation, as an array
  of digit nodes or names in ascending order of value (e.g. `["0" "1"]` for
  binary). `node_to_string` then displays every `nil`-terminated cons list
  consisting solely of these digit nodes as a decimal `&`-literal -- the
  inverse of `zelph/number`. All other cons lists keep the generic `<...>`
  display. An empty array disables the feature. Names are resolved within the
  current language, whereas `zelph/list-chars` builds its lists from the
  characters in `zelph`: register the digits of that representation under
  `.lang zelph`, or pass the digit nodes. Main thread only.

- **`(zelph/no-selffact-sugar preds...)`**  
  Exclude predicates from the self-fact display sugar. A fact whose subject
  and object are the same node normally prints in the compact prefix form
  `:pred subject` (see [The Self-Fact Prefix `:`](concepts.md#the-self-fact-prefix)).
  For term-forming operators this contraction is undesirable: `(&1 + &1)` is
  a self-fact only because all terms are hash-consed, and should print
  verbose. Registered predicates always render as `S P S`. The registration
  is **additive** across calls, so stacked modules can each register their
  own operators; it affects display only — `:+ &1` remains valid _input_ —
  and is session state like the digit alphabet (cleared by `.new` and by a
  load that replaces the graph, not persisted). The arithmetic modules
  register their operators (`+`, `-`, `d+`, ...), `stdlib/eml.zph` registers
  `eml`. Main thread only.

## Referencing Janet Variables in zelph: Unquote `,`

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
>
> It takes a **name**, not an expression: `,(zelph/resolve "Berlin")` does not
> parse. Bind the value first with `%(def berlin ...)` and write `,berlin`.

### How Unquote Works

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

### Unquote in Complex Structures

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

## Practical Patterns

### Generating Facts from Data

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

### Parameterized Rules

Janet functions can encapsulate common rule patterns:

```
%
(defn transitive-rule [rel]
  (zelph/build-rule
    (fn []
      (let [condition
            (zelph/collection
              (zelph/fact 'X rel 'Y)
              (zelph/fact 'Y rel 'Z))]
        (zelph/fact condition "~" "conjunction")
        (zelph/fact condition "=>" (zelph/fact 'X rel 'Z))))))

(transitive-rule "is part of")
(transitive-rule "is ancestor of")
(transitive-rule "is located in")
%
```

A single function generates a transitive inference rule for any given relation. The `let` pattern captures the condition set and employs it again – within `zelph/build-rule`, the Janet equivalent of the focus operator `*` in zelph syntax.

### Parameterized Queries

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

### Wikidata Query Helpers

Janet functions can provide a higher-level query interface for Wikidata:

```
%
(defn wikidata-query [& clauses]
  "Generate and execute a conjunction query from S-P-O triples."
  (let [facts (map (fn [[s p o]] (zelph/fact s p o)) clauses)
        condition-set (zelph/collection ;facts)]
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

### Read-Only Graph Inspection

The following functions inspect the graph without modifying it. Unlike `zelph/fact`, they never create nodes or facts as a side effect. If a referenced name does not exist in the graph, the functions return `false`, `nil`, or an empty array as appropriate.

#### Existence Check: `zelph/exists`

`zelph/exists` checks whether a fact was claimed — asserted or derived:

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

#### Claimed or Merely Written Down

Writing a rule materialises its conditions and consequences as real fact
nodes — the engine has nothing else to match against. Such a node is a
statement the rule *writes down*, not one anybody claimed, and the whole
read surface says so: `zelph/exists`, `zelph/sources`, `zelph/targets` and
the two closures all answer about **claimed** facts, which is what the
queries and the reasoning engine have always meant by a fact.

`zelph/mentioned` asks the other question — is this node there at all —
and is the way to inspect rule structure:

```
zelph> (Berlin is-capital-of Germany) => (Germany has-capital yes)
zelph> Paris is-capital-of France
zelph> %(zelph/out (string "exists Berlin: " (zelph/exists "Berlin" "is-capital-of" "Germany")))
exists Berlin: false
zelph> %(zelph/out (string "mentioned Berlin: " (zelph/mentioned "Berlin" "is-capital-of" "Germany")))
mentioned Berlin: true
zelph> %(zelph/out (string "exists Paris: " (zelph/exists "Paris" "is-capital-of" "France")))
exists Paris: true
zelph> %(zelph/out (string "sources: " (string/join (sorted (map zelph/name (zelph/sources "is-capital-of" "Germany"))) ",")))
sources: 
```

Claiming the statement — by typing it, by deriving it, or by calling `zelph/fact` within a Janet block or within the handler of a block keyword — makes it data from that moment on, and the rule fires:

```
zelph> %(zelph/fact "Berlin" "is-capital-of" "Germany")
<zelph/node Berlin is-capital-of Germany>
(Germany has-capital yes) ⇐ (Berlin is-capital-of Germany)
zelph> %(zelph/out (string "exists Berlin now: " (zelph/exists "Berlin" "is-capital-of" "Germany")))
exists Berlin now: true
```

A pattern carrying a variable is never data either, so a rule condition
such as `(X p Y)` is invisible to all of the above by the same rule.

A rule constructed using [`zelph/rule`](#zelphrule) writes down what its argument forms build into it, precisely as a typed rule does: the forms execute within the rule’s build, meaning any ground fact generated inside the rule – via `zelph/fact` or as a cons cell from `zelph/list-chars`, directly or through a helper function invoked by the forms – constitutes a pattern of the rule, not a claim. Only what the rule holds is marked: a fact generated by the forms that the rule does not hold, such as one a helper asserts in addition to the fact it returns, is classified as a claim once the call succeeds, and a call that fails keeps none of it. A fact created before the call, on a preceding line or within a `let` around the call, was already claimed at the time of its construction, and it stays a claim:

```
zelph> %(zelph/rule [(zelph/fact "Rome" "is-capital-of" "Italy")] (zelph/fact "Italy" "has-capital" "yes"))
<zelph/node {(Rome is-capital-of Italy)}>
zelph> %(zelph/out (string "exists Rome: " (zelph/exists "Rome" "is-capital-of" "Italy")))
exists Rome: false
zelph> %(let [madrid (zelph/fact "Madrid" "is-capital-of" "Spain")] (zelph/rule [madrid] (zelph/fact "Spain" "has-capital" "yes")))
<zelph/node {(Madrid is-capital-of Spain)}>
zelph+> %(zelph/out (string "exists Madrid: " (zelph/exists "Madrid" "is-capital-of" "Spain")))
exists Madrid: true
zelph> %(zelph/out (string "exists Spain: " (zelph/exists "Spain" "has-capital" "yes")))
exists Spain: true
```

The second rule fired immediately upon being written, because its condition was already stated as a claim; the prompt’s `+` says that the run concealed the deduction. The plain function `zelph/rule*` receives its arguments already built, meaning that for it, every fact built within its arguments constitutes a claim.

A rule that a statement mentions is written down together with what it says. The parser wraps each `=>` inside another statement in `zelph/rule-text`, thereby marking the ground parts it has created, so a typed mention claims neither the condition nor the consequence. When expressed using only `zelph/fact`, the same mention claims its parts: each invocation within a block constitutes a claim, and the nested invocations execute before the `=>` invocation, so nothing can yet know that their outcomes will serve as the rule’s parts. When enclosed within `zelph/rule-text` or `zelph/build-rule`, the invocations claim nothing:

```
zelph> %(zelph/fact "draft" "contains" (zelph/fact (zelph/fact "Lisbon" "is-capital-of" "Portugal") "=>" (zelph/fact "Portugal" "has-capital" "yes")))
<zelph/node draft contains ((Lisbon is-capital-of Portugal) => (Portugal has-capital yes))>
zelph> %(zelph/out (string "exists Portugal: " (zelph/exists "Portugal" "has-capital" "yes")))
exists Portugal: true
zelph> %(zelph/fact "draft" "contains" (zelph/rule-text (fn [] (zelph/fact (zelph/fact "Oslo" "is-capital-of" "Norway") "=>" (zelph/fact "Norway" "has-capital" "yes")))))
<zelph/node draft contains ((Oslo is-capital-of Norway) => (Norway has-capital yes))>
zelph> %(zelph/out (string "exists Norway: " (zelph/exists "Norway" "has-capital" "yes")))
exists Norway: false
```

A claim reaches the facts nested in what it asserts that remain a rule’s patterns, located outside a `=>`. When a `zelph/fact` invocation obtains a fact built by a prior call within the same block, it finds the fact already claimed. If a call obtains a fact that is still a rule’s pattern, it claims the fact, and the rule fires. The claim does not traverse a set constant or a collection handed in: a pattern among its members stays a pattern. In the example below, that fact serves as the condition for the rule built with `zelph/rule` above, which `zelph/rule-text` returns without claiming it:

```
zelph> %(def rome (zelph/rule-text (fn [] (zelph/fact "Rome" "is-capital-of" "Italy"))))
<zelph/node Rome is-capital-of Italy>
zelph> %(zelph/out (string "exists Rome: " (zelph/exists "Rome" "is-capital-of" "Italy")))
exists Rome: false
zelph> %(zelph/fact "atlas" "lists" rome)
<zelph/node atlas lists (Rome is-capital-of Italy)>
zelph+> %(zelph/out (string "exists Rome now: " (zelph/exists "Rome" "is-capital-of" "Italy")))
exists Rome now: true
zelph> %(zelph/out (string "exists Italy: " (zelph/exists "Italy" "has-capital" "yes")))
exists Italy: true
```

A claim never reaches into the argument positions of an [opaque predicate](logic.md#mentioning-a-statement-opaque-predicates), declared with `P ~ opaque`: `zelph/fact`, a handler’s `zelph/fact`, and the C API’s `zelph_fact` claim the opaque fact and nothing it holds there. What a separate `zelph/fact` invocation built was claimed by that call, so the argument that is only mentioned is written using `zelph/mention`, either in its fact form or, for a statement with parts of its own, in its thunk form:

```
zelph> recalls ~ opaque
zelph> %(zelph/fact "history" "recalls" (zelph/mention "Bonn" "is-capital-of" "Germany"))
<zelph/node history recalls (Bonn is-capital-of Germany)>
zelph> %(zelph/out (string "exists Bonn: " (zelph/exists "Bonn" "is-capital-of" "Germany")))
exists Bonn: false
zelph> %(zelph/fact "history" "recalls" (zelph/mention (fn [] (zelph/fact "Bonn" "hosted" (zelph/fact "Bundestag" "met" "there")))))
<zelph/node history recalls (Bonn hosted (Bundestag met there))>
zelph> %(zelph/out (string "exists Bundestag: " (zelph/exists "Bundestag" "met" "there")))
exists Bundestag: false
zelph> %(zelph/fact "Bonn" "is-capital-of" "Germany")
<zelph/node Bonn is-capital-of Germany>
zelph> %(zelph/out (string "exists Bonn now: " (zelph/exists "Bonn" "is-capital-of" "Germany")))
exists Bonn now: true
```

A rule is written down in the same way. Nothing holds the `=>` fact that `zelph/mention` returns, yet it is not in force: the mention marks it just as it marks every statement it writes, and only a claim of the same rule puts it in force – here `zelph/dedup-rule`, and likewise a typed rule, `zelph/fact` or `zelph/build-rule`:

```
zelph> %(zelph/mention (fn [] (zelph/fact (zelph/fact "Vienna" "is-capital-of" "Austria") "=>" (zelph/fact "Austria" "has-capital" "yes"))))
<zelph/node (Vienna is-capital-of Austria) => (Austria has-capital yes)>
zelph> Vienna is-capital-of Austria
zelph> %(zelph/out (string "exists Austria: " (zelph/exists "Austria" "has-capital" "yes")))
exists Austria: false
zelph> %(zelph/dedup-rule (fn [] (zelph/fact (zelph/fact "Vienna" "is-capital-of" "Austria") "=>" (zelph/fact "Austria" "has-capital" "yes"))))
<zelph/node (Vienna is-capital-of Austria) => (Austria has-capital yes)>
(Austria has-capital yes) ⇐ (Vienna is-capital-of Austria)
zelph> %(zelph/out (string "exists Austria now: " (zelph/exists "Austria" "has-capital" "yes")))
exists Austria now: true
```

#### Node Names: `zelph/name`

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

If no name exists in the requested language, `zelph/name` falls back through English, zelph, and other available languages before returning `nil`. This behaviour is right for showing a node but inappropriate for determining if a language names it. Appending `:strict` asks the second question: the answer is the node’s name precisely in that language, or `nil`.

#### Looking Names Up: `zelph/lookup` and `zelph/lang`

`zelph/resolve` generates a node for a name it fails to locate, rendering it unsuitable for asking whether a term refers to anything: such a query would introduce the term into the graph. `zelph/lookup` inquires without generating a new node. `zelph/lang` reads and sets the language used to resolve names, just as `.lang` does, and returns the prior value:

```
zelph> dog ~ animal
zelph> %(zelph/lookup "dog")
<zelph/node dog>
zelph> %(nil? (zelph/lookup "cat"))
true
zelph> %(zelph/name (zelph/lookup "dog") "de")
"dog"
zelph> %(nil? (zelph/name (zelph/lookup "dog") "de" :strict))
true
zelph> %(zelph/lang "de")
"zelph"
de> %(zelph/resolve "Hund")
<zelph/node Hund>
de> %(zelph/lang "zelph")
"de"
zelph> %(nil? (zelph/lookup "Hund"))
true
zelph> %(zelph/lookup "Hund" "de")
<zelph/node Hund>
```

`zelph/lookup-all` provides all nodes that carry a name, while `zelph/lookup` returns the single node the name points to. Within the Wikidata network, a label is frequently shared – for instance, "man" refers to multiple entities – and [Labels that several entities share](wikidata.md#labels-that-several-entities-share) explains which one holds it; `zelph/lookup-all` presents all such entities, ordered according to that ranking.

#### Graph Traversal: `zelph/sources` and `zelph/targets`

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

#### List Decomposition: `zelph/car` and `zelph/cdr`

These functions decompose cons cells (Lisp-style list nodes), mirroring the classic Lisp `car`/`cdr` operations:

- **`zelph/car`** returns the first element (subject) of a cons cell.
- **`zelph/cdr`** returns the rest of the list (object) of a cons cell.

Both return `nil` for invalid input. `zelph/cdr` returns the `nil` node for the last cell in a list.

```
%(def list-42 (zelph/list-chars "42"))
%(zelph/car list-42)                    # → <zelph/node> for "2"
%(zelph/cdr list-42)                    # → <zelph/node> for the sublist <4>
%(zelph/car (zelph/cdr list-42))        # → <zelph/node> for "4"
%(zelph/cdr (zelph/cdr list-42))        # → <zelph/node> for nil
```

**Important:** `zelph/sources` and `zelph/targets` do _not_ work for decomposing cons cells, because cons cells are relation nodes (fact nodes) in the graph, not entities that appear as subjects or objects in higher-level facts. Use `zelph/car` and `zelph/cdr` instead.

#### Practical Example: Inspecting a List

Combining `zelph/car` and `zelph/cdr` to walk a cons-list:

```
zelph> <42>
<2 4>
zelph> %
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
24
```

Note: `zelph/car` and `zelph/cdr` mirror the classic Lisp operations. `zelph/sources` and `zelph/targets` cannot be used for cons cell decomposition because cons cells are relation nodes in the graph, not entities.

#### Fact Decomposition: `zelph/parse`

`zelph/car` and `zelph/cdr` accept only cons cells, while `zelph/sources` and `zelph/targets` never return the parts of a fact. `zelph/parse` breaks down any fact node into `[subject predicate objects]`, maintaining the sequence of objects according to their node ids. A rule condition that is negated, written as `¬(P)`, corresponds to the fact `((P) ~ negation)` and decomposes identically to any other fact:

```
zelph> %(def likes (zelph/fact "john" "likes" "mary"))
<zelph/node john likes mary>
zelph> %(pp (zelph/parse likes))
(<zelph/node john> <zelph/node likes> (<zelph/node mary>))
zelph> %(nil? (zelph/parse (zelph/lookup "john")))
true
zelph> %(pp (zelph/parse (zelph/negate likes)))
(<zelph/node john likes mary> <zelph/node ~> (<zelph/node negation>))
```

#### Proofs as Data: `zelph/explain`

`.explain` prints a proof in the form of a tree structure, while `zelph/explain` returns the identical proof as structured data, with each step represented by a distinct struct, enabling a program to respond to the query "why" using its own language. It resolves the fact without generating any new content – using `zelph/fact` to construct it would claim a derived fact – and returns `nil` if the graph does not contain such a fact:

```
zelph> (X ~ man) => (X ~ mortal)
zelph> socrates ~ man
(socrates ~ mortal) ⇐ (socrates ~ man)
zelph> %(pp (zelph/explain "socrates" "~" "mortal"))
{:absent () :bindings {<zelph/node X> <zelph/node socrates>} :cycle-leaf false
 :fact <zelph/node socrates ~ mortal> :more false :premises ({:absent ()
 :bindings {} :cycle-leaf false :fact <zelph/node socrates ~ man> :more false
 :premises () :status :axiom :walk-edges () :walked ()}) :rule <zelph/node (X ~
 man) => (X ~ mortal)> :status :derived :walk-edges () :walked ()}
zelph> %(nil? (zelph/explain "plato" "~" "mortal"))
true
```

A step marked with `:unfounded` represents a fact that could have been inferred by rules but currently lacks any supporting derivation: a premise was removed, or a negated premise has come to hold since the fact was derived, where `.explain` prints `[asserted; no derivation found]`. A statement containing a comma-separated list in its text, like a rule with several conditions, builds a new condition set every time it is written, so only its node names it: pass the node, `(zelph/explain node)`.

#### Combining with Query Results

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

### Building a SPARQL-like Interface

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

### Rule Construction: `zelph/rule` and `zelph/negate`

These functions simplify the creation of inference rules from Janet. While rules can always be built manually using `zelph/collection`, `zelph/fact`, and `let` bindings (see [Rules in Janet: The `let` Pattern](#rules-in-janet-the-let-pattern)), `zelph/rule` encapsulates the entire pattern in a single call.

#### `zelph/negate`

Creates the negated condition from a fact pattern and returns it – the fact `((pattern) ~ negation)`, which is what a rule names as its condition:

```
%(zelph/negate (zelph/fact 'A ".." 'X))
```

It is equivalent to the zelph syntax fragment `¬(A .. X)`, or `((A .. X) ~ negation)`, within a condition set. Use the returned node as the condition; the pattern passed in remains positive in all contexts where it is utilized.

#### `zelph/rule`

Creates a complete inference rule: a conjunction of conditions linked to one or more consequences via `=>`.

```
(zelph/rule conditions consequence1 consequence2 ...)
```

- **conditions**: An array or tuple of fact nodes (the conjunction).
- **consequences**: One or more fact nodes to deduce when conditions match.
- **Returns**: The condition set node (the rule's identity in the graph).

`zelph/rule` is a macro. The argument forms execute during the rule’s build, just as the thunk of `zelph/build-rule` does, meaning the rule is constructed in the same manner as a typed rule: a collection that the forms write into the rule is the rule’s own, and any ground fact they generate within it is a pattern of the rule, not a claim, whereas a fact they build that the rule does not hold is a claim once the call succeeds. The underlying plain function is `zelph/rule*`, accepting identical arguments; it is the version to pass to `apply` or `map`, and because it receives its arguments already built, they remain unchanged.

**Example — Transitivity rule:**

```
# zelph syntax:
(*{(X R Y) (Y R Z) (R is transitive)} ~ conjunction) => (X R Z)

# Janet equivalent using zelph/rule:
%(zelph/rule
   [(zelph/fact 'X 'R 'Y)
    (zelph/fact 'Y 'R 'Z)
    (zelph/fact 'R "is" "transitive")]
   (zelph/fact 'X 'R 'Z))
```

**Example — Negation (finding the last element of a list):**

```
# zelph syntax:
(*{(A in _Num) ((A .. X) ~ negation)} ~ conjunction) => (A "is last digit of" _Num)

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

#### Parameterized Rules with `zelph/rule`

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

#### Setting Up Digit-Wise Addition

As a tangible illustration of integrating `zelph/rule` with Janet loops to establish a domain for the reasoning engine, here is the setup for single-digit arithmetic. All reasoning takes place entirely within zelph’s inference engine – Janet is responsible solely for producing the initial facts and rules:

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

The previously stated rules are universally applicable (they work on any list, not exclusively numerical ones). The lookup table encodes digit arithmetic through graph facts. Starting from this point, additional rules can process multi-digit numbers by walking lists from right to left, extracting digits, applying the lookup table, handling carries, and constructing the result lists from the bound digits – entirely inside zelph’s reasoning engine.

## Extending the REPL: `zelph/register-keyword`

`(zelph/register-keyword keyword handler)` registers a custom multi-line syntax block for the REPL and for `.zph` scripts. After the keyword is entered (optionally with content on the same line), subsequent lines are accumulated verbatim until an empty line, then passed as a single string to `handler`. The handler may return `:incomplete` to signal that the block is not yet complete (e.g. unbalanced braces) — accumulation then continues, and a second consecutive blank line forces dispatch. String results are printed line by line. Main thread only.

This is the mechanism behind zelph's [SPARQL support](sparql.md): the `sparql` keyword is an ordinary registered handler, not a built-in.

A handler responds to a block of input in the manner a `%` block does. Each `zelph/fact` it invokes constitutes a claim, regardless of whether the block was entered directly or read from a file via `.import` or `zelph/import`: a ground condition of a rule that the handler asserts becomes data, and the rule fires. A handler can import a file internally, with its lines read as the file's own, and the handler executes just once. A block that the end of input completes is dispatched like one that a blank line completes, and it is followed by the same run.

A single invocation of the handler constitutes one evaluation of a Janet block as defined in [The Scope of a Variable Symbol](#the-scope-of-a-variable-symbol-one-block): a variable symbol refers to a single variable throughout the entire call, within every `zelph/fact` the handler makes, until a `zelph/query` or a `zelph/import` that reads a file starts a new scope, and the next call of the handler starts with fresh variables.

A handler that runs the engine after its final write, such as `(zelph/run :quiet)` at its end, is not followed by the host’s run: the graph remains the fixpoint established by the handler’s run, and an additional run would yield no new derivations and merely output again, in zelph syntax, each contradiction lacking a record. Following a write or removal occurring after the handler’s run, after a single pass (`zelph/run-once`) that derived something or ran while negation records were preserved, which a single pass never re-tests, after a drop that gives a reported negation record back, and after a handler that did not run the engine at all, the host’s run follows as before; a cluster created and dropped by the handler after its run, intended to ask a question, is treated as no write. The end of input applies the identical rule.

When a handler raises an error, it is reported with the block it answered, identified by the keyword and the block's first line, with ` …` appended if further lines follow. A string error is displayed exactly as it was raised; any other error value appears in its Janet representation, prefixed by `Janet error in handler for keyword 'kw'`.

A registered block keyword shadows every statement whose first token is that same bare atom: once `kw` is registered, `kw ~ card` starts a block governed by `kw`. Enclosing the atom in quotes escapes the keyword, making `"kw" ~ card` an ordinary statement, while a node whose name duplicates a registered keyword prints quoted, ensuring its printed form re-enters as the identical statement:

```
zelph> %(zelph/register-keyword "kw" (fn [text] (if (string/find "John" text) (error "refused") (string "kw got: " text))))
zelph> kw ~ card

kw got: ~ card
zelph> "kw" ~ card
"kw" ~ card
zelph> X ~ card
Answer: "kw" ~ card
zelph> kw John is
Error in line "kw John is": refused
```

`zelph/register-keyword` features a second, three-argument variant tailored for **inline keywords** ("expression islands"): `(zelph/register-keyword open close handler)`. Each time `open` surfaces within a zelph statement (outside quoted atoms and comments), the segment up to `close`, with zelph comments already removed, is passed to `handler`, which must return a `zelph/node`; this node then replaces the island within the statement and can therefore occupy any position requiring a value – subjects, objects, nested facts, rule conditions, or consequences. Should the handler return `:incomplete`, the island extends to the subsequent `close`, enabling nested delimiters within the island's syntax to function intuitively; handlers must refrain from constructing graph structure until they have accepted their input. zelph's comment convention governs the entire statement, including the island's content: a `#` positioned at the start of a line or following a space or tab starts a comment extending to the line's end, thus preventing island grammars from employing such `#`; a `#` embedded within a token (`x#y`) or within a quoted atom reaches the handler unaltered. Variables created within an island share the scope of the enclosing statement. The components that the islands of a statement construct are considered constructed by that statement: a ground term constructed within a rule by an island, or located under a condition operator in a query, functions as a pattern, as the rest of what is written there does, while a line rejected by the parser retains none of the constructions its islands built. This is the host mechanism behind the stdlib's term islands (`$( ... )`), whose syntax is defined by a standard Janet PEG located in a `.zph` module – the language evolves through scripts, not via C++ extensions.

## Summary: zelph Syntax and Janet Equivalents

| zelph Syntax                    | Janet Equivalent                                    | Description                                                                 |
| :------------------------------ | :-------------------------------------------------- | :-------------------------------------------------------------------------- |
| `Berlin`                        | `(zelph/resolve "Berlin" "en")`                     | Resolve a name to a node, with an optional language argument                |
| `X`, `_Var`                     | `'X`, `'_Var`                                       | Variable (single uppercase letter or `_`-prefixed)                          |
| `sun is yellow`                 | `(zelph/fact "sun" "is" "yellow")`                  | Create a fact (triple)                                                      |
| `(sun is yellow)`               | `(zelph/fact "sun" "is" "yellow")`                  | Nested fact (returns relation node)                                         |
| `{ red green blue }`            | `(zelph/set "red" "green" "blue")`                  | Set constant — identified by its members, cannot be extended                |
| `@{ red green blue }`           | `(zelph/collection "red" "green" "blue")`           | Collection — own identity, membership can grow                              |
| `< Berlin Paris >`              | `(zelph/list "Berlin" "Paris")`                     | Ordered cons-list (first element is the head/outermost cons cell)           |
| `<abc>`                         | `(zelph/list-chars "abc")`                          | Compact char cons-list (LSB-first: rightmost char = outermost)              |
| `*expr`                         | `let` binding to capture and reuse a sub-expression | Focus operator                                                              |
| `,var` in zelph                 | Direct variable reference in generated code         | Unquote a Janet value (no whitespace after comma)                           |
| `% code`                        | —                                                   | Execute Janet inline (line start only; never a term)                        |
| `%` (bare)                      | —                                                   | Toggle Janet block mode                                                     |
| `X ~ human`                     | `(zelph/query (zelph/fact 'X "~" "human"))`         | Query — returns array of `@{symbol node}` tables                            |
| _(no equivalent)_               | `(zelph/exists "sun" "is" "yellow")`                | Check if a fact exists (read-only)                                          |
| _(no equivalent)_               | `(zelph/name node)`                                 | Get the name of a node as a string                                          |
| `A ~ city`                      | `(zelph/sources "~" "city")`                        | Find all subjects for a predicate–object pair                               |
| `Berlin "is located in" L`      | `(zelph/targets "Berlin" "is located in")`          | Find all objects for a subject–predicate pair                               |
| `Berlin R city`                 | _(no equivalent)_                                   | Find all predicates that connect a given Berlin and city                    |
| `S P O`                         | _(no equivalent)_                                   | List all facts in the network (use with caution on large databases)         |
| `((P) ~ negation)`              | `(zelph/negate (zelph/fact ...))`                   | Negated condition over a pattern (evaluates to the negation fact)           |
| `¬(P)`                          | `(zelph/negate P)`                                  | Negation sugar for patterns (evaluates to the negation fact)                |
| `(*{...} ~ conjunction) => ...` | `(zelph/rule [conditions] consequences...)`         | Create inference rule                                                       |
| `(cond1, cond2, cond3)`         | _(desugars to)_ set + `~ conjunction`               | Conjunction expression (comma sugar), evaluates to the conjunction set node |
| `(cond1, cond2) => cons`        | `(zelph/rule [cond1 cond2] cons)`                   | Rule using a conjunction of conditions                                      |
| `.run`                          | `(zelph/run)`                                       | Run forward chaining to a fixed point                                       |
| `.run-once`                     | `(zelph/run-once)`                                  | Run a single inference pass                                                 |
| `.run-delta`                    | `(zelph/run-delta)`                                 | Run inference seeded only by the facts added since the last run             |
| `.strata`                       | `(zelph/strata)`                                    | Array of negation levels for the rules, with a single table dedicated to each rule |
| `.negation-records on`          | `(zelph/negation-records true)`                     | Switch the negation records on; without an argument, read the switch        |
| `.cluster <name>`               | `(zelph/cluster "name")`                            | Activate a cluster; returns the active name, or `nil` for the default       |
| `.cluster-drop <name>`          | `(zelph/cluster-drop "name")`                       | Roll back everything created in the cluster; returns the node count removed |
| `.cluster` (listing)            | `(zelph/clusters)`                                  | Array of `[name node-count]` tuples                                         |
| `&42`                           | `(zelph/number "42")`                               | Number literal; delegates to the redefinable `zelph/number` hook            |
| `&`-literal display             | `(zelph/set-number-digits ["0" "1" ...])`           | Register digit alphabet; digit lists display as decimal `&`-literals        |
| `≈net(A P30 X)`                 | `(zelph/approx (zelph/fact 'A "P30" 'X) "net")`     | Neural rule condition (see [Neural Networks in the Graph](neural.md))       |
