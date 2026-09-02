# zelph

[![CI Build](https://github.com/acrion/zelph/actions/workflows/ci.yml/badge.svg)](https://github.com/acrion/zelph/actions/workflows/ci.yml)
[![Documentation](https://img.shields.io/badge/docs-GitHub_Pages-blue.svg)](https://acrion.github.io/zelph/)
[![GitHub Release](https://img.shields.io/github/v/release/acrion/zelph?include_prereleases)](https://github.com/acrion/zelph/releases)
[![License: AGPL-3.0-or-later](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)
[![NGI0 Commons Fund](https://img.shields.io/badge/NGI0-Commons_Fund-1a1a1a.svg)](https://nlnet.nl/project/Zelph/)

**zelph** is an innovative semantic network system and reasoning engine written in C++ with an embedded Janet scripting layer. It treats logic, rules, and mathematics not as external code, but as **homoiconic structures within the graph itself**.

By blending the flexibility of semantic webs (like Wikidata) with logic programming concepts (deep unification, constructive rules, negation as failure), zelph effectively transforms a static knowledge base into an **executable graph**.

Four lines are the entire concept:

```
zelph> "is part of" ~ "transitive relation"
zelph> (R ~ "transitive relation", X R Y, Y R Z) => (X R Z)
zelph> Canada "is part of" "American continent"
zelph> "American continent" "is part of" "Earth's surface"
(Canada "is part of" "Earth's surface") ⇐ {(Canada "is part of" "American continent") ("is part of" ~ "transitive relation") ("American continent" "is part of" "Earth's surface")}
```

The rule never mentions `is part of`. It quantifies over the relation itself, and the first line – an ordinary fact of the kind any dataset can carry – is what makes it apply here. Wikidata carries exactly this one, so this single rule covers every relation that Wikidata declares transitive.

## 📖 Documentation & Installation

The complete documentation, including tutorials, language references, and the architectural concepts behind "Semantic Math", is hosted on our documentation site:

👉 **[Read the zelph Documentation](https://acrion.github.io/zelph/)**

These pages are built from `main`, so they describe the current state of development and can be ahead of what you would download. The documentation belonging to the latest release lives on zelph.org instead:

[![Documentation for the latest release](https://img.shields.io/github/v/release/acrion/zelph?include_prereleases&label=docs%20for%20release&color=blue)](https://zelph.org/)

### Quick Links

- 🕹️ **[Try zelph in your browser](https://acrion.github.io/zelph/play/)** – the complete reasoning engine as WebAssembly, no installation required.
- 🚀 **[Installation & Quick Start Guide](https://acrion.github.io/zelph/quickstart/)** – Get started immediately with pre-compiled binaries for all major platforms (no build required).
- 🧠 [Core Concepts & Homoiconicity](https://acrion.github.io/zelph/concepts/#homoiconicity-the-executable-graph)
- 🧮 [Semantic Math & Rule-based Addition](https://acrion.github.io/zelph/logic/#semantic-math-computation-as-graph-rewriting)
- 📜 [Scripting with Janet](https://acrion.github.io/zelph/janet/)
- 🦀 [The Rust layer](https://acrion.github.io/zelph/rust/) – safe bindings for a host application, over the same C ABI any other language uses.
- 🗃️ [Use Case: Wikidata](https://acrion.github.io/zelph/wikidata/)
- 🛠️ [Contributing](CONTRIBUTING.md) – what a change to the engine has to bring with it.
- 🔒 [Security Policy](SECURITY.md) – how to report a vulnerability privately.

## Mathematics from the axioms up

Since the C++ core contains no arithmetic, the entire mathematical stack in `stdlib/` is implemented via zelph rules – digits, numerals, integers, polynomials, symbolic differentiation. [Six tutorials](https://acrion.github.io/zelph/math/) progress from proving an identity to a full [refutation of the Jacobian conjecture](https://acrion.github.io/zelph/math/tutorial-jacobian/) – the July-2026 Alpöge/Fable counterexample, verified over the integers using rules exclusively. Each example on those pages was executed against the actual binary and its real output pasted in.

## Funding and Collaboration

zelph is supported by NLnet’s [NGI0 Commons Fund](https://nlnet.nl/project/Zelph/) for **Auditable Reasoning over Linked Open Data**: streaming import adapters for RDF/N-Triples and JSON-LD, an open proof-export format that publishes derivation chains as Linked Data, a reusable constraint-rule library, and a web-based proof explorer. Earlier work was funded by two Wikimedia Community Fund (Rapid Fund) grants, both completed and reported:
[Wikidata Contradiction Detection and Constraint Integration](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Wikidata_Contradiction_Detection_and_Constraint_Integration_(ID:_23553409)/Final_Report>)
and
[Transitive Reasoning, Qualifier Support, and SPARQL-Subset Integration](<https://meta.wikimedia.org/wiki/Grants:Programs/Wikimedia_Community_Fund/Rapid_Fund/zelph:Transitive_Reasoning,_Qualifier_Support,_and_SPARQL-Subset_Integration_(ID:_23759260)/Final_Report>).

The project addresses real-world challenges in large-scale ontology management through direct collaboration with the [Wikidata Ontology Cleaning Task Force](https://www.wikidata.org/wiki/Wikidata:WikiProject_Ontology/Cleaning_Task_Force) and the [Mereology Task Force](https://www.wikidata.org/wiki/Wikidata_talk:WikiProject_Ontology/Mereology_Task_Force).

## Licensing

zelph is dual-licensed: [AGPL-3.0-or-later](LICENSE) for open-source use, and a commercial licence from acrion innovations GmbH for closed-source integration or special requirements ([acrion.ch/sales](https://acrion.ch/sales)). Offering the commercial option adds a possibility and takes nothing away from anyone using the open-source licence, commercial users included.
