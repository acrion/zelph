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

Download the latest `zelph-linux.zip` from [Releases](https://github.com/acrion/zelph/releases), extract it, and run the binary directly.
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

- `.help [command]` – Show help
- `.exit` – Exit interactive mode
- `.lang [code]` – Show or set current language (e.g., `en`, `de`, `wikidata`)
- `.name <node|id> <new_name>` – Set node name in current language
- `.name <node|id> <lang> <new_name>` – Set node name in specific language
- `.delname <node|id> [lang]` – Delete node name in current (or specified) language
- `.node <name|id>` – Show detailed node information (names, connections, representation, Wikidata URL)
- `.list <count>` – List first N existing nodes (internal order, with details)
- `.clist <count>` – List first N nodes named in current language (sorted by ID if feasible)
- `.out <name|id> [count]` – List outgoing connected nodes (default: 20)
- `.in <name|id> [count]` – List incoming connected nodes (default: 20)
- `.mermaid <name> [depth]` – Generate Mermaid HTML file for a node (default depth 3)
- `.run` – Full inference
- `.run-once` – Single inference pass
- `.run-md <subdir>` – Inference + Markdown export
- `.run-file <file>` – Inference + write deduced facts to file (compressed if wikidata)
- `.decode <file>` – Decode a file produced by `.run-file`
- `.list-rules` – List all defined rules
- `.list-predicate-usage [max]` – Show predicate usage statistics (top N most frequent)
- `.list-predicate-value-usage <pred> [max]` – Show object/value usage statistics (top N most frequent values)
- `.remove-rules` – Remove all inference rules
- `.remove <name|id>` – Remove a node (destructive: disconnects all edges and cleans names)
- `.import <file.zph>` – Load and execute a zelph script
- `.load <file>` – Load saved network (.bin) or import Wikidata JSON (creates .bin cache)
- `.save <file.bin>` – Save current network to binary file
- `.prune-facts <pattern>` – Remove all facts matching the query pattern (only statements)
- `.prune-nodes <pattern>` – Remove matching facts AND all involved subject/object nodes
- `.cleanup` – Remove isolated nodes
- `.stat` – Show network statistics (nodes, RAM usage, name entries, languages, rules)
- `.licenses` – Show third-party libraries and licenses
- `.log <max-depth>` – Enable detailed reasoning logging up to given recursion depth (0 = off, -1 = only statistics)
- `.auto-run` – Toggle automatic execution of `.run` after each input (default: on)
- `.wikidata-constraints <json> <dir>` – Export property constraints as zelph scripts
- `.export-wikidata <json> <id1> [id2 ...]` – Extracts exact JSON lines for Q-IDs (no import)

### What's Next?

- Explore the [Core Concepts](index.md#core-concepts) to understand how zelph represents knowledge
- Learn about [Rules and Inference](index.md#rules-and-inference) to leverage zelph's reasoning capabilities
- Check out the [Example Script](index.md#example-script) for a comprehensive demonstration
