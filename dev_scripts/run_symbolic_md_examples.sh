#!/usr/bin/env bash
# dev_scripts/run_symbolic_md_examples.sh
# Feeds each symbolic.md example block into a fresh zelph session, so the
# printed output can be validated against (and pasted into) the docs.
set -euo pipefail
ZELPH="${ZELPH:-zelph}"
for f in "$(dirname "$0")"/symbolic_md_*.zph; do
    echo "=== ${f} ==="
    "${ZELPH}" < "${f}"
    echo
done
