#!/usr/bin/env bash
set -eu

run_test() {
  local a="$1" b="$2" label="$3"
  local log_file
  log_file=$(mktemp "/tmp/zelph-mul-${label}-XXXXXX.log")

  printf '.import arithmetic\n.log -1\n&%s * &%s\n.quit\n' "$a" "$b" \
    | build-release/bin/zelph >"$log_file" 2>&1 || {
      echo "ERROR: zelph failed for ${label}, see $log_file" >&2
      return 1
    }

  echo "=== ${label}  (&${a} × &${b}) ==="
  echo "Full log: $log_file"
  echo

  echo "--- Time (final query) ---"
  line=$(grep -E '^-- [0-9]+ ms --$' "$log_file" | tail -1)
  echo "${line:-"(not found)"}"
  echo

  echo "--- Final [prof] header (equality step) ---"
  line=$(grep -E '^\[prof\] epoch=.* after .*«=»' "$log_file" | tail -1)
  echo "${line:-"(not found)"}"
  echo

  echo "--- rules_applied / deduce_calls / facts_created (final) ---"
  line=$(grep -E '^\s+rules_applied=[0-9]+ deduce_calls=[0-9]+ facts_created=[0-9]+' "$log_file" | tail -1)
  echo "${line:-"(not found)"}"
  echo

  echo "--- snapshot_facts (final) ---"
  line=$(grep -E 'snapshot_facts=[0-9]+' "$log_file" | tail -1)
  echo "${line:-"(not found)"}"
  echo

  echo "--- top_relations_by_scan (final) ---"
  line=$(grep 'top_relations_by_scan:' "$log_file" | tail -1)
  echo "${line:-"(not found)"}"
}

run_test 12 34 "2x2"
run_test 123 456 "3x3"
run_test 1234 5678 "4x4"

echo "Done."
