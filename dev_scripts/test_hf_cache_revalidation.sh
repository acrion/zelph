#!/usr/bin/env bash
# Verify that a stale cached HF manifest is revalidated and refetched.
#
# Usage: test_hf_cache_revalidation.sh [manifest-uri] [zelph-binary] [cache-root]
#   manifest-uri: pruned Wikidata manifest on Hugging Face (default below)
#   zelph-binary: built zelph executable (default: build-release/bin/zelph)
#   cache-root:   disposable cache parent directory (default: /tmp/zelph-hf-cache-revalidation)
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
MANIFEST="${1:-hf://datasets/acrion/zelph/wikidata-20260309-all-pruned/wikidata-20260309-all-pruned.hf-v2.json}"
ZELPH="${2:-${ROOT}/build-release/bin/zelph}"
CACHE="${3:-${TMPDIR:-/tmp}/zelph-hf-cache-revalidation}"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    sed -n '2,7p' "$0"
    exit 0
fi

if [[ ! -x "${ZELPH}" ]]; then
    echo "zelph binary not executable: ${ZELPH}" >&2
    exit 2
fi

rm -rf "${CACHE}"
mkdir -p "${CACHE}"

run_load() {
    printf '.load-partial %s left=0 right=0 nameOfNode=0 nodeOfName=0\n.quit\n' "${MANIFEST}" \
        | ZELPH_HF_CACHE_DIR="${CACHE}" "${ZELPH}" 2>&1
}

require_successful_load() {
    local output="$1"
    local phase="$2"
    if ! grep -Fq 'String pool size after partial load' <<<"${output}"; then
        echo "${phase} did not complete a partial load:" >&2
        printf '%s\n' "${output}" >&2
        exit 1
    fi
}

echo "Initial remote load (populate cache)"
initial_output="$(run_load)"
require_successful_load "${initial_output}" "Initial remote load"

manifest_body="$(find "${CACHE}/v2" -maxdepth 1 -type f -name 'manifest_*.body' -print -quit)"
if [[ -z "${manifest_body}" ]]; then
    echo "No cached manifest body found under ${CACHE}/v2" >&2
    exit 1
fi

echo "Tampering cached manifest and sidecar: ${manifest_body}"
sed -i 's#"binPath"[[:space:]]*:[[:space:]]*"[^"]*"#"binPath": "/nonexistent/stale-local.bin"#' "${manifest_body}"
printf 'etag="tampered-by-hf-cache-regression"\n' >> "${manifest_body}.meta"

echo "Second remote load (must revalidate and refetch)"
second_output="$(run_load)"
require_successful_load "${second_output}" "Second remote load"
if grep -Fq '/nonexistent/stale-local.bin' "${manifest_body}"; then
    echo "Cached manifest still contains the tampered binPath after refetch" >&2
    exit 1
fi
echo "HF cache revalidation regression passed"
