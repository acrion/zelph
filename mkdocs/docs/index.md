# zelph

**zelph is a semantic network system and reasoning engine written in C++ with an embedded Janet layer. It runs on the desktop, inside a host application through a C ABI, and in your browser as WebAssembly.**

## A rule is a fact

zelph is a semantic network in which rules reside within the network. A rule is not a script adjacent to the graph, and it is not a query written against it: it is a statement of the same shape as the statements it reasons about, stored beside them.

Four lines are the entire concept:

```
zelph> "is part of" ~ "transitive relation"
zelph> (R ~ "transitive relation", X R Y, Y R Z) => (X R Z)
zelph> Canada "is part of" "American continent"
zelph> "American continent" "is part of" "Earth's surface"
(Canada "is part of" "Earth's surface") ⇐ {(Canada "is part of" "American continent") ("is part of" ~ "transitive relation") ("American continent" "is part of" "Earth's surface")}
```

Read the rule once more: it never mentions `is part of`. It quantifies over the relation itself, and what makes it apply here is the first line – an ordinary fact, of the kind any dataset can carry. Wikidata carries exactly this one, so this single rule covers every relation that Wikidata declares transitive, and nobody has to write anything for any of them.

The last line is the derivation: the conclusion, and the three statements it rests on. zelph says what it used.

### Try it before you believe it

The whole engine is compiled to WebAssembly. [Open the playground](play/) and type those four lines yourself – nothing is installed, and nothing leaves your browser. The [guided demos](playground.md) go further: arithmetic on large numbers that is derived from rules rather than built into the engine, a primality test by negation as failure, SPARQL over derived facts, and neural networks inside the graph.

When you need it on your machine, the [Quick Start Guide](quickstart.md) includes prebuilt binaries for Linux, macOS and Windows, the REPL, and the standard library – no build required.

### What it is for

Wikidata includes over 113 million entries, and nobody can manually verify whether they comply with the constraints that Wikidata itself defines. zelph reads those constraints from the dump and turns them into rules. Typed out small, so the shape is visible:

```
wikidata> (I P569 Y, I P571 Z) => !
wikidata> Q42 P569 Q1900
wikidata> Q42 P571 Q1900
! ⇐ {(Q42 P569 Q1900) (Q42 P571 Q1900)}
```

An entity here possesses a date of birth and, at the same time, an inception – a date set aside for what is founded or created rather than born. What emerges is not a score and not a list of suspects: it is the two statements that cannot both be right, which is what anyone needs in order to decide which of them is the mistake. Six rules of this kind run over a 26.5-million-node slice of the dump in under a minute.

For the reasoning behind all of this – deep unification, negation, inequality constraints, semantic arithmetic – see [Logic and Computation](logic.md). For zelph on Wikidata, see [zelph and Wikidata](wikidata.md).

### Domain-agnostic by construction

The engine knows nothing about any application domain. It provides unification, forward chaining, stratified negation and a hash-consed graph — no arithmetic, no ontology semantics, no vocabulary. Everything a domain needs is written as ordinary zelph rules in `.zph` scripts, which is why one engine serves very different users:

- **[Wikidata](wikidata.md)** — contradiction detection and constraint integration across the full knowledge graph, including [qualifier semantics](qualifiers.md) and [sharded partial loading](sharding.md) for graphs that do not fit in memory.
- **Legal ontologies** — [SensibLaw](https://github.com/chboishabba/SensibLaw), part of the [ITIR-suite](https://github.com/chboishabba/ITIR-suite), uses zelph as a downstream reasoning engine: it structures source material with full provenance and exports bounded graph slices to reason over.
- **Mathematics** – the [whole stack](math/index.md), from digits up to symbolic differentiation, written in zelph rules. Not a specialization, but a demonstration of how far the general mechanism reaches.

### Video: Logic and Computation

Watch this video walkthrough of zelph's reasoning capabilities — including live demonstrations of rules, meta-reasoning, semantic arithmetic, and Wikidata analysis. For section navigation and the full technical reference, visit [Logic and Computation](logic.md).

<video controls width="100%">
    <source src="https://zelph.org/assets/2026-03-21-zelph.mp4" type="video/mp4">
  Your browser does not support the video tag.
</video>

A separate [presentation video](presentation.md) covers zelph's application to Wikidata as part of the Ontology Cleaning Task Force.

zelph is supported by NLnet’s NGI0 Commons Fund and was previously supported by two
Wikimedia Rapid Fund grants; see [Funding and Collaboration](about.md#funding-and-collaboration)
for the details, and [Use of generative AI](about.md#use-of-generative-ai) for how new code is
produced.

## Components

The zelph ecosystem includes:

- A core C++ library providing both C++ and [C interfaces](capi.md)
- A single command-line binary that offers both interactive usage (CLI) and batch processing capabilities
- API functions beyond what's available in the command-line interface
- A [Rust layer](rust.md) over the C interface, so a host application binds zelph without the Janet interpreter in between. Any other language with an FFI reaches it the same way.

The key features of zelph include:

- Representation of knowledge in a semantic network structure
- Rules encoded within the same semantic network as facts
- Support for multi-language node naming
- Contradiction detection and resolution
- Memory-efficient data structures optimized at bit level
- A flexible scripting language for knowledge definition and querying
- Built-in [import and export](import-export.md) for Wikidata JSON datasets and general binary save/load

## Where to go next

- **[Quick Start Guide](quickstart.md)** – prebuilt binaries, the REPL, the standard library. Start here if you want it running.
- **[Core Concepts](concepts.md)** – the data model and the syntax: nodes, facts, sets, lists, the focus operator, the self-fact prefix.
- **[Rules and Inference](rules.md)** – how a rule is written, how it is stored, how inference runs, and how a derivation is exported.
- **[Example Script](example-script.md)** – one annotated session that puts the pieces together.
- **[Scripts and Modules](modules.md)** – `.import`, module IDs, and interchangeable implementations.
- **[Logic and Computation](logic.md)** – the reasoning behind all of it, with the comparisons to Prolog, Datalog, Lean and Lisp.
- **[Mathematics](math/index.md)** – the whole stack from digits to symbolic differentiation, written in zelph rules.
- **[zelph and Wikidata](wikidata.md)** – contradiction detection over the full knowledge graph.
- **[Building zelph](building.md)** and **[About the Project](about.md)** – building from source, project status, funding and licensing.
