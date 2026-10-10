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
[Deduction Output Modes](../rules.md#deduction-output-modes)).

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

**Hard semantic invariants** must stay bit-identical across any change that does not affect semantics: `facts_created`, `seminaive_seeds`, `extract ok`, and `template_rejects`, the entire `evaluate`, `negation`, `check_fact`, and `termination_guard` blocks, and `seminaive_safety_extra=0`. These counters describe _what_ was derived and _which_ decisions the engine made; an optimization that moves them changes semantics, regardless of the test suite’s verdict. The full set remains bit-identical only during a sequential run (`.parallel` off). In the parallel default, `evaluate calls` and `leaf_conditions`, `extract ok` and `check_fact known` form a drift family (explained further below). The rest of the set also preserves bit-identity in that configuration: `facts_created`, `seminaive_seeds`, `template_rejects`, `evaluate conjunction_sets` and `optimize_order`, the `negation` and `termination_guard` blocks, `check_fact new` and `wrong`, and `seminaive_safety_extra=0`. Check the four drifting ones in a standalone run with `.parallel` disabled, not by executing the whole protocol sequentially: its timing measurements apply to the parallel default.

**A recorded change.** In version 1.0.2, two counters within this set were intentionally altered. The reference protocol’s dump, which covers its second phase, remains identical down to the final digit; its first phase, responsible for importing the rules, underwent the same shift (`template_rejects` from 3,137 to 2,733, `extract ok` from 12,349 to 12,347), as did the standalone, sequential execution of the Jacobian example (`.parallel` off, `.semi-naive on`, `.log -1`, `.deductions off`, `.import examples/math/jacobian`, `.prof`). In that run, `extract ok` decreased from 167,042 to 167,040 and `template_rejects` from 9,198 to 8,705, with `extract calls` (824,248 to 802,793) and `failS` (517,151 to 496,191) following suit, while `facts_created` (15,406), `deduce_calls` (39,866), `seminaive_seeds`, and the `evaluate`, `negation`, `check_fact`, and `termination_guard` blocks stayed bit-identical. The underlying mechanism: unification now checks, before extracting a candidate, whether a `=>` fact holds a variable anywhere in its rule text (`is_rule_text`, `unification.cpp`), including conditions attached to a conjunction set or a collection, which the closures of subject and consequence do not reach. Of the 21,455 readings it skips, 20,960 would have failed on the subject, 493 were rule templates previously rejected and counted during extraction, and 2 were accepted despite being rule text: rules whose variables appear only in regions unreachable by those closures were interpreted as facts. Fewer extractions result in fewer `unify()` calls and cache probes, and each `=>` reading pays a `var_closure` lookup instead, causing `var_closure queries` to increase. A comparison across this version begins with the updated values. The dump also introduced a new line, `construction:`, for the rules a generator writes: `built` tracks the constructions that assembled their components within a scratch cluster, `remembered` counts those that returned a rule previously claimed by an earlier construction, and `fingerprinted` records the rules read by the fingerprint index, which expands with the number of rules written, not with the volume of reasoning (429 in the Jacobian example).

## Drift Families

Counters lying beyond the invariant set exhibit drift, and each family possesses a known mechanism: `parallel=` and `scanned(par)` fluctuate due to nondeterministic parallel launches; `scanned(seq)`, `snapshot_facts`, and the `unify()` family drift by several dozen because the unpredictable order in which the parallel match-queue is drained alters the _order_ of fact generation, causing snapshots captured mid-run to observe marginally distinct candidate sets; `fs_cache` hits/misses and `stale_erased` drift by several thousand for the same underlying cause. The identical drain sequence allows a small number of deductions to reach conclusions that were already established, thereby shifting `deduce_calls`, `evaluate calls` and `leaf_conditions`, `extract calls`, `ok` and `failS`, and `check_fact known` by a modest amount: under the reference protocol, the second phase manifests in two stable forms, the less common one featuring two additional `deduce_calls` and two extra `check_fact known`. When `.parallel` is disabled, these counters repeat precisely. The principle is not "small drift is fine" – it is **every drift needs a mechanism**. A modification may rightly shift a counter significantly beyond its family if the mechanism predicts it precisely (hoisting the pattern decomposition out of the `Unification` constructor reduced cache probes per seed by one to two, and `fs_cache hits` declined by the predicted several hundred thousand). Unexplained drift, of any magnitude, constitutes a finding.

The number of matches processed in a run’s summary (`N matches processed`) is not a `.prof` counter and does not belong to the invariant set. It counts the bindings produced by the unification search, whether they were scanned or seeded (refer to [What the run summary counts](../rules.md#what-the-run-summary-counts)), and thus varies according to the evaluation mode: `.semi-naive check` adds the matches from its safety pass. Only compare this value within the same mode. When `.parallel` is disabled, the count remains identical; under the default parallel setting, it drifts in tandem with `deduce_calls`.

## The Semantic Nets

Counter protocols detect regressions within the measured workload; three independent nets detect them in all other scenarios. The test binary sets up each engine in `.semi-naive check` mode, ensuring that every case not explicitly choosing a mode undergoes validation via classic evaluation passes against the seeded fixpoint – the completeness net for the entire delta/anchoring system, and the justification for why "accepted divergence class" is a permissible term on the architecture page at all. Check mode does not run the classic evaluator anew; only the small subset of tests that directly contrast `.semi-naive on` with `.semi-naive off` carry out such a fresh run. This mode serves as the inherent default of the test binary, not of a test helper. Through version 1.0.1, only the cases that went through `run_both_modes` and its sibling helpers utilized it: 173 out of the 799 cases at that time constructed their engine directly, and among those 55 that ran inference, all ran outside this net. The identical mode also serves as the net for negation: it re-tests every negation that a newer fact could have refuted, a defect class where classic and semi-naive evaluation agree, rendering the re-test the exclusive method of detection. Rules featuring a neural condition, a negated path condition, or a path condition that binds a variable not bound by anything else are excluded from the re-test (see [Stratified Evaluation](../logic.md#stratified-evaluation)). `.anchors off` provides the anchor-free naive reference for suspected anchoring issues. Furthermore, the suite multiplies coverage across both parallelism modes and all three arithmetic substrates, ensuring representation-agnosticism is continuously enforced rather than merely presumed.

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

`get_fact_structures: calls` tallies solely the invocations generated by the sequential candidate scan (it always equals `scanned(seq)`); the parallel scan and every other caller are not counted. `fs_cache` hits + misses counts every structure retrieval that gets past the atom/variable gate, regardless of whether it originates from either scan path or elsewhere. Sub-second phase timings carry tens of milliseconds of run-to-run noise – compare ranges, not single runs. At present, script parsing (the Janet/PEG layer) is a visible floor of the reference timings: a measurement that "improves" it without touching the parser is measuring noise. Once the honest anticipated benefit of the next enhancement falls beneath this noise floor, the appropriate step is to cease further optimization.
