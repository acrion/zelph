# Design Decisions

Wherever this specification had more than one reasonable answer, the choice was taken as a design decision with a stable id of the form `DEC-<NAME>`. This page lists every one of them: the question it answers, the option taken, the reason, the alternatives that were weighed, and the milestone at which it takes effect. The other pages of this category cite the ids, so a reader who meets "DEC-ATOMIC" in a paragraph about the `ace` block finds the reasoning here.

Most decisions fall into one of two kinds. The first maps an ACE construct onto a zelph feature that already exists – a rule, a refutation, a negated condition, a path condition, an opaque predicate – and fixes the exact shape the front end writes. The second refuses a construct by its id where zelph cannot yet give the right answer, rather than writing a graph that answers wrongly. A third, smaller group concerns zelph itself: general engine behaviour that the work on the front end raised and that was decided and built before the funded work, released with zelph 1.1.0. Together they carry the central point of this specification: the reasoning exists, and the work is the mapping, written as stdlib `.zph` modules on top of documented engine features.

A reader with little time finds the decisions that shape the whole effort in five entries: [DEC-WORKMODE](#dec-workmode) (general engine work first, the remainder specified as bounded prerequisites), [DEC-ORDER](#dec-order) (the meaning structure first, so every refusal names a construct), [DEC-ATOMIC](#dec-atomic) (zelph itself validates each block), [DEC-STRATA](#dec-strata) (refuse what the engine cannot yet order) and [DEC-WIKIDATA](#dec-wikidata) (words reach Wikidata through their labels, deterministically and announced).

The session below shows several decisions at work in two blocks; it needs milestone M4, which brings negation.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Is John a man? John is a man. Mary is not a man.
    Is Mary a man? Is Bill a man?

    Answer: Yes. John is a man.
    Answer: No. It is false that Mary is a man.
    Answer: It is not provable that Bill is a man.
    zelph> ace Bill is a man. Mary is a man.

    Error in line "ace Bill is a man. Mary is a man.": Contradiction [ace/contradiction]: sentence 2, line 1, column 16: zelph already holds the opposite of this sentence, from "Mary is not a man." (block 1, sentence 3).
      <>Mary is a man.
    ```

- The first question comes before the statement it asks about and is still answered "Yes": a block is one theory, and its questions are asked after all its statements have been written and zelph has drawn its conclusions ([DEC-QTIME](#dec-qtime)). An empty line ends a block ([DEC-BLOCK](#dec-block)).
- "Mary is not a man." is classical negation; zelph records it as a refutation ([DEC-NEG](#dec-neg)). So a yes/no answer has three values: "Yes.", "No. It is false that …" when the graph holds the opposite, and "It is not provable that …" when it holds neither ([Yes/No Questions](answers.md#yesno-questions)).
- The second block contradicts the refutation. The front end first writes every block into a scratch cluster that it drops again, and lets zelph itself refuse what contradicts the graph; only a block that passes is written for real ([DEC-ATOMIC](#dec-atomic)). So "Bill is a man." was not kept either, although it was valid: a later "Is Bill a man?" still answers "It is not provable that Bill is a man." ([All or Nothing: the Dry Run](block.md#all-or-nothing-the-dry-run)). The message has a class, a stable id, a position and the marker `<>` at that position ([DEC-ERRORS](#dec-errors)); piped input stops at it.

## How to Read This Page

The decisions are grouped as the specification groups them: host and process, the engine capabilities the front end needed, then by milestone, then the words and Wikidata, the engine decisions taken while those capabilities were built, the three engine questions settled last, and the points settled while these pages were written. Each entry has the same parts:

- **Question** – what had to be decided.
- **Decision** – the option taken, with enough detail to implement it.
- **Why** – the reason that decided it.
- **Not taken** – the alternatives that were weighed, briefly, with the cost that ruled each out.
- **Milestone** – where the decision takes effect (milestones M0 to M9 are described in [Work Plan and Milestones](plan.md#the-milestones)); "engine, built" marks a decision about zelph itself that is already implemented, "engine, 1.1.0" one settled last for that release.

Engine items are named by their ids (`P-quiet-run`, `P-opaque`, …), described in [What zelph Already Provides](foundation.md#what-the-front-end-relies-on); the items that remain are listed in [What zelph Still Needs](prerequisites.md#the-seven-items-at-a-glance). Construct ids (`:naf/unstratified`, `:modal/can`, …) are those of [Constructs and Refusals](constructs.md#the-construct-table), message ids (`[ace/needs-math]`, …) those of [The Message Catalogue](block.md#the-message-catalogue).

Two decisions are moot, because the defect they were about was fixed before the front-end work began; one was replaced by a later decision. They keep their ids and a one-line entry, so that older references stay resolvable.

Specified sessions on this page show behaviour the specification requires and that no code implements yet; plain code blocks are real runs of the current zelph. The [Overview](index.md#two-kinds-of-transcripts) explains both conventions. As in [A Tour in Sessions](sessions.md#how-to-read-the-sessions), every specified session starts in a fresh zelph, and the `[ace/notice]` banner that `.import ace` prints once is left out. An error ends its session, because piped input stops at the first error. The wording of the messages in the sessions is illustrative unless the specification fixes it verbatim; their layout and ids are normative ([Messages](block.md#messages)).

## Index

| Id | Decision | Milestone |
|---|---|---|
| [DEC-JANET](#dec-janet) | Janet v1.42.1 plus one upstream fix | M0, built |
| [DEC-WORKMODE](#dec-workmode) | general engine work first; what remains is specified as prerequisites | engine, built |
| [DEC-HOSTFIX](#dec-hostfix) | every general engine item lands before the front-end code | engine, built |
| [DEC-MENTION-BUG](#dec-mention-bug) | moot | – |
| [DEC-DROP-DELTA](#dec-drop-delta) | a removal of derived facts invalidates the incremental run | M0, built |
| [DEC-ORDER](#dec-order) | DRS-first, with a smoke slice first | M0-M2 |
| [DEC-CORPUS](#dec-corpus) | APE's corpus fetched at build time, its small lexicon as an overlay | M0 |
| [DEC-DRS-ENTRY](#dec-drs-entry) | a public Janet DRS API; no `drs` keyword in v1 | M0 |
| [DEC-SWI](#dec-swi) | APE runs at development time only | – |
| [DEC-QUIET](#dec-quiet) | a quiet run prints nothing and returns contradictions and defeats as data | engine, built |
| [DEC-BINDINGS](#dec-bindings) | small Janet bindings: lookups, exact probes, degrees, the epoch | engine, built |
| [DEC-RUN](#dec-run) | the front end runs inference as the host would | engine, built; M1 |
| [DEC-ERRORS](#dec-errors) | message classes, stable ids, positions, the `<>` marker | M1 |
| [DEC-BLOCK](#dec-block) | the first blank line ends a block | M1 |
| [DEC-ATOMIC](#dec-atomic) | a dry run makes each block all-or-nothing | M1; option M4 |
| [DEC-QTIME](#dec-qtime) | one theory per block | M1 |
| [DEC-ECHO](#dec-echo) | the derivation echo and justified answers | M9 |
| [DEC-PROV](#dec-prov) | provenance in node annotations, from M1 on | M1, M9 |
| [DEC-MEANING](#dec-meaning) | the mapper writes domain structure | M1 |
| [DEC-LEX-STORE](#dec-lex-store) | shipped vocabulary as Janet data, user entries as graph facts | M1 |
| [DEC-LEX-SYNTAX](#dec-lex-syntax) | ulex blocks; an entry's symbol may be a Wikidata id | M1 |
| [DEC-LEX-GEN](#dec-lex-gen) | inflected forms are generated | M1 |
| [DEC-NAMES](#dec-names) | phrase predicates for prepositions | M1 |
| [DEC-CONTENT-LANG](#dec-content-lang) | replaced by DEC-CONTENT-NS | – |
| [DEC-MATH-IMPORT](#dec-math-import) | arithmetic needs an explicit `.import ace-math` | M1, M5 |
| [DEC-DATA](#dec-data) | numbers wait for `ace-math`; strings are character lists | M1, M3, M5 |
| [DEC-RELOAD](#dec-reload) | what a replacing load takes along; caches keyed by the epoch | M1 |
| [DEC-PLAYGROUND](#dec-playground) | a wasm smoke case at M1, a demo group at M4 | M1, M4 |
| [DEC-LOAD](#dec-load) | `ace/load!` and `ace/load-text!` load whole documents | M2 |
| [DEC-SHAPES](#dec-shapes) | `John waits nil` and `John is rich` | M3 |
| [DEC-OF](#dec-of) | relational nouns; no class atom on a Wikidata property | M3 |
| [DEC-INDIVIDUALS](#dec-individuals) | content-addressed individuals | M3 |
| [DEC-EVENTS](#dec-events) | the plain fact always, an occurrence only when modified | M3, M6 |
| [DEC-PATHS](#dec-paths) | an instance and a subclass predicate, pinned per graph | M1, M3, M4 |
| [DEC-GENERATOR](#dec-generator) | a substitution generator for property tests | M3 |
| [DEC-NEG](#dec-neg) | the reach of negation inside rule conditions | M4 |
| [DEC-STRATA](#dec-strata) | three shapes refused by id; the engine's verdict in the dry run | M4 |
| [DEC-NAF-RECORDS](#dec-naf-records) | a defeated NAF conclusion is reported, never withdrawn | M4 |
| [DEC-REFUTED](#dec-refuted) | a derived refutation of a claimed fact is a contradiction | M4 |
| [DEC-RECORD-SINGLE](#dec-record-single) | one-condition rules as one-element condition sets | M4 |
| [DEC-GENERATIVE](#dec-generative) | moot | – |
| [DEC-NESTED-IMP](#dec-nested-imp) | a rule generator for a switch, flattened otherwise | M4 |
| [DEC-QUNIV](#dec-quniv) | a guarded hypothetical check for conditionals in questions | M4 |
| [DEC-MATH-MODULE](#dec-math-module) | `stdlib/evaluation.zph` with `exquo`, `difference`, `compare` | M5 |
| [DEC-MATH-INDET](#dec-math-indet) | an ACE math variable is the node named as written | M5 |
| [DEC-MATH-LANG](#dec-math-lang) | numbers and the arithmetic stack live in `zelph` | engine, built |
| [DEC-NEQ](#dec-neq) | unique names for named constants, by node identity | M5 |
| [DEC-PLURALS](#dec-plurals) | plurals in full; mass nouns as portions | M7 |
| [DEC-MODAL](#dec-modal) | opaque predicates over one generalised mark | M8 |
| [DEC-RETRACT](#dec-retract) | `ace-retract`, refused while something depends on the fact | M9 |
| [DEC-LEX-LARGE](#dec-lex-large) | no shipped large lexicon | – |
| [DEC-CONTENT-NS](#dec-content-ns) | the front end names its content in language `ace` | M1 |
| [DEC-WIKIDATA](#dec-wikidata) | words reach Wikidata items through their labels | M1 |
| [DEC-UNKNOWN](#dec-unknown) | unknown words are read by their position | M1, M2 |
| [DEC-MULTIWORD](#dec-multiword) | a capitalised run is one name only where the split parse fails | M1 |
| [DEC-QWORD](#dec-qword) | a quoted word is one proper name | M1 |
| [DEC-THE-NAME](#dec-the-name) | `the` + an undeclared capitalised word is a name | M1 |
| [DEC-GOAL-DOCS](#dec-goal-docs) | the specification as a documentation category | – |
| [DEC-CAT4](#dec-cat4) | the remaining engine items are specified, not built | the funded work |
| [DEC-OPAQUE-QUERY](#dec-opaque-query) | a query asserts what it creates in an opaque argument | engine, built |
| [DEC-RESIDUE](#dec-residue) | what a query leaves, how names resolve, how forms print | engine, built |
| [DEC-DECL-RESIDUE](#dec-decl-residue) | a query or a dropped cluster takes back the relation-type declaration it wrote | engine, built |
| [DEC-REFUTE-ROUTES](#dec-refute-routes) | refutation follows one rule on every route | engine, built |
| [DEC-VOCAB](#dec-vocab) | the engine's own vocabulary belongs to no cluster | engine, built |
| [DEC-BOOKKEEPING](#dec-bookkeeping) | bookkeeping is hidden from queries and conditions | engine, built |
| [DEC-PRUNE-HOLDERS](#dec-prune-holders) | pruning removes the statements that hold a fact | engine, built |
| [DEC-NAME-MERGE](#dec-name-merge) | name merges are planned and refused before they change anything | engine, built |
| [DEC-BACKLOG](#dec-backlog) | the remaining design points of the engine work | engine, built |
| [DEC-UNBOUND-PRED](#dec-unbound-pred) | an unbound consequence predicate is refused when the rule is written | engine, built |
| [DEC-FIRING](#dec-firing) | a firing applies its consequences one by one, for now | engine, built |
| [DEC-SILENT-SKIP](#dec-silent-skip) | a consequence the binding cannot write stays silently skipped | engine, built |
| [DEC-FACT-RULE](#dec-fact-rule) | a `=>` written with `zelph/fact` or `zelph_fact` is checked when the outermost Janet evaluation ends, before a save and before every run | engine, 1.1.0 |
| [DEC-MENTION-RULE](#dec-mention-rule) | a `=>` written with `zelph/mention` is not in force until something claims it | engine, 1.1.0; used at M8 |
| [DEC-MERGE-CONTENT](#dec-merge-content) | name merges follow content identity | engine, 1.1.0 |
| [DEC-DOCS-PLACE](#dec-docs-place) | these pages live in `ace-spec/`; the user page keeps the name `ace.md` | – |
| [DEC-ONE-CONDITION-PRINT](#dec-one-condition-print) | `ace/zelph-of` prints a one-condition rule as zelph lists it | M1 |
| [DEC-VAR-LETTERS](#dec-var-letters) | variable letters in order of first occurrence, atoms in the order of the sentence; a class pattern takes a letter only where it has a variable of its own | M1 |
| [DEC-NUMERIC-REFERENT](#dec-numeric-referent) | a numeric referent gets no class pattern | M5 |
| [DEC-YESNO-WIDER](#dec-yesno-wider) | a yes/no question also reads facts with several objects, the one plain `zelph/exists` | M1; its test M3 |
| [DEC-DISCONNECTED](#dec-disconnected) | a conclusion unconnected to the condition is supported | M4 |

## Host and Process

These decisions fix how the work is organized: which engine work comes first, in what order the language is built, and how the front end is tested against the reference implementation of ACE without depending on it.

### DEC-JANET – Janet v1.42.1 plus one upstream fix {#dec-janet}

**Question.** zelph embeds the Janet language, in which the front end is written. Two defects in Janet's garbage collector made long sessions crash and made a module import from Janet unreliable. How are they fixed?

**Decision.** zelph uses Janet v1.42.1 plus the hunk of upstream commit `b437ae3feb` (Janet pull request #1888), applied by the existing build step `src/lib/patch_janet_gc.cmake`, which becomes a no-op once a Janet release contains it. Only upstream code is used; nothing had to be reported upstream. A separate-process test pins the nested import beside `tools/check_janet_gc.py`; the full test suite, the WebAssembly build with its node tests and the aarch64 cross build (Janet 1.42 enables NaN-boxing there) all run on it.

**Why.** Upstream pull request #1720, first released in Janet v1.42.0, roots every nested fiber in the code path that `janet_dobytes` takes: every nested-import test harness is clean under AddressSanitizer on v1.42.0, v1.42.1 and Janet's master branch and fails on v1.41.2. The second fix, for an error that leaves a top-level `%` line, is in no release yet, so its hunk is carried.

**Not taken.** A patch on the older v1.41.2 that would also have rooted the fiber in zelph's copy (the first plan, before the upstream fix was found); a Janet master snapshot (unreleased code); living with the crashes.

**Milestone.** M0; built.

### DEC-WORKMODE – general engine work first, the remainder specified {#dec-workmode}

**Question.** The front end needs engine features and engine fixes. Who builds them, and when?

**Decision.** Every general engine change the specification needs – defects, shortcomings, extensions – is made in zelph itself and never with ACE knowledge in C++. All of them were built before any ACE-specific code, in dependency order, so that no milestone of the front end waits for the engine. ACE-specific code (`stdlib/ace*.zph`, `stdlib/keyword-kit.zph`, the test file `test_ace.cpp`, the corpus wiring) starts only once the specification is complete, and lives in stdlib `.zph` modules.

What zelph still lacks is sorted, item by item, into four categories:

1. low-hanging fruit, fixed now;
2. clear bugs, fixed now;
3. a more extensive fix that is clearly manageable, fixed now unless it cascades through many rounds, in which case it is reverted and treated as 4;
4. the rest: not fixed, but specified with example sessions as a prerequisite in [What zelph Still Needs](prerequisites.md), its implementation part of the funded work.

Hours are no criterion for the sorting. The grounds that decide a doubtful case are the performance of the Jacobian computation (the engine's benchmark, guarded by exact counters), mathematics, the Wikidata use case (ACE instead of SPARQL), ACE without a Wikidata import, and the documentation; compatibility with old `.bin` networks is not one of them.

**Why.** zelph is released before the funded front-end work begins, so its users get every general fix at once; the front end is then written against an engine that behaves as documented, and needs no interim workaround that a later engine fix would throw away.

**As built.** The engine work grew the test suite from 1474 to 2014 test cases; every one passes, the Jacobian counters stay within their ceilings, and seven items remain in category 4 ([DEC-CAT4](#dec-cat4); [How the Remaining Items Were Chosen](prerequisites.md#how-the-remaining-items-were-chosen)).

**Milestone.** Done before M0's ACE work.

### DEC-HOSTFIX – every general engine item lands first {#dec-hostfix}

**Question.** Do the engine fixes the front end depends on land as the first work, each with its test, before any front-end code that reads or writes the graph?

**Decision.** Yes, all of them: each item as one unit with its doctest beside its neighbours, the full suite after each, the Jacobian guard green. The items are listed with what each does and how it is tested in [What zelph Already Provides](foundation.md#what-the-front-end-relies-on): among them the keyword host (claims inside a handler, error framing, end-of-input runs, imports inside a handler), quiet runs, the removal path that makes scratch clusters cheap, the refutation items, the Janet bindings, proofs as data, node annotations, opaque predicates and the retraction bindings.

**Why.** Each item was a verified contradiction between code and documentation, or a missing capability that a few lines of C++ provide. Such items need tests, not a design decision, and the rule that the front end never builds an interim workaround for a pending engine change (it refuses the construct by its id instead) needs them in place before the front end exists.

**Not taken.** Fixing only the most urgent items first and the rest at their milestone (the front end's error and end-of-input behaviour would have been pinned twice); no fixes at all, refusing the affected texts by construct id ("If John waits then Mary sleeps. John waits." would never derive, and milestone M4 could not ship).

**Milestone.** Engine; built.

### DEC-MENTION-BUG – moot {#dec-mention-bug}

A rule mentioned inside a statement (`k doubts ((T ~ king) => (sue waits nil))`) used to claim its ground parts; zelph now wraps such a rule as rule text, and the statement claims nothing of it. See [Mentioning a Rule Is Not Asserting It](../logic.md#mentioning-a-rule-is-not-asserting-it).

### DEC-DROP-DELTA – removing derived facts invalidates the incremental run {#dec-drop-delta}

**Question.** zelph's incremental run (`run-delta`) continues from the facts added since the last run. When a scratch cluster in which a run derived facts is dropped, the next incremental run did not derive those consequences again. How is that fixed?

**Decision.** In the engine, and for every kind of removal: a removal of facts a run derived (a dropped cluster with a run inside, `.prune-facts`, `.prune-nodes`, `.remove`) invalidates the record of additions, so the next incremental run is a full pass and says so. A removal of nodes no run derived only drops their entries and keeps the record valid, so a write-only dry run ([DEC-ATOMIC](#dec-atomic)) stays cheap; a rule built inside a dropped cluster leaves no entry behind either. A cluster knows whether a run derived anything in it; the user removal commands cannot tell derived from asserted facts and invalidate the record whenever they remove a fact – a full pass, never a wrong answer.

**Why.** It is a general engine defect with a native reproducer, which the front end would only have met more often (math requests, hypothetical questions and the optional consistent mode all run inference in a scratch cluster).

**Not taken.** Invalidating on every drop (every block's dry run would have made the next run a full pass); an ordering rule in the front end that runs inference before any scratch inference (code that an engine fix makes dead).

**Milestone.** M0; built (host item P-drop-delta).

### DEC-ORDER – DRS-first, with a smoke slice first {#dec-order}

**Question.** Does the language grow in vertical slices – grammar and mapping together per milestone, later constructs refused by a scan of the tokens – or DRS-first: the parser covers all of ACE 6.7 early, and the mapping widens construct by construct?

**Decision.** DRS-first, with a smoke slice first. At M0 the DRS reader, printer, matcher and classifier run on the expected DRSs of APE's regression corpus; at M1 one sentence ("John is a man.") goes through every stage; at M2 the grammar reaches parity with the corpus, and refusal is decided on the parsed DRS by `ace/drs-constructs`, the same function the tests use; from M3 the mapping widens id by id.

**Why.** Every one of the 3733 corpus cases has an expectation at every milestone – accepted, or refused naming a construct – and that needs refusal at DRS level. A scan of tokens cannot classify by DRS evidence ("and" coordinates sentences or noun phrases; "is in a park" is a located copula while a PP after a verb is a modifier), so valid ACE outside the current coverage would get a syntax error or a wrong refusal id. DRS-first also lets the parser and the mapper fail independently, and makes a DRS service nearly free.

**Not taken.** Vertical slices (the earliest end-to-end feedback, but no exact refusals); a hybrid with recognise-only grammar rules producing "unsupported" nodes (most of the grammar anyway, two kinds of rules, and syntax-level ids the corpus gate cannot compute from APE's DRSs).

**Milestone.** M0-M2 (see [DRS First, With One Sentence First](plan.md#drs-first-with-one-sentence-first)).

### DEC-CORPUS – APE's corpus at build time, its small lexicon as an overlay {#dec-corpus}

**Question.** APE, the Attempto Parsing Engine, ships a regression corpus of 3779 cases (LGPL-3, with personal data in some columns). Where does it come from for zelph's tests, and with which lexicon is it run?

**Decision.** CMake's FetchContent fetches APE at a pinned commit (`5f4d5354…`) into the build tree; the SHA-256 of the corpus file is checked; only columns 1-5 are ever read; the corpus tests are in the slow tier, behind the option `ZELPH_ACE_CORPUS`. APE's small lexicon is loaded through the front end's own ulex reader as an overlay that hides the shipped vocabulary and writes no node: `{:replace true :generate false :guess false}`. Nothing from APE is committed to zelph's repository. The gates and their counts are in [Tests and Quality Gates](tests.md#apes-regression-corpus).

**Why.** The corpus is the only automatic guard against accepting what ACE rejects (920 rejection cases besides 2813 accepted ones), and fetching it keeps zelph's licensing intact. It was confirmed by execution: APE at the pinned commit gives the expected DRS for exactly the 2682 cases runnable with its small lexicon and rejects all 920 rejections.

**Not taken.** A test lexicon derived from the corpus trees (a heuristic converter); vendoring the converted corpus under its own LGPL licence (a permanent carve-out from what acrion can relicense); hand-written sentences only (loses the 3733-case net); an own sentence set with DRSs generated by APE and committed (a licence-policy question).

**Milestone.** M0.

### DEC-DRS-ENTRY – a public Janet DRS API {#dec-drs-entry}

**Question.** The DRS (Discourse Representation Structure) is the formal meaning of an ACE text and the pivot between parser and mapper. Where do the DRS reader, printer, matcher and classifier live, and how public are they?

**Decision.** A public Janet API: `ace/ape->drs`, `ace/drs->ape`, `ace/normalise-ape-names`, `ace/drs-equivalent?`, `ace/drs-constructs`, `ace/drs-families` in the module `ace-drs`, and `ace/execute-drs!` in `ace-zelph`, which writes a given DRS into the graph. A `drs` block keyword, which would accept DRSs in APE notation from other ACE tools, is a product decision outside v1. Signatures are in [Modules, API and Data Shapes](api.md#ace-drs).

**Why.** It is the only option that tests the parser and the mapper independently without a second implementation: the mapper runs on the 2813 expected DRSs before the grammar exists, the graph built from a text can be compared with the graph built from APE's DRS of the same text, and any corpus case can be reproduced at the REPL.

**Not taken.** The reader and matcher as C++ test helpers only (a second DRS reader beside the pipeline's); no standalone DRS stage (the corpus only as an accept/reject oracle).

**Milestone.** M0.

### DEC-SWI – APE at development time only {#dec-swi}

**Question.** Should APE, a Prolog program, be available to developers, so that open questions about ACE can be settled by running the reference implementation?

**Decision.** Yes, as a development-time oracle only, also in its guessing mode: SWI-Prolog 10 is installed on the development machine, and APE builds and runs on it from source. It is never a build or test dependency of zelph.

**Why.** It settles questions the corpus does not answer (how APE reads a sentence the corpus lacks) in minutes, without making zelph depend on Prolog. Why APE is not used at run time is explained in [APE: a Development-Time Oracle](architecture.md#ape-a-development-time-oracle).

**Milestone.** None (a tool decision).

## Engine Capabilities the Front End Uses

Four decisions shaped engine features that every block relies on. They were built before the front-end work; [What zelph Already Provides](foundation.md#what-the-front-end-relies-on) documents each with its tests.

### DEC-QUIET – a quiet run returns what it met {#dec-quiet}

**Question.** A run of zelph's inference prints a banner, every deduction and every contradiction, in zelph syntax. An ACE session must show ACE only. What form does a quiet run take, and what happens to the contradictions it meets?

**Decision.** `:quiet` on `zelph/run`, `zelph/run-once` and `zelph/run-delta` (and `.run quiet`, `.run-once quiet`, `.run-delta quiet` at the prompt, one implementation) prints nothing – no banner, no deduction lines, no contradiction line – and returns the struct `{:contradictions C :naf-final N}`:

- C holds one entry per contradiction the run met, per rule and binding, in the order first met: `{:rule :conditions :conclusion :record :bindings}`. `:record` is `:new`, `:existing` or `:none` (the contradiction cannot be recorded). A consequence the engine refused to build carries `:refused <reason>` instead of `:conclusion`. A derived refutation that meets a claimed fact names that fact as `:conclusion`; the record holds it beside the conditions, and `:conditions` leaves it out.
- N holds the defeats of the run's own negation-as-failure conclusions (`{:fact :rule}`, the pairs `(zelph/defeated)` holds afterwards) whenever negation records are kept: in check mode (`.semi-naive check`, the test binary's default), where they are returned instead of raised, and with `.negation-records on`, where they are returned instead of printed. `.run-once` never re-tests records, so its N is always empty.

The run raises only for errors. Any other argument is refused (`the arguments are :quiet and :derived`).

**Why.** The front end receives every contradiction as data and reports each in ACE ([The Report of a Derived Contradiction](block.md#the-report-of-a-derived-contradiction)); nothing disappears and nothing in zelph syntax reaches an ACE session. An earlier design printed the `!` line of a contradiction the engine could not record; it was dropped because that line is zelph syntax inside an ACE session, and because contradictions turned out to be unrecordable in more shapes than first thought (rules whose conditions are all negations, guards or paths; instances that cannot be named; records switched off).

**Not taken.** A scoped `(zelph/quiet thunk)` that silences everything inside, `zelph/out` and warnings included (a general quiet section, but it would also silence the front end's own output).

**Milestone.** Engine; built (host item P-quiet-run). The return value is documented in [Running quietly](../janet.md#running-quietly).

### DEC-BINDINGS – small Janet bindings {#dec-bindings}

**Question.** The front end must look up names without creating nodes, read and set the naming language, take facts apart, probe facts in constant time on a large graph and notice when the graph was replaced. Janet had no binding for any of these.

**Decision.** A set of small, general bindings: `zelph/lookup` (non-creating; reads atoms only; nil for an absent name, the empty name and a variable), `zelph/name … :strict` (no fallback to another language), `zelph/lang` (read, and set with an argument; main thread only), `zelph/parse` (a fact's subject, predicate and objects; nil for an atom, a variable and a string), `zelph/auto-run?`, and for the Wikidata route `zelph/lookup-all` (every node that carries a name in a language), `zelph/exists … :exact` (an exact fact probe, constant time also on a miss), `zelph/degree` (constant time) and `zelph/epoch`. The epoch is a process-wide counter of its own that engine construction (`.new`), every replacing load and every dump import advance, so Janet caches of node ids can tell that the graph was replaced or relabelled; it is not the engine's graph epoch, which must not advance at an import. Every read-only binding resolves a string to an atom only, in the language the main thread published, one language per call.

**Why.** Without them every lookup would need a scratch cluster probe and every fact decomposition an iterative query (166 ms for one 15-monomial answer in a measurement before the bindings existed) – workarounds that a few lines of C++ make unnecessary and that would be thrown away.

**Not taken.** A name setter for Janet (it would expose node merging, which the front end never needs).

**Milestone.** Engine; built (host items P-lookup, P-exact, P-parts, the label index binding).

### DEC-RUN – the front end runs inference as the host would {#dec-run}

**Question.** zelph's automatic run (`.auto-run`) fires only after a keyword handler returns, too late for a question in the same block. When does a block run inference itself, and which run?

**Decision.** `ace/infer!` is `(zelph/run-delta :quiet)`, called when the block asks a question, or when it wrote something and `(zelph/auto-run?)` is true. One engine change goes with it (host item P-autorun-skip): the host's automatic run after a keyword handler is skipped when nothing was written after the handler's own last run – the handler's run count grew, the record of additions is valid and empty, no predicate was removed since and the rule count is unchanged. A `.load` switches auto-run off, even for a file of five nodes, and says so (`Auto-run has been disabled, as every .load disables it: …`); afterwards the front end infers only for a question until the user switches `.auto-run` on again.

**Why.** It is the host's own inference policy applied to ACE: ACE input costs what native input costs, an imported script of ACE blocks gets the module's single postponed run, and a large graph is never swept by an asserting sentence. The skip keeps the host from re-announcing, in zelph syntax, what the front end has just reported in ACE.

**Not taken.** Always running after every block (an imported script with N blocks would run N passes, and the first block after a bulk load would sweep the whole graph); a full quiet run instead of the incremental one (no correctness gain: the incremental run falls back to a full pass whenever the graph is not known to be at a fixpoint).

**Milestone.** Engine, built (P-auto-run, P-autorun-skip); the policy M1.

### DEC-ERRORS – what errors and refusals look like {#dec-errors}

**Question.** What are the classes, the layout and the position marker of the front end's messages, and does the host keep its prefix "Janet error in handler for keyword 'ace':"?

**Decision.** Named classes (`Not ACE`, `Correct ACE, but zelph cannot use it`, `Contradiction`, `Lexicon error`, `Internal error in ace`, `Warning`, `Note`); a stable id `[ace/<id>]` per message, which is also a documentation anchor; the position as `sentence S, line L, column C`, at the token that is the evidence of the problem: the token of the condition that is the construct's evidence; for a complex condition (a modal box, a negation box, a that-clause, an implication) the token of its first simple condition in textual order; for a context id found over the plans, which carry no token, and for a sentence-wide problem, the sentence's first token. So "John can fly." is refused at "fly", column 10, while the message names `modality ('can')`, and "John's age is 30." is marked at "is", column 12, because the number is an argument of the `be` condition and a data argument has no token of its own ([Positions and the Marker](block.md#positions-and-the-marker)); the sentence repeated on one line with the marker `<>` before that token; hint lines. The host names a block by its keyword and first line (`Error in line "ace <first line>": …`) and drops its technical prefix for string errors. The front end raises strings at every boundary. The catalogue of ids is in [The Message Catalogue](block.md#the-message-catalogue).

**Why.** Stable ids and named classes are what tests, documentation and users need; tests assert ids and classes, not wording. A two-character marker survives proportional fonts, tabs, multi-byte characters and one-line logs.

**Not taken.** Keeping the host's technical prefix on every ACE error; a compiler-style caret line under the sentence (misaligned with tabs, proportional fonts and multi-byte characters, and without stable ids).

**Milestone.** M1 (the host's framing is built, host item P-error-envelope).

## M1: The Block, the Lexicon and Naming

The first milestone fixes what a block is, how it reaches the graph, where words and their meanings live and how a session behaves around loads. The block contract itself is specified in [What a Block Does](block.md#what-a-block-does); the lexicon in [The Lexicon](grammar.md#the-lexicon).

### DEC-BLOCK – the first blank line ends a block {#dec-block}

**Question.** Where does an `ace` block end, and what does a blank line inside it mean?

**Decision.** The first blank or whitespace-only line ends a complete block. The block is held open (vetoed) only while a comment, a string or a quoted word is open; a `#` comment is masked, so delimiters inside it do not count. A sentence without an end mark before the blank line gets `[ace/no-terminator]`; a region still open at a forced dispatch gets `[ace/unterminated]` at its opening delimiter. One block is one APE paragraph. A line whose tokens are exactly `.quit` ends piped or typed input even inside a block, after dispatching the block so far; this host behaviour is documented, not changed.

**Why.** The block path then equals APE's text path (a blank line is a paragraph break), the veto stays the simplest possible, and the convention is the one the `sparql` keyword already uses. The corpus does not care: its only paragraph without an end mark before a blank line is an open comment.

**Not taken.** Holding the block open while the last sentence lacks an end mark (zelph would then accept texts APE rejects); two blank lines to end a block (every script ending with an `ace` block would fail with the host's "incomplete" error).

**Milestone.** M1.

### DEC-ATOMIC – a dry run makes each block all-or-nothing {#dec-atomic}

**Question.** How is a block made all-or-nothing, and does a block that makes the knowledge base contradictory stay?

**Decision.** A dry run: the block's write actions are applied in the scratch cluster `ace:dry`, which is then dropped; only if that succeeded are they applied again, in the caller's active cluster. Any engine refusal during the dry run (`known to be true`, `known to be wrong`) becomes a clean rejection of the whole block, with the graph unchanged. A contradiction that the block's own inference derives is reported and the block is kept, which is zelph's documented policy ([Contradiction Detection](../logic.md#contradiction-detection)). From M4 the option `(ace/option :atomic :consistent)` runs the block's inference inside the dry run as well and rejects a block whose run met a new contradiction, at the cost of one more run per block and a full pass at the next inference. From M4 the dry run also takes the engine's verdict on negation levels ([DEC-STRATA](#dec-strata)).

The dry run is sound because of what a dropped cluster restores, all built before the front end: the names of removed nodes go in time proportional to their languages (P-drop-names); a drop keeps the fact stores armed and removes only what the cluster made (P-scratch-drop); a pattern mark that the cluster revoked is restored (P-restore-mark); a refutation made inside the cluster is taken back with its weight and index entry (P-refute (e)); a defeat of a negation-as-failure conclusion that the dry run's quiet run reported goes back to the records, so the real run reports it again; a relation-type declaration the dry run wrote for an older predicate goes with it unless something outside the cluster uses it ([DEC-DECL-RESIDUE](#dec-decl-residue)); and the engine's own vocabulary that the dry run created first stays ([DEC-VOCAB](#dec-vocab)). See [Node Clusters](../rules.md#node-clusters-transactional-workspaces) and, for the block's side, [All or Nothing: the Dry Run](block.md#all-or-nothing-the-dry-run).

**Why.** The engine is the validator: there is no second implementation of its refusal logic that could drift from it. The caller's cluster semantics stay intact, so `.cluster` users and the tests' per-case clusters record a successful block. The cost is that every block is written twice, a fraction of a millisecond for a block of 20 facts in an earlier measurement.

**Not taken.** A validator in Janet that checks everything zelph would refuse (a second implementation that drifts; a case it misses leaves half a block applied); a transaction cluster per block, dropped on error (a successful block would be recorded in that cluster, not in the caller's, block clusters would pile up, and dropping a block later is unsound).

**Milestone.** M1; the option `:consistent` M4.

### DEC-QTIME – one theory per block {#dec-qtime}

**Question.** Within one block, does a question see the whole block or only the sentences before it?

**Decision.** The whole block is one theory: all writes, one quiet inference, then the questions in textual order. Each question runs in a scratch cluster of its own that is dropped afterwards; a question never asserts ([Questions in a Block](block.md#questions-in-a-block)). "Is John a man? John is a man." answers "Yes" (the opening session of this page).

**Why.** APE's DRS of a text carries no order between its conditions, so this is what the text means; it costs one run per block; it fits atomicity; and every question starts at a fixpoint, which keeps its scratch cluster safe for the incremental run. Answers are still printed in sentence order.

**Not taken.** Sentence order, with a run before each question that follows new writes (up to one run per question, and a departure from the DRS semantics).

**Milestone.** M1.

### DEC-ECHO – the derivation echo and justified answers {#dec-echo}

**Question.** What does a user see of the conclusions zelph draws from an asserting block, and how do "why" answers arrive?

**Decision.** Before M9 nothing is shown of derivations. At M9 two options: `(ace/option :echo true)` prints, after each block, every derived sentence in ACE with the sentences it rests on; `(ace/option :justify true)` adds to each yes/no and wh-answer the sentences it rests on. Both default to false. They are built on two engine features: `(zelph/run-delta :quiet :derived)`, which adds `:derived` to the quiet run's result, a tuple in derivation order of `{:fact :rule :premises :absent :bindings}` with the premises looked up and never created (P-derived), and `zelph/explain`, the proof tree as data (P-explain). Layout: [The Derivation Echo](answers.md#the-derivation-echo) and [Why: Justified Answers](answers.md#why-justified-answers).

**Why.** Seeing what was derived teaches the logic, and an answer that quotes the user's own sentences is the explanation a layperson can check. zelph's own echo of deductions is zelph syntax and therefore never an option in an ACE session.

**Not taken.** Explanations only on request through `zelph/explain`, without an echo (the first plan; the echo was added because both engine features exist).

**Milestone.** M9.

### DEC-PROV – provenance in node annotations {#dec-prov}

**Question.** Where is the block, the sentence and the verbatim text behind each node the front end builds stored? A fact that names a rule node turns the rule off ([Mentioning a Rule Is Not Asserting It](../logic.md#mentioning-a-rule-is-not-asserting-it)), so provenance cannot be ordinary facts about rules.

**Decision.** In node annotations, a general engine feature (P-annotate): node → key → list of strings, never seen by queries or rules, never a mention (so they never turn a rule off), erased by every removal (a re-asserted fact with the same id does not inherit a stale quote), carried by the merges that transfer names, filtered by a filtered save, and written by `.save` as an optional section after the name sections. `(zelph/annotate node key value)`, `(zelph/annotations node &opt key)` and `(zelph/unannotate node key &opt value)` are the bindings. The front end writes its provenance there under the key `ace:src` from M1 on; what M9 adds is the use of the records (justified answers, `ace-retract`'s listing), not their place. Documented in [Node annotations](../janet.md#node-annotations).

**Why.** It is the only option that covers rules as well as facts, adds no node (so the oracles that check that re-entering a text creates no node stay at zero), and survives `.save` and `.load`, which "why" answers quoting the user's sentences need. Old `.bin` files load unchanged, older engines ignore the section, and published networks need no regeneration. Writing provenance into a Janet table until M9 would have been a second implementation of the same store, to be thrown away.

**Not taken.** A Janet table (lost on `.new` and on `.save`/`.load`); facts `S ace:states F` in the graph (visible to queries and to meta-rules with a variable predicate, two paraphrases create two sentence nodes, and rules would need a second mechanism).

**Milestone.** M1 (writing), M9 (use); the engine feature is built.

### DEC-MEANING – the mapper writes domain structure {#dec-meaning}

**Question.** Does the mapper write domain facts and rules directly (`John likes Mary`), or ACE vocabulary atoms that shipped zelph rules map onto domain predicates?

**Decision.** The mapper writes domain structure; every content word is resolved to its node by `ace/concept!` when the block executes.

**Why.** One representation and the fastest answers. ACE facts are the facts a native user writes, so native rules and queries meet them directly.

**Not taken.** A vocabulary layer mapped by rules. It would have allowed re-binding a word for texts already entered and expressing a synonym as one rule, but every atom would exist twice, every answer would need one more rule step, and negation as failure over the vocabulary layer would need a second negation level. Since the mapper resolves every word through one function, a later change would touch only its callers. The full DRS in the graph with anaphora as rules is a rejected route ([Routes Not Taken](routes.md#routes-not-taken)).

**Milestone.** M1.

### DEC-LEX-STORE – shipped vocabulary as Janet data, user entries as graph facts {#dec-lex-store}

**Question.** Where does the lexicon live?

**Decision.** The shipped vocabulary, `ace-core` – prepositions and particles, the irregular forms, about 300 person nouns with gender, the mandatory entries and the common ACE example names (John, Mary, Bill, Sue, …) – is Janet data and is never written to the graph. User entries (`ace-lexicon` blocks, `ace/declare!`, `ace/load-ulex!`) are graph facts on word nodes in the naming language `ace-lex`, so they persist with `.save` and rules can derive them. Caches of graph-derived data are keyed by `zelph/epoch`. `ace-core.zph` is a data module without imports; `ace-lexicon.zph` imports it, so every importer of the lexicon has the shipped vocabulary, and an overlay loaded with `:replace` hides it. The vocabulary of `ace-math` is shipped data of `ace-math.zph` in the same way.

**Why.** A `.load` after `.import ace` replaces the graph and would take a written `ace-core` with it while the Janet state survives; and `.import ace` runs again after a load and must not depend on what the graph held ([DEC-RELOAD](#dec-reload)). Keeping the base vocabulary out of the graph also keeps it out of every query, and keeps the graph empty after `.import ace`, which a Wikidata dump's cache requires.

**Not taken.** Everything as graph facts in `ace-lex` (the first plan: base-vocabulary facts in every unrestricted query, and lost at a load); facts in `ace` itself; Janet tables only (lost on `.new`, not saved by `.save`, invisible to rules and queries).

**Milestone.** M1.

### DEC-LEX-SYNTAX – ulex blocks {#dec-lex-syntax}

**Question.** How do users declare words?

**Decision.** An `ace-lexicon` block keyword that reads the ulex format of the public ACE lexicon specification, and `ace/load-ulex!` with the same reader. An entry's second argument, the logical symbol, may be a Wikidata id: `pn_sg('Berlin', 'Q821244', neutr)`, `noun_sg(man, 'Q8441', masc)`, `tv_finsg(follows, 'P155')`. That is how a user binds a word to a Wikidata item: role-aware by the entry's word class, several forms may share one symbol, no node is merged ([DEC-WIKIDATA](#dec-wikidata)).

**Why.** The ulex reader is needed anyway, for user lexicons and for the corpus ([DEC-CORPUS](#dec-corpus)); AceWiki and APE lexicons load verbatim; `ace-core.zph` is written in the same format.

**Not taken.** A compact shorthand of zelph's own beside ulex (a second syntax to document and test). Declaring words with ACE sentences about strings is a rejected route.

**Milestone.** M1.

### DEC-LEX-GEN – inflected forms are generated {#dec-lex-gen}

**Question.** Does a lemma entry generate the forms it does not declare (past participles, and with them the passive), or do only declared forms exist, as in APE?

**Decision.** A lemma entry generates every form it does not declare; a declared form overrides the generated one. The same tables, run backwards, recover the lemmas of guessed words ([DEC-UNKNOWN](#dec-unknown)). Valency stays declared and strict.

**Why.** One entry per lemma; the rules were measured at 90-99.8 % accuracy on a large English lexicon, and the irregular forms are a list of zelph's own. The corpus session loads APE's lexicon with `:generate false`, so it is APE-strict and no corpus case diverges because of this decision.

**Not taken.** APE-strict generation (every form declared, many beginner errors); generating everything except participles (one more declaration per verb for its passive).

**Milestone.** M1.

### DEC-NAMES – phrase predicates for prepositions {#dec-names}

**Question.** What happens to ACE words whose node name collides with a core node of zelph (`in`, `nil`, `cons`, `conjunction`, `negation`, …) or with a name the stdlib uses internally?

**Decision.** The located copula and the prepositional modifiers of events use `is <prep>` phrases for every preposition (`John "is in" Park-…`), so core `in` never becomes a predicate. Core spellings (`~ => ! != -> in nil cons conjunction negation`) are the same node in every language: they are a closed class, never guessed, never resolved as content (`[ace/reserved-word]`, with the hint `n:Q…` on a Wikidata graph). There is no warning about stdlib names, because the front end's content lives in its own language ([DEC-CONTENT-NS](#dec-content-ns)) and the stdlib's internal names in `zelph`, and a collision exists only within one language. Moving the stdlib's internal relations into a namespace is a separate stdlib question.

**Why.** A core name written as content corrupts meaning: `X ~ negation` is the tag of negation as failure, and a fact with predicate `in` turns its object into a collection. `"is in"` is an ordinary node and follows the convention of native names such as `"is capital of"`.

**Not taken.** Documentation only (silent interactions); a list of stdlib names with a warning (dropped once content moved to its own language).

**Milestone.** M1 (the located copula M3).

### DEC-CONTENT-LANG – replaced {#dec-content-lang}

Which naming language content words use was first decided as "the current `.lang`"; [DEC-CONTENT-NS](#dec-content-ns) replaced it with the fixed language `ace`.

### DEC-MATH-IMPORT – arithmetic needs `.import ace-math` {#dec-math-import}

**Question.** How does the front end reach zelph's math stack for arithmetic sentences?

**Decision.** By an explicit `.import ace-math`, which imports `ace` and then, under `.lang zelph`, `math`, `evaluation` and `primes`, restores the user's language and registers its vocabulary. Without it, arithmetic parses and is refused with `[ace/needs-math]`, naming the import:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John's age is 30.

    Error in line "ace John's age is 30.": Correct ACE, but zelph cannot use it [ace/needs-math]: sentence 1, line 1, column 12: a number, construct data/integer.
      John's age <>is 30.
      Numbers and arithmetic need the math stack: .import ace-math
    ```

A replacing load takes the math stack with the graph while the Janet state survives, so the front end keys its math hooks by `zelph/epoch`: after a load, arithmetic is `[ace/needs-math]` again until `.import ace-math` runs again, which then imports `math` again. The earlier refusal `[ace/math-lang]` (math imported under another language) is withdrawn, because the case no longer exists ([DEC-MATH-LANG](#dec-math-lang)).

**Why.** Importing math changes the whole session: `.import math` alone leaves 5661 nodes and 418 rules (6135 and 456 measured with a prototype of the evaluation module, [DEC-MATH-MODULE](#dec-math-module)), and an automatic run over them follows every later input line. That should be the user's visible act, not the side effect of one sentence. `.import ace` and the tests of everything else stay fast.

**Not taken.** An eager import by `.import ace` (every session pays for math); a lazy import on the first arithmetic sentence, which became possible once imports work inside a keyword handler, and is deliberately not taken for the same reason.

**Milestone.** The module layout M1; arithmetic M5 ([Numbers and Formulas](math.md)).

### DEC-DATA – numbers, strings and reals without `ace-math` {#dec-data}

**Question.** What happens to numbers, strings and reals in sentences that are not arithmetic ("John's age is 30.", `A text is "abc".`) when `ace-math` is not imported?

**Decision.** Integers are refused by their id (`:data/integer`, `[ace/needs-math]`) until `ace-math` is imported, because zelph's numbers need an arithmetic substrate. Strings are character lists, built in `zelph`, never the node of that name (the string "John" must not be the person John); the empty string is the internal node `ace:empty-string`. Reals have no route: zelph has integers only. An arithmetic term without an integer value used as data ("John likes 7 / 2.", `:expr/as-data`) has no route in v1: stored, it would be a request to compute it, not data.

**Why.** `.import ace` stays free of rules, and numbers and arithmetic arrive through one import and are node-identical with the math stack's numbers.

**Not taken.** Importing a number substrate with `.import ace` (the smallest one brings more than 1300 nodes and over 70 rules, and a run cost on every later statement, for every user); numbers as named nodes (not the math stack's numbers, so facts stored before `ace-math` would never meet arithmetic); a lazy substrate import.

**Milestone.** M1 (the refusal), M3 (strings), M5 (integers).

### DEC-RELOAD – what a replacing load takes along {#dec-reload}

**Question.** A network-file load (`.load`, `zelph/load`, `zelph_load`, `.load-partial`, a manifest, a dump's cache) replaces the graph, while the Janet state of the front end survives. What does a load take along, and how does the front end stay correct across it?

**Decision.** A replacing load takes along every cluster (an active scratch cluster too), the input focus, the delta record of the incremental run, the negation records and `(zelph/defeated)`, the variables of the running Janet block, the C API's variables, the display registrations of modules (the digit alphabet among them), compiled neural-net handles, the import registry (except the claims of imports still running) and the session's node annotations (the file's own come in). It keeps the Janet environment, the registered keywords, `.lang`, the settings and the `.negation-records` switch, and switches auto-run off. Janet values that hold nodes are not repaired, so every cache of node ids in the front end (the word cache, the lexicon caches, the math hooks, the memo of declared opaque predicates, the rule registry) is keyed by `(zelph/epoch)`. Because the import registry is cleared, `.import ace` after a load runs the module again in the same session: `zelph/register-keyword` replaces the handler, and every module of the front end must survive a second run in the same Janet environment (pinned by a test). A dump's `.bin` cache is read only into an empty graph; `.import ace` keeps the graph empty, a statement or a query that names a node does not. A same-language `.name` onto a name another node holds merges the two nodes, so `.name` is no binding route for the front end's words; the lexicon is.

**Why.** A load must never leave the front end with node ids that now mean something else, and the user must be able to import `ace` again after a load without restarting.

**Milestone.** M1.

### DEC-PLAYGROUND – ACE in the browser playground {#dec-playground}

**Question.** The WebAssembly build embeds every stdlib file, so `ace` loads in the [Playground](../playground.md) anyway. Does a test run it there, and when does a demo show it?

**Decision.** One ACE block in `src/wasm/node_smoke_test.mjs` from M1; a hand-written demo group in `src/wasm/web/demos.js` from M4, which the existing demo smoke test presses button by button: declared words, an every-rule, facts, a yes/no question, a who-question, a "No." from a negation.

**Why.** One smoke case pins that the front end works under WebAssembly; a demo is worth showing once rules exist.

**Not taken.** No ACE in the playground in v1 (a WebAssembly-only failure would reach users first); a demo group from M3 that changes every milestone; buttons generated from the construct table's examples (a generator and a sync test for JavaScript).

**Milestone.** M1, M4.

## M2 and M3: Documents, Facts and Questions

These decisions fix the shapes that ACE facts take in the graph. They matter beyond the front end: a native rule or query, or a Wikidata fact, meets an ACE fact only if both have the same shape. The full mapping table is in [Every Construct and Its Native Form](semantics.md#every-construct-and-its-native-form).

### DEC-LOAD – loading a whole ACE document {#dec-load}

**Question.** `.import` takes only `.zph` and `.janet` files, and a block ends at the first blank line. How does a user load an ACE document of several paragraphs, such as a policy text with a comment header?

**Decision.** `(ace/load! path)` and `(ace/load-text! text)` in `ace-zelph`, with APE's text semantics: paragraphs split at blank or whitespace-only lines outside comments, anaphora and variables reset per paragraph, sentence ids continuing. The whole document is analysed first and rejected as a whole on any error, with the file's line and column; then one dry run over all paragraphs, the real run paragraph by paragraph (each paragraph one block, for provenance), one inference, and the questions in textual order. The WebAssembly build has no files and gets `ace/load-text!` only.

**Why.** APE and AceWiki documents load verbatim and as one theory, and the paragraph split is the one `ace/analyse` needs anyway, so there is no second implementation.

**Not taken.** Documentation only, a `.zph` script with an `ace` line before each paragraph (a paragraph whose `ace` line is forgotten silently becomes a native fact, and each paragraph is atomic on its own where APE rejects the whole text); an `ace-file` keyword (a third keyword for what a `%` line does); an engine hook that hands files with a registered extension to a Janet loader (a large C++ change, worth making when a second keyword language wants files). Loading paragraph by paragraph and stopping at the first failure was never offered: it leaves half a document applied.

**Milestone.** M2.

### DEC-SHAPES – one-place predicates {#dec-shapes}

**Question.** Which fact shapes express intransitive verbs ("John waits.") and adjectives ("John is rich.", "a rich man")?

**Decision.** `John waits nil` and `John is rich`: an intransitive verb has core `nil` as its object, an adjective is the object of the predicate `is`.

**Why.** It is the only combination without a semantic collision, and the adjective form is what a user types natively (it reads like `stdlib/examples/english.zph`). `~` stays reserved for class membership, which on Wikidata is P31.

**Not taken.** zelph's unary idiom, self-facts `:waits John` and `:rich John` ("No man sees himself." and the intransitive "A man sees." would become the same fact); adjectives as classes, `John ~ rich` (adjectives would pollute the class hierarchy, and on Wikidata the instance facts).

**Milestone.** M3.

### DEC-OF – relational nouns {#dec-of}

**Question.** How are of-phrases and possessives mapped ("the father of John", "John's age"; 577 of the 2813 accepted corpus DRSs hold one)? A plain `of` predicate is ruled out, because after `.import math` an `of` fact prints as a function application.

**Decision.** A relational noun: "Bill is the father of John." gives `John father Bill` plus the class atom `Bill <instance> father`, with no class atom on a data value. When the noun's relation resolves to a Wikidata property ([DEC-WIKIDATA](#dec-wikidata)), the class atom is left out everywhere – in statements, questions, conditions and helper bodies: Wikidata states `Q… P50 Q42` (author), not that Q42 is an instance of "author", so the class atom would make "Who is the author of …?" answer nothing. Without Wikidata nothing changes.

**Why.** It is the triple a native or Wikidata user writes, so ACE facts and existing data meet; it also opens the binding of nouns to Wikidata properties.

**Not taken.** A generic predicate `ace:of` (which relation was meant is lost); the functional term `(N of Y)` for every definite "the N of Y" (departs from the DRS, assumes uniqueness, and becomes a math term once math is loaded).

**Milestone.** M3.

### DEC-INDIVIDUALS – content-addressed individuals {#dec-individuals}

**Question.** How are the individuals of "There is a man." or "John owns a dog." named, and what does entering the same text again do?

**Decision.** Content-addressed names: `<Head>-<16 hex digits of FNV-1a-64>` in language `ace`, the hash taken over the canonical text of the referent's group (for example `Man-…`, `Park-…`). The same canonical group is the same individual everywhere, also inside one block. The negation helpers `ace:naf-<h>` use 16 hex digits as well. The departure from APE – "There is a man." entered twice is one man – is documented. Every individual the front end creates also carries the internal mark `I ace:individual nil` ([DEC-PLURALS](#dec-plurals)).

**Why.** On a large persistent graph that ACE documents are applied to repeatedly, counter names would add a fresh set of individuals on every run; content addresses make re-entry idempotent like every zelph statement, keep plans pure (no graph read is needed to name a node) and need no probes. 16 hex digits rather than 8, because 8 would merge two different descriptions silently at the volumes this is for (about 1.2 % at 10,000 individuals per head noun); with 16 digits two groups collide with a probability of about 2.7 × 10⁻⁸ at 1,000,000 individuals.

**Not taken.** Counter names `Man-1`, `Man-2` (APE's semantics and readable, but re-entering a block doubles its individuals); a restricted chase that asserts a group only when the graph does not yet satisfy it (zelph queries do not enforce distinctness, and results would depend on the graph's state). Anonymous nodes are rejected: they print as `??` and do not re-enter.

**Milestone.** M3.

### DEC-EVENTS – events that carry modifiers {#dec-events}

**Question.** How are modified events represented ("John waits in a park.", "John waits fast.")?

**Decision.** The plain fact always (`John waits nil`); and only when the event carries modifiers, an occurrence individual `ace:event-<h>` (in `zelph`) with `E ace:occurrence (John waits nil)`, one `E "is <prep>" Y` per prepositional modifier and `E ace:manner Adv` per adverb. Modifier predicates are the same `is <prep>` phrases as the located copula ([DEC-NAMES](#dec-names)). A condition with a modifier matches the occurrence by deep unification over the nested statement: "If a man waits in a park then the man is a visitor." gives the body `(X ~ man), (X waits nil), (Y ace:occurrence (X waits nil)), (Y "is in" Z), (Z ~ park)` ([Events and Modifiers](semantics.md#events-and-modifiers)).

**Why.** The shapes of M3 stay as they are, ACE facts stay visible to native and Wikidata triples, and where/when/how questions run on the event.

**Not taken.** An event node for every verb (two or three conditions per verb, and ACE facts invisible to native data). Never offered: dropping modifiers (loses information) and writing modifiers on the nested fact (zelph identifies a statement by its content, so "John waits in a park. John waits fast in a bank." would make "Does John wait fast in a park?" true).

**Milestone.** M3 (the verb shape), M6 (the occurrences).

### DEC-PATHS – an instance and a subclass predicate, pinned per graph {#dec-paths}

**Question.** ACE 6.7 has no construct for the transitive closure of a class hierarchy, and Wikidata states classes with P31 (instance of) and P279 (subclass of), ordinary nodes, not zelph's core `~`. How do class atoms in statements, questions and rule conditions reach that hierarchy?

**Decision.** The front end has two vocabulary entries, the **instance predicate** and the **subclass predicate**. They are pinned per graph, at the first class atom the front end writes, by one fact in internal vocabulary: `ace:profile ace:uses ace:wikidata` (instance P31, subclass P279, the nodes of those names in language `wikidata`) or `ace:profile ace:uses ace:core` (instance core `~`, no subclass predicate). No node is renamed. A user binding wins over the profile: a node the user named `ace:instance` or `ace:subclass` in language `ace`, looked up at the start of each block (a second `.name` merges two predicate nodes, so a binding is moved with `.delname ace:subclass ace` first). When a later block finds a graph pinned to `~` holding P31 and P279 (Wikidata imported afterwards), it warns `[ace/w-profile]` once and keeps the pin.

Statements and rule heads write `T <instance> C` for a term T and a class C. Class atoms in questions and rule conditions become `T <instance> V, ((V <subclass> C) closure zero-or-more)` when a subclass predicate exists; V is a variable of the pattern's own and takes its letter in the order of [DEC-VAR-LETTERS](#dec-var-letters), while the core profile's `T ~ C` has no such variable and takes none. One function builds every class pattern ([Class Patterns and the Graph Profile](semantics.md#class-patterns-and-the-graph-profile)). Today's zelph answers that pattern; on a graph whose instance facts use P31, a `~` class atom finds nothing. Here "Who is a person?" is asked of three facts in Wikidata's shape, first as a `~` class atom, then as the class pattern:

```
zelph> "Douglas Adams" P31 writer
zelph> writer P279 author
zelph> author P279 person
zelph> X ~ person
zelph> X P31 Y, ((Y P279 person) closure zero-or-more)
Answer: "Douglas Adams" P31 writer, writer P279∗ person
```

Wikidata models some memberships with other properties than P31, among them occupation (P106) and sex (P21). On the test fixture, which has the shape of Wikidata, continuing the tutorial session of the [Overview](index.md#after-a-wikidata-import):

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .load /tmp/ace-fixture/ace-fixture.json
    …
    zelph-> .import ace
    zelph-> ace There is a man who admires Douglas Adams.
    Is Douglas Adams a human?
    What is an occupation of Douglas Adams?

    Note [ace/word-senses]: words read as Wikidata items (the lowest Q-number among the items with this label):
      "man" is Q8441 (7 other items carry the label; https://www.wikidata.org/wiki/Q8441)
      "Douglas Adams" is Q42 (3 other items carry the label; https://www.wikidata.org/wiki/Q42)
      "occupation" (relation) is P106 (https://www.wikidata.org/wiki/Property:P106)
      To choose another item, write n:Q… for a noun or p:Q… for a name, or declare the word, e.g. noun_sg(man, 'Q…', masc).
      (ace/senses "man") lists the candidates.
    Answer: Yes. Douglas Adams is a human.
    Answer: p:Q36180 is an occupation of Douglas Adams.
    zelph-> ace Is Douglas Adams a writer?

    Answer: It is not provable that Douglas Adams is a writer.
    ```

"Is Douglas Adams a writer?" is not provable, because Wikidata states the occupation `Q42 P106 Q36180`, not an instance fact; the user page says how to ask instead, as the occupation question of the first block does ([What Wikidata Models Differently](words.md#what-wikidata-models-differently)). The last block has no note: "Douglas Adams" was announced earlier in the session, and "writer" has one carrier. For the same reason "Is a dog a mammal?" is not provable through P279 on today's Wikidata (dog reaches a "mammal" item that is not the taxon "mammal" resolves to), while "Is a dog an animal?" and "Is a horse a mammal?" hold; examples and tutorials use relations that hold.

On Wikidata the costs matter. A rule with a closure condition used to be matched in full on every run; since P-closure-delta it takes a full pass only when a fact of the predicate it walks arrived. A question's closure keeps the predicate index since P-pidx-local, and a block that introduces a new predicate clears only `~`'s index and its own. A rule whose class has a large extent derives one fact per instance; the front end warns before asserting it (`[ace/w-large-rule]`, read from `zelph/degree`, above 100,000 instances).

**Why.** It reaches the Wikidata class hierarchy without changing what any statement writes, costs nothing on a graph without a subclass predicate, and keeps the behaviour the same with and without Wikidata. Naming core `~` in language `ace` was rejected: every `~` fact would then display as `ace:instance` under `.lang ace`. Renaming P31 to `~` was rejected too: it would re-create every P31 fact (seven million on the smallest published network) and change how `~` prints.

**Not taken.** No route (ACE over Wikidata would miss every inherited class, silently); writing "Every N is a M." between two plain nouns as a subclass fact instead of a rule (native `X ~ M` queries would miss instances, and a sentence's meaning would depend on a configuration).

**Milestone.** M1 (the pin, the class-pattern function, yes/no questions), M3 (wh-questions, relational nouns), M4 (rule conditions).

### DEC-GENERATOR – a sentence generator for property tests {#dec-generator}

**Question.** Mapping defects hide in combinations nobody wrote, such as a relative clause inside a negated conditional with a proper name. Should the property tests get a generator of valid sentences beyond the corpus and the hand-written examples?

**Decision.** A substitution generator in the test glue, never shipped, from M3: each construct row's examples are templates; content words are swapped for lexicon words of the same reading, noun phrases for noun phrases of other rows' examples, to a bounded depth, from a printed seed. The oracles need no expected output: the status the classifier computes on the generated DRS, a well-formed DRS, the verbaliser reading back its own output, the same graph on two paths, bounded work. The test is `ace fuzz: generated sentences behave like the examples they come from` ([Properties](tests.md#properties)).

**Why.** It reuses the construct table that refusal needs anyway, and every new row widens it.

**Not taken.** No generator; a grammar-driven generator from the rule builders (a second interpretation of most of the grammar inside the shipped module).

**Milestone.** M3.

## M4: Rules and Negation

ACE has two negations. "not" is classical negation: the statement is false. "not provably" is negation as failure (NAF): the statement cannot be derived from what is known. zelph has both – a refutation `¬(F)` and a negated rule condition `¬(…)` – and orders rules with negated conditions into negation levels ([Negation as Failure](../logic.md#negation-as-failure), [Stratified Evaluation](../logic.md#stratified-evaluation)). These decisions fix how far each negation reaches and where the front end refuses rather than risk a wrong answer.

### DEC-NEG – negation inside rule conditions {#dec-neg}

**Question.** How far do classical negation and negation as failure reach inside the condition of a rule?

**Decision.**

- A one-atom classical negation in a condition ("If John does not wait then …") becomes the condition `((F) ~ refuted)`, which matches refuted facts only (`refuted` resolved in `zelph`).
- A complex classical negation in a condition is refused (`:neg/complex-in-body`), with the hint to write "does not provably" if that is what is meant.
- A one-atom NAF in a condition becomes `¬(F)`; a complex NAF ("If a man does not provably own a car then …"), and a NAF over a class item, becomes a Lloyd-Topor helper: a rule `ace:naf-<h>` in `zelph` whose body holds no NAF and derives a helper atom, and the condition `¬` of that atom. Helper atoms have the shape `F1 H nil`, `F1 H F2` or `F1 H <F2 … Fk>`, never `H H …`. Helpers are visible in `.list-rules` and deduplicate on re-entry (their names are content hashes; [Helper Rules for "Not Provably"](semantics.md#helper-rules-for-not-provably)).
- A top-level "It is not provable that …" has no route (`:naf/top-level`).

On a Wikidata graph a rule with a negated condition used to be matched in full on every run; since P-naf-delta an incremental run seeds it by its positive conditions, except when a predicate it negates lost a fact since the last run, when it negates under a predicate variable, when its component negates what it derives, or when it is new. Once any rule in force has a negated condition, a block that adds a rule makes the next run a full pass over every rule; so after the front end's first NAF rule, every block that writes a rule pays one ([What zelph Still Needs](prerequisites.md#z3-p1-after-the-first-negated-condition-every-new-rule-costs-a-full-pass), item Z3-P1).

**Why.** Sound rules, and negation as failure beyond one atom. zelph's `¬(…)` takes one pattern (`¬(A, B)` is refused), so a box of several conditions needs a helper; a helper with a positive body gives the right answer in one run whatever the order of the rules.

**Not taken.** Reading a complex classical negation in a condition as NAF, with a warning (it would silently turn classical logic into closed-world logic for those rules); one-atom negations only (no helpers, but no NAF beyond one atom); NAF at the top level as a denial (integrity constraints in ACE, but a ground denial is a ground rule pattern with its own engine questions).

**Milestone.** M4.

### DEC-STRATA – shapes that need more than the engine can order {#dec-strata}

**Question.** zelph orders rules with negated conditions into negation levels. Three ACE shapes need an order the engine cannot always give: a universal inside a condition ("If every dog sleeps then a cat runs.", the shape of corpus case 782), a NAF box that holds a NAF or an implication, and a chain of NAF rules across sentences. Accepted blindly, they give derivations that depend on the order of the rules and that a later fact never retracts. How does the front end handle them?

**Decision.** All three are refused by id: `:rule/in-body`, `:naf/nested-in-body` and `:naf/unstratified` ([Shapes zelph Cannot Yet Evaluate Soundly](constructs.md#shapes-zelph-cannot-yet-evaluate-soundly)).

`:naf/unstratified` is decided twice:

1. **Before the dry run**, over the transitive may-derive graph of the block's rules and the ACE rules of earlier blocks: a NAF pattern of a rule (helper atoms included) that a rule with a NAF condition can derive, directly or through positive rules. A rule head `T <instance> C'` may derive a class pattern over C when C' is C or the graph's upward subclass closure of C' contains C – a bounded walk (25 to 113 classes in a measurement on Wikidata); a later subclass fact can change that verdict, which the documentation says. `:rule/endless-generation` (a self-feeding rule with a fresh witness) is decided the same way.
2. **In the dry run**, from the engine's own verdict: after the block's rules are written in `ace:dry`, when the block writes a rule and the theory holds a NAF rule, the front end calls `(zelph/strata)`; a component that holds a NAF rule the front end wrote and is `:stratifiable false` is refused when it also holds a rule the front end did not write, or when a rule over predicates (a row with `:transparent true`, host item P-strata-transparent) is in force. These are the cases the front end's own graph cannot see: native rules beside the ACE rules, or a transitive meta-rule that makes the engine compare predicates only. Rules a generator wrote and the ACE rules of a loaded session are not in the front end's registry, so this check counts them as foreign: sound, and stricter than needed.

**Why.** Since zelph keys negation levels by pattern – a top-level pattern by its predicate and constant object – the three shapes get separate levels and the right model in every order in the core profile. They do not in the Wikidata form, where a class pattern `T P31 V, V P279∗ C` reads P31 with any object, so every class-headed NAF rule whose helper reads a class shares one component with every class rule (measured: a NAF chain right in 0 of 96 block runs); nor beside a rule over predicates. And a class hierarchy that feeds a NAF conclusion back into what a NAF rule tests is wrong in every run, deterministically. In native form, the three sentences "If a man is not provably married then the man is a bachelor. If a man is not provably a bachelor then the man is a husband. Every husband is a man." with "John is a man." are these rules (`ace:naf-1` stands for the content-addressed helper name). Typed under `.lang ace`, with automatic inference switched off and then one quiet run, as the front end runs a block, they make zelph conclude that John is a husband:

```
zelph> .lang ace
ace> .auto-run
Auto-run is now disabled.
ace-> ((X ~ man), ¬(X is married)) => (X ~ bachelor)
ace-> ((X ~ man), (X ~ bachelor)) => (X ace:naf-1 nil)
ace-> ((X ~ man), ¬(X ace:naf-1 nil)) => (X ~ husband)
ace-> (*{(X ~ husband)} ~ conjunction) => (X ~ man)
ace-> John ~ man
ace-> .run quiet
ace-> X ~ bachelor
Answer: John ~ bachelor
ace-> X ~ husband
Answer: John ~ husband
ace-> .strata
1 negation level:
Level 1:
  (¬(X ace:naf-1 nil), (X ~ man)) => (X ~ husband)
  (¬(X is married), (X ~ man)) => (X ~ bachelor)
Not stratifiable, on level 1 -- these rules negate what they derive:
  ((X ~ man), (X ~ bachelor)) => (X ace:naf-1 nil)
  (*{(X ~ husband)} ~ conjunction) => (X ~ man)
  (¬(X ace:naf-1 nil), (X ~ man)) => (X ~ husband)
  (¬(X is married), (X ~ man)) => (X ~ bachelor)
```

John is a bachelor, so "not provably a bachelor" is false, and he must not be a husband. All four rules share one component that negates what it derives. zelph evaluates such a component by alternation: a positive saturation, then one pass over its NAF rules, repeated while something is derived. Within the pass a NAF rule does not see the positive consequences of the rules before it, so the helper fact that would block "husband" arrives too late, in every order (John was a husband in 2,880 of 2,880 measured block runs; check mode, which re-tests the negations, raised in each of its 1,440 runs). Without the third sentence the engine orders the same two rules on two levels and the answer is right:

```
zelph> .lang ace
ace> .auto-run
Auto-run is now disabled.
ace-> ((X ~ man), ¬(X is married)) => (X ~ bachelor)
ace-> ((X ~ man), (X ~ bachelor)) => (X ace:naf-1 nil)
ace-> ((X ~ man), ¬(X ace:naf-1 nil)) => (X ~ husband)
ace-> John ~ man
ace-> .run quiet
ace-> X ~ bachelor
Answer: John ~ bachelor
ace-> X ~ husband
ace-> .strata
2 negation levels:
Level 1:
  (¬(X is married), (X ~ man)) => (X ~ bachelor)
Level 2:
  (¬(X ace:naf-1 nil), (X ~ man)) => (X ~ husband)
```

v1 still refuses that chain, because the may-derive check refuses every chain in every profile: in the Wikidata form the same two rules share one component and are wrong in some orders.

**Planned, not v1.** A narrowing that accepts the orderable shapes: (α) a helper leaves out each context pattern that the head of a NAF rule may derive (only the helper's extent grows; the main rule keeps those patterns); (β) a non-stratifiable component that holds no foreign rule, while no rule over predicates is in force, is accepted when no NAF rule of the component may reach a NAF pattern of a NAF rule of the component, itself included; (γ) a generator's inner rule is judged as if it shared the component of every NAF rule it touches. It is sound because in such a component every negation is final when it is tested. Evidence: every theory the engine calls stratifiable was right in every run (0 wrong of 3,912 runs), every non-stratifiable shape without such a feed was right (0 wrong of 3,614), every shape with a feed was wrong in some or all orders. Under the narrowing the core profile would accept the chains, a universal in a condition, NAF in NAF and hierarchy feedback into a helper's context; in the Wikidata form class-conclusion chains stay refused. Lifting `:rule/in-body` and `:naf/nested-in-body` would also move 14 corpus cases into M4's gate (782, 783, 1011, 1012, 1212, 1231, 1966, 2990, 3317, 3360, 3363, 3734, 1418, 3022) and needs a mapping for a universal inside a top-level or then-box negation (corpus 398, 1914). Revision of NAF conclusions, data-dependent levels and a path variant are not built either; the planned engine work is described under [Where the logic goes next](../about.md#where-the-logic-goes-next).

**Not taken.** Full multi-level negation in the engine before v1. Part of it exists since the front end's design began: negation levels keyed by pattern, `(zelph/strata)` and negation records were built; revision of conclusions was not.

**Milestone.** M4 (the refusals).

### DEC-NAF-RECORDS – a defeated NAF conclusion is reported, never withdrawn {#dec-naf-records}

**Question.** A conclusion drawn under "not provably" is final in zelph: a later fact that defeats it does not retract it. What does the front end do when that happens?

**Decision.** It reports it and withdraws nothing. zelph's `.negation-records` (off by default) records every fact derived under a negated condition with its rule and bindings and re-tests the records after each run. The front end switches the records on with `(zelph/negation-records true)` when it writes its first rule with a NAF condition (from M4) and never off; from then on the session's native NAF rules are recorded too. The quiet run returns the defeats as `:naf-final` ([DEC-QUIET](#dec-quiet)), and the front end reports them as one `[ace/w-naf-final]` per block. Natively, the same finding looks like this:

```
zelph> .lang ace
ace> .negation-records on
Negation records: on
ace> ((X ~ man), ¬(X is married)) => (X ~ bachelor)
ace> John ~ man
(John ~ bachelor) ⇐ {(¬(John is married)) (John ~ man)}
ace> John is married
Warning: 1 fact(s) no longer justified -- a negated premise they were derived under now holds, and nothing else derives them; they stay:
  John ~ bachelor  (by (¬(X is married), (X ~ man)) => (X ~ bachelor))
ace> X ~ bachelor
Answer: John ~ bachelor
```

**Why.** The warning must work at the REPL default, not only in the test binary's check mode, and a session without a NAF rule pays nothing. The cost of a record is about 250 bytes with one bound variable and about 60 more per further variable, plus about one second per million records in every run that re-tests them; with auto-run on, such a run follows every statement that adds a fact to a negated predicate. What the quiet run cannot close is documented: a host run between `ace` blocks (the automatic run after a native line, `.run`, a `?` line) prints the warning in zelph syntax and consumes those records, so the front end's next run does not see them; and a NAF conclusion in a loaded graph has no record, so its later defeat is never reported. A load discards the records and keeps the switch; `.new` resets it to off.

**Not taken.** Withdrawing the conclusion: that is truth maintenance, which zelph does not have (see [Where the logic goes next](../about.md#where-the-logic-goes-next)).

**Milestone.** M4.

### DEC-REFUTED – a derived refutation of a claimed fact {#dec-refuted}

**Question.** What happens when a rule derives `(F) ~ refuted` for a fact F that is already claimed, or when a user writes such a marking by hand?

**Decision.** Three rules, which hold on every route – typed, nested in a statement, in a comma list, from Janet, through the C API, derived whatever spelling or binding builds the marking:

- a derived `(F) ~ refuted` that meets a claimed F is a reported contradiction; its record is the condition set plus F, or the single plain condition plus F; nothing is written and F keeps answering;
- a top-level `(F) ~ refuted` line is read as a spelling of `¬(F)`: refused with `known to be true` for a claimed F; otherwise the refutation is written, and its printed form re-enters as the same node;
- a raw Janet marking of a fact node is refused, naming `zelph/refute`.

A derived marking of an already refuted fact is accepted and changes nothing; inside an opaque argument a marking, or a refuted fact, is a mention and contradicts nothing. With this, universal negation yields a "No":

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace No man waits. Bob is a man.
    Does Bob wait?

    Answer: No. It is false that Bob waits.
    ```

"No man waits." is the rule `(*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)`; it derives the refutation of "Bob waits", and the question finds it. Had an earlier block claimed "Bob waits.", the same firing would be the reported contradiction of the first point ([Contradictions and Defeated Conclusions](block.md#contradictions-and-defeated-conclusions)).

**Why.** Before, a claimed fact met by a derived marking was silently un-claimed – never designed, and contradicting the documentation. Without derived classical negation, negative conclusions would compile only to denials, and "No man waits." would never answer "No".

**Not taken.** Refusing `~ refuted` as a rule consequence (no derived classical negation at all); keeping the silent un-claim and changing the documentation instead.

**Milestone.** M4 (the engine side is built: host item P-refute).

### DEC-RECORD-SINGLE – one-condition rules as one-element condition sets {#dec-record-single}

**Question.** zelph records a contradiction by the members of a rule's condition set, so that later runs stay silent about it; a rule with a single plain condition has no such set, and its contradiction was announced again on every run – unless the rule derived the refutation of a claimed fact, which is recorded with the condition itself ([DEC-REFUTED](#dec-refuted)). Is the engine changed?

**Decision.** No. The front end builds every one-condition rule as a one-element tagged condition set inside `zelph/dedup-rule`, which the engine records. Such a rule prints as `(*{(X ~ man)} ~ conjunction) => …`, which re-enters as the same, recorded rule ([Rules and Inference](../rules.md)). A test pins it.

**Why.** An engine change would help native users, but not only: a recorded one-fact key would silence a second, different contradiction resting on the same fact that a native user sees today; an exclusion for refusals would change two-condition refusals; and record memberships are data that answer `S in O`. The condition for changing pinned engine behaviour – only positive effects – is not met. For the front end the record only keeps later runs silent; completeness comes from the quiet run's return value ([DEC-QUIET](#dec-quiet)), which no record can mask.

Open for M4: rule identity reads a one-member condition set as its single condition. So on a graph that already holds the rule in the plain form, because a user typed `(X ~ man) => …` natively before, the front end's one-condition rule meets that rule and adds nothing, and that rule's contradictions are not recorded but reported again on every run (a derived refutation of a claimed fact is recorded either way); and when the rule the user typed is ground, a native mention of it, which takes a typed ground rule out of force ([K1-P1](prerequisites.md#k1-p1-a-mention-switches-a-typed-ground-rule-off)), takes the front end's rule out of force with it (entering the sentence again builds a set-form rule of its own, which is in force). This is an open point on zelph's side, not a defect of the front end. The answer to the repeated report lies in the engine – recording every contradiction of a plain single-condition rule as those of the one-member set are recorded, where the record would be the same set constant – or in accepting the repeated report; the mention is answered by the record that a rule was asserted (K1-P1).

**Milestone.** M4 (the pin is in place since M0).

### DEC-GENERATIVE – moot {#dec-generative}

Rules with a fresh variable in the conclusion ("Every man owns a dog.") needed the engine's guard against duplicate witnesses fixed; zelph's guard is a backtracking join over facts now ([Fresh Variables: Generative Rules](../logic.md#fresh-variables-generative-rules)), so generative heads are supported from M4 and counting from M7. Documented limits: the witness set depends on which rule fires first and is unique only up to homomorphism; a witness made before a named fact arrives is kept.

### DEC-NESTED-IMP – a rule generator for a switch, flattened otherwise {#dec-nested-imp}

**Question.** A conditional inside the conclusion of another ("If John waits then every man sleeps."; "Every man feeds every dog."; 88 accepted corpus cases) can become a zelph rule generator, `(A) => ((B) => C)`, or one flattened rule `(A, B) => C`. Which, when?

**Decision.** A generator in two shapes; flattened otherwise. The forms below are those `ace/zelph-of` prints ([DEC-ONE-CONDITION-PRINT](#dec-one-condition-print), [DEC-VAR-LETTERS](#dec-var-letters)):

- a **switch**: the outer condition declares no referent (only ground atoms) and the inner rule keeps a variable – "If John waits then every man sleeps." becomes `(*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))`; applied recursively, so a chain of switches is a generator of generators;
- an inner rule that uses a referent the outer conclusion introduces – "Every man owns a dog that every cat hates." becomes `(*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog) ((*{(Z ~ cat)} ~ conjunction) => (Z hates Y))`, because Y is a fresh witness the inner rule must share, and no flat rule is equivalent.

Every other nested conditional is flattened: "Every man feeds every dog." is `((X ~ man), (Y ~ dog)) => (X feeds Y)`. A fully ground inner rule ("If John waits then if Mary sleeps then Bill runs.") is always flattened. The generator and every rule it writes claim none of their ground parts; provenance names the generator's sentence. The table of every nested shape and its mode is in [Nested Implications](semantics.md#nested-implications).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace If John waits then every man sleeps. Bill is a man.
    Does Bill sleep?

    Answer: It is not provable that Bill sleeps.
    zelph> ace John waits.
    Does Bill sleep?

    Answer: Yes. Bill sleeps.
    ```

The native form, typed as `ace/zelph-of` prints it (one-condition rules as one-element condition sets, [DEC-RECORD-SINGLE](#dec-record-single)), works in today's zelph; the switch puts the inner rule in force only once John waits, and `.list-rules` then shows both:

```
zelph> .lang ace
ace> (*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
ace> Bill ~ man
ace> X sleeps nil
ace> John waits nil
((*{(X ~ man)} ~ conjunction) => (X sleeps nil)) ⇐ {(John waits nil)}
(Bill sleeps nil) ⇐ {(Bill ~ man)}
ace> X sleeps nil
Answer: Bill sleeps nil
ace> .list-rules
Listing all rules:
------------------------
(*{(X ~ man)} ~ conjunction) => (X sleeps nil)
(*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
------------------------
```

**Why.** For first-order conditionals nesting is logically flat – (A → (B → C)) is (A ∧ B → C) – so the shape is an engineering choice. A generator per outer binding is unbounded in rules on a large graph (300 kings gave 301 rules and runs 10-20 times slower), and a generator whose inner rule is fully ground never fires. Typed natively, such a generator also takes an identical ground rule typed earlier out of force, because both are one node; when either of the two is built from Janet, as the front end builds both, that rule stays in force. The switch keeps zelph's "rule under a condition" idiom reachable from ACE at the cost of one generated rule.

**Not taken.** Always flattening (no ACE route to zelph's rule generators); always a generator (cannot work for ground inner rules, and costs a rule per binding).

**Milestone.** M4. Keeping fully ground switches as switches needs the record that a rule was asserted ([DEC-CAT4](#dec-cat4), item K1-P2).

### DEC-QUNIV – a conditional inside a question {#dec-quniv}

**Question.** "Is it true that every dog is an animal?" (the shape of corpus case 1974) asks whether a rule follows from what is known (30 accepted corpus questions hold an implication). What does the front end answer?

**Decision.** A guarded hypothetical check. In the question's scratch cluster the counter-pattern is queried first: an instance of the if-part whose then-part is refuted gives "No. It is false that …". Otherwise the if-part is asserted for fresh witnesses `ace:hyp-<h>` (in `zelph`, never a content-addressed individual, whose own facts would leak in), one quiet run follows, and the then-part is checked: "Yes. …" when it holds, "It is not provable that …" otherwise; the cluster is dropped. The guard runs before anything is written: `(zelph/rules)` lists the rules in force with their negated conditions and guards (host item P-rule-list), and when a rule with a NAF or `!=` condition is reachable from the hypothesis in the may-derive graph, the block is refused with `:rule/unsafe-hypothesis`, because a fresh witness is "not provably" everything and the answer would be wrong. Contradictions the hypothetical run meets are not reported (they go with the cluster); when it meets one, the answer is "It is not provable that …". A conditional inside a wh-question ("How does every man wait?") has no route (`:rule/in-wh-question`). The sessions and the steps are in [Questions That Hold a Conditional](answers.md#questions-that-hold-a-conditional).

**Why.** Entailment is what the question means, and a wrong "Yes" is worse than a refusal; the guard serves both. The rule listing also lets the context checks see native rules.

**Not taken.** Refusing such questions in v1; the hypothetical check without the guard (unsound beside NAF or `!=` rules). A closed-world search for counterexamples was never offered: it answers "Yes" to "Is every man a farmer?" when no man is known.

**Milestone.** M4.

## M5: Arithmetic

ACE has formulas with integers and the operators `+ - * / ^`, comparisons and identity. zelph's math stack computes them; these decisions fix what the front end adds and where it lives. The details are in [Numbers and Formulas](math.md).

### DEC-MATH-MODULE – a general evaluation module {#dec-math-module}

**Question.** Where do the rules for a value, the exact quotient, the difference and the comparison of ACE terms live, and what are they called?

**Decision.** A general stdlib module, `stdlib/evaluation.zph`, with its own documentation page (`mkdocs/docs/math/evaluation.md`) and its own test file over the three arithmetic substrates. Its names are chosen for readers who know other systems:

- `(A exquo B) = Q` – the exact quotient, silent when B does not divide A or B is 0: the name and meaning of `exquo` in Axiom/FriCAS and SymPy. ACE's `/` maps to it, never to zelph's native `/`, which truncates on naturals. A ground `exquo` term simplifies to its value, so `(:simplify T)` gives the value of every ground ACE term.
- `(L difference R) = D` – the normal form of L − R. "diff" is avoided, because `diff` and `diffby` mean differentiation in zelph.
- `(L compare R) = lt | eq | gt` – the verdict when the difference is constant: Haskell's `compare` with its three answers, in the lower-case form of zelph's `cmp`; it derives only that verdict.

`math.zph` does not import the module; `ace-math.zph` does, and maps its request keys `:value :quot :diff :cmp` to these names in one table.

**Why.** The rules carry no ACE knowledge; the gap they fill is the math stack's own: an integer evaluator across ℕ and ℤ with exact division. zelph's `:simplify` already folds ground ℕ/ℤ terms to one canonical value, so a value request of the module's own would have been a second implementation; what was missing is ACE's exact division and the verdicts.

**Not taken.** The rules private in `ace-math.zph` under ACE-prefixed names (nothing outside ACE would benefit); importing the module from `math.zph` (every math user would pay its run cost).

**Milestone.** M5.

### DEC-MATH-INDET – an ACE math variable is the node named as written {#dec-math-indet}

**Question.** Which node stands for an ACE math variable such as `X` in "X + 1 = 3"?

**Decision.** The node named as written, `X`, in language `zelph`.

**Why.** The defect that made this choice costly is fixed: zelph's formula syntax `$( … )` now reads a quoted leaf back as the named node, so ACE terms shown in zelph syntax re-enter.

**Not taken.** The lower-case node `x` (it may be an unrelated node of the user, which would then become a symbolic variable); fresh names per paragraph (unreadable, and nothing shared).

**Milestone.** M5.

### DEC-MATH-LANG – numbers and the arithmetic stack live in `zelph` {#dec-math-lang}

**Question.** Number literals used to follow the current naming language: under `.lang en`, `&3` was a meaningless list and a typed `&1 + &2` answered nothing. Should numbers be independent of the naming language?

**Decision.** Yes, in the stdlib (host item P-number-lang): number literals and `zelph/list-chars` characters are resolved in `zelph`, whatever `.lang` the importer has (`zelph/int` names `neg` and `zint` there too; node lists `< a b >` keep the current language). Every one of the 17 modules of the arithmetic stack starts with `.lang zelph`, and a module's `.lang` ends with the module, so the whole stack is named in `zelph` even when it is imported under another language. Consequence: arithmetic imported under `.lang en` computes under `.lang zelph` only; the front end executes under `zelph` anyway.

**Why.** A number has no language, the defect also broke native literals, and the fix is small and stays in the stdlib. The first version of the fix would have split a substrate imported under another language (its literals in `zelph`, its digit alphabet, tables and rules in the import language), so the whole stack resolves in `zelph`.

**Not taken.** Refusing arithmetic while `.lang` differs from the language math was imported in (a refusal kept for a defect a few lines remove); documentation only (ACE arithmetic would silently answer nothing).

**Milestone.** Engine; built. Arithmetic in ACE M5.

### DEC-NEQ – unique names for named constants, by node identity {#dec-neq}

**Question.** When a sentence compares individuals ("John = Mary.", "If a man X likes a man Y and X \= Y then …", "Is John Bill?"), do two different names denote two individuals, and where is that decided?

**Decision.** Unique names hold for **named constants** only: resolved proper names, Wikidata items, strings, numbers. Two named constants are identical if and only if they resolve to one node, decided after resolution and before the dry run (plans stay pure: the decision lives in the resolution step); names not yet in the graph are compared by their canonical symbol ([DEC-MULTIWORD](#dec-multiword)). Individuals and witnesses are not names: an identity involving one is "Yes" when both sides are one node and "not provable" otherwise, and asserting one between an individual and a name is refused (`[ace/unsupported]`: the front end never merges two existing nodes). `=` with a referent side substitutes; `\=` over variables becomes a `!=` guard (closed-world, documented, see [Inequality Constraints](../logic.md#inequality-constraints)); an identity in a conclusion becomes a denial; a false ground statement is `[ace/contradiction]` naming what each side resolved to and suggesting a binding (aliases are not imported: "Mark-Twain is Samuel-Clemens." is a contradiction until the user binds both names to one node); a false condition makes the rule vacuous (`[ace/w-vacuous]`). "John is Bill." is read like "John = Bill.", as APE gives both an identity DRS: `:copula/identity` becomes supported at M5 with `:formula/identity`, and corpus case 3751, which identifies a name with itself written after "the", is accepted ([Identity](semantics.md#identity)).

**Why.** It makes ACE say what the engine already does – `!=` and node identity are unique-name reasoning – while two names bound to the same Wikidata item count as one, which deciding by name alone would get wrong.

**Not taken.** Unique names decided by name when planning (aliases bound to one node would be judged distinct); APE's open identity, where only `\=` in rule conditions maps to `!=` (a rule with `X \= Y` would fire for John and Bill while "Is it true that John \= Bill?" stays unprovable); refusing every formula over individuals (loses the `\=` guards and the route to "at most" and "exactly").

**Milestone.** M5.

## M7 to M9: Plurals, Modality, Retraction

### DEC-PLURALS – plurals in full {#dec-plurals}

**Question.** How do plural groups, cardinalities, lists, sets and mass nouns map (P 482, U 125 and L 13 accepted corpus cases)?

**Decision.**

- A collective group is a set constant of its members: "John and Mary lift a table." is `{John Mary} lifts Table-<h>`, identity by members, re-entering with no new node.
- A top-level plural indefinite ("2 cars") gets one content-addressed individual per member; distribution ("each of", "for each of") expands to one fact per member, never a multi-object fact; a condition on members uses `M in G` over the set constant.
- "at most n" and "less than n" are denials over n+1 pairwise distinct witnesses. Because a zelph pattern cannot see whether a node is named, every individual the front end creates carries the internal mark `I ace:individual nil` (in `zelph`), written with it, and the denials skip marked witnesses with `¬(W ace:individual nil)`; Wikidata items and names need no mark (a mark on named constants would have to be written on millions of items). So "John owns a red car. John owns a car. John owns at most 1 car." is no contradiction: the two cars may be one.
- Engine witnesses of rules with a fresh variable carry no mark (a mark in the head would change the engine's duplicate-witness guard), so they count as distinct for these denials – a closed-world reading for witnesses, documented with "Every man owns a red car. Every man owns a blue car. John is a man. John owns at most 1 car.", which is a contradiction.
- "at least", "more than", "exactly" and plural indefinites in conclusions are supported too; ACE lists map to zelph lists, ACE sets to set constants.
- A mass noun without a measurement is a portion: "John drinks some water." introduces one individual `Water-<h> <instance> water`. Measured quantities and units stay refused (`:data/measured`): zelph has no units and no reals.

**Why.** Most of the plural, list and set constructs without engine work, with collective facts and distributive conclusions both expressible. The cost of "at most n" is n+1 witnesses and n(n+1)/2 guards per subject ("at most 5" over 20 men with 5 cars each: 781,300 matches per run, measured).

**Not taken.** The distributive reading for every plural (it changes ACE's meaning: "John and Mary lift a table." would say each of them lifts it); keeping every plural construct refused in v1 (up to 620 corpus cases). Collection literals were never offered: every literal builds a new container, so re-entering a block would add nodes.

**Milestone.** M7 ([Plurals, Groups, Cardinality and Mass](semantics.md#plurals-groups-cardinality-and-mass)).

### DEC-MODAL – opaque predicates over one generalised mark {#dec-modal}

**Question.** Modality ("John can swim.") and that-clauses ("John believes that Mary waits.") cover 310 accepted corpus cases. In zelph every part of a statement is claimed – `Sue says (Bob waits nil)` asserts that Bob waits. How is a statement written without claiming it?

**Decision.** Opaque predicates (host item P-opaque). `=>` is documented as the built-in opaque predicate; any predicate becomes opaque through `P ~ opaque`; every argument position of an opaque fact is a mention scope: whatever a writer newly creates there gets the existing rule-pattern mark – one mark kind for rules and mentions – and a later claim revokes it. `.explain` and `.node` choose the label by position. Programs write mentions with `zelph/mention` (fact and thunk form) and the C function `zelph_mention`; a `=>` written so is a mentioned rule, not in force until something claims it ([DEC-MENTION-RULE](#dec-mention-rule)), which is how the front end writes the rule of a that-clause ("John believes that every man waits.") inside the opaque statement that holds it. The declaration is refused while a fact of P already holds a statement. The front end declares `ace:can`, `ace:must`, `ace:should`, `ace:may` and the verbs of that-clauses opaque at their first use per graph epoch, and writes "John believes that Mary waits." as `John believes (Mary waits nil)`. A box with several conditions or a referent is the set constant of its atoms as mentions, matched by its members (host item P-set-match).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John believes that Mary waits.
    Does Mary wait?
    Does John believe that Mary waits?

    Answer: It is not provable that Mary waits.
    Answer: Yes. John believes that Mary waits.
    ```

Natively, the declaration keeps the nested statement unclaimed, a predicate that is not opaque claims it, and the declaration is refused once a fact of the predicate holds a statement – which the front end reports as `[ace/unsupported]` naming `:sub/that-clause` and the predicate:

```
zelph> believes ~ opaque
zelph> John believes (Mary waits nil)
zelph> X waits nil
zelph> X believes Y
Answer: John believes (Mary waits nil)
zelph> Sue says (Bob waits nil)
zelph> X waits nil
Answer: Bob waits nil
zelph> says ~ opaque
Error in line "says ~ opaque": fact(): a predicate cannot be declared opaque while one of its facts holds a statement -- says: Sue says (Bob waits nil)
```

Refused by id in v1:

- a that-clause holding a ground conditional (`:sub/ground-conditional`, "John believes that if Mary waits then Bill sleeps."): a plain single-condition ground rule that is typed and mentioned is one node, and the mention takes the typed rule out of force; lifting the refusal needs the record that a rule was asserted ([DEC-CAT4](#dec-cat4), item K1-P1);
- a conditional with referents inside a that-clause in a condition (`:sub/conditional-with-referents-in-body`): a rule condition that holds a rule with variables binds the stored rule's ground parts, so "If John believes that every man waits then John is happy." would conclude from "John believes that if Bob is a man then Bob waits." and never from "John believes that every man waits." – unsound for ACE;
- a conditional with referents inside a that-clause in a conclusion (`:sub/conditional-with-referents-in-head`): a firing writes nothing, or `??` witnesses, for such a mentioned rule, and a silent acceptance is never allowed.

A question over a that-clause holding a conditional stays supported: it writes a new collection and answers "not provable", which is sound.

**Why.** It is the design with the fewest special cases in the long run. The alternatives cannot carry a mention in a rule's conclusion – a firing claims what it writes – which would leave the 60 corpus cases with a modal under an implication, and that-clauses in conclusions, without a representation.

**Not taken.** Refusing modality by id (310 cases); a mention binding that reuses the rule-pattern mark without opaque predicates (it mislabels every reader); a mention with a mark of its own (every reader site duplicated). Never offered: dropping the modal operator ("John can fly." would assert that John flies) and a tautological `F => F` per mention.

**Milestone.** M8 (the engine side is built: P-opaque, P-set-match; [Modality and That-Clauses](semantics.md#modality-and-that-clauses)).

### DEC-RETRACT – `ace-retract` {#dec-retract}

**Question.** ACE has no sentence that takes a statement back, and zelph has no truth maintenance: removing a fact leaves what was derived from it. How does an ACE user undo a statement?

**Decision.** A block keyword `ace-retract`, registered at M9. Its fact sentences are analysed, planned and resolved as an `ace` block's, and each atom is removed with `(zelph/prune-fact f)` – unless a derived fact depends on it or another statement nests it, in which case the block is refused with `[ace/retract-dependents]`, listing the derived facts in ACE and naming the holding sentences, and nothing is removed. Three engine bindings serve it (host item P-retract): `(zelph/dependents f)`, the derived facts with a justification through f, found by the `.explain` machinery and bounded by `.explain-budget` (ending with `:truncated` when the budget runs out, and the refusal then says that the list may be incomplete); `(zelph/holders f)`, the statements that nest f, in time proportional to the degree of f; and `(zelph/prune-fact f)`, which answers `:removed`, `:unclaimed` (the atom stays as part of a rule or a mention, and the front end removes its own provenance for it) or nil. A holder that the same block takes back does not count, so "John waits in a park." is taken back whole while "John waits." alone is refused. Only fact sentences are taken back; a rule, a negation or a question is refused by id. The steps are in [Taking a Statement Back](block.md#taking-a-statement-back-ace-retract).

**Why.** Un-saying is unsound without knowing what depends on the fact; with the dependents known, single-fact retraction is sound.

**Not taken.** No ACE route in v1, only the native `.prune-facts` (derived facts would linger); a keyword without the dependents check (it would offer an operation known to leave stale derivations). Dropping a block's cluster was never offered: it would also destroy later blocks' facts on nodes the block created.

**Milestone.** M9 (the bindings are built).

### DEC-LEX-LARGE – no shipped large lexicon {#dec-lex-large}

**Question.** Should the front end ship a large English vocabulary beyond `ace-core`?

**Decision.** No. Guessing gives word classes ([DEC-UNKNOWN](#dec-unknown)), the graph gives meanings ([DEC-WIKIDATA](#dec-wikidata)); users load their own ulex files (AceWiki and APE exports verbatim; the large Clex lexicon only if they fetch it themselves, under its own licence). A lexicon generated from Wikidata's English lexemes stays a later option.

**Why.** The v1 milestones need none: the corpus runs on APE's small lexicon, hand-written tests on `ace-core`. The annotator reaches the lexicon only through `ace/readings`, so nothing blocks a larger lexicon later.

**Not taken.** A generated lexicon from Wikidata's lexemes (verb valency practically absent, no gender, no mass/count distinction); entries derived by zelph rules from a loaded graph (a rebuild linear in the number of entries).

**Milestone.** None in v1.

## Wikidata and Unknown Words

The requirement behind this group: after a Wikidata import, content words reach Wikidata's items directly, without declaring or binding words ("There is a man." uses Q8441); multi-word names ("Douglas Adams") work; the interplay with `.lang` is defined; without Wikidata the front end behaves the same, except that it creates the nodes; nothing in its path is linear in the size of the graph; memory after a load does not rise noticeably; and an ambiguous name is resolved deterministically and understandably, never silently. [Words and Wikidata](words.md) describes the result for users; the decisions are these.

### DEC-CONTENT-NS – the front end names its content in language `ace` {#dec-content-ns}

**Question.** zelph names nodes per naming language. In which language does the front end create the nodes for nouns, names, verbs, adjectives and individuals?

**Decision.** In language `ace`. Every node the front end builds has an explicit language: content in `ace`; internal vocabulary (`refuted`, `opaque`, `ace:…`), characters, digits and the math stack in `zelph`; user lexicon entries in `ace-lex`; Wikidata ids are found in `wikidata` and labels in `en`, never created there. The front end runs its execute step under `.lang zelph`, set and restored through `zelph/lang`, so whatever zelph names implicitly (rule and query variables, characters) lands in `zelph`. The user's `.lang` changes only what the native REPL shows and how it reads typed names: a native line meets the front end's content under `.lang ace` ([What a Block Writes in zelph](index.md#what-a-block-writes-in-zelph) shows it; [Naming Languages](words.md#naming-languages)), while native input under the default `.lang zelph` creates separate nodes. A node named only in `ace` displays by that name in every language.

**Why.** `en` is Wikidata's label space, where one name maps to one node: a node the front end creates for a verb ("likes"), an adjective ("red") or an individual must never take or merge a label a Wikidata item holds. The behaviour is the same with and without Wikidata. And the stdlib's internal names live in `zelph`, so content never collides with them ([DEC-NAMES](#dec-names)).

**Not taken.** The current `.lang` (the first decision, [DEC-CONTENT-LANG](#dec-content-lang): under `.lang en` the front end would have competed with Wikidata's labels); always `zelph` (collisions with the stdlib); an option for the content language (one more setting to keep in step).

**Milestone.** M1.

### DEC-WIKIDATA – words reach Wikidata items through their labels {#dec-wikidata}

**Question.** How does a content word find its Wikidata item, and what happens when several items carry the same label (108 items are labelled "Berlin")?

**Decision.** A content word arrives with its symbol (lemma, canonical name, or the symbol of its lexicon entry) and its **role**: *noun* (common nouns, the class of a class atom), *name* (proper names), *relation* (relational nouns in a relation position, transitive adjectives), *verb*, *adjective* (intransitive). Resolution happens in the resolution step of each block, before the dry run, and creates nothing. The first matching step wins ([The Four Steps](words.md#the-four-steps) has them with sessions):

1. **Explicit id**: a prefixed word – `p:Q42` (name), `n:Q8441` (noun), `n:P50` (relation noun), `v:P155` (verb), `a:Q3142` (adjective) – or a lexicon entry whose symbol is an id. A bare `Q42` is an ACE variable and stays one.
2. **`ace` node**: a node the front end created, or the user named, in `ace`; never a core node.
3. **Wikidata label**, on a Wikidata graph with current labels, for the roles *noun*, *name* and *relation* only. Candidates: `zelph/lookup-all` in `en` for every hyphen/space variant of the word (at most 16 lookups), for a name also with a leading "The " ("the Hague"), united; kept: items for *noun* and *name*, properties for *relation*; dropped: Wikimedia disambiguation, list, category and duplicate pages (exact probes against a short list of page classes). For a *name*, if the candidate with the highest degree is a given-name or family-name item, the name is **not** bound and stays a new individual (an undeclared "John" would otherwise be a historical count palatine). Otherwise the candidate with the **lowest Q-number** (P-number for relations) wins – the number in its Wikidata id, never a node id. A relational noun's two roles are resolved together: the relation is the property if there is one, else the noun's item; the noun is the item if there is one.
4. **New node**, named in `ace`, created in the dry and the real run.

Verbs and intransitive adjectives skip step 3: measured on Wikidata, 1 of 257 verb lemmas meets a property, in the wrong sense, and 24 of 63 adjectives meet a wrong item, while no Wikidata fact has the shape `X is <item>`.

**Announcements.** On a Wikidata graph, every word that step 3 bound to a label with more than one carrier, and every undeclared noun or name that step 4 creates, is announced once per word, role and session, in one note per block, counted only when the block succeeds (`[ace/word-senses]`; the note of the tutorial session is under [DEC-PATHS](#dec-paths), its rules in [The word-senses Note](words.md#the-word-senses-note)). A label only one node carries is no ambiguity and gets no line ("human", Q5). `(ace/senses w)` lists the candidates with their ids, labels and one superclass each. **Declared names are the user's**: a proper name with a lexicon entry is resolved by steps 1, 2 and 4 only, never through labels; `ace-core` declares the common ACE example names (John, Mary, Bill, Sue, …), so the canonical ACE texts keep their meaning on a Wikidata graph. A user who wants Berlin to be their own declares `pn_sg('Berlin', 'Berlin', neutr)`; one who wants a particular item, `pn_sg('Berlin', 'Q821244', neutr)`. A declared noun without an id symbol (`man` from `ace-core`) is announced when step 3 binds it to a shared label: its entry declared the word's grammar, not its meaning.

**Verbalising.** A node is rendered by its `ace` name; else by its `en` label in a spelling that reads back as the same node in its role; else by its prefixed id (`p:Q123`, `n:Q123`, `n:P50`, `v:P155`). Two capitalised names are never printed side by side, and in guessing mode a multi-word name is written as it is and re-enters through [DEC-MULTIWORD](#dec-multiword)'s fallback. zelph's own answers to a native line under `.lang ace` quote a name with a space and come in zelph's order, so tests compare them as a multiset. The rules, with examples, are in [How Answers Name a Node](words.md#how-answers-name-a-node).

**Output size.** At most `:max-answers` answer lines per question (default 100) and at most 5 contradictions per rule and block, each limit followed by a count of the rest ([Output Size](answers.md#output-size)).

**What the engine and the importer provide.** Every item keeps its label as its `en` name (an item without one gets its `mul` label), every import writes the marker `wikidata:labels-v2`, and a label index lists every carrier of a label (`zelph/lookup-all`). Step 3 is off, with a warning, on a network imported before these changes (`[ace/w-old-labels]`) and after a partial load (`[ace/w-index-incomplete]`), and it picks the lowest Q-number itself rather than trusting the carrier the importer ranks first. The memory this takes is estimated at about 1 % of the full graph. The removal path, the predicate index and the incremental-run items keep every dry run, question and class question bounded by the block, not the graph ([Reasoning That Costs the Change, Not the Graph](foundation.md#reasoning-that-costs-the-change-not-the-graph)). Details: [A Graph with Current Labels](words.md#a-graph-with-current-labels) and [Wikidata Labels and the Label Index](foundation.md#wikidata-labels-and-the-label-index). The published networks have to be regenerated with the current importer, which only the maintainer does ([Outside the Code](#outside-the-code)).

**The `ace` network.** The published `-small` and `-medium` networks lack most of what everyday words should reach – taxa, planets, countries, cities and people are pruned away – and while an intended item is pruned, its word binds to some other carrier. So the front end gets a network of its own, `<stem>-ace.bin`: an import into an empty graph of a subset of the dump (the class hierarchy, every property and a keep set of notable items), chosen by criteria that are data in `dev_scripts/ace-network.json` and applied by `dev_scripts/wikidata-subset.py`, both built and tested; no engine change is needed. Its size is estimated, not measured, at 6.8-7.8 million nodes and 1.8-2.5 GiB in memory (targets: at most 10 million nodes and 3 GiB). The criteria, the other estimates and the acceptance checks are in [A Network for ace](words.md#a-network-for-ace). The front end evaluates the regenerated `-ace` network first, then `-small` and `-medium`.

**Why.** It meets the requirement above: no declarations for the common case, deterministic choices, every ambiguity announced with the item's page and the way to choose another, and the user's own declarations always win. The lowest Q-number was right for 78 of 88 frequent nouns and 25 of 27 names on the full graph in a measurement, where the importer's old label holder was right for 31 and 0; excluding given-name items kept 49 of 52 places and 19 of 22 people unchanged while 30 of 31 given names became individuals.

**Not taken.** Binding words through the label holder of the old importer (wrong for almost every shared label); `.name` as the binding route (onto a name another node holds it merges the two nodes, [DEC-RELOAD](#dec-reload)); a shipped large lexicon ([DEC-LEX-LARGE](#dec-lex-large)).

**Milestone.** M1 (the importer and the index are built).

### DEC-UNKNOWN – unknown words are read by their position {#dec-unknown}

**Question.** ACE requires every content word to be in the lexicon; a user of zelph should not have to declare "admires" before using it. How are words without an entry read?

**Decision.**

- **Readings.** A lower-case word without an entry gets the readings its form allows: an irregular plural only a plural noun; a regular plural-looking form a plural noun (preferred) plus marked singular and mass readings; other forms singular and mass noun; verb readings intransitive, transitive and the double-object ditransitive only (never a ditransitive with a dependent preposition, which would swallow a prepositional phrase); participles for `-ed` and the irregular participles; `-ing` forms adjective and noun only; adjective readings; adverb readings only for `-ly`. Never guessed: prepositions, measurement nouns, core spellings, function words, and a list of English function words ACE lacks (`very also almost only even just too quite rather many few several both either neither those …`), which give `[ace/unknown-word]` with an "ACE has no …" hint.
- **Grammar.** The committed-choice grammar cannot parse sentences in which most words carry several readings, so a fourth variant is generated from the same rule builders: a backtracking interpreter over the rule data, used for every sentence with a guessed reading, with ACE's interpretation rules as cuts, a work budget, and a ranking (fewest marked readings, then rule order). A second parse of equal rank gives `[ace/w-guess-ambiguous]`, naming the token, both readings and the prefix that selects the other; the budget gives `[ace/too-ambiguous]`.
- **Multi-word names.** A guessed verb's double-object reading never takes two adjacent words of one capitalised run as its two objects; without that check "There is a man who admires Douglas Adams." would read "Douglas" and "Adams" as two objects of "admires", and the name would never be merged. A declared ditransitive verb keeps APE's parse ("John gives Bill Mary.").
- **Gender.** A guessed noun has an open gender: `who` and `which` fix it for the block (human, neuter); personal and reflexive pronouns treat it as neuter until fixed, and a failing he/she next to a guessed noun gets a hint. `ace-core` ships about 300 person nouns with gender (without them, 400 of 2682 corpus cases fail on `who` and pronouns alone).
- **Lemmas** are graph-independent: analysis by synthesis over the forward morphology rules, with the lexicon lemma, else the lemma of an earlier token of the same text, else the most specific rule; `[ace/w-lemma]` when only rule order decided. Never an `en` label or an `ace` name, because the lemma feeds the DRS and the individual names, which must not depend on the graph.
- **Messages.** Guessing is silent, except: a guessed word one typing edit away from a lexicon form or an `ace` name is rejected with `[ace/unknown-word]`, the suggestion and the prefix that insists on the word, so a typo writes nothing; `[ace/w-compound]` when a guessed adjective + noun is a known compound; `[ace/w-guess-reading]` when the chosen parse uses a marked reading; `[ace/w-ing]` when a guessed `-ing` word after `is` has a known verb stem (ACE has no progressive). A typing edit is an adjacent transposition, a doubling, a neighbouring key, or an insertion or deletion in words of 5 letters or more – plain edit distance would reject "A dog barks.", `dog` being one deletion from `do`; capitalised words are names and never near misses.
- **Strict mode** (`(ace/option :guess false)`, APE's behaviour without guessing) is an option and the corpus harness's mode.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Bill likes Sue.

    zelph> ace John likse Mary.

    Error in line "ace John likse Mary.": Lexicon error [ace/unknown-word]: sentence 1, line 1, column 6: "likse" is not a known word. Did you mean "likes"?
      John <>likse Mary.
      If "likse" is a new word, write v:likse.
    ```

"likes" was guessed in the first block and became a node in `ace`; "likse" is one transposition away from it, so it is rejected instead of being guessed as a second verb.

**Why.** Users write the words they know, the lexicon gives grammar, the graph gives meaning, and a typo never silently creates a node. The gates that guard it are in [The Guessing Gates B, N and W](tests.md#the-guessing-gates-b-n-and-w): with APE's small lexicon loaded, the 2682 runnable corpus cases must give the same DRSs in guessing mode as in strict mode, and a blind run with only the closed word classes must either give an equivalent structure, reject with a hint, or announce – never change the structure silently.

**Milestone.** M1 (guessing), M2 (the backtracking variant at grammar parity).

### DEC-MULTIWORD – multi-word proper names {#dec-multiword}

**Question.** Is "Douglas Adams" one name or two?

**Decision.** A sentence is parsed with separate names first (APE's reading); only if that fails is it parsed again with every run of two or more capitalised words merged into one name. Runs never include variables, quoted words, words with a preposition reading or individual names; strict mode never merges. A proper name's canonical symbol joins hyphen and space runs into single spaces (`Douglas-Adams`, `Douglas Adams` → "Douglas Adams"), for lookup and creation alike, so both spellings are one node with and without Wikidata; the DRS keeps APE's form. A merged run that is not a known name gets `[ace/w-multiword]` once per session; the tutorial pins its text: `Warning [ace/w-multiword]: sentence 3, line 1, column 37: "Douglas Adams" is read as one name.` Common nouns keep their hyphens (`credit-card`), and step 3 also tries the spaced variant.

**Why.** APE's parses stay intact where they exist ("John gives Bill Mary." and "Is John Bill?"), and the names users type from Wikidata work. Corpus rejections 1400 and 1402 stay rejected in strict mode.

**Milestone.** M1.

### DEC-QWORD – a quoted word is one proper name {#dec-qword}

**Decision.** A backquoted word has exactly one reading, a singular proper name; its symbol is the whitespace-normalised content, verbatim otherwise (the canonical hyphen/space form applies to unquoted names only: `` `Spider-Man` `` is the node "Spider-Man"); its gender comes from an exact name entry, else it is undefined; empty, whitespace-only and multi-line content is refused. It takes resolution steps 2-4 like any name (step 1 does not apply: `` `Q42` `` is the literal name "Q42", unless the same block also writes `p:Q42`, where the explicit id wins for both, since the DRS gives both the same name).

**Why.** It is the route to labels with punctuation (`` `Washington, D.C.` ``, `` `The Hitchhiker's Guide to the Galaxy` ``). Four differences from APE are recorded in [Deliberate Deviations from APE](grammar.md#deliberate-deviations-from-ape).

**Milestone.** M1.

### DEC-THE-NAME – `the` + an undeclared capitalised word {#dec-the-name}

**Decision.** `the` followed by an undeclared capitalised word is a proper name, the article dropped, as APE does; eval-0 corpus case 2923 is accepted. Resolution step 3 also tries the label with "The ".

**Milestone.** M1.

Spaced word forms in ulex entries stay rejected (use hyphens): a merged run looks up its hyphen-joined form in the lexicon, so `pn_sg('Douglas-Adams', 'Douglas Adams', masc)` applies to "Douglas Adams".

## Engine Decisions Taken While the Engine Work Was Built

Building the engine items raised design questions that the specification had left open, or that the tests written against each item exposed. Each was decided from the evidence, is general engine behaviour that holds without the front end, and is built; the documentation of zelph describes the result. The front end relies on them, which is why they are recorded here.

### DEC-GOAL-DOCS – the specification as a documentation category {#dec-goal-docs}

**Decision.** The complete specification of the front end goes into zelph's documentation as a category of its own – these pages – written at once for reviewers of a funding proposal and for the sessions that will implement it: every detail, with example sessions marked as specified behaviour, metadata where the implementation needs it, large data hosted on zelph.org, and a page on what zelph still needs, built from the category-4 write-ups. Real zelph transcripts follow the documentation's rule: a real run, pasted verbatim. Where the pages live is [DEC-DOCS-PLACE](#dec-docs-place).

### DEC-CAT4 – the remaining engine items are specified, not built {#dec-cat4}

**Question.** Some engine items the front end needs change zelph's model or broke unrelated tests when attempted. Are they built now?

**Decision.** No: they are category 4 of [DEC-WORKMODE](#dec-workmode), specified with real sessions in [What zelph Still Needs](prerequisites.md#the-seven-items-at-a-glance) and implemented in the funded work. There are seven:

- **[K1-P1](prerequisites.md#k1-p1-a-mention-switches-a-typed-ground-rule-off)** – mentioning a ground rule takes the typed rule out of force, and typing it again does not put it back;
- **[K1-P2](prerequisites.md#k1-p2-a-rule-generator-cannot-put-a-ground-rule-in-force)** – a rule generator never puts a fully ground rule in force;
- **[L3-P1](prerequisites.md#l3-p1-a-statement-nested-in-a-conclusion-is-explained-as-an-input)** – a statement that a rule's conclusion nests is claimed, but explained as an input;
- **[Z2-P1](prerequisites.md#z2-p1-one-reading-of-a-patterns-objects-at-every-level)** – one reading of a pattern's objects at every level of a statement (the top level is injective, the nested level reads a subset);
- **[Z3-P1](prerequisites.md#z3-p1-after-the-first-negated-condition-every-new-rule-costs-a-full-pass)** – after the first rule with a negated condition, every block that adds a rule takes a full pass;
- **[Z3-P2](prerequisites.md#z3-p2-a-firing-asserts-a-comma-list-it-was-only-handed)** – a comma list that a rule holds by node is entered by a firing;
- **[Y2-P1](prerequisites.md#y2-p1-a-rule-with-several-conclusions-is-applied-by-halves)** – the atomic firing: all consequences or none ([DEC-FIRING](#dec-firing)).

K1-P1 and K1-P2 have one cause – whether a `=>` fact is in force is read off its surroundings, and a hash-consed ground rule and its mention are one node – and one fix, a positive record that a rule was asserted ([Where the logic goes next](../about.md#where-the-logic-goes-next), "A rule in force due to being asserted").

**Why.** Each of them changes the engine's model or its output in places unrelated to ACE, and is too large for a fix round; an attempt at L3-P1 broke pinned tests in three areas and was reverted. The front end does not wait for them: in v1 it lives with each as a refusal by id or a mapping constraint – `:sub/ground-conditional` stays refused and fully ground nested conditionals are flattened (K1-P1, K1-P2); every nested atom is also written at the top level (L3-P1); no comma list obtained elsewhere is placed into a conclusion by node (Z3-P2); Z3-P1 is a cost; Y2-P1 a firing applied by halves when one consequence is refused; Z2-P1 reaches the front end only through native facts with several objects.

**Milestone.** The funded work; K1-P1 and K1-P2 after v1.

### DEC-OPAQUE-QUERY – a query asserts what it creates in an opaque argument {#dec-opaque-query}

**Decision.** A typed query asserts what it creates in an opaque argument, as it does under a mentioned `=>`; nothing in a query's mentions is marked as a pattern. The front end is unaffected, because it asks every question in a scratch cluster that it drops.

**Why.** It keeps one rule for both kinds of opaque position, the built-in `=>` and a declared opaque predicate.

### DEC-RESIDUE – what a query leaves, how names resolve, how forms print {#dec-residue}

**Decision.**

- A query's residue is never data: every typed query runs as query text, and what holds its variables goes once it is answered, in the session and after a load. The variables inside a value set pattern count; a `=>` inside a set stays a container. A query's ground collection literal is residue too, while a set constant stays: a collection has an identity nothing outside the query can name, so asserting it would only add an orphan at every asking.
- Read-only bindings never resolve a string to a variable.
- Every printed form re-enters as the same node: path and neural tag facts print their sugar outside premises, a rule's one-element condition set prints `(*{…} ~ conjunction)`, a set pattern prints `{…}` with every member, a comma list a statement holds prints as a comma list.
- A statement that holds a question about implications, and a `=>` line with variables whose consequences cannot be asserted, are queries that leave nothing.
- The message of `.load` says what happens to auto-run; auto-run still goes off.

**Why.** Repeated questions must not slow a session down by the patterns earlier questions left, and every line zelph prints – the front end's `ace/zelph-of` output included – must be enterable again with the same meaning.

### DEC-DECL-RESIDUE – an implicit relation-type declaration is provisional {#dec-decl-residue}

**Decision.** zelph writes the relation-type declaration `P ~ ->` itself when a node is first used as a predicate. Written inside a query, a cluster or a firing, that declaration is provisional. A query takes it back once it is answered, unless the query itself states the declaration or asserts a fact that uses the predicate; a word the query names for the first time keeps its node and its name, as before. A refused firing takes back what it declared, and a dropped cluster takes back the declaration it wrote for a predicate older than the cluster, unless a fact or statement outside the cluster uses it.

**Why.** A question leaves nothing behind that nobody stated ([DEC-RESIDUE](#dec-residue)), and a drop restores the graph as it was, also on the documented route of a program that asks inside a cluster it then drops. For the front end, the dry run and the scratch cluster of every question leave no declaration behind, and their drops keep the fact stores armed ([DEC-ATOMIC](#dec-atomic)).

**Not taken.** Taking back only what the query routes declare and leaving user clusters as they were: a question that a program, the C API, Rust or SPARQL asks inside a cluster that is then dropped would keep the declaration.

**Milestone.** Engine; built.

### DEC-REFUTE-ROUTES – refutation on every route {#dec-refute-routes}

**Decision.** Every route that refutes – typed, nested, in a comma list, from Janet, through the C API, a firing, whatever binding supplies `~` or `refuted` – obeys the rule of the typed line. A refutation claims its marking and nothing of its fact: a fact it creates is written as a mention and stays unasserted when the refutation is withdrawn, and the probability-0 edge goes with the withdrawal. A refused statement leaves the graph as it was: a typed statement that carries a refutation, or that builds parts, runs in a scratch cluster merged on success and dropped on refusal; a Janet or C call is checked before it writes. The C API has `zelph_refute` (Rust: `Engine::refute`), and `zelph_fact` refuses a ground marking. Recording a contradiction never asserts. A hand-written ground `(F) ~ "rule pattern"` is refused on every route.

**Why.** The front end writes from Janet, and its dry run depends on a refusal leaving nothing behind; a native user must get the same answer whichever way a statement is written.

### DEC-VOCAB – the engine's own vocabulary {#dec-vocab}

**Decision.** `refuted`, `opaque`, `rule pattern`, `closure`, `nn`, `nn-layers`, `one-or-more`, `zero-or-more` and `wikidata:labels-v2` (in `zelph`) belong to no cluster: created first inside one, they and their relation-type declaration stay after its drop. They cannot be renamed, given away or merged. A declaration `P ~ opaque` written inside a scratch cluster still goes with it.

**Why.** A dry run or a question may be the first to create such a node; dropping it would remove a node that later facts refer to. For the front end, its memo of `opaque` stays valid across a question's drop.

### DEC-BOOKKEEPING – what queries see of the engine's bookkeeping {#dec-bookkeeping}

**Decision.** Hidden from queries, rule conditions, closures (the direct walk and the index), `.explain`'s walks, `zelph/exists` and `zelph_exists`: a contradiction record's marking `C ~ refuted`, the memberships of a record no statement names (a statement without a variable that names the set makes its memberships data), and the rule-pattern marks. A user's own refutation of a fact stays visible. Rule text – a condition set's tag `~ conjunction` and its memberships – stays data, so rules over rules can read it, and the prune commands leave it. `zelph/sources`, `zelph/targets`, the plain `zelph/mentioned` and `.node` stay structural: the front end reads the contradiction records through `(zelph/sources "~" "refuted")`. `zelph/exists` asks the condition matcher, so it answers at every level as a ground rule condition does; the plain `zelph/mentioned` stays a structural question, because it asks whether a node is present, which a matcher that skips patterns does not answer. A slice carries a record whole or not at all.

**Why.** Bookkeeping must not answer a question a user asks: before, a record's marking answered `S ~ O` and a mention's pattern mark was visible as a claimed fact.

**Not taken.** A structural `zelph/mentioned` widened to every level (it would need a second matcher mode that reads patterns as candidates).

### DEC-PRUNE-HOLDERS – pruning and what holds a fact {#dec-prune-holders}

**Decision.** Both prune commands remove the statements that hold a pruned fact and keep what a rule is built from (a container a rule names stays when a member that is no rule part leaves it). `.prune-nodes` keeps taking the rules its victims are built from; it is documented as highly destructive, the published networks are cut with it, and the front end does not use it.

### DEC-NAME-MERGE – name merges {#dec-name-merge}

**Decision.** A merge through a name (`.name` onto a name another node holds) is planned and refused before it changes anything – for instance when it would fold a refuted fact into an unrefuted equal one, or make a predicate opaque. It keeps claims, and it carries the cluster journals, the negation records, `(zelph/defeated)` and the refutations to their owners; it runs no inference. A claimed statement merged with a rule's pattern or a mention stays claimed. Which pairs of nodes a merge accepts at all follows content identity ([DEC-MERGE-CONTENT](#dec-merge-content)); the front end never calls `.name` ([DEC-RELOAD](#dec-reload)).

### DEC-BACKLOG – remaining design points of the engine work {#dec-backlog}

**Decision.**

- `[axiom]` keeps the meaning `.help .explain` gives it: an input fact, or a derived one whose rule is gone.
- A nested statement that meets a fact with more objects is read as a subset in every evaluation order, independent of `.anchors` and of the planner; `.explain` and `zelph/dependents` read it the same way. The nested level keeps the subset reading, the top level its injective one; one reading at every level is item Z2-P1 ([DEC-CAT4](#dec-cat4)). The front end writes one object per fact, so it meets the difference only over native data.
- A program's `zelph/query` keeps what it is given; the documentation shows the idiom that leaves nothing, asking inside a cluster that is dropped afterwards ([A question that leaves nothing behind](../janet.md#a-question-that-leaves-nothing-behind)). The front end asks so.
- `centre ~ refuted` on an atom stays a plain statement.
- `zelph/holders` passes a membership of a data set constant through and lists the statements that hold the set; a membership in an atom or `@{}` container that the prune takes along is passed through too, and the documentation says that the container goes along; a membership in a container that stays is listed ([DEC-RETRACT](#dec-retract)).
- A nested refused consequence is reported and the run goes on, as a top-level one is; an atom container that only an opaque fact mentions stays on a prune; a prune keeps a statement that `zelph/mention` wrote.

### DEC-UNBOUND-PRED – an unbound consequence predicate is refused when the rule is written {#dec-unbound-pred}

**Question.** `(X waits nil) => (X W nil)` derived nothing, silently, and `(X waits nil) => (W W nil)` made a witness that stood as a relation type printed `->`, which does not round-trip. What should a rule whose consequence predicate is a variable that no condition binds do?

**Decision.** It is refused when it is written, with a message that says to bind the predicate in a condition or write it as a name. The check sits in the engine's rule builders, not in the parser, so the route the front end uses (Janet thunks into `zelph/dedup-rule`) is covered; one function serves the typed rule, `zelph/dedup-rule`, `zelph/rule`, `zelph/rule*`, `zelph/build-rule` and the C function `zelph_rule` (Rust: `Engine::rule`). "Binds" means that a positive condition names the variable, nested facts and the members of a set it holds included; a negated condition binds nothing. Excepted: a `=>` inside a consequence (a generator's inner rule, a mentioned rule), whose predicate the generator may bind, and a held `=>`, which runs no builder. A `=>` that Janet's `zelph/fact` or C's `zelph_fact` writes runs no builder either; it is checked later ([DEC-FACT-RULE](#dec-fact-rule)).

**Why.** A rule that can never derive what it says, or that derives a malformed node, is a mistake its author should hear about when writing it. For the front end, a rule over relations binds its predicate in a condition ([Native Rules and Meta-Rules](zelph-features.md#native-rules-and-meta-rules)).

**Not taken.** Never deriving such a consequence, silently; anonymous relation types printed `??` (no round trip, and a run would create relation types).

### DEC-FIRING – a firing applies its consequences one by one, for now {#dec-firing}

**Decision.** A refused consequence takes back what it built itself – its nested parts, its fresh witness, the marks its claim revoked, the refutations it applied – and is reported. A firing with several consequences keeps those written before the refused one; those after it are not attempted. The atomic firing (all consequences or none) is category 4, item Y2-P1 ([DEC-CAT4](#dec-cat4)). A test pins neither reading of a refused sibling.

**Why.** The atomic firing changes the output order of derivation lines, the lock scope and the journal across consequences, and the meaning of a contradiction beside other consequences; it also ends a non-termination under `.semi-naive off` and check mode that a refused sibling of a fresh witness causes. That is too much for a fix round. The front end needs the atomic reading – "If … then A and B." with B refused derives neither – so Y2-P1 is one of its prerequisites. Until it lands, v1 lives with the firing applied by halves: the contradiction is always reported, and `(ace/option :atomic :consistent)` keeps a block whose own inference meets one from being written at all ([How v1 lives with it](prerequisites.md#how-v1-lives-with-it)).

**Not taken.** The atomic firing now.

### DEC-SILENT-SKIP – a consequence the binding cannot write stays silently skipped {#dec-silent-skip}

**Decision.** When a firing's binding makes a consequence into a fact whose predicate is one of its objects (`a p a` from `(X p Y) => (Z Y X)`), the firing skips it before writing and prints nothing, as before; it leaves nothing (no nested part, no fresh witness), and the check against duplicate witnesses counts it as present, so such a rule neither grows the graph on every pass nor keeps `.semi-naive off` and check mode from terminating.

**Why.** Reporting it as a refusal would print a contradiction line on every run of such a rule (a refusal by a rule with a single plain condition is not recorded), change output that user scripts may rely on, and leave open how the witness check counts it. No shipped module reaches the skip (an instrumented count: zero in the Jacobian computation and in `.import math` followed by `.run`).

**Not taken.** Reporting it as the refusal a typed statement of that shape gets.

## Three Engine Questions Settled Last

Three questions about zelph itself remained after the engine rounds above. None is a prerequisite of the front end, and each is general engine behaviour; all three are settled for zelph 1.1.0, which is released before the funded work begins. [What zelph Already Provides](foundation.md#tests-that-pin-the-foundation) names the tests that pin them.

### DEC-FACT-RULE – a `=>` written with `zelph/fact` is checked when the outermost evaluation ends {#dec-fact-rule}

**Question.** The rule builders refuse a consequence whose predicate is a variable no condition binds ([DEC-UNBOUND-PRED](#dec-unbound-pred)). A `=>` fact that Janet's `zelph/fact` or the C function `zelph_fact` writes passes no builder, and it cannot be judged when it is written: a later call may still make it the inner rule of a generator, whose predicate the generator binds, or hold it in a statement. Such a `=>` was not checked at all; `(X waits nil) => (W W nil)` written that way was listed as a rule, and its firing made a witness that stood as a relation type. How is it checked?

**Decision.** Later, at fixed points. Each `=>` that `zelph/fact` writes with an unbound consequence predicate is noted and judged at the end of the outermost Janet evaluation – a `%` block, a keyword handler, a script –, before a save and before every run; a `=>` that `zelph_fact` writes is judged before the next save or run. If it is still a rule in force then, it is removed and refused with a message that names the variable. A generator's inner rule and a held rule are mentioned by then and pass.

**Why.** It is sound for every shape the raw route can build, it gives the answer the builders and the typed route give, and it keeps the documented way of writing a rule generator from Janet, `(zelph/fact condition "=>" consequence)`, working ([Rules That Derive Rules](../logic.md#rules-that-derive-rules)). The front end builds every rule with `zelph/dedup-rule` and meets the builder's check instead.

**Not taken.** Refusing at the call (it would refuse a raw generator whose inner predicate only the generator binds, which the documentation shows as correct); leaving the raw route unchecked as the caller's responsibility (the malformed relation type would stay reachable).

**Milestone.** Engine; part of zelph 1.1.0 ([Rules as Data and the Rule Builders](foundation.md#rules-as-data-and-the-rule-builders)).

### DEC-MENTION-RULE – a rule written as a mention is not in force {#dec-mention-rule}

**Question.** `zelph/mention` and the C function `zelph_mention` write a statement without claiming it. A `=>` written that way was nevertheless listed by `.list-rules` and fired, because whether a rule is in force was read off whether something mentions the `=>`, and nothing did. Is such a `=>` a rule in force?

**Decision.** No. The root `=>` of a mention carries the mark that a rule's ground pattern carries, the one mark kind of [DEC-MODAL](#dec-modal): it is not listed, it does not fire, and a rule over rules does not read it, until something claims it. This holds on every route, a firing that writes into an opaque argument included; the rule builders remove the mark from what they keep. Claiming it into force subjects it to the check a typed rule meets ([DEC-UNBOUND-PRED](#dec-unbound-pred)), on the raw route at the points of [DEC-FACT-RULE](#dec-fact-rule); a refused claim leaves it a mention. The front end writes the rule of a that-clause this way, inside the opaque statement that holds it ("John believes that every man waits.").

**Why.** A mention is written down, not asserted, as zelph's C header and Janet documentation describe it, and a rule that fires is asserted. The decision fits the planned record that a rule was asserted (K1-P1, [DEC-CAT4](#dec-cat4)), under which a rule is in force only where it was asserted.

**Milestone.** Engine; part of zelph 1.1.0 ([Statements About Statements](foundation.md#statements-about-statements-opaque-predicates)). Used by the front end from M8.

### DEC-MERGE-CONTENT – name merges follow content identity {#dec-merge-content}

**Question.** A name merge (`.name` onto a name another node holds) of two statements with different subjects or predicates, or of a statement and an atom, made one node that held both subjects, both predicates and both objects: after `c q d` and `a p b` were merged, the survivor also answered `a q b` and `c p d`, and kept an id that its content does not hash to. What should such a merge yield?

**Decision.** What one expects of terms that are identified by their content. A statement or a set constant is identified by what it holds, any other node by its id. Atoms merge. Two ground statements with the same subject and predicate merge into the one statement that holds the objects of both. A statement or a set constant merged with an atom survives as itself under its own id, the atom's facts are rebuilt over it, and it then reads as a definition of the atom; a set constant refuses an atom that holds a member the set lacks. Every other merge of statements or set constants is refused before anything changes: a different subject or predicate, one being part of the other, a rule's variable inside, a core node, a statement with a set constant, and two set constants, since a set is its members. The planning and the carried state of [DEC-NAME-MERGE](#dec-name-merge) stay as they are.

**Why.** A merge must not create facts nobody stated (the mixed answers above), and a statement's id must stay the hash of its content, so that its printed form re-enters as itself ([DEC-RESIDUE](#dec-residue)). Refusing to merge two set constants reverses an earlier rule that merged them into their union: two different sets are not one set. The front end is unaffected: it never calls `.name` and binds words through the lexicon ([DEC-RELOAD](#dec-reload), [DEC-LEX-SYNTAX](#dec-lex-syntax)).

**Not taken.** Keeping the node with the parts of both (it answers facts nobody stated).

**Milestone.** Engine; part of zelph 1.1.0 ([Engine Behaviour Made Precise Along the Way](foundation.md#engine-behaviour-made-precise-along-the-way)).

## Decided While This Specification Was Written

Writing these pages met a few points where two parts of the specification disagreed, or where an old measurement no longer held on the current engine. They were settled as follows; where an older statement elsewhere differs, these win.

### DEC-DOCS-PLACE – where these pages live {#dec-docs-place}

**Question.** Where in zelph's documentation do these pages live, beside the user page `ace.md` that M1 writes?

**Decision.** The specification pages live in `mkdocs/docs/ace-spec/`, in the navigation section "ACE Front End (Specification)". The user page of the front end, written from M1 on ([The User Page](plan.md#the-user-page)), keeps its planned name `ace.md` and its anchors (`ace.md#ace-<id>` for a message, `ace.md#c-<namespace>-<name>` for a construct).

**Why.** A directory `ace/` beside `ace.md` would build the same page twice (`ace/index.html`), and messages and construct rows link to the user page's anchors.

**Milestone.** None (a documentation decision).

### DEC-ONE-CONDITION-PRINT – how `ace/zelph-of` prints a one-condition rule {#dec-one-condition-print}

**Question.** zelph reads several spellings of a rule with one condition. Which one does `ace/zelph-of` print?

**Decision.** `ace/zelph-of` prints a rule with one condition exactly as zelph lists the rule the front end builds, the one-element condition set: "Every man is mortal." is `(*{(X ~ man)} ~ conjunction) => (X is mortal)`. A rule with several conditions prints as the comma list `((A), (B)) => …`, which is a condition set too.

**Why.** On a graph that lacks the rule, only this form re-enters as the same, recorded rule ([DEC-RECORD-SINGLE](#dec-record-single); [RT-3: Native Equivalence of the Shown zelph](tests.md#rt-3-native-equivalence-of-the-shown-zelph)). Typed natively there, `((X ~ man)) => …` and `(X ~ man) => …` both build the plain rule, whose contradictions are recorded only when it derives the refutation of a claimed fact – the parentheses only group – so neither is ever `ace/zelph-of` output. On a graph that already holds the rule in force, in either form, all three spellings meet it and add nothing.

**Milestone.** M1 (`ace/zelph-of`); rules from M4.

### DEC-VAR-LETTERS – variable letters and the order of atoms {#dec-var-letters}

**Question.** Which names do the variables of a rule or a query get when `ace/zelph-of` prints it, and in which order do its atoms stand?

**Decision.** Variables are named per rule or query in the order of their first occurrence, conditions left to right, then conclusions: `X`, `Y`, `Z`, then `A` to `W`, then `_v27`, `_v28`, …; a generator's inner rule shares the outer rule's names. A class pattern takes a letter only where it has a variable of its own: on a graph with Wikidata, "a man" in a condition is `X P31 Y, ((Y P279 Q8441) closure zero-or-more)`, and Y walks up the subclass hierarchy; in the core profile the class atom `X ~ man` has no such variable and takes no letter. The atoms stand in the order of the sentence: a subject's class atom before its verb, a relation before the class atoms of the referents it introduces, an event's occurrence atom before its modifiers. So "Every man owns a dog." is `(*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog)`, and "If a man waits then a dog barks." is `((X ~ man), (X waits nil)) => (Y ~ dog) (Y barks nil)`.

**Why.** zelph's native variable shape is a single capital letter or a name starting with `_` (natively `X1` is a constant), and names must be distinct within a rule or a query, because answers are keyed by them. Letters that follow the variables a rule actually has, in the order of the sentence, make the printed rule read like the sentence it comes from. One function builds every class pattern in both profiles ([DEC-PATHS](#dec-paths)); only the Wikidata form has the extra variable.

**Not taken.** A letter for every class pattern in both profiles, so that the letters of a rule would not depend on the profile (every core-profile rule would skip a letter for a variable it does not have).

**Milestone.** M1, with the first rules at M4 ([Variables](api.md#variables)).

### DEC-NUMERIC-REFERENT – a number gets no class pattern {#dec-numeric-referent}

**Question.** A relational noun gives the referent it introduces a class atom ([DEC-OF](#dec-of)). Does a referent that a formula makes numeric get a class pattern in a rule condition?

**Decision.** A referent that the typing of formulas ([Typing](math.md#typing)) makes numeric gets no class pattern from the relational noun that introduces it: "If a person's age is A and A > 17 then the person is an adult." has no condition `A ~ age`, only the relation and the comparison ([Formulas in Rules](math.md#formulas-in-rules)).

**Why.** A number is not an instance of "age": "John's age is 30." writes `John age 30` and no class atom on the value ([DEC-OF](#dec-of)), so a class condition on it would never match. These are the shapes the arithmetic design verified on today's zelph.

**Milestone.** M5.

### DEC-YESNO-WIDER – yes/no questions read facts with several objects {#dec-yesno-wider}

**Question.** The front end writes one object per fact and checks the atom of a yes/no question with the exact probe. What does it answer when a native or imported fact holds several objects, such as `John sees Mary Bill`?

**Decision.** A yes/no question over a ground atom checks it with `(zelph/exists s p o :exact)` first, in constant time. On a miss, while the graph may hold facts with several objects, `ace/ask` asks the plain `(zelph/exists s p o)`, which reads a wider fact as a rule condition does. So a native fact `John sees Mary Bill` answers "Does John see Mary?" with "Yes.".

This second probe is the one named exception to the rule that the front end never calls `zelph/exists` without `:exact` (item 7 of the [Invariants](api.md#invariants)): it is taken only in `ace/ask`, only after the exact probe missed, and its cost is bounded by the subject's adjacency for the predicate, with no scan of the graph. The source-rule test `ace work: the ace sources make no forbidden graph call` admits exactly this call ([Work on a Large Graph](tests.md#work-on-a-large-graph)), the test `ace questions: a native fact with several objects answers a single-member yes/no question` pins the answer ([Questions](tests.md#questions)), and the probe is timed on the `-small` network as a row of [The Budget Table](plan.md#the-budget-table).

**Why.** The front end writes one object per fact, but native and imported data need not; a question must answer what a rule over the same data concludes ([Facts with Several Objects](answers.md#facts-with-several-objects)).

**Not taken.** The exact probe alone: "Does John see Mary?" would be "not provable" while the wh-question "Who does John see?" over the same fact finds Mary.

**Milestone.** M1 (the probe is part of `ace/ask` from the first yes/no questions); its test and its timing at M3, which brings the transitive verbs the test needs.

### DEC-DISCONNECTED – a conclusion unconnected to the condition {#dec-disconnected}

**Question.** "If a man waits then a dog barks." concludes something about a dog that nothing in the condition names. Read logically, it says ∀x (man(x) ∧ waits(x) → ∃d (dog(d) ∧ barks(d))): one dog suffices for every waiting man. The construct `:rule/disconnected-head` was to be refused, because such a rule had been measured to fire never or on every run. Is it refused?

**Decision.** No. It is supported from M4 as the rule `((X ~ man), (X waits nil)) => (Y ~ dog) (Y barks nil)`. The test `ace rules: a conclusion unconnected to its condition makes one witness for every binding and run` pins one witness across bindings and across runs, and the earlier refusal test of unsafe, disconnected and endlessly generating rules keeps the other two ids as `ace rules: unsafe and endlessly generating rules are refused by id` ([Layer C](tests.md#layer-c-hand-written-with-the-answers-a-user-sees), M4). On today's zelph the rule makes exactly one witness for any number of bindings, in every semi-naive mode:

```
zelph> .lang ace
ace> ((X ~ man), (X waits nil)) => (Y ~ dog) (Y barks nil)
ace> John ~ man
ace> John waits nil
ace+> Bill ~ man
ace> Bill waits nil
ace> Sue ~ man
ace> Sue waits nil
ace> X barks nil
Answer: dog barks nil
```

Three men wait, and one dog barks. The run that follows "John waits" derives the dog; its lines are not printed, because the witness is no node of the session's own input, and the `+` in the prompt marks that a run hid derivations. Later bindings find the dog already there. The witness has no name; zelph prints it by its class.

**Why.** The old measurement was taken on an older engine; the current one, with the duplicate-witness guard of [DEC-GENERATIVE](#dec-generative), gives the logical reading. With this route, 57 of the 83 construct ids that decide refusals (76 DRS-level and 7 context ids) are supported by M9 and 26 have no route in v1 ([Construct Ids by Milestone](constructs.md#construct-ids-by-milestone)).

**Milestone.** M4.

### Smaller Points

- Specified sessions agree with zelph at its default settings: `[ace/contradiction-derived]` arises from a block's own inference ("John is a man. John waits. No man waits."), while a block that contradicts a fact an earlier inference already refuted is refused as `[ace/contradiction]` ([Contradictions and Defeated Conclusions](block.md#contradictions-and-defeated-conclusions)).
- Error sessions show the host's frame `Error in line "ace …": …`, which ends a piped session. A message's position follows the rule of [DEC-ERRORS](#dec-errors), and the examples that disagreed were corrected to it: "John can fly." is marked at "fly" (column 10), "John's age is 30." at "is" (column 12), a context id at its sentence's first token. Warnings and notes collected before an error follow it, each in its own layout, indented ([Layout](block.md#layout)). The wording of a message is illustrative unless the specification fixes it verbatim ([Positions and the Marker](block.md#positions-and-the-marker)).
- "No man waits." is the rule `(*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)` ([DEC-REFUTED](#dec-refuted)), never a denial; a denial is the form of the "There is no …" shapes.
- Sessions show the prompt `zelph+>` after `.import ace-math`, as after `.import math`, and `zelph>` again after the next block; the Wikidata-style fixture is loaded from a temporary copy, `.load /tmp/ace-fixture/ace-fixture.json` ([Loading It in a Test and in the Documentation](tests.md#loading-it-in-a-test-and-in-the-documentation)).
- Published pages quote no sentence of APE's corpus: they use own sentences of the same shape and cite a corpus case by its id ([DEC-CORPUS](#dec-corpus)).
- Gate A's pinned list follows the test specification: of the 19 corpus rejections that APE accepts in guessing mode, seven are accepted by design and twelve are rejected with a named id ([The Rejections Guessing Accepts](tests.md#the-rejections-guessing-accepts)).
- The round-trip oracle RT-1 (answers re-enter) exempts the answers to a universal question: their `Yes.` and `No.` lines are a rule or a negated universal, which re-enter as such ([RT-1: Answers Re-Enter](tests.md#rt-1-answers-re-enter)).
- Provenance lives in node annotations from M1 on ([DEC-PROV](#dec-prov)).
- The two tutorial sessions of the [Overview](index.md#two-sessions) are pinned by tests: the first ("Every man is mortal. …") at M4, the second (Douglas Adams on the Wikidata-style fixture) at M3 ([Tutorial Pins](tests.md#tutorial-pins)).
- With a prototype of the evaluation module, `.import ace-math` measured 6135 nodes and 456 rules ([DEC-MATH-IMPORT](#dec-math-import)). The install tier's allowance is computed from the suite's listings at M0's start.
- K1-P1 needs no migration of older `.bin` networks ([K1-P1](prerequisites.md#k1-p1-a-mention-switches-a-typed-ground-rule-off)).
- The switch example of [DEC-NESTED-IMP](#dec-nested-imp) is "If John waits then every man sleeps." everywhere ([A11](routes.md#a11-nested-conditionals-a-generator-for-a-switch-or-a-then-box-referent-flattened-otherwise)).
- The front end never uses `zelph/rule`: it looks for no equal rule and returns the condition set rather than the `=>` node. Every rule goes through `zelph/dedup-rule`.

## Nested Conditions: The Outcome

ACE lets a condition hold almost anything: another conditional, a negation, a question, a formula, a statement about a statement. The table names, form by form, the decisions that decide it and what they make of it. The complete table, with more forms and the construct id of each, is [All Senses at a Glance](zelph-features.md#all-senses-at-a-glance); the refusals among them are explained in [Shapes zelph Cannot Yet Evaluate Soundly](constructs.md#shapes-zelph-cannot-yet-evaluate-soundly).

| Form | Example | Decisions | Outcome | Milestone |
|---|---|---|---|---|
| conditional in a conclusion, ground outer condition | "If John waits then every man sleeps." | DEC-NESTED-IMP | a rule generator (a switch) | M4 |
| conditional in a conclusion over a referent the conclusion introduces | "Every man owns a dog that every cat hates." | DEC-NESTED-IMP | a rule generator; its inner rule shares the outer rule's witness | M4 |
| conditional in a conclusion, outer referent | "Every man feeds every dog." | DEC-NESTED-IMP | one flattened rule | M4 |
| one-atom classical negation in a condition | "If John does not wait then Mary sleeps." | DEC-NEG | a `((F) ~ refuted)` condition | M4 |
| complex NAF in a condition | "If a man does not provably own a car then the man walks." | DEC-NEG | a positive helper `ace:naf-<h>` plus `¬` | M4 |
| universal in a condition; NAF in NAF; NAF chains | "If every dog sleeps then a cat runs."; the bachelor/husband pair | DEC-STRATA | refused by id (`:rule/in-body`, `:naf/nested-in-body`, `:naf/unstratified`); the narrowing that would accept the orderable ones in the core profile is planned, not v1 | – |
| a class hierarchy that feeds a NAF conclusion back into what a NAF rule tests | "… Every husband is a man." beside the bachelor/husband pair | DEC-STRATA | refused (`:naf/unstratified`, by the may-derive graph and by the engine's verdict) | M4 (the refusal) |
| complex classical negation in a condition | "If John does not own a red car then Mary waits." | DEC-NEG | refused, hint "does not provably" | – |
| conditional inside a yes/no question | "Is it true that every dog is an animal?" | DEC-QUNIV | guarded hypothetical check | M4 |
| class atom over subclass data in a condition | "If a writer is not provably a human then …" | DEC-PATHS, DEC-NEG | instance predicate plus subclass closure; a positive helper under NAF | M4 |
| formula in a condition | "If a person's age is A and A > 17 then …" | DEC-MATH-MODULE, DEC-MATH-LANG | a seed rule plus a `compare` condition | M5 |
| event in a condition | "If a man waits in a park then the man is a visitor." | DEC-EVENTS | `(Y ace:occurrence (X waits nil)), (Y "is in" Z), (Z ~ park)` among the conditions, matched by deep unification | M6 |
| cardinality in a condition | "If John owns at most 1 car then John is modest." | DEC-PLURALS, DEC-NEQ | a positive helper with `!=` over named constants, then `¬` | M7 |
| statement about a statement in a condition | "If John believes that Mary sleeps then John waits." | DEC-MODAL | `believes` opaque, matched by deep unification; a box with several conditions or a referent is a set constant of mentions, matched by its members (P-set-match) | M8 |
| conditional inside a that-clause | "John believes that if a man waits then the man sleeps." | DEC-MODAL | a mention at the top level; refused by id in a condition (`:sub/conditional-with-referents-in-body`) and in a conclusion (`:sub/conditional-with-referents-in-head`); a ground conditional there stays refused (`:sub/ground-conditional`; lifting it needs the record that a rule was asserted, DEC-CAT4) | M8 |

## Engine Facts the Design Relies On

Every decision above rests on facts about zelph that were established by running it, not by reading code. The list states them as they hold for zelph 1.1.0 with the engine work of [DEC-HOSTFIX](#dec-hostfix). Each keeps the id under which the specification tracks it (S1 to S89), so that an implementing session can find the evidence; ids of facts that are no longer true – defects fixed in the meantime – are left out. A specification statement that contradicts one of these facts is wrong and is corrected.

### Architecture and Process

- APE is not used at run time: a keyword handler runs outside Janet's event loop, so waiting for a process, reading a pipe and `ev/*` fail there. The grammar and the resolver are written clean-room from the five public ACE 6.7 documents; APE's corpus and APE itself serve only at development time (S1).
- One `ace` block is one APE paragraph: variables and anaphora never cross blocks, sentence ids restart per block, proper names are global nodes (S2).
- The construct table is internal and normative, and v1 accepts ACE 6.7 and no extension of it; rows own the inventory, the grammar's rule builders own the structure – the grammar is not generated from rows, because under committed choice the order of grammar fragments matters (S3).
- `keyword-kit.zph` is a module of its own without ACE knowledge; `sparql` adopts it as a separate change (S4).
- All modules share one Janet environment: names are module-prefixed, and there is no top-level `var` outside `ace/state`, because a redefined top-level `var` splits silently from the functions compiled against it (S5).
- No feature detection: a binding lands in C++ before the front-end code that calls it, and the only interim for a pending engine change is a refusal by construct id (S6).
- Janet compiles a function body when it is defined, so modules compiled before `ace-math` reach it only through a hook table (S7).

### The Keyword and the Block

- A block that masks to whitespace is complete; a whitespace-only line ends a block; `%` lines and `.` commands inside a block are ACE text, never executed, except a line that is exactly `.quit` in piped or typed input, which dispatches the block so far and ends the session (S8).
- `#` opens a line comment wherever it starts and is masked by the veto; the host hands `#` lines and the rest of the keyword line to the handler verbatim (S9).
- After a veto, a second blank line forces dispatch with a text ending in two newlines; at the end of input inside an open region the host reports `Keyword block for 'ace' is incomplete` (S10).
- The keyword is `ace`; `"ace" ~ card` escapes it and names the same node; while the keyword is registered that node prints quoted, so the printed line re-enters; a forgotten `.import ace` silently makes a native fact (S11).
- `.import` reads only `.zph` and `.janet` files (S12).
- The front end raises strings at every boundary, so an error keeps its text on every route (S13).
- A network-file load replaces the graph and takes along clusters, the input focus, the delta record, the negation records, the running Janet block's variables, the C API's variables, display registrations, compiled nets and the import registry; it keeps the Janet environment, the registered keywords, `.lang`, the settings and the `.negation-records` switch, and disables auto-run. A failure in the header leaves the graph untouched, a failure after it leaves it discarded (S14, S83).

### Claims, Clusters and Inference

- The rule idiom: one `=>` fact carrying all heads, built inside `zelph/dedup-rule`, every `zelph/var` created inside the thunk, a one-condition rule as a one-element tagged condition set. Inside a rule-building thunk, queries, cluster switches, imports, loads, saves, runs and nested rule builds are refused; a failed thunk rolls back. Every graph write is main-thread only (S15).
- Rule and query variables have the native shape, a single capital letter or a leading `_`; natively `X1` is a constant. Every `zelph/var` call makes a new variable whatever its name (S16, S17).
- A fact that names a rule node turns the rule off; provenance never does that. A natively typed mention of a rule with variables is a separate node and leaves the typed rule in force; a fully ground rule asserted and mentioned is one node when its condition is a plain single statement (S18).
- A statement claims itself and every fact nested in it outside a `=>`; nothing within a `=>` is claimed by writing it; `zelph/mention` writes a statement unclaimed, and a `=>` written so is not in force until something claims it ([DEC-MENTION-RULE](#dec-mention-rule)); a keyword handler's and the C API's writes claim as a `%` block does; a refutation claims its marking and nothing of its fact (S19).
- Questions run in a scratch cluster that is dropped; the caller's cluster is restored with `(zelph/cluster prev)`, never a literal nil; `zelph/exists` creates nothing (S20).
- A dropped cluster removes its nodes, restores revoked pattern marks (recorded in no cluster) and takes back the index entry and the weight of every refutation made inside it; session and reload agree (S21). Engine refusals during a dry run arrive as catchable Janet errors, and the drop leaves the graph unchanged (S22).
- The drop of a cluster a run derived into invalidates the delta record, and the next incremental run is a full pass with a note saying so; a write-only drop, and a rule built in a dropped cluster, keep the run incremental (S23). `run-delta` falls back to a full pass whenever the graph is not known to be at a fixpoint; module imports suspend auto-run and run once; `(zelph/auto-run?)` reads `.auto-run`; a new rule keeps the run incremental while no rule in force has a negated condition (S24).
- Rule generators built in `zelph/dedup-rule` fire, deduplicate and re-enter natively with no new node, and make one rule per outer binding; a generator whose inner rule is fully ground never fires; typed natively, it also takes an identical ground rule typed earlier out of force, while that rule stays in force when either of the two is built from Janet (item K1-P2) (S27).
- A contradiction derived through a rule whose single condition is not in a condition set is never recorded and is announced again at every run, except a derived refutation that meets a claimed fact, which is recorded with the condition as itself ([DEC-REFUTED](#dec-refuted)); a one-element condition set is recorded. Never recorded either: rules whose conditions are all negations, guards or paths, and instances that cannot be named. A record is keyed on the instantiated conditions only (a derived refutation's also holds the claimed fact), so one record can mask a second contradiction over the same facts (S28). A refusal by a rule whose conditions form a condition set (one member suffices) is recorded as a contradiction; a record's marking and memberships answer no query, and `(zelph/sources "~" "refuted")` still lists the records (S75).
- Only a claimed marking refutes; a statement that a rule or a query only writes down can be refuted, and a rule may name a refuted statement; a marking inside a mention or an opaque argument refutes nothing (S29). A marking of a refuted fact is accepted and changes nothing; the focus spelling `*(…) ~ refuted` is refused in rules and queries; a consequence that nests a refuted fact neither aborts the run nor re-asserts the fact (S74).
- Rules with a fresh variable in a self-feeding cycle do not terminate (documented), which `:rule/endless-generation` keeps out (S30).
- Modifiers written on the nested fact would merge events; occurrence individuals keep them apart (S31).
- "At most n" is a denial over pairwise `!=` witnesses, at a cost of k^(n+1) matches; set constants re-enter with no new node, collection literals do not; a fact with several objects answers single-object questions, but separate facts do not make the multi-object fact true (S32).
- Native meta-rules and neural rules fire on ACE-shaped facts; `.explain` works on facts built from Janet (S33).
- Janet v1.42.1 with one carried upstream hunk; imports from Janet and from inside a keyword handler work (S35, S78).
- A statement that mentions a rule keeps the incremental run incremental (S86); dropping a cluster that only added a member to an older collection leaves that collection as it was (S87).
- Every query is one condition list: its ground positive parts are asserted, its operands are patterns, and a typed query leaves nothing that holds its variables and no relation-type declaration it wrote ([DEC-DECL-RESIDUE](#dec-decl-residue)) (S89).

### Negation

- Rules with a negated condition are ordered into negation levels by pattern key: a top-level pattern by its predicate and constant object; a variable or structured object, a nested statement and a path condition by its predicate with any object; every key by its predicate alone while a rule over predicates (a transitive, symmetric or reflexive meta-rule) is in force. `(zelph/strata)` returns `{:rule :level :component :stratifiable}` per rule in force, plus `:transparent true` for a rule over predicates; it sees rules written inside a scratch cluster, and dropping the cluster removes them; a generator's inner rule is analysed only once it is written (S72, S80).
- A component the analysis cannot order runs on one level by alternation; its model is right in every order when no NAF rule of the component feeds a NAF pattern of the component, and wrong in some or all orders otherwise. A theory the analysis calls stratifiable gives its stratified model in one run, whatever the order of rules and facts (0 wrong of 3,912 runs). `.strata` also calls some sound programs "Not stratifiable", so its verdict is no refusal criterion on its own. `¬(A, B)` is refused (S25, S72).
- NAF conclusions are final: a later fact does not retract one, in any mode. Check mode reports the stale fact and fails the run; `.negation-records on` prints a warning outside check mode; both fill `(zelph/defeated)`, which holds the latest run's list; a defeat is reported once per fact and rule; `.run-once` never re-tests; a load drops the records and keeps the switch; `.new` resets it to off (S73, S81).
- The test binary starts every engine in `.semi-naive check`: each run does about twice the matching, a later fact that defeats a NAF conclusion raises, and a contradiction without a record is announced twice per run; a test that needs another mode declares `.semi-naive on` with the reason (S70).

### Names and Data

- Per language a name names at most one node and a node has at most one name; one string may name different nodes in different languages; a Wikidata label several items share keeps a name entry on every carrier while the forward map points at one, the ranked holder (S61). `zelph/resolve` looks in one language only and creates there; `zelph/lookup`, `zelph/name … :strict` and `zelph/lang` read without creating; a module's `.lang` ends with the module (S62).
- `.name` onto a name that another node holds in the same language merges the two nodes, also under `.lang ace`; a core spelling is never given away; which pairs merge at all follows content identity ([DEC-MERGE-CONTENT](#dec-merge-content)) (S84).
- Internal vocabulary resolves in `zelph` (under `.lang en`, `refuted` names another node and a rule over it never fires); the core names `~ => ! != -> in nil cons conjunction negation` are the same node in every language, and `.name` refuses to give a core spelling to another node (S36).
- A fact built from names resolved in `zelph` and the same fact typed under `.lang en` are two separate facts (S37).
- `"is in"` is an ordinary node; core `in` as a predicate turns its object into a collection; after `.import math` an `of` fact prints as a function application (S38).
- Verb nodes are named by 3rd-person-singular phrases (`likes`, `"lives at"`); after a native `John likes Mary`, a query over `like` has no answer (S39).
- After a Wikidata import P31 is an ordinary node named in `wikidata`, not core `~`, so a `~` class atom matches no Wikidata instance fact (S63). The English labels of the networks published before the importer change are unusable for the front end, until they are regenerated (S64; [DEC-WIKIDATA](#dec-wikidata)).
- A write invalidates only its predicate's index, a question keeps it, and the first fact of a new predicate clears only `~`'s index and its own (S66). A drop of a cluster nothing outside refers to keeps the fact stores armed; a whole-file load and a trusted import disarm them by design (S67). Rules with a path condition, rules with a negated condition, and the rules after a new rule are seeded in an incremental run, with the exceptions of [DEC-NEG](#dec-neg) (S68).
- `zelph/exists … :exact` is constant time also on a miss; the plain form answers what a ground rule condition matches; `zelph/degree` is constant time; `zelph/sources` and `zelph/targets` copy an adjacency set, so the front end calls them only on nodes whose degree the block bounds (S69).
- A bare `Q42` or `P50` is an ACE variable token; ids are written prefixed (`p:Q42`, `n:P50`), and `` `Q42` `` is the literal name "Q42" (S76).
- A dump's `.bin` cache is read only into an empty graph or when the `.bin` itself is named; into a non-empty graph the dump is added without the cache (S82).
- `%j` prints run-time tuples in parentheses, and a struct drops nil fields and prints its keys in hash order (S42). The explicit closure form `((C p N) closure zero-or-more)` parses and answers; the `∗` sugar parses after an unquoted predicate, not after a quoted one (S43).
- `zelph/number` needs an arithmetic substrate, and a session holds one; `zelph/int` needs integer arithmetic; `(zelph/list-chars "")` is Janet nil (S41).

### Arithmetic

- `.import math` leaves 5661 nodes and 418 rules (S44; counted again on the current tree).
- zelph's formula syntax is not embedded in ACE: `$( 2^3^2 )` parses as `2^(3^2)` where ACE gives 64, and `$( -2^2 )` is −4 where ACE gives 4 (S45).
- ACE's `/` is the exact quotient with all sign cases, unknown when inexact or when dividing by zero; natively, natural `/` truncates, and `:simplify` folds ground ℕ/ℤ terms to one value (S46).
- An ACE formula is never asserted as a zelph `=` fact; a stored arithmetic subterm is a request (`john likes (&7 / &2)` derives `(&7 / &2) = &3`), so terms are stored by value, and every arithmetic rule condition gets a seed rule (S47).
- The arithmetic stack is named in `zelph`; under another `.lang`, typed numbers and operators are other nodes (S48; [DEC-MATH-LANG](#dec-math-lang)).
- The math stack has denials of its own, which can fire in the inference of an `ace` block over earlier state without the block causing them (S79).

### Answers and Mapping

- Yes/no answers are three-valued: `Yes. …`, `No. It is false that …`, `It is not provable that …`; the status comes from `zelph/exists` – the exact probe first, then the plain form, which also reads a fact with several objects ([DEC-YESNO-WIDER](#dec-yesno-wider)) – else from a query over `~ refuted` in a scratch cluster; nothing is created in the caller's graph (S49).
- One output line per answer, prefixed `Answer: `; `No answer.` for an empty wh-question; answers are sorted, because unification returns them unordered (S50).
- Disjunction in conclusions and statements is refused (S52).
- Un-saying an earlier block by dropping its cluster is unsound (the drop destroys later blocks' facts on nodes it created) and never offered; rollback within one handler call and what-if checks are sound; clusters are not saved (S53).
- The pre-resolution markers are the front end's own design; DRS referents are symbols, conditions are tuples (S54).

### Tests and the Corpus

- Token ids are never compared: DRS comparison uses sentence ids, error positions use line and column (S55).
- The corpus family counts (R 839, T 578, N 509, P 482, V 379, Q 317, M 221, D 191, U 125, S 89, A 59, C 32, F 19, L 13; 423 basic-only) are reproduced only with one family per construct id and a few evidence rules (S56).
- Deliberate deviations from APE: an unterminated `/*` and unknown characters such as `%` are rejected, where APE drops some silently; the dependent-preposition slot takes only known prepositions; a text without sentences gives an empty DRS and no message, as in APE (S57).
- The corpus: 3779 cases; eval 0 has 3733 (2813 accepted, 920 rejected); APE at the pinned commit gives the expected DRS for exactly the 2682 cases runnable with its small lexicon and rejects all 920 rejections (S58).
- APE's guessing mode guesses only lower-case words, lets the grammar position decide the class, keeps the surface form as the lemma, makes guessed nouns neuter and never guesses prepositions; on eval 0 it accepts 19 corpus rejections (S77).
- The install tier keeps at least 80 % of the test cases fast: a milestone that adds f fast cases may add at most 1.25·F − N + 0.25·f slow ones, with F and N read from the two listings at its start (S59).
- Transcripts: `.timer off` suppresses the elapsed-time line of each input, while `.load` and `.load-partial` still print the time they needed; a script file and piped input stop at their first error (S71).
- The rejected routes of the design, with their evidence, are on [Routes Not Taken](routes.md#routes-not-taken) (S60).

## Outside the Code

Two things the front end depends on are not code and are done by the maintainer.

- **Regenerating and publishing the Wikidata networks.** The networks published so far were imported before the label changes of [DEC-WIKIDATA](#dec-wikidata), so their English labels are unusable for the front end. A regeneration with the current importer keeps every label candidate, and the same run builds the `-ace` network through five new stages of the maintainer's dataset scripts (`ace-scan`, `ace-keep`, `ace-extract`, `ace`, `accept-ace`), whose self-test passes. The regeneration (estimated at about two hours for the `-ace` network alone) and the publication, like the other networks in [Binary Data Files](../binaries.md), have not been run yet. The front end evaluates the regenerated `-ace` network first, then `-small` and `-medium`.
- **Optional importer extensions**, none of them needed by the front end: a filtered import by id list in C++, sitelinks as data, and quantities such as a population (P1082) as data, which would let ACE sentences and `ace-math` talk about populations.

## Housekeeping Decisions

Smaller points that the specification settled once and that the tests rely on:

- **Corpus case 14**, a text that is only comments, is accepted with an empty DRS, as APE does; the rejection gate is therefore the other 919 rejections, and case 14 is an expected acceptance.
- **Layer A′**, APE's stored paraphrases as further inputs, gates 2294 Core ACE and 524 NP ACE cases; cases 97, 595, 1990 and 3547 are known divergences (APE's verbaliser of the time dropped prefixes; 3547 needs a participle the small lexicon lacks).
- **Multi-paragraph corpus texts** are rejected as a whole when any paragraph is rejected (APE returns a partial DRS with the error, corpus 3598).
- **The gates of [DEC-UNKNOWN](#dec-unknown)**: gate S runs the corpus in strict mode; gate A in guessing mode with the corpus lexicon, where the 2682 runnable cases must give the DRSs of strict mode and the rejections that guessing accepts are a pinned list – of APE's 19, seven accepted by design and twelve rejected with a named id, plus 1400 and 1402, which only guessing mode accepts because strict mode never merges names; gate B is a blind run with only the closed word classes and `ace-core` (no silent change of structure); gate N replaces content lemmas by nonce words, an exact oracle; gate W compares an empty graph with the Wikidata-style fixture. The cases, counts and modes are in [The Guessing Gates B, N and W](tests.md#the-guessing-gates-b-n-and-w) and [The Rejections Guessing Accepts](tests.md#the-rejections-guessing-accepts).
- **The counting grammar** is selected by one key, `{:count true}`, which `ace/analyse` passes.
- **`ace/analyse`** is built over the smoke chain at M1 and completed at M2.
- **`:command`** is renamed `:command/imperative`.
- **`ace-drs/unresolved-definite`** stays private.
- **Round trip**: an answer line that names an engine witness (rendered as an indefinite) is checked for acceptance, not for creating no new node; the answers to a universal question are exempt from RT-1 ([Smaller Points](#smaller-points)).
- **The test of a stdlib-name list** is dropped together with that list ([DEC-NAMES](#dec-names)).

## What This Builds on in zelph

The decisions lean on features documented elsewhere in zelph's documentation:

- [Mentioning a Rule Is Not Asserting It](../logic.md#mentioning-a-rule-is-not-asserting-it) and [Mentioning a Statement: Opaque Predicates](../logic.md#mentioning-a-statement-opaque-predicates) – claims, mentions and opaque predicates ([DEC-MODAL](#dec-modal), [DEC-PROV](#dec-prov));
- [Negation as Failure](../logic.md#negation-as-failure) and [Stratified Evaluation](../logic.md#stratified-evaluation) – negation levels, negation records and their limits ([DEC-NEG](#dec-neg), [DEC-STRATA](#dec-strata), [DEC-NAF-RECORDS](#dec-naf-records));
- [Rules That Derive Rules](../logic.md#rules-that-derive-rules) and [Rules That Write Rules](../rule-generators.md) – rule generators ([DEC-NESTED-IMP](#dec-nested-imp));
- [Transitive Path Conditions](../logic.md#transitive-path-conditions) – the subclass closure of class patterns ([DEC-PATHS](#dec-paths));
- [Fresh Variables: Generative Rules](../logic.md#fresh-variables-generative-rules) and [Inequality Constraints](../logic.md#inequality-constraints) – witnesses, counting and unique names ([DEC-PLURALS](#dec-plurals), [DEC-NEQ](#dec-neq), [DEC-DISCONNECTED](#dec-disconnected));
- [Languages & Names](../quickstart.md#languages-names) – naming languages and what `.name` merges ([DEC-CONTENT-NS](#dec-content-ns), [DEC-RELOAD](#dec-reload), [DEC-MERGE-CONTENT](#dec-merge-content));
- [Contradiction Detection](../logic.md#contradiction-detection) and [Node Clusters](../rules.md#node-clusters-transactional-workspaces) – contradictions kept and reported, scratch clusters for the dry run ([DEC-ATOMIC](#dec-atomic));
- [Running quietly](../janet.md#running-quietly), [Reasoning incrementally](../janet.md#reasoning-incrementally), [Negation levels](../janet.md#negation-levels) and [Node annotations](../janet.md#node-annotations) – the Janet side of quiet runs, incremental runs, `(zelph/strata)` and provenance ([DEC-QUIET](#dec-quiet), [DEC-RUN](#dec-run), [DEC-PROV](#dec-prov));
- [Labels that several entities share](../wikidata.md#labels-that-several-entities-share) – the importer's label handling ([DEC-WIKIDATA](#dec-wikidata));
- [Where the logic goes next](../about.md#where-the-logic-goes-next) – the planned engine work that would lift some refusals.
