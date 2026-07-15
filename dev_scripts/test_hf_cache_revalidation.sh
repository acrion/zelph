#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
MANIFEST="${1:-hf://datasets/acrion/zelph/wikidata-20260309-all-pruned/wikidata-20260309-all-pruned.hf-v2.json}"
ZELPH="${2:-${ROOT}/build-release/bin/zelph}"
CACHE="${3:-${TMPDIR:-/tmp}/zelph-hf-cache-revalidation}"

if [[ ! -x "${ZELPH}" ]]; then
    echo "zelph binary not executable: ${ZELPH}" >&2
    exit 2
fi

rm -rf "${CACHE}"
mkdir -p "${CACHE}"

run_load() {
    printf '.load-partial %s left=0 right=0 nameOfNode=0 nodeOfName=0\n.quit\n' "${MANIFEST}" \
        | ZELPH_HF_CACHE_DIR="${CACHE}" "${ZELPH}" >/dev/null
}

echo "Initial remote load (populate cache)"
run_load

manifest_body="$(find "${CACHE}/v2" -maxdepth 1 -type f -name 'manifest_*.body' -print -quit)"
if [[ -z "${manifest_body}" ]]; then
    echo "No cached manifest body found under ${CACHE}/v2" >&2
    exit 1
fi

echo "Tampering cached manifest and sidecar: ${manifest_body}"
sed -i 's#"binPath"[[:space:]]*:[[:space:]]*"[^"]*"#"binPath": "/nonexistent/stale-local.bin"#' "${manifest_body}"
printf 'etag="tampered-by-hf-cache-regression"\n' >> "${manifest_body}.meta"

echo "Second remote load (must revalidate and refetch)"
run_load
echo "HF cache revalidation regression passed"
