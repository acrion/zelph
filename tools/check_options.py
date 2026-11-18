#!/usr/bin/env python3
"""Hold the zelph binary to the options of its command line.

``zelph --node-map-load-factor <factor>`` gives the session the load factor
that its node map may reach once it has 2^31 buckets, as the default of the
process: the session has it from its first line on, ``.node-map-load-factor``
names it as the default, and ``.new`` restores it. The library owns all of
that (``Interactive::set_node_map_default_load_factor``) and the doctest suite
pins it there. What only a real process can show is what ``main()`` does with
its arguments: that the option is read where it stands ahead of a script, that
behind the name of a script its two words reach the script as arguments, and
that an option whose factor cannot be read ends the process with status 1
before anything has run.

Every case starts the binary with its arguments, pipes the lines of the case
into it unless the case names a script, and holds the status and the output to
what the case says.

Exit codes: 0 every case holds, 1 one of them does not, 2 the question could
not be put.
"""

import argparse
import os
import subprocess
import sys
import tempfile

OPTION = "--node-map-load-factor"
STATUS_LINE = "Node map load factor from 2^31 buckets: "
SHOW = ".node-map-load-factor\n"

# What a session outputs upon commencing input from a pipe, and thus the
# mark of a process that initiated execution.
BANNER = "REPL mode"

# Each case: a name, arguments positioned before a script, the lines of
# the session (provided via piping or written into a script when
# `as_script` is set), arguments located after the script, the anticipated
# status, the phrases that must appear in the output, and the phrases that
# must not appear in the output.
CASES = [
    {
        "name": "without the option the default is 0.8",
        "before": [],
        "lines": SHOW,
        "status": 0,
        "holds": [STATUS_LINE + "0.8 (default)"],
    },
    {
        "name": "the option gives the session its default",
        "before": [OPTION, "0.9"],
        "lines": SHOW,
        "status": 0,
        "holds": [STATUS_LINE + "0.9 (default)"],
    },
    {
        "name": "the default of the option is what .new restores and what 'default' names",
        "before": [OPTION, "0.9"],
        "lines": ".node-map-load-factor 0.95\n.new\n" + SHOW + ".node-map-load-factor 0.85\n.node-map-load-factor default\n",
        "status": 0,
        "holds": [STATUS_LINE + "0.95\n", STATUS_LINE + "0.85\n", STATUS_LINE + "0.9 (default)"],
        "count": (STATUS_LINE + "0.9 (default)", 2),
    },
    {
        "name": "the option ahead of a script",
        "before": [OPTION, "0.9"],
        "lines": SHOW,
        "as_script": True,
        "status": 0,
        "holds": [STATUS_LINE + "0.9 (default)"],
    },
    {
        # Behind the name of a script, the two words of the option serve as
        # arguments to the script: the session keeps 0.8, and the script reads
        # both words.
        "name": "behind the name of a script the option belongs to the script",
        "before": [],
        "lines": SHOW + '%(zelph/out (string "arguments: " (string/join (dyn :args) " ")))\n',
        "as_script": True,
        "after": [OPTION, "0.9"],
        "status": 0,
        "holds": [STATUS_LINE + "0.8 (default)", OPTION + " 0.9"],
    },
    {
        "name": "the option without a factor",
        "before": [OPTION],
        "lines": "a b c\n",
        "status": 1,
        "holds": ["Option " + OPTION + " needs a factor"],
        "lacks": [BANNER, "a b c"],
    },
    {
        "name": "a factor that is no number",
        "before": [OPTION, "abc"],
        "lines": "a b c\n",
        "status": 1,
        "holds": ["Option " + OPTION + ": ", "a number from 0.8 to 0.95", "not 'abc'"],
        "lacks": [BANNER, "a b c"],
    },
    {
        "name": "a factor below the range",
        "before": [OPTION, "0.5"],
        "lines": "a b c\n",
        "status": 1,
        "holds": ["a number from 0.8 to 0.95", "not '0.5'"],
        "lacks": [BANNER, "a b c"],
    },
    {
        # A script does not execute once the option preceding it has
        # failed.
        "name": "a factor above the range ahead of a script",
        "before": [OPTION, "0.96"],
        "lines": "a b c\n",
        "as_script": True,
        "status": 1,
        "holds": ["a number from 0.8 to 0.95", "not '0.96'"],
        "lacks": ["a b c"],
    },
]


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def run(command, stdin_path):
    try:
        with open(stdin_path, "rb") as piped:
            done = subprocess.run(command, stdin=piped, capture_output=True, check=False,
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

    # rlwrap would re-execute the process; a pipe prevents this action, and the
    # variable maintains this state when a test runner passes a terminal.
    os.environ["ZELPH_NO_RLWRAP"] = "1"

    violations = []

    with tempfile.TemporaryDirectory(prefix="zelph_options_") as directory:
        script = os.path.join(directory, "case.zph")
        nothing = os.path.join(directory, "nothing")
        with open(nothing, "w", encoding="utf-8"):
            pass

        for case in CASES:
            name = case["name"]
            with open(script, "w", encoding="utf-8") as handle:
                handle.write(case["lines"])

            command = [args.binary] + case["before"]
            if case.get("as_script"):
                command += [script] + case.get("after", [])
                status, output = run(command, nothing)
            else:
                status, output = run(command, script)
            output = output.replace("\r\n", "\n")

            if status != case["status"]:
                violations.append(f"{name}: expected status {case['status']}, got {status}")
            for phrase in case.get("holds", []):
                if phrase not in output:
                    violations.append(f"{name}: the output does not hold {phrase!r}")
            for phrase in case.get("lacks", []):
                if phrase in output:
                    violations.append(f"{name}: the output holds {phrase!r}")
            if "count" in case:
                phrase, times = case["count"]
                if output.count(phrase) != times:
                    violations.append(f"{name}: expected {phrase!r} {times} time(s), got {output.count(phrase)}")

    if violations:
        for violation in violations:
            print(f"error: {violation}", file=sys.stderr)
        return 1

    print(f"{len(CASES)} cases: the binary reads its options as documented.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
