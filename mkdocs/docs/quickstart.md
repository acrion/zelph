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
X is capital of Y, Y is located in Z => X is located in Z
```

After entering these statements, zelph will automatically infer that Berlin is located in Europe:

```
«Berlin» «is located in» «Europe» ⇐ («Germany» «is located in» «Europe»), («Berlin» «is capital of» «Germany»)
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

### Importing Wikidata

zelph can import and process data [from Wikidata](https://dumps.wikimedia.org/wikidatawiki/entities/):

```
# Within the zelph CLI
.wikidata path/to/wikidata-dump.json
```

For more details on Wikidata integration, see [Working with Wikidata](wikidata.md).

### What’s Next?

- Explore the [Core Concepts](#core-concepts) to understand how zelph represents knowledge
- Learn about [Rules and Inference](#rules-and-inference) to leverage zelph’s reasoning capabilities
- Check out the [Example Script](#example-script) for a comprehensive demonstration

