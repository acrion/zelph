#!/usr/bin/env bash
# dev_scripts/paper/run_all.sh
#
# Regenerates every log the paper cites.
#
# Each script is passed as a filename. Since 1.0.1 a script named on the
# command line runs as a session -- its lines are echoed, they anchor the
# deduction filter, and inference runs after each of them -- so it produces the
# same reasoning and the same deduction lines as the piped form, without the
# startup banner and the `zelph> ` prompts. That makes for a cleaner recording.
# Before 1.0.1 the two forms disagreed and only the piped one worked here.
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

  # The exit status is the whole guard, and it has to be: with stdout and stderr
  # merged into one file, an error lands on the same line as the prompt that
  # preceded it, so a `grep '^Error'` over the log matches nothing. Since 1.0.1
  # the binary exits non-zero on any failure -- a missing import, a syntax error,
  # a bad depth argument -- and a session script stops at its first failing line,
  # so a truncated log and a non-zero status arrive together.
  if "${zelph}" "${script}" > "${log}" 2>&1; then
    echo "ok   ${name}"
  else
    echo "FAIL ${name}: see ${log}" >&2
    tail -2 "${log}" >&2
    status=1
  fi
done

python3 "${here}/eml_dag_stats.py" > "${out}/eml_dag_stats.md"
echo "ok   eml_dag_stats"

exit ${status}
