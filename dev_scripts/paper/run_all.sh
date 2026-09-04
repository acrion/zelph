#!/usr/bin/env bash
# dev_scripts/paper/run_all.sh
#
# Regenerates every log under out/ that the paper cites.
#
# The scripts are fed on STDIN, not passed as a filename, and that is not a
# matter of taste. Reading from stdin puts the engine in REPL mode, which is
# what the paper's appendix instructs a reader to do, and it is the only mode
# that (a) prints the per-statement timings the measurement table reports and
# (b) tracks a "most recently created node" for the bare `.node` in s7. Run
# with a filename instead and s7 fails with "No argument given and no previous
# output node available".
#
# The recorded outputs are NOT kept in this repository. They are evidence for one
# paper at one released version, and a directory that HEAD keeps moving past
# invites hand-editing a recording so that it says what a newer version would
# have said -- which is a claim that a run did something it did not do. They live
# frozen in the paper's own artifact instead. What lives here is the scripts,
# because this is where they break when the standard library moves.
#
# Usage:  ./run_all.sh [path-to-zelph-binary] [output-directory]
# Defaults: ../../build-release/bin/zelph, and ./out beside this script.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
zelph="${1:-${here}/../../build-release/bin/zelph}"
out="${2:-${here}/out}"

if [[ ! -x "${zelph}" ]]; then
  echo "Error: zelph binary not found or not executable: ${zelph}" >&2
  echo "Build it first, or pass the path as the first argument." >&2
  exit 1
fi

# A mismatch between the bundled allocator and the system C library aborts the
# binary during static initialisation, before main and without any output, so
# check that it starts at all before blaming a script for an empty log.
if ! "${zelph}" -v >/dev/null 2>&1; then
  echo "Error: ${zelph} does not start. Run it with empty input to see." >&2
  exit 1
fi

mkdir -p "${out}"
status=0

for script in "${here}"/s[0-9]*.zph; do
  name="$(basename "${script}" .zph)"
  log="${out}/${name}.log"
  "${zelph}" < "${script}" > "${log}" 2>&1 || true

  # An import that does not resolve does NOT stop the run: the remaining
  # statements still execute, and the symbolic and EML requests still answer,
  # because those terms never needed the arithmetic module. The log then looks
  # plausible and describes a configuration nobody asked for. That is how a
  # module rename went unnoticed here for six weeks, so it is checked.
  if grep -q "not found" "${log}"; then
    echo "FAIL ${name}: an import did not resolve" >&2
    grep -m1 "not found" "${log}" >&2
    status=1
  elif grep -q "^Error" "${log}"; then
    echo "FAIL ${name}: the engine reported an error" >&2
    grep -m1 "^Error" "${log}" >&2
    status=1
  else
    echo "ok   ${name}"
  fi
done

python3 "${here}/eml_dag_stats.py" > "${out}/eml_dag_stats.md"
echo "ok   eml_dag_stats"

exit ${status}
