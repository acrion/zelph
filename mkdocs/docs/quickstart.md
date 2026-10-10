# Quick Start Guide

## Installation

Choose the method that matches your operating system:

### 🐧 Linux (Arch Linux)

zelph is available in the [AUR](https://aur.archlinux.org/packages/zelph):

```bash
paru -S zelph
```

### 🐧 Linux (Debian / Ubuntu)

Download the latest `.deb` package for your architecture from [Releases](https://github.com/acrion/zelph/releases) and install it:

```bash
sudo apt install ./zelph_*_amd64.deb
```

### 🐧 Linux (Other Distributions)

Download the latest `zelph-linux-x64.zip` (for arm64: `zelph-linux-arm64.zip`) from [Releases](https://github.com/acrion/zelph/releases), extract it, and run the binary directly.
Alternatively, see [Building zelph](building.md#building-zelph) to compile from source.

### 🍏 macOS (via Homebrew)

```bash
brew tap acrion/zelph
brew install zelph
```

### 🪟 Windows (via Chocolatey)

```powershell
choco install zelph
```

## Basic Usage

Once installed, you can run zelph in interactive mode simply by typing `zelph` in your terminal.
(If you downloaded a binary manually without installing, run `./zelph` from the extraction directory).

A file of the same lines runs the same way, either as an argument or on
standard input:

```bash
zelph my-facts.zph          # runs as if the lines had been typed
zelph < my-facts.zph        # the same, read from a pipe
```

Both are a **session**, so statements are echoed, derivations are printed and
inference runs after every line. A `#!/usr/bin/env zelph` script therefore
behaves the way it looks, and it may be called anything — `report`, not
`report.zph`. Loading a file with `.import` from inside a session is the other
thing: that makes it a quiet **module**, see [Scripts and Modules](modules.md).
zelph leaves with a non-zero status when something failed, so a script can be
used in a pipeline or a Makefile.

**Organization of the transcripts found in this documentation.** After a prompt comes your input; every other line corresponds to what zelph outputs. A line that extends a partial statement, a Janet `%` block, or a keyword block such as `sparql` is considered input, even though zelph does not display a prompt before it. Four kinds of zelph output in a terminal are typically excluded: its echo of a statement you typed, the `Importing file …` and `Skipping …` messages from an `.import`, timing indicators like `-- 12 ms --`, and the banner some modules show when initialized (`math loaded: …`). A page keeps one of these when it contains meaningful content: a banner indicating the next thing to type, a timing referenced in the text, an echo discussed within the text. A full session log includes them too: the [appendix on neural networks](neural.md#appendix-complete-session-log) and the [archival SPARQL session](sparql.md#complete-example-session). When zelph’s echo of your input varies from the original in a way that matters, the transcript captures that difference. The prompt is rendered precisely as zelph prints it: `zelph+>` after a run that hid deductions, `zelph->` when auto-run is turned off – see [Deduction Output Modes](rules.md#deduction-output-modes). A transcript that leaves out any other line indicates this: `...` or `…` on a line by itself denotes missing lines, while `…` within a line indicates a segment of that line. Long messages are broken at roughly 80 columns here; zelph outputs each as a single line.

Let's try a basic example:

```
Berlin "is capital of" Germany
Germany "is located in" Europe
(X "is capital of" Y, Y "is located in" Z) => (X "is located in" Z)
```

After entering these statements, zelph will automatically infer that Berlin is located in Europe:

```
(Berlin "is located in" Europe) ⇐ {(Germany "is located in" Europe) (Berlin "is capital of" Germany)}
```

Note that none of the items used in the above statements are predefined, i.e. all are made known to zelph by these statements.
In section [Semantic Network Structure](concepts.md#semantic-network-structure) you'll find details about the core concepts, including syntactic details.

**A statement may span several lines.** zelph reads lines until it has a
complete statement — a subject, a predicate and at least one object — so a
line that stops short of that waits for the rest. Two forms are easy to type
by accident: `a p` (the object forgotten) and `(a p b)` on its own, which is a
**term**, i.e. a statement *prefix*, and is exactly how the renderer prints a
nested fact and how `.explain` takes its argument. Both wait, and the next
statement line is appended to them:

```
zelph> (a p b)
c q d
zelph> S c O
Answer: (a p b) c q
Answer: (a p b) c d
```

The query requests the predicate `c` since that is precisely what the two lines
built: the term `(a p b)` became the subject of a statement having the
predicate `c` and objects `q` and `d`.

At end of input an unfinished statement is reported (`Input ends inside an
unfinished statement: (a p b)`), and a `.`-command typed while one is pending
says which one it is.

**Two values need whitespace between them.** A line the parser cannot read at
all is refused rather than buffered, and the most frequent cause is a value
written against an opening parenthesis — the `f(x)` of ordinary mathematical
notation:

```
zelph> a b x(c d e)
Error in line "a b x(c d e)": Syntax error: Could not parse statement. "x(" is a
value glued to a "(": the grammar separates two values by whitespace, so write
"x (" if a group was meant. Function notation such as "f(x)" exists only inside
a notation island -- see ".import math-syntax".
```

**A comment can end a line.** Initiating a comment occurs by placing a `#` at the start of a line, or immediately after a space or tab, causing it to extend to the line’s termination – regardless of whether it appears after a statement or a command, as seen in `x ~ symvar   # a variable`, which asserts a single fact. A `#` embedded within a name, such as in `C#` or within the fragment of an IRI, forms part of that name, just as a `#` inside a quoted name does; a name commencing with `#` must be enclosed in quotation marks. A quoted name can continue on the next line, yet that line remains categorized by its first character: a line starting with `#` is a comment line, and one starting with `.` is a command.

## Two Statement Prefixes

Besides the dot-commands, two prefixes modify how a *statement* is read.
They are not commands and take no arguments — they attach to the statement
itself.

**`?` — ask for a result.** Most standard-library modules expose their
answer under `=`, which normally means asserting the request, letting the
fixpoint run, and querying the result separately. `?` performs all three
actions in a single line while keeping the inference pass quiet; only a
contradiction encountered is disclosed, just as it would be after any other
line:

```
zelph> .import decimal-arithmetic
zelph+> ? &12 * &34
Answer: (&12 * &34) = &408
```

It is repeatable: once the result fact exists, asking again answers from
the graph without re-deriving anything.

**`:` — the self-fact prefix.** Many requests are facts whose subject and
object are the same node, `(T simplify T)`. The prefix spells that once:
`:simplify T` *is* `(T simplify T)`, in input and in output. See
[The Self-Fact Prefix](concepts.md#the-self-fact-prefix).

The two combine, which is the usual way to drive the mathematical modules:

```
zelph> .import math
zelph+> <x> ~ polyring
(:needsring <x>) ⇐ (<x> ~ polyring)
zelph+> ? :topoly $( (x+1)^2 )
Answer: (:topoly ((x + &1) ^ &2)) = (x poly <(pos zint &1) (pos zint &2) (pos zint &1)>)
```

When a request has no answer, nothing is printed. Throughout the standard
library that is deliberate: partiality is expressed by absence, never by a
default value.

## The Standard Library

zelph ships with a standard library of scripts. When a script given to `.import` is not found at the given path, zelph searches the standard library — there, the `.zph` extension is optional:

```
.import math                 # the whole mathematics stack in one import
.import sparql               # SPARQL query interface
.import wikidata-classes     # Wikidata class hierarchy: culprits, chains, reports
.import decimal-arithmetic   # rule-based arithmetic, base 10 (+ - * / mod cmp ^)
.import binary-arithmetic    # the same, base 2 (full-adder/subtractor axioms)
.import binary-nand-arithmetic  # the same, derived from a single NAND axiom
.import primes               # primality by trial division
.import nn                   # neural network helpers
```

The three arithmetic modules are interchangeable: each claims the module ID
`arithmetic` via `.provides`, so anything built on top of arithmetic uses
whichever you imported first. See
[Mathematics](math/index.md) for the modules stacked above them.

Examples — including every script referenced throughout this documentation — live in the `examples/` subdirectory and are addressed with their subpath:

```
.import examples/english
.import examples/neural/nn-wikidata-demo
```

Search order: `$ZELPH_STDLIB` (if set) → `stdlib/` next to the zelph executable → `../share/zelph` relative to the executable (e.g. `/usr/share/zelph`) → `/usr/local/share/zelph` and `/usr/share/zelph` on Unix-like systems.

All installation methods on this page install the standard library automatically. The portable release archives contain it as a `stdlib/` directory next to the binary — keep the two together (or point `ZELPH_STDLIB` at the directory) if you relocate the binary; otherwise `.import <name>` cannot fall back to the library.

Note: some import/export examples read data files (e.g. `taxonomy.json`) from the current working directory; run those from within their examples directory or copy the data files first.

## Loading and Saving Network State

zelph allows you to save the current network state to a binary file and load it later:

```
.save network.bin          # Save the current network
.load network.bin          # Load a previously saved network
```

The `.load` command is general-purpose:

- If the file ends with `.bin`, it loads the serialized network directly (fast).
- If the file ends with `.json` or `.json.bz2` (a Wikidata dump), it imports the data and automatically creates a `.bin` cache file for future loads.

## Data Cleanup Commands

zelph provides powerful commands for targeted data removal:

- `.prune-facts <pattern>` – Eliminates solely the matching facts (statement nodes).  
  Useful for erasing particular properties without affecting the entities they belong to. A pattern lacking variables deletes precisely the single fact it identifies; one that finds no match leaves everything unchanged. A statement that is simultaneously asserted and included in a rule’s condition or consequence retains its role as the rule’s pattern: only its claim is removed, and the rule remains active.

- `.prune-nodes <pattern>` – Removes matching facts **and** the nodes bound to the pattern's variable.  
  Requirements: exactly one variable (subject or a single object), fixed relation. Two variables are rejected — the variable names what gets deleted, so there can only be one.  
  **Warning**: a deleted node takes everything it is a **part** of with it — every fact naming it, every fact naming one of those, and every rule one of them is a condition or a conclusion of — including facts and rules unrelated to the pattern, plus its names. Use with caution!

- `.prune-nodes <variable> (<conditions>)` – The same, selected by a **conjunction** whose named variable says which bindings die.  
  Any number of variables and predicates is allowed there; the other conditions are the filter that selected the victims, and their own facts survive. With a [transitive path condition](logic.md#transitive-path-conditions) this replaces a hand-written list of subclasses:  
  `.prune-nodes A (A P31 C, C P279∗ Q6999)` removes every instance of a class at or below Q6999.

Both commands remove **claims**. A statement that appears solely within a rule’s condition or consequence is classified as graph structure rather than data – queries do not answer it, and `.explain` identifies it as a rule pattern – thus, the prune commands refrain from altering it and explicitly state this. Employ `.node` to get its identifier and `.remove` if you genuinely intend to delete that structure.

- `.cleanup` – Removes all isolated nodes and cleans name mappings. The engine's core nodes (`!`, `nil`, `conjunction`, `negation`) are exempt, since they carry no edges until something uses them.

Example:

```
.lang wikidata
A P31 Q8054                 # Query all proteins
.prune-facts A P31 Q8054    # Remove only "instance of protein" statements
.prune-nodes A P31 Q8054    # Remove statements AND all protein nodes (with all their properties!)
.cleanup                    # Clean up any remaining isolated nodes
```

## Full Command Reference

Type `.help` inside the interactive session for a complete overview, or `.help <command>` for details on a specific command.

### Session

- `.help [command]` – Show this help or detailed help for a specific command
- `.quit` – Exit REPL (quits zelph)
- `.licenses` – Show third-party libraries and licenses

### Scripts, Loading & Saving

- `.import <script> [args...]` – Load and execute a zelph (.zph, optional) or Janet (.janet) script; falls back to the standard library
- `.provides <id> [id2 ...]` – Claim module IDs in the import registry
- `.load <file>` – Load a saved network (.bin) or import Wikidata JSON dump (creates .bin cache)
- `.load-partial <file|manifest> [...]` – Load selected chunks as a read-only partial view (see `.help .load-partial`)
- `.save <file.bin>` – Save the current network to a binary file
- `.save-predicates <file.bin> <predicate> [...]` – Save only the facts of the given predicates (a slice; see [Publishing a Predicate Slice](publishing-slices.md))
- `.stat-file <file.bin>` – Show serialized-file chunk statistics without loading the network
- `.index-file <file.bin> <json>` – Emit a JSON byte-offset index for a serialized .bin file

### Languages & Names

- `.lang [code]` – Show or set current language (e.g. en, de, wikidata)
- `.name <node|id> <new_name>` – Set name in current language
- `.name <node|id> <lang> <new_name>` – Set name in specific language
- `.delname <node|id> [lang]` – Delete name in current language (or specified language)

Assigning a node a name that another node already holds in that language **merges the two**, accompanied by a warning naming both – this is how one indicates, post hoc, that a manually created node and an imported entity are the same thing. A node *is* the hash of its constituent parts, so any structure depending on the vanished node is re-created under the identifier provided by its new components, and folds into an equal fact where the graph already holds one. Core nodes never vanish during this process, and a variable cannot be merged with a non-variable, nor can a collection of a rule’s text be merged with any other node.

### Exploring the Network

- `.stat` – Show network statistics (nodes, RAM usage, name entries, languages, rules)
- `.explain [<fact>] [depth]` – Reconstruct why a fact holds (proof tree; no arg: last output, 0 = unlimited depth); alias: `.why`
- `.list <count>` – List first N existing nodes (internal map order, with details)
- `.clist <count>` – List first N nodes named in current language (sorted by ID if feasible)
- `.node [<name|id|fact>]` – Show detailed node information; defaults to last output node
- `.out <name|id|fact> [count]` – List details of outgoing connected nodes (default 20)
- `.in <name|id|fact> [count]` – List details of incoming connected nodes (default 20)
- `.mermaid [<name|id|fact>] [depth] [max_neighbours]` – Generate a Mermaid HTML graph; defaults to last output node
- `.list-predicate-usage [max]` – Show predicate usage statistics (top N most frequent predicates)
- `.list-predicate-value-usage <name|id|fact> [max]` – Show object/value usage statistics for a specific predicate (top N most frequent values)

### Inference & Rules

- `.run` – Run full inference (from Janet: [`(zelph/run)`](janet.md#running-the-engine))
- `.run-once` – Run a single inference pass (from Janet: `(zelph/run-once)`)
- `.run-delta` – Run inference seeded only by the facts added since the last run; it omits the traversal of the whole graph for rules that support seeding, whereas a rule featuring a negated condition still takes a classic pass over the facts satisfying its positive conditions at every negation level, and a rule whose conditions cannot be seeded undergoes one pass in each iteration (from Janet: `(zelph/run-delta)`, see [Reasoning incrementally](janet.md#reasoning-incrementally))
- `.run-export <file>` – Run inference and write what that run derives to a JSON Lines file (see [Exporting Derivations](rules.md#exporting-derivations))
- `.auto-run` – Toggle automatic execution of .run after each input; takes no argument (default: on). Auto-run is tied to processing an input line, so a program that only calls the Janet API has to run the engine itself with `(zelph/run)`.
- `.deductions [all|focus|quiet|off]` – Set the deduction printing mode (default: quiet)
- `.list-rules` – List all defined inference rules
- `.strata` – Display the negation levels of the rules, along with those rules that cannot be stratified (refer to [Stratified Evaluation](logic.md#stratified-evaluation))
- `.remove-rules` – Remove all inference rules

### Editing & Removing

- `.remove <name|id>` – Remove a node and everything it is a part of (destructive)
- `.prune-facts <pattern>` – Remove all facts matching the query pattern (only statements)
- `.prune-nodes <pattern>` – Remove matching facts AND all involved subject/object nodes
- `.prune-nodes <var> (<conditions>)` – ... selected by a conjunction, deleting what `<var>` binds
- `.cleanup` – Remove isolated nodes and clean name mappings (core nodes exempt)
- `.new` – Clear the complete network and re-initialize the core nodes

### Clusters

- `.cluster [name]` – Show clusters, or activate one ('default' = no cluster)
- `.cluster-drop <name>` – Remove a cluster INCLUDING all nodes created in it (rollback)
- `.cluster-merge <from> <to>` – Move a cluster's membership into another ('default' = keep nodes, forget cluster)

### Wikidata

- `.wikidata-constraints <json> <dir>` – Export property constraints as zelph scripts to a directory
- `.wikidata-qualifiers <json> [P1 P2 ...]` – Import statement qualifiers from a Wikidata dump (all, or only listed qualifier properties)
- `.export-wikidata <json> <id1> [id2 ...]` – Extract exact JSON lines for Q-IDs (no import)

### Engine Behaviour

- `.parallel` – Toggle parallel processing (default: on)
- `.anchors [on|off]` – Show or set anchor-based candidate lookups in unification (default: on)
- `.semi-naive [on|off|check]` – Show or set the fixpoint evaluation strategy (default: on)
- `.fact-stores [on|off]` – Show or disable the fact-path acceleration stores (memory vs. speed)
- `.contradiction-records [on|off]` – Show or disable writing each contradiction into the graph (memory vs. repeated reports)

### Logging & Profiling

- `.log <max-depth>` – Enable detailed reasoning logging up to given recursion depth (0 = off, -1 = counters only)
- `.log-janet` – Toggle logging of Janet function calls (inputs/outputs)
- `.prof [reset]` – Dump reasoning profiler counters (requires .log -1 or .log N); 'reset' starts a fresh window

## What's Next?

- [Mathematics](math/index.md) — proving polynomial identities, symbolic differentiation, and a stack built from a single logic gate upwards
- Explore the [Core Concepts](concepts.md#core-concepts) to understand how zelph represents knowledge
- Learn about [Rules and Inference](rules.md#rules-and-inference) to leverage zelph's reasoning capabilities
- Check out the [Example Script](example-script.md#example-script) for a comprehensive demonstration
