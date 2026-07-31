# Publishing a Predicate Slice

A **predicate slice** is a network cut down to the facts of a few predicates: the facts themselves, the nodes they connect, and the names of those nodes. It is an ordinary `.bin` file and behaves like any other network — but the Wikidata class hierarchy as a slice is a few hundred megabytes instead of 88 GB, which is what lets someone work with it on a laptop (see [Working on the Wikidata Class Hierarchy](class-hierarchy.md)).

This page is the operational recipe: what to run, in which order, with which file names, to produce such a slice and publish it. It is written for whoever maintains the published dataset — today one person, and the steps are recorded here so that the next request from the community ("could we have P31 as well?") does not require rediscovering them.

## Prerequisites

- A complete network as a `.bin` file. This comes out of the regular dump import (`.load wikidata-<date>-all.json.bz2` followed by `.save`), which is the expensive step and happens roughly twice a year.
- Enough memory to load that network. The slice is produced **from a loaded network**, so producing a slice of the complete dump requires the machine that can hold the complete dump. A slice of the pruned network needs 16 GiB and is the right way to try the procedure out first.
- The Hugging Face CLI, logged in: `hf auth login` (a write token for `acrion/zelph`).

## 1. Produce the slice

The predicates are named in the current language. For Wikidata that is `wikidata`, where the property nodes are the bare IDs:

Run zelph in the directory the artifacts live in, so that the slice lands
next to them:

```
$ cd ~/zelph
$ zelph
zelph> .load /home/stefan/zelph/wikidata-20260309-all-pruned.bin
...
String pool size after load: 20389119
Network loaded.
 Time needed for loading/importing: 0h1m30.799s
-- 1m30.799s --
zelph-> .lang wikidata
wikidata-> .save-predicates wikidata-20260309-all-pruned-P279.bin P279
Saving: probabilities size=0, left size=74608727, right size=74608727
Saving: name_of_node outer size=2, node_of_name outer size=2
Saving: string pool size=20389119
Saved 1193228 fact(s) of 1 predicate(s) to wikidata-20260309-all-pruned-P279.bin
-- 15.779 s --
wikidata-> .quit
```

The result is 236 MB.

Several predicates go into one file by listing them:

```
wikidata> .save-predicates wikidata-20260309-all-pruned-P279-P31.bin P279 P31
```

What this costs, measured on the pruned network (74.6 million nodes, 15.2 GiB resident): **15 seconds and 0.5 GiB on top of the loaded network**. The pass is linear in the size of the network and the extra memory is one entry per retained node, so on the complete dump expect minutes rather than seconds, and a few gigabytes on top of the 210 — plan the slice as part of the same session that already has the network loaded, not as a separate load.

Which predicates are worth slicing is a content question, not a technical one. `P279` alone answers everything about the class hierarchy, which is where the documented demand is.

### What travels with the facts

A slice is a network in its own right, not an extract, so three things come along without being asked for: the **relation-type declaration** of each named predicate (without it the loaded slice answers nothing at all), the **structure of nested facts** (a statement about a statement drags in what it is built from), and the **conjunction and negation tags** of any rule that ends up in the slice — a rule without them is still counted and still printed, but reads as a single condition and can never fire.

What does not travel is a fact of a predicate you did not name, even between two nodes the slice keeps. That is the point of slicing.

**Rules are a different matter, and worth knowing about before you rely on one.** Which rules end up in a slice is incidental: a rule travels when the closure happens to reach it, and a *contradiction* rule — whose consequence is `!`, a fact of no predicate — normally does not. A slice can therefore stop reporting a contradiction that its source reports, silently. `.save-predicates` says how many of the network's rules travelled:

```
Saved 8 fact(s) of 1 predicate(s) to leaf-slice.bin; 0 of 1 rule(s) travelled with it
```

The workflow that avoids the question altogether is the one the class-hierarchy work uses: publish the slice as **data**, and import the rules when you use it (`.import wikidata-classes`). A rule that does travel is complete and behaves exactly as it did in the source.

### File naming

Keep the name of the source network and append the predicates:

| Source network                     | Slice                                        |
| ---------------------------------- | -------------------------------------------- |
| `wikidata-20260309-all.bin`        | `wikidata-20260309-all-P279.bin`             |
| `wikidata-20260309-all-pruned.bin` | `wikidata-20260309-all-pruned-P279.bin`      |

The dump date has to stay in the name: it is what makes a reported number reproducible, and the whole point of publishing a file rather than an endpoint.

## 2. Verify before uploading

Three checks, in increasing strength. The first two take seconds.

**The slice loads and contains what it should:**

```
$ zelph
zelph> .load wikidata-20260309-all-pruned-P279.bin
Loading network from generic file wikidata-20260309-all-pruned-P279.bin...
Loading: left chunks=3, right chunks=3, nameOfNode chunks=2, nodeOfName chunks=2
...
String pool size after load: 1757443
Network loaded.
 Time needed for loading/importing: 0h0m2.836s
zelph-> .stat
Network Statistics:
------------------------
Nodes: 2152901
RAM Usage: 0.6 GiB
Name-of-Node Entries by language:
  wikidata: 959657
  en: 797786
Node-of-Name Entries by language:
  wikidata: 959657
  en: 797786
Languages: 2
Rules: 0
------------------------
```

**It answers the reference question:**

```
zelph-> .import wikidata-classes
zelph-> %(culprits "Q215627" "Q43229" 5)
below	class
14634	Q703534 (employee)
1761	Q30185
1733	Q1097498
812	Q11974939 (health professional)
46	Q12773225 (slave)
-- 81 topmost culprit(s) of 18933 affected class(es); showing 5 --
```

**It agrees with the network it came from.** This is the check that matters, and it is worth running once per dump. Put the query in a file:

```bash
cat > /tmp/culprits.zph <<'EOF'
.import sparql
sparql
PREFIX wd: <http://www.wikidata.org/entity/>
PREFIX wdt: <http://www.wikidata.org/prop/direct/>
SELECT DISTINCT ?class WHERE {
  ?class wdt:P279+ wd:Q215627 .
  ?class wdt:P279+ wd:Q43229 .
  MINUS {
    ?class wdt:P279 ?parent .
    ?parent wdt:P279+ wd:Q215627 .
    ?parent wdt:P279+ wd:Q43229 .
  }
}

EOF

run() { (echo ".load $1"; cat /tmp/culprits.zph) | zelph | grep -E '^Q[0-9]+' | sed 's/ .*//' | sort; }

diff <(run /home/stefan/zelph/wikidata-20260309-all-pruned-P279.bin) \
     <(run /home/stefan/zelph/wikidata-20260309-all-pruned.bin) \
  && echo "slice and source agree"
```

For the 2026-03-09 pruned pair this prints `slice and source agree` over 81 classes. Note that the second run loads the complete network — on the full dump, run this check only in a session that has it loaded anyway.

## 3. Do not publish the adjacency index

Loading a slice and asking a transitive question builds an adjacency index and writes it next to the file:

```
Saved adjacency index to wikidata-20260309-all-pruned-P279.bin.pidx.322 (1117131 edges).
```

That file is a **machine-local cache**: it stores raw pairs in host byte order and is validated against the exact network it was built from. It must not be uploaded — it would be wrong on a machine of different endianness and stale against any regenerated file. Rebuilding costs about 15 seconds and happens automatically on every user's first transitive question.

In `/home/stefan/zelph` this is already handled: `.gitignore` lists `*.pidx.*`, and `upload-all-to-hf.sh` derives its exclude list from git, so the mirror script skips them. A manual upload of a whole directory does not, so check first:

```bash
ls /home/stefan/zelph/*.pidx.*
```

## 4. Upload

The dataset repository is [acrion/zelph](https://huggingface.co/datasets/acrion/zelph); slices live next to the full and pruned networks at the repository root:

```bash
cd /home/stefan/zelph
hf upload acrion/zelph wikidata-20260309-all-pruned-P279.bin --repo-type dataset
```

(`upload-to-hf.sh <file>` in that directory is the same command.) A few hundred megabytes take a minute or two; the Xet backend deduplicates content, so re-uploading an unchanged file is cheap.

Verify what arrived, from a directory that does not contain the file:

```bash
cd /tmp && hf download acrion/zelph wikidata-20260309-all-pruned-P279.bin \
  --repo-type dataset --local-dir /tmp/hf-check
ls -la /tmp/hf-check
```

## 5. Update what points at the file

Three places name the artifacts, and a slice that nobody can find is not published:

1. **The dataset README** (`README.md` in `/home/stefan/zelph`, mirrored to the Hub): add the slice to the file list with its size and what it contains.
2. **[zelph.org/binaries](https://zelph.org/binaries)** — the same for the web page.
3. **[Working on the Wikidata Class Hierarchy](class-hierarchy.md)** — the hands-on page names the file in its `hf download` line and quotes concrete numbers (violation counts, culprit counts, timings). If the dump changed, those numbers changed. Re-run the session in that page and paste the actual output; the page exists to be followed literally.

A slice for a **new dump** replaces nothing: the old file stays, because published numbers refer to it. A slice for a **new predicate** is a new file under its own name.

## 6. When the community asks for something else

The mechanism is domain-agnostic — `.save-predicates` knows nothing about Wikidata, it slices whatever predicates it is given. Common requests and what they translate to:

| Request                                    | Command                                                     |
| ------------------------------------------ | ------------------------------------------------------------ |
| "Also instance-of, so we can check members" | `.save-predicates …-P279-P31.bin P279 P31`                  |
| "Part-of hierarchy"                        | `.save-predicates …-P361.bin P361`                          |
| "The disjointness statements themselves"    | Needs the qualifier layer imported first — see [Wikidata Qualifiers](qualifiers.md) — then slice `p:P2738`, `ps:P2738`, `pq:P11260` |

Two limits worth stating when answering such a request:

- A slice contains facts **of** the named predicates. A fact of another predicate between two retained nodes is not in it, by design.
- The slice is only as good as the network it is cut from. A slice of the pruned network inherits its removed domains; if the request is about biology or chemistry, it needs a slice of the complete dump.

## Checklist

```
[ ] complete network loaded, .lang wikidata set
[ ] .save-predicates <dump>-<predicates>.bin <predicates>
[ ] .stat on the slice looks plausible (node and name counts)
[ ] reference question answered (culprits Q215627 Q43229)
[ ] slice and source agree on the reference query
[ ] no .pidx.* file in the upload
[ ] hf upload
[ ] download check from a clean directory
[ ] dataset README, zelph.org/binaries, class-hierarchy.md updated
```
