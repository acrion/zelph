# Sharding and Partial Loading

This page documents zelph's infrastructure for loading only parts of a `.bin` file — locally, from individual shard files, or on demand from a remote host such as Hugging Face. It covers the motivation, the on-disk layout, the relevant commands, the manifest format, the production pipeline used for the published datasets, and the internal invariants that keep the different loading paths consistent.

If you only want to load a complete network, see [Precompiled Binaries](binaries.md); that page also lists the available `.bin` files and their sizes. This page assumes the sharded artifacts already exist on Hugging Face under [acrion/zelph](https://huggingface.co/datasets/acrion/zelph).

## Motivation

A complete Wikidata network is large. The current full dump (`wikidata-20260309-all.bin`) is roughly 82 GiB on disk and needs about 224 GiB of RAM to materialise fully. Even the pruned variant still needs around 15 GiB. For many uses — inspecting a handful of nodes, resolving a few names, or feeding a bounded slice of the graph into an external tool — loading the entire network is wasteful or simply impossible on the machine at hand.

Partial loading and sharding address this with three goals:

1. **Lower the cost of access.** Load only the chunks you need, cutting both RAM and load time dramatically.
2. **Enable cloud-native, on-demand access.** Host the network as many small objects on Hugging Face and fetch only the requested ranges over the network, without downloading multi-gigabyte files first.
3. **Provide a foundation for external tools.** Programmatic consumers can query specific parts of a hosted zelph graph without embedding zelph's full in-memory representation.

A partial load always produces a **read-only, incomplete graph view**. Node and name lookups, adjacency inspection (`.out`, `.in`, `.node`), and statistics (`.stat`) work normally. Operations that require the full graph — inference (`.run`), pruning, cleanup, and destructive edits — are blocked while partial mode is active.

Partial loading has been available since version 0.9.6.

## On-Disk Chunk Structure

A `.bin` file is a sequence of [Cap'n Proto](https://capnproto.org/) packed messages: one small header message followed by the chunks of four sections.

| Section      | Contents                                             |
| ------------ | ---------------------------------------------------- |
| `left`       | left-adjacency data (outgoing connections per node)  |
| `right`      | right-adjacency data (incoming connections per node) |
| `nameOfNode` | node ID → human-readable name, grouped by language   |
| `nodeOfName` | human-readable name → node ID, grouped by language   |

Each section is split into chunks of up to 1,000,000 entries. The `left` and `right` sections are keyed and ordered by node ID. The two name sections are grouped by language and ordered by their respective key: `nameOfNode` is sorted by node ID, `nodeOfName` is sorted by the name string.

The header message records the number of chunks in each section (it is read by `.stat-file`). For example, the full Wikidata file has 984 left + 984 right + 204 nameOfNode + 204 nodeOfName = 2376 chunks; the pruned file has 75 + 75 + 21 + 21 = 192 chunks.

### Chunk Index Semantics

Within each section, every chunk carries a `chunkIndex` that is **unique across the whole section** and equal to the chunk's sequential position in the file. The counter does **not** restart per language — the name sections continue counting across language boundaries. So in the pruned file, the `wikidata` name chunks occupy `nameOfNode` indices 0–13 and the `en` chunks occupy 14–20.

This invariant matters for selection: it guarantees that the selector `nameOfNode=0` refers to exactly one chunk, regardless of which loading path you use (see [Implementation Invariants](#implementation-invariants)).

Two consequences are worth keeping in mind:

- Because `nameOfNode` is sorted by node ID and `nodeOfName` by name string, the same index in the two sections covers **different** sets. A node that appears in `nameOfNode` chunk 0 is not generally resolvable through `nodeOfName` chunk 0.
- Chunk indices are **file-local**. They are not guaranteed to be stable across regenerated `.bin` files, because chunk boundaries depend on map iteration order at save time.

## Inspecting a File Without Loading It

### `.stat-file`

Reports the chunk counts of a file by reading only its header:

```
zelph> .stat-file /path/to/file.bin
Serialized File Statistics:
------------------------
File: /path/to/file.bin
File Size: 5996414847 bytes
Left Chunks: 75
Right Chunks: 75
Name-of-Node Chunks: 21
Node-of-Name Chunks: 21
Total Chunks: 192
------------------------
```

### `.index-file`

Writes a JSON byte-offset index describing the header and every chunk:

```
zelph> .index-file /path/to/file.bin /tmp/index.json
Wrote byte-offset index to /tmp/index.json
```

The output records, for the header and each chunk, its byte offset and length within the `.bin` file:

```json
{
  "file": "/path/to/file.bin",
  "header": {"offset": 0, "length": 31},
  "left":       [{"chunkIndex":0,"offset":31,"length":232195040,"which":"left"}, ...],
  "right":      [...],
  "nameOfNode": [{"chunkIndex":0,"offset":...,"length":...,"lang":"wikidata"}, ...],
  "nodeOfName": [...]
}
```

This index is the starting point for building a manifest and emitting shard files. Note that `.index-file` must walk the entire packed stream to determine each chunk's byte length, so it takes a few minutes on a large file.

## Local Partial Loading

The simplest form loads the entire file in partial (read-only) mode:

```
zelph> .load-partial /path/to/file.bin
```

To load only specific chunks, pass selectors. Each takes a comma-separated list of chunk indices:

```
zelph> .load-partial /path/to/file.bin left=0,1,2 right=5,6,9,10
```

This loads left chunks 0, 1, 2 and right chunks 5, 6, 9, 10. Sections without a selector are loaded in full. To skip a section entirely, use `none` (or `-`):

```
zelph> .load-partial /path/to/file.bin left=0,1 right=none
```

To load only the header (probabilities and counters) without any payload:

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

### Example

```
zelph> .load-partial /path/to/wikidata-20260309-all-pruned.bin left=0 right=0 nameOfNode=0 nodeOfName=0
Partial loading: left chunks=1/75, right chunks=1/75,
  nameOfNode chunks=1/21, nodeOfName chunks=1/21, skip_payload=false
...
WARNING: partial/incomplete graph loaded; reasoning, pruning, cleanup,
  and destructive edits are blocked.
zelph-> .stat
Network Statistics:
------------------------
Nodes: 1000000
RAM Usage: 2.4 GiB
Name-of-Node Entries by language:
  wikidata: 1000000
Node-of-Name Entries by language:
  wikidata: 1000000
Languages: 1
...
```

Because `nameOfNode=0` is a single section-global chunk, only one language's first chunk is loaded. To load the first chunk of each language, name both indices explicitly — in the pruned file that is `nameOfNode=0,14` — or omit the selector to load all name chunks.

## Manifest-Based Loading

A **manifest** is a JSON file describing the chunk layout of a network: where each chunk is, how large it is, and optionally where to fetch it from. This unlocks two capabilities beyond direct `.bin` loading:

1. **Seek-based access** — zelph seeks directly to a chunk's byte offset instead of scanning the file sequentially. This is faster when reading only a few chunks from a large file.
2. **Sharded storage** — each chunk is stored as an individual file (a "shard"), locally or remotely. zelph fetches only the requested shards, caches them, and loads them.

### Minimal Seek-Based Manifest

The chunk arrays from `.index-file` can be restructured into a manifest by wrapping them in a `sections` object and adding a `source` object that points to the original `.bin`:

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

`headerLengthBytes` comes from `header.length` in the index. Each chunk entry needs at minimum `chunkIndex`, `offset`, and `length`.

### Sharded Manifest (`zelph-hf-layout/v2`)

For sharded layouts, each chunk entry additionally carries an `objectPath` pointing to a separate file (a local path or a remote URL). When `objectPath` is present, zelph reads the chunk from that file instead of seeking into the source `.bin`:

```json
{
  "chunkIndex": 0,
  "length": 75535779,
  "objectPath": "hf://datasets/acrion/zelph/wikidata-20260309-all/shards/left/chunk-000000.capnp-packed",
  "which": "left"
}
```

### Generating a Sharded Manifest

The helper script `tools/emit_zelph_hf_v2.py` (Python standard library only) consumes a `.bin` plus its `.index-file` JSON and writes a `zelph-hf-layout/v2` manifest together with one shard file per chunk:

```bash
python tools/emit_zelph_hf_v2.py \
  --bin /path/to/file.bin \
  --index /tmp/index.json \
  --output /tmp/file/file.hf-v2.json \
  --artifact-name file \
  --hf-root hf://datasets/<owner>/<dataset>
```

This writes an upload-ready artifact tree under `/tmp/file/`, mirroring the layout the manifest advertises below `<hf-root>/<artifact-name>/`:

```
/tmp/file/
  file.hf-v2.json          # the manifest
  artifact.index.json      # copy of the offset index, under its advertised name
  shards/
    left/chunk-000000.capnp-packed ...
    right/chunk-000000.capnp-packed ...
    nameOfNode/chunk-000000-wikidata.capnp-packed ...
    nodeOfName/chunk-000000-wikidata.capnp-packed ...
```

Shard filenames follow `chunk-<index>.capnp-packed` for the adjacency sections and `chunk-<index>-<lang>.capnp-packed` for the name sections. Because the local tree mirrors the advertised layout, uploading `/tmp/file/` to the repo as `file` publishes exactly the paths referenced by the manifest; the tool prints the matching `hf upload` command. Overriding `--shard-root` breaks this mirror and requires manual path mapping at upload time (the tool warns in that case).

The manifest's `source.binPath` is advertised as `<hf-root>/<bin filename>` by default (override with `--bin-object-path`), so that pure-remote loads can fetch the `.bin` header without passing `source-bin=`. This assumes the source `.bin` is published at the repository root. For fully local use, pass `source-bin=<local .bin>` to `.load-partial`.

### Using a Manifest

Pass the manifest JSON as the first argument to `.load-partial`. All selectors and `meta-only` work exactly as for direct `.bin` loading:

```
zelph> .load-partial /path/to/file.hf-v2.json left=0 right=0
```

Additional options for manifest mode:

| Option              | Effect                                                            |
| ------------------- | ----------------------------------------------------------------- |
| `source-bin=<path>` | Override the `.bin` path in the manifest (used for the header)    |
| `shard-root=<path>` | Local directory containing pre-downloaded shard files             |
| `manifest=<path>`   | Explicitly specify a manifest path (alternative to the first arg) |

When chunks reference remote URLs (`hf://` or `https://`), zelph fetches them with `curl` and caches them in a temporary directory. If `shard-root` is set, zelph looks there first before downloading.

## Hosting on Hugging Face

The published sharded datasets live under [acrion/zelph](https://huggingface.co/datasets/acrion/zelph). For each artifact, the repository contains the manifest, the section shards, and the offset index, laid out as:

```
<artifact>/
  <artifact>.hf-v2.json          # the manifest
  artifact.index.json            # the byte-offset index
  shards/
    left/chunk-000000.capnp-packed ...
    right/chunk-000000.capnp-packed ...
    nameOfNode/chunk-000000-wikidata.capnp-packed ...
    nodeOfName/chunk-000000-wikidata.capnp-packed ...
```

### Loading Directly from Hugging Face

Point `.load-partial` at the manifest's `hf://` URL and request the chunks you want:

```
zelph> .load-partial hf://datasets/acrion/zelph/wikidata-20260309-all/wikidata-20260309-all.hf-v2.json left=0
```

zelph resolves the `hf://` paths to their HTTPS download URLs, fetches the manifest, fetches each requested shard, and caches everything locally so repeated loads are fast. For remote loading, the header is fetched from the source `.bin` using `source.headerLengthBytes`, so that field must be present in the manifest.

Remote cache entries are stored under a versioned `v2` directory (by default
`$TMPDIR/zelph-hf-cache/v2`). Manifest entries are revalidated through the
resolved object's ETag/repository revision before reuse. Shard and binary-range
entries are keyed by the same remote identity plus their byte range, so a
regenerated artifact cannot silently reuse ranges from an older dump. Set
`ZELPH_HF_CACHE_DIR` to choose another cache root. If metadata validation is
temporarily unavailable, an existing manifest may be reused with a diagnostic;
payload objects require a validated remote identity before reuse.

The offline decision logic is covered by `src/test/test_hf_cache.cpp`. The
network regression helper
`dev_scripts/test_hf_cache_revalidation.sh` populates a cache, tampers with a
cached manifest and its ETag, and verifies that the next remote load refetches
the manifest.

If you have already downloaded the shards (for example via `huggingface-cli download`), point `shard-root` at the local copy to skip network access:

```
zelph> .load-partial hf://datasets/acrion/zelph/wikidata-20260309-all/wikidata-20260309-all.hf-v2.json \
         shard-root=/local/cache/wikidata-20260309-all-shards left=0 right=0
```

## Route Selectors

When a manifest advertises a **node route index** — a sidecar JSON that maps node IDs and names to the chunks containing them — you can select by node or name instead of by raw chunk index:

| Selector              | Effect                                                                       |
| --------------------- | ---------------------------------------------------------------------------- |
| `route-node=<id,...>` | Resolve node IDs to the left, right, and nameOfNode chunks that contain them |
| `route-name=<name>`   | Resolve a name to the nodeOfName chunk that contains it                      |
| `route-lang=<lang>`   | Language for the route-name lookup (required with `route-name`)              |

Route selectors require manifest mode and a manifest that advertises `nodeRouteIndex` support; they can be combined with explicit chunk selectors.

```
zelph> .load-partial manifest.json route-node=1
zelph> .load-partial manifest.json route-name=A route-lang=wikidata
```

## Producing and Publishing Shards

The full pipeline that turns a `.bin` into a published, sharded artifact:

```bash
# 1. Materialise the network and write the canonical .bin.
#    The full dump needs ~224 GiB of RAM for this step.
zelph> .load /path/to/source-or-import
zelph> .save /path/to/wikidata-20260309-all.bin

# 2. Build the byte-offset index.
zelph> .index-file /path/to/wikidata-20260309-all.bin /tmp/index.json

# 3. Emit the upload-ready artifact tree (manifest, index copy, shards).
python tools/emit_zelph_hf_v2.py \
  --bin /path/to/wikidata-20260309-all.bin \
  --index /tmp/index.json \
  --output /path/to/wikidata-20260309-all/wikidata-20260309-all.hf-v2.json \
  --artifact-name wikidata-20260309-all \
  --hf-root hf://datasets/acrion/zelph

# 4. Upload the artifact tree so that the repo paths match the manifest.
#    The local directory maps to the artifact name in the repo:
hf upload acrion/zelph /path/to/wikidata-20260309-all wikidata-20260309-all --repo-type dataset
```

The repository paths must match the `objectPath` values inside the manifest exactly; uploading the tree under a different repo prefix breaks every advertised URL. The tool prints the correct `hf upload` command after emitting the tree.

Because chunk indices are file-local, the manifest and shards belong together as one immutable artifact: a manifest is only valid for the exact `.bin` it was generated from.

## Implementation Invariants

Two invariants keep the loading paths consistent and the offsets correct. They are documented here so future changes do not silently break them.

### Section-global chunk index

The two partial loaders identify chunks differently: the sequential loader selects by a chunk's position in the stream, while the manifest/shard loader selects by the `chunkIndex` value stored in the chunk. These only agree if `chunkIndex` equals the stream position — that is, if it is unique within the section. The save path therefore assigns `chunkIndex` section-globally, continuing the counter across language boundaries in the name sections. A per-language restart would give several chunks the same index and make `nameOfNode=0` mean different things on the two paths.

### Byte offsets across multi-segment messages

A Cap'n Proto message can span multiple segments; the save path uses a 512 MiB first segment, so any chunk larger than that occupies several segments. The reader behind `.index-file` only touches a chunk's root struct (which lives in segment 0), so the remaining segments stay unread until the message reader is destroyed — the destructor then skips them to position the stream at the next message. Consequently, a chunk's byte length must be measured **after** the reader has been destroyed; measuring it earlier captures only the first segment and leaves a multi-gigabyte gap before the next chunk for large files. Small files happen to stay single-segment, which is why this only surfaces at full scale.

## Limitations

- A partial load is a read-only, incomplete view. Reasoning, pruning, cleanup, and destructive edits are blocked.
- Chunk selectors are file-local and are not guaranteed to be stable across regenerated `.bin` files.
- Remote source-bin loading requires `source.headerLengthBytes` in the manifest.
- Selecting a node in one section does not imply it is resolvable through the same index in another section, because the name sections are sorted by different keys.

## Performance

Observed timings for selective chunk access on the proof-of-concept artifact at [chbwa/zelph-sharded](https://huggingface.co/datasets/chbwa/zelph-sharded):

| Access method                   | Time   |
| ------------------------------- | ------ |
| Local explicit partial load     | ~0.16s |
| Remote HF explicit partial load | ~7.9s  |
| Remote HF routed partial load   | ~5.5s  |
| Sequential fallback (same data) | ~21s   |

Remote timings depend on network conditions; the local-shard path (`shard-root`) avoids network access entirely.

## Integration with External Tools

The partial-loading and manifest infrastructure is meant not only for the interactive REPL but also as a foundation for programmatic access. [SensibLaw](https://github.com/chboishabba/SensibLaw) (part of the [ITIR-suite](https://github.com/chboishabba/ITIR-suite)) uses zelph as a downstream reasoning engine: it ingests and structures source material with full provenance, then exports bounded graph slices for zelph to reason over. With sharded manifests, such tools can query specific parts of a zelph graph hosted on Hugging Face without loading the entire network locally.

## Acknowledgments

The partial-loading and sharding infrastructure — `.load-partial`, `.stat-file`, `.index-file`, chunk selection, manifest-based loading, route selectors, remote shard support, the standalone HF v2 shard emitter, and the sharded Hugging Face proof-of-concept — was contributed by [chboishabba](https://github.com/chboishabba). Many thanks for this substantial contribution.
