# Rules That Write Rules

A consequence in zelph does not have to be a fact.
It can be a **rule** — and then the outer rule is a _rule generator_: firing it writes a new rule into the graph, which the engine picks up and applies within the same run.

```
(R is transitive) => ((X R Y, Y R Z) => (X R Z))
```

Read it as it stands: _whatever is transitive, chains_.
Declaring `before is transitive` is then an ordinary fact about an ordinary node — and it leaves a transitivity rule for `before` behind, quantified over data the declaration says nothing about.

This page is about what that buys, where the idea comes from, and where its edges are.
The syntax and the two smallest examples are in [Logic and Computation](logic.md#rules-that-derive-rules); everything here builds on them.

## Where the idea comes from

Logic has had this construct for a century, under the name [**axiom schema**](https://en.wikipedia.org/wiki/Axiom_schema).
A schema is not an axiom but a _template_ standing for a whole family of them, one per formula you substitute — the [induction schema](https://en.wikipedia.org/wiki/Peano_axioms#Peano_arithmetic_as_first-order_theory) of Peano arithmetic and the [separation schema](https://en.wikipedia.org/wiki/Axiom_schema_of_specification) of set theory are the classic cases.
Both exist because first-order logic cannot quantify over formulas, so the quantification is moved outside the theory, into the metalanguage that describes it.

A rule generator is the same move, with two differences that matter:

- The metalanguage is the object language. The generator is a statement expressed in identical syntax, housed within the same graph, matched by the same engine that processes the rules it writes.
- The instances are **actual objects**, not metaphorical constructs. A rule produced through generation surfaces in `.list-rules`, carries its own justification, may be `.explain`ed, saved, loaded, and removed individually.

That is what distinguishes a generator from the [meta-rule](logic.md#meta-rules-predicates-as-first-class-nodes) `(R is transitive, A R B, B R C) => (A R C)`, which expresses the same closure by quantifying over the predicate at match time.
Both are available and both are correct; the meta-rule keeps one rule and re-decides the quantification on every match, the generator pays it once per declaration and leaves something behind that the rest of the system can see.

## One match, one rule

The generator matches the network wherever it can, and **each match fixes the variables of one new rule**.
Two matches that fix them the same way must not produce two rules, and nothing collapses them by itself: the variables of a rule are nodes of their own, and a rule's condition set is created rather than [hash-consed](logic.md#the-executable-graph).
Deduplication is therefore explicit, and it is exact — the instantiated conditions and consequences are hash-consed, so a re-derivation lands on the very same nodes.

```
zelph> .deductions all
Deduction printing mode: all
  Every derivation is printed.
zelph> (A knows B) => ((X p B) => (X q B))
zelph> tom knows red
((X p red) => (X q red)) ⇐ (tom knows red)
zelph> sue knows red
zelph> ann knows blue
((X p blue) => (X q blue)) ⇐ (ann knows blue)
zelph> .deductions off
Deduction printing mode: off
  No derivations are printed; a run says how many it hid and marks the prompt
with '+' (e.g. "zelph+> ").
zelph> .list-rules
Listing all rules:
------------------------
(X p red) => (X q red)
(X p blue) => (X q blue)
(A knows B) => ((X p B) => (X q B))
------------------------
```

`sue knows red` is a second match and fixes `B` the same way, so it adds nothing.
`ann knows blue` fixes it differently and gets a rule of its own.
Note also what did _not_ happen: `X` is bound by nothing, so it came through as a variable — the generator produced a rule, not one instance of one.

## Worked example: order-theoretic properties

The properties that make a relation an [order](https://en.wikipedia.org/wiki/Partially_ordered_set) are statements _about_ the relation.
Written as generators, declaring one relation to be a partial order installs the [transitivity](https://en.wikipedia.org/wiki/Transitive_relation) and [antisymmetry](https://en.wikipedia.org/wiki/Antisymmetric_relation) rules for it:

```
zelph> .deductions all
Deduction printing mode: all
  Every derivation is printed.
zelph> (R is partialorder) => ((X R Y, Y R Z) => (X R Z))
zelph> (R is partialorder) => ((X R Y, Y R X) => (X sameas Y))
zelph> divides is partialorder
(((X divides Y), (Y divides Z)) => (X divides Z)) ⇐ (divides is partialorder)
(((X divides Y), (Y divides X)) => (X sameas Y)) ⇐ (divides is partialorder)
zelph> two divides four
zelph> four divides eight
(two divides eight) ⇐ {(two divides four) (four divides eight)}
```

Two declarations of a property, one declaration of a relation, and the closure runs.
Adding `contains is partialorder` next installs a second, independent pair of rules; the two never mix, because each generated rule has the predicate baked into it.

This is the shape [universal algebra](https://en.wikipedia.org/wiki/Universal_algebra) takes when it is executed: an algebraic structure is a carrier plus a list of laws, and the laws are exactly what a generator turns into rules.

## Worked example: a logic as data

A [normal modal logic](https://en.wikipedia.org/wiki/Normal_modal_logic) is named by the axiom schemas it accepts — **T** is `□A → A`, **D** is `□A → ◇A`, and so on.
Written as generators, _which schemas a system accepts_ becomes ordinary data, and each system gets its own inference rules:

```
zelph> .deductions all
Deduction printing mode: all
  Every derivation is printed.
zelph> (S accepts axiomT) => ((A necessaryin S) => (A holdsin S))
zelph> (S accepts axiomD) => ((A necessaryin S) => (A possiblein S))
zelph> kt accepts axiomT
((A necessaryin kt) => (A holdsin kt)) ⇐ (kt accepts axiomT)
zelph> kd accepts axiomD
((A necessaryin kd) => (A possiblein kd)) ⇐ (kd accepts axiomD)
zelph> p necessaryin kt
(p holdsin kt) ⇐ (p necessaryin kt)
zelph> q necessaryin kd
(q possiblein kd) ⇐ (q necessaryin kd)
```

`kt` and `kd` reason differently, from the same graph, because their rules were written by the declaration that named them.
Adding a system is a fact; adding a schema is a rule.
Neither requires touching the engine, and both are visible to it — a further rule can ask which systems accept **T**, or refuse a combination as contradictory.

## Worked example: the axioms of an ontology

The six property axioms an ontology is usually _described_ with — transitive, symmetric, sub-property, sub-class, [domain and range](https://en.wikipedia.org/wiki/RDF_Schema) — are six generators.
Every declaration a modeller writes afterwards is ordinary data and produces its own specialised rule.
The worked transcript is in [Logic and Computation](logic.md#rules-that-derive-rules); `src/test/test_derived_rules.cpp` carries it as a test, where the closure is countable by hand: twelve rules, seven derived facts, and one conclusion that arrives through a chain of three generated rules.

## Generators that generate generators

Nothing stops a generated rule from being a generator itself.
The nesting is written with parentheses, which the parser [requires](logic.md#basic-rules-and-conjunction) rather than guesses at, and it executes to any depth:

```
zelph> .deductions all
Deduction printing mode: all
  Every derivation is printed.
zelph> (G go H) => ((H is on) => ((P entails Q) => ((X P Y) => (X Q Y))))
zelph> now go k
((k is on) => ((P entails Q) => ((X P Y) => (X Q Y)))) ⇐ (now go k)
zelph> k is on
((P entails Q) => ((X P Y) => (X Q Y))) ⇐ (k is on)
zelph> parent entails ancestor
((X parent Y) => (X ancestor Y)) ⇐ (parent entails ancestor)
zelph> a parent b
(a ancestor b) ⇐ (a parent b)
```

Four levels: the outermost rule writes a rule that writes a generator that writes the rule which finally fires on `a parent b`.
Read outwards, that is a switch over a schema over a schema — the shape of a configurable rule _library_, where what a deployment enables is a fact rather than a build option.

## What stays the same

**Termination is a property of the rules, not of who wrote them.**
A generated rule terminates exactly when the same rule typed by hand would.
The shape to watch for is one whose consequence builds a term out of its own condition, because it then feeds itself:

```
(F respects R) => ((X R Y) => ((F of X) R (F of Y)))
```

Stating `succ respects sameparity` sets up a rule that derives a fact concerning `(succ of a)`, then proceeds to `(succ of (succ of a))`, and continues indefinitely. This is [congruence](https://en.wikipedia.org/wiki/Congruence_relation) stated without a bound, and it is the generated rule that runs away; manually writing it out for `succ` yields the same behaviour. A rule whose collection literal flows back into its own binding represents another such shape (see [A literal a rule derives](concepts.md#a-literal-a-rule-derives)).

**Generation itself terminates.** A generator creates nodes solely as components within the rules it writes: each rule it produces gets collections of its own where the inner rule has collection literals, and the variables within that rule are quantified by it, ensuring the [fresh-variable](logic.md#fresh-variables-generative-rules) mechanism does not affect them. What a generator is capable of is issuing one rule per match, and the matches come from a fact base that only rules can expand – thus, a runaway generator requires a rule that supplies it, precisely as any other rule does.

**A generated rule is an ordinary rule.** It survives `.save` and `.load`, `.explain` rebuilds justifications via it, `.list-rules` shows it, and `.remove` takes it – yet the generator will recreate it on the next run, since a consequence cannot be deleted while its premise persists. Much like a typed rule, merely writing it does not make an assertion: a condition or consequence that becomes ground through substitution remains a rule pattern until something asserts or derives it. Similarly to a typed rule, one that keeps a variable constitutes graph structure rather than data: no other rule’s condition matches it, meaning a rule can reason about the declaration that wrote it, but not about the rule itself (refer to [Claimed or Merely Written Down](janet.md#claimed-or-merely-written-down)). It is created within whichever [cluster](rules.md#node-clusters-transactional-workspaces) is currently active, thus `.cluster-drop` reverts it alongside the rest of an experiment.

## Reference: what is substituted

When a generator fires, the rule it writes is rebuilt under the bindings of that match:

| Part of the inner rule | What happens |
| --- | --- |
| a variable that the generator’s conditions had bound | substituted with the node it was bound to |
| a variable that they did not bind | stays a variable – it is quantified by the inner rule |
| a condition set | rebuilt as a set of its own, ensuring the new rule possesses its own conjunction |
| a negated condition | stays negated; the `¬` tag is reinserted into the rebuilt pattern |
| a `!=` guard | rebuilt in the same manner as any other condition |
| `!` as the consequence | rebuilt, enabling a generator to introduce a contradiction check |
| a container, `{...}` or `@{...}` | rebuilt with its substituted members; a collection becomes one of the new rule’s own, the one a consequence writes **into** included |

A container follows the renaming that a generator performs on the rule it
writes, so the generated rule behaves like the same rule typed by hand:

```
zelph> k is on
zelph> a p b
zelph> c p d
zelph> (K is on) => ((X p Y) => (X likes {Y}))
((X p Y) => (X likes @{Y})) ⇐ (k is on)
(a likes {b}) ⇐ (a p b)
(c likes {d}) ⇐ (c p d)
```

A collection **into** which a consequence writes is likewise reconstructed, ensuring that each rule authored by a generator accumulates within a distinct collection:

```
zelph> t go k
zelph> u go m
zelph> (G go H) => ((X G Y) => (Y in @{H}))
(G go H) => ((X G Y) => (Y in @{Y H}))
((X u Y) => (Y in @{m Y})) ⇐ (u go m)
((X t Y) => (Y in @{k Y})) ⇐ (t go k)
zelph> a t bug1
(bug1 in @{k bug1}) ⇐ (a t bug1)
zelph> b u bug2
(bug2 in @{m bug2}) ⇐ (b u bug2)
```

Each rule produced by the generator writes into the term of its own collection, just as the same rule manually authored does (refer to [Collections](concepts.md#collections)). However, the node to which a generator’s variable is bound presents a distinct case: a collection within the data is a node like any other, and the rule generated by the generator names that exact node, so `(S has C) => ((X r Y) => (X in C))` applied to `d has @{x}` writes a rule that places what it derives into `@{x}` itself. Consult [Braces](concepts.md#braces-set-constants-and-collections) for details on the two literals.

A collection of another rule's text is not such a node. A generator can bind one – via rule structure, as `(G => (S q C))` binds the collection of a ground rule, or through an engine marking fact – yet a rule's text remains immutable after the rule is written, so the rule generated names the **data term** of the collection instead: the collection that the bound rule's firings produce for it. The bound rule keeps the text it was written with, and when re-typed, it remains the same rule:

```
zelph> (a p b) => (c q @{d})
zelph> (G => (S q C)) => ((X r Y) => (X in C))
((X r Y) => (X in @{})) ⇐ ((a p b) => (c q @{d}))
zelph> e r f
(e in @{e}) ⇐ (e r f)
zelph> S in O
Answer: e in @{e}
zelph> a p b
zelph+> S in O
Answer: d in @{d e}
Answer: e in @{d e}
zelph> c q O
Answer: c q @{d e}
```

The data term contains only what has been derived into it, and holds nothing prior: `e r f` detects it as empty, while the firing of `(a p b) => (c q @{d})` adds `d`. Three configurations arise from replacing the bound collection with its data term. A rule restated from its bound components, `(G => (S q C)) => (G => (S q C))`, is a second rule, operating on the data term, beside the initial one; under a switch, `(G => (S q C)) => ((s is on) => (G => (S q C)))`, the restated rule leaves the typed one in force. Furthermore, the data term of a collection governed by a rule containing variables, like `@{d Y}` in `(X p Y) => (X likes @{d Y})`, is the term under no binding, which none of the rule’s firings ever writes: a rule that binds such a collection via a marking fact writes where the rule never looks.

While empty, the data term prints as `@{}` in every context it stands: the rule produced above reads `(X r Y) => (X in @{})` until some content is derived into the term, and given `(X likes C)` as its consequence, `e r f` derives `e likes @{}`. Typed back, `@{}` is `nil` (refer to [Braces](concepts.md#braces-set-constants-and-collections)), thus such a line re-enters as a statement concerning `nil`: no literal refers to any existing collection, not even the empty one.

A conjunction set that a generator binds represents its data term identically at every location where the generated rule writes a membership into it, and is the bound rule's conditions in all other contexts, as illustrated by `(C => H) => ((X r Y) => (C noted yes))`. A condition likewise constitutes such a membership: in `(C => (e f g)) => ((X in C) => (X flagged yes))`, the generated rule's condition reads the set's data term, which a different generated rule might write into, and the typed rule `(a p b, c q d) => (e f g)` keeps its two conditions and continues to fire. Had `X in C` been written directly into the set, it would have become a third condition of the typed rule. A firing that writes into a conjunction set simultaneously writes into its data term as well, even in cases where the generated rule holds the set itself, due to the relation of the membership having remained a variable at the time the rule was written, as in `(X R C)`.

The one shape that cannot be told apart is a fully **ground** inner rule that is also merely mentioned somewhere: [hash-consing](logic.md#mentioning-a-rule-is-not-asserting-it) makes those a single node, and the graph carries no evidence of which was meant.
A rule with variables is two nodes, because every statement names its own variables — which is why the generator can assert a rule that is written out, unasserted, elsewhere.
