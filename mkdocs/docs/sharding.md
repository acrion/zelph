# Sharding and Partial Loading

This page documents zelph's infrastructure for loading only parts of a `.bin` file — locally, from individual shard files, or on demand from a remote host such as Hugging Face. It covers the motivation, the on-disk layout, the relevant commands, the manifest format, the production pipeline used for the published datasets, and the internal invariants that keep the different loading paths consistent.

If you only want to load a complete network, see [Precompiled Binaries](binaries.md); that page also lists the available `.bin` files and their sizes. This page assumes the sharded artifacts already exist on Hugging Face under [acrion/zelph](https://huggingface.co/datasets/acrion/zelph).

## Motivation

A complete Wikidata network is large. The current full dump (`wikidata-20260309-all.bin`) is roughly 82 GiB on disk and needs about 224 GiB of RAM to materialise fully. Even the pruned variant still needs around 15 GiB. For many uses — inspecting a handful of nodes, resolving a few names, or feeding a bounded slice of the graph into an external tool — loading the entire network is wasteful or simply impossible on the machine at hand.

Partial loading and sharding address this with three goals:

1. **Lower the cost of access.** Load only the chunks you need, cutting both RAM and load time dramatically.
2. **Enable cloud-native, on-demand access.** Host the network as many small objects on Hugging Face and fetch only the requested ranges over the network, without downloading multi-gigabyte files first.
3. **Provide a foundation for external tools.** Programmatic consumers can query specific parts of a hosted zelph graph without embedding zelph's full in-memory representation.

A partial load always produces a **read-only, incomplete graph view**. Node and name lookups, adjacency inspection (`.out`, `.in`, `.node`), and statistics (`.stat`) work normally. Operations that require the full graph — inference (`.run`), pruning, cleanup, renaming, saving, and destructive edits — are blocked while partial mode is active, whether they are invoked as a command or through the Janet API.

`.import` is **not** blocked: the query layers are ordinary scripts, and `.import sparql` on a partial view is exactly what one wants. A script that only defines things (which is what a query layer does) imports silently. One that adds facts to the view says so afterwards:

```
WARNING: 'examples/english' added 239 node(s) to a partial view.
  Inference over them is blocked (.run), and the adjacency-index cache is
  disabled for this session because the graph no longer matches its file.
```

Nothing is undone — the addition is as legitimate as a typed statement, which was always allowed — but the two consequences are worth knowing, and the second one is otherwise invisible.

Partial loading has been available since version 0.9.6.

## On-Disk Chunk Structure

A `.bin` file is a sequence of [Cap'n Proto](https://capnproto.org/) packed messages: one small header message followed by the chunks of four sections.

| Section      | Contents                                             |
| ------------ | ---------------------------------------------------- |
| `left`       | left-adjacency data (outgoing connections per node)  |
| `right`      | right-adjacency data (incoming connections per node) |
| `nameOfNode` | node ID → human-readable name, grouped by language   |
| `nodeOfName` | human-readable name → node ID, grouped by language   |

Each section is split into chunks of up to 1,000,000 entries.

The two name sections are grouped by language and sorted within it: `nameOfNode` by node ID, `nodeOfName` by the name string. Their chunk boundaries are therefore key ranges, and a chunk can be located by its key.

The `left` and `right` sections are **not** sorted. They are written in the order the nodes stand in the in-memory map, which for an imported network is roughly the order in which they were created; only the adjacency list inside each entry is sorted. `left=0` thus means "the first million nodes that were created", not an ID range, and there is no way to tell from a node ID which adjacency chunk holds it — that is what a [node route index](#route-selectors) is for.

The header message records the number of chunks in each section (it is read by `.stat-file`). For example, the full Wikidata file has 984 left + 984 right + 204 nameOfNode + 204 nodeOfName = 2376 chunks; the pruned file has 75 + 75 + 21 + 21 = 192 chunks.

### Chunk Index Semantics

Within each section, every chunk carries a `chunkIndex` that is **unique across the whole section** and equal to the chunk's sequential position in the file. The counter does **not** restart per language — the name sections continue counting across language boundaries. So in the pruned file, the `wikidata` name chunks occupy `nameOfNode` indices 0–13 and the `en` chunks occupy 14–20.

This invariant matters for selection: it guarantees that the selector `nameOfNode=0` refers to exactly one chunk, regardless of which loading path you use (see [Implementation Invariants](#implementation-invariants)).

Two consequences are worth keeping in mind:

- Because `nameOfNode` is sorted by node ID and `nodeOfName` by name string, the same index in the two sections covers **different** sets. A node that appears in `nameOfNode` chunk 0 is not generally resolvable through `nodeOfName` chunk 0. In a view that loaded only one of the two directions this is visible directly: `.clist` walks `nodeOfName` and will list nodes for which `.node` — which reads `nameOfNode` — reports no name, and the other way round.
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

The manifest's `source.binPath` is advertised as `<hf-root>/<bin filename>` by default (override with `--bin-object-path`), so that pure-remote loads can fetch the `.bin` header without passing `source-bin=`. This assumes the source `.bin` is published at the repository root. A local copy of that file next to the tree is used automatically (see [Where a Chunk Is Read From](#where-a-chunk-is-read-from)); `source-bin=` is only needed for a `.bin` kept elsewhere.

### Using a Manifest

Pass the manifest JSON as the first argument to `.load-partial`. All selectors and `meta-only` work exactly as for direct `.bin` loading:

```
zelph> .load-partial /path/to/file.hf-v2.json left=0 right=0
```

Additional options for manifest mode:

| Option              | Effect                                                                        |
| ------------------- | ------------------------------------------------------------------------------ |
| `source-bin=<path>` | Override the `.bin` path in the manifest (used for the header)                |
| `shard-root=<path>` | Directory holding the shards, when they are **not** next to the manifest      |
| `manifest=<path>`   | Explicitly specify a manifest path (alternative to the first arg)             |

### Where a Chunk Is Read From

A chunk entry advertises an `objectPath`, which in the published artifacts is
an `hf://` URL. That URL says where the object lives in the repository — it
does not say that the network has to be used. zelph resolves it in this order
and takes the first hit:

1. the path as written, if it happens to be a local file;
2. relative to the **manifest's own directory** — an artifact tree keeps
   `shards/` next to the manifest, so a downloaded or freshly emitted tree
   resolves without any further option;
3. below `shard-root=`, if given;
4. the remote object, fetched with `curl` and cached.

Only step 4 touches the network, and it announces itself
(`Shard left-0 has no local copy; fetching …`). `shard-root=` is therefore
needed only when the shards were moved away from their manifest, and a purely
local artifact never reaches the network — earlier versions went straight to
step 4 whenever `shard-root=` was absent and downloaded files that were lying
next to the manifest they had just read.

The `.bin` named in `source.binPath` follows the same rule: if a file of that
name exists next to the manifest or one directory above it (the layout of the
published repository, where the `.bin` sits at the root and the artifact tree
below it), the header is read from that file instead of over the network, and
`source-bin=` is not needed. Passing `source-bin=` remains the way to point at
a `.bin` kept somewhere else.

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
temporarily unavailable, zelph may reuse the most recently cached manifest for
that exact source URI and emits a warning. Payload objects require a validated
remote identity before reuse; use `shard-root=` for deliberate fully local or
offline loading.

The offline decision logic is covered by `src/test/test_hf_cache.cpp`. The
network regression helper
`dev_scripts/test_hf_cache_revalidation.sh [manifest-uri] [zelph-binary]
[cache-root]` populates a disposable cache, tampers with a cached manifest and
its ETag, and requires the next remote header-only load to refetch the manifest
and finish successfully. It does not modify the Hub or download a shard
payload. The offline diagnostic helper
`dev_scripts/test_hf_transfer_diagnostics.sh [zelph-binary] [cache-root]`
uses a fake curl timeout and requires a URI-specific probe diagnostic.

### Remote transfer limits and diagnostics

Remote metadata probes and payload downloads use bounded `curl` calls. A
probe has a connection deadline and a short total/retry budget. A payload has
the same connection deadline, a no-progress deadline, and a byte-aware overall
budget. The latter starts from a conservative per-host throughput estimate, so
larger requested ranges receive more time without allowing a stuck process to
wait indefinitely.

Zelph records the completed call's operation, range, planned and received
bytes, cache state, remote identity, DNS/connect/TLS/first-byte/total timings,
average throughput, and outcome. A normal run prints only failures and slow
successful transfers. Set `ZELPH_HF_TRANSFER_LOG` to a file path to retain one
JSON-lines diagnostic record per call. The cache root retains a compact
per-host throughput estimate from meaningful payloads for future byte-aware
budgets.

The defaults can be tuned for unusual networks:

| Variable | Default | Meaning |
| --- | ---: | --- |
| `ZELPH_HF_CONNECT_TIMEOUT_SECONDS` | `15` | Maximum connection setup time per curl attempt. |
| `ZELPH_HF_PROBE_TIMEOUT_SECONDS` | `45` | Total retry budget for a metadata probe. |
| `ZELPH_HF_STALL_WINDOW_SECONDS` | `90` | Time below the minimum transfer rate before a payload is stalled. |
| `ZELPH_HF_MIN_PROGRESS_BYTES_PER_SECOND` | `1024` | Minimum sustained payload rate used for the stall check. |
| `ZELPH_HF_INITIAL_THROUGHPUT_BYTES_PER_SECOND` | `262144` | Initial per-host estimate before successful history exists. |

The first implementation uses curl's end-of-call metrics and its native
low-speed abort. It can classify a completed slow transfer and a curl-detected
stall, but it does not yet sample live byte progress. A monitored-subprocess
transport layer is the later extension for instantaneous throughput and exact
last-progress timestamps; its measurements will also inform shard sizing and
the planned progressive query/join executor.

If you have already downloaded the artifact (for example via
`huggingface-cli download`), address the **local** manifest instead of the
`hf://` one — the shards next to it are then read from disk and nothing goes
over the network:

```
zelph> .load-partial /local/wikidata-20260309-all-pruned/wikidata-20260309-all-pruned.hf-v2.json left=0 right=0
```

Only if the shards were separated from their manifest does the location have
to be spelled out:

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

Route selectors require manifest mode and a manifest that advertises `nodeRouteIndex` support; they can be combined with explicit chunk selectors, and the two selections are unioned.

```
zelph> .load-partial manifest.json route-node=1
zelph> .load-partial manifest.json route-name=A route-lang=wikidata
```

A node route and a name route answer different questions, and the difference is
not a limitation but the layout: `nameOfNode` is sorted by node ID, `nodeOfName`
by name string. `route-node` therefore selects adjacency and the node's own
name; `route-name` selects the one chunk in which that name can be found. A
name route alone gives a view in which the name resolves to a node ID while the
node carries no names — enough to route, not enough to display. Ask for both
when you want both.

Chunk selectors are file-local, and so are node IDs: both are assigned when the
`.bin` is written. `route-name` is the only selector that survives a
regenerated network, because names are the one identifier the graph carries
itself.

### The Sidecar Format

The route index is a JSON file listing, per section, which chunk holds which
keys. Nothing generates it yet, so it is written by whoever produces the
artifact:

```json
{
  "routing": {
    "left":       [{"chunkIndex": 0, "nodes": [11, 12]}],
    "right":      [{"chunkIndex": 0, "nodes": [11, 12]}],
    "nameOfNode": [{"chunkIndex": 0, "nodes": [11, 12]}],
    "nodeOfName": [{"chunkIndex": 0, "lang": "zelph", "names": ["alpha", "beta"]},
                   {"chunkIndex": 1, "lang": "de",    "names": ["alpha_de"]}]
  }
}
```

Every field shown is required for the entries of that section; an entry with a
missing `chunkIndex`, `nodes`, `lang` or `names` is an error rather than a
skipped line. Sections may be omitted entirely — a sidecar that only routes
names is valid. Listing only the keys that callers are expected to route by is
also valid, and is what keeps the sidecar small: a route selector that resolves
no chunk at all is reported as an error, so an incomplete index shows up as
such instead of loading a wrong subset.

The manifest points at the file and declares the capability:

```json
{
  "selectorModel": {"supportedOperations": ["header-probe", "selected-chunk-read", "node-route"]},
  "hfObjects": {"nodeRouteIndex": {"path": "hf://datasets/<owner>/<repo>/<artifact>/net.route.json"}}
}
```

`localPath` may be used instead of `path` for a purely local artifact. As for
the shards, an advertised remote path is resolved against the local tree first
(see [Where a Chunk Is Read From](#where-a-chunk-is-read-from)), so a
downloaded artifact routes without touching the network.

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

What a manifest buys locally, measured on the published pruned artifact
(`wikidata-20260309-all-pruned`, 75 left chunks, 6.0 GB) by loading its first
left chunk and nothing else, alternating the two commands over two rounds:

| Command                                                                       | Time            |
| ----------------------------------------------------------------------------- | --------------- |
| `.load-partial …-pruned.bin left=0 right=none nameOfNode=none nodeOfName=none` | 7.05 s / 7.10 s |
| `.load-partial …-pruned.hf-v2.json left=0 right=none …` (shards on disk)       | 1.07 s / 1.03 s |

Both produce the same 1,000,000-node view. The difference is the packed stream:
the `.bin` path walks it from the beginning, the manifest path seeks — or, with
shards, opens one small file.

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
