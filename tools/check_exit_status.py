#!/usr/bin/env python3
"""Hold the zelph binary to the status it reports.

zelph used to leave with 0 whatever had happened. A script whose ``.import``
did not resolve therefore reported success -- and because the statements after
it still answered (they never needed the missing module), the recorded log
looked entirely plausible while describing a configuration nobody had asked
for. That is the worst shape a failure can take for evidence, and it went
unnoticed for six weeks in exactly that way.

The status is the contract, and a contract is what a caller tests. The doctest
suite pins the flag the library keeps (``Interactive::had_failure``); this pins
the whole chain, ``main()`` and its marker lines included, in both modes the
binary has:

    zelph script.zph        a session read from a file
    zelph < script.zph      the same lines piped in

Both are checked, because the two used to fail differently and both used to
report success: the file form stopped at the failing line and its log simply
ended, while the piped form carried on and answered from a premise that had not
held. They agree now -- both stop, both say so, both leave with 1 -- and that
agreement is what these cases hold them to.

Exit codes: 0 every case holds, 1 one of them does not, 2 the question could
not be put.
"""

import argparse
import os
import subprocess
import sys
import tempfile

# Every case: a script, the status both modes must report, and a phrase the
# run has to say. The phrase matters as much as the number -- a status tells a
# shell, a line tells the person reading the log a year later.
CASES = [
    (
        "a script that runs through",
        "socrates ~ human\n(A ~ human) => (A ~ mortal)\n",
        0,
        None,
    ),
    (
        "an import that does not resolve",
        ".import no-such-module-of-any-kind\nsocrates ~ human\n",
        1,
        "not found",
    ),
    (
        "a statement that does not parse",
        "a b x(c d e)\nsocrates ~ human\n",
        1,
        "glued",
    ),
]

# Both forms stop at the first failing line, and both say so -- in their own
# words, because only one of them has a file name to give. A log that simply
# ENDS cannot be told from a shorter script, which is the whole reason these
# lines exist.
ABORT_NOTE = {
    "zelph <file>": "the lines after it did not run",
    "zelph < file": "the input after it was not read",
}


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def run(command, stdin_path=None):
    # The encoding is named on purpose. subprocess decodes with the machine's
    # locale codec otherwise, and zelph writes bytes that cp1252 has no
    # character for -- on Windows that killed the reader thread and the check
    # reported a violation it had never measured.
    try:
        if stdin_path is None:
            done = subprocess.run(command, capture_output=True, check=False,
                                  encoding="utf-8", errors="replace")
        else:
            with open(stdin_path, "rb") as script:
                done = subprocess.run(command, stdin=script, capture_output=True,
                                      check=False, encoding="utf-8", errors="replace")
    except OSError as exc:
        fail(f"cannot run {command[0]}: {exc}")
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", help="the zelph executable to question")
    args = parser.parse_args()

    if not os.path.isfile(args.binary):
        fail(f"no such binary: {args.binary}")

    # rlwrap would re-exec the process and take the status with it.
    os.environ["ZELPH_NO_RLWRAP"] = "1"

    violations = []

    with tempfile.TemporaryDirectory(prefix="zelph_exit_status_") as directory:
        for name, body, expected, phrase in CASES:
            script = os.path.join(directory, "case.zph")
            with open(script, "w", encoding="utf-8") as handle:
                handle.write(body)

            for mode, command, stdin_path in (
                ("zelph <file>", [args.binary, script], None),
                ("zelph < file", [args.binary], script),
            ):
                status, output = run(command, stdin_path)
                if status != expected:
                    violations.append(
                        f"{name}, {mode}: expected status {expected}, got {status}")
                if phrase is not None and phrase not in output:
                    violations.append(
                        f"{name}, {mode}: nothing in the output said '{phrase}'")

            # Each form owes the reader a word about the lines it never
            # reached. Every failing case above carries a line AFTER the one
            # that fails, so there is always something not to have run.
            if expected != 0:
                for mode, command, stdin_path in (
                    ("zelph <file>", [args.binary, script], None),
                    ("zelph < file", [args.binary], script),
                ):
                    _, output = run(command, stdin_path)
                    if ABORT_NOTE[mode] not in output:
                        violations.append(
                            f"{name}, {mode}: the log does not say that it stopped early")

    if violations:
        for violation in violations:
            print(f"error: {violation}", file=sys.stderr)
        return 1

    print(f"{len(CASES)} cases, both modes: the reported status matches what happened.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
