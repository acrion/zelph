#!/usr/bin/env python3
"""Build a Wikidata-dump-shaped JSON slice of all items carrying P2738
("disjoint union of") statements.

The file this writes can be fed to zelph's `.wikidata-qualifiers` command,
which expects one compact entity JSON object per line inside the usual dump
brackets. It exists so that the disjointness use case of
arXiv:2410.13707 can be demonstrated live on real Wikidata definitions
without importing a full 100 GB dump.

Usage:
    python3 dev_scripts/fetch_p2738_slice.py OUTPUT.json
"""

import json
import sys
import time
import urllib.parse
import urllib.request

UA = "zelph-demo-prep/1.0 (https://github.com/acrion/zelph)"
WDQS = "https://query.wikidata.org/sparql"
API = "https://www.wikidata.org/w/api.php"

QUERY = "SELECT DISTINCT ?c WHERE { ?c p:P2738 ?st . }"


def get(url, params, accept):
    full = url + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(full, headers={"User-Agent": UA, "Accept": accept})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read().decode("utf-8"))


def qids():
    data = get(WDQS, {"query": QUERY, "format": "json"},
               "application/sparql-results+json")
    out = []
    for row in data["results"]["bindings"]:
        out.append(row["c"]["value"].rsplit("/", 1)[-1])
    return sorted(out, key=lambda q: int(q[1:]))


def entities(batch):
    data = get(API, {"action": "wbgetentities", "ids": "|".join(batch),
                     "props": "claims|labels", "languages": "en",
                     "format": "json"}, "application/json")
    return data.get("entities", {})


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    target = sys.argv[1]

    ids = qids()
    print(f"{len(ids)} items carry P2738 statements", file=sys.stderr)

    lines = []
    for i in range(0, len(ids), 50):
        batch = ids[i:i + 50]
        got = entities(batch)
        for qid in batch:
            e = got.get(qid)
            if not e or "claims" not in e:
                continue
            # Mirror the dump's entity shape: type and id first, then labels
            # and claims. zelph reads the first "id":"..." of the line as the
            # entity ID, so nothing may precede it.
            slim = {"type": e.get("type", "item"), "id": qid,
                    "labels": e.get("labels", {}), "claims": e["claims"]}
            lines.append(json.dumps(slim, separators=(",", ":"),
                                    ensure_ascii=False))
        print(f"  fetched {min(i + 50, len(ids))}/{len(ids)}", file=sys.stderr)
        time.sleep(0.2)

    with open(target, "w", encoding="utf-8") as f:
        f.write("[\n")
        f.write(",\n".join(lines))
        f.write("\n]\n")
    print(f"wrote {len(lines)} entities to {target}", file=sys.stderr)


if __name__ == "__main__":
    main()
