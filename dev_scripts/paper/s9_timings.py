#!/usr/bin/env python3
# dev_scripts/paper/s9_timings.py
"""Per-claim work measurements for the EML paper's timing table.

Usage:  ./s9_timings.py [path-to-zelph-binary] [repeats]
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
STMT_TIMER = re.compile(r"-- (\d+) ms --")

# Shared prologues. `.auto-run` takes no argument and toggles; the default is
# on, so one occurrence turns it off.
NAND = [".deductions off", ".auto-run", ".import binary-nand-arithmetic", ".run"]
EML = [".deductions off", ".auto-run",
       ".import decimal-arithmetic", ".import symbolic-core", ".import eml",
       "x ~ symvar", "y ~ symvar", ".run"]
SYM = [".deductions off", ".auto-run",
       ".import decimal-arithmetic", ".import symbolic-core", "x ~ symvar", ".run"]

COMPILED_SUB = "((&1 eml ((&1 eml x) eml &1)) eml (y eml &1))"

# name -> (script lines, how many trailing .run-delta reports belong to the
# claim, section label, evidence script)
CASES = [
    ("$12 \\times 34$ with full provenance, NAND substrate",
     NAND + ["(&12 * &34) = X", ".run-delta", "(&12 * &34) = X"], 1, "s5"),
    (":testprime &13, NAND substrate",
     NAND + [".import primes-naf", ".run", "(:testprime &13) = X", ".run-delta",
             "(:testprime &13) = X"], 1, "s5"),
    ("Eq. (5) derivation",
     EML + [":simplify (&1 eml ((&1 eml x) eml &1))", ".run-delta",
            "(:simplify (&1 eml ((&1 eml x) eml &1))) = X"], 1, "s1"),
    ("K=5 witness recovery (compile + simplify x - y)",
     EML + [":emlcompile (x - y)", ".run-delta",
            ":simplify " + COMPILED_SUB, ".run-delta",
            "(:simplify " + COMPILED_SUB + ") = X"], 2, "s3"),
    ("cascaded constant folding, (5)*(10) materialized mid-run",
     SYM + [":simplify ((&2 + &3) * (&4 + &6))", ".run-delta",
            "(:simplify ((&2 + &3) * (&4 + &6))) = X"], 1, "s6"),
]


def run(lines):
    """Run one piped session; return (reports, answers)."""
    p = subprocess.run([str(BIN)], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    return REPORT.findall(p.stderr), re.findall(r"Answer: (.*)", p.stdout)


def measure(lines, n_delta):
    """Median wall-clock and matches of the claim's .run-delta report(s)."""
    n_prior = sum(1 for line in lines if line == ".run")
    times, matches, answers = [], [], set()
    for _ in range(REPEATS):
        reports, ans = run(lines)
        claim = reports[n_prior:n_prior + n_delta]
        if len(claim) != n_delta:
            raise SystemExit(f"expected {n_delta} delta report(s), got {len(claim)}; "
                             "did the session fail? run it by hand to see")
        times.append(sum(float(t) for t, _ in claim) * 1000.0)
        matches.append(sum(int(m) for _, m in claim))
        answers.add(ans[-1] if ans else "NO ANSWER")
    return statistics.median(times), statistics.median(matches), sorted(answers)


def measure_import():
    """The import row: per-statement timer, parse time included by design."""
    samples = []
    for _ in range(REPEATS):
        p = subprocess.run([str(BIN)],
                           input=".deductions off\n.auto-run\n.import binary-nand-arithmetic\n",
                           capture_output=True, text=True)
        hit = STMT_TIMER.search(p.stderr)
        if not hit:
            raise SystemExit("the import printed no statement timer -- it now "
                             "runs faster than kReplTimingThreshold, so this "
                             "row needs a different instrument")
        samples.append(int(hit.group(1)))
    return statistics.median(samples)


def main():
    if not BIN.is_file():
        raise SystemExit(f"zelph binary not found: {BIN}")
    print(f"<!-- {REPEATS} runs per row, fresh process each; median reported. -->")
    print()
    print("| workload | matches | time | script |")
    print("|---|---|---|---|")
    print(f"| import `binary-nand-arithmetic` (parse + bootstrap + synthesis) "
          f"| n/a | {measure_import():.0f} ms | s5 |")
    for label, lines, n_delta, script in CASES:
        ms, matches, answers = measure(lines, n_delta)
        print(f"| {label} | {matches:.0f} | {ms:.0f} ms | {script} |")
        if len(answers) != 1:
            print(f"| WARNING: answer not stable: {answers} | | | |")
    print()
    print("Answers observed (each stable across all runs):")
    for label, lines, n_delta, _ in CASES:
        _, _, answers = measure(lines, n_delta)
        print(f"- {label}: `{answers[0]}`")


if __name__ == "__main__":
    main()
