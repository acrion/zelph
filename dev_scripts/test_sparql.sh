#!/usr/bin/env bash
rm -f /home/stefan/zelph/wikidata-20260309-all-pruned.bin.pidx.322
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
build-release/bin/zelph "${SCRIPT_DIR}/test_sparql.zph"
build-release/bin/zelph "${SCRIPT_DIR}/test_sparql.zph"
