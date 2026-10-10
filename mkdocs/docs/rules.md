# Rules and Inference

One of zelph's most powerful features is the ability to define inference rules within the same network as facts. Rules are statements containing `=>` with conditions before it and a consequence after it.

For an in-depth treatment of zelph's rule system — including deep unification, negation as failure, inequality constraints, fresh variables, and the formal connection to predicate logic — see [Logic and Computation](logic.md).

## Rule Syntax

A rule in zelph is formally a statement where the subject is a **set of conditions** (marked as a conjunction) and the object is the **consequence**.

Example rule:

```
(*{(R is transitive) (X R Y) (Y R Z)} ~ conjunction) => (X R Z)
```

**Breakdown of the syntax:**

1. `{...}`: Creates a **Set** containing three fact templates:
   - `R` is a transitive relation.
   - `X` is related to `Y` via `R`.
   - `Y` is related to `Z` via `R`.
2. `~ conjunction`: Defines that this Set represents a logical "AND" (Conjunction). The inference engine only evaluates sets marked as conjunctions.
3. `(*...)`: The surrounding parentheses create the fact `Set ~ conjunction`.
4. `*`: The **Focus Operator** at the beginning ensures that the expression returns the **Set Node** itself, not the fact node `Set ~ conjunction`.
5. `=>`: The inference operator. It links the condition Set (Subject) to the consequence (Object).
6. `(X R Z)`: The consequence fact.

This rule states: _If there exists a set of facts matching the pattern in the conjunction, then the fact `X R Z` is deduced._

### Syntax Sugar for Conditions

A parenthesised group that contains commas is parsed as **conjunction syntax sugar**:

```
(cond1, cond2, cond3)
```

Each comma-separated condition is itself a normal zelph statement fragment (either a fact pattern like `X R Y`, or a nested expression). The whole parenthesised expression evaluates to a **set node** that is automatically tagged as a conjunction internally (i.e. it desugars to the same topology as `(*{...} ~ conjunction)`).

What matters is what a condition **evaluates to**: it has to be a statement, because a condition is matched against the graph and a node carries nothing to match. A [focus](concepts.md#the-focus-operator) in that position therefore does not do what it looks like — it makes its statement evaluate to the focused node — and is refused:

```
zelph> (*A p C, C q b) => (A marked yes)
Error in line "(*A p C, C q b) => (A marked yes)": condition 1 of the comma list
is "A", which is not a statement and can never match. A focus makes its
statement evaluate to the focused node, so a condition written "*A p C" is the
node A -- write it "A p C" instead.
```

A focus one level down stays useful, since the condition still evaluates to a fact: `((*A p c) q b, A r d)` is the condition `A q b`, with `A p c` created on the side.

Practical consequence: you can write the above example rule as

```
(R is transitive, X R Y, Y R Z) => (X R Z)
```

without using the set syntax `{...}` or the `conjunction` core node.

## Examples

Here is a practical example of how a transitive-closure rule works in zelph (which you can also try out in interactive mode):

```
zelph> (R is transitive, A R B, B R C) => (A R C)
((A R B), (R is transitive), (B R C)) => (A R C)
```

After the entered rule, we see zelph's output, which in this case simply confirms the input of the rule.

Now, let us state that the relation `>` (greater than) is transitive:

```
zelph> > is transitive
> is transitive
```

Next, we provide three elements ("4", "5" and "6") for which the `>` relation applies:

```
zelph> 6 > 5
6 > 5
zelph> 5 > 4
5 > 4
(6 > 4) ⇐ {(6 > 5) (> is transitive) (5 > 4)}
zelph>
```

After entering `5 > 4`, zelph's unification mechanism takes effect and automatically adds a new fact: `6 > 4`. This demonstrates the power of the transitive relation rule in action. Note that the rule uses `R` as a variable for the predicate itself — this is possible because predicates are first-class nodes in the graph, not edge labels. Any relation that is declared `is transitive` will automatically benefit from this single rule.

Rules can also define contradictions using `!`:

```
zelph> (X "is opposite of" Y, A ~ X, A ~ Y, X != Y) => !
((X "is opposite of" Y), (A ~ X), (X != Y), (A ~ Y)) => !
zelph> bright "is opposite of" dark
bright "is opposite of" dark
zelph> yellow ~ bright
yellow ~ bright
zelph> yellow ~ dark
yellow ~ dark
! ⇐ {(bright "is opposite of" dark) (yellow ~ bright) (bright != dark) (yellow ~ dark)}
Found one or more contradictions!
zelph>
```

This rule states that if X is opposite of Y and X ≠ Y, then an entity A cannot be both an instance of X and an instance of Y, as this would be a contradiction. The `X != Y` guard is essential here: without it, a reflexive fact like `bright "is opposite of" bright` could cause a spurious contradiction when `yellow ~ bright` is entered, because `X` and `Y` would both bind to `bright` (see [Inequality Constraints](logic.md#inequality-constraints) for a detailed discussion).

A contradiction is **reported, not enforced**. The facts that triggered it stay in the graph – zelph is built to audit inconsistent real-world data, and deleting the evidence would defeat that.

What _is_ written is the contradiction itself: the set of the facts that matched, entered as **refuted** – "these statements do not hold together". Nothing is retracted by it. Every member stays asserted and keeps answering queries, including the conjunctive one; the set is the only node created, and a condition that matched no fact, such as an `!=` guard, contributes nothing to it.

That record is what makes a contradiction reported **once**. A set constant is defined by its members, so the same contradiction always yields the same node, and the next run finds it already present – the same way a derived fact remains silent on the second occurrence because the graph holds it. Two consequences worth knowing: the record ceases when the facts it pertains to are removed, so a contradiction emerges as a new discovery if those facts return; and it is indexed upon those facts rather than upon the rule, so two rules contradicting on the same statements report only once between them.

`.contradiction-records off` turns the record off, and the repetition with it. The cost it trades away is one set node per distinct contradiction, which is six figures on a Wikidata-scale audit.

What you get on top is a report – on the console, and in the [derivation export](#exporting-derivations) as a record with `"kind":"contradiction"` and the premises that produced it. The export is written on every run that meets the contradiction, whether or not the console line was printed, so a second `.run-export` does not return an empty file.

`!` remains the one consequence that derives no fact, which is why a contradiction rule is always safe in the [deferred stratum](logic.md#stratified-evaluation): it can derive nothing that another rule could then negate. The refuted set is a record ABOUT the match, not a derivation from it.

## Internal Representation of facts

In a conventional semantic network, relations between nodes are labeled, e.g.

```mermaid
graph LR
    bright -->|is opposite of| dark
```

zelph's representation of relation types works fundamentally differently.
As mentioned in the introduction, one of zelph's distinguishing features is that it treats relation types as first-class nodes rather than as mere edge labels.

Internally, zelph creates special nodes to represent relations.
For example,when identifying "is opposite of" as a relation (predicate), this internal structure is created:

```mermaid
graph TD
    n_3["~"]
    n_1["->"]
    n_5688216769861436680["is opposite of ~ ->"]
    n_10["is opposite of"]
    style n_10 fill:#8a5c00,stroke:#666666,stroke-width:2px,color:#e0e0e0
    n_5688216769861436680 <--> n_10
    n_1 --> n_5688216769861436680
    n_5688216769861436680 --> n_3
```

The nodes `->` and `~` are predefined zelph nodes. `->` represents the category of all relations, while `~` represents a subset of this category, namely the category of categorical relations. Every relation that differs from the standard relation `~` (like "is opposite of") is linked to `->` via a `~` relation.

The node `is opposite of ~ ->` represents this specific relation (hence its name).
The relations to other nodes encode its meaning.

This approach provides several advantages:

1. It enables meta-reasoning about relations themselves
2. It simplifies the underlying data structures
3. It allows relations to participate in other relations (higher-order relations)
4. It provides a unified representation mechanism for both facts and rules

This architecture is particularly valuable when working with knowledge bases like Wikidata, where relations (called "properties" in Wikidata terminology) are themselves first-class entities with their own attributes, constraints, and relationships to other entities. zelph's approach naturally aligns with Wikidata's conceptual model, allowing for seamless representation and inference across the entire knowledge graph.

Similarly, when stating:

```
bright "is opposite of" dark
```

zelph creates a special relation node that connects the subject "bright" bidirectionally, the object "dark" in reverse direction, and the relation type "is opposite of" in the forward direction.

```mermaid
graph TD
    n_11["dark"]
    n_9["bright"]
    n_8445031417147704759["bright is opposite of dark"]
    n_10["is opposite of"]
    style n_10 fill:#8a5c00,stroke:#666666,stroke-width:2px,color:#e0e0e0
    n_8445031417147704759 --> n_10
    n_9 <--> n_8445031417147704759
    n_11 --> n_8445031417147704759
```

The directions of the relations are as follows:

| Element       | Example        | Relation Direction |
| ------------- | -------------- | ------------------ |
| Subject       | white          | bidirectional      |
| Object        | black          | backward           |
| Relation Type | is opposite of | forward            |

This semantics is used by zelph in several contexts, such as rule unification. It's required because zelph doesn't encode relation types as labels on arrows but rather as equal nodes. This has the advantage of facilitating statements about statements, for example, the statement that a relation is transitive.

zelph also supports **self-referential facts**, where subject and object are the same
node (e.g., `A cons A`). These arise rarely in practice — Wikidata contains a small
number of such entries, for example `South Africa (Q258) country (P17) South Africa
(Q258)`. On input and output, such facts are covered by the
[self-fact prefix `:`](concepts.md#the-self-fact-prefix): the Wikidata example prints as
`:P17 Q258`. Internally, the object connection is omitted because the subject is already
connected to the fact-node bidirectionally, which serves as the implicit object
connection. Detection is unambiguous: a fact-node whose left-neighbor set contains
only the subject node (no additional unidirectional incoming connection) is
self-referential.

## Internal representation of rules

Rules are not stored in a separate list; they are an integral part of the semantic network. The implication operator `=>` is treated as a standard relation node.

When you define:
`(*{A B} ~ conjunction) => C`

The following topology is created in the graph:

1. A node `S` is created to represent the set of conditions.
2. The conditions `A` and `B` are linked to `S` via `PartOf` relations.
3. A fact node represents `S ~ conjunction` (defining the logical AND).
4. A fact node represents `S => C` (the rule itself).

When the inference engine scans for rules, it looks for all facts involving the `=>` relation. It examines the subject (the set `S`), verifies that `S` is connected to `conjunction` via `~`, and if so, treats the elements of `S` as the condition patterns.

This means that **a rule is completely represented by standard subject-predicate-object triples**, with `=>` serving as a standard predicate.

## Facts and Rules in One Network: Unique Identification via Topological Semantics

A distinctive aspect of **zelph** is that **facts and rules live in the same semantic network**. That raises a natural question: how does the unification engine avoid confusing ordinary entities with statement nodes, and how does it keep rule matching unambiguous?

The answer lies in the network's **strict topological semantics** (see [Internal Representation of facts](#internal-representation-of-facts) and [Internal representation of rules](#internal-representation-of-rules)). In zelph, a _statement node_ is not "just a node with a long label"; it has a **unique structural signature**:

- **Bidirectional** connection to its **subject**
- **Forward** connection to its **relation type** (a first-class node)
- **Backward** connection to its **object**

The unification engine is **hard-wired to search only for this pattern** when matching a rule's conditions. In other words, a variable that ranges over "statements" can only unify with nodes that expose exactly this subject/rel/type/object wiring. Conversely, variables intended to stand for ordinary entities cannot accidentally match a statement node, because ordinary entities **lack** that tri-partite signature.

Two immediate consequences follow:

1. **Unambiguous matching.** The matcher cannot mistake an entity for a statement or vice versa; they occupy disjoint topological roles.
2. **Network stability.** Because statementhood is encoded structurally, rules cannot "drift" into unintended parts of the graph. This design prevents spurious matches and the sort of runaway growth that would result if arbitrary nodes could pose as statements.

## Performing Inference

By default, zelph triggers the inference engine immediately after every fact or rule is entered. You can toggle this behaviour using the `.auto-run` command.

**Performance Note:** When working with large datasets, continuous inference can be computationally expensive. Therefore, the `.load` command automatically **disables** auto-run mode to ensure efficient data loading. You can re-enable it manually at any time by typing `.auto-run`.

Queries containing variables (e.g., `A "is capital of" Germany`) are always evaluated immediately, regardless of the auto-run setting.

If auto-run is disabled, you can trigger inference manually:

```
.run
```

This executes complete inference: rules are applied iteratively until no new facts can be derived. Newly discovered deductions are printed as soon as they are found. A rule that generates nodes – via a fresh variable, a nested term, or a collection literal in its consequence – can prevent a run from concluding; refer to [Fresh Variables](logic.md#fresh-variables-generative-rules).

For a single inference pass:

```
.run-once
```

To record everything a run derived, for further processing:

```
.run-export <file>
```

See [Exporting Derivations](#exporting-derivations). For normal interactive
or script use, `.run` is the standard command.

### What the run summary counts

A run ends with a summary line like `Reasoning summary: N matches processed, M contradictions found.` A match refers to a binding provided by the unification search for a positive fact condition within a rule, either by scanning the facts that the condition can match or from the new fact that seeds an iteration of semi-naive evaluation. A pass that performs scanning tallies the bindings of each such condition across every rule it applies. Such passes are the solitary pass in `.run-once`, the initial pass in `.run`, each pass when `.semi-naive off` is active, and the safety pass appended by `.semi-naive check`. Semi-naive evaluation also repeats its first pass whenever a rule has derived a new rule, ensuring the new rule encounters facts older than itself (refer to [Rules That Write Rules](rule-generators.md)), and it still takes classic passes for two rule types: one containing a negated condition, and one whose conditions cannot be fully seeded due to the presence of a path or a `≈` condition, a nested condition set, or a condition without exactly one predicate (see [Reasoning incrementally](janet.md#reasoning-incrementally)). A binding is counted at the moment it is issued, regardless of whether a subsequent condition or the deduction later discards it, and it is counted again each time a later pass delivers it anew. Negated conditions `¬(…)`, `!=` guards, path conditions (`P⁺`, `P∗`) and neural `≈` conditions contribute nothing to the count.

The figure thus quantifies effort, not results. It is comparable across runs within a single evaluation mode, but not across `.semi-naive on`, `off`, and `check`, and under `.parallel` it can vary between two runs of identical input.

### Deduction Output Modes

zelph performs forward chaining: every derivable consequence is materialized
in the graph (see [Logic and Computation](logic.md#positioning-forward-chaining-over-graphs)).
For rule libraries that implement computations — the arithmetic modules are
the prime example — this is a double-edged sword: a single input like
`&10 - &3` triggers a long cascade of internal derivations (recursion
states, canonicalization steps) that are essential to the computation but
rarely interesting to read. In a goal-driven system like Prolog this
question does not arise, because only the proof of the asked goal is ever
constructed; in a forward chainer, filtering the _trace_ is the natural
counterpart.

The `.deductions` command controls which derived facts are printed, and what a
run says about the ones it held back:

    .deductions all      # print every deduction (full derivation trace)
    .deductions focus    # print only deductions about your input, and say how many were hidden
    .deductions quiet    # the same, but marked in the prompt instead of said (default)
    .deductions off      # print no deductions

In `focus` and `quiet` mode, a deduction is printed when its subject stems from a statement of the **session**: the subject is the entered fact itself, or its subject, or one of its objects. The session is everything you type, everything piped in, and every line of a script named on the command line (`zelph script.zph`) – see [Scripts and Modules](modules.md). Anchors accumulate over the session, so a rule entered later still surfaces conclusions about earlier inputs. A **module** loaded with `.import` contributes no anchors – a loaded arithmetic library stays silent about its internals.

**It is deterministic what gets derived, with three exceptions; how it appears in output is not fully deterministic.** Executing identical input twice produces identical facts and identical responses, save for three scenarios. One involves rules that negate their own outcomes, which `.strata` identifies as non-stratifiable (refer to [Stratified Evaluation](logic.md#stratified-evaluation)): here, the outcome may vary based on evaluation sequence, and such sequence can differ across runs. The second occurs when a rule includes a [fresh variable](logic.md#fresh-variables-generative-rules) under `.parallel`, which is active by default: whether a firing reuses an existing node as its witness or creates a new one depends on the sequence of match processing, so distinct runs may derive different facts. In a program lacking negation, `!=`, and `≈`, the two results still align up to homomorphism. The third case is a single pass, `.run-once` or `(zelph/run-once)`, under `.parallel`: it halts before reaching the fixpoint, and the extent of progress depends on whether a match encounters facts derived earlier by other matches in the same pass. With `.parallel` enabled, two runs of the same input may also diverge in the sequence of deduction lines, the sequence of answers to a single query, the assignment of bindings to two variables that could be swapped, the premises named after ⇐ for a fact that can be reached through multiple derivations (the one actually printed corresponds to the derivation that produced it), the timing of when a growing `@{…}` is printed, and the [number of matches](#what-the-run-summary-counts) reported in the run summary. With `.parallel` disabled, two runs output identical deductions in identical order. The transcripts presented in this documentation are actual executions; interpret each as one possible execution path.

The filter affects printing exclusively: **all facts are derived and stored regardless of the mode**, and query answers and warnings are always printed. A contradiction’s `!` line appears in every mode except `off`, regardless of its subject; `off` outputs solely the line `Found one or more contradictions!`. If a result you are interested in is not shown, query it (e.g. `&7 > X`) or switch to `.deductions all`. As a side effect, heavy computations run several times faster in every mode but `all`, because rendering large derived terms dominates the cost.

### What a filtered run tells you

Two things a run can do about what it hid, and the mode decides which. It can say so, once, at the end of the run and with the run’s total, or it can put a `+` in the prompt and leave it at that. The default is the second, because the count is worth a mark and not a sentence in the middle of the answer you asked for.

In `focus` and in `off` it is the sentence:

```
Note: 130 deductions were hidden.
```

The notice is printed on the same channel as the deduction lines it accounts for. That matters for a recorded transcript: redirecting only standard output used to keep the incomplete derivation and drop the sentence saying it was incomplete, so a log a year later could not be told from a complete one. For the same reason it carries the run’s total rather than the count since the last printed line – a reader comparing a paper against a log has one number to compare, not several to find and add.

What the notice does not do is explain the mode. It is only ever read in a mode you chose, and telling you after every answer how to reach the mode you are already in is what made the default worth changing. The explanation belongs where the choice is made, so `.deductions` gives it there.

A deduction written to a file by `.run-export` is not withheld and is not counted: the caller asked for a file, not for lines.

### The mark in the prompt

In the default mode a run says nothing and the prompt carries a `+` while the
most recent one held something back – this import is already such a run,
since the module’s derivations are not about anything you entered:

```
zelph> .deductions quiet
Deduction printing mode: quiet
  Only derivations about statements you entered are printed, and a run that hid
any marks the prompt with '+' (e.g. "zelph+> ").
zelph> .import math-syntax
zelph+> $(53 * 12) = X
(&53 * &12) = X
((&53 * &12) = &636) ⇐ {((&53 mul &12) prod &636) (:canonnum &636) (&53 * &12)}
zelph+>
```

The mark is the character `+`, and it is the fact rather than the count: a number in a prompt invites being read as a running total, and this is a per-run figure. It clears again after a run that hid nothing, and it sits beside the `-` that marks a disabled auto-run, so a session with both reads e.g. `zelph+-> ` – where the part in front of the marks is the current language and changes with `.lang`.

`off` carries the same mark and keeps its notice, because there the count is the only thing a run still tells you – which is what makes it usable for the large runs on the [class hierarchy](class-hierarchy.md) page.

`zelph script.zph` prints no prompt, so there `quiet` falls back to the notice. A mark with nowhere to appear would leave a log that does not say it is incomplete, which is the one thing the notice exists to prevent.

## Exporting Derivations

`.run-export <file>` performs full inference like `.run` and writes what THAT
run derives, plus every contradiction it meets, to `<file>` — one JSON object
per line (JSON Lines):

```json
{"kind":"deduction","conclusion":[SEG,...],"premises":[[SEG,...],...]}
```

A `SEG` is either a JSON string — literal text of the rendering, brackets
and spacing — or one of

```json
{"names":{"wikidata":"Q5","en":"human"}}
{"core":"!"}
```

the first naming one node in every language it is known by, the second one
of zelph's own vocabulary (`!`, `~`, `=>`, …).

Two properties are worth spelling out, because they are the point of the
format:

- **Nothing in it is about a target format.** Which of a node's names to
  display, which of them is a URL, whether identifiers should be
  italicised, which file a line belongs in — those are decisions of the
  consumer. zelph does not know about Wikidata, and it does not know about
  MkDocs either.
- **The premises are separate.** The console prints the condition _set_,
  `⇐ {(a p b) (b p c)}`, because that set is what the rule's subject is.
  The export hands over its elements, so no one has to take braces apart
  again.

Two more, which decide how the file may be counted:

- **A derivation already present in the graph is not re-derived, hence it is not written.** Deductions are hash-consed: a fact that is already established does not trigger a new deduction for export. On a saturated network – one that has already undergone a `.run` – the deduction side of the file is therefore EMPTY, yet the command still exits as if it had worked. Export from the run that does the deriving, or begin with `.new`. This same property means that only the FIRST derivation of any fact is ever written: a subsequent instantiation, whether from the same rule or a different one, that arrives at the same conclusion adds no additional entry, meaning the file contains one justification per fact rather than every single derivation. Under `.parallel`, the order in which instantiations occur varies across runs, affecting the premises listed in the file; conclusions remain consistent unless the program includes non-stratifiable rules or a rule involving a fresh variable (refer to [Deduction Output Modes](#deduction-output-modes)). With `.parallel` off, two runs generate identical files.
- **Contradictions are the deliberate exception, and they are duplicated.** A contradiction is written each time a run meets it, ensuring that a second `.run-export` does not return an empty file – implying that the same violation may appear on multiple lines. Thus, counting violations requires deduplicating based on the premise set, regardless of order, rather than simply counting lines.

A contradiction record carries one more field when the engine **refused** to
build the deduced fact — a shape it cannot represent — rather than finding the
knowledge base contradictory. Both stop the rule, and both are reported as
`!`, thus the reason is what tells them apart (one line of the file, wrapped
here for reading):

```json
{"kind":"contradiction",
 "conclusion":[{"core":"!"}],
 "refused":"a set constant cannot be extended -- {a b} IS its members. Write the collection literal @{...} for a container that membership can grow.",
 "premises":[["(",{"names":{"zelph":"q"}}," ",{"names":{"zelph":"p"}}," ",{"names":{"zelph":"r"}},")"]]}
```

The field is absent on a contradiction of the data, so counting those means
counting the records that do **not** have it.

Deduction printing is off during the run: rendering large derived terms
dominates the wall-clock time, and the file is the point.

```
zelph> .lang wikidata
wikidata> .auto-run
Auto-run is now disabled.
wikidata-> Q1 P279 Q2
Q1 P279 Q2
wikidata-> Q2 P279 Q3
Q2 P279 Q3
wikidata-> (*{(A P279 B) (B P279 C)} ~ conjunction) => (A P279 C)
((B P279 C), (A P279 B)) => (A P279 C)
wikidata-> .run-export /tmp/derivations.jsonl
Running full inference; derivations are written to /tmp/derivations.jsonl as
JSON Lines.
Starting reasoning with 24 worker threads.
Reasoning complete. Total unification matches processed: 5. Total contradictions
found: 0.
Reasoning summary: 5 matches processed, 0 contradictions found.
Parallel unifications activated for 1 distinct fixed relations.
Reasoning complete in 0h0m0.000s – 5 matches processed, 0 contradictions found.
Ready.
```

Content of `/tmp/derivations.jsonl` (one line, wrapped here for reading):

```json
{"kind":"deduction",
 "conclusion":["(",{"names":{"wikidata":"Q1"}}," ",{"names":{"wikidata":"P279"}}," ",{"names":{"wikidata":"Q3"}},")"],
 "premises":[["(",{"names":{"wikidata":"Q2"}}," ",{"names":{"wikidata":"P279"}}," ",{"names":{"wikidata":"Q3"}},")"],
             ["(",{"names":{"wikidata":"Q1"}}," ",{"names":{"wikidata":"P279"}}," ",{"names":{"wikidata":"Q2"}},")"]]}
```

### Converting the export

`dev_scripts/zelph-derivations.py` is the reference converter, and the two
formats it writes are what the reports on zelph.org are built from:

```bash
# The MkDocs tree behind the reports on https://zelph.org: one page per
# Wikidata identifier occurring in a conclusion, with links between pages.
dev_scripts/zelph-derivations.py /tmp/derivations.jsonl --format md --out mkdocs/docs/report

# One flat line per derivation, premises first.
dev_scripts/zelph-derivations.py /tmp/derivations.jsonl --format text --out /tmp/derivations.txt
```

```
(Q2 P279 Q3), (Q1 P279 Q2) => (Q1 P279 Q3)
```

The `text` form is also the starting point for tokenizer-friendly training
data. Long numeric identifiers (`Q123456789`) are expensive for standard
tokenizers, which split them into many sub-tokens; because every identifier
arrives in the export as a discrete token rather than as a substring of a
sentence, substituting a compact encoding for it is a dictionary lookup and
not a parse. zelph ships no such encoding: which one fits is exactly the kind
of decision that belongs to the consumer of the data.

## Node Clusters: Transactional Workspaces

When experimenting on a large loaded network — say, a full Wikidata dump — you often want to undo an entire experiment without reloading everything. Clusters provide exactly that:

```
.cluster my-experiment
... enter facts and rules, .run ...
.cluster-drop my-experiment      # roll back everything the experiment created
```

While a cluster is active, every node created is recorded in it: entities, relation nodes, rule definitions, the variables those rules are made of, and facts deduced by `.run`. Facts that already existed beforehand are never recorded, so dropping a cluster can never destroy pre-existing knowledge — a fact from before the cluster cannot name a node the cluster created. One change to a pre-existing node is undone all the same: claiming a statement that was only a rule's ground pattern revokes that marking, and the drop restores it, so an experiment cannot turn a rule's patterns into data for good. The line is that a marking is the engine's own bookkeeping about a node rather than a claim anybody made; names and merges stay outside it. What a drop does take, beyond its own nodes, is anything BUILT on one of them afterwards: a fact entered outside the cluster that names a cluster node goes with it, and so does a rule such a fact is a condition of. A fact that had merely lost a part would not be recognisable as incomplete (`.help .remove` explains why), so the reported count is what actually went, which can exceed what the cluster recorded. `.cluster-merge <from> <to>` commits a cluster's bookkeeping into another one (merging into `default` simply turns its nodes into ordinary nodes), and `.cluster default` deactivates tracking. Clusters are session state and are not persisted by `.save`.

The [neural network demo](neural.md) uses a cluster so that the entire experiment — layers, synapses, rules, and all deductions — can be removed with a single command, leaving the loaded dump untouched.

A second use is taking a demonstration back out of the graph whole – the
facts, the rule and the record together. A contradiction provoked on purpose
is announced once (see
[Contradiction Detection](logic.md#contradiction-detection)), and dropping the
cluster removes what caused it:

```
.cluster demo
:isprime &9                      # provokes the contradiction, once
.cluster-drop demo               # ... and takes it back out again
```

(`.prune-facts` on the offending fact does the same job when a cluster is
too coarse. Either way the record goes with the facts it is about, so the
same contradiction is a fresh finding if they ever return.)
