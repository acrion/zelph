# About the Project

## Project Status

The core functionality has undergone rigorous testing against the full Wikidata dataset and is operational.
Comprehensive automated tests run on every commit – see the [test suite definition](https://github.com/acrion/zelph/blob/main/src/test/CMakeLists.txt). Contributor-facing documentation of the engine's internal performance architecture — and of the measurement methodology used to develop it — lives
in the [Internals](internals/performance.md) section.

Current focus areas include:

- **Graph-based arithmetic and symbolic mathematics**: not a prototype but a fully realized system resides in the standard library, each layer constructed using ordinary zelph rules: positional arithmetic over interchangeable digit substrates – one of which derives its entire digit level from a single NAND axiom – followed by signed integers, multivariate polynomial normal forms over ℤ, a terminating term simplifier, symbolic differentiation, and a compiler that decides polynomial identities through node identity. It proves Euler’s four-square identity in a fifth of a second, and reproduces the July-2026 counterexample to the [Jacobian conjecture](math/tutorial-jacobian.md) – nine symbolic partial derivatives and a 3×3 determinant over ℤ – in less than two seconds. Every result includes a proof reconstructible down to the digit tables. Refer to [Mathematics](math/index.md); the comparison with [Lean](https://lean-lang.org) remains valid, except that here the foundation is a graph-native, homoiconic representation rather than a type theory.
- **Transitive reasoning and Wikidata integration**: A [second Wikimedia Rapid Fund project](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Transitive_Reasoning,_Qualifier_Support,_and_SPARQL-Subset_Integration_(ID:_23759260)>) delivered transitive reasoning across Wikidata’s subclass hierarchy ([native closures with a cached adjacency index](sparql.md#performance-and-the-adjacency-index)), [qualifier support](qualifiers.md), and [SPARQL-subset integration](sparql.md) in release 0.9.6 – capabilities that also function as foundational components for broader symbolic computation. The next frontier it enables is property-constraint validation contingent on qualifiers.
- **Neural networks in the graph**: Starting with version 0.9.7, zelph embeds a neural substrate directly within the semantic network – raw weighted edges as synapses, layers as ordinary sets, and rule conditions that consult trained networks using the `≈` operator. See [Neural Networks in the Graph](neural.md).
- **Potential Wikidata integration**: Investigating avenues for integration with the Wikidata ecosystem, such as the [WikiProject Ontology](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology).

Regarding potential Wikidata integration and the enhancement of semantic scripts, collaboration with domain experts would be particularly valuable. Expert input on conceptual alignment and implementation of best practices would significantly accelerate development and ensure optimal compatibility with existing Wikidata infrastructure and standards.

### Where the logic goes next

The reasoning engine has its own open directions, independent of the
mathematical stack built on top of it:

- **Negation over a group of conditions.** `¬` applies to one fact pattern.
  `¬(A, B)` — "not both" — is [rejected](logic.md#negation-as-failure)
  rather than approximated, because the honest implementation is a nested
  negation-as-failure over a conjunctive subgoal: run the subgoal's search
  and succeed only if it yields no binding. The interesting part is not the
  search but the [stratification](logic.md#stratified-evaluation): a
  negated group must be deferred exactly like a negated pattern, and its
  own conditions may themselves be negated. Until then, De Morgan plus
  several rules expresses the same thing.

- **More of Wikidata's property constraints as rules.**
  [`.wikidata-constraints`](wikidata.md#checking-wikidatas-own-constraints)
  turns two of the roughly forty constraint types Wikidata defines into
  rules; the rest are exported as commented JSON for a human to work from.
  Which ones to add next is not a question of how many statements a type
  covers, but of where a whole-graph rule engine can say something that a
  check evaluated one statement at a time cannot. Three that fit that
  description:

    - **Subject type and value-type constraints**
      ([Q21503250](https://www.wikidata.org/wiki/Q21503250),
      [Q21510865](https://www.wikidata.org/wiki/Q21510865)) — "the item
      must be an instance or subclass of X". The condition is the
      transitive closure of `P279`, which a per-statement check has to walk
      on demand and therefore has to bound; zelph materialises the closure
      once and reasons over it, which is the same machinery the
      [class-hierarchy work](wikidata.md) already runs at full-dump scale.
      Long chains and deep classes are exactly the part that goes
      unexamined today.
    - **Contemporary constraint**
      ([Q25796498](https://www.wikidata.org/wiki/Q25796498)) — subject and
      object have to have coexisted. Deciding it means reading dates off
      two different items, possibly from qualifiers, and comparing them.
      That is a join plus arithmetic rather than a lookup, and zelph has
      both [in the graph](math/index.md) — with the
      [qualifier layer](qualifiers.md) supplying the dates.
    - **Inverse constraint**
      ([Q21510855](https://www.wikidata.org/wiki/Q21510855)) — the value
      has to point back with the inverse property. Here the interesting
      output is not the violation but the fix: the same rule that reports
      the asymmetry also *derives* the missing statement, so what comes out
      is a list of edits rather than a list of complaints.

  Each of these needs a generator in `wikidata.cpp` and, for the first two,
  a small amount of vocabulary in the standard library. Input on which of
  them the ontology community would actually use is more valuable than the
  implementation.

- **Queries that leave no trace.** A query is written into the graph just like any other element: `S p O` adds its two variables and its pattern, a two-triple SPARQL `SELECT` nine nodes, and no mechanism removes them afterwards. The expense extends beyond memory usage. Each query reads every pattern previously deposited on its predicate by earlier queries, meaning that reissuing the same question repeatedly becomes progressively slower, with the cumulative time increasing quadratically. Three solutions are open: an index capable of identifying a query with a shape it has already answered, a cache keyed by the query’s textual form, or evaluating each query within a scratch area that is erased alongside the answer, as `.explain` already does for its internal patterns.

- **An integrity check.** A node is the hash of its structure, meaning a fact whose identifier does not align with the hash of its predicate, subject and objects, or a single statement stored across several identical nodes, represents corruption that nothing detects today. A `.check` command would scan the graph for such inconsistencies and report the findings.

- **Every justification.** `.explain` reassembles a single derivation and marks it as `[one of several justifications]` if additional ones exist. Listing all of them is what a reader requires to know whether a conclusion remains valid after one of its premises is withdrawn.

- **A run that cannot end should say so.** A rule generating a collection that loops back into its own binding, like `(X p Y) => (X likes @{bucket})` together with `(X likes C) => (C p k)`, derives a fresh collection with each iteration: the rules precisely dictate this behaviour, and thus the run fails to terminate. Detecting the cycle through the same rule and stopping with an error that names it would turn a hanging run into a diagnosis.

- **A rule in force due to being asserted.** Today, a `=>` statement qualifies as a rule in force solely when no other element refers to it, meaning the validity of a rule is determined by its surroundings, and it evolves alongside that context: eliminating a statement that quoted a rule, or the generator that produced it, brings that rule into force, while a typed rule ceases to fire once something references it. A positive record, created at the moment a rule is asserted (via typing, via the API, or written by a firing), would make being in force an intrinsic attribute of the rule. Consequently, referencing a rule or removing what mentions it would no longer affect its validity, and removing a rule could then safely take along the rule text that was mentioned exclusively by it.

- **Quoting a rule.** A rule containing variables that a statement merely references, such as `alice proposes ((X is bad) => (alarm is on))`, is on record but cannot be retrieved via a query, since those variables are tied to the rule. Adopting a HiLog-style quotation would make such a statement answerable as data, matching rule text up to a renaming of its variables.

### Where the mathematics goes next

The mathematical stack will keep growing, and which way it grows should depend on what mathematicians actually want from it. Several directions are open, and none of them is committed:

- **Side conditions.** Identities in the symbolic layer are currently *formal*: `exp inverseof ln` silently assumes the principal branch and u > 0. Tracking conditions as ordinary facts — so that a rewrite carries its domain with it — fits the architecture, but the right granularity is a mathematical question, not an engineering one.
- **Beyond a single polynomial ring.** Ideal membership and Gröbner bases are the obvious next layer above the existing normal forms, and Buchberger's algorithm is a fixpoint computation — an unusually good match for forward chaining. Modular arithmetic and finite fields would be cheaper and would open number theory.
- **Rationals, and with them division.** The quotient rule is missing from differentiation for exactly one reason: there is no field to divide in yet.
- **One-shot normalisation.** Rewriting is single-pass per request today. Equality saturation in the e-graph style is, at heart, forward chaining over equalities — the natural experiment, and one the current design deliberately left room for.
- **Proofs that leave the system.** `.explain` already reconstructs a complete justification from the saturated graph – one of them, and it says so when there are several. Exporting it in a form another proof checker accepts would turn zelph's answers into externally verifiable ones.
- **Arithmetic that waits for numbers.** The arithmetic triggers fire upon any operand, and for a list that is not a numeral they leave intermediate states that lead nowhere. Limiting the firing to numerals alone would save that effort; this alters proof trees and the counts recorded by measurements, thus requiring a measurement first.
- **Knowledge about terms the simplifier rewrites.** A declaration concerning a term that simplification replaces, such as one regarding `(K * U)`, is not consulted once the term has been rewritten, meaning a second simplification run might yield a different outcome. Determining when knowledge folding should consult such declarations remains a design question.
- **One integer, one comparison.** Differentiation answers `(x ^ &2) diffby x` but does not answer the identical power expressed via the internal integer form `(x ^ (pos zint &2))`. Directly comparing that format with natural numbers would remove the asymmetry, albeit at a cost in the arithmetic façade.

If one of these matters to your work — or if the interesting direction is one not listed here — that is precisely the feedback that would shape the roadmap. Issues and discussions are on [GitHub](https://github.com/acrion/zelph).

## Project History

zelph has been in continuous development since 2012, when it began as a C# application called "NeoCortex" ([archived project page from 2012, in German](https://web.archive.org/web/20120826111106/http://www.zipproth.de/entwicklung_einer_neuartigen_inferenzmaschine.html)). The core idea was already the same: the inference engine is not external to the semantic network but part of it — rules are network structures, and the system can make statements about itself. The current C++ engine is a from-scratch realization of that idea.

## Funding and Collaboration

zelph is developed with support from the
[NGI0 Commons Fund](https://nlnet.nl/project/Zelph/) (NLnet Foundation) for
*Auditable Reasoning over Linked Open Data*, and has previously been supported
by two grants from the Wikimedia Community Fund (Rapid Fund):
[Wikidata Contradiction Detection and Constraint Integration](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Wikidata_Contradiction_Detection_and_Constraint_Integration_(ID:_23553409)/Final_Report>)
and
[Transitive Reasoning, Qualifier Support, and SPARQL-Subset Integration](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Transitive_Reasoning,_Qualifier_Support,_and_SPARQL-Subset_Integration_(ID:_23759260)/Final_Report>).

The project addresses real-world challenges in large-scale ontology management
through direct collaboration with the
[Wikidata Ontology Cleaning Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Cleaning_Task_Force)
and the
[Mereology Task Force](https://www.wikidata.org/wiki/Wikidata_talk:WikiProject_Ontology/Mereology_Task_Force).

## Use of generative AI

zelph was written by hand from 2012 until the middle of 2026. Since then a
substantial part of new code is produced with the help of a large language
model, working from the existing codebase under the author's direction. The
architecture, the design decisions and the acceptance of every change remain
his, and every line is reviewed before it enters the repository.

From release 1.0.0 onwards, a commit that adds or changes code carries a
provenance record in its message body: the model, what the commit does, the
instructions it was given, and what was chosen, arranged or reworked. The
format is new and will change as the practice settles.

Commits made after release 0.9.9 were produced the same way but carry no such
record, because the practice did not exist yet.

Development is funded in part by the NLnet Foundation, whose [policy on
generative AI](https://nlnet.nl/foundation/policies/generativeAI/) asks for
this disclosure.

## Licensing

zelph is dual-licensed:

1. **AGPL v3 or later** for open-source use,
2. **Commercial licensing** for closed-source integration or special requirements.

We would like to emphasize that offering a dual license does not restrict users of the normal open-source license (including commercial users).
The dual licensing model is designed to support both open-source collaboration and commercial integration needs.
For commercial licensing inquiries, please contact us at [https://acrion.ch/sales](https://acrion.ch/sales).
