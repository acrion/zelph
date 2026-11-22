#!/usr/bin/env python3
"""Hold the zelph binary to the options of its command line.

``zelph --default '<setting line>'`` makes a setting line a default of the
process: the session has the setting from its first line on, and ``.new``
restores it. The option may be given several times. The library owns all of
that (``Interactive::add_default``) and the doctest suite pins it there. What
only a real process can show is what ``main()`` does with its arguments: that
the option is read where it stands ahead of a script, each time it is given,
that behind the name of a script its two words reach the script as arguments,
and that a line which cannot be a default ends the process with status 1
before anything has run.

``zelph -v`` and ``zelph --version`` print the version and what the binary
incorporates, and start no session. Like the option above they are read ahead
of a script's name only: behind the name both words are arguments of the
script.

The environment variable ``ZELPH_THREADS`` names the number of threads of the
process, which an engine uses until ``.threads`` gives it a count of its own;
without the variable the count is read from the CPUs the process may run on.
The doctest suite pins what an engine does with its count. Only a real process
can show where the count of a process comes from, since it is read once: the
cases below start the binary with the variable set, unset and unusable, and
with its affinity mask narrowed to one CPU.

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

OPTION = "--default"
FACTOR = ".node-map-load-factor"
STATUS_LINE = "Node map load factor from 2^31 buckets: "
SHOW = FACTOR + "\n"
ANCHORS_OFF = "Anchor-based lookups: off"

# What a session outputs upon commencing input from a pipe, and thus the
# mark of a process that initiated execution.
BANNER = "REPL mode"

# A line that is printed exclusively by the version report.
VERSION_REPORT = "zelph incorporates the following third-party software:"

# A script that says it ran, and one that prints its arguments.
RAN = '%(zelph/out "the script ran")\n'
ARGUMENTS = '%(zelph/out (string "arguments: " (string/join (dyn :args) " ")))\n'

THREADS = "Threads: "
SHOW_THREADS = ".threads\n"

# Each case: a name, arguments positioned before a script, the lines of the
# session (provided via piping or written into a script when `as_script` is
# set), arguments located after the script, the anticipated status, the phrases
# that must appear in the output, and the phrases that must not appear in the
# output. A case may set environment variables for its process (`env`;
# ZELPH_THREADS remains unset otherwise) and narrow the affinity mask of its
# process to that many CPUs (`cpus`; such a case is omitted when the platform
# is unable to perform that).
CASES = [
    {
        "name": "without the option the default is 0.8",
        "before": [],
        "lines": SHOW,
        "status": 0,
        "holds": [STATUS_LINE + "0.8 (default)"],
    },
    {
        "name": "a default line gives the session its setting",
        "before": [OPTION, FACTOR + " 0.9"],
        "lines": SHOW,
        "status": 0,
        "holds": [STATUS_LINE + "0.9 (default)"],
    },
    {
        "name": "the setting of a default line is what .new restores and what 'default' names",
        "before": [OPTION, FACTOR + " 0.9"],
        "lines": FACTOR + " 0.95\n.new\n" + SHOW + FACTOR + " 0.85\n" + FACTOR + " default\n",
        "status": 0,
        "holds": [STATUS_LINE + "0.95\n", STATUS_LINE + "0.85\n", STATUS_LINE + "0.9 (default)"],
        "count": (STATUS_LINE + "0.9 (default)", 2),
    },
    {
        "name": "the option ahead of a script",
        "before": [OPTION, FACTOR + " 0.9"],
        "lines": SHOW,
        "as_script": True,
        "status": 0,
        "holds": [STATUS_LINE + "0.9 (default)"],
    },
    {
        # A script whose line is a .new is read on by the engine that the
        # reset made, and that engine adopts the default as the first one did.
        "name": "a script with a .new has the default back",
        "before": [OPTION, ".anchors off"],
        "lines": ".anchors on\n.new\n.anchors\n",
        "as_script": True,
        "status": 0,
        "holds": ["Anchor-based lookups: on", ANCHORS_OFF],
        "count": (ANCHORS_OFF, 1),
    },
    {
        # Behind the script's name, the two words comprising the option
        # serve as arguments to the script: the session keeps its setting,
        # and the script reads both words.
        "name": "behind the name of a script the option belongs to the script",
        "before": [],
        "lines": ".anchors\n" + ARGUMENTS,
        "as_script": True,
        "after": [OPTION, ".anchors off"],
        "status": 0,
        "holds": ["Anchor-based lookups: on", "arguments: " + OPTION + " .anchors off"],
        "lacks": [ANCHORS_OFF],
    },
    {
        "name": "the option given several times",
        "before": [OPTION, ".anchors off", OPTION, ".lang de", OPTION, ".explain-budget 5000"],
        "lines": ".anchors\n.lang\n.explain-budget\n.new\n.anchors\n.lang\n.explain-budget\n",
        "status": 0,
        "holds": ["The current language is 'de'", "Explain budget: 5000 work units (default)"],
        "count": (ANCHORS_OFF, 2),
    },
    {
        "name": "of two lines for one setting the later one decides",
        "before": [OPTION, FACTOR + " 0.9", OPTION, FACTOR + " 0.85"],
        "lines": SHOW + ".new\n" + SHOW,
        "status": 0,
        "count": (STATUS_LINE + "0.85 (default)", 2),
    },
    {
        # The lines associated with the option produce no output,
        # neither at the beginning nor following a .new: the script's
        # output originates solely from its own lines.
        "name": "a default line prints nothing",
        "before": [OPTION, ".anchors off", OPTION, ".deductions all", OPTION, ".timer off"],
        "lines": RAN + ".new\n" + RAN,
        "as_script": True,
        "status": 0,
        "count": ("the script ran", 2),
        "lacks": ["Anchor-based lookups", "Deduction printing mode", "Timer:"],
    },
    {
        # .new preserves the session's configuration, so the line
        # responsible for that setting is not re-executed: the toggle that
        # disabled auto-run initially would re-enable it, and the line
        # entered after the .new would then say "disabled".
        "name": "a setting of the session is set once",
        "before": [OPTION, ".auto-run"],
        "lines": ".new\n.auto-run\n",
        "status": 0,
        "holds": ["Auto-run is now enabled."],
        "lacks": ["Auto-run is now disabled."],
    },
    {
        "name": "the option without a line",
        "before": [OPTION],
        "lines": "a b c\n",
        "status": 1,
        "holds": ["Option " + OPTION + " needs a setting line"],
        "lacks": [BANNER, "a b c"],
    },
    {
        "name": "a line that is no setting command",
        "before": [OPTION, ".import math"],
        "lines": "a b c\n",
        "status": 1,
        "holds": ["Option " + OPTION + ": '.import math' cannot be a default", "a default is one of the settings .lang,"],
        "lacks": [BANNER, "a b c"],
    },
    {
        "name": "a setting command without its value",
        "before": [OPTION, ".anchors"],
        "lines": "a b c\n",
        "status": 1,
        "holds": ["Option " + OPTION + ": '.anchors' cannot be a default", ".anchors needs the value it is to set"],
        "lacks": [BANNER, "a b c"],
    },
    {
        "name": "a value that the command refuses",
        "before": [OPTION, FACTOR + " abc"],
        "lines": "a b c\n",
        "status": 1,
        "holds": ["Option " + OPTION + ": '" + FACTOR + " abc' cannot be a default", "a number from 0.8 to 0.95"],
        "lacks": [BANNER, "a b c"],
    },
    {
        # A script does not carry out execution once a line associated
        # with the option has failed, even if a preceding line was taken.
        "name": "a line that fails behind one that was taken, ahead of a script",
        "before": [OPTION, ".anchors off", OPTION, ".timer never"],
        "lines": "a b c\n",
        "as_script": True,
        "status": 1,
        "holds": ["Usage: .timer [on|off|auto]"],
        "lacks": ["a b c"],
    },
    {
        # Nothing executes, not even the version report.
        "name": "a line that fails behind -v",
        "before": ["-v", OPTION, ".anchors maybe"],
        "lines": "a b c\n",
        "status": 1,
        "holds": ["Usage: .anchors [on|off]"],
        "lacks": [VERSION_REPORT, BANNER, "a b c"],
    },
    {
        "name": "-v prints the version and starts no session",
        "before": ["-v"],
        "lines": "a b c\n",
        "status": 0,
        "holds": [VERSION_REPORT],
        "lacks": [BANNER, "a b c"],
    },
    {
        "name": "--version prints the version and starts no session",
        "before": ["--version"],
        "lines": "a b c\n",
        "status": 0,
        "holds": [VERSION_REPORT],
        "lacks": [BANNER, "a b c"],
    },
    {
        "name": "-v ahead of a script prints the version and does not run the script",
        "before": ["-v"],
        "lines": RAN,
        "as_script": True,
        "status": 0,
        "holds": [VERSION_REPORT],
        "lacks": ["the script ran"],
    },
    {
        # Behind the name of a script, both words serve as arguments just
        # like any others. For as long as they were read in that location
        # too, such a run printed the version and did not run the script,
        # meaning no script could receive either word.
        "name": "behind the name of a script -v and --version belong to the script",
        "before": [],
        "lines": RAN + ARGUMENTS,
        "as_script": True,
        "after": ["-v", "--version"],
        "status": 0,
        "holds": ["the script ran", "arguments: -v --version"],
        "lacks": [VERSION_REPORT],
    },
    {
        "name": "behind the name of a script --version alone belongs to the script",
        "before": [],
        "lines": ARGUMENTS,
        "as_script": True,
        "after": ["--version"],
        "status": 0,
        "holds": ["arguments: --version"],
        "lacks": [VERSION_REPORT],
    },
    {
        "name": "ZELPH_THREADS names the count of the process",
        "before": [],
        "env": {"ZELPH_THREADS": "3"},
        "lines": SHOW_THREADS,
        "status": 0,
        "holds": [THREADS + "3 (ZELPH_THREADS)"],
    },
    {
        # The command gives the engine a count of its own, and `auto`
        # restores the count to that of the process, which is the
        # variable's.
        "name": ".threads overrides ZELPH_THREADS, and auto returns to it",
        "before": [],
        "env": {"ZELPH_THREADS": "3"},
        "lines": ".threads 5\n" + SHOW_THREADS + ".threads auto\n",
        "status": 0,
        "holds": [THREADS + "5\n", THREADS + "3 (ZELPH_THREADS)"],
    },
    {
        "name": "a default line sets the count of every engine, over ZELPH_THREADS",
        "before": [OPTION, ".threads 2"],
        "env": {"ZELPH_THREADS": "6"},
        "lines": SHOW_THREADS + ".new\n" + SHOW_THREADS,
        "status": 0,
        "count": (THREADS + "2\n", 2),
        "lacks": ["ZELPH_THREADS"],
    },
    {
        # A value that names no count remains unused, and the command
        # displaying the count indicates this: otherwise, a typing
        # error in the variable might be mistaken for the intended
        # count.
        "name": "a ZELPH_THREADS that names no count is not used, and .threads says so",
        "before": [],
        "env": {"ZELPH_THREADS": "many"},
        "lines": SHOW_THREADS,
        "status": 0,
        "holds": ['(automatic; ZELPH_THREADS is "many", which is no number from 1 to 1024)'],
    },
    {
        "name": "ZELPH_THREADS=0 names no count",
        "before": [],
        "env": {"ZELPH_THREADS": "0"},
        "lines": SHOW_THREADS,
        "status": 0,
        "holds": ['(automatic; ZELPH_THREADS is "0", which is no number from 1 to 1024)'],
    },
    {
        "name": "ZELPH_THREADS=auto is the automatic count",
        "before": [],
        "env": {"ZELPH_THREADS": "auto"},
        "lines": SHOW_THREADS,
        "status": 0,
        "holds": ["(automatic)"],
        "lacks": ["ZELPH_THREADS"],
    },
    {
        # The pool took as many threads as the machine possesses CPUs, even
        # in cases where the process was confined to a smaller number. With
        # one CPU, the count remains 1 regardless of what a cgroup quota
        # says, and a run names it.
        "name": "without the variable the count is that of the affinity mask",
        "before": [],
        "cpus": 1,
        "lines": SHOW_THREADS + ".run\n",
        "status": 0,
        "holds": [THREADS + "1 (automatic)", "Starting reasoning with 1 worker thread."],
    },
]


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def run(command, stdin_path, env, cpus):
    narrow = None
    if cpus is not None:
        allowed = sorted(os.sched_getaffinity(0))[:cpus]
        narrow = lambda: os.sched_setaffinity(0, allowed)
    try:
        with open(stdin_path, "rb") as piped:
            done = subprocess.run(command, stdin=piped, capture_output=True, check=False,
                                  encoding="utf-8", errors="replace", timeout=120,
                                  env=env, preexec_fn=narrow)
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
    os.environ.pop("ZELPH_THREADS", None)

    violations = []
    asked = 0

    with tempfile.TemporaryDirectory(prefix="zelph_options_") as directory:
        script = os.path.join(directory, "case.zph")
        nothing = os.path.join(directory, "nothing")
        with open(nothing, "w", encoding="utf-8"):
            pass

        for case in CASES:
            name = case["name"]
            cpus = case.get("cpus")
            if cpus is not None and not hasattr(os, "sched_setaffinity"):
                continue
            asked += 1
            with open(script, "w", encoding="utf-8") as handle:
                handle.write(case["lines"])
            env = dict(os.environ, **case.get("env", {}))

            command = [args.binary] + case["before"]
            if case.get("as_script"):
                command += [script] + case.get("after", [])
                status, output = run(command, nothing, env, cpus)
            else:
                status, output = run(command, script, env, cpus)
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

    print(f"{asked} cases: the binary reads its options as documented.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
