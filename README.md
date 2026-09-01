# zelph - A Sophisticated Semantic Network System

[![CI Build](https://github.com/acrion/zelph/actions/workflows/ci.yml/badge.svg)](https://github.com/acrion/zelph/actions/workflows/ci.yml)
[![Documentation](https://img.shields.io/badge/docs-GitHub_Pages-blue.svg)](https://acrion.github.io/zelph/)
[![GitHub Release](https://img.shields.io/github/v/release/acrion/zelph?include_prereleases)](https://github.com/acrion/zelph/releases)
[![NGI0 Commons Fund](https://img.shields.io/badge/NGI0-Commons_Fund-1a1a1a.svg)](https://nlnet.nl/project/Zelph/)

**zelph** is an innovative semantic network system and reasoning engine written in C++ with an embedded Janet scripting layer. It treats logic, rules, and mathematics not as external code, but as **homoiconic structures within the graph itself**.

By blending the flexibility of semantic webs (like Wikidata) with logic programming concepts (deep unification, constructive rules, negation as failure), zelph effectively transforms a static knowledge base into an **executable graph**.

## 🎉 Selected for an NLnet NGI0 Commons Fund Grant

zelph has been selected for a grant from NLnet's
[NGI0 Commons Fund](https://nlnet.nl/project/Zelph/) for **Auditable Reasoning
over Linked Open Data** — streaming import adapters for RDF/N-Triples and
JSON-LD, an open proof-export format that publishes derivation chains as Linked
Data, a reusable constraint-rule library, and a web-based proof explorer.

Earlier work was funded by two Wikimedia Community Fund (Rapid Fund) grants:
[Wikidata Contradiction Detection and Constraint Integration](https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Wikidata_Contradiction_Detection_and_Constraint_Integration_(ID:_23553409)/Final_Report)
and
[Transitive Reasoning, Qualifier Support, and SPARQL-Subset Integration](https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Transitive_Reasoning,_Qualifier_Support,_and_SPARQL-Subset_Integration_(ID:_23759260)/Final_Report),
both completed and reported.

## Domain-agnostic by construction

The engine knows nothing about any application domain. It provides unification, forward chaining, stratified negation and a hash-consed graph — no arithmetic, no ontology semantics, no vocabulary. Everything a domain needs is written as ordinary zelph rules in `.zph` scripts, which means the same engine serves very different users:

- **[Wikidata](https://acrion.github.io/zelph/wikidata/)** — contradiction detection and constraint integration across the full knowledge graph, including [qualifier semantics](https://acrion.github.io/zelph/qualifiers/) and [sharded partial loading](https://acrion.github.io/zelph/sharding/) for graphs that do not fit in memory.
- **Legal ontologies** — [SensibLaw](https://github.com/chboishabba/SensibLaw), part of the [ITIR-suite](https://github.com/chboishabba/ITIR-suite), uses zelph as a downstream reasoning engine: it structures source material with full provenance and exports bounded graph slices to reason over.
- **Mathematics** — see below. Not a specialisation, but a demonstration of how far the general mechanism reaches.

## 📖 Documentation & Installation

The complete documentation, including tutorials, language references, and the architectural concepts behind "Semantic Math", is hosted on our documentation site:

👉 **[Read the zelph Documentation](https://acrion.github.io/zelph/)**

### Quick Links

- 🕹️ **[Try zelph in your browser](https://zelph.org/playground/)** – the complete reasoning engine as WebAssembly, no installation required. (Bleeding edge from `main`: [GitHub Pages playground](https://acrion.github.io/zelph/playground/).)
- 🚀 **[Installation & Quick Start Guide](https://acrion.github.io/zelph/quickstart/)** – Get started immediately with pre-compiled binaries for all major platforms (no build required).
- 🧠 [Core Concepts & Homoiconicity](https://acrion.github.io/zelph/#homoiconicity-the-executable-graph)
- 🧮 [Semantic Math & Rule-based Addition](https://acrion.github.io/zelph/logic/#semantic-math-computation-as-graph-rewriting)
- 📜 [Scripting with Janet](https://acrion.github.io/zelph/janet/)
- 🦀 [The Rust layer](https://acrion.github.io/zelph/rust/) – safe bindings for a host application, over the same C ABI any other language uses.
- 🗃️ [Use Case: Wikidata](https://acrion.github.io/zelph/wikidata/)

## Mathematics from the axioms up

Because the C++ core contains no arithmetic, the entire mathematical stack in `stdlib/` is written in zelph rules — digits, numerals, integers, polynomials, symbolic differentiation. Nothing is hard-coded and nothing is a built-in operator; a `*` is a graph node like any other, and its meaning is whatever the rules say it is. The domain-agnostic core is what makes this possible, so it is worth reading as evidence about the engine rather than as a feature of its own.

The [mathematics section](https://acrion.github.io/zelph/math/) covers this in two halves. Six tutorials build up from proving an identity to a full [refutation of the Jacobian conjecture](https://acrion.github.io/zelph/math/tutorial-jacobian/) — the July-2026 Alpöge/Fable counterexample, verified over ℤ by rules alone. A [reference](https://acrion.github.io/zelph/math/reference/) documents every module and predicate.

Every example in those pages was executed against the actual binary and its real output pasted in.

## Community and Support

Development of zelph is supported by the Wikimedia Community Fund, through two Rapid Fund projects: [Wikidata Contradiction Detection and Constraint Integration](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Wikidata_Contradiction_Detection_and_Constraint_Integration_(ID:_23553409)>) and [Transitive Reasoning, Qualifier Support, and SPARQL-Subset Integration](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Transitive_Reasoning,_Qualifier_Support,_and_SPARQL-Subset_Integration_(ID:_23759260)>).

The project addresses real-world challenges in large-scale ontology management through direct collaboration with the [Wikidata Ontology Cleaning Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Cleaning_Task_Force) and the [Mereology Task Force](https://www.wikidata.org/wiki/Wikidata_talk:WikiProject_Ontology/Mereology_Task_Force).
