# What zelph Already Provides

The ACE front end does not add a reasoner into zelph. Each ACE sentence transforms into a standard zelph statement or rule, and zelph's existing engine derives the consequences, refutes what is denied, detects contradictions, explains its conclusions, and answers the questions. What the front end needs from the engine is therefore not new logic. It needs the services that any software converting its own language into zelph depends upon: a way to receive a block of text, a way to write a rule exactly once, an inference run that returns its findings as data rather than outputting them in zelph syntax, a scratch space that can be discarded without residue, lookups that create nothing, and a few representations – refutations, statements about statements, variable-containing sets, proofs – that operate identically on every route a program can take.

Every one of these was developed within zelph before the creation of any ACE-specific code, serving as general engine capabilities, each accompanied by its own set of tests and documented on the current user-facing pages, and they form part of zelph 1.1.0. This page presents them: the function of each, the reason the front end needs it, a brief recording from today's zelph showing its operation, and the dedicated page that explains it to every user. The front end itself is then composed of a set of standard-library modules (`stdlib/ace*.zph`, Janet combined with zelph) built upon these capabilities, just as `stdlib/sparql.zph` extends the public Janet API with SPARQL functionality without the C++ core being aware of SPARQL. The few remaining engine items are detailed and defined in [What zelph Still Needs](prerequisites.md); the front end does not wait for any of them.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :show-zelph true)
    true
    zelph> ace John is a man. Every man is a human. Is John a human?

    John ~ man
    (*{(X ~ man)} ~ conjunction) => (X ~ human)
    Answer: Yes. John is a human.
    ```

When the `:show-zelph` option is active, the block prints the native zelph it writes (the output of `ace/zelph-of`, refer to [What ace/zelph-of Prints](semantics.md#what-acezelph-of-prints)): one fact and one rule. The rule appears as zelph itself lists a rule featuring a single condition, a condition set containing a single member, and this line re-enters as the identical rule (decision [DEC-ONE-CONDITION-PRINT](decisions.md#dec-one-condition-print)). Everything after that constitutes zelph's own work. Typed natively into today's zelph, those same two lines derive the answer:

```
zelph> John ~ man
zelph> (*{(X ~ man)} ~ conjunction) => (X ~ human)
(John ~ human) ⇐ {(John ~ man)}
zelph> X ~ human
Answer: John ~ human
```

The block does more than just these two lines: it resolves the words, writes the block once within a scratch cluster to check if the engine refuses anything, writes it for real, runs inference quietly, poses the question in an isolated scratch cluster, and verbalizes the answer. Every one of these actions relies on one of the features detailed below. [The ace Block](block.md) specifies the steps; this page defines the foundation upon which they rest.

## How the Foundation Was Built

The engine work was carried out by the same effort that wrote this specification, implemented directly within zelph, before the inclusion of any ACE-specific code (decision [DEC-WORKMODE](decisions.md#dec-workmode)): each flaw, limitation, and absent functionality required by the specification was fixed or built in dependency order, and only after that was the specification finalized. The principles governing this work stay in force for every subsequent engine modification, the remaining prerequisites included:

- **No ACE knowledge in C++.** No engine test, no C++ string or table and no generic standard-library module refers to an ACE word, construct, message or module. The engine's own vocabulary (`refuted`, `opaque`, `rule pattern`, `closure`, `one-or-more`, `zero-or-more`, `nn`, `nn-layers`) and the Wikidata importer's marker `wikidata:labels-v2` are classified as engine vocabulary, not ACE. A thorough scan of every line added to `src/lib` and `stdlib` reveals no ACE names.
- **A capability lands before the code that uses it.** Before landing, the front end refuses any request requiring it via construct id (`[ace/unsupported]`, see [Constructs and Refusals](constructs.md)): there is no parallel mechanism, no temporary workaround that the capability would discard. All items listed here have already landed, so no front-end refusal awaits any of them; the rule now applies to the items on [What zelph Still Needs](prerequisites.md).
- **No feature detection.** The standard library is embedded from the same tree as the binary, so any binding the front end invokes is present when the module invoking it is compiled; no module checks for the existence of a `zelph/…` function.
- **Tests in the same change.** Each item constitutes its own unit, with doctests positioned adjacent to a named neighbour in the test file (never appended at the end), each accompanied by a comment explaining the rationale for the case. Every new case was initially executed against the unchanged library and seen to fail, and single-switch mutants of the change showed which case is responsible for which part. A pinned test that contradicts the change is deliberately rewritten under a new name, with a comment saying what it previously pinned and why that changed.
- **Documented and pinned.** Every behaviour the front end relies on is described on the user pages, including actual transcripts from the binary, and is safeguarded by a test – this includes cases where the work changed no code but only pinned existing engine behaviour: a fact arriving later does not retract a prior conclusion under "not provably" that had already succeeded, one handler call corresponds to one variable scope, a quoted atom bypasses keyword interpretation, and an empty collection literal evaluates to `nil`.
- **The full suite and the performance guard after every change.** The performance guard (`test_performance_guard.cpp`, "performance guard: the Jacobian workload does the expected amount of work") measures the workload of [Refuting the Jacobian Conjecture](../math/tutorial-jacobian.md): genuine walks exactly 0, cache miss ceilings, full clears, deduction calls, created facts and scans. Its baselines remained untouched. Every counter remained unchanged across all changes except one, which lowered four of them (a typed rule no longer executes an extra time as a query: rules applied 1,294 → 868, extract calls 799,221 → 798,978, failed extractions 495,216 → 494,973, fact-structure cache misses 424,126 → 416,732).
- **Janet is upstream code.** The embedded Janet version is v1.42.1, patched only by a single upstream patch not yet included in any release (a garbage-collection root), applied at configure time and retired once a release includes it.

Following the first two rounds of modifications, each alteration was attacked by an independent test-writing pass whose sole purpose was to break it; the defects it uncovered – those that hang, cause corruption, or yield incorrect results along a path the front end reaches – were fixed in subsequent iterations, while the remaining issues were classified according to the four categories that [What zelph Still Needs](prerequisites.md) explains.

The impact is visible in the measurements. As of this writing, the test suite has grown from 1474 to 2016 test cases (1884 in the fast tier, 132 in the slow tier), and `ctest` has increased from 24 to 25 entries, all of which succeed; these figures include the last two engine rounds: one resolved three design issues detailed below (a `=>` expressed via `zelph/fact`, a rule written as a mention, name merges), the other made zelph record a contradiction of a rule with a single condition as it records one of a one-element condition set. Timed comparisons were taken on a single idle machine, using alternating runs. Following the first round of changes, the Jacobian workload required 1.013 s compared to 1.014 s before the work when run sequentially, and 1.103 s versus 1.108 s in parallel. The second round, incorporating its corrections, was evaluated against the first across ten alternating rounds (trimmed means), yielding these run time ratios (values below 1 indicate faster performance): the Jacobian workload sequential 0.999 and parallel 0.998, the same with a refutation and an opaque declaration in force 0.984, 200,000 facts written from Janet 0.938, and `.prune-nodes` over a million facts 0.957. For certain workloads from the later rounds, the timed comparison remains pending (merges of a hub node, query-heavy scripts, a loaded network with two-object facts, the label index on a full network); the guard's work counters were measured after every round and remain unchanged.

## What the Front End Relies On

The table correlates the requirements of the front end with the engine item responsible for supplying them. The item ids (`P-…`) are the ones used by implementing sessions for navigation; the sections that follow detail each one.

| The front end relies on | Engine item | Section |
|---|---|---|
| a block keyword; a handler's `zelph/fact` claims, thus ground rules fire regardless of sequence | `zelph/register-keyword`, P-claim | [The keyword host](#the-keyword-host) |
| a block dispatched at the end of input triggers a run; a run inside a handler keeps the focus capture | P-eof-autorun, P-quiet-run (b) | [The keyword host](#the-keyword-host) |
| an error raised as a string reads `Error in line "ace <first line>[ …]": <message>` | P-error-envelope | [The keyword host](#the-keyword-host) |
| a handler can import a file | P-import-in-handler, Janet 1.42.1 | [The keyword host](#the-keyword-host) |
| the host does not run again after a handler that ran last; the handler can read `.auto-run` | P-autorun-skip, P-auto-run | [The keyword host](#the-keyword-host) |
| a rule built in Janet is a single `=>` fact, written once, refused when it cannot fire soundly, by every route that writes one | `zelph/dedup-rule`, P-rule-api, P-record-pin | [Rules as data](#rules-as-data-and-the-rule-builders) |
| one silent inference per block that returns every contradiction it met and every concluded statement that was defeated | P-quiet-run (a) | [Quiet runs](#quiet-runs-the-rules-in-force-and-negation-levels) |
| the rules in force and their negation levels represented as data, a rule over predicates marked | P-rule-list, `zelph/strata`, P-strata-transparent | [Quiet runs](#quiet-runs-the-rules-in-force-and-negation-levels) |
| a dry run within a scratch cluster that its drop undoes entirely, at the cost of the cluster | P-scratch-drop, P-drop-names, P-restore-mark, P-refute (e) | [Scratch clusters](#scratch-clusters-and-the-dry-run) |
| `run-delta` is correct after a dropped cluster, a removal, or an interrupted run | P-drop-delta | [Scratch clusters](#scratch-clusters-and-the-dry-run) |
| rules over Wikidata classes and rules under "not provably" cost their delta, not their extent | P-closure-delta, P-naf-delta, P-delta-new-rules, P-pidx-local | [Incremental reasoning](#reasoning-that-costs-the-change-not-the-graph) |
| "If Mary waits then John sleeps. John does not sleep." is consistent; "No man waits." responds to "Does John wait?" with "No." | P-refute | [Refutation](#refutation) |
| names, facts, and degrees read without creating anything, in an explicit language; a ground question that also observes a broader native fact | P-lookup, P-exact, P-parts, `zelph/lookup-all` | [Lookups](#lookups-that-create-nothing) |
| a loaded network file constitutes the session's graph, and Janet caches can tell | P-load-replace, `zelph/epoch` | [Loading a network](#loading-a-network-replaces-the-graph) |
| proof trees, the facts a run derived and provenance that persists after a save, as data; one claimed fact retracted, with what rests on it known | P-explain, P-derived, P-annotate, P-retract | [Proofs and provenance](#proofs-provenance-and-retraction-as-data) |
| a number literal or a string names one node under every `.lang` | P-number-lang | [Numbers](#numbers-and-characters-independent-of-the-language) |
| statements written without asserting them, in facts and in rule heads; a rule written in this manner is not in force | P-opaque, `zelph/mention` | [Opaque predicates](#statements-about-statements-opaque-predicates) |
| a box of several conditions matched as a set constant | P-set-match | [Set patterns](#boxes-as-sets-set-patterns) |
| a conclusion under "not provably" that a later fact defeats is reported | negation records | [Negation records](#negation-records-and-defeated-conclusions) |
| Wikidata items keep their English labels; a word finds every item that carries it | P-wd-labels, P-label-index, the importer fix | [Wikidata labels](#wikidata-labels-and-the-label-index) |
| a Wikidata network that holds what the front end's words should reach | `dev_scripts/wikidata-subset.py`, `dev_scripts/ace-network.json` | [The network tooling](#the-ace-network-tooling) |
| conditions and questions never engage with the engine’s bookkeeping; a question produces no residual trace; a name merge performed by a user never turns two statements that differ in subjects or predicates into a single statement | engine behaviour beside the items | [Made precise along the way](#engine-behaviour-made-precise-along-the-way) |

## The Keyword Host

A user inputs `ace`, followed by a space and English text; the block ends at the first blank line. The C++ core holds no knowledge of this keyword: `stdlib/ace.zph` registers it via `zelph/register-keyword`, the identical system used for the `sparql` keyword, and obtains every block as a string. Before the engine work, a handler was treated as a lesser entity: what it asserted failed to constitute a claim (a ground rule whose condition it asserted did not fire), a block at the file's end was sent without the inference that a blank line would have triggered, an error lost the block's name and sometimes its text, and a handler that imported a file was called twice. Each of these issues has been resolved, and a handler now delivers results precisely as a `%` block does.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John likes Mary.

    zelph> ace Mary likse John.

    Error in line "ace Mary likse John.": Lexicon error [ace/unknown-word]: sentence 1, line 1, column 6: "likse" is not a known word. Did you mean "likes"?
      Mary <>likse John.
      If "likse" is a new word, write v:likse.
    ```

The first block designates `likes` as a word within the graph (a verb guessed by the front end based on its placement), causing the second block's `likse` to be interpreted as a typo of that word and thus writing nothing. The front end raises its message as a plain string; the host names the block via its keyword and its first line, then outputs the message without modification. The wording of each message on this page serves only as an example; the structure and identifier are normative ([Messages](block.md#messages) contains the definitive list). The identical host, recorded with a small handler, written in a single line, that asserts `<name> is happy` for each name it receives and refuses any name ending with a question mark:

```
zelph> (john is happy) => (mary is glad)
zelph> %(zelph/register-keyword "tell" (fn [text] (each name (string/split " " (string/trim (string/replace-all "\n" " " text))) (if (string/has-suffix? "?" name) (error (string "a question is not a name: " name)) (zelph/fact name "is" "happy"))) nil))
zelph> tell bob
john

zelph+> X is glad
Answer: mary is glad
zelph> tell who?
Error in line "tell who?": a question is not a name: who?

zelph: finished with errors, see above.
```

The block of two lines is sent off by the blank line; the handler's `zelph/fact` constitutes a claim, thus triggering the ground rule on `john is happy` (the `+` in the prompt says that the run derived something it did not print, see [The mark in the prompt](../rules.md#the-mark-in-the-prompt)). The last block is triggered by the end of the input, and its error names it by its first line.

**The block protocol** (documented in [Extending the REPL](../janet.md#extending-the-repl-zelphregister-keyword)):

- When a line begins with a token separated by whitespace that matches the keyword, it initiates a block, provided no native statement is currently under processing. The remainder of that line, excluding any leading or trailing whitespace, forms the initial line of content, meaning the handler's column 1 position corresponds to the first character after the keyword; a keyword appearing alone on a line contributes no content line.
- Each subsequent line is added exactly as-is, preserving all leading and trailing whitespace, including `#` comment lines, `%` lines, `.` command lines, and any additional `ace …` line: the host does not remove any `#` comment from block content (trailing comments are stripped only from native statements and inline islands). A line consisting solely of whitespace or being empty triggers the handler with the accumulated text so far, with each line concluded by `\n`.
- The handler may return `:incomplete` (the front end does this when a comment, string, or quoted word remains unclosed); the host then appends `\n` for the empty line and continues reading, and a second consecutive empty line causes dispatch with text terminating in `"\n\n"`. A further `:incomplete` results in the error `Keyword block for 'ace' is incomplete`.
- Upon reaching the end of input, the block is dispatched as if a blank line had been encountered; the handler cannot tell this dispatch from one triggered by a blank line, so a block the handler still vetoes at this point receives the host's error `Keyword block for 'ace' is incomplete`, named by the block's initial line.
- The single line that remains the host's responsibility even within a block: in piped or typed input, a line whose tokens are precisely `.quit` (with optional leading blanks and a trailing `#` comment) terminates the session after dispatching the block accumulated so far, just as at the end of input. In a script supplied via a file argument or within a module, this line constitutes block text.
- Following a handler that returned without veto, the host runs its auto-run if `.auto-run` is enabled, unless the handler has already invoked the engine after its most recent write (P-autorun-skip, described below). After an error, input from pipes and scripts halts; at an interactive prompt, the next line is read as fresh input.
- A registered keyword shadows any statement whose first token is that bare atom: `ace ~ card` begins a block, `"ace" ~ card` is interpreted as a statement, and while the keyword is registered, a node named `ace` is printed in quotes, causing its printed form to re-enter as the identical statement. Without `.import ace`, a line `ace John waits.` is silently treated as a native fact; this behaviour is documented on the front end's user page.
- Main thread only.

What the front end adds to this protocol regarding which regions keep a block open and what traps a user may encounter is specified in [Where a Block Ends](block.md#where-a-block-ends).

**What a handler call is**, as built:

| Behaviour | Item | What it guarantees |
|---|---|---|
| a handler's `zelph/fact` and `zelph/refute` claim, and the C API's `zelph_fact` claims too | P-claim | a ground condition of a rule that the handler asserts becomes data and the rule fires, whether the block was typed or read through `.import` or `zelph/import`; previously, it stayed a pattern and the rule never fired. Since the refutation work, `zelph/fact` and `zelph_fact` first check what the claim would refuse and write nothing if it is refused |
| a run inside the handler keeps the input focus | P-quiet-run (b) | under `.deductions focus` a fact asserted after a run within the same handler call remains an anchor of what is displayed, even when the run raised an exception |
| a block dispatched by the end of input is followed by the host's run | P-eof-autorun | a block at the end of a file behaves as if followed by a blank line |
| errors name the block | P-error-envelope | `Error in line "<keyword> <first line>[ …]": <message>`, with ` …` if a subsequent non-blank line follows; a string or buffer error is printed without a technical prefix; any other error value keeps the prefix `Janet error in handler for keyword 'kw'` and is printed in Janet's one-line form; the name persists across nested imports |
| a handler can import a file | P-import-in-handler | the lines within the imported file execute as though they originated in the file directly, and the import does not call the handler a second time; nested imports from Janet are safe since Janet 1.42.1, as each nested fibre is rooted |
| one handler call constitutes one variable scope | (documented) | a variable symbol `'X` refers to the same variable throughout the call until a `zelph/query` or a `zelph/import` that reads a file occurs; the front end creates its variables using `zelph/var` and never uses symbols |
| `(zelph/auto-run?)` | P-auto-run | the effective value of `.auto-run`: false during module import and after a `.load`; applies only to the main thread |
| no host run after a handler that ran last | P-autorun-skip | the host skips its auto-run after a block when the handler's run count grew and nothing was written since: the run's record of additions is valid and holds no live entry, no predicate was removed since, and the rule count remains unchanged; a cluster created and discarded by the handler after its run does not count as a write. The end of input applies the same test when the block was the final dispatch |

**How the front end uses it** ([What a Block Does](block.md#what-a-block-does) specifies the procedures): each graph write performed by a block corresponds to a `zelph/fact`, `zelph/refute`, or `zelph/mention` invocation within the handler; the front end reads ground atoms of questions via `zelph/exists`, never writes them. The front end raises strings that carry their own sentence, line, and column information, resulting in outputs formatted as `Error in line "ace …": <class words> [ace/<id>]: …`, with tests matching ids using `Contains`. It runs inference itself as the final action following the block's last write, then discards its question clusters, ensuring the host's auto-run is skipped and no zelph-syntax derivation reaches an ACE session. Inference runs when the block asks a question, or when it has written data and `(zelph/auto-run?)` evaluates to true (decision [DEC-RUN](decisions.md#dec-run)): this mirrors the host behaviour, which triggers inference after native input only when `.auto-run` is active. The front end's modules depend on each other via `.import` lines and never call `zelph/import`; arithmetic is never lazily imported from a handler (the user types `.import ace-math`, decision [DEC-MATH-IMPORT](decisions.md#dec-math-import)).

## Rules as Data and the Rule Builders

Most ACE sentences are transformed into rules: "Every man is a human.", "If a man waits then the man sleeps.", "No man waits." The front end writes each one from Janet, and the engine must process a rule constructed in this manner identically to how it treats a typed rule. `zelph/dedup-rule` serves as the route: it runs a thunk that constructs a full rule and returns its `=>` fact, and returns the rule node – the existing one if the graph already holds the identical rule, disregarding variable names, thus ensuring that entering a block twice adds no duplicate rule.

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> %(defn w [name] (zelph/resolve name "ace"))
<function w>
zelph-> %(defn every-man-is-human [] (zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def c (zelph/collection (zelph/fact x "~" (w "man")))) (zelph/fact c "~" "conjunction") (zelph/fact c "=>" (zelph/fact x "~" (w "human"))))))
<function every-man-is-human>
zelph-> %(def rule (every-man-is-human))
<zelph/node (*{(X ~ man)} ~ conjunction) => (X ~ human)>
zelph-> %(= rule (every-man-is-human))
true
zelph-> %(zelph/fact (w "John") "~" (w "man"))
<zelph/node John ~ man>
zelph-> %(zelph/run-delta :quiet)
{:contradictions () :naf-final ()}
zelph-> %(zelph/exists (zelph/lookup "John" "ace") "~" (zelph/lookup "human" "ace") :exact)
true
zelph-> .list-rules
Listing all rules:
------------------------
(*{(X ~ man)} ~ conjunction) => (X ~ human)
------------------------
zelph-> (X waits nil) => (X W nil)
Error in line "(X waits nil) => (X W nil)": The predicate of "X W nil" in the rule's consequence is the variable W, which no condition binds: a firing has no predicate to write, and a new node standing in for one would be a relation type without a name. Bind W in a condition, or write the predicate as a name.
```

The thunk emits the rule in the manner the front end writes each rule with a solitary condition: the condition stated as a one-element condition set, `(zelph/collection cond)` labelled `~ conjunction`. zelph records every contradiction of such a rule exactly once, and since 2026-10-15, it records those of a rule whose singular condition resides outside a set in the same fashion: the condition acts as the sole element, and the record corresponds to the identical set constant. This applies to every contradiction of the rule: that arising from a denial, a rule whose consequence is `!`, that of a positive conclusion that meets a refuted fact, that of a derived refutation that meets a claimed fact, which integrates into the record, and a consequence the engine declines to build; previously, a single condition outside a set was recorded solely when the rule derived the refutation of a claimed fact. Since the engine work, a rule with a one-element set lists and prints as `(*{(X ~ man)} ~ conjunction) => …`, which re-enters as the exact same rule (P-record-pin; decision [DEC-RECORD-SINGLE](decisions.md#dec-record-single)). Typed natively on a graph missing the rule, `((X ~ man)) => …` and `(X ~ man) => …` both build the plain rule, listed as `(X ~ man) => …`, since parentheses function merely to group; on a graph that already has the rule in force, in either form, all spellings, including `zelph/dedup-rule`'s set form, meet it and add nothing. All three spellings record alike, so the records no longer depend on the form; the front end keeps the set form because it gives every rule it constructs a condition set, and `ace/zelph-of` outputs it because it is the form `.list-rules` presents for the rule the front end builds. The second invocation yields the first rule, the quiet run derives `John ~ human`, and the exact probe finds it. The last line shows the refusal all rule constructors enforce when a consequence contains a predicate that no condition binds.

**What a rule build enables.** Each module constructs every rule using a single idiom, which [The Rule Idiom](api.md#the-rule-idiom) prescribes: a single `=>` fact that carries all heads, generated within `zelph/dedup-rule`, every `zelph/var` created inside the thunk, `zelph/conjunction` employed for two or more conditions, and a one-element set utilized when only one is present. This idiom relies upon the following characteristics of the engine:

- A single `=>` fact, with all heads represented as objects, fires once per binding, meaning a fresh variable named by two heads serves as one witness for both ([Several Consequences](../logic.md#several-consequences)).
- Every `zelph/var` invocation generates a new variable regardless of its name, hence a rule must issue one such call for each distinct variable name; a variable created outside the thunk would leave a residual node behind when `zelph/dedup-rule` finds the rule already present. Variables adopt the native format – either a single uppercase letter or a leading `_`; the front end assigns rule variables names `X`, `Y`, `Z`, `A`, … in the order of their initial appearance (decision [DEC-VAR-LETTERS](decisions.md#dec-var-letters)). `zelph/conjunction` refuses a solitary condition.
- `zelph/dedup-rule` refuses, for each invoker, any thunk that returns anything other than the `=>` node – a basic fact or a condition set – and retains no structure the thunk constructed: `zelph/dedup-rule: the thunk returned <zelph/node a p b>, which is not a rule: it builds the whole rule and returns its `=>` fact`. The `:focus` option is managed by the parser; the front end never forwards it.
- Within the thunk, permitted operations include node-creation and node-access: `zelph/var`, `zelph/fact`, `zelph/conjunction`, `zelph/collection`, `zelph/negate`, `zelph/path`, `zelph/list`, `zelph/list-chars`, `zelph/set`, `zelph/resolve`, `zelph/mention` (which only constructs within that context), and reads like `zelph/exists`. The host refuses `zelph/cluster` (even without parameters), `zelph/cluster-drop`, `zelph/query`, `zelph/import`, `zelph/load`, `zelph/save`, run-related functions, `zelph/prune-fact`, `zelph/holders`, `zelph/dependents`, and any nested rule construction. Thus, the front end resolves its words, shifts between clusters, and poses its questions before entering the thunk.
- The inner rule within a rule generator ("If John waits then every man sleeps.") is expressed as a `=>` fact embedded inside the outer thunk; a nested `zelph/dedup-rule` is refused ([Rules That Derive Rules](../logic.md#rules-that-derive-rules)).
- A fact that names a rule node as either subject or object disables the rule ([Mentioning a Rule Is Not Asserting It](../logic.md#mentioning-a-rule-is-not-asserting-it)). This explains why provenance is stored in node annotations (see below).
- `zelph/rule` also produces a single `=>` fact, yet it looks for no identical rule and delivers the condition set instead of the `=>` node, so the front end never uses it.

**What the engine work changed for each user** (P-rule-api and decisions [DEC-UNBOUND-PRED](decisions.md#dec-unbound-pred) and [DEC-FACT-RULE](decisions.md#dec-fact-rule)):

- `zelph/rule`, `zelph/rule*`, and the C API's `zelph_rule` with several consequences generate a single `=>` fact, similar to how a typed rule operates, thereby sharing a fresh variable across all consequences; previously, each consequence produced a separate rule. Now, a `zelph_rule` invocation with a null consequence writes nothing whatsoever.
- A consequence whose predicate is a variable not bound by any condition is refused at the time the rule is written, by every builder: the typed rule, `zelph/dedup-rule`, `zelph/rule`, `zelph/rule*`, `zelph/build-rule`, `zelph_rule`, and Rust's `Engine::rule`. Previously, `(X waits nil) => (X W nil)` yielded no result silently, while `(X waits nil) => (W W nil)` created a witness representing an unnamed relation type. "Binds" means that a positive condition names the variable, including nested facts and set members it holds; a negated condition binds nothing. Exception: a `=>` within a consequence (a generator's internal rule, where the generator might bind the predicate) and a rule stored as an argument, which no builder runs. For the front end, this means that a rule over relations binds its predicate within a condition. See [Fresh Variables](../logic.md#fresh-variables-generative-rules).
- A `=>` fact written by Janet's `zelph/fact` or the C API's `zelph_fact` cannot be judged at the moment of writing, since a subsequent call might later designate it as a generator's inner rule or hold it in a statement. Validation occurs at the end of the outermost Janet evaluation – be it a `%` block, a keyword handler, or a script – as well as immediately before each save (`.save`, `zelph/save`, `zelph_save`, `.save-predicates`) and prior to any run: a `=>` whose consequence contains a predicate not bound by any condition, and which persists as a rule in force at that point, is refused and discarded, accompanied by a message that specifies the variable. By then, generator's inner rules and held rules are mentioned and pass. The front end builds every rule via `zelph/dedup-rule` and meets the builder's check instead.
- A typed rule statement claims its rule and never runs as a query (the four lower counters above).

## Quiet Runs, the Rules in Force and Negation Levels

A translator must report what inference found, expressed in its own language. Before the engine work, a run from Janet printed its banner, its derivations, a `!` line for every contradiction, and the results from check mode in zelph syntax, then returned nothing. `(zelph/run :quiet)` produces no output whatsoever and returns the findings as data; the front end turns each result into an ACE report.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John waits.

    zelph> ace No man waits. John is a man.

    Contradiction [ace/contradiction-derived]: these sentences do not hold together:
      "No man waits." (block 2, sentence 1)
      "John waits." (block 1, sentence 1)
      "John is a man." (block 2, sentence 2)
    zelph> ace Does John wait?

    Answer: Yes. John waits.
    ```

"No man waits." is the rule `(*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)`; during the second block's internal inference, the firing for John meets a fact that the graph claims. The engine writes nothing, records the contradiction, and returns it from the quiet run, with the claimed fact as its conclusion; the front end quotes the three sentences from their provenance ([The Report of a Derived Contradiction](block.md#the-report-of-a-derived-contradiction)). The block remains intact, and "John waits" continues to respond (decision [DEC-REFUTED](decisions.md#dec-refuted)). Reversing the sequence presents a distinct scenario: a block that asserts a fact previously refuted by an earlier inference is refused by the dry run as `[ace/contradiction]` ([below](#scratch-clusters-and-the-dry-run)). The engine side, recorded with a denial typed natively:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> ((X ~ man), (X ~ woman)) => !
zelph-> pat ~ man
zelph-> pat ~ woman
zelph-> %(defn report [r] (each e (r :contradictions) (zelph/out (string/format "%q" [(e :conditions) (e :conclusion) (e :record)]))))
<function report>
zelph-> %(report (zelph/run-delta :quiet))
((<zelph/node pat ~ woman> <zelph/node pat ~ man>) <zelph/node !> :new)
zelph-> %(report (zelph/run :quiet))
((<zelph/node pat ~ woman> <zelph/node pat ~ man>) <zelph/node !> :existing)
```

During the first quiet run, the contradiction's record (`:new`) is written; the subsequent run encounters it once more and says that a record is already in place (`:existing`). Neither run outputs a line.

**`:quiet`, as built** (P-quiet-run (a); decision [DEC-QUIET](decisions.md#dec-quiet); documented in [Running quietly](../janet.md#running-quietly)):

- `zelph/run`, `zelph/run-once`, and `zelph/run-delta` take the keywords `:quiet` and `:derived`, each appearing once and in any sequence; anything else raises `<name>: the arguments are :quiet and :derived`, while `:derived` used alone raises `<name>: :derived needs :quiet`. The commands `.run quiet`, `.run-once quiet`, and `.run-delta quiet` (with `derived`) share the same underlying implementation; a command now refuses an unrecognized argument rather than silently disregarding it.
- A quiet run produces no output whatsoever: no banner, no `Ready.`, no derivation line, no comment about hidden deductions, no `+` in the prompt, no contradiction line, no `Found one or more contradictions!`, no list of check mode's negation check results, and no alert regarding negation records. It only raises exceptions for errors; check mode's negation check is returned rather than raised.
- It returns `{:contradictions C :naf-final N}`, with `:derived D` added under `:derived` (refer to [Proofs and provenance](#proofs-provenance-and-retraction-as-data)).
- **C** holds a struct for every distinct contradiction detected during the run, one for each (rule, bindings) pair – incorporating fresh witnesses – ordered by initial occurrence: `:rule` refers to the `=>` node; `:conditions` lists the facts that matched the positive conditions, sorted by node identifier – retrieved, never constructed (a broader fact that matched the condition, a set constant matching a set pattern through its members, the marking of a refuted fact), empty when all conditions are negations, guards, or paths; `:conclusion` is the core node `!` of a denial, the refuted fact that a consequence would have asserted, or, in the case of a derived refutation that met a claimed fact, that fact itself (excluded from `:conditions`, though the record preserves it); `:record` is `:new` (this run wrote the record), `:existing` (a record already existed), or `:none` (it cannot be recorded: recording is disabled, a rule with only negations, guards, or paths as conditions unless a derived refutation met a claimed fact, an instance not located through lookup); `:bindings` is a table from the rule’s variables to their assigned values. A consequence the engine refused to build – for instance, one that would extend a set constant – carries `:refused <reason>` instead of `:conclusion`; the run continues.
- **N** holds one `{:fact F :rule R}` for each conclusion the run identified as derived under a negated condition whose pattern has become true (see [Negation records](#negation-records-and-defeated-conclusions)); it is filled only when negation records are kept.
- A full pass meets every recorded contradiction in the graph again, as `:existing`; the front end keeps its own set of contradictions it has already reported and reports each only once. The note about a condition the engine will not evaluate (a negated `!=`, a path with both endpoints unbound) is deferred to the next run that prints output.

Why `:quiet` differs from the native `?` prefix: the `?` prefix delivers results to a person at the command line, who sees the findings; `:quiet` delivers results to a program, which receives them as input data, and outputting them additionally would put zelph syntax within the program's own output.

What the `:quiet` setting does not resolve, and the front end’s user page documents: when `.auto-run` is enabled, the host’s own run following a later native line prints each contradiction without a record (records disabled, rules featuring all conditions as negations, guards, or paths unless a derived refutation meets a claimed fact, unnamable instances), and when negation records are active, such a host run prints and consumes the defeats before the front end’s next quiet run sees them.

**How the front end utilizes it**: `ace/infer!` functions as `(zelph/run-delta :quiet)`, the sole inference call (`:quiet :derived` when derivation echo is active). Each entry in C whose key is fresh within the session transforms into `[ace/contradiction-derived]`, regardless of its `:record`; N converts into one `[ace/w-naf-final]` for each block. [Contradictions and Defeated Conclusions](block.md#contradictions-and-defeated-conclusions) specifies the reporting conventions.

**The rules in force and their respective levels of negation.** Two read-only bindings give the rules as data:

- `(zelph/rules)` (P-rule-list) yields the rules in force – the set `.list-rules` shows, rules a statement merely mentions excluded – as a tuple sorted by rule node of `{:rule :conditions :negated :guards :paths :neural :heads}`: the plain positive conditions, the patterns P of the negated conditions `¬(P)`, the `!=` guards, the path conditions and the `≈` conditions (both as the fact their operator builds, which prints as typed, `X P279⁺ Y`), and the consequences, the core node `!` for a denial. Each component is a tuple of nodes sorted by node id. It creates nothing; main thread exclusively. The front end's sole invoker is the guard of a conditional within a question ("If a man waits does the man sleep?"): before the question's hypothetical execution, it verifies whether a rule featuring a negated condition or a `!=` guard is reachable from the hypothesis and refuses the block via the id `:rule/unsafe-hypothesis` (decision [DEC-QUNIV](decisions.md#dec-quniv); [Questions That Hold a Conditional](answers.md#questions-that-hold-a-conditional)).
- `(zelph/strata)` delivers, for each rule in force, `{:rule :level :component :stratifiable}` – the negation level (applicable only to a rule with a negated condition), the strongly connected component, and whether the component can be ordered into levels (`:stratifiable false` for every rule within a component that negates what it derives). The levels are keyed by pattern, not by predicate: a top-level pattern with a constant object by its predicate and that object; one with a variable or structured object, a nested statement and a path condition by its predicate with any object. A rule over predicates – a transitive, symmetric or reflexive meta-rule such as `(R is transitive, A R B, B R C) => (A R C)` – stands in no component and makes every key compare predicates alone while it is in force; P-strata-transparent marks its row with `:transparent true`, and the other rows lack the key. `(zelph/strata)` sees rules written inside a cluster until the cluster is dropped. See [Stratified Evaluation](../logic.md#stratified-evaluation) and [Negation levels](../janet.md#negation-levels).

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> (X ~ man, ¬(X ~ married)) => (X ~ bachelor)
zelph-> %(each r (zelph/rules) (zelph/out (string/format "%q" [(r :conditions) (r :negated) (r :heads)])))
((<zelph/node X ~ man>) (<zelph/node X ~ married>) (<zelph/node X ~ bachelor>))
zelph-> %(defn levels [] (each line (sort (map |(string ($ :rule) " -- stratifiable " ($ :stratifiable) (if ($ :transparent) ", transparent" "")) (zelph/strata))) (zelph/out line)))
<function levels>
zelph-> %(levels)
(¬(X ~ married), (X ~ man)) => (X ~ bachelor) -- stratifiable true
zelph-> (R is transitive, A R B, B R C) => (A R C)
zelph-> %(levels)
((R is transitive), (B R C), (A R B)) => (A R C) -- stratifiable true, transparent
(¬(X ~ married), (X ~ man)) => (X ~ bachelor) -- stratifiable false
```

The rule under "not provably" is stratifiable independently. After the meta-rule comes into effect, comparisons occur solely between predicates, and the same rule ceases to be stratifiable since it both negates and writes `~`. The front end requires precisely this verdict: its internal assessment determines `:naf/unstratified` for the ACE rules it authored, and during the dry run, if a block writes a rule while a rule under "not provably" is in force, it also takes the engine's judgment from `(zelph/strata)`: a non-stratifiable component that holds one of its NAF rules is refused when the component also holds a rule the front end did not write, or when any row carries `:transparent true` (decision [DEC-STRATA](decisions.md#dec-strata); [Step 4: The Dry Run](block.md#step-4-the-dry-run)).

## Scratch Clusters and the Dry Run

The front end never implements the engine's refusal logic a second time. It writes each block twice: initially within a scratch cluster `ace:dry`, where any refusal is captured, then drops the cluster and, if no refusal occurred, writes the block once more into the user's active cluster (decision [DEC-ATOMIC](decisions.md#dec-atomic)). Consequently, a block is either accepted in its entirety or refused entirely, and a refused block results in the graph remaining unchanged. This approach remains sound solely due to the restoration provided by a dropped cluster, which is precisely what the engine work ensured was fully realized.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John waits.

    zelph> ace Bill waits. John does not wait.

    Error in line "ace Bill waits. John does not wait.": Contradiction [ace/contradiction]: sentence 2, line 1, column 13: zelph already holds the opposite of this sentence, from "John waits." (block 1, sentence 1).
      <>John does not wait.
    ```

The engine refuses to refute a fact the graph claims, and the dry run transforms this refusal into the block's error. The first sentence of the second block was written during the dry run and accompanied its drop, thus leaving no information about Bill afterwards: a later "Does Bill wait?" yields the response "It is not provable that Bill waits." The protocol, captured using a Janet function that performs the same operation as the front end's dry run:

```
zelph> (a p b) => (c q d)
zelph> x r y
zelph> %(defn dry-run [writes]
   (def prev (zelph/cluster))
   (zelph/cluster "ace:dry")
   (def result (protect (writes)))
   (zelph/cluster prev)
   (zelph/cluster-drop "ace:dry")
   result)
<function dry-run>
zelph> %(dry-run (fn [] (zelph/fact "a" "p" "b") (zelph/fact "k" "l" "m") (zelph/refute "x" "r" "y")))
(false "fact(): this fact is known to be true")
zelph> %(zelph/exists "a" "p" "b")
false
zelph> %(zelph/mentioned "a" "p" "b")
true
zelph> %(nil? (zelph/lookup "k"))
true
zelph> %(zelph/exists "x" "r" "y")
true
zelph> %(zelph/clusters)
@[]
zelph> .fact-stores
Fact-path stores: on
```

The dry run claimed `a p b`, which the ground rule had specified solely as its pattern, generated `k l m`, and was subsequently refused when attempting to refute the claimed fact `x r y`. Following the drop, `a p b` reverts to being a pattern (existing, yet unclaimed), `k` has disappeared, `x r y` remains valid, no cluster persists, and the fact-path stores continue to be active.

**What a drop restores** (as built; [Scoped work: clusters](../janet.md#scoped-work-clusters) and [Node Clusters](../rules.md#node-clusters-transactional-workspaces) document it for users):

| Guarantee | Item | Before the engine work |
|---|---|---|
| a drop eliminates all nodes generated during the cluster's active period and all external facts or rules referencing any of them; a cluster that solely added a member to a prior collection leaves that collection unchanged | P-scratch-drop | the general path destroyed such a collection and all facts that held it |
| a drop that has no external references to it maintains the fact-path stores in an armed state and incurs a cost equal to the cluster's size | P-scratch-drop | every drop disarmed the stores for the rest of the session: the first block of each session lacking a loaded network would have triggered the slow state the performance guard is designed to detect (genuine walks 0 → 634,623 on the Jacobian workload) |
| the names of the removed nodes are collected in a single batch, with one lookup per language and node; a language with no remaining names is erased | P-drop-names | removing a node scanned the name maps of each language (11.4 million entries on the published `-small` network) |
| a rule's pattern previously claimed by the cluster is reset to a pattern state, and this reset is recorded in no cluster | P-restore-mark | the restored mark was recorded in whatever cluster was active at the time of the drop, and a subsequent drop of that cluster or a save-and-load caused the pattern to become a claim |
| the effect of a refutation within the cluster on a fact not created by the cluster – its refuted-index entry and its probability-0 edge – is undone, on both drop pathways, including for a refutation derived during a run there | P-refute (e) | neither was undone; session and reload diverged |
| the defeats of conclusions under "not provably" reported by a run within the cluster are restored to the records, ensuring the following run reports them where they remain valid | (negation records) | the dry run's quiet run consumed them |
| the engine's internal vocabulary (`refuted`, `opaque`, `closure`, …) established initially within the cluster persists, along with what references it from outside; a declaration `P ~ opaque` authored there goes with the cluster | decision [DEC-VOCAB](decisions.md#dec-vocab) | a drop could carry `refuted` along |
| a drop of a cluster where a run derived facts (a per-cluster flag indicates this) invalidates the record of additions, so the next `run-delta` performs a full pass with the note `Incremental run not applicable (a removal took facts a run may have derived) - running a full pass.`; a write-only drop, and a rule constructed within a dropped cluster, preserve the run's incremental nature | P-drop-delta | `run-delta` silently missed what the dropped run had derived; a rule created within a dropped cluster forced a full pass |

Two modifications remain beyond the journal's scope and are not reverted: neural weights configured via `zelph/set-weight` or the `nn-*` functions, and an annotation inserted within the cluster onto a node that was present prior. The front end alters neither in a scratch cluster.

P-drop-delta covers every additional removal as well: `.prune-facts`, `.prune-nodes`, `.remove`, `.cleanup`, and `zelph/prune-fact` are unable to distinguish between derived and asserted facts, and invalidate the record each time a fact is eliminated; a run halted due to an error results in an invalid record; and eliminating the final statement that mentions a rule is equivalent to generating that rule (decision [DEC-DROP-DELTA](decisions.md#dec-drop-delta); [Reasoning incrementally](../janet.md#reasoning-incrementally)).

**How the front end uses it.** Each cluster switch saves the active cluster and reestablishes it, always avoiding the literal `(zelph/cluster nil)`, which would stop the recording of the user's cluster. The four scratch clusters `ace:dry`, `ace:q`, `ace:probe`, and `ace:math` are each discarded within the handler call that initiated them; identifiers starting with `__` are assigned to the engine and are refused, `stdlib/sparql.zph` queries within a cluster `sparql:query` in the same way, and every `zelph/query` from the front end runs inside a scratch cluster, since a program's `zelph/query` keeps the pattern it receives ([A question that leaves nothing behind](../janet.md#a-question-that-leaves-nothing-behind)). A load that replaces the graph forgets all clusters, including `ace:dry`; the handler loads nothing, so this occurs solely between blocks. [Scratch Clusters](block.md#scratch-clusters) outlines the full discipline, and [How a Failed Dry Run Is Classified](block.md#how-a-failed-dry-run-is-classified) details the engine's refusal mapping – `known to be wrong`, `known to be true`, the refused opacity declaration – to the block's messages.

## Reasoning That Costs the Change, Not the Graph

On a loaded Wikidata network, a rule over a class with millions of instances must not be fully matched in every block, and a question must not trigger index rebuilding across a million edges. The front end executes `(zelph/run-delta :quiet)` following each block that performs writes (when auto-run is active) and preceding every question, making the cost of a seeded run the determining factor in whether ACE on Wikidata remains viable. `run-delta` seeds semi-naive evaluation using facts created since the last run and automatically reverts to a full pass – accompanied by a note – whenever the graph is not known to be a fixpoint: no previous run, a bulk load, a removed rule, a new rule when a rule with a negated condition is in force, a removal that took facts a prior run may have derived, or `.semi-naive` being off ([Reasoning incrementally](../janet.md#reasoning-incrementally)). Four items made the seeded run bear the cost of the change only:

| Item | As built | Before (measured on reproducers) |
|---|---|---|
| P-closure-delta | a rule that features a path condition (`D P279∗ C`) gets seeded via its other conditions and takes a classic pass exclusively during an iteration where newly introduced facts include one of the predicate traversed by the path; a path with a variable predicate maintains the rule as unseeded | each such rule was fully matched in every execution: 5,000 instances of a class, one unrelated fact, followed by `.run-delta`: 5,000 matches; currently 0 |
| P-naf-delta | a rule situated under "not provably" is seeded by its positive conditions and tests its negation against every bound instance; it keeps its classic pass if a predicate it negates lost a fact since the last run, if it negates under a variable predicate, if its component negates what it derives, or if it is newly introduced | a classic pass at every negation level of every seeded run: 2,000 matches per `.run-delta` following an unrelated write; currently 0 |
| P-delta-new-rules | a newly added rule undergoes one classic pass while all other rules remain seeded, provided no rule currently in force has a negated condition; a statement that only mentions a rule preserves the run as incremental | any new rule, or any mention of one, caused the subsequent run to perform a full pass across all rules (5,001 matches after one new rule; now 1) |
| P-pidx-local | a write operation invalidates solely the index associated with its own predicate; a question, a removed cluster, and the deletion of a fact from another predicate preserve the built index and its `.pidx` sidecar; the first fact of a new predicate clears only `~`'s index and its own | every new fact and each removal triggered the invalidation of every predicate index and deactivated the sidecars for the session: on the published `-small` network, each class question after an unrelated write reconstructed the `P279` index across 1,114,758 relation nodes |

One condition continues to be less precise than it might be: as soon as a rule featuring a negated condition is in force, each subsequent block that adds a new rule makes the next `run-delta` execute as a full pass. The arithmetic stack holds such a rule (serving as the simplifier's fallback for identities), meaning this behaviour begins at the very first "not provably" sentence and persists whenever `ace-math` is imported. On a Wikidata network, this results in delays measured in seconds to minutes per such block rather than milliseconds (an estimate); it is specified as an engine item, [Z3-P1](prerequisites.md#z3-p1-after-the-first-negated-condition-every-new-rule-costs-a-full-pass).

**No element along the front end's path scales linearly with the graph's size.** All operations permitted by this invariant are currently available: hash lookups (`zelph/lookup`, `zelph/exists … :exact`, `zelph/degree`, `zelph/name … :strict`, `zelph/parse`), the carriers of a single label (`zelph/lookup-all`), writing facts, executing queries within a scratch cluster, removing a cluster that the front end populated, the rules in force once per block when necessary (`zelph/rules`, `zelph/strata`), accessing nodes with bounded degree via adjacency, and traversing upward through subclasses starting from a rule head's class, performed using `zelph/closure` over the subclass predicate, which relies on the predicate index. The front end calls `zelph/exists` exclusively with `:exact`, except for one named exception: the second probe during a yes/no question in `ace/ask`, executed solely after the initial exact probe missed, whose cost is bounded by the number of facts associated with that predicate for the subject ([below](#lookups-that-create-nothing)). The complete inventory, detailing what made each operation local, appears in [Nothing Is Linear in the Size of the Graph](architecture.md#nothing-is-linear-in-the-size-of-the-graph).

## Refutation

ACE incorporates classical negation – such as "John does not wait." or "No man waits." – in addition to negation as failure, exemplified by "John does not provably wait." zelph encodes classical negation through refutation: `¬(F)` claims that F does not hold. Before the engine enhancements, "refuted" had two representations that disagreed, a refutation of a statement that a ground rule merely mentioned was refused, a derived refutation silently withdrew a fact that the user had asserted, and a rule attempting to conclude something already refuted aborted the entire run. Now, refutation maintains a single consistent interpretation on every route.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace If Mary waits then John sleeps. John does not sleep.

    zelph> ace No man waits. Bob is a man. Does Bob wait?

    Answer: No. It is false that Bob waits.
    ```

The first block was refused regardless of sequence before the work, as the rule establishes the pattern `john sleeps nil` and the refutation took that pattern as a claim. The engine side:

```
zelph> (mary waits nil) => (john sleeps nil)
zelph> ¬(john sleeps nil)
zelph> (*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)
zelph> bob ~ man
((bob waits nil) ~ refuted) ⇐ {(bob ~ man)}
zelph> X ~ refuted
Answer: (bob waits nil) ~ refuted
Answer: (john sleeps nil) ~ refuted
zelph> %(zelph/exists "bob" "waits" "nil")
false
zelph> pat waits nil
zelph> %(protect (zelph/refute "pat" "waits" "nil"))
(false "fact(): this fact is known to be true")
zelph> pat ~ man
! ⇐ {(pat ~ man)}
Found one or more contradictions!
zelph> %(zelph/exists "pat" "waits" "nil")
true
```

The refutation of the rule's pattern is accepted; "No man waits." derives the refutation for each man; `zelph/exists` reports the refuted fact as absent while the marking answers; a fact that the graph claims cannot be refuted; and a derived refutation that meets a claimed fact constitutes a contradiction, recorded alongside that fact among its members, while the fact continues to respond.

**The meaning, as built** (P-refute, decisions DEC-REFUTED and [DEC-REFUTE-ROUTES](decisions.md#dec-refute-routes); documented under [Negation as Failure](../logic.md#negation-as-failure)):

- A refutation of a fact F is the claimed marking `F ~ refuted`. The entry for F in the refuted index and the probability-0 edge for F exist precisely when a claimed marking of F is present.
- **A claimed fact is never refuted.** Every route refuses it via `fact(): this fact is known to be true`, and any derived refutation of it constitutes a contradiction whose record holds the rule's conditions along with F; no data is written, and F continues to respond. A test requiring a refuted fact therefore relies on one that a rule only writes down, or on a new one.
- A statement that a rule or a query only writes down can be refuted, and a rule may refer to a fact that was previously refuted: a condition on it never matches, and a consequence on it results in a contradiction when the rule fires.
- Only a claimed marking performs refutation. A marking named by a rule's condition is a condition pattern, so "If John does not wait then Mary sleeps." translates into a rule over `((John waits nil) ~ refuted)` that fires once the refutation is claimed. This remains true after a save and reload.
- The same applies across every route: `¬(F)`, the typed line `(F) ~ refuted` (read as the spelling of `¬(F)`, so the printed marking re-enters), a marking embedded within a claimed statement or a comma list, Janet's `zelph/refute`, the C API's `zelph_refute`, Rust's `Engine::refute`, and a firing, whichever binding supplies `~` or `refuted`. A ground marking written with Janet's `zelph/fact` or C's `zelph_fact` is refused and names `zelph/refute` or `zelph_refute`; a marking whose fact holds a variable is a pattern, which is how a program requests refutations.
- A refutation claims its marking and nothing about its fact: a fact it generates is written as a mention. A statement whose refutation is refused leaves the graph unchanged. A refutation of a refutation marking is refused; removing a refutation (pruning its marking, `.remove`) ends it as a reload would read it.
- A rule conclusion that contains a refuted fact does not halt the run nor reassert the fact: a marking for an already refuted fact causes no change, and a statement attempting to claim it is a contradiction. The focus syntax `*(…) ~ refuted` is refused in rules and queries, where it would otherwise read or derive the fact in a positive sense. Within an argument of an opaque predicate, a marking acts as a mention and performs no refutation.
- A dropped cluster takes back what its refutations accomplished (as described above); a refuted fact travels with a predicate slice.

**How the front end uses it**: The statement "John does not wait." is represented as `zelph/refute`; the rule "No man waits." has a head expressed as the marking `((X waits nil) ~ refuted)`; the "No." in a yes/no response originates from the query `((X waits nil) ~ refuted)` executed within a scratch cluster, where only the subject is a variable (a ground marking would constitute a claim) and the rows are narrowed down by node identity in Janet; a subsequent assertion "John waits." is refused during the dry run (`known to be wrong`) and reported as `[ace/contradiction]`. These correspond to the mapping rows of milestone M4 in [Every Construct and Its Native Form](semantics.md#every-construct-and-its-native-form).

## Lookups That Create Nothing

Word resolution, the yes/no questions, and the validations before a block are all read operations: determining if a word already possesses a node, identifying which Wikidata items include a label, verifying whether a fact is true, counting the number of facts associated with a class node. Before the engine work, Janet's `zelph/resolve` created every name it encountered, `zelph/exists` walked the whole adjacency of its subject upon failure (158,576 relations for Paris across the full network), `zelph/name` defaulted to a different language without explicit indication, no binding read or write occurred for `.lang`, and no mechanism informed a Janet cache that the graph had been replaced.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> .lang ace
    ace> John sees Mary Bill
    ace> ace Does John see Mary? Does John see Sue?

    Answer: Yes. John sees Mary.
    Answer: It is not provable that John sees Sue.
    ```

The native line, typed under `.lang ace` so that it names the nodes the front end accesses, expresses a single fact involving two objects. The initial question locates it, just as a rule condition would; the subsequent question names Sue, who remains unknown to the graph, and creates nothing for her. The engine side:

```
zelph> a p b c
zelph> (X ~ dog) => (X ~ pet)
zelph> %(zelph/lookup "a")
<zelph/node a>
zelph> %(nil? (zelph/lookup "cat"))
true
zelph> %(nil? (zelph/lookup "X"))
true
zelph> %(nil? (zelph/name (zelph/lookup "a") "de" :strict))
true
zelph> %(zelph/exists "a" "p" "b")
true
zelph> %(zelph/exists "a" "p" "b" :exact)
false
zelph> %(zelph/exists "a" "p" "b" "c" :exact)
true
zelph> %(zelph/degree "a")
2
zelph> %(zelph/parse (zelph/fact "a" "p" "b" "c"))
(<zelph/node a> <zelph/node p> (<zelph/node b> <zelph/node c>))
zelph> %(zelph/lang)
"zelph"
```

`a` is found; `cat` is absent, and so is the rule's variable `X`, even though a variable bearing that name is shown; `a` lacks a German name and the strict read does not fall back. The plain probe locates `a p b` via the wider fact `a p b c`, just as a rule condition would, whereas the exact probe fails to do so; `a` possesses two edges; `zelph/parse` takes the wider fact apart. None of this creates a node.

**The bindings, as built** (P-lookup, P-exact, P-parts; decision [DEC-BINDINGS](decisions.md#dec-bindings); documented in [Querying (read-only)](../janet.md#querying-read-only), [Looking Names Up](../janet.md#looking-names-up-zelphlookup-and-zelphlang) and [Fact Decomposition](../janet.md#fact-decomposition-zelphparse)):

| Binding | Returns | Notes |
|---|---|---|
| `(zelph/lookup name &opt lang)` | the node associated with `name` in `lang`, or nil | never generates; nil when the name is missing, the name is empty, or the name is a variable's display name; a core spelling (`~`) gives the core node across all languages; any thread |
| `(zelph/lookup-all name &opt lang)` | the tuple of all nodes bearing `name` in `lang`, ordered by rank, or `:incomplete` | see [Wikidata labels](#wikidata-labels-and-the-label-index) |
| `(zelph/name node &opt lang :strict)` | the name of the node in `lang`, or nil | with `:strict`, no fallback to another language; any additional keyword is refused; a core node answers with its spelling |
| `(zelph/lang &opt lang)` | the current language; with an argument, sets it as `.lang` does and returns the prior value | the empty language is refused; main thread only, including the read |
| `(zelph/epoch)` | an integer | see [Loading a network](#loading-a-network-replaces-the-graph) |
| `(zelph/exists s p o & objs :exact)` | whether the claimed fact exists precisely | only the hash probe, O(1) even on a miss or at a hub; needs at least one object; any other trailing keyword is refused (previously returned false) |
| `(zelph/exists s p o & objs)` | whether a ground rule condition of that shape matches | since the engine work, the plain form asks the condition matcher, thus returning what a rule condition would answer at every level (a wider fact `a p b c` implies `a p b`, including nested); rule patterns, refuted facts, and the engine's bookkeeping are never found; a question containing a variable answers false. The matcher is invoked solely when a fact involving multiple objects could potentially be present (for example, following the loading of a network file that might contain such a fact); in all other scenarios, the exact result is definitive, and a miss incurs the cost of a hash probe. On such a graph, a miss costs a match anchored to the subject if it is an atom (otherwise to an object), which reads the anchor's facts with that predicate and never scans the graph, so the front end passes `:exact` everywhere except in the second probe of a yes/no question (below) |
| `(zelph/degree node &opt side)` | the number of edges, O(1) | `:in`, `:out`, or both; 0 for a non-existent node; an unrecognized side is refused; any thread |
| `(zelph/parse node)` | `(subject predicate objects)`, with objects sorted by node id | nil for an atom, a variable, a string, a missing node, or a non-fact; a negated condition `¬(P)` becomes the fact `((P) ~ negation)` and a `=>` node parses into its condition set, `=>`, and its heads; any thread |

For each read-only binding, two principles apply: a string never refers to a rule's variable, meaning `(zelph/exists "X" "~" "dog")` evaluates to false despite a rule's variable being shown as `X` (a quoted atom bearing a variable-like name is still found); and a binding called on a different thread reads the language most recently published by the main thread, once per invocation, ensuring that a single call resolves all its strings within a single language.

**How the front end uses them**: word resolution searches for a word within the explicit language `ace`, subsequently in `en` via `zelph/lookup-all`, and retains a candidate solely following exact probes against the Wikidata profile's page and given-name classes, along with a `zelph/degree` comparison ([Words and Wikidata](words.md)). Each node the front end names is named in an explicit language, never in the user's `.lang`: content in `ace`, Wikidata ids in `wikidata`, internal vocabulary, characters, and the maths stack in `zelph`; the handler runs under `.lang zelph`, established and reverted using `zelph/lang`, ensuring that what zelph implicitly names (rule variables, characters) lands in `zelph` (decision [DEC-CONTENT-NS](decisions.md#dec-content-ns)). Every ground atom in a question undergoes a bottom-up check with `zelph/exists … :exact` before any pattern is constructed. If the ground atom a yes/no question queries fails to match, `ace/ask` invokes the plain `zelph/exists` once again, enabling a native fact with several objects, `John sees Mary Bill`, to respond "Does John see Mary?" with yes, as a rule condition would read it (decision [DEC-YESNO-WIDER](decisions.md#dec-yesno-wider); [Facts With Several Objects](answers.md#facts-with-several-objects)). This secondary probe constitutes the sole deviation from the front end's principle that `zelph/exists` is invoked exclusively with `:exact` ([Invariants](api.md#invariants)), and the source-rule test permits precisely this invocation. Absent facts involving multiple objects, it functions as a hash lookup; otherwise, its expense is limited by the number of facts associated with the subject and the given predicate, never requiring a full scan, and a budget row times it on the `-small` network at M3 ([Budgets](tests.md#budgets-counts-always-timings-only-on-an-idle-machine)). From M3, the test `ace questions: a native fact with several objects answers a single-member yes/no question` pins this reading. An atom that neither probe locates yields "No." if its refutation is found ([above](#refutation)) and "It is not provable that …" otherwise. The profile pin (`ace:profile ace:uses ace:wikidata` or `ace:core`, determining whether "is a" maps to `P31`/`P279` or core `~`) is retrieved via `zelph/lookup` and subjected to an exact probe at the start of each block; no node is ever renamed, and no name setter is present or required (decision [DEC-PATHS](decisions.md#dec-paths)). The verbaliser breaks down facts using `zelph/parse`.

## Loading a Network Replaces the Graph

During an ACE session, when a user performs a `.bin` file load, the resulting graph must match the file's contents precisely, and the front end's node id caches must reflect this change. Before the merge preceding the engine work, the `.load` command integrated the file into the session based on node id. Currently, every network-file load – including `.load <file>.bin`, `zelph/load`, the C API's `zelph_load`, `.load-partial`, a manifest, or a dump whose `.bin` cache is accessed – triggers a replacement of the session's graph (P-load-replace; decision [DEC-RELOAD](decisions.md#dec-reload); [Persistence](../janet.md#persistence)):

- A failed load at the file's header preserves the graph unchanged; any other load initially removes all index entries, all cluster associations (leaving none active), the input focus, the record of additions behind `run-delta`, the negation records and `(zelph/defeated)`, the running Janet block's `'X` variables, the display registrations (including the digit alphabet used in the maths stack), the handles for compiled networks, the import registry (retaining only the claims from imports currently in progress), and the session's node annotations; then, the file's own annotations are introduced.
- The Janet environment and its associated values remain intact, as do the registered keywords, `.lang`, the configuration settings, and the `.negation-records` switch. A Janet value that previously referenced a node is not repaired.
- Auto-run is deactivated, and the host explains: `Auto-run has been disabled, as every .load disables it: inference after each input line can be slow on a loaded network. Use .auto-run to enable it again.` The front end thus performs inference solely for a question until the user reactivates `.auto-run`.
- Since the import registry is erased, `.import ace` executed after a load runs the modules within the same Janet environment; `zelph/register-keyword` replaces the handler. Every front-end module must endure this re-execution, which a test pins.
- A dump import into an empty graph reads or writes the dump's `.bin` cache; into a non-empty graph it adds the dump and announces the addition. `.import ace` writes nothing, so the graph remains empty with respect to the dump's cache.

`(zelph/epoch)` serves as the mechanism by which Janet caches tell: a process-wide incrementing value that advances at the end of each engine construction (`.new`, each C API engine), upon every dump import (even when the dump contributed no data), and during each replacing load. This counter operates independently and is distinct from the engine's internal graph epoch, which must not increase during an import, as the session layer interprets such an increase as "the graph was replaced". The front end keys every cache of graph-derived data – the word cache, the lexicon caches, rule registry, reported contradictions, maths hook table, and memo of declared opaque predicates – using `(zelph/epoch)`, and the first block of a new epoch drops them.

## Proofs, Provenance and Retraction as Data

The front end responds to "why" questions in ACE, can echo each derived sentence alongside the premises it rests on, and may retract a sentence ("ace-retract") when nothing relies on it. Before the engine work, there existed no binding for a proof, for the facts generated during a run, for anything contingent on a fact, nor for a way to eliminate a single fact from Janet; furthermore, provenance could not be stored as facts about rules, since a fact naming a rule node would deactivate that rule.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :justify true)
    true
    zelph> ace John is a man.

    zelph> ace Every man is a human. Every human is mortal.

    zelph> ace Is John mortal?

    Answer: Yes. John is mortal.
      "Every human is mortal." (block 2, sentence 2)
      "Every man is a human." (block 2, sentence 1)
      "John is a man." (block 1, sentence 1)
    ```

The proof originates from `zelph/explain`; the quoted sentences represent the provenance that the front end wrote into the node annotations of each fact and rule at the time the blocks were written ([Why: Justified Answers](answers.md#why-justified-answers) specifies the lines). On the engine side, a single fact bears a manual annotation:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> (X ~ man) => (X ~ mortal)
zelph-> socrates ~ man
zelph-> x says (socrates ~ man)
zelph-> %(zelph/annotate (zelph/fact "socrates" "~" "man") "source" "Socrates is a man.")
true
zelph-> %(each e ((zelph/run-delta :quiet :derived) :derived) (zelph/out (string (e :fact) "  rests on  " (string/join (map string (e :premises)) ", "))))
socrates ~ mortal  rests on  socrates ~ man
zelph-> %(def proof (zelph/explain "socrates" "~" "mortal"))
{:absent () :bindings {<zelph/node X> <zelph/node socrates>} :cycle-leaf false :fact <zelph/node socrates ~ mortal> :more false :premises ({:absent () :bindings {} :cycle-leaf false :fact <zelph/node socrates ~ man> :more false :premises () :status :axiom :walk-edges () :walked ()}) :rule <zelph/node (X ~ man) => (X ~ mortal)> :status :derived :walk-edges () :walked ()}
zelph-> %(zelph/annotations ((get (proof :premises) 0) :fact) "source")
("Socrates is a man.")
zelph-> %(zelph/dependents "socrates" "~" "man")
(<zelph/node socrates ~ mortal>)
zelph-> %(zelph/holders "socrates" "~" "man")
(<zelph/node x says (socrates ~ man)>)
zelph-> %(zelph/prune-fact (get (zelph/holders "socrates" "~" "man") 0))
:removed
zelph-> %(zelph/holders "socrates" "~" "man")
()
```

The quiet run returns the fact it derived, along with the premise it rests on; `zelph/explain` returns the proof as a struct; the premise's annotation quotes its source; `zelph/dependents` lists what a rule derived from the fact in a single step, and `zelph/holders` indicates the statement that nests it. Taking the fact back would leave the dependent behind while taking the holder with it, which is why the front end's `ace-retract` refuses such an action; the holder alone can be removed, and once that occurs, no entity holds the fact.

**The bindings, as built** (documented in [Proofs as Data](../janet.md#proofs-as-data-zelphexplain), [Running quietly](../janet.md#running-quietly) and [Node annotations](../janet.md#node-annotations)):

- **`(zelph/explain fact)`, `(zelph/explain s p o & objs)`** (P-explain) returns the proof that `.explain` outputs, one struct per step: `:fact`, `:status` (`:derived`, `:axiom`, `:unfounded`, `:rule-pattern`, `:mentioned`, `:rule-mentioned`, `:truncated`), `:rule` (for a step derived via a rule), `:premises`, `:absent` (the patterns of the negated conditions), `:walked` and `:walk-edges` (path conditions), `:cycle-leaf`, `:more`, `:bindings`, and `:fallback true` at the root if the depth-limited fallback provided an answer. The fact is resolved without creating anything and exactly as given; nil if the graph holds no such fact. The depth limit is 4, and the search is bounded by the session's `.explain-budget` (default 80,000 units). Runs only on the main thread. Two readings bind the front end: `:unfounded` applies both to a fact whose premise was pruned and to a conclusion under "not provably" that a later fact defeated, which `(zelph/defeated)` from the same run distinguishes; and a premise is the fact matched by a condition, the broader fact included, and the witness of a fresh variable is explained by the firing that made it.
- **`:derived`** (P-derived): `(zelph/run-delta :quiet :derived)` adds `:derived D`, one struct per written fact in derivation order (a rule with two consequences yields two entries): `{:fact :rule :premises :absent :bindings}`, with premises retrieved and never created. A seeded run returns only what it derived from the new facts. This is the derivation echo of the `:echo` option (decision [DEC-ECHO](decisions.md#dec-echo)).
- **`(zelph/annotate node key value)`, `(zelph/annotations node &opt key)`, `(zelph/unannotate node key &opt value)`** (P-annotate): a side store beside the graph, mapping node → key → list of strings in insertion order, each value once. Annotations are not nodes, edges, names, or mention: queries and rules never see them, and an annotated rule stays in force. Every removal of the node erases them, so a fact re-created under the same ID starts bare; a name merge carries them to the surviving node; a dropped cluster takes the annotations of its nodes; a withdrawn claim keeps them. They are saved as an optional segment in the `.bin` file (a new header field): a file without annotations is byte-identical to one written by the prior engine, and an older engine loads a file with the section and ignores it. A full replacement load takes the file's annotations, while a partial or manifest load has none. `zelph/annotate` and `zelph/unannotate` run on the main thread, `zelph/annotations` on any thread. The front end writes its provenance under the key `ace:src` starting from the first milestone (decision [DEC-PROV](decisions.md#dec-prov)).
- **`(zelph/prune-fact f)`, `(zelph/holders f)`, `(zelph/dependents f)`**, each also available as `s p o & objs` (P-retract; decision [DEC-RETRACT](decisions.md#dec-retract); [Graph construction](../janet.md#graph-construction-mutating)):
  - `zelph/prune-fact` withdraws one claimed fact, just like the ground form of `.prune-facts`, exactly as given (`a p b` never prunes `a p b c`): `:removed` when the node was eliminated, including the statements that nest it; `:unclaimed` when it stays as a rule's pattern or a mention and only its claim is withdrawn (the node retains its annotations); nil when no claim exists. It invalidates the record of additions. A core predicate's relation-type declaration remains intact.
  - `zelph/holders` returns the statements that claim the fact by containing it, ordered by node ID; rule text, mentions, the fact's own markings, and the engine's bookkeeping are not holders; a membership in a set constant, or in a container pruned alongside, is passed through to the statements holding the container. What it lists is what `zelph/prune-fact` takes with the fact. It needs no budget and is always complete.
  - `zelph/dependents` advances the `.explain` engine by one step over the rules in force: the facts a rule produces in a single step, justified via the fact (either as a condition or as an edge traversed by a path condition), sorted by node ID. It lists only direct dependents, and a justification rather than all justifications: a fact with a second justification appears too, since zelph does not track derivation chains. It is limited by `.explain-budget` and ends with the keyword `:truncated` if the budget is exhausted, ensuring an unfinished search never returns nothing.
  - All three perform no creation, refuse a pattern (`the fact holds a variable, which makes it a pattern; .prune-facts removes what a pattern matches`), refuse a node that is not a fact, return nil for a name resolving to nothing, run only on the main thread, are refused inside a rule build, and are blocked on a partially loaded graph.

zelph lacks truth maintenance: pruning a premise results in the persistence of what was derived from it ([Data Cleanup Commands](../quickstart.md#data-cleanup-commands)). This is why `ace-retract` asks `zelph/dependents` and `zelph/holders` first and refuses with `[ace/retract-dependents]` if either is not empty ([Taking a Statement Back](answers.md#taking-a-statement-back-ace-retract)).

**Known limit.** A statement that a rule's conclusion nests is claimed by the firing, yet `.explain` treats it as an input and `zelph/dependents` fails to include it in its output. Until the engine item [L3-P1](prerequisites.md#l3-p1-a-statement-nested-in-a-conclusion-is-explained-as-an-input) fixes this discrepancy, each mapping row writes every nested atom at the top level as well (an event sentence writes `John waits nil` beside the occurrence `ace:event-<h> ace:occurrence (John waits nil)`), ensuring the front end's justifications and its dependents check remain fully comprehensive.

## Numbers and Characters Independent of the Language

An ACE number, "30" within the sentence "John's age is 30.", and an ACE string are transformed into a zelph number and character-list nodes, with the arithmetic stack performing operations on them. Before the engine work, `(zelph/number "3")` existed as one node beneath `.lang zelph` and a separate node beneath `.lang en`, meaning a number built under the user's language was not recognized as a number by the maths stack; characters resolved according to the current language and, on a Wikidata network, met items labelled "a" and "b"; and a `.lang` situated inside a module leaked to the caller.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .lang en
    en> .import ace-math
    en+> ace What is 2 + 3 * 4? What is 7 / 2?

    Answer: 2 + 3 * 4 = 14.
    No answer.
    ```

The user operates within `.lang en`; the block continues to compute, as its numbers are the nodes the arithmetic stack is named in, regardless of the currently configured language. `7 / 2` lacks a whole-number result, thus the question has no answer ([Numbers and Formulas](math.md) specifies the rest). The engine side:

```
zelph> .lang en
en> .import decimal-arithmetic
en+> .lang
The current language is 'en'
en+> %(def three (zelph/number "3"))
<zelph/node &3>
en> %(zelph/lang "zelph")
"en"
zelph> %(= three (zelph/number "3"))
true
zelph> %(= (zelph/list-chars "ab") (do (zelph/lang "en") (zelph/list-chars "ab")))
true
```

The decimal substrate was brought in under `.lang en`, and `.lang` continues to state `en` afterwards: the module's own `.lang zelph` ended with the module. The literal 3 built under `en` is the node built under `zelph`, just as the character list of a string is.

**As built** (P-number-lang, variant (i); decision [DEC-MATH-LANG](decisions.md#dec-math-lang); documented in [Multi-language Support](../modules.md#multi-language-support) and [Lists](../janet.md#lists-zelphlist-and-zelphlist-chars)):

- `zelph/list-chars` resolves each character within `zelph`, regardless of what `.lang` specifies, ensuring that `zelph/number` for every arithmetic substrate and the compact `<abc>` syntax refer to identical nodes across all languages. Node lists `< a b c >` maintain their role in naming nodes within the current language: they list nodes, not individual characters. `zelph/int` names its vocabulary within `zelph`; `zelph/int` applied to a non-negative literal returns the corresponding natural numeral.
- The `.lang` directive of a module concludes with the module itself: after the final line of the module, the importer's language is reinstated before executing the postponed run, and likewise when the module terminates due to an error. A session script keeps its `.lang` setting, just as individual typed lines do.
- All 17 modules comprising the arithmetic stack begin with `.lang zelph`, meaning the stack is named in `zelph` irrespective of the language under which it was imported. Arithmetic thus operates under `.lang zelph`, the language in which the front end is running.

The front end builds numbers exclusively via `zelph/number` and `zelph/int` (utilizing the hook table of `ace-math`, as detailed in [The hook](math.md#the-hook)), without employing any additional number implementation, and constructs strings using `zelph/list-chars` within the context of `.lang zelph`.

## Statements About Statements: Opaque Predicates

"John believes that Mary waits." conveys information regarding John alone and carries no implication about Mary: writing `John believes (Mary waits nil)` must not cause `Mary waits nil` to be considered a fact. Before the engine's update, each component of a plain statement was treated as claimed, and the sole statement zelph could hold without asserting it was a rule. Now, zelph supports opaque predicates: `=>` serves as the built-in one, and the fact `P ~ opaque` makes any predicate P operate in the same way.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Mary believes that Bill waits. No man waits. Bill is a man. Does Bill wait?

    Answer: No. It is false that Bill waits.
    ```

The front end marks `believes` as opaque at its first that-clause, meaning what Mary believes is a mention, and the assertion "No man waits." refutes it without generating a contradiction. Before the introduction of opaque predicates, the identical passage reported a contradiction, since the believed content was treated as a claim. Natively:

```
zelph> believes ~ opaque
zelph> mary believes (bill waits nil)
zelph> S waits O
zelph> (*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)
zelph> bill ~ man
((bill waits nil) ~ refuted) ⇐ {(bill ~ man)}
zelph> X ~ refuted
Answer: (bill waits nil) ~ refuted
zelph> %(zelph/fact "mary" "believes" (zelph/mention "bob" "sleeps" "nil"))
<zelph/node mary believes (bob sleeps nil)>
zelph> %(zelph/exists "bob" "sleeps" "nil")
false
```

`S waits O` finds nothing: the believed statement is no data. The derived refutation meets merely a mention and constitutes no contradiction. From Janet, `zelph/mention` produces an argument of the same type.

**The semantics, as built** (P-opaque; decision [DEC-MODAL](decisions.md#dec-modal) (d); documented in [Mentioning a Statement: Opaque Predicates](../logic.md#mentioning-a-statement-opaque-predicates) and [Claimed or Merely Written Down](../janet.md#claimed-or-merely-written-down)):

- "A statement claims itself and every fact nested in it outside an opaque argument position." The subject and objects within an opaque fact serve as mention scopes: any new creation made there gets the same mark type as a rule's ground pattern (one specific mark category); anything that existed prior retains its original status; a subsequent claim of the same statement revokes the mark. A fact marked by a refutation constitutes a second mention scope of the same type.
- **`(zelph/mention s p o & objs)` and `(zelph/mention thunk)`** generate a statement without asserting it and yield its node; an existing fact is returned unchanged, whether claimed or refuted. The thunk variant operates similarly to `zelph/rule-text`'s thunk, within a scratch cluster and without claiming anything; a thunk returning no node is refused and leaves nothing. The C API provides `zelph_mention`, Rust offers `Engine::mention`.
- **A rule expressed as a mention is not in force.** A `=>` written by `zelph/mention` or `zelph_mention` bears the same mark as a rule's ground pattern: it remains unlisted, does not fire, and a rule over rules does not read it, until something claims it (decision [DEC-MENTION-RULE](decisions.md#dec-mention-rule)). A claim that puts it in force verifies it in the manner a typed rule would be checked, and a refusal leaves the mention unchanged. The front end writes the rule of a that-clause, "Mary believes that every man waits.", in this manner, embedded within the opaque statement that contains it (M8).
- `.explain` tags a mention `[mentioned; not asserted]`, `.node` outputs `Mentioned (not asserted): yes`, `zelph/explain` returns `:mentioned`; the label is chosen by the node's position.
- **The declaration** `P ~ opaque` (`opaque` a node in the `zelph` language) is refused when a fact of P already holds a statement, with `fact(): a predicate cannot be declared opaque while one of its facts holds a statement -- <P>: <the fact>`, and for a core predicate with `fact(): a core predicate cannot be declared opaque -- <name>`; removing a declaration is refused similarly, as is a name merge that would make a predicate opaque. The node `opaque` keeps its name and does not merge with any other node.
- Typed input encloses every argument of an opaque predicate that is neither a plain atom nor a variable within a mention; a firing decides opacity per firing based on its bound predicates and writes mentions in opaque positions; Janet's `zelph/fact` claims an opaque fact and nothing it holds, so an argument constructed via a separate `zelph/fact` call would already be claimed and thus is written using `zelph/mention` instead.
- Readers: a claim terminates at an opaque argument; deep unification reads inside it by structure; the top-level candidate scan never takes a mention as data; negation levels read the predicate of an opaque fact and nothing within its arguments; pruning a claimed statement that a mention also holds withdraws the claim while preserving the mention. A query asserts what it generates within an opaque argument, as if under a mentioned `=>` (decision [DEC-OPAQUE-QUERY](decisions.md#dec-opaque-query)); the front end remains unaffected, as its queries execute in a scratch cluster it drops.
- The performance guard pins the number of pattern marks a claim revokes during the Jacobian workload precisely (15), since revoking a non-leaf mark takes a path that might disarm the fact stores.

**How the front end uses it** (milestone M8; [Modality and That-Clauses](semantics.md#modality-and-that-clauses)): it declares every modal or that-clause predicate (`ace:can`, `ace:must`, `ace:should`, `ace:may` in `zelph`, a verb like `believes` in `ace`) with `(zelph/fact P "~" "opaque")` once per `(zelph/epoch)`, at initial usage and outside a question's scratch cluster (a declaration written there would go with its drop); it writes a that-clause as `(zelph/fact holder P (zelph/mention (fn [] …)))`. Three forms remain refused by construct id in this version: a that-clause holding a conditional with referents, when the that-clause stands in a rule's condition (`:sub/conditional-with-referents-in-body`: the condition would bind the stored rule's ground parts, which is unsound for ACE) or in its conclusion (`:sub/conditional-with-referents-in-head`: a firing cannot write such a mention), and a that-clause holding a fully ground conditional (`:sub/ground-conditional`), which needs the record that a rule was asserted ([K1-P1](prerequisites.md#k1-p1-and-k1-p2-a-record-that-a-rule-was-asserted)). [A Rule Inside a That-Clause](constructs.md#a-rule-inside-a-that-clause) explains the three refusals.

## Boxes as Sets: Set Patterns

"John can enter a card." is represented by a box containing two conditions and a referent: John performs an entry of an object, and that object is designated as a card. The front end encodes this as a set constant composed of mentions: `{(Card-<h> ~ card) (John enters Card-<h>)} ace:can nil`, with `<h>` denoting the 16 hexadecimal digits corresponding to the card's content address, and any condition or question involving this box must match that set by its members. Before the engine work, a set literal incorporating a variable functioned as an independent collection and did not match any stored set constant.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace If John can enter a card then John is able. John can enter a card. Is John able?

    Answer: Yes. John is able.
    ```

The session needs modality, which the front end enables starting at milestone M8; before that, "can" is refused by its construct id. The engine side, with the rule and the box written natively in the form the front end constructs (`card1` represents the individual `Card-<h>`):

```
zelph> ace:can ~ opaque
zelph> (*{({(X ~ card) (john enters X)} ace:can nil)} ~ conjunction) => (john is able)
zelph> {(card1 ~ card) (john enters card1)} ace:can nil
zelph+> S is able
Answer: john is able
zelph> S enters O
zelph> .explain john is able
john is able
   └─ {(john enters card1) (card1 ~ card)} ace:can nil  [axiom]
```

The rule's single condition, a one-element condition set akin to every rule the front end writes, contains a set literal featuring the variable `X`; the stored box is a set constant composed of two mentions. The two meet through their members, triggering the rule for John, and `.explain` names the matched set constant as the premise. `S enters O` locates nothing, since the box contains only mentions.

**As built** (P-set-match; decision DEC-MODAL; documented in [A literal with a variable in a pattern](../concepts.md#a-literal-with-a-variable-in-a-pattern)):

- A **set pattern** refers to a set literal containing a variable: a template within a rule's text (`{X}` and `@{X}` belong to the same class, and a rule prints its `{X}` as `@{X}`, which re-enters), a set constructed by `zelph/set`, the typed `{…}` or the C API's `zelph_set` from elements that hold variables (a node class of its own, thus preserving its status as a set pattern even after saving and reloading), or a set constant whose members contain such a pattern, matched recursively at each level.
- Within a rule condition, a query, or a statement a condition holds and a subject that holds one, an unbound set pattern meets a **set constant** that is itself no pattern through a bijective pairing of its members; every valid pairing is listed. The number of members must be equal (`{X Y}` does not match `{a}`). Only a set constant is matched on the data side, because a collection may expand; a `zelph/collection` on either side keeps its identity and matches solely by identity.
- A membership condition puts its subject into the literal it designates: `(Z in {Y})` interprets as precisely the set {Z, Y}. A set pattern matches a box of mentions via its members.
- A justification identifies the set constant that the pattern matched, as demonstrated by `.explain` above, in a form that re-enters; a query over a set pattern results in nothing remaining; `.prune-facts` and `.explain` access the fact that the pattern matched.

The front end builds the pattern side using `zelph/set` (a template located within `zelph/dedup-rule`, a value in a question's scratch cluster) and the data side as a set constant composed of ground mentions, matching atom count precisely. Plural conditions based on set membership (milestone M7) rely on the identical match. Consequently, a box containing multiple conditions or a referent within a condition does not require an individual refusal.

## Negation Records and Defeated Conclusions

A conclusion under "not provably" in zelph is definitive: a later fact that defeats it fails to revoke it. ACE users must be informed at minimum. The engine keeps a record of each conclusion derived under a negated condition whenever negation records are active, and each execution re-evaluates those records.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace If a man is not provably married then the man is a bachelor. John is a man.

    zelph> ace John is married.

    Warning [ace/w-naf-final]: a later fact defeats what these conclusions assumed to be not provable; zelph keeps them, because conclusions under "not provably" are final:
      "If a man is not provably married then the man is a bachelor." (block 1, sentence 1)
        John is a bachelor.
    ```

The first block writes the rule `((X ~ man), ¬(X is married)) => (X ~ bachelor)` and switches the records on; its inference determines John qualifies as a bachelor. The second block's inference finds the conclusion defeated and returns it as data, which the front end turns into the warning ([Conclusions Under "Not Provably" That a Later Block Defeats](block.md#conclusions-under-not-provably-that-a-later-block-defeats)). The engine side:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> %(zelph/negation-records true)
true
zelph-> ((X ~ man), ¬(X is married)) => (X ~ bachelor)
zelph-> john ~ man
zelph-> %(zelph/run-delta :quiet)
{:contradictions () :naf-final ()}
zelph-> %(zelph/exists "john" "~" "bachelor")
true
zelph-> john is married
zelph-> %(each e ((zelph/run-delta :quiet) :naf-final) (zelph/out (string (e :fact) "  derived by  " (e :rule))))
john ~ bachelor  derived by  (¬(X is married), (X ~ man)) => (X ~ bachelor)
zelph-> %(zelph/exists "john" "~" "bachelor")
true
```

The first quiet run concludes that John is a bachelor. Once he is married, the next quiet run yields the defeated conclusion, accompanied by the rule that derived it, and prints nothing; the conclusion itself stays.

**As built** (decision [DEC-NAF-RECORDS](decisions.md#dec-naf-records); [Stratified Evaluation](../logic.md#stratified-evaluation)):

- Enabling `.negation-records on` or `(zelph/negation-records true)` switches the records on; by default, they are disabled. Check mode (`.semi-naive check`, the default for the test binary) maintains them as well.
- During a loud run, `Warning: N fact(s) no longer justified … they stay:` appears for each concluded statement that has been defeated (check mode reports `Negation check: …` and fails the run); a quiet run returns the identical pairs as `:naf-final` and prints nothing; `(zelph/defeated)` holds the most recent run's list. Each defeat is reported only once. `.run-once` never re-tests the records. The defeated fact stays; `.explain` calls it `[asserted; no derivation found]`.
- A defeat that a run reported within a cluster that has been dropped goes back to the records and is re-reported at the point where it remains valid; a merge into another named cluster passes it on.
- Loading discards the records while preserving the switch setting; `.new` resets the switch to off. Consequently, a conclusion in a loaded graph lacks a record, and its subsequent defeat is never reported.
- Overhead: approximately 250 B per record with a single bound variable and roughly 60 B for each additional one, plus around 1 second per million records in every run that re-tests them (with auto-run enabled, such a run follows each statement that adds a fact into a negated predicate).

**How the front end uses it**: it switches the records on upon encountering its first rule featuring a "not provably" condition, and maintains them perpetually, ensuring `[ace/w-naf-final]` works at the REPL default and not merely within the test binary; sessions lacking such a rule incur no cost. From that point onward, the session's native rules under negation are also recorded, as stated on the front end's user page. Tests evaluating conclusions across blocks run with `.semi-naive on` and explain their rationale; one test pins the warning during check mode and another at the REPL default.

## Wikidata Labels and the Label Index

Following a Wikidata import, ACE words should connect directly to Wikidata's items: "man" ought to refer to Q8441 without requiring any user input (the session [After a Wikidata Import](index.md#after-a-wikidata-import) demonstrates this). Before the engine work, the importer gave a label shared by multiple items to the item it processed last, and each prior carrier lost its English name: in the published networks, the intended item held its label for 0 out of 24 test labels ("man" was a Drosophila gene), and Q8441, Q5, Q42, `P31`, and `P279` lacked an English name entirely.

```
zelph-> %(map |(zelph/name $ "wikidata") (zelph/lookup-all "man" "en"))
@["Q8441" "Q709369" "Q29719014"]
zelph-> %(zelph/name (zelph/lookup "man" "en") "wikidata")
"Q8441"
zelph-> %(map |(zelph/name $ "wikidata") (zelph/lookup-all "human" "en"))
@["Q5"]
zelph-> %(zelph/lookup-all "nothing" "en")
()
zelph-> %(nil? (zelph/lookup "wikidata:labels-v2" "zelph"))
false
zelph-> %(zelph/exists (zelph/lookup "Q8441" "wikidata") (zelph/lookup "P279" "wikidata") (zelph/lookup "Q5" "wikidata") :exact)
true
```

The session begins by importing a dump containing seven entities (not shown). The term "man" is associated with three carriers, listed in rank order; the name resolves to the top-ranked carrier; "human" is linked to a single carrier; an unknown word has no carriers assigned; the importer wrote its marker; and the exact probe finds the subclass fact.

**As built** (P-wd-labels, P-label-index and its binding `zelph/lookup-all`; decision [DEC-WIKIDATA](decisions.md#dec-wikidata); documented in [Labels that several entities share](../wikidata.md#labels-that-several-entities-share)):

- Each imported item keeps its label as its `en` name; an item lacking an English label gets its `mul` label (Q42 possesses only that). The label search is restricted to the entity's labels object.
- The name → node entry for a shared label is assigned to the first carrier according to ranking – a property precedes an item, which precedes any other node, followed by the smaller Wikidata number, then the node id – regardless of the dump's sequence or import thread order. `.name` maintains its takeover semantics.
- An index lists every carrier of a label, preserved solely for languages where the two name maps diverge, updated by the import, every removal, rename, takeover and merge, and reconstructed after a load. When the node a label resolves to loses it, the first-ranked surviving carrier assumes it, ensuring that a pruned network keeps resolvable labels.
- `(zelph/lookup-all name &opt lang)` returns the carriers in rank order (the holder alone if the label is not shared, the empty tuple if nothing carries it, never a variable or a core node), or the keyword `:incomplete` following a partial load that picked fewer name chunks than the file contains (session-wide, for each language). After a takeover via `.name`, the holder need not come first, so a caller never assumes it does; the front end independently selects the lowest Q-number.
- Each successful import writes the marker `wikidata:labels-v2` into `zelph`, which `.cleanup` and predicate slices keep; the front end turns label resolution off, using `[ace/w-old-labels]`, on a Wikidata graph lacking this marker. The `.bin` format remained unchanged; the published networks hold the old labels until regenerated using this engine.
- The importer also reads every main statement in situ: previously, a property whose first statement lacked a value lost all its item claims, and a statement carrying a hash was ignored. On the dump's initial 842 entities, the import duration dropped from 3.3 s to 0.215 s and restored the single lost claim of 31,783 (the effect on the full dump remains unmeasured).
- Memory usage, assessed on a synthetic fixture of 300,000 items: approximately 52 B per `en` entry of a carrier that does not hold the label, roughly 63 B per shared label plus 8 B per carrier for the index, about 61 B per `mul` name on disk. Extrapolated (an estimate), the index occupies roughly 0.24 GB on the full graph.

The manner in which the front end resolves a word via these – by role, in a deterministic fashion, announced upon label sharing – is detailed in [Resolving a Word](words.md#resolving-a-word).

## The ace Network Tooling

The `-small` and `-medium` networks, as released, fail to include most of what ACE's words are meant to access, since the pruning method used to trim them eliminates such content: animals and plants are taxa, Earth and the planets each carry an epoch statement (`P6259`), countries reference themselves via `P17`, cities are linked through `P131`, and the pruning recipe strips out every item matching these shapes. Rebuilding restores the labels, but not the underlying data. Consequently, the front end gets a distinct network, `<stem>-ace.bin`: a filtered subset of the dump is imported into a freshly empty graph. The tooling is general-purpose and included within the repository; no modification to the engine was needed for this process:

- `dev_scripts/wikidata-subset.py` (utilizing only the Python standard library) featuring subcommands `scan` (transforming a dump into one table row per entity: sitelinks, the largest amount of each quantity the criteria name, every item-valued main claim at every rank), `keep` (taking a table and criteria to produce a list of retained entities along with their justifications), `extract` (converting a dump and keep list into a dump of the retained entities, their `en` and `mul` labels, and their item claims), `accept` (processing criteria and keep list to generate a zelph session that checks the network), `verdict` (analysing the session's log to yield figures, warnings, and failures), and `criteria` (performing validation exclusively). Command syntax is detailed in [Advanced Commands](../wikidata.md#advanced-commands).
- `dev_scripts/ace-network.json`, the criteria expressed as data: each property; the subclass-of graph excluding instances of seven classes related to chemistry, biology, and coins; items possessing at least 30 sitelinks that are not internal to Wikimedia; sovereign states and their capitals; settlements with populations of 100,000 or more; taxa at rank order or above, or those with at least 20 sitelinks; given names; value items referenced by at least 20 kept entities; and the upward subclass closure of all such elements. It also holds the acceptance list: the words that must resolve to given items ("man" to Q8441, "Douglas Adams" to Q42, "occupation" to P106, …), facts and subclass paths that must be valid, required absences, and size targets.
- `tools/check_wikidata_subset.py`, a 62-entity dump structured in the real layout with 24 named cases, registered as the `ctest` entry `wikidata_subset_holds_what_its_criteria_keep`.
- Five stages in the maintainer's regeneration script (`ace-scan`, `ace-keep`, `ace-extract`, `ace`, `accept-ace`), executed without access to the full network (estimated duration: two hours).

Before the network undergoes regeneration, its dimensions remain an approximation based on current Wikidata: approximately 6.8 to 7.8 million nodes, consuming 1.8 to 2.5 GiB of memory, occupying 0.7 to 0.9 GB on disk, and requiring 6 to 8 seconds to load (target benchmarks: no more than 10 million nodes, 3 GiB, 1.2 GB, and 10 seconds). The responsibility for executing the regeneration and publishing the file lies with the maintainer. [A Network for ace](words.md#a-network-for-ace) shows the complete criteria and explains how the front end uses the network.

## Engine Behaviour Made Precise Along the Way

The rounds that built the items above also established overarching engine behaviour that changes what the front end and its tests observe. Each one is documented on the user pages and pinned by tests.

- **A question yields no residue.** Each typed query runs inside a dedicated scratch space, and once finished, every element it generated that holds one of its variables is removed, along with a relation-type declaration `P ~ ->` it produced, provided nothing that stays depends on it; the remaining ground nodes remain intact. A query's collection literal lacking a variable is also discarded, while a set constant endures. A program's own `zelph/query` retains its input; the cluster idiom produces no lasting effect (decision [DEC-RESIDUE](decisions.md#dec-residue); [A question that leaves nothing behind](../janet.md#a-question-that-leaves-nothing-behind)).
- **The engine's bookkeeping remains hidden from conditions, queries, closures and `zelph/exists`:** the marking of a contradiction record, the memberships of a record not named by any statement, and the markings of rule patterns. A user's own refutation marking stays visible; rule text persists as data for rules over rules; `zelph/sources`, `zelph/targets`, `zelph/mentioned` and `.node` remain structural, enabling the front end's `ace/contradictions` to interpret the records (decision [DEC-BOOKKEEPING](decisions.md#dec-bookkeeping); [Contradiction Detection](../logic.md#contradiction-detection)).
- **The engine's vocabulary is independent of any cluster and retains its names:** `refuted`, `opaque`, `rule pattern`, `closure`, `one-or-more`, `zero-or-more`, `nn`, `nn-layers` and `wikidata:labels-v2` cannot be renamed, given away or merged (decision DEC-VOCAB).
- **A nested statement is read as a subset across all evaluation sequences:** `(a p b) bad Z` is stated by `(a p b c) bad k`, with anchors active or inactive, following a load and within a negation; the top level continues to match a condition's objects injectively ([Facts with Multiple Objects](../logic.md#facts-with-multiple-objects)). The front end writes one object per fact and meets the difference solely over native data; one reading at every level is the engine item [Z2-P1](prerequisites.md#z2-p1-one-reading-of-a-patterns-objects-at-every-level).
- **A query answers each binding exactly once**, along every route (the REPL, `zelph/query`, `zelph_query`, Rust), even when both a narrower and a wider fact state it ([Key Features](../queries.md#key-features)).
- **Every printed form re-enters as the identical node:** path and `≈` conditions print their sugar outside premises, a rule's one-element condition set renders as `(*{…} ~ conjunction)`, a set pattern renders as `{…}` (within a rule's text `@{…}`), and a comma-separated list held by a statement renders as a comma list. This enables the front end's `ace/zelph-of` lines and the engine's own echoes to be re-typed.
- **Pruning removes the statements that hold a pruned fact** and keeps what a rule or a mention is built from; `.prune-nodes` continues to take the rules from which its victims are built (decision [DEC-PRUNE-HOLDERS](decisions.md#dec-prune-holders)). The front end never uses `.prune-nodes`.
- **Name merges are governed by content identity:** a statement or a set constant is recognized by its content, any other node by its identifier. Atoms merge; two ground statements sharing the same subject and predicate merge into the single statement containing the objects from both; a statement or a set constant merged with an atom survives as the statement or set, which then reads as a definition of the atom. Any other attempt to merge statements or set constants is refused before any modification: if the subject or predicate differs, if one is contained within the other, if a rule’s variable appears inside, if a core node is involved, if a statement and a set constant are combined, if an atom holds a member not present in the set constant, or if two set constants are merged, because a set constant is defined by its members. A merge is planned before any modification occurs, preserves claims, and carries the cluster journals, records and defeats (decisions [DEC-NAME-MERGE](decisions.md#dec-name-merge) and [DEC-MERGE-CONTENT](decisions.md#dec-merge-content); [Languages & Names](../quickstart.md#languages-names)). The front end never calls `.name` (decision DEC-RELOAD); its lexicon serves as the route for binding a word to a node.
- **A refused firing and a refused statement leave the graph unchanged:** a consequence the engine refuses is reported (in a quiet run, via an entry with `:refused`) and retracts the nested parts, fresh witnesses, revoked marks and refutations it generated, then continues execution; a typed statement constructing parts operates within a scratch space that a refusal drops. A firing with several consequences keeps those written before a refused one (decision [DEC-FIRING](decisions.md#dec-firing)); the front end needs the firing to be atomic, which is the engine item [Y2-P1](prerequisites.md#y2-p1-a-rule-with-several-conclusions-is-applied-by-halves). A consequence that the binding cannot write (its predicate among its objects) is skipped silently and produces no effect (decision [DEC-SILENT-SKIP](decisions.md#dec-silent-skip)).
- **A set constant is never extended**, neither by a firing nor by a rule constructed by a firing; the refusal reads `a set constant cannot be extended -- {c d} IS its members. …` ([Set constants](../concepts.md#set-constants)).
- **A rule can derive a fact whose subject is its predicate**, and that fact stays visible.
- **The echo of a `%` line** prints a struct, table, tuple or array on a single line in Janet's `%q` format, truncated after 1000 bytes ([Block Mode](../janet.md#block-mode)).

## What zelph Still Needs

Seven engine items remain. They were categorized into the fourth of four categories: low-hanging fruit and clear bugs were resolved immediately, a broader correction that is clearly manageable was made unless it triggered cascading effects, and the remaining items – modifications to the engine's model too substantial for a single fix cycle – are detailed with example sessions and incorporated into the proposed development effort. The items are the positive record of a rule being asserted (two items: a mention of a ground rule takes the typed rule out of force, and a rule generator cannot put a fully ground rule into force), a statement that a rule's conclusion nests being explained as an input, one reading of a pattern's objects at each level of a statement, the complete pass following the initial rule under "not provably", a comma list held by node that a firing asserts, and the atomic firing. The front end does not await these. When one affects a construct, this version either refuses the construct by its identifier or keeps to a constraint on the mapping; two such constraints apply to this page's items: every nested atom is also written at the top level, and a comma list obtained elsewhere is never inserted into a conclusion by node. Each item is documented, specifying today's failure mode, how this version of the front end handles it, what a fix must achieve, its validation tests, and associated risks, at [What zelph Still Needs](prerequisites.md); [The Seven Items at a Glance](prerequisites.md#the-seven-items-at-a-glance) provides the summary.

## Tests That Pin the Foundation

The tests that implementing sessions depend on, per item. Each case's comment explains its purpose. The names correspond to doctest names in `src/test/`, with the exception of the two `ctest` entries, which run Python checks from `tools/`; a quoted name starting with `…` continues the preceding name.

| Item | Test file | Principal test cases |
|---|---|---|
| Janet 1.42.1 (nested imports) | `tools/check_janet_gc.py` (`ctest` entry `janet_programs_survive_garbage_collection`) | "a script imports a file that collects and allocates 100,000 strings"; "a script allocates 30,000 strings and then imports math" |
| P-claim | `test_rule_patterns.cpp`, `test_capi.cpp` | "rule patterns: a fact asserted from a block keyword handler revokes the mark"; "capi: a fact asserted through the C API claims a rule's pattern" |
| P-quiet-run (b) | `test_output.cpp` | ".deductions focus: a statement after a run in the same block is still an anchor" |
| P-eof-autorun | `test_inline_keyword.cpp` | "input end: a half-read block is dispatched, not discarded" (SUBCASE "a keyword block dispatched by the end of input is followed by a run") |
| P-error-envelope | `test_syntax_errors.cpp` | "syntax errors: a keyword block is named, not by the blank line that ends it" |
| P-import-in-handler | `test_inline_keyword.cpp` | "block keyword: a handler that imports a script reads the script, not its own block" |
| variable scope of a handler call | `test_inline_keyword.cpp` | "block keyword: a handler call is one variable scope"; "block keyword: a zelph/import inside a handler call starts a new scope, a zelph/run-script does not"; "block keyword: a quoted atom escapes the keyword" |
| P-auto-run | `test_script_run.cpp` | "zelph/auto-run?: reports .auto-run, and false while a module is imported" |
| P-autorun-skip | `test_inline_keyword.cpp` | "block keyword: a handler that ran after its last write is not followed by another run"; "block keyword: a handler whose run left the negation records untested is followed by the host's run" |
| P-rule-api | `test_reasoning.cpp`, `test_rule_identity.cpp`, `test_capi.cpp`, `test_script_run.cpp` | "rules: zelph/rule with several consequences is ONE rule, like => (A) (B)"; "rule identity: dedup-rule refuses a thunk that does not return a rule"; "capi: a rule with several consequences is one rule, so they share a fresh variable"; "fresh variables: a predicate that no condition binds is refused when the rule is written"; "zelph/rule: every builder refuses a consequence whose predicate no condition binds"; "capi: a rule whose consequence has a predicate no condition binds is refused, and nothing is written" |
| a `=>` written with `zelph/fact` or `zelph_fact` (DEC-FACT-RULE) | `test_script_run.cpp`, `test_capi.cpp` | "zelph/fact: a `=>` whose consequence predicate no condition binds is refused at the end of its block"; "zelph/fact: a raw rule generator whose inner predicate the generator binds is kept"; "zelph/fact: a raw rule a statement of the same block holds is kept, and not in force"; "capi: a rule generator built from zelph_fact calls passes the run and fires" |
| P-record-pin | `test_contradiction_records.cpp` | "contradiction record: a one-condition rule built as a one-element condition set is recorded once"; "contradiction record: the printed one-condition set re-enters as the same rule"; "contradiction record: a one-element set that meets the plain rule entered first is recorded once" |
| P-quiet-run (a) | `test_script_run.cpp` | "zelph/run: an argument other than :quiet and :derived is refused"; "zelph/run: :quiet derives without a word"; "zelph/run: :quiet returns a contradiction it records"; "… a contradiction a record already holds"; "… a contradiction it cannot record"; "… a contradiction once however often the run meets it"; "zelph/run: :quiet names the claimed fact that a derived refutation meets"; "zelph/run: :quiet in check mode returns the negation check as data and prints nothing" |
| P-rule-list | `test_rule_identity.cpp` | "rule identity: zelph/rules lists the rules in force with their parts" |
| P-strata-transparent | `test_stratified.cpp` | "stratified: zelph/strata marks a rule over predicates"; "stratified: zelph/strata lists the rules written in a cluster until the cluster is dropped" |
| P-scratch-drop | `test_clusters.cpp`, `test_performance_guard.cpp` | "clusters: dropping a cluster nothing outside refers to keeps the fact stores armed"; "clusters: a cluster that only added a member to an older collection leaves the collection as it was"; "performance guard: a dropped cluster leaves the math stack's stores armed" |
| P-drop-names | `test_pruning.cpp`, `test_clusters.cpp` | "pruning: a prune walks no name map"; "clusters: a dropped cluster walks no name map" |
| P-restore-mark | `test_clusters.cpp` | "clusters: a restored rule pattern is recorded in no cluster" |
| P-drop-delta | `test_script_run.cpp` | "run-delta: falls back to a full pass after a dropped cluster held derivations"; "run-delta: falls back to a full pass after a pruned derived fact"; "run-delta: falls back to a full pass after a run that an error stopped"; "run-delta: a rule whose last mention was removed sees the older facts" |
| P-pidx-local | `test_predicate_index.cpp` (slow) | "a fact of another predicate keeps the built index and the sidecar"; "a question keeps it"; "a SPARQL question keeps it"; "a ground fact of the predicate rebuilds it" |
| P-closure-delta | `test_seminaive.cpp` | "semi-naive: a write no path walks does not re-match a path rule"; "semi-naive: a new edge of the walked predicate re-matches the path rule" |
| P-naf-delta | `test_seminaive.cpp` | "semi-naive: a write a negation does not read does not re-match a NAF rule"; "semi-naive: a dropped fact that a negation reads re-matches the NAF rule"; "semi-naive: a NAF rule in a non-stratifiable component keeps its classic pass" |
| P-delta-new-rules | `test_script_run.cpp` | "run-delta: a rule added since the last run sees the older facts"; "run-delta: a statement that mentions a rule keeps the run incremental" |
| P-refute | `test_stratified.cpp`, `test_contradiction_records.cpp`, `test_clusters.cpp` | "negation: a statement a ground rule only writes down can be refuted"; "refuted marking: a hand-written one of a claimed fact is refused"; "refuted marking: a derived one meeting a claimed fact is a contradiction, recorded with both facts"; "refuted marking: a pattern with a variable can still be queried from Janet"; "refuted marking: a ground one from Janet is refused and names zelph/refute"; "clusters: dropping a cluster restores a weight a refutation inside it changed" |
| P-lookup | `test_janet_names.cpp`, `test_script_run.cpp` | "janet: zelph/lookup finds an existing node and creates none"; "janet: a string names no rule variable in the bindings that only read"; "janet: a strict name read does not fall back"; "janet: zelph/lang reads and sets the language and returns the previous one"; "janet: zelph/epoch advances at .load, .new and a dump import"; "Janet threads: a name is read in one language while the main thread switches it" |
| P-exact | `test_janet_names.cpp` | "janet: an exact probe answers a miss without widening"; "janet: zelph/degree counts a node's edges"; "janet: zelph/exists reads a statement among its arguments as a ground rule condition does" |
| P-parts | `test_janet_names.cpp` | "janet: zelph/parse decomposes a fact and refuses an atom" |
| `zelph/lookup-all` | `test_janet_names.cpp` | "janet: zelph/lookup-all lists every carrier and creates none"; "janet: zelph/lookup-all answers :incomplete after a partial load of the name sections" |
| P-load-replace | `test_bin_inspection.cpp`, `test_import_guard.cpp` | "load: a network file replaces the session's graph"; "import: a module imported again after a load registers what it registered before" |
| P-explain | `test_explain.cpp` | "janet: zelph/explain returns the rule and premises of a derived fact"; "janet: zelph/explain calls a conclusion unfounded once a later fact defeats its negated premise"; "janet: zelph/explain answers within the depth limit when the budget runs out, and says so"; "explain: the witness of a fresh variable is explained by the firing that made it" |
| P-derived | `test_script_run.cpp` | "zelph/run: :quiet :derived returns each derived fact with its rule and premises"; "zelph/run: :quiet :derived names a premise without creating one" |
| P-annotate | `test_annotations.cpp` | "annotations: an annotation is no fact: queries and rules do not see it, and an annotated rule stays in force"; "annotations: a removal erases them, so a fact re-created with the same id starts bare"; "annotations: .save/.load keeps them, and a file without the section loads unchanged" |
| P-retract | `test_pruning.cpp`, `test_explain.cpp` | "janet: zelph/prune-fact removes one claimed fact, as .prune-facts does"; "janet: zelph/holders lists the statements that claim a fact by nesting it"; "janet: zelph/prune-fact of a rule's asserted pattern withdraws the claim and keeps the rule"; "janet: zelph/dependents lists the derived facts that rest on a fact"; "explain-budget: zelph/dependents ends with :truncated when the budget is spent" |
| P-number-lang | `test_numbers.cpp`, `test_script_session.cpp` | "zelph/list-chars: the characters are the same nodes under every .lang"; "number literals: a literal denotes the same node under every .lang (all arithmetic modules)"; "number literals: an arithmetic module imported under another .lang names nothing in it"; "session script: a module's .lang ends with the module" |
| P-opaque | `test_opaque.cpp`, `test_capi.cpp`, `test_performance_guard.cpp` | "mention: a mentioned statement answers no query until it is claimed"; "opaque: a statement claims nothing in the argument positions of an opaque predicate"; "opaque: declaring a predicate opaque is refused while one of its facts holds a statement"; "opaque: a firing writes a mention where its consequence has an opaque predicate"; "opaque: negation levels read nothing inside an opaque argument"; "capi: zelph_mention writes a statement without claiming it" |
| a rule written as a mention (DEC-MENTION-RULE) | `test_opaque.cpp`, `test_rule_patterns.cpp`, `test_capi.cpp` | "mention: a rule zelph/mention writes is not in force"; "mention: a rule an opaque holder believes stays out of force, also once the holder is pruned"; "rule patterns: a rule over rules does not read a rule written as a mention"; "capi: a rule written with zelph_mention is not in force until zelph_fact claims it" |
| P-set-match | `test_sets.cpp`, `test_opaque.cpp` | "sets: a literal carrying a variable in a pattern matches a set constant by its members"; "sets: the members of a set pattern pair with a set constant's in every way that fits"; "sets: a membership condition puts its subject into the literal it names"; "sets: a set pattern in a rule survives .save and .load"; "opaque: a set pattern matches a box of mentions by its members" |
| negation records | `test_stratified.cpp` | "negation records: a defeat reported inside a dropped cluster is reported again where it holds"; "stratified: a base fact arriving later does not retract a negation that already succeeded" |
| P-wd-labels, P-label-index | `test_wikidata_import.cpp` | "wikidata import: every item keeps its label, and the lowest number holds the name"; "wikidata import: every carrier of a shared label is listed, in rank order" |
| the importer fix | `test_wikidata_import.cpp` | "wikidata import: a property whose first claim has no value keeps its item claims"; "wikidata import: a main snak that carries a hash yields its claim" |
| name merges by content (DEC-MERGE-CONTENT) | `test_name_merge.cpp` | "name merge: an atom merged with a statement becomes the statement"; "name merge: two statements of different subjects are refused, and nothing changes"; "name merge: two set constants are refused, and nothing changes" |
| the `ace` network tooling | `tools/check_wikidata_subset.py` (`ctest` entry `wikidata_subset_holds_what_its_criteria_keep`) | 24 named cases on a 62-entity dump |
| the performance guard | `test_performance_guard.cpp` | "performance guard: the Jacobian workload does the expected amount of work" |
