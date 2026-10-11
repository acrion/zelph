#!/usr/bin/env python3
"""Hold the zelph binary to its ``.timer`` setting.

The REPL prints a line such as ``-- 12 ms --`` after an input, and ``.timer``
decides when: ``auto`` (the default) from 10 ms on, ``on`` after every input,
``off`` never. The library owns that rule (``Interactive::report_timing``) and
the doctest suite pins it there; what only a real process can show is that the
binary asks it after every input and prints what it is told -- the binary used
to apply a threshold of its own, which no setting could reach.

Every case pipes its lines into the binary and counts the timing lines in what
comes back. The slow input is a Janet sleep of 50 ms, because a timing that
must appear needs an input that cannot be faster than the threshold on any
machine; a fast input is never expected to stay silent under ``auto``, since a
loaded machine may make anything take 10 ms.

Exit codes: 0 every case holds, 1 one of them does not, 2 the question could
not be put.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

TIMING = re.compile(r"^-- ([0-9]+) ms --$", re.MULTILINE)

SLOW = "%(os/sleep 0.05)\n"

# Each case: a name, the lines received via input, the least and maximum
# count of timing lines the output can contain, and the least number of
# milliseconds the longest one must show (None if nothing is printed).
CASES = [
    ("auto, the default, times an input of 50 ms", SLOW, 1, 1, 50),
    ("on times a statement however fast it was", ".timer on\na b c\n", 2, 2, 0),
    ("off prints no timing even after an input of 50 ms", ".timer off\n" + SLOW, 0, 0, None),
    # The line ".timer auto" undergoes timing under auto, thus it might or might
    # not add one.
    ("auto again after on", ".timer on\n.timer auto\n" + SLOW, 2, 3, 50),
]


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def run(command, stdin_path=None):
    try:
        if stdin_path is None:
            done = subprocess.run(command, capture_output=True, check=False,
                                  encoding="utf-8", errors="replace", timeout=120)
        else:
            with open(stdin_path, "rb") as script:
                done = subprocess.run(command, stdin=script, capture_output=True, check=False,
                                      encoding="utf-8", errors="replace", timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        fail(f"cannot run {command[0]}: {exc}")
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", help="the zelph executable to question")
    args = parser.parse_args()

    if not os.path.isfile(args.binary):
        fail(f"no such binary: {args.binary}")

    # rlwrap would re-execute the process; this step is bypassed when dealing
    # with a pipe, and the variable keeps it so when a test runner passes a
    # terminal.
    os.environ["ZELPH_NO_RLWRAP"] = "1"

    violations = []

    with tempfile.TemporaryDirectory(prefix="zelph_timer_") as directory:
        script = os.path.join(directory, "case.zph")

        for name, body, least, most, slowest in CASES:
            with open(script, "w", encoding="utf-8") as handle:
                handle.write(body)

            status, output = run([args.binary], script)
            timings = [int(ms) for ms in TIMING.findall(output)]
            if status != 0:
                violations.append(f"{name}: expected status 0, got {status}")
            if not least <= len(timings) <= most:
                violations.append(f"{name}: expected {least} to {most} timing line(s), got {len(timings)}")
            elif slowest is not None and max(timings) < slowest:
                violations.append(f"{name}: the slowest timing shows {max(timings)} ms, expected at least {slowest}")

        # A script named on the command line does not function as a
        # REPL: it prints no timing lines, regardless of the
        # configuration setting.
        with open(script, "w", encoding="utf-8") as handle:
            handle.write(".timer on\n" + SLOW)
        status, output = run([args.binary, script])
        if status != 0:
            violations.append(f"zelph <file> with .timer on: expected status 0, got {status}")
        if TIMING.search(output):
            violations.append("zelph <file> with .timer on: printed a timing line")

    if violations:
        for violation in violations:
            print(f"error: {violation}", file=sys.stderr)
        return 1

    print(f"{len(CASES) + 1} cases: the binary prints its timing lines as .timer says.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
