#!/usr/bin/env python3
"""Hold the zelph binary to its continuation prompt.

While a statement spans several lines, a person typing at a terminal sees a
prompt of dots to the width of the one the statement began under, ``.....> ``
beneath ``zelph> ``; before, the prompt simply vanished, and an unfinished
statement looked exactly like a REPL that had stopped responding. A pipe and a
file keep the empty prompt, because a recorded session -- every transcript in
the documentation -- shows a continuation line exactly as it was written.

Only a real process can be asked which of the two it is, so the same two-line
statement is sent twice: once on a pseudo-terminal, once through a pipe. POSIX
only (the pty module).

Exit codes: 0 both hold, 1 one of them does not, 2 the question could not be
put.
"""

import argparse
import os
import pty
import select
import subprocess
import sys

# The sequence "a p" does not yet constitute a statement, thus it remains
# pending, and the next line brings it to completion.
LINES = ["a p", "b", ".quit"]
FIRST = "zelph> "
CONTINUATION = ".....> "


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def drain(master):
    # The terminal returns to the master side whatever gets written into it;
    # read it so that the line discipline never experiences a block due to a
    # full buffer.
    while select.select([master], [], [], 0)[0]:
        try:
            if not os.read(master, 4096):
                return
        except OSError:
            return


def on_terminal(binary):
    master, slave = pty.openpty()
    try:
        process = subprocess.Popen([binary], stdin=slave, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
    except OSError as exc:
        fail(f"cannot run {binary}: {exc}")
    os.close(slave)

    os.write(master, ("\n".join(LINES) + "\n").encode())
    drain(master)

    try:
        output, _ = process.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()
        fail("zelph did not leave after .quit on a terminal")
    finally:
        os.close(master)
    return process.returncode, output.decode("utf-8", errors="replace")


def through_a_pipe(binary):
    try:
        done = subprocess.run([binary], input="\n".join(LINES) + "\n", capture_output=True,
                              check=False, encoding="utf-8", errors="replace", timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        fail(f"cannot run {binary}: {exc}")
    return done.returncode, (done.stdout or "") + (done.stderr or "")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", help="the zelph executable to question")
    args = parser.parse_args()

    if not os.path.isfile(args.binary):
        fail(f"no such binary: {args.binary}")

    # On a terminal, the binary would re-exec itself under rlwrap, which
    # takes control of the prompt from that point onward.
    os.environ["ZELPH_NO_RLWRAP"] = "1"

    violations = []

    status, output = on_terminal(args.binary)
    if status != 0:
        violations.append(f"on a terminal: expected status 0, got {status}")
    if FIRST + CONTINUATION not in output:
        violations.append(f"on a terminal: no '{CONTINUATION}' after the line that left the statement open")

    status, output = through_a_pipe(args.binary)
    if status != 0:
        violations.append(f"through a pipe: expected status 0, got {status}")
    if FIRST not in output:
        violations.append(f"through a pipe: no '{FIRST}' at all, so the absence below proves nothing")
    if CONTINUATION.strip() in output:
        violations.append(f"through a pipe: a continuation prompt was printed")

    if violations:
        for violation in violations:
            print(f"error: {violation}", file=sys.stderr)
        return 1

    print("A terminal shows the continuation prompt, a pipe does not.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
