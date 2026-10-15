# A Tour in Sessions

This page walks through the Attempto Controlled English (ACE) front end as a user meets it: a single brief session for each feature, presented in the sequence dictated by the milestones. Each session shows what the user types after `.import ace` and what zelph answers. Beneath it stands the native zelph that the block writes, and beneath that, wherever a short one exists, a recording of today's zelph running that native zelph. The trio collectively conveys the central point of this specification: **zelph already performs the reasoning.** Rules, negation as failure, contradiction detection, statements concerning statements, arithmetic, and proofs are all documented engine features, released with zelph 1.1.0, together with the engine items that were developed beforehand for the front end. The funded effort consists of the mapping between ACE and zelph, written as stdlib `.zph` modules (Janet combined with zelph) built upon these features, mirroring how `stdlib/sparql.zph` layers SPARQL over the public Janet API without requiring the C++ core to be aware of SPARQL.

A reviewer examining solely the sessions should leave with a clear understanding of the front end's functionality. Each implementing session should find here the acceptance picture for every milestone: the answers a block gives, the native form `ace/zelph-of` renders, and the construct ids that are either supported or refused. This page serves as a tour, not the definitive reference. The complete mapping table appears at [From Sentences to the Graph](semantics.md#every-construct-and-its-native-form), the answer formats at [Questions, Answers and Explanations](answers.md), the block contract and message catalogue at [The ace Block](block.md), the construct table at [Constructs and Refusals](constructs.md), and the milestones along with their gates at [Work Plan and Milestones](plan.md).

## How to Read the Sessions

On this page, two types of transcripts are present, and they are never mixed:

- **Specified sessions** are enclosed within a box labelled "Specified session — the behaviour this specification requires, not yet implemented". No front-end component has been built so far; these sessions represent the requirement, extracted directly from the specification's test rows and illustrative cases. When a line needed completion, it is derived from the specification's rules (answer templates, message layout, answer order).
- **Real runs** are recordings of today's zelph binary, captured from the current tree. They show the native zelph that a specified session writes, thereby showing that the reasoning underpinning each feature is functional at present. During a real run, names are typed in zelph's default language, ensuring that `ace`'s lexical set (`refuted`, `ace:occurrence`, `ace:naf-…`) and its content words meet in one language; the front end maintains its own content using the naming language `ace`, while its internal vocabulary remains in `zelph` (refer to [Words and Wikidata](words.md)).

The transcripts follow the format defined in the [quick start](../quickstart.md): within a block, continuation lines appear without a prompt; the blank line concluding an `ace` block is shown as an empty line; lines such as `Importing file …`, module banners, the `[ace/notice]` banner emitted once by `.import ace`, timing lines, and the echo of typed lines are omitted. Each specified session starts in a fresh zelph, hence blocks are numbered starting from 1.

Three further conventions:

- The individual introduced by an indefinite noun phrase is named using the head noun and the 16 hexadecimal digits derived from a hash of its description (`Man-6f1c2a9e0b3d4c5a`). The digits shown in these sessions are illustrative; the actual values are determined exclusively by the description, ensuring that identical descriptions consistently name the same individual.
- The block's handler triggers an error, which is then displayed by zelph's host in the format `Error in line "ace <first line>": <message>`, appending ` …` if the block includes additional lines. Input processed through a pipe terminates at the first error, meaning a session exhibiting an error ends with that error. Every message's wording is illustrative; however, its layout (class, id, position, the sentence containing the marker `<>`, and the hint lines) and its id are normative. A position names the token serving as evidence of the construct, as [Positions and the Marker](block.md#positions-and-the-marker) defines it; the catalogue of ids is provided in [The Message Catalogue](block.md#the-message-catalogue).
- Construct ids such as `:rule/horn` name the language constructs; each section lists the ones it covers. The full reference table, including examples and reasons, is available in [Constructs and Refusals](constructs.md).

**"Show me the zelph."** The option `(ace/option :show-zelph true)` makes a block print the native zelph lines it writes, via the pure function `ace/zelph-of`. The first session employs this; subsequent sections present the identical listing under each session, marked as the zelph the block writes, with the questions incorporated as `ace/zelph-of` renders them (a yes/no question as a comment `# yes/no: …`, a wh-question as its query line). Lines containing solely content words, core names, and rule variables re-enter natively under `.lang ace` and add no node; lines containing `ace`'s internal vocabulary (`refuted`, `opaque`, names beginning with `ace:`) or the maths stack are intended for reading, since those names reside within the language `zelph`.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :show-zelph true)
    true
    zelph> ace John is a man.

    John ~ man
    zelph> .lang ace
    ace> X ~ man
    Answer: John ~ man
    ```

The block printed the single native line it wrote, and a native query under `.lang ace` finds the associated fact. The block wrote an additional fact that the listing omits: in a graph lacking Wikidata, the first class statement pins the profile with the internal fact `ace:profile ace:uses ace:core`, ensuring that "is a" remains tied to core `~` within this graph permanently (on a Wikidata graph, it is `P31`; refer to [Words That Reach Wikidata](#words-that-reach-wikidata)). Such bookkeeping is never included in the rendered lines.

**A quick overview of the tour.**

| Section | Milestone | Construct ids it shows |
|---|---|---|
| [First Block and Words](#first-block-and-words) | M1 | `:name/proper`, `:object/count`, `:copula/noun`, `:question/yes-no` |
| [Facts and Questions](#facts-and-questions) | M3 | `:individual/top-level`, `:object/something`, `:verb/intransitive`, `:verb/transitive`, `:verb/ditransitive`, `:adj/positive`, `:adj/transitive`, `:adj/comparison`, `:of/relation`, `:copula/located`, `:question/wh`, `:data/string` |
| [Rules and Negation](#rules-and-negation) | M4 | `:rule/horn`, `:rule/generative-head`, `:rule/disconnected-head`, `:rule/nested`, `:rule/negated`, `:rule/in-question`, `:neg/fact`, `:neg/statement`, `:neg/not-every`, `:neg/in-head`, `:neg/atom-in-body`, `:neg/atom-in-question`, `:naf/atom-in-body`, `:naf/complex-in-body`, `:naf/in-question`, `:or/in-body`, `:or/in-question` |
| [Numbers and Formulas](#numbers-and-formulas) | M5 | `:formula/compare`, `:formula/identity`, `:expr/arith`, `:data/integer`, `:copula/identity`, `:neg/formula`, `:naf/formula` |
| [Events and Modifiers](#events-and-modifiers) | M6 | `:mod/pp`, `:mod/adverb`, `:question/where-when-how` |
| [Plurals, Counts and Groups](#plurals-counts-and-groups) | M7 | `:object/plural-count`, `:object/at-most`, `:object/at-least`, `:object/group`, `:group/condition-list`, `:object/mass`, `:data/list`, `:data/set`, `:question/how-many` |
| [Possibility and Belief](#possibility-and-belief) | M8 | `:modal/can`, `:modal/must`, `:modal/should`, `:modal/may`, `:sub/that-clause` |
| [Why, and Taking a Statement Back](#why-and-taking-a-statement-back) | M9 | no new id: the options `:echo` and `:justify`, the keyword `ace-retract` |
| [Words That Reach Wikidata](#words-that-reach-wikidata) | M1, M3, M4 | no id: word resolution and the instance and subclass predicates |
| [Refusals](#refusals) | every milestone | the 26 ids that v1 refuses |

Milestones M0 (foundations: the construct table and the DRS oracle) and M2 (the complete grammar of ACE 6.7, strict and guessing) add no mapping; they appear here solely through what they make possible. Out of the 83 construct ids that determine if a text is accepted (the DRS-level and context ids listed in the [construct table](constructs.md#the-construct-table): 76 DRS-level and 7 context ids), 57 are supported by the end of M9 and 26 are refused with a reason.

## First Block and Words

**Milestone M1.** The first milestone brings the whole block contract across four constructs: a proper name (`:name/proper`), a singular count noun (`:object/count`), the copula with a noun (`:copula/noun`, "John is a man."), and the yes/no question (`:question/yes-no`). A block encompasses all content from the keyword `ace` up to the subsequent empty line. It undergoes complete analysis before anything is written; it is then written once within a scratch cluster that is discarded afterwards (the dry run, which lets the engine itself refuse a contradicting block), followed by a single real write; afterwards, zelph executes a single quiet inference if the block asks a question or if it wrote something while auto-run is on; finally, the questions are answered in the order sentences were written. Each block represents a single theory, meaning a question also sees sentences appearing after it.

### A Block, a Question, Three Answers

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John is a man.

    zelph> ace Is John a man? Is Mary a man?

    Answer: Yes. John is a man.
    Answer: It is not provable that Mary is a man.
    zelph> ace Is Rex a dog? Rex is a dog.

    Answer: Yes. Rex is a dog.
    zelph> .lang ace
    ace> ¬(Bill ~ man)
    ace> ace Is Bill a man?

    Answer: No. It is false that Bill is a man.
    ```

    The zelph that these blocks write (`ace/zelph-of`):

    ```
    John ~ man
    # yes/no: John ~ man
    # yes/no: Mary ~ man
    Rex ~ dog
    # yes/no: Rex ~ dog
    # yes/no: Bill ~ man
    ```

An asserting block prints nothing: neither a confirmation nor a deduction line. Answers are three-valued: `Yes.` if the fact holds, `No. It is false that …` if the graph refutes it, and `It is not provable that …` in all other cases. The `No.` here comes from a refutation expressed natively; starting with M4, "Bill is not a man." writes the identical refutation in ACE. Answers maintain round-trip integrity: a `Yes.` or `No.` line, after removing `Answer: ` and the leading `Yes. ` or `No. `, becomes valid ACE that re-enters as a block and adds no new node (for `No.` lines from M4 onward, where ACE can state a refutation), and a not-provable line re-entered as the original question yields the same line. The answers to a universal question represent the sole exception ([below](#universal-questions-and-disjunction)); the validation of this round-trip is performed by the test [RT-1](tests.md#rt-1-answers-re-enter).

A yes/no question never writes. Every question executes within its own isolated scratch cluster `ace:q`, which is dropped afterwards. A ground atom is verified via `(zelph/exists s p o :exact)`, an operation that takes constant time and generates no new data. Upon failure, while the graph might contain facts with several objects, the plain `(zelph/exists s p o)` determines the outcome: it reads a broader fact in the same manner as a rule condition, so following the native fact `John sees Mary Bill`, the question "Does John see Mary?" yields a positive response (decision [DEC-YESNO-WIDER](decisions.md#dec-yesno-wider); starting from M3, the test `ace questions: a native fact with several objects answers a single-member yes/no question` pins it). This second probe is the sole instance where `ace` calls `zelph/exists` without `:exact`; it reads only the subject's facts under that predicate and never scans the entire graph. A word absent from the graph ("Mary" in the example, on a blank graph) terminates evaluation as not provable, without being created. When the graph includes a subclass predicate, the class question transforms into a pattern that follows the subclass hierarchy (shown under [Words That Reach Wikidata](#words-that-reach-wikidata)). The step-by-step evaluation process is detailed in [Yes/No Questions](answers.md#yesno-questions).

On today's zelph, the identical trio of values exists natively: a fact, a refutation, a probe that creates nothing, alongside the two probes over a fact with two objects.

```
zelph> John ~ man
zelph> ¬(Bill ~ man)
zelph> X ~ man
Answer: John ~ man
zelph> (X ~ man) ~ refuted
Answer: (Bill ~ man) ~ refuted
zelph> %(zelph/out (string/format "John: %q  Bill: %q  Sue: %q" (zelph/exists "John" "~" "man" :exact) (zelph/exists "Bill" "~" "man" :exact) (zelph/exists "Sue" "~" "man" :exact)))
John: true  Bill: false  Sue: false
zelph> John sees Mary Bill
zelph> %(zelph/out (string/format "exact: %q  plain: %q" (zelph/exists "John" "sees" "Mary" :exact) (zelph/exists "John" "sees" "Mary")))
exact: false  plain: true
```

`zelph/exists … :exact` answers true exclusively for the claimed fact. The refutation `¬(Bill ~ man)` is stored as the marking `(Bill ~ man) ~ refuted`, which a query retrieves; this is how the front end tells `No.` from `It is not provable`. For the fact `John sees Mary Bill`, the exact probe fails to locate the fact, while the plain probe succeeds in finding it. Language interoperability functions in both directions: a native fact under `.lang ace` answers an ACE question, and an ACE fact answers a native query under `.lang ace`. Within the default `.lang zelph`, a typed `man` represents a distinct node, since `ace` names its content using the language `ace`; the user's `.lang` setting never changes what a block resolves.

### Words the Lexicon Does Not Know

`ace-core` ships the function words, the irregular forms, roughly 300 person nouns along with their gender, and the common ACE example names (John, Mary, Bill, Sue, …), yet excludes general vocabulary. A word lacking a lexicon entry is interpreted based on its placement within the sentence (guessing, the default behaviour). A guessed word that is one typing edit away from a recognized word is rejected with a recommendation, ensuring that a typo never writes a node; the prefix `n:`, `v:`, `a:`, or `p:` enforces the word exactly as written ([Unknown words, words ACE lacks and near misses](grammar.md#unknown-words-words-ace-lacks-and-near-misses)).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John is a blicket. Is John a blicket?

    Answer: Yes. John is a blicket.
    zelph> ace John is a mna.

    Error in line "ace John is a mna.": Lexicon error [ace/unknown-word]: sentence 1, line 1, column 11: "mna" is not a known word. Did you mean "man"?
      John is a <>mna.
      If "mna" is a new word, write n:mna.
    ```

    In strict mode, APE's behaviour without guessing, each unknown word is reported instead:

    ```
    zelph> .import ace
    zelph> %(ace/option :guess false)
    false
    zelph> ace John is a blicket.

    Error in line "ace John is a blicket.": Lexicon error [ace/unknown-word]: sentence 1, line 1, column 11: "blicket" is not a known word.
      John is a <>blicket.
      If "blicket" is a noun, write n:blicket or declare it: ace-lexicon noun_sg(blicket, blicket, neutr).
    ```

    A sentence that is not ACE is reported by indicating the most distant position the parser reached:

    ```
    zelph> .import ace
    zelph> ace John like Mary.

    Error in line "ace John like Mary.": Not ACE [ace/syntax]: sentence 1, line 1, column 6: found 'like', expected 'is', 'does' or a verb in the third person singular.
      John <>like Mary.
      Did you mean 'likes'?
    ```

Columns count the user's characters; on the keyword line, column 1 refers to the initial character following `ace`. A rejected block leaves the graph in its original state. A user lexicon is defined within `ace-lexicon` blocks using the ulex format from the public Attempto lexicon specification (`noun_sg(blicket, blicket, neutr).`); an entry where the second argument is a Wikidata identifier (`noun_sg(man, 'Q8441', masc).`) binds the word with that item. Guessing, strict mode, lemma recovery, multi-word names, and the lexicon are detailed in [Tokenizer, Grammar and Lexicon](grammar.md).

Between milestones, a sentence whose construct arrives later is refused by its identifier, alongside the milestone that brings it: at M1, "There is a man." is refused naming `individual/top-level`, which is planned for milestone M3. Starting from M2 onward, the grammar parses all of ACE 6.7, meaning that from that point forward, any sentence accepted by APE is either supported or refused by identifier, never resulting in a syntax error, except for a small number of [documented deviations from APE](grammar.md#deliberate-deviations-from-ape).

## Facts and Questions

**Milestone M3.** Facts of every basic shape and wh-questions. The shapes adhere to a single rule (decision [DEC-SHAPES](decisions.md#dec-shapes)): a verb functions as a predicate node, an intransitive verb assigns the object `nil`, an adjective serves as an object linked to the predicate `is`, and a phrase containing a preposition or a comparison constitutes a single predicate node composed of several words. An indefinite noun phrase at the top level introduces an individual with a content address (decision [DEC-INDIVIDUALS](decisions.md#dec-individuals)), meaning that "There is a man." typed twice, whether within a single block or across two, refers to one man, and re-adding an entire block contributes no new content. This departs from APE's interpretation, where each indefinite noun phrase creates a fresh referent, a point explicitly stated in the user documentation.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace There is a man who sings. John likes Mary. John is rich.
    John is fond-of Mary. John is taller than Bill. John gives Mary a book.
    Bill is a father of John. John is in a park. John says "hello".

    zelph> ace Is there a man who sings? Who likes Mary? Who is taller than Bill?
    Who is a father of John? Who is a father of Mary? Who is in a park?
    What does John say? Who is John?

    Answer: Yes. Man-6f1c2a9e0b3d4c5a sings.
    Answer: John likes Mary.
    Answer: John is taller than Bill.
    Answer: Bill is a father of John.
    No answer.
    Answer: John is in Park-2b7e9d4c1a3f5e6b.
    Answer: John says "hello".
    Answer: John is rich.
    ```

    The zelph the first block writes (`ace/zelph-of`):

    ```
    Man-6f1c2a9e0b3d4c5a ~ man
    Man-6f1c2a9e0b3d4c5a sings nil
    John likes Mary
    John is rich
    John "is fond of" Mary
    John "is taller than" Bill
    John gives <Book-0c4d2e7f9a1b3c5d Mary>
    Book-0c4d2e7f9a1b3c5d ~ book
    John father Bill
    Bill ~ father
    John "is in" Park-2b7e9d4c1a3f5e6b
    Park-2b7e9d4c1a3f5e6b ~ park
    John says <o l l e h>
    ```

The second block's questions each consist of a single `zelph/query` within the scratch cluster `ace:q`: `X ~ man, X sings nil` (asked as a yes/no question, thus only the first row suffices), `X likes Mary`, `X "is taller than" Bill`, `John father X, X ~ father`, `Mary father X, X ~ father` (no row provided, hence `No answer.`), `X "is in" Y, Y ~ park`, `John says X`, and for "Who is John?" the two patterns `John ~ X` and `John is X`, which outline John through his classes and adjectives. Answers are sorted by their textual content, capped at 100 per question (via the `:max-answers` option), followed by `… and N more answers.`; a blank wh-answer appears as `No answer.`. An answer that names an individual (`Man-6f1c…`) re-enters without a warning, as the annotator reads a capitalized word of that shape as a recognized proper name. The mechanism by which each question transforms into a query and each row into an answer line is specified on [Wh-Questions](answers.md#wh-questions).

What every M3 construct writes (the rows for each milestone appear in [Every Construct and Its Native Form](semantics.md#every-construct-and-its-native-form)):

| Id | Example | Native form |
|---|---|---|
| `:individual/top-level` | "There is a man." | `Man-<16 hex> ~ man`, along with the internal mark `Man-<16 hex> ace:individual nil` (not rendered; M7's "at most" reads it) |
| `:object/something` | "Somebody waits." | `Somebody-<16 hex> waits nil`, lacking any class fact |
| `:verb/intransitive` | "John waits." | `John waits nil` |
| `:verb/transitive` | "John likes Mary." | `John likes Mary` |
| `:verb/ditransitive` | "John gives Mary a book." | `John gives <Book-<16 hex> Mary>`, with the objects organized as an ordered list |
| `:adj/positive` | "John is rich." | `John is rich` |
| `:adj/transitive` | "John is fond-of Mary." | `John "is fond of" Mary` |
| `:adj/comparison` | "John is taller than Bill." | `John "is taller than" Bill`; "as rich as" gives `"is as rich as"` |
| `:of/relation` | "Bill is a father of John." | `John father Bill` and `Bill ~ father` (the possessor acts as the subject; on a Wikidata property no class fact, refer to [below](#words-that-reach-wikidata)) |
| `:copula/located` | "John is in a park." | `John "is in" Park-<16 hex>`; the core `in` is never used as a content predicate |
| `:question/wh` | "Who likes Mary?" | the query `X likes Mary` within `ace:q` |
| `:data/string` | `John says "hello".` | a character list, `John says <o l l e h>`; the empty string corresponds to the internal node `ace:empty-string` |

The identical shapes, typed natively into zelph today, with the queries the questions become:

```
zelph> Man-6f1c2a9e0b3d4c5a ~ man
zelph> Man-6f1c2a9e0b3d4c5a sings nil
zelph> John likes Mary
zelph> John is rich
zelph> John "is fond of" Mary
zelph> John gives <Book-0c4d2e7f9a1b3c5d Mary>
zelph> Book-0c4d2e7f9a1b3c5d ~ book
zelph> John father Bill
zelph> Bill ~ father
zelph> John "is in" Park-2b7e9d4c1a3f5e6b
zelph> Park-2b7e9d4c1a3f5e6b ~ park
zelph> X ~ man, X sings nil
Answer: Man-6f1c2a9e0b3d4c5a ~ man, Man-6f1c2a9e0b3d4c5a sings nil
zelph> X likes Mary
Answer: John likes Mary
zelph> John father X, X ~ father
Answer: John father Bill, Bill ~ father
zelph> X "is in" Y, Y ~ park
Answer: John "is in" Park-2b7e9d4c1a3f5e6b, Park-2b7e9d4c1a3f5e6b ~ park
```

**Refused in these families:** the comparative without a target ("John is taller.", `:adj/comparative`), the superlative (`:adj/superlative`), a copula involving multiple prepositional phrases (`:copula/located-many`), real numbers and measured quantities (`:data/real`, `:data/measured`: zelph contains only integers and lacks units), and a wh-question whose referent is unconstrained ("Who is there?", `:question/unrestricted`). Integers come with M5, where-questions with M6, and how-many questions with M7.

## Rules and Negation

**Milestone M4.** Universal statements and conditional constructs are transformed into zelph rules; classical negation is reinterpreted as refutation; the phrase "not provably" is replaced by negation as failure; any contradictions that a block's inference meets are reported in ACE. Every rule is built as a single `=>` fact that incorporates all its outcomes, wrapped inside `zelph/dedup-rule`, ensuring that duplicating a rule produces no effect. A rule with a single condition is built as a condition set comprising one element, designated `~ conjunction`, which ensures that every rule the front end generates results in a condition set (decision [DEC-RECORD-SINGLE](decisions.md#dec-record-single)). zelph records each contradiction of such a rule, and thus reports it just once, identical to how it handles the same condition when not within a set: the condition is treated as the sole member, and the record is the same set constant. zelph lists such a rule as `(*{(X ~ man)} ~ conjunction) => …`, and `ace/zelph-of` outputs it exactly in that format; the plain `(X ~ man) => …` re-enters as the identical rule and is recorded alike, but the printed line corresponds to the one zelph lists for the rule the front end builds (decision [DEC-ONE-CONDITION-PRINT](decisions.md#dec-one-condition-print)).

### Every and If-Then

The first tutorial session within the user documentation is pinned from M4 onward by a test, exactly as presented here (test `ace tutorial: every man is mortal, without Wikidata`; [Tutorial Pins](tests.md#tutorial-pins)):

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Every man is mortal. John is a man. Douglas Adams is a man.
    Who is mortal?

    Warning [ace/w-multiword]: sentence 3, line 1, column 37: "Douglas Adams" is read as one name.
    Answer: Douglas Adams is mortal.
    Answer: John is mortal.
    zelph> .lang ace
    ace> X ~ man
    Answer: "Douglas Adams" ~ man
    Answer: John ~ man
    ```

    The zelph that the block writes (`ace/zelph-of`):

    ```
    (*{(X ~ man)} ~ conjunction) => (X is mortal)
    John ~ man
    "Douglas Adams" ~ man
    X is mortal
    ```

The name "Douglas Adams" is initially parsed as two names, an attempt that fails; the fallback then combines the capitalized run into a single name and issues a warning once per session. In answers, the sequence is output exactly as written, which re-enters through the same fallback mechanism. Natively, zelph quotes a name containing a space; its answer lines come in the host's order, determined by the graph's prior history.

The rule, typed natively within today's zelph:

```
zelph> (*{(X ~ man)} ~ conjunction) => (X is mortal)
zelph> John ~ man
(John is mortal) ⇐ {(John ~ man)}
zelph> "Douglas Adams" ~ man
("Douglas Adams" is mortal) ⇐ {("Douglas Adams" ~ man)}
zelph> X is mortal
Answer: "Douglas Adams" is mortal
Answer: John is mortal
zelph> X ~ man
Answer: "Douglas Adams" ~ man
Answer: John ~ man
```

"Every man is mortal." qualifies as a Horn rule (`:rule/horn`): its conclusion refers solely to what the condition introduces. A conditional is mapped the same way: "If a man waits then the man sleeps." translates to `((X ~ man), (X waits nil)) => (X sleeps nil)`. When a conclusion brings in a novel element, the rule employs a fresh variable (`:rule/generative-head`): "Every man owns a dog." becomes `(*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog)`, and a run makes one witness per man, unless a dog he owns is already recognized ([Fresh Variables](../logic.md#fresh-variables-generative-rules)).

A conclusion may additionally incorporate an element that is connected to nothing the condition binds (`:rule/disconnected-head`): "If a man waits then a dog barks." translates to `((X ~ man), (X waits nil)) => (Y ~ dog) (Y barks nil)`. Interpreted in first-order logic, this means "if any man waits, then there is a dog that barks", implying a single dog fulfils the requirement regardless of how many men wait. zelph implements precisely this behaviour: a single witness, regardless of the number of men waiting or the number of subsequent runs (decision [DEC-DISCONNECTED](decisions.md#dec-disconnected)). Starting from M4, the test `ace rules: a conclusion unconnected to its condition makes one witness for every binding and run` pins this behaviour. Natively, with two men waiting and then a third; the witness lacks a name, so zelph prints it by its class, and both queries find the same single dog:

```
zelph> ((X ~ man), (X waits nil)) => (Y ~ dog) (Y barks nil)
zelph> John ~ man
zelph> John waits nil
zelph+> Bill ~ man
zelph> Bill waits nil
zelph> X barks nil
Answer: dog barks nil
zelph> Tom ~ man
zelph> Tom waits nil
zelph> X barks nil
Answer: dog barks nil
```

The variables are assigned names in the order of their initial appearance, with conditions processed from left to right, followed by conclusions: `X`, `Y`, `Z`, then `A` through `W` (decision [DEC-VAR-LETTERS](decisions.md#dec-var-letters)). A class condition takes a distinct letter solely when its pattern includes a variable of its own: on a graph with Wikidata, "a man" is represented as `X P31 Y, ((Y P279 Q8441) closure zero-or-more)`, where `Y` walks up the subclass hierarchy; the core `X ~ man` lacks such a variable, which is why the dog above is `Y`. The atoms stand in the sequence dictated by the sentence: a subject's class atom precedes its verb, as in `(Y ~ dog) (Y barks nil)`, and a relation comes before the class atoms of the referents it introduces, as in `(X owns Y) (Y ~ dog)`.

### Negation: No, Does Not, Not Provably

ACE's `not` represents classical negation; "not provably" embodies negation as failure (the norm in AceRules, the Attempto rule engine). The two never mix (decision [DEC-NEG](decisions.md#dec-neg)).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace No man sings. John is a man. Bill does not sleep.
    Does John sing? Does Bill sleep? Does Sue sleep?

    Answer: No. It is false that John sings.
    Answer: No. It is false that Bill sleeps.
    Answer: It is not provable that Sue sleeps.
    ```

    The zelph this block writes (`ace/zelph-of`; the lines containing `refuted` are for reading):

    ```
    (*{(X ~ man)} ~ conjunction) => ((X sings nil) ~ refuted)
    John ~ man
    ¬(Bill sleeps nil)
    # yes/no: John sings nil
    # yes/no: Bill sleeps nil
    # yes/no: Sue sleeps nil
    ```

    In a fresh session, "not provably":

    ```
    zelph> .import ace
    zelph> ace If a man does not provably wait then the man sleeps.
    If a man does not provably own a car then the man walks.
    John is a man. Bill is a man. Bill waits. Bill owns a car. Who sleeps? Who walks?

    Answer: John sleeps.
    Answer: John walks.
    ```

    The zelph this block writes (`ace/zelph-of`; the lines containing `ace:naf-…` are intended for reading):

    ```
    ((X ~ man), ¬(X waits nil)) => (X sleeps nil)
    ((X ~ man), (X owns Y), (Y ~ car)) => (X ace:naf-9c2e4a6b8d0f1e3c nil)
    ((X ~ man), ¬(X ace:naf-9c2e4a6b8d0f1e3c nil)) => (X walks nil)
    John ~ man
    Bill ~ man
    Bill waits nil
    Bill owns Car-5e8a1c3b7d9f2e4a
    Car-5e8a1c3b7d9f2e4a ~ car
    X sleeps nil
    X walks nil
    ```

"No man sings." constitutes APE's implication featuring a negated conclusion, thus transforming into a rule that derives a refutation (`:neg/in-head`); "Does John sing?" subsequently yields the response `No.`. "Bill does not sleep." represents a refutation of a single fact (`:neg/fact`). "If a man does not provably wait …" puts one atomic element under "not provably" (`:naf/atom-in-body`), which zelph's `¬(…)` takes directly, and "Which man does not provably wait?" serves as a query involving the identical negated condition (`:naf/in-question`). A negation of a complex box at the topmost level amounts to a denial, a rule whose conclusion is `!` (`:neg/statement`): "There is no laundry." is expressed as `(*{(X ~ laundry)} ~ conjunction) => !`.

zelph's `¬(…)` accepts a single pattern. Thus, a "not provably" across multiple conditions is managed via a helper rule that features a positive body and a content-addressed name `ace:naf-<16 hex>` (a Lloyd-Topor helper; `:naf/complex-in-body`), while the main rule negates the fact produced by the helper. The helper's body also holds the conditions from the main rule that established the binding of its variables ("a man"), ensuring that it derives facts only for men who own a car, rather than for every car owner. These helpers appear in the list generated by `.list-rules` and undergo deduplication upon re-entry ([Helper Rules for "not provably"](semantics.md#helper-rules-for-not-provably)). The initial rule containing a "not provably" condition switches zelph's negation records on for the duration of the session (decision [DEC-NAF-RECORDS](decisions.md#dec-naf-records)), a state required by a subsequent subsection.

The "not provably" block, typed natively. The facts precede all else: a block writes every element before its single inference, and a conclusion drawn under "not provably" is definitive, hence the sequence is significant natively, with auto-run executing after each line.

```
zelph> John ~ man
zelph> Bill ~ man
zelph> Bill waits nil
zelph> Bill owns Car-5e8a1c3b7d9f2e4a
zelph> Car-5e8a1c3b7d9f2e4a ~ car
zelph> ((X ~ man), ¬(X waits nil)) => (X sleeps nil)
(John sleeps nil) ⇐ {(John ~ man) (¬(John waits nil))}
zelph> ((X ~ man), (X owns Y), (Y ~ car)) => (X ace:naf-9c2e4a6b8d0f1e3c nil)
(Bill ace:naf-9c2e4a6b8d0f1e3c nil) ⇐ {(Bill ~ man) (Car-5e8a1c3b7d9f2e4a ~ car) (Bill owns Car-5e8a1c3b7d9f2e4a)}
zelph> ((X ~ man), ¬(X ace:naf-9c2e4a6b8d0f1e3c nil)) => (X walks nil)
(John walks nil) ⇐ {(John ~ man) (¬(John ace:naf-9c2e4a6b8d0f1e3c nil))}
zelph> X sleeps nil
Answer: John sleeps nil
zelph> X walks nil
Answer: John walks nil
```

### Contradictions

A contradiction is managed through one of two mechanisms. When a statement asserts something the graph already refutes, or refutes something it asserts, the dry run detects it: the engine refuses the write operation, and the block is discarded without any data written (`[ace/contradiction]`). A contradiction that arises solely from the block's inference is reported in ACE and the block is retained (`[ace/contradiction-derived]`), with a maximum of five such instances per rule and block.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John is a man. John waits.

    zelph> ace No man waits.

    Contradiction [ace/contradiction-derived]: these sentences do not hold together:
      "No man waits." (block 2, sentence 1)
      "John is a man." (block 1, sentence 1)
      "John waits." (block 1, sentence 2)
    zelph> ace Does John wait? Mary is not a man.

    Answer: Yes. John waits.
    zelph> ace Mary is a man.

    Error in line "ace Mary is a man.": Contradiction [ace/contradiction]: sentence 1, line 1, column 1: zelph already holds the opposite of this sentence, from "Mary is not a man." (block 3, sentence 2).
      <>Mary is a man.
    ```

The report quotes the user's original statements: starting from M1, every fact and rule `ace` writes carries its source sentence as a node annotation (key `ace:src`), which adds no new node, never turns a rule off, and persists through `.save` and `.load` (decision [DEC-PROV](decisions.md#dec-prov)). A fact lacking such a source, like one from a loaded network, is quoted as its ACE sentence followed by "(a fact of the loaded graph)". The rule's derived refutation met a claimed fact, thus nothing was written and "John waits." still holds (decision [DEC-REFUTED](decisions.md#dec-refuted)); zelph records the contradiction, causing subsequent runs to remain silent on it, and `ace` keeps a session set of the contradictions it has reported, ensuring it does not report the same contradiction twice. The method for constructing the report is detailed in [Contradiction Reports](semantics.md#contradiction-reports).

The report originates from data, not from zelph's printed output. The block's inference is `(zelph/run-delta :quiet)` (host item P-quiet-run), which produces no output and returns every contradiction encountered, including its rule, condition facts, conclusion, and record status. The current version of zelph, with auto-run off so the quiet run occurs first:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> John ~ man
zelph-> John waits nil
zelph-> (*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)
zelph-> %(zelph/out (string/format "%q" (zelph/run-delta :quiet)))
{:contradictions ({:bindings @{<zelph/node X> <zelph/node John>} :conclusion <zelph/node John waits nil> :conditions (<zelph/node John ~ man>) :record :new :rule <zelph/node (*{(X ~ man)} ~ conjunction) => ((X waits nil) ~ refuted)>}) :naf-final ()}
zelph-> X waits nil
Answer: John waits nil
```

### A Conclusion That a Later Block Defeats

A conclusion drawn under "not provably" is definitive within zelph: any subsequent fact defeating it is reported, but never withdrawn (decisions [DEC-STRATA](decisions.md#dec-strata), DEC-NAF-RECORDS).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Every man who is not provably dead is alive. John is a man.

    zelph> ace John is dead.

    Warning [ace/w-naf-final]: conclusions drawn under "not provably" that no longer hold; zelph keeps them, because such conclusions are final:
      "Every man who is not provably dead is alive." (block 1, sentence 1)
        John is alive.
    ```

    The zelph that the first block writes (`ace/zelph-of`):

    ```
    ((X ~ man), ¬(X is dead)) => (X is alive)
    John ~ man
    ```

The warning originates from the data once more: when negation records are enabled, the quiet run returns the defeated conclusions beside the contradictions ([Conclusions Under "Not Provably" That a Later Block Defeats](block.md#conclusions-under-not-provably-that-a-later-block-defeats)). Today's zelph, with the records activated as `ace` activates them through its first "not provably" rule, and auto-run off so that quiet runs are the sole type of runs:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> %(zelph/negation-records true)
true
zelph-> ((X ~ man), ¬(X is dead)) => (X is alive)
zelph-> John ~ man
zelph-> %(zelph/out (string/format "%q" (zelph/run-delta :quiet)))
{:contradictions () :naf-final ()}
zelph-> John is dead
zelph-> %(zelph/out (string/format "%q" (zelph/run-delta :quiet)))
{:contradictions () :naf-final ({:fact <zelph/node John is alive> :rule <zelph/node (¬(X is dead), (X ~ man)) => (X is alive)>})}
zelph-> X is alive
Answer: John is alive
```

### Nested Conditionals

A conclusion containing a conditional (`:rule/nested`) gets transformed into a single rule where that is logically equivalent ("Every man sees every dog." becomes `((X ~ man), (Y ~ dog)) => (X sees Y)`). Two specific configurations lack a flattened equivalent and instead become rule generators, rules whose conclusion is another rule (decision [DEC-NESTED-IMP](decisions.md#dec-nested-imp); [Rules That Derive Rules](../logic.md#rules-that-derive-rules)): a switch with a ground outer condition, and an inner rule based on a referent that the outer conclusion introduces ([Nested Implications](semantics.md#nested-implications)).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace If John waits then every man sleeps. Bill is a man.
    Every man owns a dog that every cat hates. Tom is a cat.

    zelph> ace John waits. Does Bill sleep? Does Tom hate a dog?

    Answer: Yes. Bill sleeps.
    Answer: Yes. Tom hates a dog.
    ```

    The zelph these blocks write (`ace/zelph-of`):

    ```
    (*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
    Bill ~ man
    (*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog) ((*{(Z ~ cat)} ~ conjunction) => (Z hates Y))
    Tom ~ cat
    John waits nil
    # yes/no: Bill sleeps nil
    # yes/no: Tom hates X, X ~ dog
    ```

The dog serves as a witness, a node generated by the run without designation; an answer assigns it a name through its class, as an indefinite. The generator makes one dog for each man and one rule for each dog, with that rule firing on every cat. Natively:

```
zelph> (*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
zelph> Bill ~ man
zelph> (*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog) ((*{(Z ~ cat)} ~ conjunction) => (Z hates Y))
(Bill owns ??) ⇐ {(Bill ~ man)}
((*{(Z ~ cat)} ~ conjunction) => (Z hates dog)) ⇐ {(Bill ~ man)}
zelph+> Tom ~ cat
(Tom hates dog) ⇐ {(Tom ~ cat)}
zelph> John waits nil
((*{(X ~ man)} ~ conjunction) => (X sleeps nil)) ⇐ {(John waits nil)}
(Bill sleeps nil) ⇐ {(Bill ~ man)}
zelph> X sleeps nil
Answer: Bill sleeps nil
zelph> Tom hates X, X ~ dog
Answer: Tom hates dog, dog ~ dog
zelph> .list-rules
Listing all rules:
------------------------
(*{(John waits nil)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))
(*{(Z ~ cat)} ~ conjunction) => (Z hates dog)
(*{(X ~ man)} ~ conjunction) => (X owns Y) (Y ~ dog) ((*{(Z ~ cat)} ~ conjunction) => (Z hates Y))
(*{(X ~ man)} ~ conjunction) => (X sleeps nil)
------------------------
```

### Universal Questions and Disjunction

A conditional within a yes/no question (`:rule/in-question`, decision [DEC-QUNIV](decisions.md#dec-quniv)) undergoes hypothetical evaluation within the question's scratch cluster: first, a counter-instance is sought among the graph's own individuals (denoted `No.`); if none is found, the condition is provisionally accepted for a newly introduced hypothetical individual, a single quiet inference is executed, and the conclusion is assessed (marked `Yes.`); if neither outcome occurs, the answer cannot be established as provable. Before this, the block is refused (`:rule/unsafe-hypothesis`) if the hypothesis might reach a rule containing a "not provably" condition or a `!=` guard, since a fresh individual is "not provably" everything. The answer expresses the conditional using the variables that the ACE verbalizer introduces ([Questions That Hold a Conditional](answers.md#questions-that-hold-a-conditional)).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Every dog is an animal. Is it true that every dog is an animal?

    Answer: Yes. If there is a dog X then X is an animal.
    zelph> ace Rex is a dog. Rex does not bark. Is it true that every dog barks?

    Answer: No. It is false that if there is a dog X then X barks.
    ```

The second answer arises from the counter-instance Rex, identified before any assumption being made. These two lines constitute an exception to the round-trip process: re-entered, the `Yes.` line represents the rule itself, while the `No.` line stands as a negated universal, re-entering in that form, thus the round-trip test exempts the answers to a universal question.

A condition containing a disjunction generates a separate rule for each disjunct (`:or/in-body`): "If John waits or Mary sleeps then Bill runs." translates into the pair of rules `(*{(John waits nil)} ~ conjunction) => (Bill runs nil)` and `(*{(Mary sleeps nil)} ~ conjunction) => (Bill runs nil)`. A disjunction within a question gives one query per disjunct, with the resulting answers united (`:or/in-question`, "Who waits or sleeps?"). A one-atom negation in a condition applies exclusively to refuted facts (`:neg/atom-in-body`: "If John does not wait then Mary sleeps." corresponds to the condition `((John waits nil) ~ refuted)`), just as a single-atom negation in a question does (`:neg/atom-in-question`, "Who does not wait?"). "It is false that every dog barks." asserts the existence of one named individual dog that does not bark, with a refuted fact (`:neg/not-every`, `:rule/negated`).

**Refused within these families**, each by its unique identifier and before anything is written: a universal embedded within a condition (`:rule/in-body`), a "not provably" applied to a box containing another "not provably" or a universal (`:naf/nested-in-body`), a "not provably" condition that a conclusion derived from a "not provably" rule can reach (`:naf/unstratified`, also takes zelph's own verdict regarding the rules in force), complex classical negation appearing in a condition or a question (`:neg/complex-in-body` with the hint to write "does not provably", `:neg/complex-in-question`), "not provably" situated in a statement or a conclusion (`:naf/top-level`), a disjunction present in a statement or a conclusion (`:or/in-head-or-statement`), a conditional appearing in a wh-question (`:rule/in-wh-question`), and the rules zelph cannot execute safely (`:rule/unsafe`, "Everybody waits.", where no element binds the subject; `:rule/endless-generation`, "Every person has a mother who is a person.", leading to infinite witnesses). The section [Refusals](#refusals) explains the rationale behind refusing the negation forms in v1.

## Numbers and Formulas

**Milestone M5.** The maths stack is required for handling numbers and formulas, which changes the entire session: zelph's mathematics, its primes module, and the newly introduced evaluation module collectively amount to 6135 nodes and 456 rules (measured on the current tree using the evaluation module's starting text). Consequently, the stack is brought in explicitly via `.import ace-math` (decision [DEC-MATH-IMPORT](decisions.md#dec-math-import)). Absent this import, arithmetic parses and is refused by name:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John's age is 30.

    Error in line "ace John's age is 30.": Correct ACE, but zelph cannot use it [ace/needs-math]: sentence 1, line 1, column 12: a number, construct data/integer.
      John's age <>is 30.
      Numbers and arithmetic need the math stack: .import ace-math
    ```

The marker precedes "is" since the number functions as an argument for the copula, and the copula's token marks the position of that condition.

Using it, a formula (`:formula/compare` applied to numbers, `:formula/identity` applied to names, `:expr/arith` for the operators) is decided, verified, or compiled, and is never asserted as a zelph `=` fact (such a misstatement would result in a request yielding two answers). ACE's `/` represents the exact quotient `exquo` from `stdlib/evaluation.zph` (`exquo`, `difference`, `compare`), a general standard library module comprising 23 rules authored by M5, which `math.zph` does not include, and never zelph's truncating `/` on natural numbers. Each operator in ACE associates to the left, including `^`, so `2^3^2` evaluates to 64 here, whereas zelph's `$( … )` island reads it as 512. A term within a fact is stored by its computed value. A formula within a rule condition gets a seed rule, since zelph's arithmetic operates through requests: a rule merely asks, and a seed rule makes the request. Further details can be found in [Numbers and Formulas](math.md).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace-math
    zelph+> ace John's age is 29 + 1. John is a person.
    If a person's age is A and A > 17 then the person is an adult.
    Is John an adult? What is 2^3^2? What is 12 / 4? What is 7 / 2?

    Answer: Yes. John is an adult.
    Answer: 2^3^2 = 64.
    Answer: 12 / 4 = 3.
    No answer.
    zelph> ace Is it true that 1 + 2 = 3?
    For every integer X is it true that (X + 1)^2 = X^2 + 1?

    Answer: Yes. 1 + 2 = 3.
    Answer: No. It is false that for every integer X (X + 1)^2 = X^2 + 1. (X + 1)^2 - (X^2 + 1) = 2*X.
    zelph> ace 1 + 2 = 4.

    Error in line "ace 1 + 2 = 4.": Contradiction [ace/contradiction]: sentence 1, line 1, column 1: this formula is false: 1 + 2 - 4 = -1.
      <>1 + 2 = 4.
    ```

    The zelph the first block writes (`ace/zelph-of`; lines that name the maths stack are for reading):

    ```
    John age &30
    John ~ person
    ((X ~ person), (X age Y)) => (Y compare &17)
    ((X ~ person), (X age Y), ((Y compare &17) = gt)) => (X ~ adult)
    ```

`29 + 1` was evaluated once before the dry run, within a standalone scratch cluster `ace:math`; the fact holds the value `&30`. Since age is numeric, the rule lacks a class condition for it (no `Y ~ age`; decision [DEC-NUMERIC-REFERENT](decisions.md#dec-numeric-referent)), and its comparison transforms into a seed rule that requests a verdict and a main rule that tests it ([Formulas in Rules](math.md#formulas-in-rules)). `7 / 2` does not yield an integer result, thus it produces no answer (it is never rounded down to 3). A false universal answer carries the difference as its evidence on the identical line. These answer lines correspond to those in the question table under [Questions and Answers](math.md#questions-and-answers). The prompt `zelph+>` following the import is zelph's own: similar to `.import math`, the import ends with a run that hides the deductions from the maths stack.

Today's zelph performs the arithmetic and the request-driven rule, using the `cmp` comparison that the standard library already provides; `compare` within `stdlib/evaluation.zph` is the version that only delivers a verdict, which `ace-math` uses:

```
zelph> .import math
zelph+> ? :simplify ((&2 ^ &3) ^ &2)
Answer: (:simplify ((&2 ^ &3) ^ &2)) = &64
zelph+> ? :simplify ((&2 - &5) + &1)
Answer: (:simplify $( &2 - &5 + &1 )) = (neg zint &2)
zelph+> ((X ~ person), (X age Y)) => (Y cmp &17)
zelph> ((X ~ person), (X age Y), ((Y cmp &17) = gt)) => (X ~ adult)
zelph> ann ~ person
zelph> ann age &18
(&18 cmp &17) ⇐ {(ann ~ person) (ann age &18)}
(&18 lcmp &17) ⇐ (&18 cmp &17)
((&18 lcmp &17) res gt) ⇐ {(&18 lcmp &17) (0 isdigit true) (gt != eq) (1 isdigit true) ((&9 lcmp &8) res gt)}
((&18 cmp &17) = gt) ⇐ {(&18 cmp &17) ((&18 lcmp &17) res gt)}
(&18 > &17) ⇐ {((&18 lcmp &17) res gt) (&18 cmp &17)}
(ann ~ adult) ⇐ {(ann ~ person) (ann age &18) ((&18 cmp &17) = gt)}
zelph+> carl ~ person
zelph> carl age &17
(&17 cmp &17) ⇐ {(carl ~ person) (carl age &17)}
(&17 lcmp &17) ⇐ (&17 cmp &17)
((&17 lcmp &17) res eq) ⇐ {((1 dcmp 1) res eq) ((&8 lcmp &8) res eq) (&17 lcmp &17)}
(&17 == &17) ⇐ {(&17 cmp &17) ((&17 lcmp &17) res eq)}
((&17 cmp &17) = eq) ⇐ {(&17 cmp &17) ((&17 lcmp &17) res eq)}
zelph+> X ~ adult
Answer: ann ~ adult
```

The request for `(2 ^ 3) ^ 2`, the grouping ACE prescribes for `2^3^2`, yields 64. The seed rule requests a comparison for each person possessing an age, and the main rule fires for ann, who is 18, and does not fire for carl, who is 17.

"John is Bill." (`:copula/identity`) is interpreted as "John = Bill." (decision [DEC-NEQ](decisions.md#dec-neq)): two named constants are considered equal precisely when they resolve to the same node, decided after word resolution and before the dry run. Thus, "John is Bill." presents a contradiction that names what each side resolved to and suggests a lexicon binding, whereas "Sun is the Sun." is accepted and records no information. An identity linking an individual to a name cannot be established through a question, and is refused when stated, since `ace` never merges two existing nodes ([Identity](math.md#identity)). A negated formula is the formula with its truth value inverted (`:neg/formula`, `:naf/formula`): "It is false that 5 = 2 + 2." functions as a verification, never as a denial.

**Refused:** the operation of joining strings (`:expr/concat`), comparisons of strings, lists, or sets (`:formula/non-numeric`), and employing a term lacking an integer value as data ("John likes 7 / 2.", `:expr/as-data`): if stored as a term, it would constitute a request.

## Events and Modifiers

**Milestone M6.** A sentence containing a prepositional phrase (`:mod/pp`) or an adverb (`:mod/adverb`) keeps its plain fact while introducing an occurrence: an event individual within the internal vocabulary that holds the plain fact as a nested statement and carries the modifiers (decision [DEC-EVENTS](decisions.md#dec-events)). The question "Does John wait?" continues to be answered via the plain fact, and a rule condition featuring a modifier matches the occurrence through deep unification.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John waits in a park. John is a man.
    If a man waits in a park then the man is a visitor.
    Where does John wait? Who is a visitor? Does John wait?

    Answer: John waits in Park-2b7e9d4c1a3f5e6b.
    Answer: John is a visitor.
    Answer: Yes. John waits.
    ```

    The zelph that the block writes (`ace/zelph-of`; the lines with `ace:` names are meant for reading):

    ```
    John waits nil
    ace:event-3d5f7a9c1e2b4d6f ace:occurrence (John waits nil)
    ace:event-3d5f7a9c1e2b4d6f "is in" Park-2b7e9d4c1a3f5e6b
    Park-2b7e9d4c1a3f5e6b ~ park
    John ~ man
    ((X ~ man), (X waits nil), (Y ace:occurrence (X waits nil)), (Y "is in" Z), (Z ~ park)) => (X ~ visitor)
    X ace:occurrence (John waits nil), X Y Z
    X ~ visitor
    # yes/no: John waits nil
    ```

The where-question (`:question/where-when-how`) seeks an occurrence associated with a variable predicate and retains only those rows where the predicate consists of a locative `is <preposition>` construction; when-questions preserve the temporal ones, while how-questions additionally read the adverb fact `E ace:manner fast` ([Events and Modifiers](semantics.md#events-and-modifiers)). Natively, on today's zelph:

```
zelph> John waits nil
zelph> ace:event-3d5f7a9c1e2b4d6f ace:occurrence (John waits nil)
zelph> ace:event-3d5f7a9c1e2b4d6f "is in" Park-2b7e9d4c1a3f5e6b
zelph> Park-2b7e9d4c1a3f5e6b ~ park
zelph> ((X ~ man), (X waits nil), (Y ace:occurrence (X waits nil)), (Y "is in" Z), (Z ~ park)) => (X ~ visitor)
zelph> John ~ man
(John ~ visitor) ⇐ {(Park-2b7e9d4c1a3f5e6b ~ park) (John ~ man) (ace:event-3d5f7a9c1e2b4d6f ace:occurrence (John waits nil)) (John waits nil) (ace:event-3d5f7a9c1e2b4d6f "is in" Park-2b7e9d4c1a3f5e6b)}
zelph> X ~ visitor
Answer: John ~ visitor
zelph> X ace:occurrence (John waits nil), X Y Z
Answer: ace:event-3d5f7a9c1e2b4d6f ace:occurrence (John waits nil), ace:event-3d5f7a9c1e2b4d6f ace:occurrence (John waits nil)
Answer: ace:event-3d5f7a9c1e2b4d6f "is in" Park-2b7e9d4c1a3f5e6b, ace:event-3d5f7a9c1e2b4d6f ace:occurrence (John waits nil)
```

The rule matched the occurrence via its nested statement, `(X waits nil)` within `Y ace:occurrence …` ([Deep Unification](../logic.md#deep-unification)). Its conditions also hold the plain fact, since a condition bearing a modifier is mapped as a statement is: the plain atom, the occurrence, and its modifiers. The query returns the occurrence fact itself as well; the front end retains solely the locative row.

**Refused:** adverbs in the comparative or superlative (`:mod/adverb-degree`) and a copula accompanied by multiple prepositional phrases (`:copula/located-many`).

## Plurals, Counts and Groups

**Milestone M7.** A collective group (`:object/group`) functions as a set constant, with its identity defined by its members, meaning "John and Mary" and "Mary and John" represent a single node ([Set constants](../concepts.md#set-constants)). A plural count (`:object/plural-count`) introduces that many individuals, and "How many …?" serves to count them (`:question/how-many`). "At most" (`:object/at-most`, whose noun phrase ACE encloses within a condition list, `:group/condition-list`) translates into a denial concerning pairwise distinct witnesses (decisions [DEC-PLURALS](decisions.md#dec-plurals), DEC-NEQ). A mass noun lacking a measurement corresponds to a portion (`:object/mass`).

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John and Mary lift a table. John owns 2 cars. John drinks some water.
    Who lifts a table? How many cars does John own?

    Answer: John and Mary lift Table-7a1c3e5b9d2f4a6c.
    Answer: John owns 2 cars.
    zelph> ace Bill owns a red car. Bill owns a car. Bill owns at most 1 car.

    zelph> ace Sue owns Herbie. Sue owns Christine. Herbie is a car. Christine is a car.

    zelph> ace Sue owns at most 1 car.

    Contradiction [ace/contradiction-derived]: these sentences do not hold together:
      "Sue owns at most 1 car." (block 4, sentence 1)
      "Sue owns Herbie." (block 3, sentence 1)
      "Sue owns Christine." (block 3, sentence 2)
      "Herbie is a car." (block 3, sentence 3)
      "Christine is a car." (block 3, sentence 4)
    ```

    The zelph belonging to the first block and the final one (`ace/zelph-of`; the line containing `ace:individual` is for reading):

    ```
    {John Mary} lifts Table-7a1c3e5b9d2f4a6c
    Table-7a1c3e5b9d2f4a6c ~ table
    John owns {Car-1a2b3c4d5e6f7a8b Car-9f8e7d6c5b4a3f2e}
    Car-1a2b3c4d5e6f7a8b ~ car
    Car-9f8e7d6c5b4a3f2e ~ car
    John drinks Water-8e0a2c4b6d1f3e5a
    Water-8e0a2c4b6d1f3e5a ~ water
    ((Sue owns X), (X ~ car), ¬(X ace:individual nil), (Sue owns Y), (Y ~ car), ¬(Y ace:individual nil), (X != Y)) => !
    ```

The principle of unique names applies exclusively to named constants. It is possible for two indefinite descriptions to refer to the same entity – such as "a red car" and "a car" potentially denoting the same vehicle – thus Bill's block does not constitute a contradiction: each individual `ace` creates possesses the internal mark `I ace:individual nil`, and the denial process bypasses marked witnesses. Herbie and Christine are designated names, hence they are treated as separate vehicles. Witnesses associated with a generative rule carry no mark and are considered distinct, a closed-world interpretation the user documentation illustrates with the example: "Every man owns a red car. Every man owns a blue car. John is a man. John owns at most 1 car.", which presents a contradiction. The phrase "At most n" incurs a cost of n+1 witnesses and n(n+1)/2 guards per subject.

Natively: a set constant entered in either member sequence, followed by the denial with the mark conditions, two marked individuals that fail to trigger it, and two named cars that do:

```
zelph> {John Mary} lifts Table-7a1c3e5b9d2f4a6c
zelph> {Mary John} lifts Table-7a1c3e5b9d2f4a6c
zelph> X lifts Y
Answer: {John Mary} lifts Table-7a1c3e5b9d2f4a6c
zelph> ((John owns X), (X ~ car), ¬(X ace:individual nil), (John owns Y), (Y ~ car), ¬(Y ace:individual nil), (X != Y)) => !
zelph> Car-1a2b3c4d5e6f7a8b ace:individual nil
zelph> John owns Car-1a2b3c4d5e6f7a8b
zelph> Car-1a2b3c4d5e6f7a8b ~ car
zelph> Car-9f8e7d6c5b4a3f2e ace:individual nil
zelph> John owns Car-9f8e7d6c5b4a3f2e
zelph> Car-9f8e7d6c5b4a3f2e ~ car
zelph> John owns Herbie
zelph> Herbie ~ car
zelph> John owns Christine
zelph> Christine ~ car
! ⇐ {(¬(Herbie ace:individual nil)) (¬(Christine ace:individual nil)) (John owns Herbie) (Herbie ~ car) (John owns Christine) (Christine ~ car) (Christine != Herbie)}
Found one or more contradictions!
```

"At least", "more than", and "exactly" each introduce a corresponding number of individuals at the top level and the same number of new witnesses in a conclusion (`:object/at-least`); "exactly" further incorporates the "at most" denial. "Each of John and Mary waits." breaks down into a separate fact for each member. Lists and sets of data (`:data/list`, `:data/set`) are represented as `zelph/list` and `zelph/set`. The complete mapping for this family can be found at [Plurals, Groups, Cardinality and Mass](semantics.md#plurals-groups-cardinality-and-mass).

**Refused:** quantities and measures expressed in units ("2 kg of apples", `:object/measure`, `:data/measured`), because zelph lacks units and real numbers.

## Possibility and Belief

**Milestone M8.** It is required that "John can swim." and "Mary believes that Bill sings." do not assert the actual swimming of John or the singing of Bill (`:modal/can`, `:sub/that-clause`; `must`, `should`, and `may` are mapped identically to `can`). zelph's opaque predicates perform precisely this function: each argument slot within an opaque statement constitutes a mention scope, meaning the content placed there is recorded without asserting its truth (decision [DEC-MODAL](decisions.md#dec-modal); host item P-opaque; [Opaque Predicates](../logic.md#mentioning-a-statement-opaque-predicates)). The modal predicates `ace:can`, `ace:must`, `ace:should`, and `ace:may` are each declared opaque once per graph, at their initial appearance. A verb that takes a that-clause is declared by every block employing it with a that-clause, and this declaration constitutes one of the lines the block writes.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace Mary is a woman. Mary believes that Bill sings. John can swim.
    If a woman believes that Bill sings then the woman is worried.
    Does Bill sing? Who believes that Bill sings? Can John swim? Does John swim?
    Who is worried?

    Answer: It is not provable that Bill sings.
    Answer: Mary believes that Bill sings.
    Answer: Yes. John can swim.
    Answer: It is not provable that John swims.
    Answer: Mary is worried.
    ```

    The zelph that the block writes (`ace/zelph-of`; the lines containing `opaque` and `ace:can` are for reading; the declaration `ace:can ~ opaque` is written once per graph and does not appear among the lines):

    ```
    Mary ~ woman
    believes ~ opaque
    Mary believes (Bill sings nil)
    (John swims nil) ace:can nil
    ((X ~ woman), (X believes (Bill sings nil))) => (X is worried)
    ```

A box containing multiple conditions or possessing its own referent is written as a set constant of mentions: "A customer can enter a card." claims the existence of a customer, yet does not claim the existence of a card, and writes `{(Card-<16 hex> ~ card) (Customer-<16 hex> enters Card-<16 hex>)} ace:can nil`. A rule condition or a question concerning this box matches the stored set through its members (host item P-set-match). The that-clause and the rule from the session above, natively, on the current zelph:

```
zelph> believes ~ opaque
zelph> Mary ~ woman
zelph> Mary believes (Bill sings nil)
zelph> X sings nil
zelph> X believes (Bill sings nil)
Answer: Mary believes (Bill sings nil)
zelph> .explain Bill sings nil
Bill sings nil  [mentioned; not asserted]
zelph> ((X ~ woman), (X believes (Bill sings nil))) => (X is worried)
(Mary is worried) ⇐ {(Mary believes (Bill sings nil)) (Mary ~ woman)}
zelph> X is worried
Answer: Mary is worried
```

The nested statement does not satisfy any query independently, `.explain` labels it as mentioned but not asserted, and a rule condition continues to access the contents within the opaque argument.

**Refused in v1:** a that-clause containing a ground conditional ("John believes that if Mary waits then Bill sleeps.", `:sub/ground-conditional`; lifting requires the planned record that a rule was asserted, one of the engine items on [What zelph Still Needs](prerequisites.md#k1-p1-and-k1-p2-a-record-that-a-rule-was-asserted)), a that-clause holding a conditional with variables within a condition or conclusion (`:sub/conditional-with-referents-in-body`, `-in-head`), and commands, which lack a truth value (`:command/imperative`). At the top level, "John believes that every man waits." is supported: the rule is written using `zelph/mention` within the opaque statement, thus it is not in force and does not fire until an assertion claims it (decision [DEC-MENTION-RULE](decisions.md#dec-mention-rule); [Mentioning a Rule](../logic.md#mentioning-a-rule-is-not-asserting-it)). The reason why conditionals within a that-clause are refused is detailed in [A Rule Inside a That-Clause](constructs.md#a-rule-inside-a-that-clause).

## Why, and Taking a Statement Back

**Milestone M9.** No new construct introduced, yet three requests commonly made by a reasoner's user emerge: the justification for why a particular answer holds, a block's derived content, and the method for retracting a given statement. Two options, both off by default, print ACE output only (decision [DEC-ECHO](decisions.md#dec-echo)): `:echo` outputs every sentence a block's inference derived, alongside the premises it relies upon, and `:justify` extends each answer with the supporting sentences. The source information comes from `ace:src` annotations written since M1, ensuring persistence across `.save` and `.load` operations; a source originating from a different session prints as "(from a saved graph)".

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> %(ace/option :echo true)
    true
    zelph> %(ace/option :justify true)
    true
    zelph> ace John is a man.

    zelph> ace Every man is a human. Every human is mortal.

    Derived: John is a human.
      "Every man is a human." (block 2, sentence 1)
      "John is a man." (block 1, sentence 1)
    Derived: John is mortal.
      "Every human is mortal." (block 2, sentence 2)
      "John is a human." (derived above)
    zelph> ace Is John mortal?

    Answer: Yes. John is mortal.
      "Every human is mortal." (block 2, sentence 2)
      "Every man is a human." (block 2, sentence 1)
      "John is a man." (block 1, sentence 1)
    ```

The echo reads the facts the quiet run derived, in the sequence of their derivation, along with the premises they rely on (`(zelph/run-delta :quiet :derived)`, host item P-derived); the justified answer reads the complete proof (`zelph/explain`, host item P-explain; [Proofs as Data](../janet.md#proofs-as-data-zelphexplain)). The layout of each is defined in [The Derivation Echo](answers.md#the-derivation-echo) and [Why: Justified Answers](answers.md#why-justified-answers). Today's zelph, the facts a quiet run derived, each accompanied by the premises it is based on, as structured data:

```
zelph> .auto-run
Auto-run is now disabled.
zelph-> (*{(X ~ man)} ~ conjunction) => (X ~ human)
zelph-> (*{(X ~ human)} ~ conjunction) => (X is mortal)
zelph-> John ~ man
zelph-> %(each e (get (zelph/run-delta :quiet :derived) :derived) (zelph/out (string/format "%q  from  %q" (e :fact) (e :premises))))
<zelph/node John ~ human>  from  (<zelph/node John ~ man>)
<zelph/node John is mortal>  from  (<zelph/node John ~ human>)
```

**Taking a statement back.** The block keyword `ace-retract` (decision [DEC-RETRACT](decisions.md#dec-retract)) retracts fact statements. zelph lacks truth maintenance: a fact that was inferred from a removed premise would persist. Thus, `ace-retract` refuses, performs no removal, provided that a derived fact rests on the statement or another statement nests it, and enumerates such dependencies in ACE.

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John is a man. Mary is a woman. Every man is a human.

    zelph> ace-retract Mary is a woman.

    zelph> ace Is Mary a woman? Is John a human?

    Answer: It is not provable that Mary is a woman.
    Answer: Yes. John is a human.
    zelph> ace-retract John is a man.

    Error in line "ace-retract John is a man.": Correct ACE, but zelph cannot use it [ace/retract-dependents]: sentence 1, line 1, column 1: other facts rest on this sentence, so nothing was taken back.
      <>John is a man.
      "John is a man." is a premise of:
        John is a human.
    ```

A sentence featuring a modifier is fully withdrawn ("John waits in a park." eliminates both the occurrence and its associated modifiers alongside the plain fact), whereas "John waits." by itself is refused as long as the occurrence holds it. Only factual assertions are withdrawn; a rule sentence within an `ace-retract` block is refused, and a statement that the graph does not support is untouched. Natively, the proof `.explain` prints, and two of the three engine bindings `ace-retract` uses (host item P-retract): `zelph/dependents` lists what rests on a fact, `zelph/prune-fact` withdraws one; the third, `zelph/holders`, lists the statements that enclose a fact. The step-by-step procedure is detailed in [Taking a Statement Back](answers.md#taking-a-statement-back-ace-retract).

```
zelph> (*{(X ~ man)} ~ conjunction) => (X ~ human)
zelph> (*{(X ~ human)} ~ conjunction) => (X is mortal)
zelph> John ~ man
(John ~ human) ⇐ {(John ~ man)}
(John is mortal) ⇐ {(John ~ human)}
zelph> .explain John is mortal
John is mortal
   └─ John ~ human
      └─ John ~ man  [axiom]
zelph> %(zelph/out (string/format "%q" (zelph/dependents "John" "~" "man")))
(<zelph/node John ~ human>)
zelph> Mary ~ woman
zelph> %(zelph/out (string/format "%q" (zelph/dependents "Mary" "~" "woman")))
()
zelph> %(zelph/out (string/format "%q" (zelph/prune-fact "Mary" "~" "woman")))
:removed
zelph> X ~ woman
```

## Words That Reach Wikidata

**Milestones M1 (word resolution), M3 (class statements and the second tutorial), M4 (class conditions in rules).** Following a Wikidata import, ACE content words reach Wikidata's items via their English labels, without requiring any declaration (decision [DEC-WIKIDATA](decisions.md#dec-wikidata)). A word is resolved based on its function within the sentence (noun, name, relation, verb, adjective) through four stages: an explicit identifier (`n:Q8441`, `p:Q42`, `n:P50`, `v:P155`, `a:Q3142`), a node already named in `ace`, a Wikidata label (applicable only to nouns, names, and relational nouns, with the lowest Q-number among the carriers taking precedence), or a newly created node in `ace`. When a label is shared by multiple items, it is never bound silently: each block outputs a single note listing these bindings ([The word-senses Note](words.md#the-word-senses-note)). The second tutorial session in the user documentation operates using the Wikidata-style test fixture, a small dump of 86 entities that M0 incorporates into the test data ([The Wikidata-Style Fixture](words.md#the-wikidata-style-fixture)). The session retrieves a duplicate from a temporary directory; the `.load` command disables auto-run, thus producing the prompt `zelph->`, and the `…` line represents the importer's lines. It is pinned from M3 onward by a test, exactly as presented here (test `ace tutorial: words reach Wikidata items`; [Tutorial Pins](tests.md#tutorial-pins)):

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

On this graph, the block writes `Man-<16 hex> P31 Q8441` and `Man-<16 hex> admires Q42`: the individual and the verb `admires` are newly created nodes within `ace`, while the remaining elements correspond to items from Wikidata. The term "human" lacks a line in the note, as only a single item carries its label. The initial question seeks the class pattern `Q42 P31 X, ((X P279 Q5) closure zero-or-more)`: zero steps of the closure include Q5 itself, and a deeper class would be reached by walking `P279`. "occupation" was resolved to the property P106, so the second question poses `Q42 P106 X` without any class condition (decision [DEC-OF](decisions.md#dec-of)); its answer is the item writer, Q36180, printed with its prefix since its label "writer" is in lowercase and so is no spelling of a name. On a Wikidata graph, `ace/zelph-of` follows each line with a comment `# nodes: …` listing the resolved items; there, the lines serve solely for reading.

The instance and subclass predicates are pinned per graph through a single internal fact, written with the first class statement: `ace:profile ace:uses ace:wikidata` (instance `P31`, subclass `P279`) within a Wikidata graph, `ace:profile ace:uses ace:core` (instance core `~`, no subclass predicate) in all other cases (decision [DEC-PATHS](decisions.md#dec-paths)). Statements and conclusions write `X P31 C`; questions and rule conditions follow the subclass hierarchy. Wikidata retains certain memberships in alternative properties: "Is Douglas Adams a writer?" yields `It is not provable that Douglas Adams is a writer.`, since the occupation is encoded using P106, which is why the user guide presents the occupation question higher up ([What Wikidata Models Differently](words.md#what-wikidata-models-differently)). Absent Wikidata, the same block operates identically, except that each word turns into an independent node.

The native shapes, on facts of the Wikidata-like kind entered into zelph today; the explicit closure form output by `ace/zelph-of` and the `∗` shorthand in zelph are the same condition ([Transitive Path Conditions](../logic.md#transitive-path-conditions)):

```
zelph> Q42 P31 Q5
zelph> Q5 P279 Q215627
zelph> Q42 P106 Q36180
zelph> Q42 P31 V, ((V P279 Q215627) closure zero-or-more)
Answer: Q42 P31 Q5, Q5 P279∗ Q215627
zelph> Q42 P31 V, V P279∗ Q36180
zelph> Q42 P106 X
Answer: Q42 P106 Q36180
```

The first query reaches the class Q215627 via a single `P279` transition starting from Q5. The identical pattern applied to the item writer (Q36180) yields no result, as Douglas Adams is a writer solely through P106, which the last query reads.

The way a word gets resolved, how to select an alternative sense, the `ace` Wikidata network designed for this purpose, and the test fixture are all detailed in [Words and Wikidata](words.md).

## Refusals

Each construct that the parser is capable of processing corresponds to a row within one construct table. A block either fully accommodates all constructs it contains or is refused, explicitly identifying the construct; it never builds a graph that conveys a meaning different from the original text. A refusal is triggered prior to any writing taking place: by the classifier operating on the DRS, by `ace/plan`, by the validation checks during the resolution step, or by a dry run conducted on a scratch cluster that is subsequently discarded, ensuring that the graph following a refused block remains identical to the graph preceding it. The identical functions are used to classify the APE regression corpus during testing, so starting from M2 onward, every accepted corpus text located outside the current milestone is refused using a unique identifier of its own, never due to a syntax error.

Throughout development, a refusal names the milestone that brings the construct. This instance is the specification's own example, before M8:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace John can fly.

    Error in line "ace John can fly.": Correct ACE, but zelph cannot use it [ace/unsupported]: sentence 1, line 1, column 10: modality ('can'), construct modal/can.
      John can <>fly.
      Modal statements, recorded without being asserted, are planned for milestone M8.
    ```

The marker precedes "fly": a modal box lacks a token of its own, thus the position corresponds to the initial condition within it, the verb ([Positions](constructs.md#positions)); the message's text continues to identify the word responsible for rendering the sentence modal, "can". Upon M8’s arrival, the sentence is accepted; the ids that stay refused in v1 refer to a reason rather than a milestone.

In version v1, 26 identifiers remain refused, each accompanied by a reason. Three of these: two originating from M4's families of rules and negation, and the command:

!!! example "Specified session — the behaviour this specification requires, not yet implemented"

    ```
    zelph> .import ace
    zelph> ace If every dog barks then a cat sleeps.

    Error in line "ace If every dog barks then a cat sleeps.": Correct ACE, but zelph cannot use it [ace/unsupported]: sentence 1, line 1, column 10: a universal inside a condition ('dog'), construct rule/in-body.
      If every <>dog barks then a cat sleeps.
      zelph orders "not provably" over classes only without Wikidata and only where no conclusion feeds back, and this sentence would need it.
    ```

    ```
    zelph> .import ace
    zelph> ace If a man is not provably married then the man is a bachelor.
    If a man is not provably a bachelor then the man is a husband.
    Every husband is a man. John is a man.

    Error in line "ace If a man is not provably married then the man is a bachelor. …": Correct ACE, but zelph cannot use it [ace/unsupported]: sentence 2, line 2, column 1: "not provably" over what another "not provably" concludes, construct naf/unstratified.
      <>If a man is not provably a bachelor then the man is a husband.
      Sentence 1 concludes that a man is a bachelor, which this sentence tests with "not provably"; sentence 3 leads the conclusion of this sentence back to "a man". zelph cannot order these sentences.
    ```

    ```
    zelph> .import ace
    zelph> ace John, buy a car!

    Error in line "ace John, buy a car!": Correct ACE, but zelph cannot use it [ace/unsupported]: sentence 1, line 1, column 7: a command ('buy'), construct command/imperative.
      John, <>buy a car!
      A command has no truth value that zelph could store.
    ```

"Every dog barks" within a condition needs a closed-world test of the universal, whose helper rule would itself contain a "not provably", thus meeting the same limit as the second text. A command possesses no truth value. The second text is refused by a context id, which the resolution step identifies through the block's rules; since those carry no token, the position corresponds to the first token of the sentence where the "not provably" is reached.

Why the second text is refused, and why a refusal constitutes the truthful response: zelph assigns rules featuring "not provably" conditions to distinct negation levels ([Stratified Evaluation](../logic.md#stratified-evaluation)). v1 accepts a "not provably" condition solely when no conclusion stemming from a "not provably" rule can reach it, hence the first two sentences alone are already refused, even though zelph organizes that pair appropriately within a graph lacking Wikidata. The third sentence closes a cycle: the class hierarchy guides the conclusion of sentence 2 back to "a man", which the helper rule in sentence 2 reads. zelph evaluates such a loop through alternating evaluation, yet here the alternation goes wrong. Typed natively with its helper rule, today's zelph concludes that John is both a bachelor and a husband simultaneously, whereas the sole interpretation consistent with the text portrays him as a bachelor but not a husband. The outcome remains identical across every run:

```
zelph> ((X ~ man), ¬(X is married)) => (X ~ bachelor)
zelph> ((X ~ man), (X ~ bachelor)) => (X ace:naf-4b6d8f0a2c1e3b5d nil)
zelph> ((X ~ man), ¬(X ace:naf-4b6d8f0a2c1e3b5d nil)) => (X ~ husband)
zelph> (*{(X ~ husband)} ~ conjunction) => (X ~ man)
zelph> John ~ man
(John ~ bachelor) ⇐ {(¬(John is married)) (John ~ man)}
(John ~ husband) ⇐ {(¬(John ace:naf-4b6d8f0a2c1e3b5d nil)) (John ~ man)}
(John ace:naf-4b6d8f0a2c1e3b5d nil) ⇐ {(John ~ man) (John ~ bachelor)}
zelph> X ~ husband
Answer: John ~ husband
```

The front end identifies these forms two times before any writing occurring: across the rules it wrote, via a graph mapping which conclusion can influence which condition (with the subclass hierarchy traversed upwards), and via zelph's own verdict on the rules in force, which `(zelph/strata)` returns ([Negation levels](../janet.md#negation-levels)). The runs on a graph structured like Wikidata, the complete detection process, and the narrowing that would permit orderable shapes post-v1 (decision [DEC-STRATA](decisions.md#dec-strata)) are detailed in [Constructs and Refusals](constructs.md#not-provably-that-depends-on-another-not-provably). The remaining refusals from v1 are listed with their reasons in [Without a Route in v1](constructs.md#without-a-route-in-v1); the engine items that would lift some of these are found in [What zelph Still Needs](prerequisites.md) and in [Where the logic goes next](../about.md#where-the-logic-goes-next).

## What This Builds On in zelph

Each native form presented here leverages an engine feature that is present, described in documentation, and verified through testing in zelph 1.1.0, with the exception of the three requests from `stdlib/evaluation.zph` (`exquo`, `difference`, `compare`), which M5 writes as a general stdlib module. [What zelph Already Provides](foundation.md) outlines each one, specifying the host item responsible for its creation; the individual sections within the zelph documentation describe the features themselves:

| Feature in the sessions | zelph documentation |
|---|---|
| facts, phrase predicates, `nil` objects, nested statements | [Core Concepts](../concepts.md), [Facts with Multiple Objects](../logic.md#facts-with-multiple-objects) |
| yes/no probes that create nothing | [Existence Check](../janet.md#existence-check-zelphexists) |
| set constants for groups | [Set constants](../concepts.md#set-constants) |
| rules, a single `=>` with multiple conclusions, fresh variables | [Rules Over Graphs](../logic.md#rules-over-graphs), [Fresh Variables](../logic.md#fresh-variables-generative-rules) |
| rule generators | [Rules That Derive Rules](../logic.md#rules-that-derive-rules) |
| refutation, denials, contradiction records | [Contradiction Detection](../logic.md#contradiction-detection) |
| negation as failure and its levels | [Negation as Failure](../logic.md#negation-as-failure), [Stratified Evaluation](../logic.md#stratified-evaluation) |
| `!=` guards | [Inequality Constraints](../logic.md#inequality-constraints) |
| class patterns using `P279` | [Transitive Path Conditions](../logic.md#transitive-path-conditions) |
| occurrences matched via nested statements | [Deep Unification](../logic.md#deep-unification) |
| mentions without assertion | [Opaque Predicates](../logic.md#mentioning-a-statement-opaque-predicates), [Mentioning a Rule](../logic.md#mentioning-a-rule-is-not-asserting-it) |
| quiet runs returning contradictions, defeats, and derivations | [Running quietly](../janet.md#running-quietly) |
| provenance expressed as node annotations | [Node annotations](../janet.md#node-annotations) |
| proofs as data | [Proofs as Data](../janet.md#proofs-as-data-zelphexplain) |
| arithmetic and requests | [Why zelph for Mathematics](../math/index.md) |
| Wikidata labels common to multiple items | [Labels that several entities share](../wikidata.md#labels-that-several-entities-share) |
| a keyword language built atop the Janet API | [SPARQL](../sparql.md) |

A small number of engine items that the front end still needs, and the way v1 functions in their absence until they are available, are detailed in [What zelph Still Needs](prerequisites.md).
