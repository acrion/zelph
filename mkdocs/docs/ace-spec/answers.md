# Questions, Answers and Explanations

A user asks the graph a question in English and gets English back. A yes/no question has three
answers, not two: `Yes.`, `No. It is false that …` when the graph holds the opposite, and `It is not
provable that …` when it holds neither. A wh-question answers with one sentence per match, sorted.
Every answer line is itself an ACE sentence that can be typed back into the same session; a line that
states a fact is accepted there and adds nothing, because it only says what the graph already holds
(the few lines that are checked differently are listed under [the round
trip](#answers-re-enter-the-round-trip)). A question never changes the graph: it is evaluated in a
scratch cluster that is dropped as soon as the answer is rendered.

From milestone M9 an answer can carry its reasons. With `(ace/option :justify true)` each answer is
followed by the user's own sentences it rests on, quoted with the block and sentence they came from;
with `(ace/option :echo true)` each block reports what zelph derived from it, again in ACE; and the
keyword `ace-retract` takes a statement back, or refuses and lists in ACE what still rests on it. All
of this is a mapping onto features zelph already has, released with zelph 1.1.0: questions are
existence probes (the exact one first) and queries inside a scratch cluster, explanations are
`zelph/explain` and the quiet run's `:derived` data, sources are node annotations, and retraction is
`zelph/dependents`, `zelph/holders` and `zelph/prune-fact`. The real runs on this page show each of
them working today.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Every man is a human. No man waits. Bob is a man.
    Is Bob a human? Does Bob wait? Is Bill a man? Who is a human?

    Answer: Yes. Bob is a human.
    Answer: No. It is false that Bob waits.
    Answer: It is not provable that Bill is a man.
    Answer: Bob is a human.
    ```

The block is one theory: its three statements are written, zelph runs once, and then the four
questions are answered in the order they were asked. "No man waits." made zelph derive that Bob does
not wait, so the second answer is `No.`; nothing is known about Bill, so the third is `It is not
provable …`. The native zelph of the block, as `ace/zelph-of` prints it (milestone M4, when the two
rules arrive; a one-condition rule prints as zelph lists it,
[DEC-ONE-CONDITION-PRINT](decisions.md#dec-one-condition-print)):

```
(*{(X ~ man)} ~ conjunction) => (X ~ human)
(*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)
Bob ~ man
# yes/no: Bob ~ human
# yes/no: Bob waits nil
# yes/no: Bill ~ man
X ~ human
```

The two universal sentences are rules, "Bob is a man." is a fact. A question writes nothing, so
`(ace/option :show-zelph true)` prints no line for it; `ace/zelph-of` renders a yes/no question as a
comment, because it is decided by probes rather than by a native query, and a wh-question as the query
it runs. Typed natively under `.lang ace`, that last line answers `Answer: Bob ~ human`: the ACE answer
and the native answer are the same match (the naming languages are on [Words and
Wikidata](words.md#naming-languages)).

The page covers, in this order: what a question does to the graph, how a question becomes an action,
yes/no questions, class questions, wh-questions, the output size, the round trip of answer lines,
questions that hold a conditional ("Is it true that every man is mortal?"), the verbaliser that writes
every answer, and the M9 features: justified answers, the derivation echo, persistent provenance and
`ace-retract`. Statements and their native forms are on [From Sentences to the
Graph](semantics.md#every-construct-and-its-native-form), the block's steps and messages on [The ace
Block](block.md), arithmetic questions on [Numbers and Formulas](math.md#questions-and-answers). A
shorter tour of every feature is [A Tour in Sessions](sessions.md).

Two conventions hold for every session on this page. The 16 hex digits in an individual's name
(`Dog-6f1c2a9e0b3d4c5a`) are illustrative; the real ones are computed from the sentence ([Canonical
Text and Names](semantics.md#canonical-text-and-names)). The wording of messages is illustrative,
while their structure — class words, id, position, the sentence line with the marker, the hint lines —
is normative ([Messages](block.md#messages)).

## A Question Changes Nothing

**One theory per block** ([DEC-QTIME](decisions.md#dec-qtime) (a)). A block is handled as one theory:
first all of its writes, then one inference, then its questions in textual order. A question therefore
sees the whole block, the sentences after it included:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace Is John a man? John is a man.

    Answer: Yes. John is a man.
    ```

A block that holds a question always runs the inference, even with auto-run off
([DEC-RUN](decisions.md#dec-run)): after a `.load`, which switches auto-run off, a block that only
writes infers nothing until `.auto-run` is switched on again, while a block that asks still answers
from a fixpoint ([When the Block Reasons](block.md#when-the-block-reasons)).

**A question never asserts.** Each question runs in its own scratch cluster `ace:q`. The handler reads
the caller's cluster first, `(def prev (zelph/cluster))`, switches to `ace:q`, evaluates, renders the
answer lines, restores with `(zelph/cluster prev)` and drops `ace:q` with `(zelph/cluster-drop
"ace:q")`. A literal `(zelph/cluster nil)` is never written: it would stop recording in a cluster the
user had active. The drop costs O(question), not O(graph) (host item P-scratch-drop), and it happens
within the same handler call, so the host's auto-run after the handler never sees the question's
pattern nodes.

**Nothing is created to be compared.** Every content word of the question is resolved without creating
(steps 1-3 of [word resolution](words.md#the-four-steps)). A word that falls through to step 4, the
step that would create a new node for a statement, is neither created nor announced, and it ends the
evaluation: a yes/no question is then not provable, a wh-question has no rows. An individual named in
a question (`(:skolem …)` term) is looked up with `(zelph/lookup name "ace")`, internal vocabulary by
its name in the language `zelph` ([Internal Vocabulary](api.md#internal-vocabulary)). The one
exception is an identity between two named constants ("Is John Bill?", M5): the resolution step
decides it by node identity, or by canonical symbol for names not yet in the graph, before anything
runs ([Identity](math.md#identity)).

**Ground atoms are probed, never built.** Every ground atom of the question, nested ones included, is
checked bottom-up with `(zelph/exists s p o :exact)`: one hash lookup, O(1) also on a miss, and it
creates nothing. When a ground atom that a yes/no question asks about misses, the plain
`(zelph/exists s p o)` asks once more; it reads a fact with further objects as a rule condition reads
it, so a native `John sees Mary Bill` answers "Does John see Mary?" with `Yes.`
([DEC-YESNO-WIDER](decisions.md#dec-yesno-wider); [Facts With Several
Objects](#facts-with-several-objects)). This second probe is the one named exception to the rule that
`ace` calls `zelph/exists` only with `:exact` (invariant 7 of [Invariants](api.md#invariants)), and
the source-rule test "ace work: the ace sources make no forbidden graph call" admits exactly that
call. The engine does that wider match only while the graph may hold a fact with several objects (one
was written, a statement or set constant was merged by name, or a network was loaded whose file does
not record that it holds none, as no file saved by an older engine does); otherwise the second probe
is a hash lookup as well. When it does the wider match, its cost is bounded by the subject's facts with
that predicate, never a scan of the graph; M3 times it on the published `-small` network, as a row of
[The Budget Table](plan.md#the-budget-table). An absent
nested atom ends the evaluation, since no pattern over it can match; an absent ground item is `No.` or
not provable by the value table of [Yes/No Questions](#yesno-questions).

A ground atom is never built with `zelph/fact` to be checked — an operand under `¬` included: a
ground operand built with `zelph/fact` under `zelph/negate` inside `ace:q` would be claimed there, and
the negation as failure would then answer nothing. Patterns are built only around `zelph/var`s.
`zelph/exists` finds no engine bookkeeping (a contradiction record's marking, a rule
pattern's mark), while a user's refutation marking `(F) ~ refuted` of a fact answers it
([DEC-BOOKKEEPING](decisions.md#dec-bookkeeping)). From M8 there is one exception to "absent ends the
evaluation": an atom in an opaque argument position that is mentioned (`zelph/mentioned`) is built
with `zelph/mention` inside `ace:q`, and the evaluation goes on; what the question writes into an
opaque argument is data of the scratch cluster, which the drop removes
([DEC-OPAQUE-QUERY](decisions.md#dec-opaque-query)).

Open for M3: whether a ground atom nested in a pattern takes the second probe as well; the exception
above covers only the ground items of a yes/no question. The query itself reads a wider fact in a
nested position, so an exact miss that ends the evaluation there would answer `No answer.` where the
query would find rows.

The tests "ace keyword: questions leave no nodes behind", "ace questions: a question never asserts"
and "ace keyword: a question sees the whole block, also later sentences" pin these rules from M1.

This real run shows the probes a yes/no question is built from, on a graph that holds one fact, one
refutation and one fact with two objects. The exact probe is true for the fact and false both for the
refuted fact and for a fact nobody stated. For the two-object fact `john sees mary bill` the exact
probe of `john sees mary` is false and the plain probe true: that second probe is how a native fact
with several objects answers a question about one of them. The refutation is found by a query with
only the subject a variable, run in the scratch cluster `ace:q` and dropped afterwards with its three
nodes. The cluster `session` holds the user's nodes; its count, 11, is the same before and after the
question:

```
zelph> .cluster session
Active cluster: session
zelph> john ~ man
zelph> ¬(bill ~ man)
zelph> john sees mary bill
zelph> %(zelph/clusters)
@[("session" 11)]
zelph> %[(zelph/exists "john" "~" "man" :exact) (zelph/exists "bill" "~" "man" :exact) (zelph/exists "tom" "~" "man" :exact)]
(true false false)
zelph> %[(zelph/exists "john" "sees" "mary" :exact) (zelph/exists "john" "sees" "mary")]
(false true)
zelph> %
(def prev (zelph/cluster))
(zelph/cluster "ace:q")
(def rows (zelph/query (zelph/fact (zelph/fact (zelph/var "X") "~" "man") "~" "refuted")))
(def refuted (map |(zelph/name (get $ 'X)) rows))
(zelph/cluster prev)
(def dropped (zelph/cluster-drop "ace:q"))
(zelph/out (string/format "refuted: %q, dropped with ace:q: %d nodes" refuted dropped))
%
refuted: @["bill"], dropped with ace:q: 3 nodes
zelph> %(zelph/clusters)
@[("session" 11)]
```

## From Question to Action

A question reaches the planner as a normalized question box D: the DRS of the question with its
`query` conditions removed into a map from referent to question word (`who`, `what`, `which`, `where`,
`when`, `how`, `howm`). The plan turns D into one or more ask actions, each carrying the sentence's
source `{:block :sid :span :text}` (the plan's data shape is on [Modules, API and Data
Shapes](api.md#plan)); the construct ids are rows of the [construct table](constructs.md#the-construct-table).

| Shape of D | Action | Construct id | From |
|---|---|---|---|
| no own referent, no class item; every item a ground atom, a one-atom `:neg` or a one-atom `:naf` | `(:ask-yn {:atoms (Item …) :drs D})`, items `Atom`, `(:refuted Atom)`, `(:naf Atom)`, all constants | `:question/yes-no` | M1 |
| own referents, or a class item (`(:class …)`, also under a one-atom `:naf`), no `query` | `(:ask-yn {:pattern Pats :drs D})`; the resolution step turns it into `:atoms` when the expansion leaves no variable | `:question/yes-no` | M1 |
| `query` `:who`, `:what`, `:which` | `(:ask-wh {:pattern Pats :vars (all of D's referents) :answer {Ref Var} :drs D})` | `:question/wh` | M3 |
| the query referent substituted by a constant ("Who is John?") | two `:ask-wh` actions with `:answer {}`: patterns `T <instance> X` (the classes are the answer) and `T is X` (X a variable); answered by the describe rule | `:question/wh` | M3 |
| `(:or D1 D2)` in D | one action per disjunct, all sharing the sentence's source; combined at answer time | `:or/in-question` | M4 |
| `:copula/identity` ("Is the man John?") | the identity verdict of the resolution step | `:copula/identity` | M5 |
| a `:formula/compare` formula (a numeric identity copula included) or an arithmetic term | `:ask-value`, handed to the math hook, never to the rows above | the A ids | M5 |
| `query` `:where`, `:when`, `:how` | `E ace:occurrence A`, `E P Y` with P a variable; a row is kept when P's preposition (P's name without its leading `is `) is classified locative, temporal or manner in the lexicon data; `how` also takes `E ace:manner Adv` | `:question/where-when-how` | M6 |
| `query` `:howm` | the counting pattern ([How Many](#how-many)) | `:question/how-many` | M7 |
| `(:imp …)` without `query` | the guarded hypothetical check ([below](#questions-that-hold-a-conditional)) | `:rule/in-question` | M4 |
| modal and label boxes ("Can John swim?", "Does John believe that …?") | built with `zelph/mention` ([Modal and Subordinated Statements](#modal-and-subordinated-statements)) | the M and S ids | M8 |

**Patterns** are the rule-body patterns of [From Sentences to the Graph](semantics.md#rules), with the
question's referents as variables. The naming scope is `:question`; variables are named in order of
first occurrence, `X`, `Y`, `Z`, then `A` to `W`, then `_v27`, …. A class pattern's V takes a letter
only where the expansion uses it, in the subclass closure of the Wikidata profile; in the core profile
`T ~ C` has no variable of its own and takes none ([DEC-VAR-LETTERS](decisions.md#dec-var-letters)).
The plan, which is the same with and without Wikidata, carries each V under a name outside the letters
(`_c1`, `_c2`, …), and the resolution step names the scope again where a V survives, so a `_c` name
never reaches the graph or an answer ([Variables](api.md#variables)).

- A one-atom classical negation becomes `(:refuted Atom)`, the pattern `((Atom) ~ refuted)` ("Who
  does not sleep?", the shape of corpus case 1448, `:neg/atom-in-question`, M4).
- A one-atom negation as failure becomes `(:naf Atom)`, `¬(Atom)` inside the question's query ("Which
  visitor does not provably sleep?", the shape of 2772, `:naf/in-question`, M4).
- Over a class item it is the question-only pattern `(:naf (:class T C V))`, evaluated in Janet
  (below), because `¬(…)` holds one pattern and the class expansion may give two; in a rule body the
  same item goes through a positive helper rule instead, since a question writes no rule.
- Class items are `(:class T C V)` patterns ([Class
  Questions](#class-questions-and-the-subclass-closure)).

The plan of "Is John a human?" and its `ace/zelph-of` line, without Wikidata:

```
(:ask-yn {:drs {:refs () :conds ()} :pattern ((:class (:name "John" :name) (:name "human" :noun) (:var "_c1")))} {:text "Is John a human?" :block 1 :span (11 15) :sid 3})
```

```
# yes/no: John ~ human
```

A class pattern renders as `T ~ C` without a resolution map; with one on a Wikidata graph it renders
by the profile, `(T P31 V), ((V P279 C) closure zero-or-more)`, and every line is followed by a
display line `# nodes: …` naming the resolved nodes ([What ace/zelph-of
Prints](semantics.md#what-acezelph-of-prints)). Plans hold names, never nodes, so the plan of "Is John
a man?" is the same on an empty graph and on a Wikidata graph.

**Order of actions**: `:warn` anywhere, `:check` first, then the write actions in sentence order, then
at most one `:infer`, then the question actions in sentence order.

## Yes/No Questions

| Answer | Line | Meaning |
|---|---|---|
| yes | `Answer: Yes. <S>` | the graph holds it (asserted or derived) |
| no | `Answer: No. It is false that <S>` | the graph holds its refutation (asserted or derived) |
| unknown | `Answer: It is not provable that <S>` | neither |

On a graph without rules (milestone M1) the three answers are already reachable, the `No.` through a
refutation typed natively:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> .lang ace
    ace> ¬(Mary ~ man)
    ace> ace John is a man.
    Is John a man? Is Mary a man? Is Bill a man?

    Answer: Yes. John is a man.
    Answer: No. It is false that Mary is a man.
    Answer: It is not provable that Bill is a man.
    ```

`.lang ace` only makes the native line meet the nodes `ace` uses; it never changes what the block
resolves ([DEC-CONTENT-NS](decisions.md#dec-content-ns)). From M4, "John does not wait." refutes, "No
man waits." derives refutations, and the `No.` answers come from ACE itself (the opening session).

**Evaluation** (`ace/ask`, effect class "reads"), for one action:

1. Resolve every content term without creating (above). An absent name ends the evaluation: a yes/no
   question is not provable.
2. Check the ground items, and every ground atom nested in a pattern, bottom-up with `zelph/exists …
   :exact`; a ground item that misses is probed once more with the plain `zelph/exists` (above).
3. Give each item a value:

    | Item | yes | no | unknown |
    |---|---|---|---|
    | `Atom` | it exists | it is refuted | otherwise |
    | `(:refuted A)` | A is refuted | A exists | otherwise |
    | `(:naf A)` | A does not exist | A exists | — |

    "A is refuted" is decided by the exact probe of A's marking, `(zelph/exists F "~" "refuted"
    :exact)` with F the fact node of A: a user's refutation marking answers `zelph/exists`, the
    engine's bookkeeping never does (DEC-BOOKKEEPING). The marking is never built as a ground pattern,
    which would be a claim. A conjunction of items is yes iff all are yes, no iff any is no, and
    unknown otherwise.

    Open for M1: how `ace/ask` reaches the fact node F of a ground atom without creating it. On
    today's engine the query `((X P O) ~ refuted)` in `ace:q`, with only the subject a variable and
    the rows filtered by node identity in Janet, finds the refutation and leaves nothing behind (the
    real run above).

4. Patterns: inside `ace:q`, every `zelph/var` the action needs is created, then one `zelph/query`
   runs (a conjunction for two or more patterns, `zelph/path` for a closure); rows are read by
   variable symbol and turned into bindings before the drop. A yes/no pattern is yes when the query
   returns a row. A `(:naf (:class T C V))` item is not part of that query: without a subclass
   predicate it is the single pattern `¬(T <instance> C)`; with one, the class pattern is queried in
   the same `ace:q` with T bound (once for a ground T, once per row otherwise), and the item holds
   where that query has no row — for a yes/no question yes iff there is no row, for a wh-question a
   row is kept iff its class query has none.
5. Combine the actions of one sentence (a disjunction gives several): wh rows are united and
   deduplicated by binding; yes/no values are "yes if any is yes, no if all are no, else unknown".

**The line.** For ground items, `<S>` is `(ace/verbalise D)`, the question box rendered as a
statement. For a pattern, `<S>` is the line the wh rule ([below](#wh-questions)) gives for the row
whose line sorts first: "Is there a man who waits?" → `Answer: Yes. Man-6f1c2a9e0b3d4c5a waits.` A
class question answers with the asked class, never with the intermediate class the closure walked
through.

### Facts With Several Objects

`ace` never writes a fact with several objects: "John sees each of Mary and Bill." becomes one fact
per member ([zelph Features Through ACE](zelph-features.md#facts-with-several-objects)). A native one
typed by a user still answers ACE questions about each of its objects, through the second, plain
probe of step 2 (from M3, when transitive verbs arrive):

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> .lang ace
    ace> John sees Mary Bill
    ace> ace Does John see Mary? Does John see Sue?

    Answer: Yes. John sees Mary.
    Answer: It is not provable that John sees Sue.
    ```

The exact probe of `John sees Mary` misses, because the graph holds the two-object fact and not the
one-object one; the plain probe then matches the two-object fact as a rule condition `John sees Mary`
would. The other direction does not hold: two separate facts `John sees Mary` and `John
sees Bill` do not make the two-object fact true. `Sue` is in no fact, so the second question ends at
the resolution of the name. The test "ace questions: a native fact with several objects answers a
single-member yes/no question" pins this session from M3.

## Class Questions and the Subclass Closure

ACE 6.7 has no construct for the transitive closure of a class hierarchy, and Wikidata does not state
classes with zelph's core `~`: it uses P31 (instance of) and P279 (subclass of). `ace` therefore has
two vocabulary entries, the **instance predicate** and the **subclass predicate**, and a graph's
**profile** decides which nodes they are ([DEC-PATHS](decisions.md#dec-paths) (b)): `ace:wikidata`
(instance P31, subclass P279) or `ace:core` (instance core `~`, no subclass predicate). The profile is
pinned once per graph by one internal fact, `ace:profile ace:uses ace:wikidata` or `ace:profile
ace:uses ace:core`, written right before the first write action that holds a class item (a statement,
or a rule with one in a head or a condition) — never by a question, which writes nothing. Before the
pin exists, a question uses the profile the pin will take: `ace:wikidata` when the graph holds P31 and
P279 in the language `wikidata`, else `ace:core`. A node the user named `ace:instance` or
`ace:subclass` in the language `ace` wins over the profile for its predicate. Details: [Classes: the
Instance and Subclass Predicates](words.md#classes-the-instance-and-subclass-predicates).

One function, `ace-drs/class-pattern`, builds every class item. In a question it is the pattern
`(:class T C V)`, V a fresh variable of the question's scope. The resolution step expands it:

- with a subclass predicate: `(T <instance> V)` plus `((V <subclass> C) closure zero-or-more)`; zero
  steps include V = C, so a direct member matches too;
- without one: `(T <instance> C)`, and V is unused.

A class item of a yes/no question ("Is Douglas Adams a human?") is always planned as a `:pattern`,
because a path is not checked with `zelph/exists` and the plan does not know whether there will be a
subclass predicate. A pattern that the expansion leaves without a variable becomes `:atoms` in the
resolution step, so its atoms are probed with `zelph/exists` as above and never built. The refuted
check behind a `No.` answer always uses the plain atom `T <instance> C`, the form a statement writes,
never the pattern: "Mary is not a man." refutes exactly that atom.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .load /tmp/ace-fixture/ace-fixture.json
    …
    zelph-> .import ace
    zelph-> ace Rex is a dog. Is Rex an animal? Is Rex a mammal?
    Is Douglas Adams a writer?

    Note [ace/word-senses]: words read as Wikidata items (the lowest Q-number among the items with this label):
      "Rex" is a new individual (no item carries the label)
      "mammal" is Q7377 (1 other item carries the label; https://www.wikidata.org/wiki/Q7377)
      "Douglas Adams" is Q42 (3 other items carry the label; https://www.wikidata.org/wiki/Q42)
      To choose another item, write n:Q… for a noun or p:Q… for a name, or declare the word, e.g. noun_sg(mammal, 'Q…', neutr).
      (ace/senses "mammal") lists the candidates.
    Answer: Yes. Rex is an animal.
    Answer: It is not provable that Rex is a mammal.
    Answer: It is not provable that Douglas Adams is a writer.
    ```

The session runs on the Wikidata-style fixture of the test suite, a small dump in Wikidata's JSON
format that M0 adds as `src/test/data/wikidata/ace-fixture.json` and that is imported through the real
importer ([The Wikidata-Style Fixture](tests.md#the-wikidata-style-fixture)); the omitted lines are
the load's report. The note follows the rules of [The word-senses Note](words.md#the-word-senses-note):
"dog", "animal" and "writer" each have one carrier on the fixture and get no line. "Is Rex an animal?"
follows Q144 (dog) → Q110551885 → Q729 (animal). "Is Rex a mammal?" is not provable because `mammal`
resolves to Q7377, the lowest Q-number among the items with that label, which the dog's subclass chain
does not reach; the same holds on today's Wikidata. "Is Douglas Adams a writer?" is not provable
because Wikidata states occupations with P106, not with P31; the way to ask is a relation ("What is an
occupation of Douglas Adams?", [below](#relations-on-wikidata); [What Wikidata Models
Differently](words.md#what-wikidata-models-differently)). These cases are pinned by "ace words: a
class question follows the subclass predicate" (M1).

The same patterns, typed natively on a few Wikidata-shaped facts: a yes/no question binds T (the first
query finds a row, the second none), a wh-question leaves it a variable (`∗` is the zero-or-more
closure):

```
zelph> Rex P31 Q144
zelph> Fido P31 Q144
zelph> Tom P31 Q146
zelph> Q144 P279 Q110551885
zelph> Q110551885 P279 Q729
zelph> Rex P31 V, V P279∗ Q729
Answer: Rex P31 Q144, Q144 P279∗ Q729
zelph> Rex P31 V, V P279∗ Q7377
zelph> X P31 V, V P279∗ Q729
Answer: Q144 P279∗ Q729, Rex P31 Q144
Answer: Q144 P279∗ Q729, Fido P31 Q144
```

## Wh-Questions

A wh-question (`who`, `what`, `which`, M3) is one `zelph/query` in `ace:q`, with the class patterns
and closures above. Each distinct row becomes one answer line:

- **Bindings.** A binding maps each referent of D to the spelling `ace-zelph/render` gives its node in
  the role `:name` ([Rendering a Node](#rendering-a-node)), to a data value (a string is a character
  list, read back with `zelph/car` and `zelph/cdr` and reversed; a number per [Numbers and
  Formulas](math.md#printing)), or to `(:some C)` for an unnamed node, C the class read from the
  node's instance facts (an engine witness prints as its class), nil when it has none. Rendering
  happens before the drop of `ace:q`.
- **The line** (`ace-verbalise/line`). The line is `(ace/verbalise D' bindings)`, where D' is D
  without its `query` conditions and without the `object` conditions on bound referents — except where
  an object is the only condition left on its referent: "Who is a man?" answers `John is a man.`,
  "Which man is rich?" answers `John is rich.`
- **Empty.** A wh-question without rows prints `No answer.` (without the `Answer: ` prefix; it is no
  ACE and is never re-entered).
- **Order.** Lines of one question are sorted by their text (parallel unification returns rows
  unordered); questions answer in sentence order. Each line is one `zelph/out` call.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace John is a man. John is rich. Bill owns a dog.
    Who is a man? Who owns a dog? Who is John? Who is a cat?

    Answer: John is a man.
    Answer: Bill owns Dog-6f1c2a9e0b3d4c5a.
    Answer: John is a man.
    Answer: John is rich.
    No answer.
    ```

"Who is John?" is answered by the **describe rule**: its two actions ask for the classes of John
(`John <instance> X`) and for his adjectives (`John is X`), and each row prints `John is a <class>.`
or `John is <adjective>.`; rows whose bound node renders as no ACE word are skipped. "Who is a cat?"
has no rows: `cat` is in no fact, so the word resolves to nothing and the question ends there.

A dog that a rule made up is no individual with a name. It is an engine witness, the fresh node of a
generative rule (see [Fresh Variables: Generative
Rules](../logic.md#fresh-variables-generative-rules)), and it is rendered as an indefinite noun
phrase. Continuing the session above (from M4, when the rule arrives):

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace Every man owns a dog. John is a man.
    Who owns a dog?

    Answer: Bill owns Dog-6f1c2a9e0b3d4c5a.
    Answer: John owns a dog.
    ```

### Negation in a Wh-Question

From M4, a one-atom "not" asks for refuted facts, a one-atom "not provably" asks for absent ones. The
answer lines are the box D' with its negation:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace John is a visitor. Mary is a visitor. Mary sleeps. Bill does not sleep.
    Who does not sleep? Which visitor does not provably sleep?

    Answer: It is false that Bill sleeps.
    Answer: It is not provable that John sleeps.
    ```

A complex classical negation in a question ("Is there no dog?", the shape of corpus case 763) is
refused as `:neg/complex-in-question` with the hint to say "not provably" instead
([DEC-NEG](decisions.md#dec-neg) (a): the two negations are never mixed).

### Relations on Wikidata

From M3, a relational noun ("the author of", "an occupation of") resolves in the relation role; when
it resolves to a Wikidata property, no class atom is added ([DEC-OF](decisions.md#dec-of)). The second
tutorial transcript, pinned by "ace tutorial: words reach Wikidata items" (M3), shows a class question
and a relational question on the fixture:

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
    ```

The occupation is the item writer, Q36180, reached through `Q42 P106 Q36180`. It prints as `p:Q36180`
because its English label "writer" begins with a lower-case letter and so is no spelling of a name. A
relational answer lists the value of every rank Wikidata states: "What is a capital of Germany?"
answers with Berlin and Bonn on the fixture, since the importer reads every item-valued claim (pinned
by "ace words: a relational answer lists the value of every rank", M3).

### Where, When, How

From M6, a modified sentence keeps its plain fact and adds an occurrence event: "John waits in a
park." writes `John waits nil` and `E ace:occurrence (John waits nil)`, `E "is in" Park-<h>`
([DEC-EVENTS](decisions.md#dec-events); [Events and Modifiers](semantics.md#events-and-modifiers)). A
where/when/how question queries the event's modifiers with the predicate as a variable and keeps the
rows whose preposition the lexicon classifies as locative, temporal or manner; `how` also takes `E
ace:manner Adv`. Two modifiers asked together must hold of one event: after "John waits fast." and
"John waits in a park.", "Does John wait fast in a park?" is not provable, because the two modifiers
belong to two events (pinned by "ace events: where, when and how ask the event", M6).

### How Many

From M7, the count of a how-many question is the number of distinct nodes X with `S P X, X <instance>
C`, united with `S P G, X in G, X <instance> C` for collective objects (a group is a set constant). It
is closed-world over individuals and witnesses ([Plurals, Groups, Cardinality and
Mass](semantics.md#plurals-groups-cardinality-and-mass)). The line renders the counted referent as the
count and the noun, singular for 1:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace John owns 3 dogs. Mary owns a dog.
    How many dogs does John own? How many dogs does Mary own?

    Answer: John owns 3 dogs.
    Answer: Mary owns 1 dog.
    ```

### Modal and Subordinated Statements

From M8, a question over a modal box or a that-clause is built with `zelph/mention` in `ace:q`, the
outer fact checked with `zelph/exists … :exact`; a wh pattern matches inside the mentioned statement
by deep unification ("Who can swim?"), and a box with several conditions or a referent is matched as a
set constant by its members (P-set-match). A mentioned statement answers no plain question: after "A
customer can enter a card.", "Is there a customer?" answers `Yes.` and "Is there a card?" `It is not
provable …`, since the sentence does not say that there is a card. A rule inside a that-clause ("John
believes that every man waits.") is a mention as well: it is not in force, `zelph/rules` does not list
it and it never fires ([DEC-MENTION-RULE](decisions.md#dec-mention-rule)). A question over it writes
a new collection in `ace:q` and answers "not provable", which is sound ([Modality and
That-Clauses](semantics.md#modality-and-that-clauses)).

### Native Rules and Numbers

A native rule that fires on ACE facts answers ACE questions in ACE: with the native meta-rule `((X R
Y), (R is symmetric)) => (Y R X)` and `marries is symmetric` typed under `.lang ace`, "John marries
Mary. Who marries John?" answers `Mary marries John.` (pinned by "ace interop: a native meta-rule fires
on ACE-built facts", M3; the session is on [zelph Features Through
ACE](zelph-features.md#native-rules-and-meta-rules)).

From M5, a question that holds a formula or an arithmetic term is an `:ask-value` action and is
answered by the math stack ("What is 2 + 3?" → `Answer: 2 + 3 = 5.`); its answer forms are on [Numbers
and Formulas](math.md#questions-and-answers).

## Output Size

At most `(ace/option :max-answers)` lines are printed per question (default 100). All rows are
rendered and sorted, the first `:max-answers` are printed, then one line `… and N more answers.`
(`answer` when N is 1), without the `Answer: ` prefix. The cost is linear in the rows the query
returns, never in the size of the graph.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> %(ace/option :max-answers 3)
    3
    zelph> ace John is a man. Bill is a man. Tom is a man. Sam is a man. Bob is a man.
    Who is a man?

    Answer: Bill is a man.
    Answer: Bob is a man.
    Answer: John is a man.
    … and 2 more answers.
    ```

Pinned by "ace options: max-answers caps the answers and says how many are left" (M3; [Options and
Output Size](tests.md#options-and-output-size)).

## Answers Re-enter: the Round Trip

Every answer line is ACE that the same session accepts. This is the property that makes the verbaliser
the inverse of the mapping, and three oracles of the test suite check it; [Oracles Defined
Once](tests.md#oracles-defined-once) owns their procedures. In short:

- **RT-1, answers re-enter** ([RT-1](tests.md#rt-1-answers-re-enter)). Strip `Answer: ` and a leading
  `Yes. ` or `No. `. A line that starts with `It is not provable that` is checked by asking its
  question again after the same premises, in a fresh cluster: the same line must come back. Any other
  line, entered as an `ace` block in a fresh cluster after the original block, must be accepted and
  leave the cluster at 0 nodes with the rule count unchanged; a refused line fails, since a refused
  block also creates 0 nodes. `No answer.`, the `… and N more answers.` line and the support lines of
  `:justify` are not ACE and are skipped. `No.` lines re-enter cleanly because refuting a refuted fact
  again creates nothing; a `word-senses` note printed by the re-entry is allowed.
- **Acceptance only**, without the 0-node check: a line that names an engine witness ("John owns a
  dog." may create a new individual), a how-many line, a line that names a node through a name in
  another language (a node named only under `.lang zelph`, step 5 of [Rendering a
  Node](#rendering-a-node)), and a `Yes.` line whose atom the graph holds only inside a native fact
  with more objects, whose re-entry writes the narrower fact ([Facts With Several
  Objects](#facts-with-several-objects)).
- **Exempt**: the `Yes.` and `No.` lines that answer a universal question ([Questions That Hold a
  Conditional](#questions-that-hold-a-conditional)). The first is a rule and the second a negated
  universal; re-entered, each is written as what it says (a rule; one individual with a refuted fact)
  instead of adding nothing.
- **By milestone**: a line that uses a construct supported only later must be refused with
  `[ace/unsupported]` naming a milestone; a `No. It is false that …` answer at M1-M3, reachable
  through a native refutation, re-enters as `:neg/fact`, which arrives at M4. An answer that needs a
  construct without a route is a verbaliser defect.
- **RT-2, the verbaliser reads back.** For the DRS d of every accepted text, `(ace/verbalise d)` is
  either `{:refused id}`, which is allowed and counted, or ACE that parses back to the same status, the
  same refused ids and a DRS equivalent to d, and for a text without questions to the same graph; a
  text that parses back to something else is a failure ([RT-2](tests.md#rt-2-the-verbaliser-reads-back)).
- **RT-3, the shown zelph is native zelph.** The `:show-zelph` lines of a block's write actions,
  entered natively under `.lang ace` in a fresh cluster, add no node, apart from lines that name
  internal vocabulary, the math stack or string data; on a Wikidata graph RT-3 does not apply
  ([RT-3](tests.md#rt-3-native-equivalence-of-the-shown-zelph)).

## Questions That Hold a Conditional

"Is it true that every man is mortal?" (the shape of corpus case 1974) asks whether a universal
statement follows. zelph answers it by assuming a man about whom nothing else is known, running the
rules once, and looking whether the assumed man came out mortal — all inside the question's scratch
cluster ([DEC-QUNIV](decisions.md#dec-quniv) (c), construct `:rule/in-question`, M4).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace Every man is a human. Every human is mortal. John is a man.
    Is it true that every man is mortal? Is it true that every man waits?

    Answer: Yes. If there is a man X then X is mortal.
    Answer: It is not provable that if there is a man X then X waits.
    zelph> ace Bob is a man. Bob does not sleep.
    Is it true that every man sleeps?

    Answer: No. It is false that if there is a man X then X sleeps.
    zelph> ace Every woman is a human. No woman is a human.
    Is it true that every woman is a human?

    Answer: It is not provable that if there is a woman X then X is a human.
    ```

The answer lines render the question box by the templates of the verbaliser, which write a universal
as `If there is a man X then …` ([Templates](#templates)). The second block finds a counter-instance,
Bob, among the graph's own individuals. In the third, the two rules are consistent as long as there is
no woman; the assumed woman becomes a human and is refuted as one, and an assumption that contradicts
the graph proves nothing.

**Plan.** `(:ask-yn {:assume (Atom …) :atoms (Atom …) :counter (Pat …) :drs D} Src)`, with `:pattern`
in place of `:atoms` when the then-box needs a query:

- `:assume` is the if-box A with its referents as hypothetical terms `(:hyp Ref Head Key)`, named
  `ace:hyp-<16 hex>` in the language `zelph` (the hex digits are the FNV-1a-64 of A's canonical text
  and the referent's index). A hypothetical witness is never a content-addressed individual: "There is
  a man." already made `Man-<h>`, whose own facts would leak into the hypothesis.
- `:atoms` is the then-box C over the same terms.
- `:counter` is A's patterns plus C's atoms as `(:refuted …)`: a known instance of A whose C is
  refuted.

**Guard** (in the resolution step, before the dry run). `(zelph/rules)` lists the rules in force with
their negated conditions and guards (P-rule-list); a rule that is only mentioned, such as one inside a
that-clause, is not in force and is not listed. The may-derive graph of the [static
checks](semantics.md#static-checks) is built over those rules plus the block's own resolved rules — not
yet written, but in force when the question runs — and entered at the resolved `:assume` atoms. When a
rule with a "not provably" (NAF) condition or a `!=` guard is reachable, the whole block is refused
with `[ace/unsupported]` naming `:rule/unsafe-hypothesis`, and nothing of it is written: a fresh
witness is "not provably" everything, so such a rule would decide the answer from ignorance. `ace/plan`
cannot decide this alone, because the rules in force are graph data and the hypothesis's predicates
are known only after resolution.

**Evaluation**, in `ace:q`:

1. Run the `:counter` query first, before anything is assumed. A row → `No. It is false that <S>`. The
   counter-instance always comes from the graph's own individuals: run after the assumption, the
   counter query would match the witness wherever the graph refutes C for every A, and with no A in
   the graph that would answer `No.` although nothing refutes the universal.
2. Otherwise assert `:assume` and run `ace/infer!` once. When that run meets a contradiction, the
   answer is `It is not provable that <S>`; its contradictions are not reported, they go with `ace:q`.
3. Otherwise check `:atoms` (a query when C declares referents) → `Yes. <S>`, else `It is not provable
   that <S>`.
4. Drop `ace:q`.

The block's own inference runs before its questions, so the hypothesis starts at a fixpoint. When the
hypothetical run derived facts, the drop invalidates the delta record and the next `run-delta` is a
full pass ([DEC-DROP-DELTA](decisions.md#dec-drop-delta)): correct, only slower.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> ace If a man is not provably married then the man is a bachelor.
    Is it true that every man is a bachelor?

    Error in line "ace If a man is not provably married then the man is a bachelor. …": Correct ACE, but zelph cannot use it [ace/unsupported]: sentence 2, line 2, column 1: a conditional in a question that a rule with "not provably" could decide, construct rule/unsafe-hypothesis.
      <>Is it true that every man is a bachelor?
      An assumed man is not provably married, so the rule "If a man is not provably married then the man is a bachelor." would decide the answer.
    ```

`:rule/unsafe-hypothesis` is a context id: the resolution step finds it over the plans and the rules in
force, with no evidence condition of its own, so the position is the first token of the question
([Positions](constructs.md#positions)). A question that holds a conditional and a question word ("How
does every dog bark?", the shape of 2087) is `:rule/in-wh-question` and has no route: the hypothetical
witness decides whether every A is C, not which individuals answer.

The hypothetical check works on today's engine. This real run does by hand what `ace` does inside
`ace:q`: the counter query, one fresh witness, one quiet run, one exact probe, the drop. "Every man is
mortal" follows, "every man waits" does not, "every man sleeps" has a counter-instance, and no witness
is left behind:

```
zelph> (X ~ man) => (X ~ human)
zelph> (X ~ human) => (X ~ mortal)
zelph> john ~ man
(john ~ human) ⇐ (john ~ man)
(john ~ mortal) ⇐ (john ~ human)
zelph> bob ~ man
(bob ~ human) ⇐ (bob ~ man)
(bob ~ mortal) ⇐ (bob ~ human)
zelph> ¬(bob sleeps nil)
zelph> %
(defn every? [cls pred obj]
  (def prev (zelph/cluster))
  (zelph/cluster "ace:q")
  (def x (zelph/var "X"))
  # first the counter query, before anything is assumed
  (def counter (zelph/query (zelph/conjunction (zelph/fact x "~" cls)
                                               (zelph/fact (zelph/fact x pred obj) "~" "refuted"))))
  (var answer "not provable")
  (if (empty? counter)
    # then a fresh witness of the class, one quiet run, one exact probe
    (let [w (zelph/resolve "ace:hyp-1")]
      (zelph/fact w "~" cls)
      (def run (zelph/run-delta :quiet))
      (when (and (empty? (run :contradictions)) (zelph/exists w pred obj :exact))
        (set answer "yes")))
    (set answer (string "no, counter-instance " (zelph/name (get (first counter) 'X)))))
  (zelph/cluster prev)
  (def dropped (zelph/cluster-drop "ace:q"))
  (string/format "%s; %d scratch nodes dropped; witness left: %q"
                 answer dropped (not (nil? (zelph/lookup "ace:hyp-1")))))
%
<function every?>
zelph> %(every? "man" "~" "mortal")
"yes; 12 scratch nodes dropped; witness left: false"
zelph> %(every? "man" "waits" "nil")
"not provable; 14 scratch nodes dropped; witness left: false"
zelph> %(every? "man" "sleeps" "nil")
"no, counter-instance bob; 8 scratch nodes dropped; witness left: false"
zelph> X ~ mortal
Answer: john ~ mortal
Answer: bob ~ mortal
```

## The Verbaliser

Every answer, every report line and every explanation is written by one component, the verbaliser in
the module `ace-verbalise`. It is the inverse of the mapping: where the mapping turns an ACE sentence
into zelph, the verbaliser turns a DRS or a zelph fact back into ACE that re-enters as the same thing.
It never prints zelph syntax to the user.

### Contract

| Aspect | Rule |
|---|---|
| Input | `(ace/verbalise drs &opt bindings)`: a resolved DRS (from `ace/analyse` or `ace/ape->drs`) or a normalized box; `bindings` a struct from referent to a rendered spelling (a string from [Rendering a Node](#rendering-a-node)), a data term, or `(:some C)` |
| Output | ACE text: sentences separated by one space, each ending in `.`; or `{:refused <construct id>}` for the first construct whose template is not available at `ace/milestone` |
| Fact input | `(ace/verbalise-node node)`: a fact node → its ACE sentence, through `zelph/parse` and the inverse of the atom mapping: instance predicate → `S is a C.`; `is` + adjective; a verb node; a relational noun or Wikidata property `S N O` → `O is the N of S.`; located and comparison phrases; a marking `((F) ~ refuted)` → `It is false that <F's sentence>`; the subclass predicate → `Every C1 is a C2.` (display only); names rendered as below; nil for anything else, internal vocabulary included. From M4 (contradiction and "not provably" reports); M9 uses it for derivations, justified answers and `ace-retract`'s listing |
| Effect | `ace/verbalise` reads the lexicon (`ace/inflect`, `ace/readings`), never the graph; `ace/verbalise-node` and `ace-zelph/render` read the graph; none of them writes |
| Guarantee | `ace/analyse` of the output is equivalent to the input, ignoring sentence ids (RT-2) |
| Confirmation | an accepted asserting sentence prints nothing; a true `:check` prints nothing |

### Templates

Variables `X`, `Y`, `Z`, `X1`, `X2`, … are assigned to unbound referents once per text (ACE variables
are paragraph-scoped, and one text is one paragraph). An unbound referent is introduced once, at its
declaring box, and referred to by its bare variable afterwards. Event referents are never introduced;
the property referents of copula pairs are folded into the copula. A lemma that is a lexicon symbol
(an id such as `Q8441`) is rendered by the first form declared for it, then inflected.

| DRS | Text |
|---|---|
| introduce R with `object(R, N, countable, na, eq, 1)`, adjectives, `relation(R, of, Y)` | `there is a [Adj and …] N V [of Y]` (an apposition before the of-phrase); in an if-part `there is a N V` likewise |
| introduce R: `something` / `somebody` / no object | `there is something V` / `there is somebody V` / `there is something V` |
| a bound R | its rendered spelling; `(:some C)` → `a C`, or `something` when C is nil |
| `(:named N)` | N, spelled by the name rule of [Rendering a Node](#rendering-a-node): in guessing mode a capitalised run of 2-5 words as it is (`Douglas Adams`), else backquoted when N is not a capitalised word token |
| data | `30`, `-7`, `"abc"` (with backslash escapes) |
| `predicate` with 3 / 4 slots | `S waits` / `S likes O` (`ace/inflect V :sg3`) |
| `predicate` with 5 slots | `S gives DO to IO` when the lexicon gives the verb's dependent preposition, else `S gives IO DO` |
| `be` + `object` on a fresh referent R | `S is a N` |
| `be` + `property`, degree `pos` / `pos` with target / `comp_than` / `pos_as` | `S is A` / `S is fond-of Y` / `S is taller than Y` (`more A than` without a synthetic comparative) / `S is as A as Y` |
| a 6-slot `property`, `subj` / `obj` | `S is more fond-of Y than Z` / `S is more fond-of Y than of Z`, the adjective's preposition repeated (`than in Z` for `interested-in`, corpus case 1564); `pos_as` with `as … as` (2178, 2179) |
| a normalized `object` / `property` on a name or bound referent | `John is a man`, `John is rich` |
| `be` with 3 slots + `modifier_pp` | `S is in Y` |
| `modifier_pp`, `modifier_adv` on an event | appended to its clause: `S waits in Y`, `S waits quickly` |
| a conjunction of conditions | clauses joined with `and`, introductions first |
| `(:imp A C)` | `If <A> then <C>.` |
| `(:neg D)` | `It is false that <D>.` (inside a then-part `then it is false that …`); with several clauses `that` is repeated: `It is false that A and that B.` |
| `(:naf D)` | `It is not provable that <D>.`, `that` repeated as for `:neg` |
| `(:or D1 D2)` | `<D1> or <D2>` (inside a coordination `, or`) |
| groups, counts (M7) | `John and Mary`, `2 cars`, `at most 1 car`, `some water` |
| modality, label (M8) | `S can V`, `It is possible that …`, `S believes that …` |
| a question, a command | `{:refused :question/yes-no}` and so on: answers use statements only |

The templates are written with the article `a`; before a vowel the answers write `an`, as the pinned
tutorial transcript requires ("an occupation"), and the grammar reads both articles alike, so the
round trip does not depend on the choice. Open for M1: the rule by which the verbaliser chooses `an` —
the first letter of the next word, or a lexicon flag for words such as "hour" and "unit".

**Two capitalised names are never side by side** ([DEC-WIKIDATA](decisions.md#dec-wikidata),
"Verbalising"). A 5-slot predicate whose direct and indirect object both render as names uses `to IO`
when the lexicon gives the preposition (`John gives Bill to Mary.`); otherwise the direct object is
backquoted (`` John gives Mary `Bill`. ``). The same rule backquotes the second of any two adjacent
names another template would produce. A capitalised run written as it is counts as one name.

### Rendering a Node

`(ace-zelph/render node role)` reads the graph and writes nothing. It tries, in this order
(DEC-WIKIDATA, "Verbalising"; how this looks on a Wikidata graph is on [How Answers Name a
Node](words.md#how-answers-name-a-node)):

1. **The `ace` name**, `(zelph/name node "ace" :strict)`, spelled by the rule of the role.
2. **A lexicon form**: when a lexicon entry of the role's word class has the node's `wikidata` name as
   its symbol, the first form declared for that symbol (the inverse lookup symbol → form of
   `ace/inflect`). After `noun_sg(man, 'Q8441', masc)` the item Q8441 renders as `man`, after
   `pn_sg('Berlin', 'Q821244', neutr)` Q821244 renders as `Berlin`, whatever their English labels are;
   both spellings re-enter as the same node.
3. **The `en` label**, only on a graph with current labels (the marker `wikidata:labels-v2`):
   `(zelph/name node "en" :strict)`, spelled by the rule of the role, and accepted only when resolving
   that spelling in the node's role — silently, without announcements and without touching the
   announcement cache — gives this node back. The check is role-aware: the plain `en` map prefers a
   property, so "country" is P17, while the noun role gives Q6256.
4. **The prefixed id** from the `wikidata` name: `p:Q123` (name), `n:Q123` (noun), `n:P50` (relation),
   `v:P155` (verb), `a:Q123` (adjective). Never a bare `Q123`, which ACE reads as a variable.
5. **No such name**: an unnamed node renders as `(:some C)`; a node named only in another language
   (typed natively under `.lang zelph`) renders by that name, spelled as step 1 spells an `ace` name,
   and its line is exempt from the 0-node round trip (it re-enters as an `ace` node of that name).

**Spelling rules.** A spelling that does not satisfy the rule of its role falls through to the next
step.

- **Name.** An individual name (`Man-6f1c2a9e0b3d4c5a`) as it is. A single word with a capital first
  letter, then letters and digits, as it is — unless it has the variable shape `[A-Z][0-9]*` or is a
  function word (`` `Q42` ``, `` `If` ``). In guessing mode, the default, two to five such words
  separated by single spaces, **each** capitalised and none a variable, an individual name or a word
  with a preposition reading, as it is (`Douglas Adams`, `New York City`): it re-enters through the
  multi-word fallback of the grammar ([DEC-MULTIWORD](decisions.md#dec-multiword); it needs the rule
  that a guessed double-object frame never splits a capitalised run,
  [DEC-UNKNOWN](decisions.md#dec-unknown)). In strict mode, which never merges words, or when only the
  first word is capitalised, two to five such words joined with hyphens (`Douglas-Adams`,
  `Leonardo-da-Vinci`), which gives the same canonical symbol. Anything else is backquoted, provided it
  holds no backquote and no line break: a name holding a hyphen (`` `Spider-Man` ``, which unquoted
  would be the canonical symbol "Spider Man"), punctuation or more words (`` `Washington, D.C.` ``,
  `` `The Hitchhiker's Guide to the Galaxy` ``).
- **Noun, relation.** Lower-case first; letters, digits, spaces and hyphens; at most 4 separators,
  spaces written as hyphens (`credit-card`). A capitalised label (`Homo sapiens`) is no ACE noun.
- **Verb, adjective.** A lower-case word with hyphens.

On an old network without the marker `wikidata:labels-v2`, step 3 is skipped, because its `en` names
sit on the wrong items; answers then name items by their prefixed ids ("Is p:Q42 a n:Q5?" → `Yes.
p:Q42 is a n:Q5.`, pinned by "ace words: a Wikidata graph without the label marker resolves no word by
its label", M1).

Native answers of zelph itself are the host's, not `ace`'s: under `.lang ace` zelph quotes a name that
holds a space (`Answer: "Douglas Adams" ~ man`), which is what makes the native line re-enter, and it
prints native answers in its own match order.

### Coverage per Milestone

| Milestone | Templates available |
|---|---|
| M1 | names, `object` on names (`John is a man.`), `Yes.` / `No. It is false that` / `It is not provable that`; node rendering |
| M3 | introductions with variables, verbs, adjectives, of-relations, the located copula, strings, bound individuals |
| M4 | if-then, negation, "not provably", disjunction; `ace/verbalise-node` for report lines; witnesses as indefinites |
| M5 | formulas and terms (`ace/math-text`); identity |
| M6 | modifiers on events |
| M7 | plurals, groups, counts, mass, lists, sets, how-many lines |
| M8 | modality, that-clauses |
| M9 | the derivation echo and justified answers; `ace/verbalise-node` for every fact shape of the contract |

## Why: Justified Answers

From milestone M9 an answer can say why. `(ace/option :justify true)` (default false) adds to every
`Answer:` line of a yes/no or wh question the sentences it rests on: the user's own sentences, quoted
with the block and sentence they came from, and for facts nobody typed in ACE their ACE rendering.
Like everything `ace` prints, these lines are ACE only ([DEC-ECHO](decisions.md#dec-echo) (b)).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :justify true)
    true
    zelph> ace No man waits. Bob is a man.
    Does Bob wait? Who is a man? Is Bill a man?

    Answer: No. It is false that Bob waits.
      "No man waits." (block 1, sentence 1)
      "Bob is a man." (block 1, sentence 2)
    Answer: Bob is a man.
      "Bob is a man." (block 1, sentence 2)
    Answer: It is not provable that Bill is a man.
    ```

On a Wikidata graph the facts come from the loaded network and carry no ACE source; they are rendered
by `ace/verbalise-node` and marked as such. A class answer that went through the subclass closure
lists the instance fact and the chain of subclass facts. The session continues the second tutorial
transcript above; it prints no note, because "Douglas Adams" was announced there already and "person"
has one carrier on the fixture, Q215627:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph-> %(ace/option :justify true)
    true
    zelph-> ace Is Douglas Adams a person?

    Answer: Yes. Douglas Adams is a person.
      "Douglas Adams is a human." (a fact of the loaded graph)
      "Every human is a person." (a fact of the loaded graph)
    ```

### Support Lines

One helper, `ace-zelph/support-lines`, makes the indented lines under an answer and under a derived
sentence:

1. **First the rules**, each by its sentences, in the order met, root first. A rule's sentences are
   found as for a [contradiction report](semantics.md#contradiction-reports): the session's rule
   registry, the `ace:src` annotations of its `=>` node; for a rule a generator wrote, the generator's
   sentences (`zelph/explain` of the generated rule names the generator); otherwise the line `a rule
   not written in ace`.
2. **Then the facts**, sorted by block and sentence, facts of other sessions and of the loaded graph
   last, duplicates merged:
    - a fact with provenance: its sources, `"<sentence>" (block B, sentence S)`, or
      `"<sentence>" (from a saved graph)` for a source of another session;
    - a fact without provenance: its `ace/verbalise-node` sentence, quoted and followed by
      `(derived above)` when the same echo showed it, `(derived earlier)` when `zelph/explain` gives
      its root the status `:derived`, else `(a fact of the loaded graph)`;
    - a fact `ace/verbalise-node` cannot express: `a fact of the loaded graph that ace cannot express`.
3. Negated conditions found absent, `!=` guards and helper atoms give no line: the rule's sentence
   states them ("not provably", `\=`).

### What Each Answer Rests On

The support lines come from the whole proof, `zelph/explain` (host item P-explain): the `:rule` of
every `:derived` step, the leaves of every other status. The proof is read before `ace:q` is dropped.

| Answer | Support lines |
|---|---|
| `Yes.` over ground items | each Atom item explained; a `(:refuted A)` item through its marking `((A) ~ refuted)`; a `(:naf A)` item gives no line (an absence rests on nothing) |
| `Yes.` over a pattern, and every wh line | the row's instantiated atoms; a class pattern expanded with a closure gives the instance atom `T <instance> V` and the subclass chain from V up to C (the bounded upward walk of the rule checks, the first shortest chain), each edge explained |
| `No. It is false that …` | the fact that decides it: for an Atom item its marking `((A) ~ refuted)` — an asserted refutation gives its sentence, a derived one its rule and premises ("No man waits." and "Bob is a man.") — and for a `(:refuted A)` item the fact A |
| `It is not provable that …`, `No answer.` | no lines |
| a universal question | `Yes.` by the then-box atoms over the hypothetical terms, whose assumed atoms are the question's own and give no line; `No.` by the counter row's atoms |
| how-many lines, math answers | none in v1: a count rests on every counted fact, and a math answer is decided by a scratch run that is dropped |

A proof cut short by `.explain-budget` (a `:truncated` node, or `:fallback true` at the root) adds the
line `… (the explanation was cut short)`. Each atom costs one `zelph/explain`, bounded by the
session's budget; the `… and N more answers.` line has no support lines. A justification is the one
proof `zelph/explain` returns; listing every justification of a fact is a planned engine feature
([Where the logic goes next](../about.md#where-the-logic-goes-next)).

## The Derivation Echo

`(ace/option :echo true)` (default false, M9) makes each block report, in ACE, what its inference
derived, each derived sentence with the sentences it rests on:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :echo true)
    true
    zelph> %(ace/option :justify true)
    true
    zelph> ace John is a man.

    zelph> ace Every man is a human. Every human is mortal.
    Is John mortal?

    Derived: John is a human.
      "Every man is a human." (block 2, sentence 1)
      "John is a man." (block 1, sentence 1)
    Derived: John is mortal.
      "Every human is mortal." (block 2, sentence 2)
      "John is a human." (derived above)
    Answer: Yes. John is mortal.
      "Every human is mortal." (block 2, sentence 2)
      "Every man is a human." (block 2, sentence 1)
      "John is a man." (block 1, sentence 1)
    ```

The first block derives nothing and prints nothing. In the second, the echo shows one step per derived
fact, a premise derived in the same run as `(derived above)`; the justified answer shows the whole
proof, rules root first, then the facts by block and sentence.

**How the echo is built.** With `:echo` on, `ace/infer!` runs `(zelph/run-delta :quiet :derived)`
instead of `(zelph/run-delta :quiet)` (host item P-derived), and its result carries `:derived D`
beside `:contradictions` and `:naf-final`: D in derivation order, one `{:fact :rule :premises :absent
:bindings}` per derived fact, the premises looked up and never created. Then:

- one `Derived: <S>` line per entry whose fact `ace/verbalise-node` expresses, with its support lines
  built from the entry's `:rule` and `:premises` — one step only, since a premise derived in the same
  run was shown above it;
- helper atoms are skipped silently; other facts the verbaliser cannot express are counted in one
  closing line `… and N derived facts ace cannot express.`;
- at most 5 entries per rule and block, then `… and N more like this` (DEC-WIKIDATA, "Output size");
- only the block's own inference is echoed, and that of `ace-retract`: never the dry run's (under
  `:atomic :consistent`) and never the run of a hypothetical check;
- a `run-delta` derives only what is new, so a block that derives nothing new prints no echo.

**Place in the output** ([Output](block.md#output)): the `word-senses` note first, then warnings,
show-zelph lines, derived contradictions, the echo, and the answers last. With both options off, no
derivation is ever shown: an asserting block prints nothing, and before M9 setting either option to
true raises a Janet error.

## Provenance: Where a Sentence Came From

Both features quote the user's sentences, and they can do so across `.save` and `.load` because every
built node carries its source as a **node annotation** (host item P-annotate, decision
[DEC-PROV](decisions.md#dec-prov) (c); the execution detail is on [Provenance and the Rule
Registry](semantics.md#provenance-and-the-rule-registry)). An annotation is a string list on a node
under a key; it adds no node, no edge and no name, queries and rules never see it, and it is not a
mention, so an annotated rule stays in force ([Node annotations](../janet.md#node-annotations)).

- **What is written.** From M1, in the real run only (never in the dry run), each built node gets the
  value `"<session>|<block>|<sid>|<text>"` under the key `ace:src`: the fact node of every `:fact`,
  the refuted fact of every `:refute`, the `=>` node of every `:rule`, `:denial` and `:helper-rule`; a
  helper rule's node gets the source of the rule it serves. Sources are appended per real run, oldest
  first, so a fact stated twice keeps both.
- **`<session>`** is the token `(ace/state :session)`, drawn once per Janet VM. A source of the
  current session prints as `(block B, sentence S)`; one of another session prints as `(from a saved
  graph)`, because block numbers restart in every session. `.save` and `.load` within one VM keep the
  numbers meaningful.
- **Removal erases.** Every removal of a node erases its annotations, so a fact asserted again later
  under the same node id does not inherit a stale quote.
- **Never a graph fact.** Provenance is never stored as a fact that names a rule node: such a fact
  would turn the rule off ([Mentioning a Rule Is Not Asserting
  It](../logic.md#mentioning-a-rule-is-not-asserting-it)).
- **Before M9** the annotations are already written, and the contradiction reports of M1-M8 read the
  rule's sentences from the session's rule registry; from M9 they read the annotations, which survive
  a load.

Tests: "ace provenance: recording provenance adds no node and never changes the rule count" (M1, for
a rule block from M4), "ace provenance: a block's sentences survive .save and .load as node
annotations" (M1; from M9 a justified answer after the load quotes the sentence as one from a saved
graph) and "ace provenance: a re-asserted fact does not inherit a stale quote" (M9).

Everything the two features read is in zelph today. `.explain` prints a proof as a tree, and
`zelph/explain` returns the same proof as data, or nil for a fact the graph does not hold ([Proofs as
Data](../janet.md#proofs-as-data-zelphexplain)):

```
zelph> (X ~ man) => (X ~ human)
zelph> (X ~ human) => (X ~ mortal)
zelph> john ~ man
(john ~ human) ⇐ (john ~ man)
(john ~ mortal) ⇐ (john ~ human)
zelph> .explain john ~ mortal
john ~ mortal
   └─ john ~ human
      └─ john ~ man  [axiom]
zelph> %
(defn show [step depth]
  (zelph/out (string (string/repeat "  " depth) (step :fact) "  " (step :status)
                     (if (step :rule) (string "  by " (step :rule)) "")))
  (each p (step :premises) (show p (+ depth 1))))
%
<function show>
zelph> %(show (zelph/explain "john" "~" "mortal") 0)
john ~ mortal  derived  by (X ~ human) => (X ~ mortal)
  john ~ human  derived  by (X ~ man) => (X ~ human)
    john ~ man  axiom
zelph> %(nil? (zelph/explain "bill" "~" "mortal"))
true
```

The next run writes provenance as `ace` will, one `ace:src` annotation per node, then builds the echo
from a quiet run's `:derived` data and the support lines of a justified answer from the proof.
Auto-run is switched off first, so that the facts are derived by the quiet run rather than when they
are typed. Only the ACE rendering of the derived facts is missing — the run prints `john ~ human`
where `ace` prints "John is a human." — and that is `ace/verbalise-node`'s part:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> john ~ man
zelph-> (X ~ man) => (X ~ human)
zelph-> (X ~ human) => (X ~ mortal)
zelph-> %
# provenance as ace records it: one annotation "<session>|<block>|<sentence>|<text>" per node
(zelph/annotate (zelph/fact "john" "~" "man") "ace:src" "s1|1|1|John is a man.")
(def texts {"(X ~ man) => (X ~ human)" "s1|2|1|Every man is a human."
            "(X ~ human) => (X ~ mortal)" "s1|2|2|Every human is mortal."})
(each r (zelph/rules) (zelph/annotate (r :rule) "ace:src" (texts (string (r :rule)))))
(defn quote-src [n]
  (when-let [v (first (zelph/annotations n "ace:src"))]
    (def [session block sid text] (string/split "|" v 0 4))
    (string/format "%q (block %s, sentence %s)" text block sid)))
%
<function quote-src>
zelph-> %
# the echo: what a quiet run derived, each fact with its rule and premises
(def above @{})
(each d ((zelph/run-delta :quiet :derived) :derived)
  (zelph/out (string "Derived: " (d :fact)))
  (zelph/out (string "  " (quote-src (d :rule))))
  (each p (d :premises)
    (zelph/out (string "  " (or (quote-src p) (string p (if (above p) " (derived above)" ""))))))
  (put above (d :fact) true))
%
Derived: john ~ human
  "Every man is a human." (block 2, sentence 1)
  "John is a man." (block 1, sentence 1)
Derived: john ~ mortal
  "Every human is mortal." (block 2, sentence 2)
  john ~ human (derived above)
zelph-> %
# a justified answer: the rules of the proof, root first, then its leaves
(defn rules-of [p] (if (p :rule) [(p :rule) ;(mapcat rules-of (p :premises))] []))
(defn leaves-of [p] (if (= (p :status) :derived) (mapcat leaves-of (p :premises)) [(p :fact)]))
(def proof (zelph/explain "john" "~" "mortal"))
(each n [;(rules-of proof) ;(leaves-of proof)] (zelph/out (string "  " (quote-src n))))
%
  "Every human is mortal." (block 2, sentence 2)
  "Every man is a human." (block 2, sentence 1)
  "John is a man." (block 1, sentence 1)
```

## Taking a Statement Back: `ace-retract`

zelph keeps no record of what a fact was derived from. Removing a fact therefore leaves standing what
was derived from it: after `(X ~ man) => (X ~ mortal)`, pruning `bob ~ man` leaves `bob ~ mortal`
([Data Cleanup Commands](../quickstart.md#data-cleanup-commands)). An ACE user must not end up with
such a graph silently, so `ace` offers retraction only where it is safe: the block keyword
`ace-retract` (M9, [DEC-RETRACT](decisions.md#dec-retract) (c)) takes back fact statements, and refuses
— removing nothing — while a derived fact rests on one of them or another statement holds it.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Every man is a human. John is a man. Mary is a woman.

    zelph> ace-retract Mary is a woman.

    zelph> ace Is Mary a woman? Is John a human?

    Answer: It is not provable that Mary is a woman.
    Answer: Yes. John is a human.
    zelph> ace-retract John is a man.

    Error in line "ace-retract John is a man.": Correct ACE, but zelph cannot use it [ace/retract-dependents]: sentence 1, line 1, column 1: derived facts rest on this sentence, and zelph keeps no record of why a fact holds; nothing was taken back.
      <>John is a man.
      "John is a man." supports facts that would stay:
        John is a human.
    ```

A statement with a modifier is held by its occurrence event: "John waits in a park." can be taken
back whole, its occurrence first, while "John waits." alone is refused as long as the occurrence holds
it, since removing the plain fact would leave the park dangling ([The ace
Block](block.md#taking-a-statement-back-ace-retract) shows that session). The procedure in short; [The
Steps](block.md#the-steps) and [What the Mapping Contributes](block.md#what-the-mapping-contributes)
hold it in full:

- The text is analysed, planned and resolved as an `ace` block's, so a sentence names the same atoms
  under both keywords. A sentence that plans to anything but fact actions (a rule, a negation, a
  question, a formula) is refused with `[ace/unsupported]`.
- Each atom is looked up with `zelph/exists … :exact`, never created; an atom the graph does not hold
  is left alone silently. Nothing is written before the removal — no pin, no mark, no dry run.
- `(zelph/dependents f)` lists the derived facts that rest on f, bounded by `.explain-budget` (a list
  cut short refuses as well, and the message says it may be incomplete); `(zelph/holders f)` lists the
  statements that nest f. A holder the same block takes back does not count. Any remaining answer
  refuses the block with `[ace/retract-dependents]`, which lists the dependents as ACE sentences (at
  most 5, then `… and N more like this`) and the holders by the sentences that wrote them.
- Otherwise `(zelph/prune-fact f)` withdraws each atom, holders first. It answers `:removed` (the node
  is gone, its annotations with it), `:unclaimed` (the node stays as a rule's pattern or a mention, and
  `ace` removes its own provenance with `(zelph/unannotate f "ace:src")`) or nil (there was no claim to
  withdraw). Every key of `(ace/state :reported)` that holds a removed or unclaimed fact node is
  dropped, so that a contradiction rebuilt later is reported again.
- The inference then runs as for an `ace` block, a removal counting as a write. A fact that a rule
  also derives comes back there: `zelph/dependents` lists what rests on f, not what f could be derived
  from. With `:echo` on, the echo shows it coming back.

Before M9 the user page `ace.md` gives the native route: the block's show-zelph line, typed under
`.lang ace`, is the `.prune-facts` argument, and derived facts stay. A statement that a firing's
conclusion nests is not among `zelph/dependents`' answers (item [L3-P1](prerequisites.md#l3-p1-a-statement-nested-in-a-conclusion-is-explained-as-an-input)
of What zelph Still Needs); `ace`'s mapping writes every nested atom also at the top level, so its own
statements are not affected. `zelph/prune-fact` reads the rule parts on every call (O(rule text) per
atom), which M9 measures for blocks of many atoms.

Open for M9: a sentence whose atom holds a conditional ("John believes that if a man waits then the
man sleeps.", M8) cannot be named in the `s p o` form of `zelph/prune-fact`, because each such
statement builds its own condition set; `ace` either passes the node from its provenance or refuses
the sentence by id.

The three bindings `ace-retract` rests on, in a real run: `zelph/dependents` finds the derived fact
that rests on `john ~ man`, `zelph/holders` finds the occurrence that holds `bill waits nil`,
`zelph/prune-fact` removes a fact nothing rests on and answers nil for one the graph does not hold,
and `.prune-facts` shows why `ace` asks first — the derived `john ~ human` stays after its premise is
gone:

```
zelph> (X ~ man) => (X ~ human)
zelph> john ~ man
(john ~ human) ⇐ (john ~ man)
zelph> mary ~ woman
zelph> ev1 occurrence (bill waits nil)
zelph> ev1 "is in" park1
zelph> %(zelph/dependents "john" "~" "man")
(<zelph/node john ~ human>)
zelph> %(zelph/dependents "mary" "~" "woman")
()
zelph> %(zelph/holders "mary" "~" "woman")
()
zelph> %(zelph/holders "bill" "waits" "nil")
(<zelph/node ev1 occurrence (bill waits nil)>)
zelph> %(zelph/prune-fact "mary" "~" "woman")
:removed
zelph> %(zelph/exists "mary" "~" "woman" :exact)
false
zelph> %(nil? (zelph/prune-fact "tom" "~" "man"))
true
zelph> .prune-facts john ~ man
Pruned 1 matching facts.
Consider running .cleanup.
zelph> X ~ human
Answer: john ~ human
```

## API and Options

The functions of this page; their signatures and effect classes are collected on [Modules, API and
Data Shapes](api.md#ace-verbalise).

| Function | Module | Returns | Effect |
|---|---|---|---|
| `(ace/ask action xctx)` | `ace-zelph` | a tuple of answer lines for one ask action | reads (its own scratch cluster `ace:q`) |
| `(ace/verbalise drs &opt bindings)` | `ace-verbalise` | ACE text, or `{:refused id}` | reads (lexicon only) |
| `(ace/verbalise-node node)` | `ace-verbalise` | the ACE sentence of a fact node, or nil | reads |
| `ace-verbalise/line` | `ace-verbalise` | one answer line from a question box, its bindings and its value (private) | reads |
| `(ace-zelph/render node role)` | `ace-zelph` | a spelling of a node in a role (private) | reads |
| `ace-zelph/support-lines` | `ace-zelph` | the indented lines under an answer or a derived sentence (private, M9) | reads |
| `(ace/infer!)` | `ace-zelph` | `{:contradictions C :naf-final N}`, plus `:derived D` while `:echo` is true | writes |
| `(ace/retract-handler text)` | `ace` | `:incomplete` or nil; the handler of `ace-retract` (M9) | writes (removes facts) |
| `ace-zelph/retract!` | `ace-zelph` | the steps of [Taking a Statement Back](#taking-a-statement-back-ace-retract) (private, M9) | writes (removes facts) |

| Option | Default | Effect | From |
|---|---|---|---|
| `:max-answers` | 100 | lines per question, then `… and N more answers.` | M1 (wh-questions from M3) |
| `:justify` | false | support lines under every yes/no and wh answer | M9 (a Janet error before) |
| `:echo` | false | the derivation echo of each block | M9 (a Janet error before) |
| `:show-zelph` | false | the native lines of a block's write actions; questions print none | M1 |

Messages this page uses: `[ace/unsupported]` for a refused question construct and for a sentence of an
`ace-retract` block that is no fact statement; `[ace/retract-dependents]` for a retraction that
something rests on. The catalogue is [The Message Catalogue](block.md#the-message-catalogue).

## Construct Ids of Questions

The question rows of the construct table, with own example sentences in the shape of the corpus cases
named; the full table is on [Constructs and Refusals](constructs.md#the-construct-table).

| Id | Example | Milestone | Mapping |
|---|---|---|---|
| `:question/yes-no` | "Is John a man?" | M1 | `:ask-yn` |
| `:question/wh` | "Who owns a dog?", "Which visitor sleeps?" (249, 2703) | M3 | `:ask-wh` |
| `:question/where-when-how` | "Where does John wait?" (21, 2200) | M6 | event modifiers queried |
| `:question/how-many` | "How many dogs does John own?" (3741) | M7 | counting pattern |
| `:question/unrestricted` | "What is there?" (1679) | — | no route: a question word constrained by no condition |
| `:neg/atom-in-question` | "Who does not sleep?" (1448) | M4 | `(:refuted Atom)` |
| `:neg/complex-in-question` | "Is there no dog?" (763) | — | refused, hint "not provably" |
| `:naf/in-question` | "Which visitor does not provably sleep?" (2772) | M4 | `(:naf Atom)`, `(:naf (:class …))` |
| `:or/in-question` | a disjunction in a question (2621) | M4 | one action per disjunct |
| `:rule/in-question` | "Is it true that every dog is an animal?" (1974) | M4 | guarded hypothetical check |
| `:rule/in-wh-question` | "How does every dog bark?" (2087) | — | no route: the check answers yes/no only |
| `:rule/unsafe-hypothesis` | a hypothesis that reaches a NAF or `!=` rule | — | no route (context id, resolution step) |

## Tests

The per-construct question tests, by the names the specification gives them. The other tests of this
page's features are on [Tests and Quality Gates](tests.md): the cross-cutting question tests under
[Questions](tests.md#questions), the block's question behaviour and show-zelph under [Keyword, Block
and Execution](tests.md#keyword-block-and-execution), interop and provenance under [Re-Entry, Interop,
Persistence, Provenance](tests.md#re-entry-interop-persistence-provenance), class questions,
occupations, ranks and spellings under [Words and Wikidata](tests.md#words-and-wikidata), names in
answers under [Names](tests.md#names), `:max-answers`, `:justify` and `:echo` under [Options and Output
Size](tests.md#options-and-output-size), the two tutorial transcripts under [Tutorial
Pins](tests.md#tutorial-pins), and `ace-retract` under [Retraction](tests.md#retraction).

| Test | Pins | From |
|---|---|---|
| ace questions: who, what and which answer with sorted sentences | wh answers in sorted order (249) | M3 |
| ace questions: an answer naming an individual re-enters without a warning | RT-1 on individual names | M3 |
| ace negation: universal negation answers No | "No man waits." → "Does John wait?" → `No.` | M4 |
| ace negation: not provably is negation as failure | 2772 | M4 |
| ace rules: a question holding a conditional is checked hypothetically | 1974 | M4 |
| ace rules: a hypothesis that could reach a NAF rule is refused | `:rule/unsafe-hypothesis`, nothing written | M4 |
| ace events: where, when and how ask the event | 21, 113, 2200 | M6 |
| ace modality: a possibility is mentioned, not asserted | "Does John fly?" stays not provable (1454) | M8 |
| ace interop: a native or Wikidata fact verbalises as ACE | `ace/verbalise-node` on facts without provenance | M9 |
| ace provenance: a re-asserted fact does not inherit a stale quote | after a removal, the fact asserted again quotes only its new sentence | M9 |

## What This Builds On in zelph

Nothing on this page needs a new reasoning feature; each part is a use of an engine feature that is
built, documented, tested and part of zelph 1.1.0 (see [What zelph Already Provides](foundation.md)):

- **Scratch clusters** and their drop: a question is answered in a cluster that is dropped with the
  answer ([Node Clusters](../rules.md#node-clusters-transactional-workspaces), [A question that leaves
  nothing behind](../janet.md#a-question-that-leaves-nothing-behind)).
- **Exact existence probes** that create nothing, and the plain probe that reads a fact with further
  objects ([Existence Check](../janet.md#existence-check-zelphexists); [Facts with Multiple
  Objects](../logic.md#facts-with-multiple-objects)).
- **Path conditions** for the subclass closure of class questions ([Transitive Path
  Conditions](../logic.md#transitive-path-conditions)).
- **Refutation and negation as failure** for `No.` and "not provably" ([Contradiction
  Detection](../logic.md#contradiction-detection), [Negation as
  Failure](../logic.md#negation-as-failure)).
- **Quiet runs** that return contradictions and derived facts as data ([Running
  quietly](../janet.md#running-quietly)) and the rule list `zelph/rules` for the hypothesis guard
  ([Janet API Reference](../janet.md#janet-api-reference-zelph)).
- **Proofs as data**, `zelph/explain` ([Proofs as Data](../janet.md#proofs-as-data-zelphexplain)).
- **Node annotations** for provenance ([Node annotations](../janet.md#node-annotations)).
- **`zelph/dependents`, `zelph/holders`, `zelph/prune-fact`** for retraction ([Janet API
  Reference](../janet.md#janet-api-reference-zelph), [Data Cleanup
  Commands](../quickstart.md#data-cleanup-commands)).

The engine side of each item, with its tests, is on [What zelph Already
Provides](foundation.md#proofs-provenance-and-retraction-as-data). What `ace` adds is the mapping in
both directions: from ACE questions to probes and queries, and from proofs, derived facts and
refusals back to ACE sentences. The order in which these parts are built is on [Work Plan and
Milestones](plan.md); the decisions behind them (DEC-QTIME, DEC-QUNIV, DEC-ECHO, DEC-PROV,
DEC-RETRACT) on [Design Decisions](decisions.md); why the verbaliser is designed as the mapping's
inverse on [Design Routes Taken and Not
Taken](routes.md#a5-the-verbaliser-is-the-mappings-inverse).
