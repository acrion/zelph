#!/usr/bin/env bash
# dev_scripts/analyze-wikidata-smoke.sh
#
# Wikidata reasoning smoke test: baseline guard for engine changes
# (condition ordering, semi-naive evaluation, unification anchors) on
# Wikidata-shaped workloads.
#
# Design principle: all rules use ! (contradiction) as their consequence,
# so NO facts are created -- contradictions are only counted. The measured
# cost is therefore pure scan/join/negation work and scales with the P361
# extension size, not with the number of matches. (A fact-producing variant
# derived >100k smokeMutual facts from reflexive P361 entries and did not
# terminate in useful time.)
#
#   rule 1 -- join anchored via the constant Q6256 (object-driven anchor)
#   rule 2 -- self-join over the full P361 extension (parallel snapshot);
#             X != Y guards against the dump's many reflexive P361 facts
#   rule 3 -- negation with all variables bound (one existence check per
#             P361 match; the documented inverse-consistency pattern)
#
# .run-once = one classic iteration: deterministic, no fixpoint loop, and
# exactly the workload shape of a one-shot Wikidata consistency check.
# NOT measured: the .load step (~1 min, memory-bound, high variance).
set -eu

BIN=build-release/bin/zelph
DUMP=/home/stefan/zelph/wikidata-20260309-all-pruned.bin
LOG_FILE=$(mktemp /tmp/zelph-wd-smoke-XXXXXX.log)

printf '%s\n' \
  ".load ${DUMP}" \
  ".lang wikidata" \
  "(X P31 Q6256, X P36 Y, ¬(X smokeguard Y)) => !" \
  "(X P31 Q6256, X P361 Y, ¬(Y P527 X)) => !" \
  "(X P361 Y, X P527 Y, X != Y) => !" \
  ".run-once" \
  ".quit" \
  | "$BIN" >"$LOG_FILE" 2>&1 || echo "WARNING: zelph exited non-zero, see $LOG_FILE" >&2

echo "Full log: $LOG_FILE"
echo
echo "--- Load (excluded from measurement) ---"
grep "Time needed for loading" "$LOG_FILE" || echo "(load time line not found)"
echo
echo "--- Measured reasoning pass ---"
grep "Reasoning complete in" "$LOG_FILE" | tail -1 || echo "(no timing found -- did .run-once execute?)"
grep -m1 "matches processed" "$LOG_FILE" || true
echo
echo "--- Contradiction count (sanity: must be large and stable across engine changes) ---"
grep -cE '^«?!' "$LOG_FILE" || true
