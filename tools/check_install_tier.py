#!/usr/bin/env python3
"""Keep the tests that packages run at install time cheap and broad.

The packages do not run the whole suite on a user's machine. They run
everything outside the "slow" test suite, and this script holds that selection
to three properties:

    breadth    the selection holds at least a given share of all test cases, so
               a budget cannot be met by marking ever more cases slow
    balance    no single case takes more than a given share of the selection, so
               one new heavyweight cannot dominate it again
    budget     the selection stays under a declared wall clock ceiling

Breadth and balance are shares rather than seconds, so they mean the same on a
laptop and on a build machine. The budget is the one absolute number, and it is
declared in CMakeLists.txt beside the test that runs this.

Exit codes: 0 all three hold, 1 one of them does not, 2 the question could not
be put.
"""

import argparse
import re
import subprocess
import sys

DURATION = re.compile(r"^([0-9.]+) s: (.*)$")


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def run(*command):
    try:
        done = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as exc:
        fail(f"cannot run {command[0]}: {exc}")
    if done.returncode != 0:
        fail(f"{' '.join(command)} exited {done.returncode}: {done.stderr.strip()[:400]}")
    return done.stdout


def parse_durations(text):
    found = {}
    for line in text.splitlines():
        match = DURATION.match(line)
        if match:
            found[match.group(2)] = float(match.group(1))
    return found


def count_cases(binary, *filters):
    listing = run(binary, "--list-test-cases", *filters)
    # The listing carries a banner and a rule of equals signs around the names.
    return sum(1 for line in listing.splitlines()
               if line.strip() and not line.startswith("[doctest]") and not line.startswith("==="))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--run", metavar="BINARY", help="the test binary to measure")
    source.add_argument("--from-listing", metavar="FILE", help="a recorded --duration=true listing")
    parser.add_argument("--exclude-suite", default="slow")
    parser.add_argument("--budget", type=float, required=True, help="seconds the selection may take")
    parser.add_argument("--max-share", type=float, default=0.25,
                        help="share of the selection one case may take")
    parser.add_argument("--min-breadth", type=float, default=0.80,
                        help="share of all cases the selection must hold")
    args = parser.parse_args()

    breadth = None
    if args.run:
        durations = parse_durations(
            run(args.run, f"--test-suite-exclude={args.exclude_suite}", "--duration=true"))
        everything = count_cases(args.run)
        selected = count_cases(args.run, f"--test-suite-exclude={args.exclude_suite}")
        if everything == 0:
            fail(f"{args.run} reports no test cases at all")
        breadth = selected / everything
    else:
        try:
            durations = parse_durations(open(args.from_listing, encoding="utf-8").read())
        except OSError as exc:
            fail(f"cannot read {args.from_listing}: {exc}")

    if not durations:
        fail("no timed test cases were found; was --duration=true in effect?")

    total = sum(durations.values())
    slowest, slowest_time = max(durations.items(), key=lambda item: item[1])
    share = slowest_time / total if total else 0.0

    print(f"selection: {len(durations)} cases, {total:.2f} s, "
          f"slowest {slowest_time:.2f} s ({share:.0%} of it)"
          + (f", breadth {breadth:.0%}" if breadth is not None else ""))

    problems = []
    if breadth is not None and breadth < args.min_breadth:
        problems.append(f"holds {breadth:.0%} of all cases, below the required {args.min_breadth:.0%}")
    if share > args.max_share:
        problems.append(f"'{slowest}' takes {share:.0%} of the selection, above the allowed {args.max_share:.0%}")
    if total > args.budget:
        problems.append(f"takes {total:.2f} s, above the declared budget of {args.budget:.2f} s")

    if not problems:
        return 0
    for problem in problems:
        print(f"  the install selection {problem}", file=sys.stderr)
    # Name the way out, because the number in the message is not the interesting
    # part. Whoever reads this has just added a test, and needs to know that
    # marking it is a normal thing to do.
    print("  mark an expensive case with doctest::test_suite(\"slow\"), or raise the declared budget",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
