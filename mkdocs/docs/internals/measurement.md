# Measurement Methodology

The performance results documented on the
[architecture page](performance.md) matter less than the method that
produced them. An inference engine's worst failure mode under optimization
pressure is silent semantic drift — a candidate set that is _almost_
complete, a cache that is _usually_ fresh. This page records the discipline
that kept zelph's optimization work honest: falsifiable predictions, exact
counter identities, bit-stable invariants, and independent semantic nets.
It is written so that future engine changes can be held to the same
standard.

## Falsifiable Predictions First

Every increment starts with a written bet _before_ the measurement: a
predicted timing band, the counters expected to stay bit-identical, and the
counters expected to drift — with the mechanism that explains the drift and
its rough magnitude. The numbers then overrule the bet. Missed predictions
are the valuable ones: a change that speeds nothing up, or a counter that
moves without a mechanism, has just told you your model of the engine is
wrong — which is precisely what the next profile must resolve before more
code is written. Expect calibration to take several misses in both
directions; keep betting anyway.

## Counter Mode and `.prof`

The reasoning profiler (`reasoning_profiler.hpp`) is enabled by `.log -1`:
**counter-only mode**, which accumulates all counters without emitting a
single log line. This matters twice over. First, measurement purity — log
rendering locks and allocates, and at ten thousand deductions it dominates
what you are trying to measure; never profile with `.log 1` or deeper.
Second, production purity — every counter increment is gated on
`logging_active()`, so unlogged runs pay nothing. In counter mode the
window accumulates across statements and imports; `.prof` dumps it on
demand and `.prof reset` starts a fresh window between phases. Use
`.deductions off` as well: rendering large derived terms dominates
wall-clock time otherwise (see
[Deduction Output Modes](../index.md#deduction-output-modes)).

## The Reference Protocol

The standing reference workload is the Jacobian pair
(`dev_scripts/test-jacobian-var` for the differentiation phase,
`dev_scripts/test-jacobian-a` for the determinant phase), run in a fresh
REPL:

```
.deductions off
.log -1
.import dev_scripts/test-jacobian-var
.prof reset
.import dev_scripts/test-jacobian-a
.prof
```

The `-- N ms --` timer after each import is the phase timing; the `.prof`
dumps are the counter protocol. The full test suite (timed as a whole) is
the third standing number. Always compare fresh-REPL runs — counters and
caches accumulate by design.

## Identities and Invariants

Two classes of counter relations are checked on every protocol.

**Exact identities** must hold to the last digit. `fs_cache misses` equals
`genuine hits + genuine walks` — every cache miss is answered by exactly one
of the two layers below it. On store-armed workloads, all three walk
counters (`genuine walks`, `var_closure walk_fallbacks`, `template_vars
walks`) are **zero**; any nonzero value means the disarm funnel fired
mid-run, and the measurement stops until the trigger is understood.

**Hard semantic invariants** must be bit-identical across any
semantics-neutral change: `facts_created`, `seminaive_seeds`,
`extract ok` and `template_rejects`, the entire `evaluate`, `negation`,
`check_fact` and `termination_guard` blocks, and
`seminaive_safety_extra=0`. These counters describe _what_ was derived and
_which_ decisions the engine took; an optimization that moves them has
changed semantics, whatever the test suite says.

## Drift Families

Counters outside the invariant set drift, and every family has a known
mechanism: `parallel=` and `scanned(par)` vary with nondeterministic
parallel launches; `scanned(seq)`, `snapshot_facts` and the `unify()`
family drift by dozens because the parallel match-queue drain order
perturbs fact-creation _order_, so snapshots taken mid-run see slightly
different candidate sets; `fs_cache` hits/misses and `stale_erased` drift
by a few thousand for the same reason. The rule is not "small drift is
fine" — it is **every drift needs a mechanism**. A change may legitimately
move a counter far outside its family when the mechanism predicts it
exactly (hoisting the pattern decomposition out of the `Unification`
constructor removed one to two cache probes per seed, and `fs_cache hits`
dropped by the predicted several hundred thousand). Unexplained drift, of
any size, is a finding.

## The Semantic Nets

Counter protocols catch regressions in the measured workload; three
independent nets catch them everywhere else. The test suite runs
permanently in `.semi-naive check` mode, so every case is verified by
classic evaluation passes against the seeded fixpoint — the completeness
net for the entire delta/anchoring machinery, and the reason "accepted
divergence class" is an admissible phrase on the architecture page at all.
`.anchors off` provides the anchor-free naive reference for suspected
anchoring bugs. And the suite itself multiplies coverage across both
parallelism modes and all three arithmetic substrates, so
representation-agnosticism is continuously enforced rather than assumed.

## CPU Profiles

Wall-clock attribution comes from `perf` on a dedicated build tree with
release flags plus `-g`:

```
# nushell
".deductions off\n.import dev_scripts/test-jacobian-var\n.quit\n"
  | perf record -o perf-diffby.data build-prof/bin/zelph

# bash / zsh
printf '.deductions off\n.import dev_scripts/test-jacobian-var\n.quit\n' \
  | perf record -o perf-diffby.data build-prof/bin/zelph

perf report -i perf-diffby.data --no-children --percent-limit 0.5 --stdio | head -n 80
```

On hybrid CPUs, judge by the `cpu_core` block: the `cpu_atom` block is
dominated by pool-worker idle churn (futex and scheduler symbols) and its
kernel lines push the interesting samples down — hence the generous
`head -n 80`. Read profiles together with the counters: self-time tells you
_where_ cycles go, counters tell you _how often_ and _whether the work was
necessary_; conclusions drawn from only one of the two have repeatedly been
wrong.

## Known Honesty Gaps

The profiler does not count `get_fact_structures` calls on the parallel
scan path (rare, and visible as a small calls/scanned discrepancy).
Sub-second phase timings carry tens of milliseconds of run-to-run noise —
compare bands, not single runs. And at the current state, script parsing
(the Janet/PEG layer) is a visible floor of the reference timings: a
measurement that "improves" it without touching the parser is measuring
noise. When the honest expected gain of the next increment drops into that
noise floor, the correct optimization is to stop.
