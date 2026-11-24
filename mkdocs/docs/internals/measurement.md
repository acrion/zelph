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
of the two layers below it. On a store-armed workload that reads no node
written by a load, a trusted import, or a name merge, all three walk
counters (`genuine walks`, `var_closure walk_fallbacks`, `template_vars
walks`) are **zero**; any nonzero value there means the disarm funnel fired
mid-run, and the measurement stops until the trigger is understood.

**Hard semantic invariants** must stay bit-identical across any change that does not affect semantics: `facts_created`, `deduce_calls`, `seminaive_seeds`, `extract ok`, and `template_rejects`, the entire `evaluate`, `negation`, `check_fact`, and `termination_guard` blocks, and `seminaive_safety_extra=0`. These counters describe _what_ was derived and _which_ decisions the engine made; an optimization that moves them changes semantics, regardless of the test suite’s verdict. The full set remains bit-identical across runs, under the default configuration as well as with `.parallel` off (refer to [Drift Families](#drift-families)).

**The current values.** During a standalone sequential run of the Jacobian example (`.parallel` off, `.semi-naive on`, `.log -1`, `.deductions off`, `.import examples/math/jacobian`, `.prof`), the set has the values listed below, formatted as `.prof` writes them and excluding the counters that do not belong to the set. Each line corresponds to a single line in the dump, except `extract:`, which lacks a dedicated line there: it forms the second half of the line `get_fact_structures:`. The test suite holds that run to each of them in `src/test/test_performance_guard.cpp`, and the ctest check `measurement_page_names_the_values_the_guard_holds` fails if this block and that file disagree.

<!-- tools/check_invariant_values.py compares this block with the sequential case in src/test/test_performance_guard.cpp. -->

```
deduce_calls=39869 facts_created=15406 seminaive_seeds=352931 seminaive_safety_extra=0
evaluate: calls=135611 leaf_conditions=123425 conjunction_sets=520 optimize_order=58291
extract: ok=165298 template_rejects=8532
check_fact: known=24463 new=15406 wrong=0
negation: cond=1379 ok=460 fail=919
termination_guard: checks=0 skips=0 fresh_vars=0 fresh_nodes=0
```

**A recorded change.** In version 1.1.0, two counters within this set were intentionally altered. The reference protocol’s dump, which covers its second phase, remains identical down to the final digit; its first phase, responsible for importing the rules, underwent the same shift (`template_rejects` from 3,137 to 2,733, `extract ok` from 12,349 to 12,347), as did the standalone, sequential execution of the Jacobian example (`.parallel` off, `.semi-naive on`, `.log -1`, `.deductions off`, `.import examples/math/jacobian`, `.prof`). In that run, `extract ok` decreased from 167,042 to 167,040 and `template_rejects` from 9,198 to 8,705, with `extract calls` (824,248 to 802,793) and `failS` (517,151 to 496,191) following suit, while `facts_created` (15,406), `deduce_calls` (39,866), `seminaive_seeds`, and the `evaluate`, `negation`, `check_fact`, and `termination_guard` blocks stayed bit-identical. The underlying mechanism: unification now checks, before extracting a candidate, whether a `=>` fact holds a variable anywhere in its rule text (`is_rule_text`, `unification.cpp`), including conditions attached to a conjunction set or a collection, which the closures of subject and consequence do not reach. Of the 21,455 readings it skips, 20,960 would have failed on the subject, 493 were rule templates previously rejected and counted during extraction, and 2 were accepted despite being rule text: rules whose variables appear only in regions unreachable by those closures were interpreted as facts. Fewer extractions result in fewer `unify()` calls and cache probes, and each `=>` reading pays a `var_closure` lookup instead, causing `var_closure queries` to increase. The dump also introduced a new line, `construction:`, for the rules a generator writes: `built` tracks the constructions that assembled their components within a scratch cluster, `remembered` counts those that returned a rule previously claimed by an earlier construction, and `fingerprinted` records the rules read by the fingerprint index, which expands with the number of rules written, not with the volume of reasoning (430 in the Jacobian example).

**A second recorded change.** Subsequently in version 1.1.0, `polynomial.zph` gained the rule `(V pouter W, W pouter V, V != W) => !`, which reports a cycle in the variable ordering (refer to [Variable order](../math/polynomial.md#variable-order)). It derives nothing on these workloads – `facts_created` (15,406), `check_fact new` and the `negation` and `termination_guard` blocks stayed bit-identical – however, the invariant set shifted once more. In the standalone sequential Jacobian run (the commands listed above), `extract ok` went from 167,040 to 165,298, `template_rejects` from 8,705 to 8,648, `extract calls` from 802,793 to 799,107, `failS` from 496,191 to 495,108, `evaluate calls` from 137,779 to 136,037, `deduce_calls` from 39,866 to 39,869, `seminaive_seeds` from 352,925 to 352,931 and `check_fact known` from 24,460 to 24,463; the run summary went from 105,904 to 104,159 matches processed, similarly in the parallel default configuration. In the reference protocol, the dump from the second phase moved (`template_rejects` from 3,056 to 3,004, `extract ok` from 111,459 to 110,121, while `facts_created`, `deduce_calls` and `seminaive_seeds` remained unchanged), and so did the first phase (`template_rejects` from 2,733 to 2,737, `extract ok` from 12,347 to 12,627). The cause lies in the shift in node numbering, not the rule’s own matching behaviour: the identical rule applied to a predicate unused by any fact, positioned identically, shifts the Jacobian run nearly as far (`extract ok` 165,490, 104,255 matches processed), and the rule’s direct contribution amounts to six additional `seminaive_seeds`. Later in version 1.1.0, a statement began to assert the facts nested in it (refer to [Parentheses: Nested Facts](../concepts.md#parentheses-nested-facts)); in the same run, this moved `extract calls` to 799,221 and `failS` to 495,216, while every counter of the invariant set kept the value specified previously. Subsequently, a statement that writes a rule ceased to be evaluated as a query as well, since its variables belong to the rule (refer to [Querying in zelph](../queries.md)). Of the 430 rules written by the math stack and the example, the 426 that contain a variable had each incurred a single `evaluate` call on a leaf condition in this way, an evaluation that derived nothing (a rule without a variable had never been evaluated as a query): in the same run, `evaluate calls` went from 136,037 to 135,611 and `leaf_conditions` from 123,851 to 123,425, `extract calls` to 798,978 and `failS` to 494,973, while every other counter of the invariant set kept its value, as did the run summary. A comparison across this version begins with these values.

**A third recorded change.** Later in version 1.1.0, a condition with a fixed relation and fully bound subject and objects began to be answered by a single exact-triple probe rather than a walk through the subject's adjacency, and a condition bound at both ends that no probe answers is now read from the smaller of the two adjacencies (see [Candidate Sets: Anchoring and Semi-Naive Seeding](performance.md#candidate-sets-anchoring-and-semi-naive-seeding)). One counter of the invariant set moved. In the standalone sequential Jacobian run, `template_rejects` went from 8,648 to 8,532, while `facts_created` (15,406), `deduce_calls` (39,869), `seminaive_seeds` (352,931), `extract ok` (165,298), and the `evaluate`, `negation`, `check_fact` and `termination_guard` blocks stayed bit-identical, as did the run summary (104,159 matches processed). The probe delivers solely the fact the condition denotes, whereas the walk previously delivered every fact of the condition's relation within the subject's adjacency, including rule patterns that extraction had rejected as templates. Compared to the parent of this change, `extract calls` and `scanned(seq)` went from 798,978 to 722,255, `failS` from 494,973 to 465,213, `failO` from 126,164 to 79,511, and `fs_cache misses` from 416,732 to 388,047. In the reference protocol, `template_rejects` went from 2,737 to 2,731 in the first phase and from 3,004 to 2,947 in the second, `extract ok` (12,627 and 110,121), `facts_created`, `deduce_calls`, and `seminaive_seeds` remaining unchanged. The dump now includes the line `anchors:`, which tracks the cost of reading candidates before any were attempted: `walked`, the adjacency entries read by subject- and object-driven anchors; `climbed`, the entries read by the climbs of partial anchors; and `exact_probes`. In the sequential Jacobian run, these are 2,877,924, 1,678,159, and 24,179; an instrumented build of the parent walked 8,132,826 entries and climbed 1,678,159, matching exactly the number this build climbs. `scanned(seq)` counts only the candidates retained by a walk, so the walk accounted for the portion of the cost not reflected in any counter: in the stdlib's comparisons, it grew with every number in the graph while `scanned(seq)` remained constant. In the parallel default, `walked` and `climbed` form part of the drift family of `scanned(seq)` and vary by a small number of entries across runs; with `.parallel` off, they repeat precisely. A comparison across this version begins with these values.

**A fourth recorded change.** Later in version 1.1.0, the scan of a relation’s whole extent began to test a reading after decoding the candidate, rather than testing the candidate’s edges before decoding it (it now examines the edges solely for a reading that names the scanned relation and does not hash back to the candidate), the scan of a relation containing fewer than 4,096 facts began to stay on the asking thread irrespective of the `.threads` setting, and a scan spread over threads started delivering its matches in the sequence in which one thread finds them. No counter of the invariant set was altered. In the standalone sequential Jacobian run, `scanned(seq)` and `get_fact_structures: calls` went from 722,255 to 722,841, while `extract calls` stayed at 722,255: the further 586 candidates are facts whose subject is the scanned relation, which previously were excluded by the edge test and are now rejected during decoding due to their predicate. These are looked up in the cache just like every other candidate, so `fs_cache hits` rose from 2,757,939 to 2,758,393 and `misses`, along with `genuine hits`, from 388,047 to 388,179. In the reference protocol, with `.parallel` disabled, `scanned(seq)` and `get_fact_structures: calls` rose from 66,021 to 66,230 in the first phase and from 492,801 to 493,022 in the second, `fs_cache hits` from 184,175 to 184,326 and from 1,859,832 to 1,860,046, and `misses`, along with `genuine hits`, from 13,216 to 13,274 and from 360,287 to 360,294; all other counters preserved their values. Under the default configuration, the protocol’s dump had previously varied from run to run across every counter family outside the hard invariants; since this change, it now repeats down to the final digit and equals the dump of a run with `.parallel` disabled, as none of the protocol’s relations reaches 4,096 facts or more.

## Drift Families

Under the reference protocol, no counter experiences drift: runs executed under the default setting produce the same dump digit for digit, and a run with `.parallel` disabled yields that same dump as well. None of the protocol’s relations reaches the 4,096 facts that trigger a scan to be spread over threads, so each scan remains confined to the asking thread. A scan that is spread moves counters against the same scan on one thread in two distinct manners, and each movement has its own mechanism. Three such movements occur by definition: `parallel=` tracks the number of launches, `scanned(par)` takes the candidates of such a scan from `scanned(seq)`, and `get_fact_structures: calls` counts only the candidates scanned on the asking thread. A few additional counters may shift because a spread scan extracts every binding of its relation before the first of its matches fires, whereas a scan on one thread reads each candidate only after the matches before it have fired: `snapshot_facts`, `extract calls`, `template_rejects`, the `unify()` family, `var_closure queries`, and `walked` then exhibit minor discrepancies compared to the run on a single thread. `template_rejects` is thus the sole hard invariant altered by a spread scan (by 5 in the Jacobian example, assessed using a build where the threshold is 0: with the threshold as shipped, none of that example’s scans is spread); the other hard invariants, `extract ok`, and the number of matches processed remain unchanged. Between two runs featuring spread scans, only `fs_cache` hits and misses and `genuine hits` differ, by a small margin: their total sum is identical, and when two threads miss one structure simultaneously, both count the miss. The principle is not "small drift is fine" – it is **every drift needs a mechanism**. A modification may rightly shift a counter significantly beyond its family if the mechanism predicts it precisely (hoisting the pattern decomposition out of the `Unification` constructor reduced cache probes per seed by one to two, and `fs_cache hits` declined by the predicted several hundred thousand). Unexplained drift, of any magnitude, constitutes a finding.

The number of matches processed in a run’s summary (`N matches processed`) is not a `.prof` counter and does not belong to the invariant set. It counts the bindings produced by the unification search, whether they were scanned or seeded (refer to [What the run summary counts](../rules.md#what-the-run-summary-counts)), and thus varies according to the evaluation mode: `.semi-naive check` adds the matches from its safety pass. Only compare this value within the same mode; within one mode it remains consistent across every run.

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

On hybrid CPUs, judge by the `cpu_core` block: the `cpu_atom` block holds
only the few samples from the moments the process spent on an efficiency
core, and its kernel lines push the interesting samples down — hence the
generous `head -n 80`. Read profiles together with the counters: self-time
tells you _where_ cycles go, counters tell you _how often_ and _whether the
work was necessary_; conclusions drawn from only one of the two have
repeatedly been wrong.

## Known Honesty Gaps

`get_fact_structures: calls` tallies solely the invocations generated by the sequential candidate scan (it always equals `scanned(seq)`); the parallel scan and every other caller are not counted. `fs_cache` hits + misses counts every structure retrieval that gets past the atom/variable gate, regardless of whether it originates from either scan path or elsewhere. Sub-second phase timings carry tens of milliseconds of run-to-run noise – compare ranges, not single runs. At present, script parsing (the Janet/PEG layer) is a visible floor of the reference timings: a measurement that "improves" it without touching the parser is measuring noise. Once the honest anticipated benefit of the next enhancement falls beneath this noise floor, the appropriate step is to cease further optimization.
