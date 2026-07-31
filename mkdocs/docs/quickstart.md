### Installation

Choose the method that matches your operating system:

#### 🐧 Linux (Arch Linux)

zelph is available in the [AUR](https://aur.archlinux.org/packages/zelph):

```bash
pikaur -S zelph
```

#### 🐧 Linux (Debian / Ubuntu)

Download the latest `.deb` package for your architecture from [Releases](https://github.com/acrion/zelph/releases) and install it:

```bash
sudo apt install ./zelph_*_amd64.deb
```

#### 🐧 Linux (Other Distributions)

Download the latest `zelph-linux-x64.zip` (for arm64: `zelph-linux-arm64.zip`) from [Releases](https://github.com/acrion/zelph/releases), extract it, and run the binary directly.
Alternatively, see [Building zelph](index.md#building-zelph) to compile from source.

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

Let's try a basic example:

```
Berlin "is capital of" Germany
Germany "is located in" Europe
(X "is capital of" Y, Y "is located in" Z) => (X "is located in" Z)
```

After entering these statements, zelph will automatically infer that Berlin is located in Europe:

```
( Berlin   is located in   Europe ) ⇐ {( Germany   is located in   Europe ) ( Berlin   is capital of   Germany )}
```

Note that none of the items used in the above statements are predefined, i.e. all are made known to zelph by these statements.
In section [Semantic Network Structure](index.md#semantic-network-structure) you'll find details about the core concepts, including syntactic details.

### Two Statement Prefixes

Besides the dot-commands, two prefixes modify how a *statement* is read.
They are not commands and take no arguments — they attach to the statement
itself.

**`?` — ask for a result.** Most standard-library modules expose their
answer under `=`, which normally means asserting the request, letting the
fixpoint run, and querying the result separately. `?` does all three in one
line, and keeps the inference pass quiet:

```
zelph> .import decimal-arithmetic
zelph> ? &12 * &34
Answer: (&12 * &34) = &408
```

It is repeatable: once the result fact exists, asking again answers from
the graph without re-deriving anything.

**`:` — the self-fact prefix.** Many requests are facts whose subject and
object are the same node, `(T simplify T)`. The prefix spells that once:
`:simplify T` *is* `(T simplify T)`, in input and in output. See
[The Self-Fact Prefix](index.md#the-self-fact-prefix).

The two combine, which is the usual way to drive the mathematical modules:

```
zelph> .import math
zelph> <x> ~ polyring
zelph> ? :topoly $( (x+1)^2 )
Answer: (:topoly ((x + &1) ^ &2)) = (x poly <(pos zint &1) (pos zint &2) (pos zint &1)>)
```

When a request has no answer, nothing is printed. Throughout the standard
library that is deliberate: partiality is expressed by absence, never by a
default value.

### The Standard Library

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

### Loading and Saving Network State

zelph allows you to save the current network state to a binary file and load it later:

```
.save network.bin          # Save the current network
.load network.bin          # Load a previously saved network
```

The `.load` command is general-purpose:

- If the file ends with `.bin`, it loads the serialized network directly (fast).
- If the file ends with `.json` or `.json.bz2` (a Wikidata dump), it imports the data and automatically creates a `.bin` cache file for future loads.

### Data Cleanup Commands

zelph provides powerful commands for targeted data removal:

- `.prune-facts <pattern>` – Removes only the matching facts (statement nodes).  
  Useful for deleting specific properties without affecting the entities themselves. A pattern without variables removes exactly the one fact it names; a pattern that matches nothing changes nothing.

- `.prune-nodes <pattern>` – Removes matching facts **and** the nodes bound to the pattern's variable.  
  Requirements: exactly one variable (subject or a single object), fixed relation. Two variables are rejected — the variable names what gets deleted, so there can only be one.  
  **Warning**: a deleted node takes everything it is a **part** of with it — every fact naming it, every fact naming one of those, and every rule one of them is a condition or a conclusion of — including facts and rules unrelated to the pattern, plus its names. Use with caution!

- `.cleanup` – Removes all isolated nodes and cleans name mappings. The engine's core nodes (`!`, `nil`, `conjunction`, `negation`) are exempt, since they carry no edges until something uses them.

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

#### Session

- `.help [command]` – Show this help or detailed help for a specific command
- `.quit` – Exit REPL (quits zelph)
- `.licenses` – Show third-party libraries and licenses

#### Scripts, Loading & Saving

- `.import <script> [args...]` – Load and execute a zelph (.zph, optional) or Janet (.janet) script; falls back to the standard library
- `.provides <id> [id2 ...]` – Claim module IDs in the import registry
- `.load <file>` – Load a saved network (.bin) or import Wikidata JSON dump (creates .bin cache)
- `.load-partial <file|manifest> [...]` – Load selected chunks as a read-only partial view (see `.help .load-partial`)
- `.save <file.bin>` – Save the current network to a binary file
- `.save-predicates <file.bin> <predicate> [...]` – Save only the facts of the given predicates (a slice; see [Publishing a Predicate Slice](publishing-slices.md))
- `.stat-file <file.bin>` – Show serialized-file chunk statistics without loading the network
- `.index-file <file.bin> <json>` – Emit a JSON byte-offset index for a serialized .bin file

#### Languages & Names

- `.lang [code]` – Show or set current language (e.g. en, de, wikidata)
- `.name <node|id> <new_name>` – Set name in current language
- `.name <node|id> <lang> <new_name>` – Set name in specific language
- `.delname <node|id> [lang]` – Delete name in current language (or specified language)

#### Exploring the Network

- `.stat` – Show network statistics (nodes, RAM usage, name entries, languages, rules)
- `.explain [<fact>] [depth]` – Reconstruct why a fact holds (proof tree; no arg: last output, 0 = unlimited depth); alias: `.why`
- `.list <count>` – List first N existing nodes (internal map order, with details)
- `.clist <count>` – List first N nodes named in current language (sorted by ID if feasible)
- `.node [<name|id>]` – Show detailed node information; defaults to last output node
- `.out <name|id> [count]` – List details of outgoing connected nodes (default 20)
- `.in <name|id> [count]` – List details of incoming connected nodes (default 20)
- `.mermaid <node_name> [max_depth]` – Generate Mermaid HTML file for a node (default depth 3)
- `.list-predicate-usage [max]` – Show predicate usage statistics (top N most frequent predicates)
- `.list-predicate-value-usage <pred> [max]` – Show object/value usage statistics for a specific predicate (top N most frequent values)

#### Inference & Rules

- `.run` – Run full inference (from Janet: [`(zelph/run)`](janet.md#running-the-engine))
- `.run-once` – Run a single inference pass (from Janet: `(zelph/run-once)`)
- `.run-delta` – Run inference seeded only by the facts added since the last run; costs time in the size of the addition rather than of the graph (from Janet: `(zelph/run-delta)`, see [Reasoning incrementally](janet.md#reasoning-incrementally))
- `.run-export <file>` – Run inference and write all derivations to a JSON Lines file (see [Exporting Derivations](index.md#exporting-derivations))
- `.auto-run` – Toggle automatic execution of .run after each input; takes no argument (default: on). Auto-run is tied to processing an input line, so a program that only calls the Janet API has to run the engine itself with `(zelph/run)`.
- `.deductions [all|focus|off]` – Set the deduction printing mode (default: focus)
- `.list-rules` – List all defined inference rules
- `.remove-rules` – Remove all inference rules

#### Editing & Removing

- `.remove <name|id>` – Remove a node and everything it is a part of (destructive)
- `.prune-facts <pattern>` – Remove all facts matching the query pattern (only statements)
- `.prune-nodes <pattern>` – Remove matching facts AND all involved subject/object nodes
- `.cleanup` – Remove isolated nodes and clean name mappings (core nodes exempt)
- `.new` – Clear the complete network and re-initialize the core nodes

#### Clusters

- `.cluster [name]` – Show clusters, or activate one ('default' = no cluster)
- `.cluster-drop <name>` – Remove a cluster INCLUDING all nodes created in it (rollback)
- `.cluster-merge <from> <to>` – Move a cluster's membership into another ('default' = keep nodes, forget cluster)

#### Wikidata

- `.wikidata-constraints <json> <dir>` – Export property constraints as zelph scripts to a directory
- `.wikidata-qualifiers <json> [P1 P2 ...]` – Import statement qualifiers from a Wikidata dump (all, or only listed qualifier properties)
- `.export-wikidata <json> <id1> [id2 ...]` – Extract exact JSON lines for Q-IDs (no import)

#### Engine Behaviour

- `.parallel` – Toggle parallel processing (default: on)
- `.anchors [on|off]` – Show or set anchor-based candidate lookups in unification (default: on)
- `.semi-naive [on|off|check]` – Show or set the fixpoint evaluation strategy (default: on)
- `.fact-stores [on|off]` – Show or disable the fact-path acceleration stores (memory vs. speed)

#### Logging & Profiling

- `.log <max-depth>` – Enable detailed reasoning logging up to given recursion depth (0 = off, -1 = counters only)
- `.log-janet` – Toggle logging of Janet function calls (inputs/outputs)
- `.prof [reset]` – Dump reasoning profiler counters (requires .log -1 or .log N); 'reset' starts a fresh window

### What's Next?

- [Mathematics](math/index.md) — proving polynomial identities, symbolic differentiation, and a stack built from a single logic gate upwards
- Explore the [Core Concepts](index.md#core-concepts) to understand how zelph represents knowledge
- Learn about [Rules and Inference](index.md#rules-and-inference) to leverage zelph's reasoning capabilities
- Check out the [Example Script](index.md#example-script) for a comprehensive demonstration
