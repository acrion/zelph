#!/usr/bin/env bash
# Verify that a timed-out HF metadata probe fails promptly with a useful
# diagnostic.  This is offline: it replaces curl with a fixture that emits the
# same write-out JSON curl produces for exit code 28.
#
# Usage: test_hf_transfer_diagnostics.sh [zelph-binary] [cache-root]
#   zelph-binary: built zelph executable (default: build-release/bin/zelph)
#   cache-root:   disposable cache parent directory (default: /tmp/zelph-hf-transfer-diagnostics)
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
ZELPH="${1:-${ROOT}/build-release/bin/zelph}"
CACHE="${2:-${TMPDIR:-/tmp}/zelph-hf-transfer-diagnostics}"
MANIFEST="hf://datasets/acrion/zelph/wikidata-20260309-all-pruned/wikidata-20260309-all-pruned.hf-v2.json"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    sed -n '2,7p' "$0"
    exit 0
fi
if [[ ! -x "${ZELPH}" ]]; then
    echo "zelph binary not executable: ${ZELPH}" >&2
    exit 2
fi

rm -rf "${CACHE}"
mkdir -p "${CACHE}/bin"
FAKE_CURL="${CACHE}/bin/curl"
cat > "${FAKE_CURL}" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' '{"time_connect":0.001,"time_total":0.002,"size_download":0,"speed_download":0,"http_code":0,"exitcode":28}'
exit 28
EOF
chmod +x "${FAKE_CURL}"

output="$(printf '.load-partial %s meta-only\n.quit\n' "${MANIFEST}" \
    | ZELPH_HF_CACHE_DIR="${CACHE}" ZELPH_HF_CURL_BIN="${FAKE_CURL}" "${ZELPH}" 2>&1 || true)"
if ! grep -Fq 'HF probe response_timeout' <<<"${output}"; then
    echo "Timed-out probe was not classified descriptively:" >&2
    printf '%s\n' "${output}" >&2
    exit 1
fi
if ! grep -Fq "${MANIFEST}" <<<"${output}"; then
    echo "Diagnostic omitted the requested manifest URI:" >&2
    printf '%s\n' "${output}" >&2
    exit 1
fi
echo "HF transfer diagnostic regression passed"
