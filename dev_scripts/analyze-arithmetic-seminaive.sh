#!/usr/bin/env bash
# dev_scripts/analyze-arithmetic-seminaive.sh
# A/B comparison classic vs semi-naive, plus a check-mode verification run.
set -eu

run_variant() {
  local mode="$1" a="$2" b="$3" label="$4"
  local log_file
  log_file=$(mktemp "/tmp/zelph-mul-${label}-${mode}-XXXXXX.log")
  printf '.import arithmetic\n.semi-naive %s\n.log -1\n&%s * &%s\n.quit\n' "$mode" "$a" "$b" \
    | build-release/bin/zelph >"$log_file" 2>&1 || {
      echo "ERROR (${label}, ${mode}), see $log_file" >&2
      return 1
    }
  echo "=== ${label} (&${a} x &${b}) mode=${mode} ==="
  line=$(grep -E '^-- [0-9]+ ms --$' "$log_file" | tail -1); echo "${line:-"(time not found)"}"
  line=$(grep -E 'snapshot_facts=[0-9]+' "$log_file" | tail -1); echo "${line:-"(snapshot line not found)"}"
  line=$(grep -E '^\s+rules_applied=' "$log_file" | tail -1); echo "${line:-"(counter line not found)"}"
  line=$(grep 'top_relations_by_scan:' "$log_file" | tail -1); echo "${line:-"(top relations not found)"}"
  if grep -q "completeness violation" "$log_file"; then
    echo "!!! COMPLETENESS VIOLATION reported -- see $log_file"
  fi
  echo
}

for pair in "12 34 2x2" "123 456 3x3" "1234 5678 4x4"; do
  set -- $pair
  run_variant off   "$1" "$2" "$3"
  run_variant on    "$1" "$2" "$3"
  run_variant check "$1" "$2" "$3"
done
echo "Done."