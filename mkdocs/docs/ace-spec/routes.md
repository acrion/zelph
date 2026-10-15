# Design Routes Taken and Not Taken

Many design questions of the ACE front end have a straight answer: translate the sentence directly, refuse what is hard, keep what is needed wherever it is needed. Some also have an indirect answer. It reaches the same result by a detour, and because of the detour it buys something the straight answer cannot: a test oracle that costs nothing extra, a refusal that cannot drift away from the engine, a feature that a later project can add without reopening the design. These pages call such a detour a *roundabout route*. This page lists every one that was considered, in three groups: the routes the front end takes, with what each buys and costs and what it obliges version 1 to do now (A1 to A17); the later layers those routes leave room for, with the *hooks* version 1 must keep open for them (B1 to B33); and the routes that were tried and rejected, with the evidence (C1 to C18).

The central point of these pages holds here too: zelph already does the reasoning, and the funded work is the mapping between ACE and zelph, written as standard-library `.zph` modules (Janet plus zelph) on top of documented engine features, in the same way `stdlib/sparql.zph` layers SPARQL on the public Janet API. Most routes below are ways of letting the engine do a job that the front end would otherwise do a second time: zelph checks a block before the block is kept, zelph's own rule generators carry nested conditionals, zelph's relations carry complex negation, zelph's opaque predicates carry statements about statements. The engine features these routes rely on were built for the front end beforehand and are released with zelph 1.1.0 ([What zelph Already Provides](foundation.md)); the few that remain are specified in [What zelph Still Needs](prerequisites.md), and where a route waits for one of them, this page says so. Every route rests on facts about the engine that were established by running zelph, not by reading its code; the recordings on this page are runs of the current build, and the last section restates all of those facts in plain words.

## Several Routes in One Session

The session below shows four routes at work. It belongs to milestone M9, the last one ([The Milestones at a Glance](plan.md#the-milestones-at-a-glance)), and runs without `.import ace-math`.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John is a man. John is a farmer. John owns a dog.
    Bill is a man. Bill is a farmer.
    If a man is not provably a farmer who owns a dog then the man is free.

    zelph> %(ace/option :justify true)
    true
    zelph> ace Who is free?

    Answer: Bill is free.
      "If a man is not provably a farmer who owns a dog then the man is free." (block 1, sentence 6)
      "Bill is a man." (block 1, sentence 4)
    zelph> ace Sue is a farmer. John's age is 30.

    Error in line "ace Sue is a farmer. John's age is 30.": Correct ACE, but zelph cannot use it [ace/needs-math]: sentence 2, line 1, column 29: a number, construct data/integer.
      John's age <>is 30.
      Numbers and arithmetic need the math stack: .import ace-math
    ```

    Piped input stops at the first error; at the interactive prompt the session goes on, and the refused block has written nothing:

    ```
    zelph> ace Is Sue a farmer?

    Answer: It is not provable that Sue is a farmer.
    ```

- The rule in the first block negates a group of conditions: "a farmer who owns a dog". zelph's `¬` takes one fact pattern, not a group. The front end does not refuse the sentence and does not rewrite it by De Morgan's law, which would lose the dog; it gives the group a name of its own, a helper relation that an ordinary zelph rule derives, and negates that ([A12](#a12-complex-conditions-become-named-helper-predicates)).
- With the option `:justify` the answer is followed by the sentences it rests on, quoted as the user typed them, with block and sentence number. They come from records the front end keeps on each node it writes, a store that queries and rules never see ([A7](#a7-provenance-quotes-the-users-sentences)).
- "John's age is 30." is correct ACE, but numbers need the arithmetic stack. The block is refused by the id of the construct (`data/integer`), never with a syntax error and never with a silent, wrong graph ([A2](#a2-one-construct-table-owns-the-inventory-and-every-refusal)). A number has no token of its own, so the marker stands before `is`, the condition that holds it ([Positions and the Marker](block.md#positions-and-the-marker)).
- The refused block is refused whole: "Sue is a farmer." was not written either. A refusal by construct id is decided before the block writes anything. What only the engine can decide, such as a sentence the graph already contradicts, is found by applying the block first inside a scratch area of the graph that is then thrown away; only a block that passes there is applied for real ([A8](#a8-the-engine-is-the-validator)).

## How to Read This Page

Each route has an id. **A** routes shape the architecture of version 1 (v1); each names the decision that took it (the DEC ids of [Design Decisions](decisions.md)) and states what it binds in v1 now. **B** entries are later layers: features outside v1 that a route makes cheap. Each names its **hook**, a property v1 must have so that the layer can be added without redesign; a hook is a requirement on v1, not a feature. **C** entries are rejected routes with the evidence; none is proposed again without new evidence. The facts the routes rest on carry the ids F1 to F23 ([The Facts the Routes Rest On](#the-facts-the-routes-rest-on)).

Where a route's evidence is a short zelph session, the page shows a recording of it on the current build. Those recordings are native zelph: the front end does not exist yet, so they show the engine behaviour a route relies on, written by hand the way the front end would write it. Names that the front end would compute (individuals, helper relations, events) appear with fixed example hashes there. In the specified sessions, the wording of a message is illustrative, while its layout and its id are normative ([Messages](block.md#messages)); an error ends a piped session, so where a session goes on after one, it is shown as typed at the interactive prompt.

| Id | Route | What it buys beyond the straight answer | Decision | Milestone |
|---|---|---|---|---|
| A1 | the DRS, shaped as APE produces it, is the pivot; its test half comes first | the mapper is tested against APE's corpus before a grammar exists; graph-level differential tests | [DEC-DRS-ENTRY](decisions.md#dec-drs-entry), [DEC-ORDER](decisions.md#dec-order) | M0 |
| A2 | one construct table owns the inventory and every refusal | every corpus case has an expectation at every milestone; docs and error catalogue generated | construct table; [DEC-ORDER](decisions.md#dec-order), [DEC-UNKNOWN](decisions.md#dec-unknown) | M0, M2 |
| A3 | shipped vocabulary as Janet data, user entries as graph facts, one resolution step | user words persist and can be derived; base words survive a `.load` and stay out of queries | [DEC-LEX-STORE](decisions.md#dec-lex-store), [DEC-LEX-SYNTAX](decisions.md#dec-lex-syntax), [DEC-BINDINGS](decisions.md#dec-bindings), [DEC-CONTENT-NS](decisions.md#dec-content-ns) | M1 |
| A4 | ACE meaning in the Janet mapper; the resolution step as the seam for mapping rules | a later layer of mapping rules changes one step, not the plan format | [DEC-MEANING](decisions.md#dec-meaning) | M1 |
| A5 | the verbaliser is the mapping's inverse | answers re-enter as input; any fact, also a native one, can be said in ACE | round trip; [DEC-ECHO](decisions.md#dec-echo), [DEC-WIKIDATA](decisions.md#dec-wikidata) | M1, M9 |
| A6 | one re-entry oracle across every input path | differences between Janet construction and the native parser are caught without anyone looking for them | [DEC-INDIVIDUALS](decisions.md#dec-individuals), [DEC-DRS-ENTRY](decisions.md#dec-drs-entry), [DEC-PROV](decisions.md#dec-prov), [DEC-CONTENT-NS](decisions.md#dec-content-ns) | M1 |
| A7 | provenance quotes the user's sentences | explanations independent of verbaliser coverage; text export; survives `.save` | [DEC-PROV](decisions.md#dec-prov) | M1, M9 |
| A8 | the engine is the validator: dry run, then the real run | no second copy of the engine's refusal logic; atomic blocks; what-if checks | [DEC-ATOMIC](decisions.md#dec-atomic), [DEC-DROP-DELTA](decisions.md#dec-drop-delta) | M1; option M4 |
| A9 | of-phrases are relations; the functional reading only for function nouns | the corpus comparison stays simple; relational nouns reach Wikidata properties | [DEC-OF](decisions.md#dec-of), [DEC-NAMES](decisions.md#dec-names) | M3 |
| A10 | the keyword kit as a module of its own | positioned errors for SPARQL and later keyword languages | keyword kit | M1 |
| A11 | nested conditionals: a rule generator for a switch or a then-box referent, flattened otherwise | zelph's rules that write rules reachable from English, at a bounded cost | [DEC-NESTED-IMP](decisions.md#dec-nested-imp) | M4 |
| A12 | complex conditions become named helper predicates | negation over several conditions; the negated group gets a name that answers queries | [DEC-NEG](decisions.md#dec-neg), [DEC-STRATA](decisions.md#dec-strata) | M4 |
| A13 | events only where modifiers need them | facts keep the shape of native and Wikidata triples; where/when/how questions | [DEC-EVENTS](decisions.md#dec-events) | M3, M6 |
| A14 | individuals named by their description | re-entering a text adds nothing; no counters, no probes | [DEC-INDIVIDUALS](decisions.md#dec-individuals) | M3 |
| A15 | words reach Wikidata items through their labels | ACE over Wikidata without declaring words; every choice announced | [DEC-WIKIDATA](decisions.md#dec-wikidata), [DEC-CONTENT-NS](decisions.md#dec-content-ns) | M1 |
| A16 | class atoms over an instance and a subclass predicate, pinned per graph | class questions and rules over the Wikidata hierarchy with nothing to configure | [DEC-PATHS](decisions.md#dec-paths) | M1, M3, M4 |
| A17 | statements about statements are facts of opaque predicates | modality and that-clauses that questions and rule conditions can read | [DEC-MODAL](decisions.md#dec-modal) | M8 |

The forms of nested conditions sit among these routes: a conditional inside a conclusion in A11, complex negation as failure in A12, conditions over modified events in A13, class conditions over subclass data in A16, conditions over statements about statements in A17. [From Sentences to the Graph](semantics.md#every-construct-and-its-native-form) maps each form, and [Nested Conditions in All Their Senses](zelph-features.md#nested-conditions-in-all-their-senses) lists them with their outcome.

## Routes That Shape the Architecture

### A1: The DRS Is the Pivot, and Its Test Half Comes First

**The straight answer.** Translate the parse tree straight into zelph statements.

**The route.** Every text passes through a Discourse Representation Structure (DRS), the logical form ACE's reference implementation APE produces, in exactly APE's shape. `ace/drs-raw` and `ace/refres` build it from the parse (the second step resolves anaphora); `ace/ape->drs` reads the same value from APE's printed notation; `ace/execute-drs!` runs any such DRS through classification, normalisation, planning and execution. The resolved, unnormalised DRS is the point of comparison with APE ([The DRS Is the Pivot](architecture.md#the-drs-is-the-pivot-and-stays-ape-shaped-until-normalization) places it in the pipeline; [The Oracle Functions](tests.md#the-oracle-functions) specifies the reader, the printer and the matcher).

**What it buys.**

- The mapper is tested on the 2813 DRSs that APE's regression corpus expects, before the grammar exists, so parser and mapper fail independently.
- A graph-level differential test: the graph the front end builds from a text against the graph `ace/execute-drs!` builds from APE's DRS of the same text. Both go through the one production mapping, so every representational difference that never reaches the graph is tolerated.
- The expectation for each corpus case and the refusal in the front end come from the same classifier, `ace/drs-constructs`.
- A DRS service for the Attempto ecosystem becomes cheap (B12).

**What it costs.** zelph reproduces APE's conventions: how anaphora are resolved, collective plurals, where referents are introduced, sentence ids (token ids are never compared, because APE's count inserted tokens). The reader of APE's notation is small: a PEG of about 30 lines parsed all 3779 corpus DRS terms (0.069 s, measured on zelph 1.0.1, not re-measured).

**What it binds now.**

1. The DRS value grammar is a faithful image of APE's terms, without shortcuts of its own: `ace/drs->ape` after `ace/ape->drs` is the identity on all 3779 terms up to the names of referents.
2. Order of work: the oracle half of the module `ace-drs` is written at M0, before any grammar code.
3. The reader uses no PEG function captures (no Janet callback per token): it captures tagged groups and converts them in a separate Janet pass, a stability measure.
4. The corpus uses an older encoding of proper names in 538 accepted cases, in which a name is a referent used as an argument. `ace/normalise-ape-names` rewrites it on the expected side ([Proper-Name Normaliser](tests.md#proper-name-normaliser)). Running APE confirmed that the current APE never writes this encoding and that the 499 such cases its small lexicon covers agree after the rewrite; the other 39 need the large lexicon.

**Not in v1.** A public `drs` keyword that accepts APE notation (B12) is a product decision outside v1 (DEC-DRS-ENTRY).

### A2: One Construct Table Owns the Inventory and Every Refusal

**The straight answer.** Write the grammar, and write a refusal message wherever a feature is missing.

**The route.** The rows of `ace/constructs` own the inventory: each construct of ACE has a row with an id such as `:modal/can`, its metadata, examples, verbaliser template and refusal data. The grammar's rule builders own the structure, and every node of the syntax tree a builder makes carries its row id. The grammar is not generated mechanically from the rows (C15). The same rule builders yield four variants of the grammar: plain, counting (it measures its own work), diagnostic (it reports what it expected), and a backtracking interpreter over the rule data, used for every sentence in which a word was guessed from its position (DEC-UNKNOWN; [The Backtracking Variant](grammar.md#the-backtracking-variant)).

The session shows a refusal between M2, when the grammar reads all of ACE, and M8, which brings modality:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace John can fly.

    Error in line "ace John can fly.": Correct ACE, but zelph cannot use it [ace/unsupported]: sentence 1, line 1, column 10: modality ('can'), construct modal/can.
      John can <>fly.
      Modal statements, recorded without being asserted, are planned for milestone M8.
    ```

The message names the triggering word `can`, while the marker stands before `fly`, the first simple condition inside the `can` box, which is the evidence for the construct ([Positions and the Marker](block.md#positions-and-the-marker)).

**What it buys.**

- Valid ACE that zelph cannot use yet is refused by construct id, never with a syntax error and never with a silent graph, so each of the 3733 corpus cases has an expectation at every milestone.
- The reference documentation and the error catalogue are generated from the table, one anchor per id; data-driven tests run every row's `:examples` and `:negatives`.
- Ids are independent of English word order, so a second concrete syntax keys the same rows (B16), and a public extension API can be offered later (B31).
- Because the backtracking variant interprets the rule data, the grammar is reachable as data, which a generator of test sentences needs (B3).

**What it does not buy.** Parser and verbaliser are not kept in step by construction: the builder lives in the grammar module, the verbaliser template in the row. Drift is caught by a round trip, the verbaliser's text read back ([RT-2](tests.md#rt-2-the-verbaliser-reads-back)), run over every supported row's examples once the verbaliser covers the row.

**What it costs.** The table is designed up front. The builders need ordering discipline: the grammar commits to the first alternative that matches, so order matters (a naive coordination rule rejects `John likes Mary and Bill sees Sue.`). A prototype grammar of 30 rules compiled in 0.07 ms (zelph 1.0.1), so recompiling costs nothing.

**What it binds now.** An `:id` names no English word order. Refusal is decided on DRS-level and context ids, by `ace/drs-constructs` and `ace/unsupported`, the same functions the tests use; ids of syntax rules never gate a refusal (DEC-ORDER). [The Construct Table](constructs.md#the-construct-table) lists every id, and [The Refusal Message](constructs.md#the-refusal-message) specifies the message.

### A3: Shipped Vocabulary in Janet, User Entries in the Graph

**Settled before the route.** A pre-pass, `ace/annotate`, tags every token with all its readings; an unknown word gets the open-class readings its form allows (DEC-UNKNOWN), and the grammar matches the tags literally. No lookup creates a node before a block is accepted: `zelph/resolve` creates a node when the name is missing, `zelph/lookup` never does.

**The route.** The shipped vocabulary `ace-core` (prepositions, irregular forms, about 300 person nouns with their gender, the entries ACE requires, the names common in ACE examples) is Janet data and is never written to the graph. `ace-core.zph` is a data module without imports and `ace-lexicon.zph` imports it, so every module that loads the lexicon has the shipped words; an overlay loaded with `:replace` hides them. The vocabulary of `ace-math` (words such as `prime`, `divisor`, `derivative`) is shipped Janet data of that module in the same way. User entries, from `ace-lexicon` blocks, `ace/declare!` or `ace/load-ulex!`, are graph facts on word nodes in the naming language `ace-lex`. An entry's logical symbol may be a Wikidata id, `noun_sg(man, 'Q8441', masc)` or `tv_finsg(follows, 'P155')`: that is how a user binds a word to an item, by word class, with aliases allowed and no node merged. There is no `ace/bind!`. A `.name` onto a name another node already holds does not move the name: zelph merges the two nodes by their content (two atoms become one node; a merge of two statements with different subjects or predicates is refused before anything changes), so `.name` is no binding route, and the front end never calls it ([DEC-MERGE-CONTENT](decisions.md#dec-merge-content), [DEC-NAME-MERGE](decisions.md#dec-name-merge)). Content nodes the front end creates are named in the language `ace`, and the user's `.lang` never changes what the front end resolves ([Naming Languages](words.md#naming-languages); the lexicon itself: [The Lexicon](grammar.md#the-lexicon)).

**What it buys.** User entries persist with `.save`, can be queried, and rules can derive them (B17). The base vocabulary stays out of every query and survives a `.load` that replaces the graph: written into the graph it would leave with the old graph while the Janet state stays. Since a replacing load also clears the import registry, `.import ace` afterwards runs every module again in the same Janet environment, which every module survives. German becomes another naming language over the same concepts (B16). Bindings need no API of their own.

**What it costs.** The annotator reads two stores together. Caches of graph-derived data are keyed by `zelph/epoch`, which `.new`, a replacing load and a dump import advance; the lexicon cache also notices user entries that a dropped cluster removed or a run derived within one epoch, by a fingerprint of the entry counts. User entries appear in unrestricted `S P O` queries. A lookup in the graph cost 2.7 µs against 0.044 µs in a Janet table (zelph 1.0.1), paid only while the cache is rebuilt.

**What it binds now.**

1. The annotated string is a documented interface that the lexicon tests assert on ([The Annotated String](grammar.md#the-annotated-string)).
2. Every content symbol of a plan is resolved in the resolution step of `ace/execute!`, before the dry run, by `ace/concept!` alone, which creates nothing; `ace/concept-node!` creates the node in the dry and real runs ([The Resolution Step](semantics.md#the-resolution-step)). No other function calls `zelph/resolve` or `zelph/lookup` on a content name. Labels (A15), German (B16) and a layer of mapping rules (A4) each change that one step only.
3. The annotator reads both stores only through `ace/readings`, so the store stays swappable (B15, B17).
4. Lemmas never come from the graph: the lemma feeds the DRS and the hashes that name individuals, which must not depend on what is loaded.

**Not taken.** Everything in the graph (DEC-LEX-STORE (a)): a `.load` would take `ace-core` away with the graph. Janet tables only (c): user entries would be lost on `.save` and hidden from rules.

### A4: ACE Meaning Lives in the Mapper; One Resolution Step Is the Seam

**The difference that matters.** A binding resolved at translation time is not retroactive. If a user later binds "located in" to Wikidata's P131, texts entered earlier stay on the old predicate.

**The route not taken, and why it was weighed.** DEC-MEANING (b) would write ACE vocabulary atoms and ship zelph rules that map them onto domain predicates. Re-binding a word would then re-map old texts, a synonym would be one rule, and `.explain` would show the mapping step: the closest fit to "knowledge lives in zelph rules". Its costs decided against it for v1: every atom exists twice; every answer needs one more rule step; every negation-as-failure rule would sit on the vocabulary predicates and share their negation levels (A12); the verbaliser and the oracles would have to know which layer they read. Implications would still be built in Janet, because rule generators cannot create the variables that a DRS of arbitrary shape needs.

**The route taken.** [DEC-MEANING](decisions.md#dec-meaning) (a): the mapper writes domain structure directly.

**What it binds now.** The shape of the plan that `ace/plan` produces is cheap to choose now and expensive once tests pin graph shapes. Name terms `(:name N Role)` are resolved only in the resolution step (A3), so a later switch to mapping rules touches that step and adds a shipped rule module, not the plan format. The rejected variant, the full DRS in the graph with anaphora as rules, is C4.

### A5: The Verbaliser Is the Mapping's Inverse

**The route.** `ace/verbalise` turns a question's DRS plus its query bindings into ACE (M1). `ace/verbalise-node` turns a fact node into ACE: from M4 the fact shapes that contradiction reports and the `[ace/w-naf-final]` warning need, at M9 every fact node and every derivation, including facts that never came from ACE (B10, B11). [The Verbaliser](answers.md#the-verbaliser) has the contract and the templates.

The session pastes an answer back as a statement, in a cluster of its own, which shows that the answer adds nothing to the graph (the block holds a rule, so it needs M4):

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace Every man is mortal. John is a man. Who is mortal?

    Answer: John is mortal.
    zelph> .cluster rt
    Active cluster: rt
    zelph> ace John is mortal.

    zelph> .cluster
    Active cluster: rt
      rt: 0 node(s)
    ```

**Naming.** A node is rendered by its `ace` name; else by the first declared form of a lexicon entry whose symbol is the node's Wikidata id; else by its English label, in a spelling that re-enters (a capitalised run, a hyphenated form, backquotes), when resolving that spelling in the node's role gives the same node back; else by its prefixed id, `p:Q123` for a name, `n:Q123` for a noun, `n:P50` for a relation, `v:P155` for a verb, `a:Q123` for an adjective. A bare `Q123` is never printed: in ACE it is a variable. Two capitalised names are never printed side by side. A node the engine created as a witness is read through Janet, never by its printed name, and an answer renders it as an indefinite ("a car"). [Rendering a Node](answers.md#rendering-a-node) specifies the order.

**What it buys.** Output round-trips ([RT-1](tests.md#rt-1-answers-re-enter)): an answer line, stripped of `Answer:` and a leading `Yes.` or `No.`, entered as an `ace` block is accepted and creates no node, because hash-consing makes the same statement the same node. A line whose re-entry may legitimately write something new, such as one that names a witness or a how-many line, is checked for acceptance only; the answers to a universal question are exempt, because their `Yes.` or `No.` line is a rule or a negated universal, which re-enters as such. APE's corpus stores 2395 paraphrases in Core ACE and 539 in NP ACE whose re-parse APE verified; the front end gates 2294 and 524 of them, the ones APE's small lexicon covers. APE itself round-trips only 2395 of 2813 accepted texts (85 %) and no yes/no question, which is why explanations quote the user's sentences (A7) rather than depend on what the verbaliser covers.

**What it binds now.** Every mapping choice must stay recoverable from the graph's structure plus the lexicon, or `ace/verbalise-node` cannot exist: the fact shapes, events (A13), names of individuals (A14), of-relations (A9), predicate names, class patterns (A16), opaque facts (A17). Concretely, `ace/predicate-name` SHOULD have an inverse over the declared lexicon (a predicate name back to its kind and lemma), pinned by a test over the shipped vocabulary, and a declaration that breaks it warns with `[ace/w-lexicon-conflict]`. The other inverse, from a logical symbol to its first declared form (`pn_sg('Acme', 'Acme_Corporation', neutr)` answers with "Acme"), is part of the contract of `ace/inflect`.

### A6: One Re-entry Oracle Across Every Path

**The route.** Each hand-written test case runs through every input path that exists at the time, each in its own scratch cluster, and every path must build the same graph ([The Paths](tests.md#the-paths) owns the list):

| Path | Input |
|---|---|
| p1 | the `ace` block (the reference) |
| p2 | native re-entry, under `.lang ace`, of the printed form of every node the block built (`:built`); the `ace/zelph-of` lines are checked the same way by [RT-3](tests.md#rt-3-native-equivalence-of-the-shown-zelph) |
| p3 | `ace/execute-drs!` on the DRS of the text, printed in APE notation and read back |
| p4 | `ace/execute-drs!` on APE's expected DRS (corpus cases), after `ace/normalise-ape-names` |
| p5 | the verbaliser's text of the DRS, entered as an `ace` block (RT-2); the answers re-enter by RT-1 |
| p6 | the row's `:zelph` field, the native twin the documentation shows, typed under `.lang ace` |

"The same graph" ([G-1 and G-2](tests.md#g-1-and-g-2-the-same-graph)) means two things: the second path's cluster gains no node (an ACE path after a native one adds exactly its bookkeeping, the profile pin of A16 and the marks of A14), **and** `zelph/exists` is false for every ground rule pattern the text does not assert, because node counts cannot see whether a fact is claimed (F4).

**What it buys.** Janet construction that diverges from the native parser is caught without anyone looking for it. Examples found this way: a variable named `X1` is a constant to the native parser, so a printed rule with it re-enters as a different rule (F1); on an older tree `zelph/rule` wrote one rule per consequence where the typed rule is one rule with a shared witness (F4; the engine now writes one `=>`, and the front end builds every rule with `zelph/dedup-rule`, which deduplicates and returns the `=>` node, where `zelph/rule` does neither). The same oracle confirms the one-element condition set of DEC-RECORD-SINGLE: the front end's one-condition rule, printed as zelph lists it, re-enters as the same rule. Vocabulary splits between ACE and native input are caught as well: in a prototype that named verbs by their lemma, after a native `John likes Mary` the ACE question `Who likes Mary?` answered `No answer.`, which is why predicate names are third-person-singular phrases.

**What it costs.** A C++ test helper that feeds printed lines through the interactive reader, as the SPARQL tests already do, and a count of nodes per cluster. Paths are switched on as their stages exist.

**One language per typed line (F20).** A native line is read in one naming language. The front end names content in `ace` and its internal vocabulary in `zelph` (DEC-CONTENT-NS), so a line that names only content re-enters with no new node under `.lang ace`, while a line that also names internal vocabulary creates that name anew there. Under the default `.lang zelph` the content names are created anew instead. The recording builds a fact and a helper atom from Janet, as the front end would, and re-enters both natively, each in a cluster of its own; `(zelph/clusters)` then lists how many nodes each cluster gained:

```
zelph> %(def j (zelph/resolve "John" "ace"))
<zelph/node John>
zelph> %(def m (zelph/resolve "man" "ace"))
<zelph/node man>
zelph> %(def h (zelph/resolve "ace:naf-0123456789abcdef" "zelph"))
<zelph/node ace:naf-0123456789abcdef>
zelph> %(zelph/fact j "~" m)
<zelph/node John ~ man>
zelph> %(zelph/fact j h "nil")
<zelph/node John ace:naf-0123456789abcdef nil>
zelph> .cluster z
Active cluster: z
zelph> John ~ man
zelph> .lang ace
ace> .cluster rt
Active cluster: rt
ace> John ~ man
ace> .cluster rt2
Active cluster: rt2
ace> John ace:naf-0123456789abcdef nil
ace> .cluster default
Active cluster: default
ace> %(zelph/clusters)
@[("rt" 0) ("rt2" 3) ("z" 3)]
ace> %(= h (zelph/resolve "ace:naf-0123456789abcdef" "ace"))
false
```

The content line re-enters with 0 nodes under `.lang ace` (cluster `rt`) and creates 3 under `.lang zelph` (cluster `z`); the line with the internal name `ace:naf-…` creates 3 under `.lang ace` (cluster `rt2`), and the name it creates there is a different node from the one in `zelph`. Such a line passes p2 in neither language today, so p2 and RT-3 exempt lines that name internal vocabulary (helper rules, refutation markings, events); B33 is the host change that would let every line pass. On a Wikidata graph `ace/zelph-of` adds the resolved nodes as display only; the re-entry contract holds without Wikidata.

**What it binds now.** `ace/execute!` returns the nodes it built per sentence (`:built`). Variables have the native shape and one plan variable `(:var Name)` is one `zelph/var` node per naming scope, because every `zelph/var` call makes a new variable whatever its name (F11; [The Scope of a Variable Symbol](../janet.md#the-scope-of-a-variable-symbol-one-block)). No `.import math` outside arithmetic cases: one corpus loop went from 82 ms to 93 s with it (zelph 1.0.1). The tests, owned by [Tests and Quality Gates](tests.md#re-entry-interop-persistence-provenance): `ace re-entry: every built fact and rule re-enters natively with no new node`, `ace re-entry: ground rule patterns stay unclaimed`, `ace re-entry: rule variables have the native variable shape`, `ace drs path: a text and its printed and re-read DRS build the same graph`.

### A7: Provenance Quotes the User's Sentences

**The route.** Every action of a plan carries its source, `{:block :sid :span :text}`, from M1 on. The front end keeps these records in the engine's node annotations ([Node annotations](../janet.md#node-annotations)): one value `"<session>|<block>|<sentence>|<text>"` per source under the key `ace:src` on every node a block builds. Annotations are never seen by queries or rules and are never a mention of a node; every removal erases them, so a fact asserted again with the same node id does not inherit a stale quote; `.save` writes them as an optional section of the file, a filtered save filters them, and the published networks need no regeneration. [Provenance: Where a Sentence Came From](answers.md#provenance-where-a-sentence-came-from) specifies what is written, and [Proofs, provenance and retraction as data](foundation.md#proofs-provenance-and-retraction-as-data) the engine store.

The session shows a contradiction that the inference of the second block meets: "No man waits." is the rule `(*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)`; it derives that John does not wait, the opposite of what the first block stated. The report quotes the three sentences it rests on; the block is kept, because a contradiction derived by inference is reported, not undone (M4):

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace John is a man. John waits.

    zelph> ace No man waits.

    Contradiction [ace/contradiction-derived]: these sentences do not hold together:
      "No man waits." (block 2, sentence 1)
      "John is a man." (block 1, sentence 1)
      "John waits." (block 1, sentence 2)
    zelph> ace Does John wait?

    Answer: Yes. John waits.
    ```

Had "John waits." come in a later block, after the refutation was derived, the dry run would have rejected that block with `[ace/contradiction]` ([Contradictions and Defeated Conclusions](block.md#contradictions-and-defeated-conclusions)).

**What it buys.** Explanations and contradiction reports quote the user's own sentences, so they do not depend on what the verbaliser covers (A5). "Where did I say that?" has an answer. ACE text can be exported again (B13), also after a `.load`. It is the only store that covers rules as well as facts and adds no node, so the oracles that count nodes (A6) stay as they are.

**Why not facts (F2).** A fact that names a rule node, as subject or object, turns the rule off. This is zelph's documented semantics ([Mentioning a Rule Is Not Asserting It](../logic.md#mentioning-a-rule-is-not-asserting-it)), not a defect. The recording builds a rule from Janet the way the front end builds "Every man is mortal.", annotates it, and lets it fire; then it states one fact about the rule node:

```
zelph> %(def r (zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def c (zelph/collection (zelph/fact x "~" "man"))) (zelph/fact c "~" "conjunction") (zelph/fact c "=>" (zelph/fact x "is" "mortal")))))
<zelph/node (*{(X ~ man)} ~ conjunction) => (X is mortal)>
zelph> %(zelph/annotate r "ace:src" "s1|1|1|Every man is mortal.")
true
zelph> %(zelph/annotations r)
{"ace:src" ("s1|1|1|Every man is mortal.")}
zelph> john ~ man
(john is mortal) ⇐ {(john ~ man)}
zelph> X is mortal
Answer: john is mortal
zelph> .list-rules
Listing all rules:
------------------------
(*{(X ~ man)} ~ conjunction) => (X is mortal)
------------------------
zelph> %(zelph/fact r "said" "s1")
<zelph/node ((*{(X ~ man)} ~ conjunction) => (X is mortal)) said s1>
zelph> .list-rules
No rules found.
zelph> bill ~ man
zelph> X is mortal
Answer: john is mortal
```

The annotation leaves the rule in force; the fact `… said s1` takes it out of force, so `bill ~ man` derives nothing, and `X is mortal` still answers only John. Provenance kept as facts about rules would switch off every rule it describes. The test `ace provenance: recording provenance adds no node and never changes the rule count` pins that annotations do not.

**Generated rules.** A rule that a switch writes (A11) is no write of the front end and has no record of its own. Its source is the generator: the generated `=>` node is a derived fact justified by the generator, whose record names the sentence; `zelph/explain` of the generated rule leads there. An imported fact without provenance is described as "a fact of the loaded graph".

**Not taken.** Statements such as `S ace:states F` (DEC-PROV (b)) would show in `S P O` queries and to meta-rules with a variable predicate; two paraphrases would make two sentence nodes, so the oracles that require no new node would fail; and rules would need a second mechanism (C12). A Janet table keyed by node, used until M9, was planned and dropped once the engine store existed: it would have been a second implementation, thrown away at M9.

### A8: The Engine Is the Validator

**The straight answer.** A validator in the front end that predicts what the engine will refuse, or one cluster per block.

**The route.** Every block's write actions run first in a scratch cluster, `ace:dry`, which is dropped; when they succeed, they run again in the caller's active cluster. A contradiction that the block's inference derives is reported and the block is kept (A7's session). With `(ace/option :atomic :consistent)`, from M4, the block's inference also runs inside the dry run, and a block whose run met a new contradiction is rejected, at the cost of one more run per block. [All or Nothing: the Dry Run](block.md#all-or-nothing-the-dry-run) specifies it within the handler's steps. The session needs negation, which arrives at M4:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace Sue is not a doctor.

    zelph> ace Tom is a doctor. Sue is a doctor.

    Error in line "ace Tom is a doctor. Sue is a doctor.": Contradiction [ace/contradiction]: sentence 2, line 1, column 18: zelph already holds the opposite of this sentence, from "Sue is not a doctor." (block 1, sentence 1).
      <>Sue is a doctor.
    ```

    At the interactive prompt the session goes on:

    ```
    zelph> ace Is Tom a doctor?

    Answer: It is not provable that Tom is a doctor.
    ```

The engine refused `Sue ~ doctor` inside the dry run; the drop took back `Tom ~ doctor` as well, so the caller's graph is as it was.

**What it buys.** No second implementation of the engine's refusal logic, which would drift from it. The caller's cluster semantics stay intact, so users of `.cluster` and the tests' per-case clusters see every block. Any refusal of the engine becomes a clean rejection of the block. What-if checks (B14) and the hypothetical check behind universal questions reuse the mechanism.

**What makes it sound.** A dropped cluster restores what the dry run changed: its nodes go with every outside fact that names them, a revoked rule-pattern mark comes back, a refutation made inside it is taken back, a relation-type declaration it wrote for an older predicate goes unless something outside uses it, names and fact stores are cleaned in time proportional to the cluster, and negation records reported as defeated return to the record set ([What a Dropped Cluster Restores](block.md#what-a-dropped-cluster-restores) lists each property with the engine item that built it; [Node Clusters](../rules.md#node-clusters-transactional-workspaces) and [Scoped work: clusters](../janet.md#scoped-work-clusters) document the mechanism). A failed rule build rolls back, so the real run cannot leave half a rule behind. Engine refusals arrive as catchable Janet errors, and `(zelph/cluster prev)` restores the caller's cluster.

**Inference inside a dropped cluster (F10).** Before the host item P-drop-delta, a run inside a cluster that was then dropped left the incremental run believing the graph was at a fixpoint: what that run derived about earlier facts went with the drop and was not derived again until a full run. Now the drop of a cluster in which a run derived something invalidates the record of additions, and the next `.run-delta` says so and runs a full pass:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> (X ~ man) => (X ~ mortal)
zelph-> tim ~ man
zelph-> .run-delta
Incremental run not applicable (no previous run, bulk load, changed rule set, or non-semi-naive strategy) - running a full pass.
Starting reasoning with 24 worker threads.
(tim ~ mortal) ⇐ (tim ~ man)
Reasoning complete. Total unification matches processed: 1. Total contradictions found: 0.
Reasoning summary: 1 matches processed, 0 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m0.000s – 1 matches processed, 0 contradictions found.
Ready.
zelph-> john ~ man
zelph-> .cluster w
Active cluster: w
zelph-> bill ~ man
zelph-> .run-delta
Starting reasoning with 24 worker threads.
(john ~ mortal) ⇐ (john ~ man)
(bill ~ mortal) ⇐ (bill ~ man)
Reasoning complete. Total unification matches processed: 2. Total contradictions found: 0.
Reasoning summary: 2 matches processed, 0 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m0.000s – 2 matches processed, 0 contradictions found.
Ready.
zelph-> .cluster default
Active cluster: default
zelph-> .cluster-drop w
Dropped cluster w: removed 4 node(s).
zelph-> .run-delta
Incremental run not applicable (a removal took facts a run may have derived) - running a full pass.
Starting reasoning with 24 worker threads.
(john ~ mortal) ⇐ (john ~ man)
Reasoning complete. Total unification matches processed: 2. Total contradictions found: 0.
Reasoning summary: 2 matches processed, 0 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m0.000s – 2 matches processed, 0 contradictions found.
Ready.
zelph-> X ~ mortal
Answer: john ~ mortal
Answer: tim ~ mortal
```

`john ~ mortal`, derived inside `w` and dropped with it, is derived again. A write-only drop, which is what the dry run is, keeps the record valid, and a rule built inside the dropped dry run leaves no entry behind, so the dry run stays cheap. The user's removal commands (`.prune-facts`, `.prune-nodes`, `.remove`, `.cleanup`, and `zelph/prune-fact`, which `ace-retract` uses) cannot tell derived from asserted facts and invalidate the record whenever they remove a fact: a full pass, never a wrong answer. The rule the real run builds is new to the next run; it takes a classic pass of its own, unless a rule in force has a negated condition, which makes the whole run a full pass (after the front end's first negation-as-failure rule, every block that writes a rule pays one; this is the category-4 item [Z3-P1](prerequisites.md#z3-p1-after-the-first-negated-condition-every-new-rule-costs-a-full-pass)). Under `:atomic :consistent` the dry run holds a run, and when that run derived anything, its drop makes the block's real inference a full pass.

**Why not one cluster per block (DEC-ATOMIC (b)).** A block recorded in a cluster of its own is invisible to the caller's active cluster: with `.cluster probe` active, a block built in `ace:block-1` leaves `probe` empty. Block clusters pile up in `.cluster`. And dropping one to un-say a block is unsound (C7).

**What it binds now.** The names of individuals are the same in both runs (A14). In the default mode no inference runs inside `ace:dry`; `:atomic :consistent` runs one there, which is sound only with P-drop-delta in place (it is built). Both modes rely on P-drop-names, P-scratch-drop and P-restore-mark from M1 and on P-refute (e) from the first construct that refutes (M4); all are built ([Scratch clusters and the dry run](foundation.md#scratch-clusters-and-the-dry-run)). The caller's cluster is restored with `(zelph/cluster prev)`, never with `nil`, and nothing inside a rule-building thunk switches or drops a cluster (the engine refuses it).

### A9: Of-phrases Are Relations

Of-relations occur in 577 of the 2813 accepted corpus DRSs (20.5 %): possessives, the Saxon genitive, `whose`, `the N of NP`. They are needed at M3:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace Bill is a father of John. Who is a father of John?

    Answer: Bill is a father of John.
    ```

The block writes the relation `John father Bill` and the class atom `Bill ~ father`, and the question reads the relation.

**The route not taken.** A definite "the N of Y" read as the zelph term `(N of Y)`, the application predicate of zelph's math stack, would reach `derivative` and similar functions without any vocabulary ([DEC-OF](decisions.md#dec-of) (c)). It departs from the DRS, where the N is a referent and not a term; it assumes uniqueness ("a friend of John" is no function); and it makes the corpus comparison of A1 harder for a fifth of the cases.

**A verified collision (F8).** Once math is imported, a fact with the predicate `of` prints as a function application, and the question shows it:

```
zelph> father of John
zelph> X of John
Answer: father of John
zelph> .import math
zelph+> X of John
Answer: $( father(John) )
```

So `of` is never a predicate the front end writes.

**The route taken.** A relational noun gives `R2 N R1` plus the class atom `R1 <instance> N`, as in the session above ([Relational Nouns](words.md#relational-nouns)). When the noun, in its relation role, resolves to a Wikidata property (A15), the class atom is left out everywhere: Wikidata states `Q… P50 Q42`, not that Q42 is an instance of "author", so the atom would make "Who is the author of …?" answer nothing. The located copula and the prepositional phrases of events use `is <prep>` phrases (`"is in"`), so zelph's core node `in` never becomes a predicate (DEC-NAMES). The functional reading applies only to nouns that carry a `:math` reading in `ace-math`'s vocabulary, such as `divisor` and `derivative` ([Numbers and Formulas](math.md)).

**What it binds now.** Readings carry class attributes that the plan reads (`:math`), whichever store the entry comes from (the hook of B19).

### A10: The Keyword Kit Is a Module of Its Own

**The route.** `keyword-kit.zph`, prefix `kit/`, holding no ACE knowledge, exists from the first day: `kit/mask`, `kit/open-region`, `kit/line-col`, `kit/marked`, `kit/expect` (masking regions, positions in code points, the `<>` marker, the furthest failure of a PEG; [keyword-kit](api.md#keyword-kit) has the signatures).

**What it buys.** SPARQL can get positioned errors: today a syntax error in a `sparql` block gives one fixed message without a position. Later keyword languages get the same error experience (B27). Offering the kit to `sparql.zph` is a separate change.

**What it binds now.** No ACE word, message id or construct id appears in `keyword-kit.zph`, and its tests run without `.import ace`.

### A11: Nested Conditionals: a Generator for a Switch or a Then-box Referent, Flattened Otherwise

**The straight answer.** Flatten a conditional inside a conclusion, `If A then if B then C`, into one rule `(A, B) => C`. For first-order conditionals nesting is logically flat, so the shape is an engineering choice, except where the inner rule uses a referent that the outer conclusion introduces.

**The route.** zelph's own rules that write rules, `A => (B => C)` ([Rules That Write Rules](../rule-generators.md)), built in one rule-building thunk, in exactly two cases:

1. A **switch**: the outer condition declares no referent (only ground atoms) and the inner rule keeps a variable. Applied recursively, a chain of switches is a generator of generators.
2. An inner rule that uses a **referent the outer conclusion introduces**: "Every man owns a dog that every cat hates." has no flat equivalent, because the dog is a fresh witness per man that the inner rule must share.

Every other nested conditional is flattened.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace If John waits then every man sleeps. Bill is a man.
    Does Bill sleep?

    Answer: It is not provable that Bill sleeps.
    zelph> ace John waits. Does Bill sleep?

    Answer: Yes. Bill sleeps.
    ```

The first block writes a rule that writes a rule, and the fact `Bill ~ man`:

```
(*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
Bill ~ man
```

Every rule the front end writes with a single condition takes the form of a one-element condition set, `*{…} ~ conjunction`, so that every rule it builds has a condition set ([DEC-RECORD-SINGLE](decisions.md#dec-record-single)). zelph reads a one-member condition set as its single condition, so this form and the plain spelling are one rule, and since 2026-10-15 zelph records a contradiction of a rule with a plain single condition exactly as it records one of the one-member set: the form no longer decides whether a contradiction is recorded. `ace/zelph-of` prints it as zelph lists it ([DEC-ONE-CONDITION-PRINT](decisions.md#dec-one-condition-print)), and this form, typed, builds the same rule; `(X ~ man) => …` and `((X ~ man)) => …`, typed into a graph that does not hold the rule yet, build the plain rule, since the parentheses only group, and its contradictions are recorded alike ([What ace/zelph-of Prints](semantics.md#what-acezelph-of-prints)). The same lines in zelph today, with a second man added after the switch:

```
zelph> (*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
zelph> Bill ~ man
zelph> X sleeps nil
zelph> .list-rules
Listing all rules:
------------------------
(*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
------------------------
zelph> John waits nil
((*{(X ~ man)} ~ conjunction) => (X sleeps nil)) ⇐ {(John waits nil)}
(Bill sleeps nil) ⇐ {(Bill ~ man)}
zelph> X sleeps nil
Answer: Bill sleeps nil
zelph> .list-rules
Listing all rules:
------------------------
(*{(X ~ man)} ~ conjunction) => (X sleeps nil)
(*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
------------------------
zelph> Tom ~ man
(Tom sleeps nil) ⇐ {(Tom ~ man)}
zelph> X sleeps nil
Answer: Bill sleeps nil
Answer: Tom sleeps nil
```

Until `John waits nil` holds, `.list-rules` shows only the generator and nothing sleeps; then the generated rule is in force, listed, and fires for later men too. For the second case the generator holds the then-box's witness in the inner rule (the letters follow the order of first occurrence, conditions before conclusions, and the inner rule shares the outer rule's names: [Variables](api.md#variables), [DEC-VAR-LETTERS](decisions.md#dec-var-letters)):

```
(*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog) ((*{(Z ~ cat)} ~ conjunction) => (Z hates Y))
```

It derives one witness and one generated rule per man.

**Why not always a generator (DEC-NESTED-IMP (b)).** With a referent in the outer condition a generator makes one rule per binding, where one flat rule does the same job. The commonest nested form is of that kind: "Every man sees every dog." (corpus case 84 has this shape) is a conditional inside a conclusion in APE's DRS, and 85 accepted corpus cases hold an implication inside a conclusion. Over 300 men and 5 dogs, run on the current build:

| Form | Rules after the run | Nodes | Derived `sees` facts |
|---|---|---|---|
| a generator, `(*{(X ~ man)} ~ conjunction) => ((*{(Y ~ dog)} ~ conjunction) => (X sees Y))` | 301 | 2742 | 1500 |
| the flat rule the front end writes, `((X ~ man), (Y ~ dog)) => (X sees Y)` | 1 | 2139 | 1500 |

The generator form also ran about eight times as long (about 0.25 s against 0.03 s in seven alternating runs each on the current build; timings are indications, not a benchmark). And a generator whose inner rule is fully ground never fires; typed natively, it also turns off an identical ground rule typed earlier, the two being one hash-consed node: "If Mary sleeps then Bill runs." followed by "If John waits then if Mary sleeps then Bill runs." as a generator leaves only the generator in force, and Bill does not run when John waits and Mary sleeps. When either of the two is built from Janet, as the front end builds both, that rule stays in force. So a fully ground nested conditional is flattened too; the engine fix, the positive record that a rule was asserted, is the category-4 item [K1-P2](prerequisites.md#k1-p2-a-rule-generator-cannot-put-a-ground-rule-in-force). [Nested Implications](semantics.md#nested-implications) specifies the planner's choice.

**What it buys.** zelph's rule generators are reachable from English at a bounded cost of one generated rule per switch; the then-box-referent shape, which no flat rule expresses, becomes available; `.list-rules` shows which rule is in force; at M9 an explanation names the fact that put a rule in force.

**What it costs.** Two shapes for one DRS construct: a decision in the planner, two renderings in the verbaliser and in show-zelph, two rows of tests. A then-box-referent generator makes one rule per outer binding, the price of a shape without a flat form. A generated rule has no provenance record of its own (A7). A ground outer condition asserted from the keyword handler must claim the fact, which the host item P-claim provides (built); a generated rule's ground parts stay rule patterns until something asserts them.

**What it binds now.** A head of a plan may be `(:imp {:if … :then …})` for a switch or a then-box referent. The inner rule is a `=>` fact inside the outer one, never a nested rule build (the engine refuses those). Its variables are created in the same thunk and share the outer rule's naming scope, the then-box's fresh witness included; each `(:var Name)` is one `zelph/var`. The check for endless generation, `:rule/endless-generation`, sees generator heads in both of its places: in `ace/plan`, over the block's rules and the registry of earlier ACE rules with class patterns compared by name, and in the resolution step, through the graph's subclass closure.

### A12: Complex Conditions Become Named Helper Predicates

**The straight answers.** Refuse negation as failure over several conditions, or apply De Morgan's law, as zelph's own error message suggests. De Morgan fails as soon as the negated group has a referent of its own: in "… is not provably a farmer who owns a dog", the dog is quantified inside the negation.

**The route.** The Lloyd–Topor transformation: the negated group becomes a helper rule whose conclusion is a helper predicate `ace:naf-<16 hex digits>` in the language `zelph`, and the condition becomes `¬(helper atom)`. The helper atom carries the group's free referents in a fixed shape (none: `nil H nil`; one: `F1 H nil`; two: `F1 H F2`; more: `F1 H <F2 … Fk>`). The helper's body is the group plus the positive conditions of the rule that bind its free referents, so its extent is bounded by the rule's own body. The hex digits are a hash of the group's canonical text, so the same group in two sentences is one helper. For the first block of the opening session the front end writes (hashes abbreviated as `<h1>`, `<h2>`; the individual mark of A14 and the profile pin of A16 are bookkeeping and not shown; the helper rule names its own variables, in the order of [DEC-VAR-LETTERS](decisions.md#dec-var-letters), so the man is X and the dog Y):

```
John ~ man
John ~ farmer
John owns Dog-<h1>
Dog-<h1> ~ dog
Bill ~ man
Bill ~ farmer
((X ~ man), ¬(X ace:naf-<h2> nil)) => (X is free)
((X ~ man), (X ~ farmer), (X owns Y), (Y ~ dog)) => (X ace:naf-<h2> nil)
```

The same lines in zelph today, with automatic inference off and the main rule entered before its helper; the last line types the rule with the negated group directly, which zelph refuses:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> John ~ man
zelph-> John ~ farmer
zelph-> John owns Dog-6f1c2a9e0b3d4c5a
zelph-> Dog-6f1c2a9e0b3d4c5a ~ dog
zelph-> Bill ~ man
zelph-> Bill ~ farmer
zelph-> ((X ~ man), ¬(X ace:naf-3c9e01aa5b7d2f40 nil)) => (X is free)
zelph-> ((X ~ man), (X ~ farmer), (X owns Y), (Y ~ dog)) => (X ace:naf-3c9e01aa5b7d2f40 nil)
zelph-> .run
Starting reasoning with 24 worker threads.
(John ace:naf-3c9e01aa5b7d2f40 nil) ⇐ {(John ~ man) (John ~ farmer) (John owns Dog-6f1c2a9e0b3d4c5a) (Dog-6f1c2a9e0b3d4c5a ~ dog)}
(Bill is free) ⇐ {(Bill ~ man) (¬(Bill ace:naf-3c9e01aa5b7d2f40 nil))}
Reasoning complete. Total unification matches processed: 10. Total contradictions found: 0.
Reasoning summary: 10 matches processed, 0 contradictions found.
Parallel unifications activated for 0 distinct fixed relations.
Reasoning complete in 0h0m0.000s – 10 matches processed, 0 contradictions found.
Ready.
zelph-> X is free
Answer: Bill is free
zelph-> X ace:naf-3c9e01aa5b7d2f40 nil
Answer: John ace:naf-3c9e01aa5b7d2f40 nil
zelph-> .strata
1 negation level:
Level 1:
  ((X ~ man), ¬(X ace:naf-3c9e01aa5b7d2f40 nil)) => (X is free)
zelph-> ((X ~ man), ¬((X ~ farmer), (X owns Y), (Y ~ dog))) => (X is free)
Error in line "((X ~ man), ¬((X ~ farmer), (X owns Y), (Y ~ dog))) => (X is free)": fact(): a condition cannot be a conjunction and a negation at once. zelph negates a single fact pattern, not a group of them. Use De Morgan: not(A and B) is the same as (not A) or (not B), and a disjunction is written as several rules with the same consequence.
```

With the helper, one run derives the helper atom for John and `Bill is free`, whatever the order of the rules, the helper relation answers a query of its own, and `.strata` gives the main rule a negation level. Without the helper, zelph refuses `¬` over a group of conditions and suggests De Morgan's law.

**What it buys.** Negation as failure over more than one condition (corpus case 3317; rules in the style of case 2772 with a larger group). The negated group gets a name: the helper is an ordinary relation that answers queries, as the recording shows, and that `.explain` can show. Content-hashed names make re-entry idempotent.

**What it costs.** Helper rules are visible in `.list-rules`. The engine orders rules with negated conditions into negation levels keyed by pattern ([Stratified Evaluation](../logic.md#stratified-evaluation)). In the core profile (class atoms with `~`) helpers and their rules get correct levels. In the Wikidata form a class pattern reads `P31` with any object, so rules that conclude a class and whose helpers read a class share one component with every class rule, and a class hierarchy that feeds a conclusion under "not provably" back into what such a rule tests is wrong in every run. Conclusions under "not provably" are final across runs: a later fact does not withdraw them. The front end switches the engine's negation records on with its first such rule, so a later fact that defeats one is reported as `[ace/w-naf-final]`. On a Wikidata graph a rule with a negated condition was matched in full on every run until the host item P-naf-delta, which is built.

**What stays refused.** Complex classical negation in a condition (`:neg/complex-in-body`, corpus case 3165), with the hint to write "does not provably". A universal inside a condition (`:rule/in-body`), negation as failure inside negation as failure (`:naf/nested-in-body`), and rules that cannot be ordered into negation levels (`:naf/unstratified`) stay refused in v1 ([DEC-STRATA](decisions.md#dec-strata); [why these refusals are honest](constructs.md#not-provably-that-depends-on-another-not-provably)); the narrowing that would accept the orderable cases is planned, not v1 (B32).

**What it binds now.** Helper names are resolved in `zelph`. No internal name contains `#`, which starts an end-of-line comment in a typed statement. `:naf/unstratified` is decided by one function over the graph of which rules may derive which conditions (`ace-drs/may-derive`), in which a conclusion `X <instance> C′` may derive a class pattern over C when C′ is C or the graph's upward subclass closure of C′ contains C; the function stays pure, and the resolution step, which passes it that walk, raises the id. From M4 the dry run also reads the engine's verdict, `(zelph/strata)`, for the cases the front end's own graph cannot see (native rules in the same component, a rule over predicates in force). The helper atom keeps its `F1 H …` shape, never `H H …`. A defeated conclusion that the engine reports as data (`:naf-final`) becomes `[ace/w-naf-final]`. [Helper Rules for "not provably"](semantics.md#helper-rules-for-not-provably) and [Static Checks](semantics.md#static-checks) specify the transformation and the checks.

### A13: Events Only Where Modifiers Need Them

**The straight answers.** Drop modifiers, which loses information. Give every verb an event node: two or three conditions per verb, and ACE facts become invisible to native and Wikidata triples. Write the modifiers on the nested fact: rejected (C13).

**The route.** The plain fact always; and only when the event carries modifiers, an occurrence individual `ace:event-<h>` in `zelph`, with `E ace:occurrence (fact)`, `E <predicate-name> Y` per prepositional phrase and `E ace:manner Adv` per adverb (M6).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace John waits in Paris. John waits fast in Rome.
    Does John wait fast in Paris?
    Does John wait in Paris?
    Does John wait?

    Answer: It is not provable that John waits fast in Paris.
    Answer: Yes. John waits in Paris.
    Answer: Yes. John waits.
    ```

The block writes (hashes abbreviated):

```
John waits nil
ace:event-<h1> ace:occurrence (John waits nil)
ace:event-<h1> "is in" Paris
ace:event-<h2> ace:occurrence (John waits nil)
ace:event-<h2> "is in" Rome
ace:event-<h2> ace:manner fast
```

The recording contrasts the rejected representation, modifiers written on the nested fact (for John), with occurrence individuals (for Mary), over the same two sentences:

```
zelph> (John waits nil) "is in" Paris
zelph> (John waits nil) "is in" Rome
zelph> (John waits nil) ace:manner fast
zelph> ((John waits nil) "is in" P), ((John waits nil) ace:manner fast)
Answer: (John waits nil) "is in" Rome, (John waits nil) ace:manner fast
Answer: (John waits nil) "is in" Paris, (John waits nil) ace:manner fast
zelph> Mary waits nil
zelph> ace:event-1a2b3c4d5e6f7081 ace:occurrence (Mary waits nil)
zelph> ace:event-1a2b3c4d5e6f7081 "is in" Paris
zelph> ace:event-92a3b4c5d6e7f809 ace:occurrence (Mary waits nil)
zelph> ace:event-92a3b4c5d6e7f809 "is in" Rome
zelph> ace:event-92a3b4c5d6e7f809 ace:manner fast
zelph> (E ace:occurrence (Mary waits nil)), (E "is in" P), (E ace:manner fast)
Answer: ace:event-92a3b4c5d6e7f809 ace:manner fast, ace:event-92a3b4c5d6e7f809 ace:occurrence (Mary waits nil), ace:event-92a3b4c5d6e7f809 "is in" Rome
zelph> (E ace:occurrence (Mary waits nil)), (E "is in" Paris)
Answer: ace:event-1a2b3c4d5e6f7081 ace:occurrence (Mary waits nil), ace:event-1a2b3c4d5e6f7081 "is in" Paris
zelph> Mary waits O
Answer: Mary waits nil
```

With the modifiers on the nested fact, hash-consing makes both waits one node, and "fast" is found together with Paris although the text says John waited fast in Rome. With occurrence individuals only Rome comes back for "fast", "in Paris" still answers, and the plain question still answers through the plain fact.

**What it buys.** The verb shapes of M3 stay as they are; ACE facts keep the shape of native and Wikidata triples, so a native line meets them under `.lang ace`; where, when and how questions run on the event; a rule condition with a modifier is deep unification over the nested statement ([Deep Unification](../logic.md#deep-unification)), while the plain fact stays claimed.

**What it costs.** One verb has two shapes: a rule condition with a modifier matches the occurrence, one without matches the plain fact, which always exists. One event individual per modified sentence.

**What it binds now.** The verb shape of M3, and predicate names for event modifiers from `ace/predicate-name` ([Events and Modifiers](semantics.md#events-and-modifiers) has the mapping).

### A14: Individuals Are Named by Their Description

**The straight answer.** Counter names, `Man-1`, `Man-2`, fresh for every occurrence, as APE gives every paragraph new individuals.

**The route.** Content-addressed names, `<Head>-<16 lower-case hex digits of FNV-1a-64>` in the language `ace`, computed from the canonical text of the group of conditions that describes the individual. The same canonical group is the same individual everywhere, inside one block too.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace There is a woman who sings.

    zelph> ace There is a woman who sings. How many women sing?

    Answer: 1 woman sings.
    ```

Both sentences name the same individual, `Woman-<h>`; APE would count two women. The session needs M7 for the question.

**What it buys.** Re-entering a block is idempotent like every zelph statement, so the oracles of A6 cover indefinites. On a large persistent graph to which ACE documents are applied repeatedly, a run adds no fresh set of individuals. There is no counter, no probe of the graph, and no collision after a `.load`. Paraphrases that normalise to the same group meet the same node. Plans need no graph.

**What it costs.** Equal descriptions denote one individual ("There is a man." twice is one man), which differs from APE and is documented. The names are unreadable. A group's referents need a canonical order ([Canonical Text and Names](semantics.md#canonical-text-and-names)). Sixteen hex digits, because eight would merge two descriptions silently at the volumes the front end is meant for: 1.2 % at 10,000 individuals per head noun.

Individuals are not names (DEC-NEQ), and a zelph pattern cannot see whether a node has a name. So every individual the front end asserts carries the internal mark `I ace:individual nil` in `zelph`, written with it from M3, which the "at most" and "less than" denials of M7 skip with `¬(W ace:individual nil)`. Witnesses the engine creates for a conclusion with a fresh variable carry no mark and count as distinct for these denials, a closed-world reading for witnesses that the user documentation states with an example ("Every man owns a red car. Every man owns a blue car. John is a man. John owns at most 1 car." is a contradiction). A referent inside an opaque statement (A17) is no claimed individual and carries no mark.

**Evidence.** FNV-1a-64 computed in zelph's Janet gives the published test values; content-addressed names re-enter with no new node, also inside a group fact; the tokenizer keeps an individual name as one word, and the annotator recognises the shape `^[A-Z][^-]*(-[^-]+)*-[0-9a-f]{16}$`. The rejected variant, anonymous nodes, is C6.

### A15: Words Reach Wikidata Items Through Their Labels

**The straight answer.** Bind each word by declaring it: a lexicon entry whose symbol is an id, or an `ace` name on the item. It stays available (step 1 below), but it fails the requirement that "There is a man." reaches Wikidata's item Q8441 without any declaration.

**Why the label route was rejected at first (F18).** zelph's Wikidata importer used to give a shared English label to one item only, the one processed last in the dump, and every other carrier lost its English name. In the published networks the intended item held its label for 0 of 24 test labels; resolving words through them would pick the wrong item silently (C17).

**The route.** Make the importer keep every label (host item P-wd-labels, built), and let the front end choose deterministically and say so. A content word arrives with its symbol and its role (*noun*, *name*, *relation*, *verb*, *adjective*) and is resolved in four steps, the first match winning ([The Four Steps](words.md#the-four-steps) specifies them in full):

1. an explicit id: a prefixed word (`p:Q42`, `n:Q8441`, `n:P50`, `v:P155`, `a:Q3142`) or a lexicon entry whose symbol is an id;
2. a node named in `ace`;
3. on a Wikidata graph with current labels, for nouns, names and relations only: every node carrying the label in any hyphen or space variant (names also with "The "), Wikimedia pages dropped, a name whose most connected candidate is a given-name or family-name item left unbound, otherwise the lowest Q-number (P-number for a relation); a relational noun's two roles are resolved together;
4. a new node in `ace`.

Every word that step 3 binds to a label more than one node carries, and every undeclared noun or name that step 4 creates, is announced once per word, role and session, in one `word-senses` note per block; `(ace/senses w)` lists the candidates. [The word-senses Note](words.md#the-word-senses-note) shows the note, and [Choosing Another Sense](words.md#choosing-another-sense) how to choose another item.

**Evidence (F19).** On the full graph the lowest Q-number is the intended item for 78 of 88 frequent nouns and 25 of 27 names; the importer's old holder was for 31 and 0, the first sense of the English lexeme for 74 and 18. Trying "The " in front of a name raised the names resolved right from 80 to 94 of 101. The given-name rule turned 30 of 31 given names into individuals and left places (49 of 52) and people (19 of 22) unchanged. Verbs meet a property for 1 of 257 lemmas, in the wrong sense; adjectives meet a wrong item for 24 of 63, and no Wikidata fact has the shape `X is <item>`: hence step 3 for nouns, names and relations only.

**What it buys.** ACE over Wikidata without declarations, and the same behaviour without Wikidata, where nodes are created instead; multi-word names; a choice the user can read and override per word (`n:Q…`, an entry); relational nouns reach properties ("Who is the author of …?" over P50, A9).

**What it costs.** A word's meaning depends on the loaded graph; that is announced, never silent. English ambiguity stays: no criterion reaches 100 %, and the lowest Q-number misses words such as group, company, member and end. Memory for the kept labels and their index: about 0.5 GB on the full graph, 2 to 2.3 GB with the fallback to `mul` labels (about 1 % of the graph), about 85 MB on the small network (estimates). On a network imported before the importer change, step 3 is off and `[ace/w-old-labels]` says so.

**Host work, all built.** P-wd-labels (every item keeps its label; the lowest number holds the name whatever the thread order; an item without an English label gets its `mul` label; a marker tells current labels from old ones), P-label-index (a label → every carrier, with re-election on removal; the binding `zelph/lookup-all`), and P-drop-names, P-scratch-drop and P-pidx-local, which keep every dry run and every question sublinear. The importer changed; the `.bin` format did not. Regenerating the published networks with the new importer is a step of the maintainer's ([Outside the Code](decisions.md#outside-the-code)); a network imported before it keeps step 3 off. A network built for the front end is a new file ([A Network for ace](words.md#a-network-for-ace); the engine items: [Wikidata labels and the label index](foundation.md#wikidata-labels-and-the-label-index)).

**What it binds now.** Resolution is one step before the dry run and creates nothing; its session cache is keyed by (word, role) and `zelph/epoch`. Declared names skip step 3: `ace-core` ships the common ACE example names, so the canonical ACE texts keep their meaning on a Wikidata graph. A bare `Q42` stays an ACE variable; prefixed ids are tokens of their own; a quoted `` `Q42` `` is the literal name "Q42", unless the same block also writes `p:Q42`; the front end never creates a node in the language `wikidata`. The verbaliser renders through the same resolver (A5). Nothing in the path is linear in the size of the graph: the candidates of one label, exact probes, `zelph/degree`. An index that `zelph/lookup-all` reports incomplete (after a partial load) turns step 3 off with `[ace/w-index-incomplete]`.

**Variants not taken.** Criteria that read instance counts or in-degree at lookup time: no better, and they cost time proportional to the degree (Q5 has over a million P31 statements pointing at it). Sitelinks: not in the graph. Lexeme senses: no better (B15). Verbs and adjectives through labels: above. Content nodes named in `en`: a node the front end creates for "likes", "red" or an individual must never take or merge a label a Wikidata item holds (DEC-CONTENT-NS).

### A16: Class Atoms Over an Instance and a Subclass Predicate, Pinned per Graph

**The straight answer.** Class atoms with zelph's core `~` and no hierarchy. Over Wikidata, class questions would then answer nothing, twice over: instance facts use the property P31, not `~` (F21), and classes are P279 data that ACE 6.7 has no closure construct to follow (F15). Renaming `~` to P31 is rejected (C18).

**The route.** Two vocabulary entries, the **instance predicate** and the **subclass predicate**, pinned per graph at the first class atom the front end writes, by one fact in internal vocabulary: `ace:profile ace:uses ace:wikidata` (instance P31, subclass P279, the nodes of those names in the language `wikidata`) or `ace:profile ace:uses ace:core` (instance core `~`, no subclass predicate). No node is renamed. A user binding wins over the profile: a node the user named `ace:instance` or `ace:subclass` in the language `ace`. A graph that later looks different (pinned to `ace:core`, then holding P31 and P279) gets `[ace/w-profile]` once per session and keeps its pin. Statements and rule conclusions write `X <instance> C`; class conditions in questions and rules become `X <instance> D, ((D <subclass> C) closure zero-or-more)` when a subclass predicate exists. Nothing is materialised.

The recording uses a Wikidata-shaped graph with ordinary nodes named `P31` and `P279`:

```
zelph> Douglas P31 writer
zelph> writer P279 person
zelph> person P279 human
zelph> X ~ writer
zelph> X P31 human
zelph> (X P31 C), ((C P279 human) closure zero-or-more)
Answer: writer P279∗ human, Douglas P31 writer
```

`X ~ writer` finds nothing, because the instance fact uses P31; `X P31 human` finds nothing, because Douglas is an instance of writer, not of human; the condition with the subclass closure finds him. zelph prints a closure over an unquoted predicate in its `∗` form; over a quoted predicate such as `"is subclass of"` it prints the explicit form, because the `∗` form does not parse after a quoted predicate.

**Why the pin is a fact and not a name.** An `ace` name on core `~` would make every `~` fact display as `ace:instance` under `.lang ace`.

**What it buys.** ACE questions and rules over the Wikidata class hierarchy, and over any taxonomy loaded natively, with nothing to configure; without Wikidata nothing changes.

**What it costs.** A rule with a closure condition was matched in full on every run until P-closure-delta, and a question's closure rebuilt its index until P-pidx-local; both are built. A rule whose class has a large extent derives one fact per instance, which the front end warns about before asserting it (`[ace/w-large-rule]` above 100,000 instances, read with `zelph/degree`). Wikidata models some memberships with other properties (occupation P106, sex P21): "Is Douglas Adams a writer?" answers "It is not provable …", and the user documentation says how to ask instead (B29; [What Wikidata Models Differently](words.md#what-wikidata-models-differently)).

**What it binds now.** One function in `ace/plan` (`ace-drs/class-pattern`) builds every class item: the atom `(:atom T (:vocab :instance) C)` in statements and conclusions, the pattern `(:class T C V)` in conditions and questions. The resolution step resolves the two predicates per block (a user binding, else the pin, else the profile the pin will take) and expands the pattern; the resolved predicates never enter the plan, and the pin is written by both runs outside any rule thunk, as no plan action. The pin, that function and the subclass closure in yes/no questions are M1; wh-questions and relational nouns use them from M3, rule conditions from M4. `ace/zelph-of` renders the explicit closure form. A class condition under "not provably" goes through a positive helper in a rule (A12); in a question it is evaluated in Janet, since a question writes no rule. [Class Patterns and the Graph Profile](semantics.md#class-patterns-and-the-graph-profile) specifies the function and the pin.

### A17: Statements About Statements Are Facts of Opaque Predicates

**The straight answers.** Drop the modal operator: unsound, "John can fly." would assert that John flies. Refuse the 310 corpus cases with modality (221) or subordinate statements (89). Write the content as a nested statement: every fact nested in a plain statement outside a `=>` is claimed (F16), so "John believes that Mary waits." would assert that Mary waits. A tautological rule as a mention: rejected (C14).

**The route.** Generalise the one opaque construct zelph already had. `=>` is documented as the built-in opaque predicate; a predicate becomes opaque through the fact `P ~ opaque`; every argument position of an opaque fact is a mention scope: whatever a writer newly creates there gets the existing rule-pattern mark (one kind of mark for every reader), and a later claim revokes it; `.explain` and `.node` choose their label by position. `zelph/mention` and the C function `zelph_mention` serve programmatic writers; a `=>` written with them is a mention too, not in force until something claims it and not read by a rule over rules, which is how the front end writes the conditional inside a that-clause ([DEC-MENTION-RULE](decisions.md#dec-mention-rule)). A declaration is refused while a fact of P already holds a statement (host item P-opaque, built; [Mentioning a Statement: Opaque Predicates](../logic.md#mentioning-a-statement-opaque-predicates); [Statements about statements: opaque predicates](foundation.md#statements-about-statements-opaque-predicates)). A that-clause becomes `John believes (Mary waits nil)` with `believes` opaque; a modal statement becomes `(John swims nil) ace:can nil` with `ace:can` opaque.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace John believes that Mary waits.
    Does Mary wait?
    Who believes that Mary waits?

    Answer: It is not provable that Mary waits.
    Answer: John believes that Mary waits.
    ```

The block writes `believes ~ opaque` (on first use of the verb with a that-clause) and `John believes (Mary waits nil)`, the inner statement as a mention. In zelph today, with a predicate `says` that is not opaque beside one that is:

```
zelph> John says (Mary waits nil)
zelph> S waits O
Answer: Mary waits nil
zelph> believes ~ opaque
zelph> John believes (Sue waits nil)
zelph> S waits O
Answer: Mary waits nil
zelph> X believes (Y waits nil)
Answer: John believes (Sue waits nil)
zelph> (X believes (Y waits nil)) => (X expects Y)
(John expects Sue) ⇐ (John believes (Sue waits nil))
zelph> X expects Y
Answer: John expects Sue
zelph> Sue waits nil
zelph> Sue waits O
Answer: Sue waits nil
zelph> says ~ opaque
Error in line "says ~ opaque": fact(): a predicate cannot be declared opaque while one of its facts holds a statement -- says: John says (Mary waits nil)
```

The fact nested under `says` is claimed: `S waits O` answers Mary. The fact nested under the opaque `believes` is not, yet a question and a rule condition read it by deep unification; Sue waits only once it is stated. Declaring `says` opaque at the end is refused, because one of its facts already holds a statement; the front end turns that refusal into `[ace/unsupported]` naming `:sub/that-clause` ([How a Failed Dry Run Is Classified](block.md#how-a-failed-dry-run-is-classified)).

**What it buys.** Modality and belief contexts that questions and rule conditions can read. Modal statements and that-clauses in conclusions, which a mention per call cannot carry because a firing claims what it writes (60 corpus cases have a modal statement under `=>`). One mark for every reader. Whether a statement is claimed becomes a property of the graph rather than of the path by which it was written.

**What it costs.** The estimate was 400 to 700 lines of C++ with 25 to 35 doctests; as built, the change spans 37 files (1857 lines added, 32 removed) with 27 doctests, and the performance guard's counters are unchanged. Its prerequisites, P-claim, the refutation items P-refute (a), (b), (d) and P-restore-mark, are built.

**What stays out of v1.** A ground conditional inside a that-clause stays refused (`:sub/ground-conditional`): in the front end's own spelling the mention and the asserted rule do not collide, but the same ground rule with a single condition in the plain spelling, typed natively or held in a loaded graph, is one node with its mention, so the mention takes the rule out of force; the fix is the positive record that a rule was asserted (category-4 item [K1-P1](prerequisites.md#k1-p1-a-mention-switches-a-typed-ground-rule-off); [A Rule Inside a That-Clause](constructs.md#a-rule-inside-a-that-clause)). A conditional with variables inside a that-clause is a mention that no query matches, so a question about it answers "not provable". A rule condition over such a that-clause is refused (`:sub/conditional-with-referents-in-body`), because a condition that holds a rule with variables binds the parts of stored ground rules; a conclusion holding one is refused (`:sub/conditional-with-referents-in-head`), because a firing writes nothing for a mentioned rule whose conditions are a set and unnamed witnesses for one with a single condition. A box with several conditions or a referent, written as a set constant of mentions, is matched by its members through the host item P-set-match, which is built.

**What it binds now.** The DRS keeps every box; the modal and subordination rows are refused by id until M8; the plan places a statement in an argument position as the term `(:stmt Atom)`. A referent that the box introduces is named like an individual with the box in its hash, written inside the mention scope, unclaimed and without the individual mark (A14). Opaque declarations `P ~ opaque` are graph facts that the front end writes on first use, in the dry and real runs, since `ace-core` writes nothing to the graph. [Modality and That-Clauses](semantics.md#modality-and-that-clauses) has the mapping.

### Routes Absorbed Into the Baseline

Some routes were options once and are now simply how v1 works:

| Route | Where it lives now |
|---|---|
| classical negation through `~ refuted`, `!` and `¬` | the plan's actions ([Top-Level Conditions](semantics.md#top-level-conditions), [Rules](semantics.md#rules)) |
| a formula in a conclusion as a `!` constraint; one seed rule per arithmetic condition | [Numbers and Formulas](math.md) |
| universal statements about numbers decided, never compiled over integer nodes | [Numbers and Formulas](math.md) |
| the DRS matcher written from the specification, not ported | `ace/drs-equivalent?` |
| a pure planning stage | `ace/plan` |
| the lexical pre-pass | A3 |
| atomic blocks | A8 |
| "show me the zelph" | `(ace/option :show-zelph true)` |
| strings as character lists | `:data/string` at M3 ([DEC-DATA](decisions.md#dec-data)) |
| content-hashed helper names | A12 |
| APE once, as the development-time oracle | [DEC-SWI](decisions.md#dec-swi): APE runs from source on SWI-Prolog 10; nothing of APE is committed, the corpus is fetched at test time ([Differential Testing Against APE](tests.md#differential-testing-against-ape)) |
| suggest instead of guessing | replaced by [DEC-UNKNOWN](decisions.md#dec-unknown): unknown words are read by their position, and a guessed word one typing edit from a known form is rejected with the suggestion and the prefix that insists (`n:mna`) |
| cardinality bounds as denials with `!=` | [DEC-PLURALS](decisions.md#dec-plurals), M7 (below) |
| a rule with one condition as a one-element condition set | [DEC-RECORD-SINGLE](decisions.md#dec-record-single): the form zelph lists for the rule the front end builds; since 2026-10-15 the engine records a contradiction of a rule with a plain single condition as it records one of the one-member set, so the form no longer decides whether a contradiction is recorded |
| a conditional in a question as a guarded hypothetical check in a scratch cluster | [DEC-QUNIV](decisions.md#dec-quniv), M4 ([Questions That Hold a Conditional](answers.md#questions-that-hold-a-conditional)) |
| nonce words and graph independence as exact oracles | gates N and W ([The Guessing Gates B, N and W](tests.md#the-guessing-gates-b-n-and-w)) |
| a substitution generator over the construct examples | [DEC-GENERATOR](decisions.md#dec-generator), M3 |

**Cardinality as a denial (F17).** "John owns at most 1 car." becomes a denial over two distinct witnesses, each restricted to nodes without the individual mark of A14:

```
zelph> ((John owns X), (X ~ car), ¬(X ace:individual nil), (John owns Y), (Y ~ car), ¬(Y ace:individual nil), (X != Y)) => !
zelph> Car-0a1b2c3d4e5f6071 ace:individual nil
zelph> John owns Car-0a1b2c3d4e5f6071
zelph> Car-0a1b2c3d4e5f6071 ~ car
zelph> Car-8192a3b4c5d6e7f8 ace:individual nil
zelph> John owns Car-8192a3b4c5d6e7f8
zelph> Car-8192a3b4c5d6e7f8 ~ car
zelph> John owns Herbie
zelph> Herbie ~ car
zelph> John owns Herbie
zelph> John owns Bumblebee
zelph> Bumblebee ~ car
! ⇐ {(John owns Herbie) (Bumblebee ~ car) (¬(Herbie ace:individual nil)) (¬(Bumblebee ace:individual nil)) (Herbie != Bumblebee) (Herbie ~ car) (John owns Bumblebee)}
Found one or more contradictions!
```

The two cars that are individuals (in ACE, "John owns a red car. John owns a car.") may be one car and do not count; the two named cars are distinct by name and make a contradiction; `John owns Herbie` typed a second time does not. Each mark is typed before its car, because zelph runs inference after every typed line here; the front end writes a block whole and runs once. "At most n" costs n+1 witnesses and n(n+1)/2 guards per subject, which [Plurals, Groups, Cardinality and Mass](semantics.md#plurals-groups-cardinality-and-mass) and [Numbers and Formulas](math.md) take into account.

## Later Layers and the Hooks They Need

Each later layer below is outside v1. Its **hook** is what v1 must already have, so that the layer can be added without redesign; the [hook table](#the-hook-table) at the end collects them with the page that owns each. Some layers already run in part in v1's tests: the paraphrase classes of B1, the token mutations of B3, B4 and B5 in the slow tier, and the reasoning premises of B6. Numbers that are absent (B7, B18, B30) were absorbed into the baseline above.

### Tests and Oracles

**B1: the corpus as a generator of metamorphic tests.** APE's corpus holds 322 classes of paraphrases (754 cases), verbaliser pairs and mutation pairs; the oracle needs no expected graph, only consistency between members. Users could add their own "these two texts mean the same" pairs. Because individuals are named by their description (A14), two texts that normalise to the same groups build the same nodes, so no renaming step is needed. *Hook:* `ace/drs-equivalent?` is total and pure.

**B2: the corpus as its own lexicon** (DEC-CORPUS (b), not taken). With APE's small lexicon as an overlay, 2682 of the 2813 accepted cases are runnable; the other 131 need APE's large lexicon, which is never used. Deriving the entries per case from the corpus tree and DRS would make all 2813 runnable and test the morphology on every entry; APE keeps a prefixed word verbatim as its lemma, so a derived entry can take the lemma as written. *Hook:* `ace/load-ulex!` with `{:replace true}`, and a lexicon cache that notices the removal of user entries as well as their addition (a lexicon loaded inside a cluster and dropped with it), which `zelph/epoch` alone does not see.

**B3: a generator driven by the grammar, and mutation fuzzing** (DEC-GENERATOR (c), not taken for v1). Random valid sentences from the rule data itself, so every syntax id is exercised also where no corpus case or example exists, and mutations along the categories of rejection. Oracles without expected output: no crash, bounded work, atomic rejection, a well-formed DRS, the paths of A6. *Hook:* `ace/last-work` counts the work of the counting grammar; the rule data the builders produce is reachable as data (which the backtracking variant of A2 already requires), with reading tags as plain string literals so a generator can substitute words of the lexicon per tag.

**B4: lookahead completeness.** For every prefix of every corpus sentence, the next token is in the expected set; this guards against continuations that the committed choice of the grammar hides. *Hook:* `ace/expected` is the same function that produces syntax errors.

**B5: deliberate nonsense from zelph's own text.** Feed zelph's own stdlib modules, examples and documentation (about 5,000 lines of `.zph` and 12,400 lines of Markdown, counted on zelph 1.0.1) into `ace` blocks. The property: each block is accepted, refused by id, or rejected with a position; the graph is unchanged on rejection; nothing crashes; `%`, `.` and `#` lines never execute. Only this weak property holds, because documentation prose can be valid ACE by accident, and with guessing on more of it is. *Hook:* the atomicity of A8; `%` and `.` lines inside a block are ACE text.

**B6: reasoning tests from APE's OWL and SWRL translations.** APE's stored translations (206 into pure OWL, 43 with a SWRL rule) mark the Horn fragment; hand-written premises and entailments test reasoning, not parsing. *Hook:* none.

### Output: Uses of the Verbaliser

**B8: ACE as a tutor for native zelph.** The show-zelph switch is in v1. What remains: documentation tables that put ACE and native zelph side by side, generated from real runs. *Hook:* every construct row's `:examples` and `:zelph`, and `ace/zelph-of` lines that re-enter (A6).

**B9: answers that are proofs; `.explain` in ACE** (DEC-ECHO, M9). With `:justify` each yes/no and wh answer quotes the sentences it rests on and renders absent premises as "It is not provable that …"; with `:echo` each derived sentence is shown with its premises. Both are in v1 at M9 ([Why: Justified Answers](answers.md#why-justified-answers), [The Derivation Echo](answers.md#the-derivation-echo)); the hook is what makes them cheap. *Hook:* provenance keyed by node, rules included, persisted by node annotations (A7); `zelph/explain` returning the proof tree with its rule, and the derived facts of a run as data.

**B10: speak any graph, `ace/describe`.** "What do you know about John?" over native or Wikidata knowledge, read without growing the graph (F5). On a Wikidata graph the names come from A5's rendering, and a fact whose property resolves as a relation verbalises as a relational noun (`Q… P50 Q42` → "Douglas Adams is an author of …"). Other predicate names become lexicon entries (`"lives in"` a transitive verb `lives-in`); output-only templates cover predicates ACE cannot express; the fallback uses prefixes (`p:berlin v:is-capital-of p:germany.`). Rules are verbalised through `zelph/rules`, since no query lists a rule with a variable (F5). *Hooks:* `ace/verbalise-node` for every fact node (M9) over `zelph/parse`; the role-aware rendering (A5, A15); invertible predicate names (A5).

**B11: ACE explains SPARQL results.** Each result row instantiates the WHERE patterns, which are verbalised (`Berlin is located in Germany.`); SPARQL's evaluator is callable from another module (F7). *Hooks:* the verbaliser accepts triples or nodes; SPARQL exports the instantiated patterns (a change to `sparql.zph`, not to the front end); the role-aware rendering of Wikidata items.

**B12: zelph as an ACE-to-DRS parser for the Attempto ecosystem.** `ace/drs->ape` prints APE notation, and a public `drs` keyword would accept it (outside v1; [DEC-DRS-ENTRY](decisions.md#dec-drs-entry)). A parser that acrion can license freely and that needs no Prolog, also in the browser playground: the stdlib is embedded in the WebAssembly build, whereas APE in the browser needs a Prolog system compiled to WebAssembly (2.28 MB plus 1.66 MB) plus an image. *Hook:* A1, and path p3 of A6.

**B13: ACE text as a save format.** An export writes `.import ace` and one `ace` block per stored block, verbatim from provenance; importing it again rebuilds the knowledge base, including the block structure that `.save` loses (clusters are not saved, F3). Facts of a loaded graph have no provenance and are not exported. *Hook:* provenance stores the verbatim text and the block per node, rules included (A7).

**B14: what-if and entailment checks.** A block keyword that asserts a text in a scratch cluster, runs, reports whether given sentences follow and whether the text is consistent, and drops the cluster. The mechanism is already in v1 twice: `:atomic :consistent` runs a block's inference inside its dry run, and a conditional in a question is a guarded hypothetical check ([Questions That Hold a Conditional](answers.md#questions-that-hold-a-conditional)). *Hooks:* the machinery of A8; quiet runs that return the contradictions they met as data ([Running quietly](../janet.md#running-quietly)); P-drop-delta, which makes a run inside a scratch cluster sound over a graph that is not at a fixpoint (F10); P-scratch-drop, which keeps the drop proportional to the cluster.

### Lexicon and World

**B15: a lexicon generated from Wikidata's English lexemes** ([DEC-LEX-LARGE](decisions.md#dec-lex-large): a later option). Wikidata's lexemes are CC0: 99,194 English lexemes, 47,426 of them nouns; 21,177 nouns and 3,128 proper nouns have a sense linked to an item through P5137 (counted 2026-10-11). Since labels already reach items (A15), what a generated lexicon would add is word classes and forms for nouns that guessing reads only by their position. It would not improve the choice of sense: the first noun sense is right for 74 of 88 frequent nouns against 78 for the lowest Q-number (F19). Verb valency is practically absent, and no gender or mass/count flag comes with it. The importer flattens a lexeme's senses onto the lexeme node and makes no lemma a name, so a converter script is needed. *Hooks:* the annotator reaches every store only through `ace/readings` (A3); an entry's symbol may be a Wikidata id, so a generated entry carries its sense without new machinery. Where generated entries would live (Janet data beside `ace-core`, or user entries in `ace-lex`) is decided with the layer, not in v1.

**B16: a second concrete syntax, German ACE over the same graph.** Two naming languages bind one concept, and a rule built from German names fires on native data (F6). German content names would live in a language of their own beside `ace`. Reaching Wikidata needs German labels, and the importer reads only English ones. Costly: German morphology, case and gender agreement. *Hooks:* construct ids independent of English (A2); the one resolution step (A3), which gains a source-language parameter when this starts; DRS lemmas resolved through that language's lexicon, never as English strings.

**B17: the lexicon is knowledge** (DEC-LEX-LARGE (c), not taken for v1). Entries derived by rules: every item that is an instance of human a proper name with its gender from P21, so that "he" can refer to a person found through a label; APE's checks on word classes as `!` rules; `.explain` says why a word is a noun. Rules can derive user entries in `ace-lex`, never the shipped vocabulary. Millions of entries are never materialised over Wikidata: rebuilding the cache is linear in the entries, so a derived lexicon of that size needs a lookup per token. *Hooks:* the lexicon cache notices entries a run derived; `ace/readings` can look up per token form.

**B19: mathematical vocabulary declared by users** (the next step of DEC-OF). In v1 the math vocabulary (`great`, `large`, `small`, `prime`, `divisor`, `derivative`) is shipped Janet data of `ace-math.zph`, and every reading of such a word carries `:math {:kind … :node …}`. The later layer: a user declares a word for a new operator in an `ace-lexicon` block, and it works at once. It needs a declaration form for a `:math` reading, which v1 does not have: the lexicon reader accepts exactly the 27 entry predicates of the ACE lexicon format and no extension. *Hook:* readings carry class attributes that the plan reads (`:math`, preposition classes), from both stores (A9).

### Semantics

**B20: the graph as discourse context for definite descriptions.** An unresolved `the N that …` would become a uniqueness query: one answer → that node; none → the behaviour of APE (a new referent and `[ace/w-definite-new]`); several → an error naming them. Enabling it moves `ace/refres` from pure to reading the graph. *Hook:* the branch for unresolved definites is one private function, `ace-drs/unresolved-definite`, defaulting to APE's behaviour.

**B21: standing questions.** A question compiled into a rule with the conclusion `(q₁ answer X)`, so its answers grow as texts arrive and stay explainable; questions with "not provably" deferred. `q₁` is a plain node, never a rule node (F2). *Hook:* question plans are data (`:ask-yn`, `:ask-wh` with `:pattern`) that can be emitted as a query or as a rule.

**B22: equality through meta-rules.** In v1 ([DEC-NEQ](decisions.md#dec-neq); [Identity](semantics.md#identity)) identity is decided by node identity after resolution: two named constants are identical when they resolve to one node; "John is Bill." is read like "John = Bill."; a false ground identity is a contradiction that suggests a binding ("Mark-Twain is Samuel-Clemens."); an identity asserted between an individual and a name is refused, because the front end never merges two existing nodes. The later layer would accept it: `Clark ace:same Superman` plus congruence rules duplicates every fact of the identified nodes, and a node merge is irreversible and would need a host primitive. *Hook:* identities that node identity cannot decide are refused in one place, the resolution step.

**B23: disjunction by refutation and contrapositives.** k+1 sound Horn rules over `~ refuted` for a k-way disjunction, incomplete; the ground case relies on the refutation items, which are built. *Hook:* none needed: the rows stay refused.

**B24: modal axioms and quotation over opaque predicates.** On top of A17:

- modal axioms as ordinary rules or switches over opaque positions: the factive `(X knows (A P B)) => (A P B)`, the axiom `((A P B) must nil) => (A P B)`. Both run natively today. One axiom per opaque predicate, with the predicate fixed (an axiom with a relation variable relabels input facts in `.explain`). ACE's DRS makes `know` non-factive, so no axiom is installed by default; an opt-in switch is possible;
- quoted statements as surface syntax over opaque predicates, `<<mary waits nil>>` in the manner of RDF 1.2 triple terms: compiled to a predefined opaque predicate, they would open SPARQL-star patterns in `sparql.zph` and the quoting of a rule up to a renaming of its variables ("Quoting a rule" in [Where the logic goes next](../about.md#where-the-logic-goes-next)), which would make conditionals with variables inside that-clauses answerable (F23) and could lift `:sub/conditional-with-referents-in-body`;
- ground conditionals in that-clauses (`:sub/ground-conditional`), once the positive record that a rule was asserted exists ([K1-P1](prerequisites.md#k1-p1-a-mention-switches-a-typed-ground-rule-off)).

*Hooks:* A17's single mark with its label chosen by position; the DRS keeps every box; the rows that stay out are refused by id.

**B29: membership beyond the instance and subclass predicates.** On top of A16:

- memberships that Wikidata models with other properties (occupation P106, sex P21): a class atom could also read a declared membership property ("Is Douglas Adams a writer?" through `Q42 P106 Q36180`). Not designed. The user documentation tells the user to ask "What is an occupation of Douglas-Adams?", which answers with the item writer, Q36180, printed `p:Q36180` because its lower-case label is no name spelling; "Is a writer an occupation of Douglas-Adams?" is not provable by design (it asks for an instance of writer, and the occupation is the class itself);
- ACE taxonomies as subclass data: "Every N is a M." between two plain nouns written as `N <subclass> M` instead of a rule (DEC-PATHS (c), not taken: native `X ~ M` queries would miss instances, and a sentence's meaning would depend on a configuration);
- if the front end ever writes P279 facts, the closure index needs incremental maintenance, not only the per-predicate invalidation it has.

*Hook:* every class pattern comes from one function of the plan (A16).

**B32: multi-level negation.** The engine already keys negation levels by pattern, reports them with `(zelph/strata)`, and keeps negation records with `(zelph/defeated)`; revising a conclusion under "not provably" (truth maintenance), levels that depend on data, and a variant for paths are not built ("A conclusion that a later fact withdraws" and "Negation levels that compare objects alongside rules over predicates" in [Where the logic goes next](../about.md#where-the-logic-goes-next)). In v1 the three shapes stay refused (A12; [Shapes zelph Cannot Yet Evaluate Soundly](constructs.md#shapes-zelph-cannot-yet-evaluate-soundly)). The later layer is a narrowing (planned, not v1): a helper leaves out each context condition that a negated rule's conclusion may derive, and a component that cannot be ordered, made only of the front end's rules, is accepted when no rule with "not provably" in it may reach a negated pattern of a rule of the same component. Every theory the engine calls orderable was right in every run (0 wrong of 3,912 runs), and every unorderable shape without such a feed was right too (0 wrong of 3,614). The narrowing would accept, in the core profile, the chains, the universal in a condition, "not provably" inside "not provably", and hierarchy feedback into a helper's context; in the Wikidata form, where class patterns are keyed together, chains of class conclusions would stay refused. *Hooks:* `:rule/in-body`, `:naf/nested-in-body` and `:naf/unstratified` refused by id; the may-derive check is one function (A12); the engine's verdict is read in one place, the dry run.

### Math, User Experience and Host

**B25: math extensions.** Value and comparison rules as a general stdlib module are in v1 at M5: `stdlib/evaluation.zph` with `exquo`, `difference` and `compare` ([DEC-MATH-MODULE](decisions.md#dec-math-module)). Units as opaque constants: `:data/measured` stays refused, zelph has no units or reals. Exact rationals: rejected for ACE (C11). *Hook:* [Numbers and Formulas](math.md), [Not in v1](math.md#not-in-v1).

**B26: predictive editing in the playground, colour by word class.** About 20 lines of C++ for a WebAssembly export without ACE knowledge and 100 to 150 lines of JavaScript (estimates); the colours must not break copy and paste. *Hook:* `ace/expected` (B4).

**B27: further front ends through the kit.** Logical English templates are not ACE and would get their own keyword; `sparql.zph` adopts the kit for positioned errors as a change of its own. *Hook:* A10.

**B28: a hint for a missing import.** ACE typed without `.import ace` silently becomes native facts. A general host hint, reading a keyword list from a stdlib data file, is not needed by the front end and is not scheduled; after `.import ace` a node named `ace` prints quoted, so the keyword is not shadowed. *Hook:* none in the front end.

**B31: extended ACE through a public construct API.** A public `ace/construct` in the manner of `math-syntax/operator`, with a strict-mode switch and documentation that marks extended ACE as "not ACE 6.7"; user builders could break ordering or unambiguity. *Hook:* builders keyed by construct id; `ace/construct` stays a read accessor in v1.

**B33: native input that meets both namespaces.** A typed name that is absent in the current language could fall back, read-only and never creating, to `zelph`, like the existing fallback to core spellings. `.lang ace` would then be a full working language for native users (ACE content plus the stdlib and the `ace:` vocabulary), and every `ace/zelph-of` line would re-enter natively with no new node (A6, F20). A host change in name reading; not scheduled. *Hook:* none in the front end.

### The Hook Table

Every hook is a requirement on v1. Two of them are stated only on this page and are adopted by their owner when it is built; they are marked.

| Hook | For | Owner |
|---|---|---|
| `ace/drs-equivalent?` is total and pure | B1 | [Matcher](tests.md#matcher) |
| `ace/load-ulex!` with `{:replace true}`; the lexicon cache notices user entries a drop removed or a run derived, not only a change of `zelph/epoch` | B2, B17 | [Cache and freshness](grammar.md#cache-and-freshness) |
| `ace/last-work` over the counting grammar; the rule data reachable as data, reading tags as literals | B3 | [ace-grammar](api.md#ace-grammar), [Work Measure](tests.md#work-measure) |
| `ace/expected` is the error function | B4, B26 | [Syntax errors](grammar.md#syntax-errors) |
| atomic rejection; `%` and `.` lines inside a block are ACE text | B5 | [All or Nothing: the Dry Run](block.md#all-or-nothing-the-dry-run), [Where a Block Ends](block.md#where-a-block-ends) |
| `:examples` and `:zelph` per construct row; the round trip run per supported row | A2, B8 | [One Table for Code, Tests and Documentation](constructs.md#one-table-for-code-tests-and-documentation), [RT-2](tests.md#rt-2-the-verbaliser-reads-back) |
| provenance keyed by node, rules included, with verbatim text and block, persisted in node annotations | A7, B9, B13 | [Provenance and the Rule Registry](semantics.md#provenance-and-the-rule-registry); the engine store: [Proofs, provenance and retraction as data](foundation.md#proofs-provenance-and-retraction-as-data) |
| `ace/predicate-name` invertible over the lexicon (SHOULD; stated only here); a symbol → its first declared form (`ace/inflect`) | A5, B10 | [Node Names](grammar.md#node-names), [Predicate Names](api.md#predicate-names) |
| one resolution step (`ace/concept!`, `ace/concept-node!`) for every content word | A3, A4, A15, B15, B16 | [The Four Steps](words.md#the-four-steps), [Word Resolution](api.md#word-resolution) |
| readings carry class attributes the plan reads (`:math`, preposition classes), from both stores (stated only here as a general rule; `:math` on shipped readings is specified with the math vocabulary) | A9, B19 | [Readings of a token](grammar.md#readings-of-a-token), [Vocabulary](math.md#vocabulary) |
| the branch for unresolved definites is one private function | B20 | [The Stages](semantics.md#the-stages) (`ace-drs/unresolved-definite`), [Resolving Anaphora](semantics.md#resolving-anaphora) |
| question plans are data | B21 | [Plan](api.md#plan) |
| refused rows by id; the DRS keeps every box | B22, B23, B24, B32 | [Without a Route in v1](constructs.md#without-a-route-in-v1), [The Classifier](semantics.md#the-classifier) |
| P-drop-delta built before any front-end code runs inference inside a scratch cluster (it is built) | A8, B14 | [Scratch clusters and the dry run](foundation.md#scratch-clusters-and-the-dry-run) |
| every class item from one function of the plan, expanded over the instance and subclass predicates in the resolution step; the explicit closure rendering | A16, B29 | [Class Patterns and the Graph Profile](semantics.md#class-patterns-and-the-graph-profile) |
| a statement in an argument position of an opaque predicate has one plan form, `(:stmt Atom)` | A17, B24 | [Modality and That-Clauses](semantics.md#modality-and-that-clauses); the engine side: [Statements about statements: opaque predicates](foundation.md#statements-about-statements-opaque-predicates) |
| the may-derive check of `:naf/unstratified` is one function (`ace-drs/may-derive`) | A12, B32 | [Static Checks](semantics.md#static-checks) |
| builders keyed by construct id; `ace/construct` read-only | B31 | [The Janet API](constructs.md#the-janet-api), [The Grammar](grammar.md#the-grammar) |
| `keyword-kit.zph` holds no ACE knowledge | A10, B27 | [keyword-kit](architecture.md#keyword-kit) |
| `ace/execute!` returns the nodes it built; one `zelph/var` per `(:var Name)` per naming scope | A6, A11 | [The Result of ace/execute!](api.md#the-result-of-aceexecute), [Variables](api.md#variables) |

## Routes Not Taken

Each of these was considered and rejected on evidence. None is proposed again without new evidence.

**C1: APE at run time, as a swappable back end, or keyword handlers in the event loop.** A keyword handler runs outside Janet's event loop: it may start a process but cannot wait for it, read a pipe or use `ev/*`. The WebAssembly build has no processes, and APE is packaged nowhere. Running APE beside zelph would be allowed by its licence; porting its grammar rule by rule is ruled out by the clean-room approach ([APE: a Development-Time Oracle](architecture.md#ape-a-development-time-oracle), [The Clean Room](architecture.md#the-clean-room)). The DRS pivot (A1) keeps the door open.

**C2: parsing as deduction.** zelph rules can parse: a prototype found every reading, but took 47 ms per 200 sentences with 5 rules against 0.8 ms for the PEG (zelph 1.0.1); errors reduce to "the longest derivable prefix"; building the semantics inside rules is hard; the token facts need a scratch cluster.

**C3: the ACE grammar as zelph facts.** Terminals can be read from facts and compiled into a PEG (F9), but productions with feature parameters and the builders are Janet code; function words become nodes in the user's namespace; and nothing would consume the facts. What remains useful: the construct table may export a read-only description for the documentation, outside the graph.

**C4: the full DRS in the graph, anaphora as zelph rules.** "The closest preceding accessible antecedent" is a minimisation over textual order, which needs negation as failure over levels that ACE's own predicates break (A12); the graph would hold two representations; the work is large. What is useful survives as DEC-MEANING (b) (A4) and B17.

**C5: embedding the term grammar of `math-syntax` in ACE.** The same characters mean different things: in ACE `2^3^2` is 64 and `-2^2` is 4, while zelph's term island reads `$( 2^3^2 )` as `2^(3^2)`, which simplifies to 512, and `$( -2^2 )` as −4 (checked on the current build). The island does not fail; it silently computes another value. ACE has its own term grammar and builds terms node-identical to the island's where the meanings agree; operator extensions reach ACE through vocabulary (B19).

**C6: engine witnesses or anonymous nodes for top-level individuals.** Witnesses no longer multiply, but the witness set depends on the order of firing and is unique only up to homomorphism, and a witness made before a named fact arrives is kept ([Fresh Variables](../logic.md#fresh-variables-generative-rules)); anonymous nodes print as `??` or as their class and do not re-enter; they have no name for later anaphora; each sentence would leave a rule. Named individuals round-trip (A14). Fresh variables inside rules are not affected: `:rule/generative-head` is supported from M4, and so is a conclusion connected to nothing its condition binds (`:rule/disconnected-head`): "If a man waits then a dog barks." makes exactly one dog for any number of waiting men, in every semi-naive mode and across runs, which is the reading ∀x(… → ∃d …) asks for ([DEC-DISCONNECTED](decisions.md#dec-disconnected)).

**C7: un-saying an earlier block by dropping its cluster.** Dropping a cluster takes with it the nodes it created first, and with them later blocks' facts about those nodes; entering the block again does not bring them back:

```
zelph> .cluster b1
Active cluster: b1
zelph> John ~ man
zelph> .cluster b2
Active cluster: b2
zelph> John ~ farmer
zelph> .cluster default
Active cluster: default
zelph> .cluster-drop b1
Dropped cluster b1: removed 4 node(s).
zelph> X ~ farmer
zelph> .cluster b1
Active cluster: b1
zelph> John ~ man
zelph> .cluster default
Active cluster: default
zelph> John ~ Y
Answer: John ~ man
```

`John ~ farmer`, stated in `b2`, went with `b1`'s drop because `John` was created in `b1`. Minting names outside the block's cluster first protects later blocks but leaves the dropped block's derivations behind. Un-saying needs truth maintenance in the engine, outside this work. What stays: rollback within one handler call (A8), what-if checks (B14), and the retraction of single fact statements by the block keyword `ace-retract` at M9 ([DEC-RETRACT](decisions.md#dec-retract)). `ace-retract` removes each atom with `zelph/prune-fact` and refuses with `[ace/retract-dependents]`, removing nothing, while a derived fact depends on an atom or another statement nests it, so no stale derivation and no dangling modifier remains ([Taking a Statement Back: ace-retract](block.md#taking-a-statement-back-ace-retract)).

**C8: ACE about its own words** (`"dog" is a noun.`). It gives a sentence a meaning APE does not give it, mixing the object level with the level of the language; the words needed to start would have to be declared beforehand. Declarations go through `ace-lexicon` blocks, `ace/load-ulex!` or prefixes.

**C9: morphology as zelph rules over character lists.** Janet generated 38,935 forms in 40 ms (zelph 1.0.1); forward chaining would materialise every form and is much slower.

**C10: an ACE prelude compiled with magic sets.** Without the demand transformation, the formula variables of a prelude rule stay unbound; the `:math` readings of `ace-math`'s vocabulary (B19) do the job.

**C11: exact rationals for ACE's `/`.** A greatest common divisor through rules, sign normalisation, three number substrates: it belongs to the roadmap of the math stack. ACE's `/` maps to `exquo`, the exact quotient, which is silent when the division is inexact or by zero and never truncates ([The Evaluation Module](math.md#the-evaluation-module)).

**C12: provenance as zelph facts about rules.** A fact that names a rule node turns the rule off (F2; A7's recording). Facts about plain fact nodes are harmless, which was DEC-PROV (b) for facts only, not taken (A7). Node annotations store no fact at all.

**C13: modifiers on the nested fact.** Hash-consing merges every event with the same subject, verb and object, so after "John waits in Paris. John waits fast in Rome." the representation says John waited fast in Paris (A13's recording). DEC-EVENTS offers neither this nor dropping the modifiers.

**C14: mentions by a tautological rule; dropping the modal operator.** `F => F` built in a rule thunk leaves one visible rule per mention in `.list-rules`, fails silently when F was built outside the thunk, and fails when a statement about F comes from a `%` block, which claims F again. A tautology written inside the statement, `john believes ((mary waits nil) => (mary waits nil))`, mentions F with no visible rule, but it is no clean representation: the object is the `=>` node, so plain conditions do not match it, and rules over rules read it as a rule. Dropping the operator is unsound: "John can fly." would assert that John flies. Opaque predicates (A17) replace both.

**C15: the grammar generated mechanically from the construct rows.** ACE's constructs interact (coordination, relative clauses, gaps); only rule builders with feature parameters were verified; and committed choice makes the order of row fragments significant. Rows own the inventory, builders the structure (A2).

**C16: a list of antecedents across the whole session.** It is not saved by `.save`; it survives a `.load` that replaces the graph, because the Janet state does, so it would name nodes of another graph; and an imported script would see the REPL's blocks. One block is one APE paragraph; definite descriptions resolved against the graph are B20.

**C17: resolving words through the importer's single English holder.** The label route itself is the design now (A15). What stays rejected is reading the networks imported before the importer change through their one-holder name map: the holder is decided by the dump's line order and is the intended item for 0 of 24 test labels, 31 of 88 frequent nouns and 0 of 27 names (F18, F19). On a network without the importer's marker for current labels, step 3 is off and `[ace/w-old-labels]` says so.

**C18: core `~` as P31.** `.name ~ wikidata P31` would merge and re-create all 7,034,880 P31 facts of the small Wikidata network (far more on the full graph) and change how core `~` prints everywhere (F21). The instance predicate of A16 reaches P31 without touching `~`.

## The Facts the Routes Rest On

Every route above rests on facts about the engine that were established by running zelph. The table restates them in plain words, with their state on the current build; where a recording on this page shows a fact, the table says where.

| Id | Fact | On the current build | Routes |
|---|---|---|---|
| F1 | A rule or query variable is a single capital letter or a name starting with `_`; `X1` is a constant, so a rule written with it never fires, and a printed rule with it re-enters as a different rule | holds | A6, A11 |
| F2 | A fact that names a rule node, as subject or object, turns the rule off; a fact about a plain fact node is harmless; a typed mention of a rule with variables builds a node of its own and leaves the typed rule in force. Documented semantics | holds (A7's recording) | A7, B21, C12 |
| F3 | A rule built inside an active cluster is recorded there; dropping an earlier block's cluster destroys later blocks' facts on nodes it created, and entering the block again does not restore them; a what-if (assert, run, read, drop in one call) is sound; clusters are not saved | holds (C7's recording) | A8, B14, C7 |
| F4 | Counting nodes cannot see whether a fact is claimed: a ground rule pattern built outside the rule's thunk is claimed, yet native re-entry adds no node. `zelph/rule` with two consequences once built two rules with separate fresh witnesses where the typed rule is one | the first sentence holds (re-run); `zelph/rule` now writes one `=>`, but it does no deduplication and returns the condition set, so the front end builds its rules with `zelph/dedup-rule` | A6 |
| F5 | Janet reads the graph without growing it: a query in a scratch cluster, then a drop, returns every fact about a node, derived ones included. No query lists a rule with a variable (by design); `zelph/rules` does | holds; `zelph/rules` built | B10 |
| F6 | One concept carries names in several naming languages (`.name human ace-de Mensch`); a rule built from the German names fires on native data | holds | A3, B16 |
| F7 | Janet of another module is callable: SPARQL's evaluator, called from outside `sparql.zph`, returns rows | holds | B10, B11 |
| F8 | After `.import math` a fact with the predicate `of` prints as a function application | holds (A9's recording) | A9 |
| F9 | Grammar terminals can be read from graph facts and compiled into a PEG; the words become nodes in the user's language | holds | C3 |
| F10 | Inference inside a cluster that is dropped left the incremental run stale for rules without a negated condition | fixed by P-drop-delta (A8's recording) | A8, B14 |
| F11 | Every `zelph/var` call makes a new variable, whatever its name: two `(zelph/var "C")` in one query are two variables | holds (documented in [janet.md](../janet.md#the-scope-of-a-variable-symbol-one-block)) | A6, A11, A16 |
| F12 | zelph refuses negation over a group of conditions; a helper rule that names the group as a relation works in one run whatever the rule order, and the helper relation answers queries | holds (A12's recording) | A12, B32 |
| F13 | Modifiers written on the nested fact merge events; occurrence individuals keep them apart, and the plain question still answers | holds (A13's recording) | A13, C13 |
| F14 | A rule generator with a ground outer condition is a switch; with a referent in the outer condition it makes one rule per binding (301 rules, 2742 nodes against 1 rule, 2139 nodes for 300 men and 5 dogs, the same 1500 facts); a generator whose inner rule is fully ground never fires and, typed natively, turns off an identical ground rule typed earlier; when either of the two is built from Janet, that rule stays in force | holds (A11's recording; counts and the ground case re-run with the one-element condition sets) | A11 |
| F15 | A class question does not follow a subclass hierarchy; the explicit closure form answers, natively and from Janet; the `∗` form does not parse after a quoted predicate | holds (A16's recording) | A16 |
| F16 | Every fact nested in a plain statement outside a `=>` is claimed; a rule mentioned inside a statement claims none of its ground parts | holds (A17's recording) | A17, C14 |
| F17 | A cardinality bound is a denial with `!=`; a repeated fact does not fire it | holds (recording under [Routes Absorbed Into the Baseline](#routes-absorbed-into-the-baseline)) | absorbed routes |
| F18 | After a Wikidata import, one English label used to name one item: the item processed last took a shared label and every other carrier lost its English name; items whose name Wikidata keeps only as `mul` got none | fixed in the importer (P-wd-labels); networks imported before the change still show it | A15, C17 |
| F19 | Choosing among the items that carry a label: the lowest Q-number is right for 78 of 88 frequent nouns and 25 of 27 names; instance counts are no better and cost time proportional to the degree; sitelinks are not in the graph; for 12 of 100 frequent nouns no single sense could be named | measured on the full graph | A15, B15 |
| F20 | A typed line is read in one naming language: content named in `ace` re-enters with no new node under `.lang ace`, internal names in `zelph` do not | holds (A6's recording) | A6, B33 |
| F21 | Wikidata's instance facts are `X P31 C` with the ordinary node P31; a class atom `X ~ C` matches none of them | holds (A16's recording) | A16, C18 |
| F22 | Who claims: a typed statement, `zelph/fact` and `zelph/refute` (from a `%` block, a keyword handler or the C API) and a derivation claim; a statement written inside a mentioned `=>` stays unclaimed and is matched only by a condition with the same `=>` shape; a `=>` written with `zelph/mention` is not in force until something claims it | holds; the last clause is [DEC-MENTION-RULE](decisions.md#dec-mention-rule), checked on the current build | A17, C14 |
| F23 | A mentioned rule with variables is invisible to queries and conditions; a firing never derives a consequence that keeps a variable of a mentioned rule whose conditions are a set, and with a single condition the variables become fresh witnesses | holds; documented as limits ("Quoting a rule" and "A consequence that mentions a rule with variables" in [Where the logic goes next](../about.md#where-the-logic-goes-next)) | A17, B24 |

## What This Builds On in zelph

The routes use zelph 1.1.0 as it is documented:

- clusters as transactional workspaces, for the dry run, questions and what-if checks: [Node Clusters](../rules.md#node-clusters-transactional-workspaces), [Scoped work: clusters](../janet.md#scoped-work-clusters), [Reasoning incrementally](../janet.md#reasoning-incrementally), [Running quietly](../janet.md#running-quietly);
- node annotations for provenance: [Node annotations](../janet.md#node-annotations);
- rules that write rules for nested conditionals: [Rules That Write Rules](../rule-generators.md), [Rules That Derive Rules](../logic.md#rules-that-derive-rules);
- negation as failure, negation levels and inequality guards for helpers and cardinality: [Negation as Failure](../logic.md#negation-as-failure), [Stratified Evaluation](../logic.md#stratified-evaluation), [Inequality Constraints](../logic.md#inequality-constraints);
- deep unification and opaque predicates for events and statements about statements: [Deep Unification](../logic.md#deep-unification), [Mentioning a Rule Is Not Asserting It](../logic.md#mentioning-a-rule-is-not-asserting-it), [Mentioning a Statement: Opaque Predicates](../logic.md#mentioning-a-statement-opaque-predicates);
- path conditions for the subclass closure: [Transitive Path Conditions](../logic.md#transitive-path-conditions);
- name lookups that create nothing, for word resolution: [Looking Names Up](../janet.md#looking-names-up-zelphlookup-and-zelphlang);
- the precedent of a keyword language layered on the Janet API: [SPARQL Queries](../sparql.md#why-an-import-is-required);
- the engine's own plans that later layers would use: [Where the logic goes next](../about.md#where-the-logic-goes-next).

[What zelph Already Provides](foundation.md) lists the engine items that were built for the front end, and [What zelph Still Needs](prerequisites.md) the bounded remainder that some routes wait for.
