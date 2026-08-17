# Binary Data Files

Here, we will regularly publish precompiled `.bin` files for zelph that you can load and use directly. These files contain prepared semantic networks, mainly based on Wikidata data, but also on other domains. The focus is on efficiency: compared with JSON files, which can take hours to read, `.bin` files load in just a few minutes, depending on the hardware.

I plan to upload new `.bin` files regularly based on current Wikidata dumps (see [Wikidata Dumps](https://dumps.wikimedia.org/wikidatawiki/entities/) for transparency), and also to provide files for other data sources in the future.

## Available Files

All `.bin` files are available on [Hugging Face](https://huggingface.co/datasets/acrion/zelph).

Currently, I offer the following Wikidata variants:

| File                                      | Variant                                                        |       Nodes | File Size | RAM Usage | Name Entries (`wikidata` / `en`) | Load Time |
| ----------------------------------------- | -------------------------------------------------------------- | ----------: | --------: | --------: | -------------------------------: | --------: |
| `wikidata-20260309-all.bin`               | Current full Wikidata dump, a 1:1 port of the JSON dump        | 983,424,620 |    82 GiB | 223.7 GiB |         119,231,266 / 83,261,799 |   23m 23s |
| `wikidata-20260309-all-pruned-medium.bin` | Pruned, **keeps people**                                       | 114,477,445 |   9.0 GiB |  25.9 GiB |          21,015,182 / 14,668,496 |    2m 08s |
| `wikidata-20260309-all-pruned-small.bin`  | Pruned further, no people — fits an ordinary laptop            |  26,533,048 |   2.2 GiB |   6.0 GiB |            7,106,526 / 4,316,673 |       25s |
| `wikidata-20171227.bin`                   | Historic full Wikidata dump from 2017                          | 203,190,311 |    18 GiB |  44.6 GiB |          42,187,613 / 27,960,315 |    3m 20s |
| `wikidata-20171227-pruned.bin`            | Historic pruned Wikidata dump from 2017                        |  17,407,259 |   1.4 GiB |   3.8 GiB |            4,307,749 / 2,324,957 |     14.0s |

The values above reflect observed loading statistics from zelph on my system. Actual loading times and memory usage may vary depending on hardware and build configuration.

### Which one do you want?

**RAM decides, and it is the RAM column, not the file size.** A `.bin` roughly
triples when it is loaded, because the graph is reconstructed as adjacency maps and
name tables rather than mapped from the file.

* **`-small`** (6.0 GiB) is the one to begin with. It loads in 25 seconds and
  leaves room to work on a 16 GiB machine; on 8 GiB it fits, though not with
  much to spare.
* **`-medium`** (25.9 GiB) is the same network with **people** still in it, and
  wants 32 Gi-GB.
* The **full** file is the faithful port of the Wikidata dump and needs a
  machine built for it – 223.7 GiB resident. It is the right choice when you
  need everything, and the wrong one for anything else.

**What the pruned variants drop** – in the order they were removed, least
missed first: the encyclopedia’s own plumbing (categories, templates, list and
disambiguation pages, one item per integer and per calendar day), astronomical
catalogues, sequence databases and chemistry, individual museum objects,
publications and their citation graph, everything filed under an
administrative entity, everything carrying a country, and – in `-small` only –
people.

**What they retain, intentionally: the class hierarchy.** Removing instances never
eliminates the classes they are instances of, so both pruned versions respond
to class-level inquiries identically. The disjointness query of
[Working on the Wikidata Class Hierarchy](class-hierarchy.md) returns the same
81 topmost culprits on `-medium` and on `-small`; they vary only in which
individual items they still include.

## Loading a Full Network

To load a `.bin` file in zelph, start zelph in interactive mode and use the command:

```zelph
.load /path/to/file.bin
```

This loads the entire network into memory. Afterwards, you can execute queries, define rules, or start inference (e.g. with `.run`). For Wikidata-specific work, you need to adjust the language to use Q-item names etc:

```zelph
.lang wikidata
```

Tip: if you work with the full JSON file, zelph automatically creates a `.bin` cache file during the first import to speed up future runs.

In addition, **qualifier-extended variants** are available (currently `wikidata-20260309-all-P11260.bin`, containing the statement/qualifier layer needed for disjointness analysis). See [Wikidata Qualifiers](qualifiers.md) for details; additional qualifier sets are available on request.

## Partial Loading

Since version 0.9.6, zelph supports **partial loading** of `.bin` files. Instead of materialising the entire network into RAM, you can selectively load individual chunks, significantly reducing memory usage and load time. This is especially useful for inspecting or querying large networks on machines that cannot hold the full graph in memory, or for external tools that need targeted access to specific parts of the graph without loading the whole dataset.

A partial load produces a **read-only, incomplete graph view**. Node and name lookups, adjacency inspection (`.out`, `.in`, `.node`), and statistics (`.stat`) work normally. Operations that require the full graph — inference (`.run`), pruning, cleanup, and destructive edits — are blocked while partial mode is active.

### Chunk Structure of `.bin` Files

A `.bin` file is internally organised into four sections of numbered chunks:

- **left** — left-adjacency data (outgoing connections per node)
- **right** — right-adjacency data (incoming connections per node)
- **nameOfNode** — maps from node IDs to human-readable names, grouped by language
- **nodeOfName** — maps from human-readable names to node IDs, grouped by language

Each section is divided into multiple chunks. For example, the pruned Wikidata 2026 file contains 75 left chunks, 75 right chunks, 21 nameOfNode chunks, and 21 nodeOfName chunks (192 chunks total).

### Inspecting a `.bin` File Before Loading

Use `.stat-file` to get a quick overview of a file's chunk counts without loading it:

```
zelph> .stat-file /path/to/wikidata-20260309-all-pruned-small.bin
Serialized File Statistics:
------------------------
File: /path/to/wikidata-20260309-all-pruned-small.bin
File Size: 2367565351 bytes
Left Chunks: 27
Right Chunks: 27
Name-of-Node Chunks: 13
Node-of-Name Chunks: 13
Total Chunks: 80
------------------------
(declared by the header; use .index-file to verify the chunks)
```

This reads the header and nothing else, which is what makes it instant on an 88 GB file — and is also its limit: a file that was cut off after the header still reports the counts the header declares. Only a header whose counts could not fit in the file at all is refused. A file that is not a serialized network, or is empty, is refused by name rather than with a Cap'n Proto backtrace.

Use `.index-file` to generate a detailed JSON byte-offset index for every chunk:

```
zelph> .index-file /path/to/file.bin /tmp/index.json
Wrote byte-offset index to /tmp/index.json
```

The resulting JSON file lists the byte offset and length of each chunk within the `.bin` file. This is useful for understanding the internal layout and serves as the starting point for creating manifest files (see [Manifest-Based Loading](#manifest-based-loading) below). Because it reads every chunk, it is also the command that notices a truncated or corrupted file — at the cost of one pass over the whole thing.

### Basic Partial Loading from a Local `.bin` File

The simplest form loads the entire file in partial mode (all chunks, but with destructive operations blocked):

```
zelph> .load-partial /path/to/file.bin
```

To load only specific chunks, use selectors. Each selector takes a comma-separated list of chunk indices:

```
zelph> .load-partial /path/to/file.bin left=0,1,2 right=5,6,9,10
```

This loads only left-adjacency chunks 0, 1, and 2 and right-adjacency chunks 5, 6, 9, and 10. All nameOfNode and nodeOfName chunks are loaded by default (since no selector was given for them). To explicitly skip a section, use `none` (or `-`):

```
zelph> .load-partial /path/to/file.bin left=0,1 right=none
```

This loads only two left-adjacency chunks and no right-adjacency data at all; name maps are still loaded in full.

To load only the file header (metadata such as probabilities and counters) without any chunk payloads:

```
zelph> .load-partial /path/to/file.bin meta-only
```

### Selector Reference

| Selector             | Effect                                                      |
| -------------------- | ----------------------------------------------------------- |
| `left=0,1,2`         | Load only left-adjacency chunks 0, 1, and 2                 |
| `right=5,6`          | Load only right-adjacency chunks 5 and 6                    |
| `nameOfNode=0,1`     | Load only name-of-node chunks 0 and 1 (alias: `name=`)      |
| `nodeOfName=0,1`     | Load only node-of-name chunks 0 and 1 (alias: `node-name=`) |
| `<section>=none`     | Skip that section entirely (also accepts `-`)               |
| _(selector omitted)_ | Load all chunks of that section                             |
| `meta-only`          | Load only the header; skip all chunk payloads               |

A selector naming a chunk the file does not have is refused before anything is loaded, so the network you already had is still there — use `.index-file` (or `.stat-file` for the counts alone) to find the valid indices. A load that fails *while reading*, on a truncated or corrupted file, is a different matter: the previous graph has been discarded by then and the message says so, with `.new` as the way back.

### Practical Example

The following session loads a subset of the pruned Wikidata file and inspects it:

```
zelph> .load-partial /path/to/wikidata-20260309-all-pruned-small.bin left=0,1,2 right=5,6,9,10
Partial loading: left chunks=3/27, right chunks=4/27,
  nameOfNode chunks=13/13, nodeOfName chunks=13/13, skip_payload=false
String pool size after partial load: 11423140
WARNING: partial/incomplete graph loaded; reasoning, pruning, cleanup,
  and destructive edits are blocked.
 Time needed for partial loading: 0h0m16.968s
zelph-> .stat
Network Statistics:
------------------------
Nodes: 3000000
RAM Usage: 2.6 GiB
...
```

With only 7 out of 54 adjacency chunks loaded, RAM usage dropped from 6.0 GiB to 2.6 GiB. The floor is the name maps: they are loaded whole here, and at 7.1 M `wikidata` plus 4.3 M `en` entries they are most of what remains. `nameOfNode=none` drops them as well, at the cost of being unable to look a node up by name.

## Manifest-Based Loading

For advanced use cases — especially when hosting `.bin` data remotely or as pre-split shard files — zelph supports loading via a **manifest file**. A manifest is a JSON file that describes the chunk layout of a `.bin` file: where each chunk is located, how large it is, and optionally where to fetch it from.

This enables two key capabilities beyond direct `.bin` loading:

1. **Seek-based access**: Instead of scanning through the `.bin` file sequentially, zelph can seek directly to the byte offset of each requested chunk. This is faster when loading only a few chunks from a large file.

2. **Sharded storage**: Each chunk can be stored as an individual file (a "shard"), either locally or on a remote host such as Hugging Face. The manifest maps chunk indices to file paths or URLs. zelph fetches only the chunks you request, caches them locally, and loads them.

### Creating a Manifest

The starting point is the JSON index generated by `.index-file`:

```
zelph> .index-file /path/to/file.bin /tmp/index.json
```

This produces a file containing byte offsets and lengths for the header and every chunk. To turn this into a manifest, you need to restructure it: wrap the chunk arrays under a `sections` object and add a `source` object pointing to the original `.bin` file. A minimal manifest looks like this:

```json
{
  "source": {
    "binPath": "/path/to/file.bin",
    "headerLengthBytes": 31
  },
  "sections": {
    "left":       {"chunks": [{"chunkIndex": 0, "offset": 31, "length": 232195040}, ...]},
    "right":      {"chunks": [...]},
    "nameOfNode": {"chunks": [...]},
    "nodeOfName": {"chunks": [...]}
  }
}
```

To generate a ready-to-use `zelph-hf-layout/v2` manifest plus individual shard files automatically, use the helper script in `tools/emit_zelph_hf_v2.py`:

```bash
python tools/emit_zelph_hf_v2.py \
  --bin /path/to/file.bin \
  --index /tmp/index.json \
  --output /tmp/file/file.hf-v2.json \
  --artifact-name file \
  --hf-root hf://datasets/<owner>/<dataset>
```

This writes an upload-ready artifact tree under `/tmp/file/`:

- the manifest `file.hf-v2.json`
- a copy of the offset index under its advertised name `artifact.index.json`
- one shard object per section-local chunk under `shards/`

The tree mirrors the layout advertised in the manifest, so uploading `/tmp/file/` to the repo as `file` (the tool prints the matching `hf upload` command) makes it consumable via `.load-partial manifest.json ...`.

The `headerLengthBytes` value comes from the `header.length` field in the index output. Each chunk entry needs at minimum `chunkIndex`, `offset` (byte offset into the source `.bin`), and `length`.

For sharded layouts where each chunk is stored as a separate file, each chunk entry can additionally contain an `objectPath` field pointing to a local file path or a remote URL. When `objectPath` is present, zelph reads the chunk from that file instead of seeking into the source `.bin`. The manifest version `zelph-hf-layout/v2` is used for this mode. A sharded chunk entry looks like:

```json
{
  "chunkIndex": 0,
  "length": 75535779,
  "objectPath": "hf://datasets/chbwa/zelph-sharded/minimal-proof/shards/left/chunk-000000.capnp-packed",
  "which": "left"
}
```

### Using a Manifest

Pass the manifest JSON as the first argument to `.load-partial`:

```
zelph> .load-partial /path/to/manifest.json
```

All chunk selectors (`left=`, `right=`, etc.) and `meta-only` work the same way as with direct `.bin` loading.

Additional options for manifest mode:

| Option              | Effect                                                                 |
| ------------------- | ---------------------------------------------------------------------- |
| `source-bin=<path>` | Override the `.bin` path specified in the manifest (for the header)    |
| `shard-root=<path>` | Local directory containing pre-downloaded shard files                  |
| `manifest=<path>`   | Explicitly specify a manifest path (alternative to the first argument) |

When chunks reference remote URLs (`hf://` or `https://`), zelph fetches them automatically using `curl` and caches them in a temporary directory. If `shard-root` is set, zelph first looks for matching files there before attempting a remote download.

Manifests can also be loaded directly from Hugging Face:

```
zelph> .load-partial hf://datasets/chbwa/zelph-sharded/20260309-v3/manifest.json
```

### Route Selectors

When a manifest provides a **node route index** (a sidecar JSON file that maps node IDs and names to chunk indices), you can use route selectors instead of specifying chunk indices manually. This is the most convenient way to load only the data relevant to a specific node or name.

| Selector              | Effect                                                                       |
| --------------------- | ---------------------------------------------------------------------------- |
| `route-node=<id,...>` | Resolve node IDs to the left, right, and nameOfNode chunks that contain them |
| `route-name=<name>`   | Resolve a name to the nodeOfName chunk that contains it                      |
| `route-lang=<lang>`   | Language for the route-name lookup (required with `route-name`)              |

Route selectors require manifest mode and a manifest that advertises `nodeRouteIndex` support. They can be combined with explicit chunk selectors.

Example — load only the chunks that contain node ID 1:

```
zelph> .load-partial manifest.json route-node=1
```

Example — load only the nodeOfName chunk containing name "A" in language "wikidata":

```
zelph> .load-partial manifest.json route-name=A route-lang=wikidata
```

### Sharded Files on Hugging Face

Proof-of-concept sharded zelph storage on Hugging Face is available at [chbwa/zelph-sharded](https://huggingface.co/datasets/chbwa/zelph-sharded). This repository demonstrates the v2 and v3 manifest formats with individually stored shard files that can be fetched on demand, including both explicit chunk selection and route-based selection.

Observed performance for selective chunk access (on the v3 proof artifact):

| Access method                    | Time   |
| -------------------------------- | ------ |
| Local explicit partial load (v3) | ~0.16s |
| Remote HF explicit partial load  | ~7.9s  |
| Remote HF routed partial load    | ~5.5s  |
| Sequential fallback (same data)  | ~21s   |

Loading directly from Hugging Face:

```
zelph> .load-partial hf://datasets/chbwa/zelph-sharded/20260309-v3/manifest.json left=0
zelph> .load-partial hf://datasets/chbwa/zelph-sharded/20260309-v3-route/manifest.json route-node=7009581169707405312
```

We are working on publishing pre-sharded versions of the full available `.bin` files along with ready-to-use manifest files. The long-term goal is to enable partial loading of Wikidata networks directly from the cloud without downloading entire multi-gigabyte files first.

### Integration with External Tools

The partial loading and manifest infrastructure is designed not only for interactive use in the zelph REPL, but also as a foundation for programmatic access by external tools. For example, [SensibLaw](https://github.com/chboishabba/SensibLaw) (part of the [ITIR-suite](https://github.com/chboishabba/ITIR-suite)) uses zelph as a downstream reasoning engine: it ingests and structures source material with full provenance, then exports bounded graph slices for zelph to reason over. With partial loading and sharded manifests, such tools can query specific parts of a zelph graph hosted on Hugging Face without needing to load the entire network locally.

## The `.pidx` Sidecar Files

After a transitive query, zelph generates a companion file adjacent to the `.bin`,
named `<file>.bin.pidx.<number>`, for example
`wikidata-20260309-all-pruned-small.bin.pidx.322`. The number is the node id of
a predicate – 322 is [P279](https://www.wikidata.org/wiki/Property:P279), *subclass of* – and the file contains the **persisted transitive closure** of that
predicate.

**It is a cache, not data.** Nothing within it is irrecoverable from the `.bin`
itself, and the engine reconstitutes it automatically whenever it is absent or was
generated by an earlier format version. Removing one incurs time, never
information. What it gains is precisely that time: the first `p+` or `p*` query
over a large hierarchy must walk the closure once, and on a full dump that is
substantial.

Three consequences worth knowing:

* **It is not part of the download, deliberately.** The file contains raw pairs
  in *host byte order* and is verified against the precise network it was constructed
  from, so it would be invalid on a machine with different endianness and outdated
  relative to any re-generated file. Published datasets thus distribute the `.bin`
  alone; see [Publishing Slices](publishing-slices.md) for the upload rule.
  Your own copy appears on the first transitive question and takes roughly
  fifteen seconds to build.
* **It is bound to the file it sits adjacent to.** A closure derived from one
  network fails to represent another, so the sidecar is only applicable to the
  `.bin` whose name it appends.
* **A version bump silently discards old sidecars.** The format version was
  incremented to 2 on 3 August 2026; files produced prior to that are disregarded and
  reconstructed instead of being misinterpreted.

## Generation of the Pruned Files

The pruned versions above were created by loading the full dump and removing
large knowledge domains from it, in phases, saving after each one. The removal
works in two ways, and the difference matters:

* **by class** – `.prune-nodes A P31 Q13442814` removes the direct instances of
  a class (here: scholarly articles). It does *not* traverse the class
  hierarchy, so the class named must have actual instances.
* **by property** – `.prune-nodes A (A P1433 B)` eliminates every **subject** of a
  predicate (here: everything with a *published in* statement). This proved to be the far more powerful mechanism: that single line erased 544 million nodes, because the cascade removes every fact hanging off each deleted item, and it reached the publication domain more thoroughly than naming its classes did.

Each phase concludes with `.save`, followed by a reload, since a removal fails to reduce
the resident set independently – the containers retain their capacity and the string
pool is append-only, so only loading the saved file compacts it. A final
`.cleanup` eliminates the nodes left isolated by deletions (~1.7 million in each
of the two published variants).

The programs responsible for generating the released files are
[`dev_scripts/prune-full-dump-phased.zph`](https://github.com/acrion/zelph/tree/main/dev_scripts)
and `prune-full-dump-phased-2.zph`. Previous log entries from earlier versions are
stored at
[https://github.com/acrion/zelph/tree/main/logs](https://github.com/acrion/zelph/tree/main/logs);
note that they record a distinct, earlier pruning operation and not the files
listed above.

## Acknowledgments

The partial loading infrastructure — `.load-partial`, `.stat-file`, `.index-file`, chunk selection, manifest-based loading, route selectors, remote shard support, and the sharded Hugging Face proof-of-concept — was contributed by [chboishabba](https://github.com/chboishabba). Many thanks for this substantial contribution!
