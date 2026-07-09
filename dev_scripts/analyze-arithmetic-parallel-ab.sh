#!/usr/bin/env bash
# dev_scripts/analyze-arithmetic-parallel-ab.sh
# A/B: parallel unification on vs off, with the scoring of the current build.
# Run once per scoring variant (old / degree-aware) and compare medians of 3.
set -eu

run_case() {
  local script="$1" expr="$2" par="$3" label="$4"
  for i in 1 2 3; do
    local log_file
    log_file=$(mktemp "/tmp/zelph-parab-${label}-${par}-${i}-XXXXXX.log")
    if [ "$par" = "off" ]; then
      printf '.import %s\n.parallel\n.log -1\n%s\n.quit\n' "$script" "$expr" \
        | build-release/bin/zelph >"$log_file" 2>&1
    else
      printf '.import %s\n.log -1\n%s\n.quit\n' "$script" "$expr" \
        | build-release/bin/zelph >"$log_file" 2>&1
    fi
    t=$(grep -E '^-- [0-9]+(\.[0-9]+)? (ms|s) --$' "$log_file" | tail -1)
    echo "${label} parallel=${par} run${i}: ${t:-"(time not found)"}  ($log_file)"
  done
  echo
}

run_case arithmetic        '&1234 * &5678'                      on  dec4x4
run_case arithmetic        '&1234 * &5678'                      off dec4x4
run_case binary-arithmetic '(&3495734893 * &92348793847) = X'   on  binbig
run_case binary-arithmetic '(&3495734893 * &92348793847) = X'   off binbig
