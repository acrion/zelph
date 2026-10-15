# zelph Features Through ACE

This page goes through zelph's features one by one and says, for each of them, how the ACE front end
reaches it. For a feature a user can name, there are four possible answers. An ACE sentence writes or
reads the feature, from a named milestone on. Or ACE can express the sentence, but the front end
refuses it by construct id, naming the milestone that brings it or the reason why it stays out. Or no
ACE sentence can reach the feature by design, and the page says why. Or no sentence writes it, but
native zelph uses the feature on knowledge that ACE sentences built, which the page calls interop.
The remaining rows describe engine behaviour that applies to ACE blocks unchanged, and engine items
the front end uses, obeys or still needs.

The reason this works is the central design choice of the front end: it adds no reasoning of its
own. Every ACE sentence becomes ordinary zelph facts and rules, written by stdlib `.zph` modules
through zelph's public Janet API, in the same way `stdlib/sparql.zph` layers SPARQL on top of the
engine. Whatever zelph does with facts and rules, it therefore also does with ACE-built knowledge,
and where ACE has no sentence for a feature, a native zelph line still works on the same nodes. The
second half of the page takes one question that ACE raises in many forms, the nested condition, and
lists every sense of it with its route.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John is a man.

    zelph> X ~ man
    zelph> .lang ace
    ace> X ~ man
    Answer: John ~ man
    ace> (X ~ man) => (X is mortal)
    (John is mortal) ⇐ (John ~ man)
    ace> Bill ~ man
    (Bill is mortal) ⇐ (Bill ~ man)
    ace> ace Who is mortal?

    Answer: Bill is mortal.
    Answer: John is mortal.
    ```

The ACE sentence writes the fact `John ~ man`. A native query under the default language finds
nothing, because `ace` names its content in a language of its own, `ace`; after `.lang ace` (the
prompt then names the language) the native query finds the fact, a native rule fires on it, and a
native fact about Bill is answered by an ACE question. The adjective "mortal" is the object of the
content predicate `is`, which is how "John is mortal." is written
([DEC-SHAPES](decisions.md#dec-shapes)), so the native rule derives exactly the fact the ACE question
asks for. The native half of this session runs in zelph today
([Content Lives in Language `ace`](#content-lives-in-language-ace) shows a recording, with the rule
in the form `ace` builds).

## How to Read This Page

Every zelph feature that a user meets has exactly one row in [the matrix](#the-matrix): every
user-visible feature of the pages [Core Concepts](../concepts.md), [Logic and
Computation](../logic.md), [Rules and Inference](../rules.md), [Querying](../queries.md), [Rules
That Write Rules](../rule-generators.md), [Neural Networks in the Graph](../neural.md), [Working on
the Wikidata Class Hierarchy](../class-hierarchy.md) and [Scripting with Janet](../janet.md), every
command of the [quick start's command list](../quickstart.md#full-command-reference), and every
planned item of [the project's outlook](../about.md#where-the-logic-goes-next) that would lift a
refusal. A row has a number (`Z01` to `Z154`, the numbering of the specification, kept so that the
implementing sessions can navigate by it), the feature, an ACE sentence that reaches it, the construct
id or the evidence note behind the route, the milestone, and the status.

| Status | Meaning |
|---|---|
| supported | an ACE sentence writes or reads the feature from the milestone given |
| refused | valid ACE, refused by construct id until the milestone or engine item named ([The Construct Table](constructs.md#the-construct-table)) |
| no route | no ACE sentence reaches the feature, by design; the row gives the reason |
| interop | no ACE sentence writes it, but native use of the feature works on ACE-built knowledge; the row names the evidence |
| transparent | engine or session behaviour that applies to ACE blocks unchanged; nothing to map |
| planned | an engine feature that does not exist yet; the row says what it would open for ACE |
| used by the front end | an engine feature the `ace` modules use themselves, which no sentence names (rule builders, annotations, plan terms) |
| constraint | an engine rule the `ace` modules must obey |
| not built | a category-4 prerequisite: specified, implemented in the funded work ([What zelph Still Needs](prerequisites.md#the-seven-items-at-a-glance)) |

The other ids on this page:

- **Construct ids** such as `:rule/nested` or `:naf/unstratified` name one construct of the
  language; every refusal names one ([Constructs and Refusals](constructs.md)). Messages in the
  sessions follow the layout and the ids of [Messages](block.md#messages); their wording is
  illustrative.
- **Evidence notes** `FM-nested`, `FM-strata`, `FM-qimp`, `FM-paths`, `FM-neq`, `FM-atmost`,
  `FM-groups`, `FM-meta`, `FM-neural`, `FM-retract`, `FM-load` and `FM-modal` name the rows whose route
  needed a design checked against the engine; [the section on them](#evidence-behind-the-designed-rows)
  gives what was checked.
- **Host item ids** such as P-opaque or P-rule-list name engine items that were built before the
  front end, so that the front end can rest on them; they are part of zelph 1.1.0
  ([What zelph Already Provides](foundation.md)). "Built" in a row means such an item.
- **Decision ids** such as DEC-STRATA name a design decision ([Design Decisions](decisions.md)).
- **Category-4 ids** K1-P1, K1-P2, L3-P1, Z2-P1, Z3-P1, Z3-P2 and Y2-P1 name the engine items that
  remain ([What zelph Still Needs](prerequisites.md)).
- **Milestones** M1 to M9 are those of the
  [work plan](plan.md#the-milestones-at-a-glance).

The ACE column uses sentences of our own in the shape of a construct. Where a row rests on a case
of the APE regression corpus, it names the case by its number ("corpus case 2013"); the corpus is
fetched at test time and never quoted in the repository
([APE's Regression Corpus](tests.md#apes-regression-corpus)).

The user documentation of the front end, `ace.md` ([DEC-DOCS-PLACE](decisions.md#dec-docs-place)),
gets a section "zelph and ACE" that is written by hand from this matrix, each entry linking to its
constructs, and grows with every milestone. The construct reference and the list of constructs without
a route are generated from the construct table, never from this page.

## Native zelph on ACE-Built Knowledge

ACE predicates are ordinary nodes. "John likes Mary." writes the fact `John likes Mary`, whose
predicate is the node `likes`; "John is taller than Mary." writes `John "is taller than" Mary`, a
predicate of three words. A native rule, a meta-rule over predicates, a neural condition, a SPARQL
query or `.explain` therefore sees these facts as it sees any other fact, provided it names the same
nodes. That proviso is the one rule of interop.

### Content Lives in Language `ace`

zelph names nodes per language, and `ace` names every content node it creates (nouns, proper names,
predicate phrases, adjectives, individuals) in the language `ace`
([DEC-CONTENT-NS](decisions.md#dec-content-ns)). The reasons:
`en` is Wikidata's label space, where a node `ace` creates for a verb or an individual must never
take or merge a label an item holds; the behaviour is then the same with and without Wikidata; and
the standard library's internal names live in `zelph`, so content never collides with them.

| What | Language |
|---|---|
| content nodes `ace` creates; names a user gives nodes for `ace` | `ace` |
| Wikidata labels (with the importer's `mul` fallback) | `en` |
| Wikidata ids (`Q42`, `P31`) | `wikidata` |
| user lexicon entries and lexicon vocabulary | `ace-lex` |
| internal vocabulary (`refuted`, `opaque`, `ace:individual`, `ace:occurrence`, helper predicates `ace:naf-<h>`, the profile pin), characters, digits, the math stack, rule and query variables | `zelph` |
| core nodes `~ => ! != -> in nil cons conjunction negation` | the same node in every language |

[Naming Languages](api.md#naming-languages) gives the full contract. `ace` runs its execute step under
`.lang zelph`, set and restored through `zelph/lang`, so that what zelph names implicitly (variables,
characters) lands in `zelph`; the user's `.lang` changes only what
the native REPL shows and how it reads typed names, never what `ace` resolves. A native line meets
ACE content under `.lang ace`, and a node named only in `ace` displays by that name in every
language. This run types, under `.lang ace`, the fact and the rule that "John is a man. Every man is
mortal." writes (the rule in the one-element-set form `ace` builds), and asks under both languages:

```
zelph> .lang ace
ace> John ~ man
ace> (*{(X ~ man)} ~ conjunction) => (X is mortal)
(John is mortal) ⇐ {(John ~ man)}
ace> .lang zelph
zelph> X ~ man
zelph> .lang ace
ace> X ~ man
Answer: John ~ man
ace> X is mortal
Answer: John is mortal
```

Under `.lang zelph` the typed `man` is another node than the `man` in `ace`, so the query finds
nothing; under `.lang ace` it does. Two consequences follow for native lines:

- A native line that needs both ACE content and internal vocabulary, such as a rule over refuted
  ACE facts or an opaque declaration (`refuted` and `opaque` live in `zelph`) or a rule over ACE
  facts and the math stack, cannot be typed under one `.lang`; typed under `.lang ace`, `opaque`
  names another node, and the declaration declares nothing. Such a line is written from Janet,
  naming each node with its language (`(zelph/resolve "nn-layers" "zelph")`), as the neural example
  below does. An interop row whose native side needs both says so.
- A `.name` onto a name another node already holds merges the two nodes, also within one language;
  for words, which are atoms, the merge always happens
  ([DEC-MERGE-CONTENT](decisions.md#dec-merge-content)). `ace` never calls `.name`, and its
  documentation names the lexicon as the way to bind a word for `ace`
  ([Choosing Another Sense](words.md#choosing-another-sense)).

### Native Rules and Meta-Rules

No ACE sentence writes a meta-rule (Z24): ACE is first-order, and verbs are not noun phrases, so no
sentence quantifies over a predicate. A native [meta-rule](../logic.md#meta-rules-predicates-as-first-class-nodes)
typed under `.lang ace` fires on ACE facts all the same:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> .lang ace
    ace> ((X R Y), (R is symmetric)) => (Y R X)
    ace> marries is symmetric
    ace> ace John marries Mary. Who marries John?

    Answer: Mary marries John.
    ```

"John marries Mary." writes `John marries Mary`, whose predicate is the node `marries` in `ace`, the
one the native line `marries is symmetric` named. The block holds a question, so `ace` runs its quiet
inference first, in which the meta-rule derives `Mary marries John`; the question then finds it. The
native side, with the facts typed as the sentences write them:

```
zelph> .lang ace
ace> John "is taller than" Mary
ace> Mary "is taller than" Bill
ace> "is taller than" is transitive
ace> ((X R Y), (R is transitive), (Y R Z)) => (X R Z)
(John "is taller than" Bill) ⇐ {("is taller than" is transitive) (Mary "is taller than" Bill) (John "is taller than" Mary)}
ace> John "is taller than" X
Answer: John "is taller than" Bill
Answer: John "is taller than" Mary
ace> John marries Mary
ace> marries is symmetric
ace> ((X R Y), (R is symmetric)) => (Y R X)
(Mary marries John) ⇐ {(John marries Mary) (marries is symmetric)}
ace> X marries John
Answer: Mary marries John
```

Plain rules work in both directions: the ACE rule "Every man is a human." fires on a native
`John ~ man` typed under `.lang ace`, and a native rule fires on ACE facts, as the first session of
this page shows.

### Neural Conditions

ACE has no graded truth and no construct that names a network, so no sentence writes a
[neural condition](../neural.md#neural-rule-conditions) (Z80). A native `≈` rule fires on ACE-built
facts. Its native side needs the network's vocabulary, which lives in `zelph`, beside content in
`ace`, so the network is declared from Janet:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John knows Mary.

    zelph> .lang ace
    ace> John in KIn
    ace> Mary in KOut
    ace> %(zelph/nn-connect "John" "Mary" 0.9)
    ace> %(zelph/fact "knet" (zelph/resolve "nn-layers" "zelph") (zelph/list "KIn" "KOut"))
    <zelph/node knet nn-layers <KIn KOut>>
    ace> ((A knows B), ≈knet(A knows B)) => (A trusts B)
    (John trusts Mary) ⇐ {((John knows Mary) nn knet) (John knows Mary)}
    ace> ace Does John trust Mary?

    Answer: Yes. John trusts Mary.
    ```

The derived fact carries the net's confidence as its
[fact probability](../neural.md#confidence-becomes-fact-probability), here 0.9. It is claimed like
any derived fact, so the yes/no answer says "Yes." without the probability (Z81); graded answers are
not offered in v1, and the user documentation says so. The native side alone, ending with the
probability read back:

```
zelph> .lang ace
ace> John knows Mary
ace> John in KIn
ace> Mary in KOut
ace> %(zelph/nn-connect "John" "Mary" 0.9)
ace> %(zelph/fact "knet" (zelph/resolve "nn-layers" "zelph") (zelph/list "KIn" "KOut"))
<zelph/node knet nn-layers <KIn KOut>>
ace> ((A knows B), ≈knet(A knows B)) => (A trusts B)
(John trusts Mary) ⇐ {((John knows Mary) nn knet) (John knows Mary)}
ace> X trusts Y
Answer: John trusts Mary
ace> %(zelph/weight (zelph/fact "John" "trusts" "Mary") "trusts")
0.9
```

### SPARQL

ACE facts are ordinary triples, so [SPARQL](../sparql.md) reads them (Z66). Names without a prefix
resolve in the current language, so the query runs under `.lang ace`; on a Wikidata graph the class
facts `ace` writes are P31 facts, which SPARQL reaches as `wdt:P31` whatever the language
([Prefix Handling and Languages](../sparql.md#prefix-handling-and-languages)). The facts below are
what "John likes Mary. Bill likes Mary." writes:

```
zelph> .import sparql
SPARQL subset loaded. Type 'sparql', paste your query, finish with an empty line.
zelph> .lang ace
ace> John likes Mary
ace> Bill likes Mary
ace> sparql
SELECT ?x WHERE { ?x likes Mary . }
?x
John
Bill
-- 2 result(s) --
```

ACE answers for SPARQL rows, each row's instantiated patterns rendered as an ACE sentence, are a
later layer the verbaliser leaves room for
([Output: Uses of the Verbaliser](routes.md#output-uses-of-the-verbaliser)).

### Facts with Several Objects

zelph allows a fact with [several objects](../logic.md#facts-with-multiple-objects), `John sees Mary
Bill`. `ace` never writes one: "John sees each of Mary and Bill." is distributed into one fact per
member (Z13, FM-groups), because two separate facts do not make a two-object fact true, while a
two-object fact does answer for each of its objects. A native multi-object fact therefore answers ACE
questions over one of its objects, wh-questions and yes/no questions alike:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> .lang ace
    ace> John sees Mary Bill
    ace> ace Who does John see? Does John see Mary?

    Answer: John sees Bill.
    Answer: John sees Mary.
    Answer: Yes. John sees Mary.
    ```

A wh-question is a pattern, which matches a wider fact as a rule condition does. A yes/no question
checks each ground atom first with the exact probe `(zelph/exists s p o :exact)`, one hash lookup that
finds only the fact as stated; on a miss it asks the plain `(zelph/exists s p o)`, which reads a fact
with further objects as a rule condition reads it
([DEC-YESNO-WIDER](decisions.md#dec-yesno-wider); [Facts With Several
Objects](answers.md#facts-with-several-objects)). This second probe is the one named exception to the
rule that `ace` calls `zelph/exists` only with `:exact` ([Invariants](api.md#invariants)), and the
source-rule test admits exactly that call. It asks the condition matcher only while the graph may hold
a fact with several objects (one was written, a statement or set constant was merged by name, or a
network was loaded whose file does not record that it holds none, as no file saved by an older engine
does), and then its cost is bounded by the subject's facts with that predicate, never a scan of the graph; a
budget row times it on the `-small` Wikidata network at M3
([Budgets](tests.md#budgets-counts-always-timings-only-on-an-idle-machine)). Otherwise it answers a
miss with a hash lookup too. The native side:

```
zelph> .lang ace
ace> John sees Mary Bill
ace> John sees X
Answer: John sees Mary
Answer: John sees Bill
ace> %(zelph/exists "John" "sees" "Mary")
true
ace> %(zelph/exists "John" "sees" "Mary" :exact)
false
ace> Sue sees Mary
ace> Sue sees Bill
ace> %(zelph/exists "Sue" "sees" "Mary" "Bill")
false
```

The query finds both objects; the plain `zelph/exists` answers true for `John sees Mary`, the exact
probe false; and the two separate facts about Sue do not make the two-object fact true. Inside a
nested statement zelph reads a fact with more objects by the subset reading, at the top level
injectively; one reading at every level is the category-4 item
[Z2-P1](prerequisites.md#z2-p1-one-reading-of-a-patterns-objects-at-every-level), which reaches `ace`
only through such native data (Z149).

### Proofs and `.explain`

Native `.explain` works on facts and rules built from Janet (Z102), and so on everything `ace`
writes; `zelph/explain` returns the same proof as data. The ACE explanations of M9 are built from it:
[justified answers](answers.md#why-justified-answers) and
[the derivation echo](answers.md#the-derivation-echo).

### Traps

What native rules do beside ACE rules is mostly harmless, but four interactions need care. The
front end guards against the first by refusal and documents the others in `ace.md`.

**A native rule and an ACE "not provably" sentence that zelph evaluates together.** `ace` decides
whether its own negation as failure can be ordered over the rules `ace` wrote; native rules are not
in that analysis. From M4 the dry run of a block that writes a rule therefore also asks the engine for
its negation levels, `(zelph/strata)`, and refuses the block as `:naf/unstratified` when a component
that zelph cannot order holds a rule with a "not provably" condition that `ace` wrote together with a
rule `ace` did not write, or while a rule over predicates is in force
([DEC-STRATA](decisions.md#dec-strata)):

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> .lang ace
    ace> (X ~ bachelor) => (X ~ man)
    ace> ace If a man is not provably a husband then the man is a bachelor.

    Error in line "ace If a man is not provably a husband then the man is a bachelor.": Correct
    ACE, but zelph cannot use it [ace/unsupported]: sentence 1, line 1, column 1: "not provably"
    that zelph evaluates together with rules not written in ACE, construct naf/unstratified.
      <>If a man is not provably a husband then the man is a bachelor.
      zelph evaluates this sentence together with rules not written in ACE and cannot put them in
      order, so the answer would depend on the order of the rules.
    ```

zelph prints the error as one line; it is broken here for reading. `:naf/unstratified` is a context
id, for which no single condition is the evidence, so the marker stands before the sentence's first
token ([Positions and the Marker](block.md#positions-and-the-marker)). The front end writes "not
provably a husband" as a positive helper rule, which collects the men who are husbands, and a main
rule that negates the helper's fact. The dry run wrote both into its scratch cluster, found them in
one component with the native rule, a component zelph cannot order, and dropped the cluster: nothing
reached the graph. The same rules typed natively show the engine's verdict, first without and then
with the native rule (the helper is named `naf-husband` here; the front end names it
`ace:naf-<16 hex digits>`):

```
zelph> .lang ace
ace> ((X ~ man), (X ~ husband)) => (X naf-husband nil)
ace> ((X ~ man), ¬(X naf-husband nil)) => (X ~ bachelor)
ace> .strata
1 negation level:
Level 1:
  ((X ~ man), ¬(X naf-husband nil)) => (X ~ bachelor)
ace> (X ~ bachelor) => (X ~ man)
ace> .strata
1 negation level:
Level 1:
  ((X ~ man), ¬(X naf-husband nil)) => (X ~ bachelor)
Not stratifiable, on level 1 -- these rules negate what they derive:
  ((X ~ husband), (X ~ man)) => (X naf-husband nil)
  ((X ~ man), ¬(X naf-husband nil)) => (X ~ bachelor)
  (X ~ bachelor) => (X ~ man)
```

Without the native rule, `.strata` lists the main rule on its negation level and reports nothing it
cannot order, so the sentence is accepted. With it, `.strata` reports all three rules as not
stratifiable: the main rule derives `~ bachelor`, the native
rule turns that into `~ man`, and the helper reads `~ man`, so the negation depends on its own
conclusion. A rule over predicates has the same effect for a different reason: a transitive
meta-rule makes every negation level a predicate level, and beside one an ACE "not provably" rule
whose conclusion is an adjective ("… then the man is happy.") lands on one level with the negation it
tests, where the model is wrong in one rule order. `stdlib/examples/english.zph` holds such a
meta-rule.

The other three are documented:

- **The content predicate `is`.** An adjective is the object of the content predicate `is` ("John is
  rich." writes `John is rich`, DEC-SHAPES), and a native declaration such as `marries is symmetric`
  uses the same node. A native meta-rule over a variable predicate `R` therefore also sees
  `John is rich`.
- **A host run between ACE blocks.** Once `ace` has written its first "not provably" rule it keeps
  negation records, so that a later fact that defeats a "not provably" conclusion is reported as
  `[ace/w-naf-final]` (DEC-NAF-RECORDS;
  [Conclusions Under "Not Provably" That a Later Block Defeats](block.md#conclusions-under-not-provably-that-a-later-block-defeats)).
  A host run between blocks (the auto-run after a native line, `.run`, a `?` line) prints such a
  defeat in zelph syntax and consumes the record, so `ace`'s next quiet run does not report it again.
  The native "not provably" rules of the session are recorded too.
- **A native mention of a ground rule.** A native statement that mentions a plain single-condition
  ground rule turns that rule off, because both are one node ([Mentioning a Rule Is Not Asserting
  It](../logic.md#mentioning-a-rule-is-not-asserting-it)). `ace` builds every one-condition rule from
  Janet as a one-element condition set, which stays in force after a typed mention of the same text,
  unless the user typed the plain rule before: then `ace`'s rule meets that rule, and the mention takes
  it out of force ([DEC-RECORD-SINGLE](decisions.md#dec-record-single)); a typed mention of a rule
  with variables is another node and leaves the ACE rule in force (Z132). The general fix is the
  category-4 item [K1-P1](prerequisites.md#k1-p1-a-mention-switches-a-typed-ground-rule-off).

### Tests That Pin Interop

Every interop row is pinned by a test that builds the ACE side through the `ace` keyword and types the
native side as text under `.lang ace`. From M1 the tests `ace interop: …` pin that a native fact
answers an ACE question and an ACE fact a native query, that native input meets `ace`'s content under
`.lang ace` only, that `.lang` never changes what `ace` resolves and that `ace` restores the user's
language, and that a native rule fires on ACE facts; from M3, the native meta-rule session above and
internal vocabulary under another `.lang`; from M4, an ACE rule firing on native facts. At M3
`ace questions: a native fact with several objects answers a single-member yes/no question` pins
[Facts with Several Objects](#facts-with-several-objects). Three tests of the construct table pin the
engine-verdict clause: the refusal beside a native rule (the session above) and beside a rule over
predicates, both M4, and the acceptance beside the math stack at M5, where the ACE rules stay in
components zelph can order, apart from the 456 rules of `.import ace-math`. The full list, with what
each test asserts, is in
[Re-Entry, Interop, Persistence, Provenance](tests.md#re-entry-interop-persistence-provenance),
[Questions](tests.md#questions) and [The Construct Table](tests.md#the-construct-table).

## Nested Conditions in All Their Senses

ACE nests conditions in many ways: a rule inside a conclusion ("If John waits then every man
sleeps."), a negation inside a condition, a universal inside a question, a belief that holds a rule.
Each sense goes its own way into zelph, and each was decided against what the engine does. The table
at the end of this section ([All Senses at a Glance](#all-senses-at-a-glance)) lists all of them; the
subsections explain the groups. The decisions behind them are summarized in
[Nested Conditions: The Outcome](decisions.md#nested-conditions-the-outcome), and the mapping of each
is in [From Sentences to the Graph](semantics.md#every-construct-and-its-native-form).

### A Conditional in a Conclusion

zelph's [rules that derive rules](../logic.md#rules-that-derive-rules) make a nested conditional
possible as it stands, as a rule generator. `ace` writes one in exactly two cases and flattens every
other nested conditional into one rule ([DEC-NESTED-IMP](decisions.md#dec-nested-imp)):

- **a switch**: the outer condition declares no referent (it is ground, "If John waits …") and the
  inner rule keeps a variable. The generator writes the inner rule once the outer condition holds.
  Applied again inside the inner rule, a chain of switches is a
  [generator of generators](../rule-generators.md#generators-that-generate-generators).
- **an inner rule over a referent the outer conclusion introduces**: "Every man owns a dog that every
  cat hates." The dog is a fresh witness of the outer rule, and the inner rule must name that same
  witness, so no flat rule can express the sentence.

Everything else is flattened: "Every man feeds every dog." is one rule with both conditions, the
comma list `((A), (B)) => (C)`; an inner denial flattens into one denial; a fully ground nested
conditional ("If John waits then if Mary sleeps then Bill runs.") is flattened too.
`ace/option :show-zelph` prints the zelph each block writes:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :show-zelph true)
    true
    zelph> ace If John waits then every man sleeps. Bill is a man. Does Bill sleep?

    (*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
    Bill ~ man
    Answer: It is not provable that Bill sleeps.
    zelph> ace John waits. Does Bill sleep?

    John waits nil
    Answer: Yes. Bill sleeps.
    zelph> ace Every man feeds every dog. Rex is a dog. Does Bill feed Rex?

    ((X ~ man), (Y ~ dog)) => (X feeds Y)
    Rex ~ dog
    Answer: Yes. Bill feeds Rex.
    ```

Every rule `ace` writes is one `=>` fact built inside `zelph/dedup-rule`; a rule with one condition
gets a one-element condition set tagged `~ conjunction`, so that every rule `ace` writes has a
condition set; a contradiction it meets is recorded as that of a plain single condition is
([DEC-RECORD-SINGLE](decisions.md#dec-record-single)); `ace/zelph-of` prints it exactly as zelph
lists it ([DEC-ONE-CONDITION-PRINT](decisions.md#dec-one-condition-print)). The ground outer
condition `John waits nil` is a rule pattern until "John waits." claims it. Rule variables are named
`X`, `Y`, `Z`, `A`, … in order of first occurrence, conditions before conclusions; a class condition
`X ~ man` has no variable of its own and takes no letter, so the flattened rule names its dog `Y`
([DEC-VAR-LETTERS](decisions.md#dec-var-letters); [Variables](api.md#variables)). The native
form of the switch, typed under `.lang ace`, fires level by level: the claim of `John waits nil`
writes the inner rule, which then fires for Bill, and `.list-rules` shows the generated rule beside
the generator:

```
zelph> .lang ace
ace> (*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
ace> Bill ~ man
ace> X sleeps nil
ace> John waits nil
((*{(X ~ man)} ~ conjunction) => (X sleeps nil)) ⇐ {(John waits nil)}
(Bill sleeps nil) ⇐ {(Bill ~ man)}
ace> .list-rules
Listing all rules:
------------------------
(*{(X ~ man)} ~ conjunction) => (X sleeps nil)
(*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
------------------------
ace> X sleeps nil
Answer: Bill sleeps nil
```

The second generator, whose inner rule shares the outer rule's witness:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :show-zelph true)
    true
    zelph> ace Every man owns a dog that every cat hates. John is a man. Tom is a cat.

    (*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog) ((*{(Z ~ cat)} ~ conjunction) => (Z hates Y))
    John ~ man
    Tom ~ cat
    zelph> ace Who owns a dog? What does Tom hate?

    Answer: John owns a dog.
    Answer: Tom hates a dog.
    ```

The conclusions follow the sentence, a relation before the class of the referent it introduces, and
the inner rule's variable takes the next letter. The witness has no name, so an answer renders it as
an indefinite, "a dog". Natively, typed as the block writes it:

```
zelph> .lang ace
ace> (*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog) ((*{(Z ~ cat)} ~ conjunction) => (Z hates Y))
ace> John ~ man
((*{(Z ~ cat)} ~ conjunction) => (Z hates ??)) ⇐ {(John ~ man)}
(John owns ??) ⇐ {(John ~ man)}
ace+> Tom ~ cat
(Tom hates dog) ⇐ {(Tom ~ cat)}
ace> .list-rules
Listing all rules:
------------------------
(*{(Z ~ cat)} ~ conjunction) => (Z hates dog)
(*{(X ~ man)} ~ conjunction) => ((*{(Z ~ cat)} ~ conjunction) => (Z hates Y)) (X owns Y) (Y ~ dog)
------------------------
ace> X owns Y
Answer: John owns dog
ace> X hates Y
Answer: Tom hates dog
```

zelph lists the rule with its conclusions in its own stored order, the inner rule first;
the order of a rule's conclusions does not change the rule. The firing for John writes the witness,
its class fact and one generated rule. In the firing's own lines the fresh witness still prints as
`??`; the `+` in the next prompt marks a deduction the run did not print. Afterwards zelph prints the
unnamed witness by its class: the generated rule lists as `(*{(Z ~ cat)} ~ conjunction) => (Z hates
dog)`, it fires for Tom, and the native queries answer with `dog` where the ACE answers say "a dog".

**Why only these two shapes are generators.** The engine evidence (FM-nested):

- A generator whose inner rule is fully ground never fires: the asserted copy and the mentioned inner
  rule are one hash-consed node. Typed, it also turns off an identical ground rule typed earlier;
  when either is built from Janet, as `ace` builds both, that rule stays in force. Flattened, the
  conditional fires. Keeping ground switches as switches needs the asserted-rule record, the
  category-4 items
  [K1-P1 and K1-P2](prerequisites.md#k1-p1-and-k1-p2-a-record-that-a-rule-was-asserted).
- A generator over a referent of the outer condition makes one generated rule per outer binding: 300
  kings give 301 rules against 1 when flattened, with the same 1500 derived facts and 2739 against 2142
  nodes. The shape is correct (a generated rule claims none of its fresh ground parts), but the cost
  per binding stays, so `ace` flattens it.
- The switch shape and chains of switches fire level by level, claim nothing and re-enter with no
  new node.
- The generator over a then-box referent makes one witness and one generated rule per outer binding:
  two men and one cat give the cat hating each man's dog and three rules in all. That cost has no flat
  alternative.
- In the APE corpus, 85 accepted cases hold a conditional inside a conclusion. Of the 73 occurrences
  directly in a conclusion (61 cases), 14 have an inner rule that uses a referent of the conclusion: 5
  over a group member that M7 expands first, and 9 generators of the second kind. The rest flatten.

`ace/norm-nested-imp` takes the mode as an argument (`:switch`, the default; `:generator`;
`:flatten`), so tests run every mode directly ([Nested Implications](semantics.md#nested-implications)).

### Negation Inside a Condition

ACE has two negations. "does not" is classical negation, "does not provably" negation as failure
(NAF), and the two are never mixed ([DEC-NEG](decisions.md#dec-neg)); a session with both is in
[A Tour in Sessions](sessions.md#negation-no-does-not-not-provably):

| In a condition | zelph | Id | Ms |
|---|---|---|---|
| one atom, classical: "If John does not wait then Mary sleeps." | `((F) ~ refuted)`, which matches refuted facts only | `:neg/atom-in-body` | M4 |
| several conditions, classical (corpus case 3165) | refused, with the hint to say "does not provably" | `:neg/complex-in-body` | — |
| one atom, NAF: "If a man does not provably wait then the man sleeps." | `¬(F)`; over a class atom a positive helper rule, because a class condition may expand to two patterns and `¬` takes one | `:naf/atom-in-body` | M4 |
| several conditions, NAF: "If a man does not provably own a car then the man walks." | a positive helper rule `ace:naf-<h>` that collects the box, and `¬` over its atom | `:naf/complex-in-body` | M4 |

A helper's body must be positive. Its name is content-hashed, so a block entered twice adds no
helper; it is visible in `.list-rules`
([Helper Rules for "not provably"](semantics.md#helper-rules-for-not-provably) gives the
construction).

### Shapes That Need Ordered Negation

Three shapes put a "not provably" inside the helper body or chain such rules, and all three are
refused by id in v1 ([DEC-STRATA](decisions.md#dec-strata)):

| Shape | Example | Id |
|---|---|---|
| a universal inside a condition, also inside a negation | "If every man waits then a dog barks." · "Nobody likes everybody." | `:rule/in-body` |
| NAF inside NAF | "If a man is not provably a farmer who does not provably own a dog then the man is idle." | `:naf/nested-in-body` |
| a "not provably" pattern that a rule with a "not provably" condition can derive, directly, through positive rules or through a class hierarchy | "If a man is not provably married then the man is a bachelor. If a man is not provably a bachelor then the man is a husband." | `:naf/unstratified` |

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace If every man waits then a dog barks.

    Error in line "ace If every man waits then a dog barks.": Correct ACE, but zelph cannot use it
    [ace/unsupported]: sentence 1, line 1, column 10: a universal inside a condition ('man'),
    construct rule/in-body.
      If every <>man waits then a dog barks.
      zelph orders "not provably" over classes only without Wikidata's class data and only where no
      conclusion feeds back; otherwise the answer would depend on the order of the rules.
    ```

The refusal names the construct and writes nothing. The marker stands before "man", the first word of
the condition that is evidence for the construct (the universal inside the if-part). The hint names
what zelph cannot order and no planned feature.

The reason lies in zelph's [stratified evaluation](../logic.md#stratified-evaluation). zelph orders
rules with a negated condition into levels; a component of rules that negate what they derive stays on
one level and alternates, and there a "not provably" test can run before the rule that feeds it, so
the model depends on the rule order. The levels are keyed by pattern: a top-level pattern by its
predicate and constant object, a variable or structured object, a nested statement and a path condition
by the predicate with any object, and every pattern by its predicate alone while a rule over predicates
is in force. What that means for the three shapes:

- Without Wikidata, where a class atom is `X ~ C`, the three shapes get separate levels and the right
  model in every one-run order (nine of nine one-run scenarios in every evaluation mode, where
  before the levels were keyed by pattern three to five of nine were wrong).
- In the Wikidata form a class pattern reads (P31, any), so every class-headed "not provably" rule
  whose helper reads a class shares one component with every class rule, and the model is wrong in
  some or all orders (the bachelor and husband chain right in 0 of 96 block runs; the farmer and free
  pair in 0 of 24). Beside a rule over predicates the core form falls back to predicate keys too.
- A class hierarchy that feeds a NAF conclusion back into what a NAF rule tests is wrong in every run,
  deterministically: with "Every husband is a man." beside the bachelor and husband pair, John becomes
  a husband in 2,880 of 2,880 block runs. "Every bachelor is a man." beside the same pair is right in
  every run (2,656 of 2,656), because its cycle passes only through the helper's context pattern; v1
  refuses it anyway, which is sound and stricter than needed.
- A NAF conclusion is final across runs: a later block neither retracts it nor draws what it blocked
  (Z43).

Accepting the shapes by profile, by the kind of conclusion or by the engine's verdict would be
arbitrary for the user or unsound, so v1 refuses all three. `:rule/in-body` and `:naf/nested-in-body`
are found on the DRS. `:naf/unstratified` is found twice: in the resolution step, over the may-derive
graph of the ACE rules (through the graph's upward subclass walk, so a block can be refused on a
Wikidata graph and accepted on an empty one), and in the dry run, from the engine's verdict, for the
cases `ace`'s own graph cannot see ([Traps](#traps)).
["Not Provably" That Depends on Another "Not Provably"](constructs.md#not-provably-that-depends-on-another-not-provably)
gives both checks in full, with real runs of the chain. The engine's verdict is never an acceptance
criterion: zelph calls the hierarchy feedback, wrong in every run, and the "Every bachelor is a man."
theory, right in every run, alike non-stratifiable.

**Planned, not v1.** A narrowing of the refusals would accept the cases zelph can order: a helper
would leave out each context pattern that a NAF conclusion may derive, and a non-stratifiable
component of `ace`'s rules alone would be accepted when no NAF rule of it can reach one of its NAF
patterns. The evidence for it: every theory the engine calls stratifiable was right in every run (0
wrong of 3,912 runs), every non-stratifiable shape in which no NAF rule feeds one of its NAF patterns
was right (0 wrong of 3,614), and every shape with such a feed was wrong in some or all orders. The
core form would then accept the chains, the universal in a condition, NAF in NAF and hierarchy
feedback into a helper's context; the Wikidata form would still refuse the class-conclusion chains.
Lifting the refusals is a decision of its own, outside v1. In the corpus, 44 accepted cases carry
`:rule/in-body`, 13 of them a universal inside an if-box; lifting `:rule/in-body` and
`:naf/nested-in-body` would move 14 cases into M4's gate and needs a mapping for a universal inside a
negated box.

### A Conditional Inside a Question

A yes/no question can hold a universal: "Is it true that every man is mortal?" asks whether a rule
follows from what the graph holds.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Every man is a human. Every human is mortal. John is a man.
    Is it true that every man is mortal?

    Answer: Yes. If there is a man X then X is mortal.
    ```

`ace` answers by a guarded hypothetical check ([DEC-QUNIV](decisions.md#dec-quniv),
`:rule/in-question`, M4) in the question's scratch cluster `ace:q`. A counter-instance among the
graph's own individuals, a man whose mortality is refuted, gives "No. It is false that …". Otherwise a
hypothetical man `ace:hyp-<h>` is assumed and one quiet inference runs: when the then-part holds for
him, the answer is "Yes. …"; otherwise, and also when that run meets a contradiction, "It is not
provable that …". The cluster is dropped, so nothing of the hypothesis remains. A fresh witness is
"not provably" everything, so a rule with a NAF or `!=` condition reachable from the hypothesis would
make "Yes" wrong: the resolution step reads the rules in force with `zelph/rules` (P-rule-list),
native rules included, and refuses such a question as `:rule/unsafe-hypothesis` before anything is
written. [Questions That Hold a Conditional](answers.md#questions-that-hold-a-conditional) gives the
procedure, its plan and a real run of the check in today's zelph.

A conditional inside a wh-question ("How does every woman sleep?") has no route
(`:rule/in-wh-question`): the witness can decide whether every A is a C, not which individuals answer
a question that holds a universal. In the corpus, 30 accepted questions hold a conditional, 10 yes/no
and 20 with a question word.

### Conditions Over Classes, Formulas, Events, Counts and Groups

- **A class atom over subclass data** ("If a writer is not provably a human then the writer is
  suspicious.", M4): with a subclass predicate a class condition becomes `X <instance> D, ((D
  <subclass> C) closure zero-or-more)`, under NAF through a positive helper (DEC-PATHS, FM-paths);
  [Classes: the Instance and Subclass Predicates](words.md#classes-the-instance-and-subclass-predicates).
- **A formula** ("If a person's age is A and A > 17 then the person is an adult.", M5): a seed rule and
  a `compare` condition, and no class condition `A ~ age` on the number
  ([DEC-NUMERIC-REFERENT](decisions.md#dec-numeric-referent)); a negated formula is the formula's
  mapping with the polarity flipped, every side that is neither a variable nor a number literal bound
  to its value first, never a helper or a denial (`:naf/formula`, `:neg/formula`);
  [Formulas in Rules](math.md#formulas-in-rules).
- **Identity and inequality** ("If a man X likes a man Y and X \= Y then X is generous.", M5): a `!=`
  guard; `=` with a referent side substitutes; a false ground condition makes the rule vacuous,
  `[ace/w-vacuous]` (FM-neq; [Identity](semantics.md#identity)).
- **An event** ("If a man waits in a park then the man is a visitor.", M6): `E ace:occurrence (X waits
  nil), E "is in" Y`, matched by [deep unification](../logic.md#deep-unification) (DEC-EVENTS;
  [Events and Modifiers](semantics.md#events-and-modifiers)).
- **A count** ("If John owns at most 1 car then John is modest.", M7): a helper over n+1 pairwise
  distinct witnesses that skip `ace`'s own individuals, then `¬`; "at least" puts the distinct
  witnesses into the body (FM-atmost).
- **The members of a group** ("For each of John and Mary a cat likes him/her.", M7): expanded over the
  members; a condition on members of a group term is `M in G` over set constants (FM-groups;
  [Plurals, Groups, Cardinality and Mass](semantics.md#plurals-groups-cardinality-and-mass), which
  also has the counts).

### Statements About Statements

zelph's [opaque predicates](../logic.md#mentioning-a-statement-opaque-predicates) let a fact hold a
statement without asserting it. From M8 the verb of a that-clause and the modal predicates `ace:can`,
`ace:must`, `ace:should`, `ace:may` are opaque ([DEC-MODAL](decisions.md#dec-modal), FM-modal):

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :show-zelph true)
    true
    zelph> ace John believes that Mary sleeps. If John believes that Mary sleeps then John waits.
    Does Mary sleep? Does John wait?

    believes ~ opaque
    John believes (Mary sleeps nil)
    (*{(John believes (Mary sleeps nil))} ~ conjunction) => (John waits nil)
    Answer: It is not provable that Mary sleeps.
    Answer: Yes. John waits.
    ```

The declaration `believes ~ opaque` names the internal node `opaque` in `zelph`, so it is shown but
does not re-enter as typed under `.lang ace`; `ace` writes it from Janet at the verb's first
that-clause use. [Modality and That-Clauses](semantics.md#modality-and-that-clauses) has the mapping
and a real run of the native form. The senses:

- **In a condition** ("If John believes that Mary sleeps then John waits.", `:sub/that-clause`;
  "If John can swim then John is happy.", `:modal/can`): an ordinary condition, matched inside the
  opaque fact by deep unification. A box with several conditions or a referent is stored as the set
  constant of its atoms as mentions and matched by its members (P-set-match).
- **A conditional inside a that-clause** ("John believes that if a man waits then the man sleeps."):
  at the top level a mention of a rule, built as a condition set with `zelph/mention` inside the
  opaque statement that holds it. A `=>` written this way is not in force until something claims it,
  and a rule over rules does not read it ([DEC-MENTION-RULE](decisions.md#dec-mention-rule)), so it
  does not fire, and a question over it answers "It is not provable …" until zelph can quote rules
  (Z145). In a condition it is refused by `:sub/conditional-with-referents-in-body`, because a rule
  condition holding a rule with variables binds the parts of stored ground rules: "If John believes
  that every man waits then John is happy." would conclude from "John believes that if Bob is a man
  then Bob waits." and never from the universal. In a conclusion it is refused by
  `:sub/conditional-with-referents-in-head`, because a firing writes nothing for a mentioned rule
  whose conditions are a set.
- **A ground conditional inside a that-clause** ("John believes that if Mary waits then Bill
  sleeps.") is refused by `:sub/ground-conditional` in v1: a typed plain ground rule and its mention
  are one node, and the mention takes the rule out of force. Lifting the refusal needs the
  asserted-rule record (K1-P1, Z144), part of the funded work
  ([A Rule Inside a That-Clause](constructs.md#a-rule-inside-a-that-clause) shows all three
  refusals).

### All Senses at a Glance

| Sense | Example | Route | Id | Ms |
|---|---|---|---|---|
| conditional in a conclusion, ground outer condition (switch) | "If John waits then every man sleeps." | rule generator; a chain of switches is a generator of generators | `:rule/nested` | M4 |
| conditional in a conclusion, outer referent | "Every man feeds every dog." | one flattened rule; an inner denial flattens into one denial | `:rule/nested` | M4 |
| conditional in a conclusion over a referent the conclusion introduces | "Every man owns a dog that every cat hates." | rule generator: one witness per outer binding, shared by the outer conclusions and the inner rule | `:rule/nested` | M4 |
| conditional in a conclusion, fully ground | "If John waits then if Mary sleeps then Bill runs." | flattened | `:rule/nested` | M4 |
| one-atom classical negation in a condition | "If John does not wait then Mary sleeps." | `((F) ~ refuted)` condition | `:neg/atom-in-body` | M4 |
| complex classical negation in a condition | corpus case 3165 | refused, hint "does not provably" | `:neg/complex-in-body` | — |
| one-atom NAF in a condition | "If a man does not provably wait then the man sleeps." | `¬(F)`; over a class atom a positive helper | `:naf/atom-in-body` | M4 |
| complex NAF in a condition | "If a man does not provably own a car then the man walks." | positive helper `ace:naf-<h>` plus `¬` | `:naf/complex-in-body` | M4 |
| universal inside a condition | "If every man waits then a dog barks." | refused in v1; the narrowing is planned | `:rule/in-body` | — |
| universal inside a negation, at the top level or in a conclusion | "Nobody likes everybody." · "No woman is older than every cat." | refused: a negated box counts as a condition context | `:rule/in-body` | — |
| NAF inside NAF in a condition | "If a man is not provably a farmer who does not provably own a dog then the man is idle." | refused in v1 | `:naf/nested-in-body` | — |
| NAF chain; positive helper fed by a NAF rule; hierarchy feedback; a component zelph cannot order beside foreign rules or a rule over predicates | the bachelor and husband pair | refused in v1 | `:naf/unstratified` (context) | — |
| negated conditional at the top level | "Not everyone sleeps." | the condition's referents named and its atoms asserted; the conclusion refuted (one ground atom) or denied | `:neg/not-every`, `:rule/negated` | M4 |
| disjunction in a condition | "If John waits or Mary sleeps then Bill runs." | one rule per disjunct | `:or/in-body` | M4 |
| conditional inside a yes/no question | "Is it true that every man is mortal?" | guarded hypothetical check; refused when a NAF or `!=` rule is reachable from the hypothesis | `:rule/in-question`; `:rule/unsafe-hypothesis` (context) | M4; — |
| conditional inside a wh-question | "How does every woman sleep?" | no route | `:rule/in-wh-question` | — |
| negation or NAF inside a question | "Who does not sleep?" · "Which man does not provably sleep?" | a `((F) ~ refuted)` pattern or a `¬` pattern; a complex classical negation is refused | `:neg/atom-in-question`, `:naf/in-question`; `:neg/complex-in-question` | M4; — |
| class atom over subclass data in a condition | "If a writer is not provably a human then the writer is suspicious." | instance predicate plus subclass closure; a positive helper under NAF | (a mapping, no id) | M4 (yes/no questions M1, wh-questions M3) |
| formula in a condition | "If a person's age is A and A > 17 then the person is an adult." | seed rule plus a `compare` condition | `:formula/compare` | M5 |
| negated formula in a condition | "If a person's age is A and it is not provable that A > 18 then the person is a minor." | the formula's mapping with the polarity flipped, sides bound first; never a helper or a denial | `:naf/formula`, `:neg/formula` | M5 |
| identity or inequality in a condition | "If a man X likes a man Y and X \= Y then X is generous." | `!=` guard; `=` with a referent side substitutes; a false ground condition: `[ace/w-vacuous]` | `:formula/identity` | M5 |
| event in a condition | "If a man waits in a park then the man is a visitor." | `E ace:occurrence (X waits nil), E "is in" Y` by deep unification | `:mod/pp` | M6 |
| cardinality in a condition | "If John owns at most 1 car then John is modest." | helper over n+1 pairwise-distinct unmarked witnesses, then `¬`; "at least": distinct unmarked witnesses in the body | `:object/at-most`, `:object/at-least` | M7 |
| a condition over the members of a group | "For each of John and Mary a cat likes him/her." | expanded over the known members; `M in G` over set constants | `:object/group` | M7 |
| statement about a statement in a condition | "If John believes that Mary sleeps then John waits." | `believes` opaque, matched by deep unification; a box with several conditions or a referent by its members (P-set-match) | `:sub/that-clause` | M8 |
| modality in a condition | "If John can swim then John is happy." | an opaque modal fact, matched as above | `:modal/can` | M8 |
| conditional inside a that-clause | "John believes that if a man waits then the man sleeps." · "John believes that if Mary waits then Bill sleeps." | a mention at the top level; refused in a condition and in a conclusion; a ground one refused in v1 | `:sub/that-clause` with `:rule/horn`; `:sub/conditional-with-referents-in-body`, `:sub/conditional-with-referents-in-head`; `:sub/ground-conditional` | M8; — |

## The Matrix

The rows are grouped by area. Within a row, "Ms" is the milestone from which the route applies.

### Graph and Data

The basic building blocks of the graph ([Core Concepts](../concepts.md); [Logic and
Computation](../logic.md), "Facts with Multiple Objects" and "Unary Predicates"). Most of them are
written by plain ACE sentences from M3; the data types follow with their milestones.

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z01 | a fact `S P O` with any predicate node | "John likes Mary." | `:verb/transitive` | M3 | **supported**: the verb's predicate node is named in `ace`. On a Wikidata graph a verb never resolves through a label (label resolution takes nouns, names and relational nouns only); `v:P155` names a property explicitly |
| Z02 | classification `~` | "John is a man." | `:copula/noun`, `:object/count` | M1 | **supported**: `X <instance> C`, the instance predicate pinned per graph: core `~` without Wikidata, P31 on a Wikidata graph (Z63) |
| Z03 | predicates of several words | "John is fond-of Mary." · "John is taller than Mary." · "John is in a park." | `:adj/transitive`, `:adj/comparison`, `:copula/located` | M3 | **supported**: the phrase predicates `"is fond of"`, `"is taller than"`, `"is in"`, named in `ace` (DEC-NAMES); core `in` never becomes a content predicate |
| Z04 | core nodes `~ => ! != -> in nil cons conjunction negation` | none as content words | naming | — | **no route** as content: a core name is the same node in every language. Core spellings are a closed class, never guessed, never resolved as content: `[ace/reserved-word]`, with `n:Q…` as the hint on a Wikidata graph. The mapping writes `~ => ! != negation nil` itself |
| Z05 | nested facts `(s p o)` as subject or object | "John waits in a garden." | `:mod/pp` | M6 | **supported**: `E ace:occurrence (John waits nil)` with `E "is in" Garden-<h>` (DEC-EVENTS) |
| Z06 | a statement claims every fact nested in it | "John believes that Mary waits." | `:sub/that-clause`, `:modal/*`; FM-modal | M8 | **refused** until M8, then supported: a plain nested fact would assert "Mary waits", so from M8 the that-clause verb and the modal predicates are opaque (P-opaque, built; DEC-MODAL; Z17) |
| Z07 | set constants `{a b}` | "{ Mary } sleeps." · "John and Mary lift a table." | `:data/set`, `:object/group`; FM-groups | M7 | **supported** (DEC-PLURALS): a collective group is the set constant of its members |
| Z08 | collections `@{…}` | none | — | — | **no route**: every literal builds a fresh container, so entering a block again would add nodes, and a printed collection cannot be typed back. Collective groups use set constants |
| Z09 | lists `<a b>` | "John gives Mary a book." · "[Mary, Bill] is a list." | `:verb/ditransitive` (objects `<DO IO>`); `:data/list`; FM-groups | M3; M7 | **supported** |
| Z10 | character lists, `zelph/list-chars` | `John says "hello".` | `:data/string` | M3 | **supported** (DEC-DATA): a character list built under `zelph`, never the node of that name; the empty string is the internal node `ace:empty-string`, since `(zelph/list-chars "")` is nil |
| Z11 | focus operator `*` | — | inside `zelph/conjunction`, `zelph/negate` | — | **transparent** |
| Z12 | self-facts `:p X` | "John sees himself." | `:verb/transitive` | M3 | **supported** for reflexive objects: `John sees John` prints as `:sees John`, which the answer verbaliser reads as a two-place fact. One-place predicates are not self-facts: "John waits." is `John waits nil` (DEC-SHAPES), and a native `:waits John` does not meet it |
| Z13 | facts with several objects `s p o1 o2` | "John sees each of Mary and Bill." | FM-groups: one fact per member | M7 | **interop**: a native multi-object fact answers wh-questions and yes/no questions over one of its objects (a yes/no question falls back from the exact probe to the plain `zelph/exists` on a miss, [DEC-YESNO-WIDER](decisions.md#dec-yesno-wider)); separate facts do not make the multi-object fact true ([Facts with Several Objects](#facts-with-several-objects)) |
| Z14 | names and languages, `.lang`, `.name` | every content word | naming ([Naming in the Graph](api.md#naming-in-the-graph)) | M1 | **supported** (DEC-CONTENT-NS): see [Content Lives in Language `ace`](#content-lives-in-language-ace). The documented way to bind a word to a node of the user's choice is a lexicon entry whose symbol is an id; an `ace` name given with `.name` (`.lang wikidata`, then `.name Q64 ace Berlin`) is found too, but a `.name` onto a name another node holds merges the two nodes ([DEC-MERGE-CONTENT](decisions.md#dec-merge-content)), so a binding is moved with `.delname` first (DEC-RELOAD); a core spelling is refused. `ace` itself never calls `.name` ([Choosing Another Sense](words.md#choosing-another-sense)) |
| Z15 | number literals `&n`, digit lists, `zelph/number` | "John's age is 30." | `:data/integer` | M5 | **supported** with `.import ace-math` (DEC-MATH-IMPORT); without it `[ace/needs-math]`. Numbers and characters are language-independent (P-number-lang, built; DEC-MATH-LANG) |
| Z16 | `.mermaid`, `.node`, `.out`, `.in` | — | — | — | **transparent**: they take ACE content by name under `.lang ace`; a node named only in `ace` displays by that name in every language. The ACE rendering of a node is `ace/verbalise-node`: from M4 for the facts of contradiction reports, from M9 for every fact node |
| Z17 | opaque predicates `P ~ opaque`, `=>` as the built-in one, `zelph/mention` | that-clauses, modality | P-opaque; FM-modal | M8 | **supported** from M8 (P-opaque, built): every argument position of an opaque fact is a mention scope; `zelph/mention` (fact and thunk form), C `zelph_mention`, Rust `Engine::mention`; a `=>` written with `zelph/mention` is not in force until something claims it, and a rule over rules does not read it ([DEC-MENTION-RULE](decisions.md#dec-mention-rule)); `.explain` labels a mention `[mentioned; not asserted]`, `zelph/explain` returns `:mentioned`; a query asserts what it creates in an opaque argument (DEC-OPAQUE-QUERY). The declaration is a graph fact, so `ace` writes it at a predicate's first use, never at import |
| Z18 | node annotations (node, key, list of strings) | — | P-annotate | M1, M9 | **used by the front end** (P-annotate, built): the provenance of ACE-built facts and rules is kept as node annotations from M1 on and persisted as an optional `.bin` section (DEC-PROV); M9 reads it for justified answers and the derivation echo. No ACE sentence writes or reads annotations |

### Rules

[Rules Over Graphs](../logic.md#rules-over-graphs), [Rules and Inference](../rules.md) and [Rules
That Write Rules](../rule-generators.md). Every rule `ace` writes is one `=>` fact built inside
`zelph/dedup-rule`, from M4.

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z20 | a rule with a conjunction of conditions | "Every man is a human." · "If a man waits then the man sleeps." | `:rule/horn` | M4 | **supported**: one `=>` fact; a rule with one condition is a one-element condition set tagged `~ conjunction`, so that every rule `ace` writes has a condition set; its contradictions are recorded as those of a plain single condition are (DEC-RECORD-SINGLE); it lists, and `ace/zelph-of` prints it, as `(*{(X ~ man)} ~ conjunction) => …` ([DEC-ONE-CONDITION-PRINT](decisions.md#dec-one-condition-print)) |
| Z21 | [several consequences](../logic.md#several-consequences) in one rule | "Every man is a human and is mortal." | `:rule/horn` | M4 | **supported**: one `=>` fact carrying all conclusions, so that a fresh variable two conclusions name is one witness; `ace` builds it with `zelph/dedup-rule` (Z130). A firing whose consequence is refused keeps the consequences written before it and skips those after it (DEC-FIRING); the all-or-nothing reading the front end needs is the category-4 item Y2-P1 (Z153) |
| Z22 | [fresh variables](../logic.md#fresh-variables-generative-rules) (generative rules) | "Every man owns a dog." · "If a man waits then a dog barks." | `:rule/generative-head`; `:rule/disconnected-head` (context) | M4 | **supported**: "Every man owns a dog." is `(*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog)`; a witness is made only when no assignment already satisfies the conclusions. Documented limits: the witness set depends on the firing order and is unique only up to homomorphism; a witness made before a named fact arrives is kept. A conclusion whose fresh referent no condition variable reaches ("If a man waits then a dog barks.", `((X ~ man), (X waits nil)) => (Y ~ dog) (Y barks nil)`) is supported from M4 too ([DEC-DISCONNECTED](decisions.md#dec-disconnected)): zelph makes one witness for any number of bindings and runs, in every semi-naive mode, which is the reading "then there is a dog that barks" asks for; a test pins one witness across bindings and runs |
| Z23 | generative rules that never end | "Every person has a mother who is a person." · "Every human has a mother who is a woman." (woman a subclass of human) | `:rule/endless-generation` | — | **no route**: refused as a context id over the may-derive graph of the block's and earlier blocks' ACE rules, once on the plan (a class keyed by its own name) and once in the resolution step through the graph's upward subclass closure. Native rules are not inspected (documented limit) |
| Z24 | [meta-rules](../logic.md#meta-rules-predicates-as-first-class-nodes) over predicates | none | FM-meta | — | **no route; interop**: no ACE sentence quantifies over a predicate; a native meta-rule under `.lang ace` fires on ACE facts ([Native Rules and Meta-Rules](#native-rules-and-meta-rules)) |
| Z25 | [rules that derive rules](../logic.md#rules-that-derive-rules) | "If John waits then every man sleeps." · "Every man owns a dog that every cat hates." | `:rule/nested`; FM-nested | M4 | **supported** as a generator for a switch and for an inner rule over a referent the outer conclusion introduces; flattened otherwise (DEC-NESTED-IMP) |
| Z26 | generators of generators | "If John waits then if Mary sleeps then every man runs." · corpus case 2013 | `:rule/nested`; FM-nested | M4 | **supported** for chains of switches; flattened when an outer condition declares a referent and the inner rule uses none of the conclusion's, as in corpus case 2013 |
| Z27 | [mentioning a rule is not asserting it](../logic.md#mentioning-a-rule-is-not-asserting-it) | "John believes that if a man waits then the man sleeps." · "John believes that if Mary waits then Bill sleeps." | `:sub/that-clause` with `:rule/horn`; `:sub/ground-conditional`; `:sub/conditional-with-referents-in-body`, `:sub/conditional-with-referents-in-head`; FM-modal | M8; — | **supported** at M8 for a that-clause holding a conditional with variables, stored as a mention; a question over it answers "It is not provable …"; refused in a condition and in a conclusion; a ground conditional is **refused** in v1 until the asserted-rule record exists (K1-P1, Z144) ([Statements About Statements](#statements-about-statements)) |
| Z28 | [deep unification](../logic.md#deep-unification) | "If a man waits in a park then the man is a visitor." | `:mod/pp` in a condition | M6 | **supported** |
| Z29 | disjunction as several rules | "If John waits or Mary sleeps then Bill runs." | `:or/in-body` | M4 | **supported**: one rule per disjunct |
| Z30 | the conjunction tag `*{…} ~ conjunction` | — | `zelph/conjunction` | — | **transparent**: `zelph/conjunction` refuses a single condition, so the one-element set of Z20 is a `zelph/collection` tagged `~ conjunction`, built inside the `dedup-rule` thunk |

### Negation, Contradiction and Inequality

[Negation as Failure](../logic.md#negation-as-failure), [Contradiction
Detection](../logic.md#contradiction-detection), [Inequality
Constraints](../logic.md#inequality-constraints). "does not" is classical negation, "does not
provably" negation as failure (DEC-NEG).

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z40 | NAF `¬(P)` over one pattern | "If a man does not provably wait then the man sleeps." · "If a person's age is A and it is not provable that A > 18 then the person is a minor." | `:naf/atom-in-body`; `:naf/formula` | M4; M5 | **supported**. A seeded run seeds a NAF rule by its positive conditions (P-naf-delta, built), except in the cases DEC-NEG names; a negated formula is `:naf/formula` (Z71) |
| Z41 | `¬(A, B)` is rejected; free variables are quantified inside `¬` | "If a man does not provably own a car then the man walks." | `:naf/complex-in-body`: positive helper `ace:naf-<h>` | M4 | **supported** while the box holds no NAF and no universal (else `:naf/nested-in-body`) and reads nothing a rule with a NAF condition can derive (else `:naf/unstratified`) |
| Z42 | negation levels, keyed by pattern | "If every man waits then a dog barks." · NAF inside NAF · two "not provably" sentences where one tests what the other derives | `:rule/in-body`, `:naf/nested-in-body`, `:naf/unstratified`; FM-strata | — | **refused** by the three ids in v1 (DEC-STRATA); the narrowing that would accept the orderable cases is planned, not v1 ([Shapes That Need Ordered Negation](#shapes-that-need-ordered-negation)) |
| Z43 | a NAF conclusion is final across runs | "If a man does not provably wait then …" in one block, "John waits." in a later one | — | M4 | **transparent**, documented for every NAF construct: a later block does not retract a NAF conclusion. `ace` switches the negation records on with its first NAF rule (DEC-NAF-RECORDS), so its quiet run returns the defeat, which `ace` reports as `[ace/w-naf-final]`; natively check mode raises "Negation check: …" and a run with records on prints `Warning: N fact(s) no longer justified …` |
| Z44 | NAF in a query | "Which man does not provably sleep?" | `:naf/in-question` | M4 | **supported** |
| Z45 | `¬` has no reading as a conclusion or a statement | "If a man owns a car then he does not provably sell it." · "It is not provable that Mary sleeps." | `:naf/top-level` | — | **no route** (DEC-NEG) |
| Z46 | refutation `¬(F)` on its own line | "John does not wait." · "It is false that Bill runs." | `:neg/fact` | M4 | **supported** through P-refute (a)-(e) (built). A top-level `(F) ~ refuted` line is the native spelling of `¬(F)`, and a raw Janet marking is refused (DEC-REFUTED); neither is an ACE route |
| Z47 | `(F) ~ refuted` as a condition | "If John does not wait then Mary sleeps." | `:neg/atom-in-body` | M4 | **supported**: matches refuted facts only; a ground marking the rule only names refutes nothing until it is claimed (P-refute (d), built) |
| Z48 | contradiction rules `=> !` | "There is no unicorn." · "No man waits and sleeps." | `:neg/statement`; `:neg/in-head` for a complex negated conclusion | M4 | **supported** as denials |
| Z49 | complex classical negation in a condition | corpus case 3165 | `:neg/complex-in-body` | — | **refused** (DEC-NEG), hint "does not provably" |
| Z50 | contradiction records, reported once | any block that makes the graph inconsistent | block contract, step 7 ([Contradictions and Defeated Conclusions](block.md#contradictions-and-defeated-conclusions)) | M4 | **supported**: the quiet run returns every contradiction it met (DEC-QUIET), also those no record can hold (records off, conditions that are all guards or negations); `ace` reports each in ACE, at most 5 per rule and block. One-condition rules have their contradictions recorded whether the condition stands in the one-element condition set `ace` builds or plain, with the same set constant as record ([DEC-RECORD-SINGLE](decisions.md#dec-record-single)); the record only keeps later runs silent |
| Z51 | inequality `!=` | "If a man X likes a man Y and X \= Y then X is generous." · "If X = Y then Y = X." | `:formula/identity`; FM-neq | M5 | **supported** (DEC-NEQ): `\=` over variables becomes a `!=` guard (closed world, documented) |
| Z52 | functional-property denials with `!=` | "At most 1 woman sleeps." · "Every man has at most 1 father." | `:object/at-most`; `:object/at-least` for "exactly", which adds the same denial; FM-atmost | M7 | **supported** (DEC-PLURALS): denials over pairwise-distinct witnesses that are named constants or distinct by `\=`; every individual `ace` creates carries the internal mark `I ace:individual nil`, and the denials skip marked witnesses with `¬(W ace:individual nil)`; engine witnesses of generative heads carry no mark and count as distinct (closed world for witnesses) |
| Z53 | ["Is this the only one?"](../logic.md#is-this-the-only-one) (a derived marker and `¬`) | "If John owns at most 1 car then John is modest." | FM-atmost: helper with `!=` over unmarked witnesses | M7 | **supported**, as Z52 |
| Z54 | disjunction in a conclusion | "John waits or Mary sleeps." | `:or/in-head-or-statement` | — | **no route** |
| Z55 | identity between two constants | "John is Bill." | `:copula/identity`, `:formula/identity`; FM-neq | M5 | **supported** (DEC-NEQ): read like "John = Bill."; two named constants are identical iff they resolve to one node; a false ground statement is `[ace/contradiction]`; an identity between an individual and a name is refused with `[ace/unsupported]`, because `ace` never merges two existing nodes |
| Z56 | a derived refutation `=> ((F) ~ refuted)` | "No man swims." · "Every man does not sleep." | `:neg/in-head` | M4 | **supported** through P-refute (c-2) (built; DEC-REFUTED): after "Tom is a man.", "Does Tom swim?" answers "No. It is false that Tom swims."; a derived marking that meets a claimed F is a reported contradiction whose record is the condition set plus F, and F keeps answering |

### Paths, Classes and Wikidata

[Transitive Path Conditions](../logic.md#transitive-path-conditions), [Working on the Wikidata Class
Hierarchy](../class-hierarchy.md), [Wikidata Qualifiers](../qualifiers.md), [zelph and
Wikidata](../wikidata.md). ACE has no closure construct; the front end applies the subclass closure
itself wherever a class is asked or tested
([Classes: the Instance and Subclass Predicates](words.md#classes-the-instance-and-subclass-predicates)).

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z60 | path conditions `P⁺`, `P∗` | "Is Douglas a human?" over instance and subclass facts · "Every human is mortal." | class atoms of `:copula/noun`, `:object/count`; FM-paths | M1 yes/no questions, M3 wh-questions, M4 conditions | **supported** where the graph has a subclass predicate (DEC-PATHS): a class atom in a question or a rule condition becomes `X <instance> D, ((D <subclass> C) closure zero-or-more)`; statements and conclusions write `X <instance> C`. Without a subclass predicate ACE sees direct and derived class facts only |
| Z61 | a path under `¬` | "If a writer is not provably a human then the writer is suspicious." | FM-paths, positive helper | M4 | **supported**, subject to `:naf/unstratified` |
| Z62 | cycle test `:p⁺ X` | none | — | — | **no route**: ACE has no closure construct and no way to name a predicate |
| Z63 | Wikidata instance and subclass facts (P31, P279) | any class sentence on a Wikidata graph | the instance and subclass predicates; FM-paths | M1 (pin, yes/no), M3 (wh, relational nouns), M4 (conditions) | **supported** (DEC-PATHS): the pin is one internal fact in `zelph`, written with the first class atom `ace` writes: `ace:profile ace:uses ace:wikidata` (instance P31, subclass P279) where `P31` and `P279` exist in language `wikidata`, else `ace:profile ace:uses ace:core`. No node is renamed. A node the user named `ace:instance` or `ace:subclass` in `ace` wins; a later block that finds a graph pinned to `ace:core` holding P31 and P279 warns `[ace/w-profile]` once and keeps the pin. P31 is an ordinary node, not core `~`. Memberships Wikidata keeps in other properties (occupation P106, sex P21) answer "It is not provable …", and the user documentation says how to ask instead ("What is an occupation of Douglas Adams?"). A rule whose class has more than 100,000 instances warns `[ace/w-large-rule]` |
| Z64 | qualifiers (reified statements), ranks | — ("John waits in a park." has another shape) | — | — | **no route** in v1: ACE events (`E ace:occurrence (fact)`) and Wikidata statement nodes are different reifications. The importer reads every rank and no qualifiers, so "What is the capital of Germany?" can have several answers, and values that are not items (population, dates) are not imported; the user documentation says both |
| Z65 | class-hierarchy reports (`wikidata-classes`, culprits) | — | — | — | **no route**: reports, not statements; their results are facts ACE questions can read |
| Z66 | SPARQL (`.import sparql`) | — | — | — | **interop**: ACE facts are ordinary triples, met under `.lang ace` ([SPARQL](#sparql)); ACE answers for SPARQL rows are a later layer |
| Z67 | Wikidata labels as names (`en`, `mul` fallback) | "There is a man." (Q8441) · "Douglas Adams is a writer." | word resolution, step 3 | M1 | **supported** (DEC-WIKIDATA): nouns, names and relational nouns resolve through `zelph/lookup-all` by role, the lowest Q-number winning (P-number for relations); a name whose most connected candidate is a given-name or family-name item is not bound. The `word-senses` note announces every binding to a label more than one node carries; `p:Q42`, `n:Q8441`, `n:P50`, `v:P155`, `a:Q3142` name an item explicitly. A graph without the marker `wikidata:labels-v2` gets `[ace/w-old-labels]`, a partial load `[ace/w-index-incomplete]`, and label resolution is then off |

### Mathematics

[Semantic Math](../logic.md#semantic-math-computation-as-graph-rewriting) and the [mathematics
pages](../math/index.md). Every route here needs `.import ace-math`; [Numbers and Formulas](math.md)
owns the mapping.

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z70 | rule-based arithmetic `+ - * / ^` | "What is 2 + 3?" | `:expr/arith` | M5 | **supported** with `ace-math`; ACE's `/` maps to `exquo` (Z77), never to native `/`, which truncates on naturals |
| Z71 | comparison | "Is 5 greater than 3?" · "John is 5." · "It is false that 2 = 5 - 4." | `:formula/compare`; `:neg/formula`, `:naf/formula` | M5 | **supported** with `ace-math`, the verdict through `compare` (Z77). A comparative with a math reading and a numeric side, and "John is 5.", are `:formula/compare`, not `:copula/identity`. A negated formula is the formula's mapping with the polarity flipped, so "It is false that 2 = 5 - 4." is a check, never a denial |
| Z72 | `≡`, polynomials, polynomial rings | "For every integer X, (X + 1)^2 = X^2 + 2*X + 1." | math-universal statements | M5 | **supported** with `ace-math`; an ACE math variable is the node named as written, in `zelph` (DEC-MATH-INDET) |
| Z73 | demand-driven requests (seed rules; a term nested in a statement is a request) | a formula in a condition · "John likes 7 / 2." | seed rules; `:expr/as-data` | M5; — | **supported** with `ace-math`: every arithmetic rule condition gets a seed rule, and a term with an integer value is stored by its value. A term without one as an argument of a stored atom has **no route** in v1: stored as a term it would be a request, because a plain statement claims every fact nested in it (DEC-DATA) |
| Z74 | math islands `$( … )` | — | — | — | **no route**: ACE has its own term grammar. `^` is right-associative in the island, so ACE's `2^3^2` and the island's differ in value |
| Z75 | string concatenation | `"xyz" is "xy" & "z".` | `:expr/concat` | — | **no route** |
| Z76 | reals, measured numbers | "3.14", "180cm" | `:data/real`, `:data/measured` | — | **no route**: zelph has integers only and no units |
| Z77 | `stdlib/evaluation.zph`: `exquo`, `difference`, `compare` | the values and verdicts of ACE formulas | DEC-MATH-MODULE | M5 | **supported** from M5, when the module is written as a general stdlib module with its own documentation page: `ace-math` imports it and maps the requests `:value :quot :diff :cmp` in one table; `math.zph` does not import it, and native users import it themselves |

### Neural Conditions and Probabilities

[Neural Networks in the Graph](../neural.md). ACE has no graded truth.

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z80 | `≈net(…)` conditions | none | FM-neural | — | **no route; interop**: ACE has no construct that names a network; a native `≈` rule fires on ACE-built facts ([Neural Conditions](#neural-conditions)) |
| Z81 | fact probabilities | — | FM-neural | — | **no route**: a fact derived with a probability is claimed, so a yes/no answer says "Yes." without the probability (documented) |
| Z82 | `nn` helpers, training | — | — | — | **no route** |

### Queries and Requests

[Querying in zelph](../queries.md) and the quick start's [statement
prefixes](../quickstart.md#two-statement-prefixes). Every ACE question runs in its own scratch
cluster `ace:q`, which is dropped afterwards, so a question writes nothing
([A Question Changes Nothing](answers.md#a-question-changes-nothing)).

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z90 | a ground check (`zelph/exists`) | "Is John a human?" | `:question/yes-no` | M1 | **supported**: three answers ("Yes. …", "No. It is false that …", "It is not provable that …"); ground atoms are never built: each is checked with `zelph/exists … :exact` (P-exact, built), one hash lookup, and on a miss with the plain `zelph/exists`, which reads a native fact with further objects as a rule condition does ([DEC-YESNO-WIDER](decisions.md#dec-yesno-wider), Z13). That second probe is the one plain `zelph/exists` the front end makes; it asks the condition matcher only while the graph may hold such facts, bounded by the subject's facts with that predicate, never a scan. `zelph/exists` finds no engine bookkeeping (DEC-BOOKKEEPING) |
| Z91 | single- and multi-condition queries | "Who is a human?" · "Which man that waits likes Mary?" | `:question/wh` | M3 | **supported**; at most `:max-answers` (default 100) answers, then "… and N more answers." |
| Z92 | variables over predicates (`X R Y`) | none | — | — | **no route**: no ACE question word ranges over verbs |
| Z93 | the `?` request | every ACE question | block contract | M1 | **supported**: a block with a question runs `ace/infer!`, `(zelph/run-delta :quiet)`, before it answers, which is what `?` does (DEC-RUN); `ace` never uses `?` itself |
| Z94 | conditional checks | "Is it true that every man is mortal?" | `:rule/in-question`; `:rule/unsafe-hypothesis`; `:rule/in-wh-question`; FM-qimp | M4; —; — | **supported** from M4 through P-rule-list (`zelph/rules`, built): the guarded hypothetical check (DEC-QUNIV); refused as `:rule/unsafe-hypothesis` when a NAF or `!=` rule is reachable from the hypothesis; a conditional in a wh-question has **no route** ([A Conditional Inside a Question](#a-conditional-inside-a-question)) |
| Z95 | counting | "How many dogs does John own?" | `:question/how-many` | M7 | **supported**. A count over witnesses can exceed the minimal model when a named fact arrives after a witness (Z22's limit, documented) |
| Z96 | event questions | "Where does John wait?" | `:question/where-when-how` | M6 | **supported** |
| Z97 | negation in a question | "Who does not sleep?" · "Is there no dog?" | `:neg/atom-in-question`; `:neg/complex-in-question` | M4; — | one atom: **supported** (a `(F) ~ refuted` pattern); complex: **no route**, hint "not provably" (`:naf/in-question` is supported) |
| Z98 | disjunction in a question | "Who waits or sleeps?" | `:or/in-question` | M4 | **supported** |
| Z99 | a query with an unconstrained variable | "What is there?" | `:question/unrestricted` (context) | — | **no route**: zelph enumerates matches of patterns, and the wh-referent carries none |

### Session Commands

The quick start's [command reference](../quickstart.md#full-command-reference). The commands work on
ACE-built knowledge as on any other; the rows say what `ace` does about each.

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z100 | `.run`, `.run-once`, `.run-delta`, `.auto-run` | — | `ace/infer!` | M1; M9 | **transparent**: `ace/infer!` is `(zelph/run-delta :quiet)` when the block asks a question, or when it wrote something and `(zelph/auto-run?)` is true; the host skips its own auto-run after a handler that ran after its last write (P-autorun-skip, built; DEC-RUN). From M9, while `(ace/option :echo)` is true, it is `(zelph/run-delta :quiet :derived)` (P-derived, built) |
| Z101 | `.deductions` modes | — | DEC-QUIET, DEC-ECHO | M1; M9 | **transparent**: a quiet run prints nothing. Before M9 nothing of derivations is shown; from M9 `(ace/option :echo true)` prints an ACE echo of each derived sentence with its premises, and `(ace/option :justify true)` adds to each answer the sentences it rests on (both false by default) |
| Z102 | `.explain` | — | P-explain | M9 | **interop**: native `.explain` works on facts and rules built from Janet; it labels unclaimed parts `[rule pattern; not asserted]`, mentioned rules `[rule mentioned; not in force]` and mentions `[mentioned; not asserted]`; `zelph/explain` (built) returns the proof as data, with the statuses `:derived :axiom :unfounded :rule-pattern :rule-mentioned :truncated :mentioned`. A derived fact whose rule went out of force stays `[axiom]` (DEC-BACKLOG). ACE explanations are M9 |
| Z103 | `.run-export` | — | — | — | **transparent** |
| Z104 | clusters: `.cluster`, `.cluster-drop`, `.cluster-merge` | — | cluster discipline ([Scratch Clusters](block.md#scratch-clusters)) | M1 | **transparent**: blocks are recorded in the caller's cluster, and dropping a user cluster removes a whole experiment; taking back one block is never offered. Cluster names beginning `__` are refused (`ace:dry` and `ace:q` are unaffected), and no cluster may be switched inside a rule build (Z131). A drop keeps the fact stores armed (P-scratch-drop, built), invalidates the record of additions only when a run derived facts in the cluster (DEC-DROP-DELTA), takes back the refutations made in it (P-refute (e), built), gives back the NAF defeats a quiet run inside it reported, and keeps the engine vocabulary created in it (DEC-VOCAB) |
| Z105 | `.save`, `.load` | — | session events | M1; M9 | **transparent**: ACE-built facts, set constants and the unclaimed state of ground rule patterns survive; user lexicon entries are graph facts in `ace-lex` and persist, the shipped vocabulary is Janet data (DEC-LEX-STORE); provenance is kept as node annotations, persisted as an optional `.bin` section (DEC-PROV). A whole-file `.load` replaces the session's graph and clears the import registry, so `.import ace` and `.import ace-math` run again after it (DEC-RELOAD); a dump's `.bin` cache is read and written only when the import begins on an empty graph, which `.import ace` leaves empty; `ace`'s caches of node ids are keyed by `zelph/epoch`; the pin and the individual marks are graph facts and go with the graph |
| Z106 | retraction: `.prune-facts`, `.prune-nodes`, `.remove`, `.remove-rules`, `.cleanup` | an `ace-retract` block: "John is a man." | FM-retract; P-retract | M9 | **supported** at M9 through the block keyword `ace-retract`, for fact statements only: each atom is removed with `zelph/prune-fact`; the block is refused, removing nothing, while derived facts rest on an atom (`zelph/dependents`, `[ace/retract-dependents]`) or another statement nests it (`zelph/holders`). zelph has no truth maintenance: a derived fact outlives its pruned premise, which is why an unguarded retraction is not offered. Rules and nodes have no ACE route |
| Z107 | `.new` | — | session events | M1 | **transparent**: `zelph/epoch` advances, and `.negation-records` is off again until `ace`'s next NAF rule after a new `.import ace`; `.new` inside a module is refused |
| Z108 | `.import`, `.provides`, the standard library | `.import ace`, `.import ace-math` | module layout ([The Modules](api.md#the-modules)) | M1; M5 | **supported**: `.import ace-math` is explicit (DEC-MATH-IMPORT) and imports math under `.lang zelph`; math imported earlier under another language is the same stack, named in `zelph` (P-number-lang, built) |
| Z109 | loading a text file | an ACE text document | FM-load | M2 | **supported** through `(ace/load! path)` and `(ace/load-text! text)` (DEC-LOAD); `.import` takes only `.zph` and `.janet` files |
| Z110 | `.list-rules` | — | `ace/zelph-of` lines | M1 | **transparent**: ACE rules list in native form, one-condition rules as `(*{…} ~ conjunction) => …`, helper rules `ace:naf-<h>` visible. A listed line that names only content re-enters natively under `.lang ace` with no new node |
| Z111 | `.stat`, `.list`, `.clist`, `.list-predicate-usage`, `.list-predicate-value-usage` | — | — | — | **transparent** (`.stat` lists the languages `ace` and `ace-lex` beside the others) |
| Z112 | `.load-partial`, `.save-predicates`, `.stat-file`, `.index-file`, sharding | — | — | — | **transparent**; a partial load leaves the label index incomplete, so label resolution is off with `[ace/w-index-incomplete]` (Z67); shard trees and manifests carry no annotations |
| Z113 | `.wikidata-constraints`, `.wikidata-qualifiers`, `.export-wikidata`; the dump importer | — | P-wd-labels | — | **no route**: import tooling. The importer keeps every item's `en` label and writes the marker `wikidata:labels-v2` (P-wd-labels, built); the published networks predate it and wait for regeneration, and the `.bin` format does not change. The tooling for the dedicated `ace` network, `dev_scripts/wikidata-subset.py` with its criteria in `dev_scripts/ace-network.json`, is built ([Words and Wikidata](words.md)) |
| Z114 | `.parallel`, `.anchors`, `.semi-naive`, `.fact-stores`, `.negation-records` | — | — | — | **transparent** (`ace` sorts its answers). The test binary starts every engine in `.semi-naive check`, where a later fact that defeats a NAF conclusion raises "Negation check: …"; a test that needs another mode opts out with `.semi-naive on` and says why. `.negation-records` (off by default) keeps the records outside check mode; `ace` switches it on with its first NAF rule and never off (DEC-NAF-RECORDS); a load discards the records and keeps the switch, `.new` resets it |
| Z115 | `.contradiction-records` | — | — | — | **transparent**: with `off` the quiet run still returns every contradiction it met, so nothing disappears from `ace` (Z50) |
| Z116 | `.log`, `.log-janet`, `.prof` | — | — | — | **transparent** |
| Z117 | Janet `%` lines | — | the `ace/*` API | — | **no route** inside ACE: a block holds no Janet; programs call the API ([The Public API](api.md#the-public-api)) |
| Z118 | `.strata` | — | FM-strata | — | **transparent**: it lists ACE-built rules with all others. Its binding `(zelph/strata)` gives the verdict as data; `:naf/unstratified` is `ace`'s own analysis, and from M4 the dry run takes the engine's verdict as a further refusal clause, never as an acceptance criterion (DEC-STRATA) |
| Z119 | `.explain-budget` | — | P-explain, P-retract | M9 | **transparent**: it bounds native `.explain` on ACE-built facts and, from M9, the `zelph/dependents` check of `ace-retract`, whose answer then ends with `:truncated` and whose refusal says that the list may be incomplete (default 80,000 units; `.new` restores it) |
| Z120 | `.timer` | — | — | — | **transparent**: a slow block is followed by the host's timing line under `auto`; a script run as `zelph file.zph` prints none; transcripts are taken with `.timer off` |

### The Janet API

[Scripting with Janet](../janet.md). The `ace` modules are Janet and zelph, so the Janet API is what
they are written in; these rows say which parts they use and which rules they obey.

| # | zelph feature | ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z130 | rule builders: `zelph/dedup-rule`, the `zelph/rule` macro, `zelph/rule*`, `zelph/build-rule` | every rule | the rule idiom | M4 | **used by the front end**: every rule is one `=>` fact carrying all conclusions, built inside `zelph/dedup-rule` with every `zelph/var` created in the thunk. `dedup-rule` leaves nothing when its thunk fails, prefers a rule in force over one only mentioned when it looks for a duplicate, and refuses a thunk that returns no `=>` node (P-rule-api, built). The other builders have no `ace` use: `zelph/rule` does no deduplication and returns the condition set rather than the `=>` node |
| Z131 | refusals inside a rule build | — | an invariant of the modules | M4 | **constraint**: inside a rule-building thunk `zelph/query`, `zelph/cluster` (even without an argument), `zelph/cluster-drop`, `zelph/import`, `zelph/load`, `zelph/save`, the run functions and every nested rule builder are refused; `zelph/exists`, `zelph/resolve` and `zelph/var` are allowed. So `ace` never probes, queries or imports while a rule is built, and a generator's inner rule is a `=>` fact written in the same thunk |
| Z132 | `zelph/rule-text` | — | — | — | **no direct use**: the parser wraps every `=>` that stands inside another statement in it, so a native mention of a rule claims none of its parts; a generator's inner rule is marked by `dedup-rule` itself, and mentions from M8 go through `zelph/mention`. A native mention of a plain single-condition ground rule turns that rule off (one hash-consed node, documented); `ace`'s one-element set form stays in force after a typed mention of the same text (unless it met a plain rule typed before, [Traps](#traps)), a ground comma-list rule is a new node per statement, and a typed mention of a rule with variables is another node |
| Z133 | writes on the main thread only | — | — | — | **constraint**: every graph write, rule build and engine state change is main-thread only; `ace` writes only from the main thread (its handlers and API calls) and starts no threads; it reads names with `zelph/lookup` (P-lookup, built), which never creates |
| Z134 | `zelph/refute`, `zelph/negate`, `zelph/path`, `zelph/path-guard`, `zelph/set`, `zelph/list`, `zelph/collection` | the plan's refutations, NAF conditions, paths, `!=` guards, sets, lists, condition sets | plan terms ([Terms](api.md#terms)) | M1-M7 | **used by the front end** as the Janet side of the plan's terms. A keyword handler's `zelph/fact` claims the marked parts it contains (P-claim, built); `zelph/refute` claims its marking and nothing of its fact, which it writes as a mention when it creates it (DEC-REFUTE-ROUTES) |

### Engine Features Built, Planned or Still Needed

The last group lists engine features that change what ACE can do: the planned items of
[the project's outlook](../about.md#where-the-logic-goes-next) that would lift a refusal, the engine
items built for the front end that the rows above do not name, and the category-4 items that remain
([What zelph Still Needs](prerequisites.md#the-seven-items-at-a-glance)). Two category-4 items have no
row of their own, because they constrain the mapping rather than a feature:
[L3-P1](prerequisites.md#l3-p1-a-statement-nested-in-a-conclusion-is-explained-as-an-input) (a
statement that a rule's conclusion nests is derived but explained as an input, so the mapping writes
every nested atom also at the top level) and
[Z3-P2](prerequisites.md#z3-p2-a-firing-asserts-a-comma-list-it-was-only-handed) (a firing enters a
comma list a rule holds by node, so the mapping never places such a list into a conclusion).

| # | zelph feature | What it opens for ACE | Id or note | Ms | Status |
|---|---|---|---|---|---|
| Z140 | negation levels keyed by pattern | `:rule/in-body`, `:naf/nested-in-body`, `:naf/unstratified` with the helper bodies of [Helper Rules for "not provably"](semantics.md#helper-rules-for-not-provably) | FM-strata | — | **transparent**, built: a top-level pattern is keyed by (predicate, constant object), a variable or structured object, a nested statement and a path condition by (predicate, any), every key by its predicate alone while a rule over predicates is in force. The three ids stay refused in v1 (DEC-STRATA); the narrowing is planned, not v1. Levels that depend on the data, and a variant for paths, are not built |
| Z141 | revision of NAF conclusions | NAF across blocks without `[ace/w-naf-final]` | FM-strata | — | **planned** (the outlook's "A conclusion that a later fact withdraws"): in v1 a defeated NAF conclusion is reported, never withdrawn (DEC-NAF-RECORDS) |
| Z142 | a Janet binding for the negation levels | `:naf/unstratified` from the engine's verdict, native rules included | FM-strata | M4 | **supported**: `(zelph/strata)` (built), with a `:transparent` field for rules over predicates (P-strata-transparent, built); from M4 the dry run refuses as `:naf/unstratified` a component zelph cannot order that holds a NAF rule `ace` wrote beside a rule `ace` did not write, or while a rule over predicates is in force. Never an acceptance criterion |
| Z143 | a report of lost facts outside check mode | `[ace/w-naf-final]` in every mode | FM-strata | M4 | **supported**: `.negation-records`, `(zelph/negation-records &opt on)` and `(zelph/defeated)` (built); `ace` switches the records on with its first NAF rule, so the quiet run returns the defeats in every mode (Z43, Z114) |
| Z144 | the asserted-rule record (a positive record that a rule was asserted) | that-clauses holding a ground conditional (`:sub/ground-conditional`, Z27); ground switches kept as switches | FM-modal; K1-P1, K1-P2 | — | **not built**, category 4 ([DEC-CAT4](decisions.md#dec-cat4); also the outlook's "A rule in force due to being asserted"): specified with example sessions, implemented in the funded work; v1 refuses what would need it by id and flattens fully ground nested conditionals |
| Z145 | quoting of rules (matching a mentioned rule up to a renaming of its variables) | conditions over a that-clause holding a conditional with variables (now refused by `:sub/conditional-with-referents-in-body`); questions over one, which answer "not provable" until then | FM-modal | — | **planned** (the outlook's "Quoting a rule") |
| Z146 | `zelph/claim-text`: a typed statement that carries a refutation, takes a reference or builds a part runs in a scratch cluster, merged on success, dropped on refusal (built) | — | DEC-REFUTE-ROUTES | — | **no direct use**: the parser emits it for typed statements, which is why a refused typed statement leaves the graph as it was; `ace` refutes with `zelph/refute` and asks in its own scratch clusters |
| Z147 | the C API's `zelph_refute` and `zelph_mention`, Rust `Engine::refute` and `Engine::mention`; the refusals of `zelph_fact`, `zelph_exists` and `zelph_rule` (built) | — | — | — | **no route**: `ace` is a stdlib module and uses the Janet bindings; the [C API](../capi.md) entries keep programmatic writers on the same rules |
| Z148 | `.run quiet [derived]`, `.run-once quiet`, `.run-delta quiet`; the help topic `.help opaque` (built) | — | — | — | **transparent**: the commands are the same implementation as `(zelph/run :quiet :derived)`, which `ace/infer!` calls; `ace` types no command |
| Z149 | the subset reading of a nested statement that meets a fact with more objects, in every evaluation order, and of a statement as a condition's predicate (built) | — | DEC-BACKLOG | — | **transparent**: `ace` writes one object per fact and the importer one per claim, so the shape arises only over native data. The top level stays injective; one reading at every level is the category-4 item Z2-P1 |
| Z150 | a consequence whose predicate is a variable no condition binds is refused by every rule builder when the rule is written (built); a `=>` written with `zelph/fact` or C `zelph_fact` is checked at the end of the outermost Janet evaluation, before a save and before any run, and removed and refused if it is still a rule in force then | — | [DEC-UNBOUND-PRED](decisions.md#dec-unbound-pred), [DEC-FACT-RULE](decisions.md#dec-fact-rule) | M4 | **transparent**: `ace` builds every rule with `zelph/dedup-rule`, so it meets the builder check; its predicates are nodes, a variable predicate stands only in question patterns, and a rule `ace` writes over relations binds its predicate in a condition |
| Z151 | foci that change nothing are refused; a focus's side fact is recorded and claimed (built) | — | — | — | **no route**: `ace` builds its rules and facts in Janet, not through focused zelph lines; native lines keep the documented meaning |
| Z152 | a refused consequence, a firing refused midway and a typed statement refused midway leave the graph as they found it; a set constant is never extended by a firing (built) | — | DEC-FIRING, DEC-SILENT-SKIP | M4 | **supported**: the dry run and the real run see a refused consequence as an entry with `:refused` and nothing it built |
| Z153 | the atomic firing: a rule with several consequences is applied whole or not at all | "If a man owns a car then he drives it and he insures it." with one conclusion refused or contradicted | Y2-P1 | — | **not built**, category 4: today a firing is applied by halves and depends on the stored order of its consequences, and with a fresh witness beside a refused sibling it grows by one witness per full run, so `.semi-naive off` and `check` do not end; the front end needs it before such texts meet a refusal |
| Z154 | an incremental run after a block that adds a rule, when the rule changes no negation level | every sentence that adds a rule after the first "not provably" sentence | Z3-P1 | M4 | **not built**, category 4: from the first NAF rule on, every block that adds a rule makes the next run a full pass over every rule; the answers stay right, the cost is the extent of every rule (on a Wikidata slice, by estimate, seconds to minutes per sentence) |

## Evidence Behind the Designed Rows

Some routes could not be read off the documentation; they were checked against the engine first. The
notes for nested conditionals (FM-nested), ordered negation (FM-strata), conditionals in questions
(FM-qimp), meta-rules (FM-meta) and neural conditions (FM-neural) stand in the sections above. The
mapping behind each note belongs to [From Sentences to the Graph](semantics.md) ([Numbers and
Formulas](math.md) for formulas, [The ace Block](block.md) for documents and retraction); each note
below summarizes the route in a few lines, links to the mapping and keeps the evidence.

### Class Atoms Over Subclass Data (FM-paths)

Decision [DEC-PATHS](decisions.md#dec-paths). The instance and subclass predicates are pinned per
graph by one internal fact, `ace:profile ace:uses ace:wikidata` on a Wikidata graph and
`ace:profile ace:uses ace:core` otherwise (instance core `~`, no subclass predicate); no node is
renamed. In a question or a rule condition a class atom becomes `X <instance> D, ((D <subclass> C)
closure zero-or-more)`, and zero steps include D = C, so direct members match; answers name the asked
class, never D. [Classes: the Instance and Subclass Predicates](words.md#classes-the-instance-and-subclass-predicates)
and [Class Patterns and the Graph Profile](semantics.md#class-patterns-and-the-graph-profile) give the
mapping.

Evidence, with `~` and `"is subclass of"` (on a Wikidata graph only the two predicate nodes differ):
the plain question finds nothing; the path-aware question finds one row and leaves nothing behind; the
path rule derives `douglas ~ mortal`; the rule re-enters natively with no new node; NAF over a path
atom through a helper gives `douglas suspicious false, kafka suspicious true`. On Wikidata a rule with a
closure condition takes a full pass only when a fact of the walked predicate arrived (P-closure-delta,
built), and a question's closure keeps the predicate index (P-pidx-local, built).

### Identity and Inequality Between Constants (FM-neq)

Decision [DEC-NEQ](decisions.md#dec-neq). Unique names hold for named constants only (resolved names,
Wikidata items, strings, numbers): two are identical iff they resolve to one node, decided after
resolution and before the dry run. Individuals and witnesses are not names: an identity involving one
is "Yes" when both sides are one node and "not provable" otherwise, and asserting one between an
individual and a name is refused ([Identity](semantics.md#identity)). Evidence:
`((X likes Y), (X != Y)) => (X is generous)` gives `tom generous false, john generous true`;
distinctness by node identity counts two pairs of distinct fathers; an inequality in a conclusion
becomes a denial: `\=` between the subject and the object of "likes" gives `(:likes X) => !`, which
reports `! ⇐ (:likes tom)` for someone who likes himself.

### At Most, At Least, Exactly (FM-atmost)

Decisions [DEC-PLURALS](decisions.md#dec-plurals) and DEC-NEQ; the mapping in every position is in
[Plurals, Groups, Cardinality and Mass](semantics.md#plurals-groups-cardinality-and-mass). "At most n"
is a denial over n+1 witnesses with pairwise `!=` guards ("At most 1 woman sleeps." becomes
`((X ~ woman), (X sleeps nil), ¬(X ace:individual nil), (Y ~ woman), (Y sleeps nil),
¬(Y ace:individual nil), (X != Y)) => !`), in a condition a helper over those witnesses and `¬` over
it; "at least n" puts n distinct witnesses into the body. The witnesses are pairwise distinct only as
named constants or by `\=`: every individual `ace` creates carries the internal mark
`I ace:individual nil`, which the denials skip, so "John owns a red car. John owns a car. John owns at
most 1 car." is no contradiction. Engine witnesses of generative heads carry no mark and count as
distinct, so "Every man owns a red car. Every man owns a blue car. John is a man. John owns at most 1
car." is one, a closed-world reading for witnesses that the user documentation states with this
example.

Evidence: "at most n" needs n+1 witnesses and n(n+1)/2 guards, and the join grows as k^(n+1) per
subject: "at most 5" over 20 men with 5 cars each processes 781,300 matches per run. Verified with the
marks: two marked cars give no contradiction, a named car beside them none, a second named car one,
recorded like any denial over a condition set; the if-box helper sits on its own negation level and
gives the right model.

### Groups, Distribution, Lists, Sets and Mass Nouns (FM-groups)

Decision DEC-PLURALS; the mapping is in
[Plurals, Groups, Cardinality and Mass](semantics.md#plurals-groups-cardinality-and-mass). A collective
group is the set constant of its members ("John and Mary lift a table." writes
`{John Mary} lifts Table-<h>`); distribution ("each of", "for each of") expands to one fact per
member, never a multi-object fact; a condition on members is `M in G` over set constants; "[Mary,
Bill] is a list." writes `<Mary Bill>`; a mass noun at the top level is one portion individual; units
and measured quantities are refused, because zelph has neither.

Evidence: the group fact entered with the other member order re-enters with no new node; `John in G`
answers `John in {John Mary}`; `((M in G), (G lifts T)) => (M ~ lifter)` derives both members; a
collection re-enters with 4 new nodes, which is why groups are set constants.

### Taking a Statement Back (FM-retract)

Decision [DEC-RETRACT](decisions.md#dec-retract), at M9. The block keyword `ace-retract` removes each
atom of its fact sentences with `zelph/prune-fact`, and is refused, removing nothing, while derived
facts rest on an atom (`zelph/dependents`, `[ace/retract-dependents]`) or another statement nests it
(`zelph/holders`); [Taking a Statement Back: ace-retract](block.md#taking-a-statement-back-ace-retract)
gives the steps. The evidence for the guard: after `bob ~ man` and `(X ~ man) => (X ~ mortal)`,
`.prune-facts bob ~ man` leaves `bob ~ mortal`; zelph keeps no truth maintenance. Before M9 the user
documentation gives the native route: the block's show-zelph line, typed under `.lang ace`, is the
argument of `.prune-facts`, and derived facts stay.

### ACE Text Files (FM-load)

Decision [DEC-LOAD](decisions.md#dec-load), at M2. `(ace/load! path)` reads a file (native builds
only; the wasm build has no user files and gets `(ace/load-text! text)`). The whole document is
analysed first, with APE's paragraph semantics, and rejected as a whole on any error; then one dry run
over all paragraphs, the real run paragraph by paragraph, one inference, and the questions in textual
order. Positions in messages are file lines and columns ([ACE Documents](block.md#ace-documents)).

### Statements Written Without Claiming Them (FM-modal)

Decision DEC-MODAL; host items P-opaque and P-set-match, both built. The mapping is in
[Modality and That-Clauses](semantics.md#modality-and-that-clauses). What the engine does:

- Without an opaque declaration a nested statement is claimed: `john believes (mary waits nil)` makes
  `S waits O` answer, typed, from a `%` block and from a keyword handler alike. That is why the
  that-clause verb and the modal predicates must be opaque.
- An opaque position holds a mention, also when a rule's firing writes it, and a rule condition
  matches inside it by deep unification; a refutation or a refuted fact inside an opaque argument is a
  mention and contradicts nothing.
- A plain single-condition ground rule that is both asserted and mentioned is one node and not in
  force; a ground comma-list rule and `ace`'s one-element-set ground rule, built from Janet, stay in
  force after a typed mention of the same text, unless `ace`'s rule met a plain rule typed before
  ([Traps](#traps)). A rule written through `zelph/mention` is a mention:
  not in force until something claims it, and no data for a rule over rules (DEC-MENTION-RULE). A natively typed plain
  ground rule is still one node with its plain mention (K1-P1), so `:sub/ground-conditional` stays
  refused until the asserted-rule record exists.
- A statement whose object is a rule with variables is rule text: no query sees it, so "John believes
  that every man waits." is stored, but the question whether John believes it is not provable until
  zelph can quote rules (Z145). A rule condition holding a rule with variables binds the parts of a
  stored ground rule, hence `:sub/conditional-with-referents-in-body`.
- A firing that mentions a rule with variables derives nothing when the mentioned rule's conditions are
  a set, and writes `??` witnesses for a plain single condition, hence
  `:sub/conditional-with-referents-in-head`; `ace` builds every conditional it writes into an opaque
  position as a condition set.
- A box with several conditions or a referent is the set constant of its atoms as mentions. A
  condition or a question built with `zelph/set` around variables matches a stored set constant of
  ground members with the same count by a bijective pairing, every pairing enumerated (P-set-match).

## Engine Findings and How the Front End Responds

Each finding is generic engine behaviour, without ACE knowledge; the host specification owns any fix.

| Finding | State | What the front end does |
|---|---|---|
| a generated rule claimed the fresh ground statements of its instance | fixed in zelph 1.0.2 and documented in [Rules That Write Rules](../rule-generators.md#what-stays-the-same) | nothing; DEC-NESTED-IMP stands on the remaining evidence |
| a generator whose inner rule is fully ground never fires and, typed, turns off an identical ground rule typed earlier | a documented corner; category 4, K1-P2 | `ace` flattens that shape |
| negation levels | keyed by pattern: the core form orders the three shapes, the Wikidata form and a rule over predicates do not; NAF conclusions are final | DEC-STRATA refuses the shapes, the engine's verdict is a further refusal clause, the negation records are switched on; the narrowing is planned, not v1 |
| a rule with a plain single condition had a contradiction recorded only when it derived the refutation of a claimed fact; every other one, a denial's included, was reported again on every run | fixed 2026-10-15: a plain single condition is recorded as the one-member condition set is, with the same set constant as record; denials, conclusions meeting a refuted fact and refusals alike | [DEC-RECORD-SINGLE](decisions.md#dec-record-single): `ace` keeps building one-element sets, the form zelph lists for the rule it builds; records no longer depend on it. A host test pins that the set is recorded, and the set prints as `(*{…} ~ conjunction) => …` and re-enters as the same rule (P-record-pin, built) |
| the quiet run must return the contradictions it met | built as P-quiet-run | step 7 of the block contract reads the returned contradictions; records only keep later runs silent (DEC-QUIET) |
| `.new` after a ground rule crashed a script run from the command line | fixed; `.new` inside a module is refused | none |
| under `.import math` a rule of the math stack derives class atoms | the ACE-shaped rules keep stratifiable components of their own beside the 418 rules of `.import math` | `:naf/unstratified` does not see stdlib rules; the engine-verdict clause costs nothing there |
| a rule that mentions a single-condition rule turns its variables into fresh witnesses, where a mentioned condition set derives nothing | intended, documented as a limit ([Mentioning a Rule Is Not Asserting It](../logic.md#mentioning-a-rule-is-not-asserting-it)) | `ace` builds every mentioned conditional as a condition set and refuses a conclusion's that-clause holding a conditional with referents |

## What This Builds On in zelph

- [Logic and Computation](../logic.md): [meta-rules](../logic.md#meta-rules-predicates-as-first-class-nodes),
  [rules that derive rules](../logic.md#rules-that-derive-rules),
  [mentioning a rule](../logic.md#mentioning-a-rule-is-not-asserting-it),
  [opaque predicates](../logic.md#mentioning-a-statement-opaque-predicates),
  [deep unification](../logic.md#deep-unification),
  [facts with multiple objects](../logic.md#facts-with-multiple-objects),
  [negation as failure](../logic.md#negation-as-failure) and its
  [stratified evaluation](../logic.md#stratified-evaluation),
  [inequality constraints](../logic.md#inequality-constraints),
  [generative rules](../logic.md#fresh-variables-generative-rules),
  [path conditions](../logic.md#transitive-path-conditions),
  [contradiction detection](../logic.md#contradiction-detection).
- [Rules That Write Rules](../rule-generators.md) for generators and generators of generators.
- [Neural Networks in the Graph](../neural.md#neural-rule-conditions) for `≈` conditions.
- [SPARQL Queries](../sparql.md), the precedent for a language layered on the Janet API.
- [Scripting with Janet](../janet.md), [Rules and Inference](../rules.md) (clusters, deduction
  modes) and the [command reference](../quickstart.md#full-command-reference).
- [The project's outlook](../about.md#where-the-logic-goes-next) for the planned engine items Z141
  and Z145, and for the asserted-rule record of Z144, which the funded work implements.
- [What zelph Already Provides](foundation.md) for the engine items built for the front end, among
  them [negation levels as data](foundation.md#quiet-runs-the-rules-in-force-and-negation-levels) and
  [opaque predicates](foundation.md#statements-about-statements-opaque-predicates).

## Where Each Item Is Specified

| Item | Page |
|---|---|
| construct ids, families, milestones | [The Construct Table](constructs.md#the-construct-table) |
| the refusal message | [The Refusal Message](constructs.md#the-refusal-message) |
| plan shapes, terms, naming, internal vocabulary, the pin, generated names, `ace/option` keys | [Modules, API and Data Shapes](api.md#naming-in-the-graph) |
| word resolution, the `word-senses` note, the profile on Wikidata | [Words and Wikidata](words.md#resolving-a-word) |
| the block contract: dry run, inference, questions, contradiction reports, show-zelph, `ace-retract` | [The ace Block](block.md#what-a-block-does) |
| the mapping behind every evidence note: rules, nested implications, helpers, static checks, class atoms, cardinality, modality | [From Sentences to the Graph](semantics.md#every-construct-and-its-native-form) |
| universal questions, answers, the derivation echo and justified answers | [Questions, Answers and Explanations](answers.md) |
| formulas, `evaluation`, `ace-math`, identity and inequality | [Numbers and Formulas](math.md) |
| the engine items the front end rests on | [What zelph Already Provides](foundation.md) |
| the category-4 items | [What zelph Still Needs](prerequisites.md#the-seven-items-at-a-glance) |
| tests, including the interop tests | [Tests and Quality Gates](tests.md#re-entry-interop-persistence-provenance) |
| the decisions named on this page | [Design Decisions](decisions.md) |
| the later layers (ACE answers for SPARQL rows, ACE text export) | [Design Routes Taken and Not Taken](routes.md#later-layers-and-the-hooks-they-need) |
