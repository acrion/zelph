#!/usr/bin/env python3
# dev_scripts/paper/s9_timings.py
"""Per-claim work measurements for the EML paper's timing table.

Usage:  ./s9_timings.py [path-to-zelph-binary] [repeats]
        ./s9_timings.py --self-test
"""

import re
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / ".." / ".." / "build-release" / "bin" / "zelph"
REPEATS = int(sys.argv[2]) if len(sys.argv) > 2 else 5

REPORT = re.compile(r"Reasoning complete in 0h0m([\d.]+)s . (\d+) matches")
IMPORT_MS = re.compile(r"import-ms ([\d.]+)")

# Shared prologues. `.auto-run` takes no parameter and toggles; the initial
# state is enabled, meaning a single invocation disables it. Afterwards, no
# inference occurs until a `.run` or `.run-delta` explicitly requests it, and
# each such command prints a single report.
NAND = [".deductions off", ".auto-run", ".import binary-nand-arithmetic", ".run"]
EML = [".deductions off", ".auto-run",
       ".import decimal-arithmetic", ".import symbolic-core", ".import eml",
       "x ~ symvar", "y ~ symvar", ".run"]
SYM = [".deductions off", ".auto-run",
       ".import decimal-arithmetic", ".import symbolic-core", "x ~ symvar", ".run"]

# The session associated with the import row. Janet's monotonic clock times
# the import exclusively, within the session. The REPL's statement timer is
# unable to do so: it prints only from 10 ms on (kReplTimingThreshold,
# src/app/main.cpp), and the import duration is roughly that long, so most
# runs printed none. The query following the import pins the row's label:
# with auto-run disabled, no prior derivation has derived the gate table, so
# the query must stay unanswered. An answer would imply that inference
# executed, and the row would time more than parse and load.
IMPORT_SESSION = [".deductions off", ".auto-run",
                  '%(let [t0 (os/clock :monotonic)] (zelph/import "binary-nand-arithmetic") '
                  '(printf "import-ms %.3f" (* 1000 (- (os/clock :monotonic) t0))))',
                  "(0 nand 1) out X"]

COMPILED_SUB = "((&1 eml ((&1 eml x) eml &1)) eml (y eml &1))"

# Each row: (label, script lines, count of the session's most recent
# reports that pertain to the claim, evidence script).
BOOTSTRAP = ("NAND bootstrap and table synthesis (first `.run` after the import)", NAND, 1, "s5")
CASES = [
    ("$12 \\times 34$, NAND substrate",
     NAND + ["(&12 * &34) = X", ".run-delta", "(&12 * &34) = X"], 1, "s5"),
    (":testprime &13, NAND substrate",
     NAND + [".import primes-naf", ".run", "(:testprime &13) = X", ".run-delta",
             "(:testprime &13) = X"], 1, "s5"),
    ("Eq. (5) derivation",
     EML + [":simplify (&1 eml ((&1 eml x) eml &1))", ".run-delta",
            "(:simplify (&1 eml ((&1 eml x) eml &1))) = X"], 1, "s1"),
    ("compile + simplify round trip, x - y",
     EML + [":emlcompile (x - y)", ".run-delta",
            ":simplify " + COMPILED_SUB, ".run-delta",
            "(:simplify " + COMPILED_SUB + ") = X"], 2, "s3"),
    ("cascaded constant folding, (5)*(10) materialized mid-run",
     SYM + [":simplify ((&2 + &3) * (&4 + &6))", ".run-delta",
            "(:simplify ((&2 + &3) * (&4 + &6))) = X"], 1, "s6"),
]


def session(lines):
    """Run one piped session; return its (stdout, stderr)."""
    # Input bytes, output UTF-8, both intentional. A text-mode pipe decodes
    # using the locale's codec: on Windows this interprets the en dash in the
    # run report as three other characters, causing REPORT to find nothing,
    # or it fails when encountering a byte that the codec cannot map to a
    # character. It would also send CRLF line ends.
    p = subprocess.run([str(BIN)], input=("\n".join(lines) + "\n").encode("utf-8"),
                       capture_output=True)
    return tuple(stream.decode("utf-8", "replace").replace("\r\n", "\n")
                 for stream in (p.stdout, p.stderr))


def run(lines):
    """Run one piped session; return (reports, answers)."""
    out, err = session(lines)
    return REPORT.findall(err), re.findall(r"Answer: (.*)", out)


def claim_reports(reports, lines, n_claim):
    r"""The reports that belong to the claim: the session's last n_claim.

    Every `.run` and `.run-delta` line prints one report, and with auto-run
    off nothing else does, so any other count means the session did not do
    what its lines say, and no report can be attributed.

    The bootstrap row claims the prologue's own `.run`:

    >>> claim_reports([("0.012", "780")], NAND, 1)
    [('0.012', '780')]

    A claim after the prologue takes only its delta:

    >>> claim_reports([("0.012", "780"), ("0.005", "466")],
    ...               NAND + ["(&12 * &34) = X", ".run-delta"], 1)
    [('0.005', '466')]

    and a claim of two deltas takes both:

    >>> claim_reports([("0.010", "1"), ("0.001", "2"), ("0.002", "3")],
    ...               EML + [":emlcompile (x - y)", ".run-delta",
    ...                      ":simplify (x eml &1)", ".run-delta"], 2)
    [('0.001', '2'), ('0.002', '3')]

    A missing report is an error, not a shorter claim:

    >>> claim_reports([("0.012", "780")], NAND + ["(&12 * &34) = X", ".run-delta"], 1)
    Traceback (most recent call last):
    ...
    SystemExit: expected 2 report(s), got 1; did the session fail? run it by hand to see
    """
    expected = sum(1 for line in lines if line in (".run", ".run-delta"))
    if len(reports) != expected:
        raise SystemExit(f"expected {expected} report(s), got {len(reports)}; "
                         "did the session fail? run it by hand to see")
    return reports[len(reports) - n_claim:]


def parse_import_ms(output):
    r"""The import time in ms that the import session printed, or None.

    >>> parse_import_ms("zelph-> import-ms 9.310\nzelph-> (0 nand 1) out X\n")
    9.31
    >>> parse_import_ms("zelph-> (0 nand 1) out X\n") is None
    True
    """
    hit = IMPORT_MS.search(output)
    return float(hit.group(1)) if hit else None


def measure(lines, n_claim):
    """Median wall-clock and matches of the claim's report(s)."""
    times, matches, answers = [], [], set()
    for _ in range(REPEATS):
        reports, ans = run(lines)
        claim = claim_reports(reports, lines, n_claim)
        times.append(sum(float(t) for t, _ in claim) * 1000.0)
        matches.append(sum(int(m) for _, m in claim))
        answers.add(ans[-1] if ans else "NO ANSWER")
    return statistics.median(times), statistics.median(matches), sorted(answers)


def measure_import():
    """The import row: parse and load only, inference deferred (IMPORT_SESSION)."""
    samples = []
    for _ in range(REPEATS):
        out, err = session(IMPORT_SESSION)
        ms = parse_import_ms(out)
        if ms is None:
            raise SystemExit("the import session printed no import-ms line:\n" + out + err)
        if "Answer:" in out:
            raise SystemExit("inference ran in the import session, so the row would "
                             "time more than parse and load:\n" + out + err)
        samples.append(ms)
    return statistics.median(samples)


def self_test():
    import doctest
    result = doctest.testmod()
    if result.attempted == 0:
        raise SystemExit("self-test: no examples found")
    print(f"self-test: {result.attempted - result.failed} of {result.attempted} examples passed")
    return 1 if result.failed else 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    if not BIN.is_file():
        raise SystemExit(f"zelph binary not found: {BIN}")
    print(f"<!-- {REPEATS} runs per row, fresh process each; median reported. -->")
    print()
    print("| workload | matches | time | script |")
    print("|---|---|---|---|")
    print(f"| import `binary-nand-arithmetic` (parse and load; inference deferred) "
          f"| n/a | {measure_import():.1f} ms | s5 |")
    label, lines, n_claim, script = BOOTSTRAP
    ms, matches, _ = measure(lines, n_claim)
    print(f"| {label} | {matches:.0f} | {ms:.0f} ms | {script} |")
    observed = []
    for label, lines, n_claim, script in CASES:
        ms, matches, answers = measure(lines, n_claim)
        print(f"| {label} | {matches:.0f} | {ms:.0f} ms | {script} |")
        if len(answers) != 1:
            print(f"| WARNING: answer not stable: {answers} | | | |")
        observed.append((label, answers))
    print()
    print("Answers observed (each stable across all runs):")
    for label, answers in observed:
        print(f"- {label}: `{answers[0]}`")


if __name__ == "__main__":
    main()
