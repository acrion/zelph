#!/usr/bin/env python3
"""Hold the zelph binary to finishing a Janet program that makes Janet collect.

Janet 1.41.2 frees the table of its top-level dynamic bindings on the first
garbage collection, and zelph keeps its :err binding there. Janet's event loop
reads that table when it reports on a finished task, so a program that
allocated enough to trigger a collection killed the process with SIGSEGV --
after its last line of output, which made the run look finished.
src/lib/patch_janet_gc.cmake makes the collector mark the table, and these
cases hold the build to it.

Both cases died before the patch: a plain Janet script without any thread, and
the hand-off that the Threads section of mkdocs/docs/janet.md recommends (a
thread computes, a thread channel carries the result, the main thread writes
it). A crash ends the process that meets it, which is why this asks the binary
in a process of its own instead of the doctest suite, where the first failure
would end every test after it.

Exit codes: 0 every case holds, 1 one of them does not, 2 the question could
not be put.
"""

import argparse
import os
import subprocess
import sys
import tempfile

# Each case: a file name, its content, and a phrase that must be printed
# once the run completes its allocations. The phrase catches a run that
# terminates prematurely before finishing, the status captures one that fails
# during exit, after the phrase, just as the hand-off did before the patch.
CASES = [
    (
        "a Janet script that allocates two million strings",
        "alloc.janet",
        "(var acc @[])\n"
        "(for i 0 2000000 (array/push acc (string \"x\" i)))\n"
        "(print \"script done \" (length acc))\n",
        "script done 2000000",
    ),
    (
        "a thread hands 50,000 pairs to the main thread, which writes them",
        "handoff.zph",
        # A single line in the inline form, as a REPL user types it.
        " ".join([
            "%(do (def ch (ev/thread-chan 4))",
            "(ev/spawn-thread",
            "  (for b 0 50",
            "    (def batch @[])",
            "    (for i 0 1000",
            "      (def k (+ (* b 1000) i))",
            "      (array/push batch [(string \"n\" k) (string \"v\" (% (* k 7919) 1000))]))",
            "    (ev/give ch batch))",
            "  (ev/give ch nil))",
            "(var written 0)",
            "(forever",
            "  (def batch (ev/take ch))",
            "  (unless batch (break))",
            "  (each [s o] batch (zelph/fact s \"maps-to\" o) (++ written)))",
            "(zelph/out (string \"main wrote \" written)))",
        ]) + "\n",
        "main wrote 50000",
    ),
]

# Sufficient in either scenario, requiring only a few seconds; a run that
# hangs constitutes a violation, not a stuck check.
TIMEOUT_SECONDS = 600


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


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

    with tempfile.TemporaryDirectory(prefix="zelph_janet_gc_") as directory:
        for name, file_name, body, phrase in CASES:
            script = os.path.join(directory, file_name)
            with open(script, "w", encoding="utf-8") as handle:
                handle.write(body)

            # The encoding is named explicitly, for the rationale provided by
            # check_exit_status.py.
            try:
                done = subprocess.run([args.binary, script], stdin=subprocess.DEVNULL,
                                      capture_output=True, check=False, encoding="utf-8",
                                      errors="replace", timeout=TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                violations.append(f"{name}: no result within {TIMEOUT_SECONDS} seconds")
                continue
            except OSError as exc:
                fail(f"cannot run {args.binary}: {exc}")

            output = (done.stdout or "") + (done.stderr or "")
            if done.returncode != 0:
                violations.append(f"{name}: expected status 0, got {done.returncode}")
            if phrase not in output:
                violations.append(f"{name}: nothing in the output said '{phrase}'")

    if violations:
        for violation in violations:
            print(f"error: {violation}", file=sys.stderr)
        return 1

    print(f"{len(CASES)} cases: each Janet program ran to its end, collections included.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
