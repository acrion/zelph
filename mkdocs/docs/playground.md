# Playground

The playground is the complete zelph reasoning engine — the same C++ core as the
native binaries — compiled to WebAssembly. It runs entirely in your browser;
nothing is sent to a server.

<p style="font-size: 1.15em">
  👉 <strong><a href="../play/" target="_blank">Launch the playground</a></strong>
</p>

It opens as a separate page because it is a full-screen terminal application
rather than an embeddable widget.

The built-in demo buttons walk you through:

- **Arithmetic as inference** — multiplication and division of arbitrarily large numbers, derived purely by rules ([background](arithmetic.md))
- **Logic and meta-rules** — a primality test using negation as failure, transitivity as a taught concept, contradiction detection ([background](logic.md))
- **SPARQL** — queries over facts derived by reasoning ([background](sparql.md))
- **Neural networks** — represented and executed inside the semantic graph ([background](neural.md))
- **From NAND to EML** — arithmetic synthesized from a single NAND axiom, with a symbolic layer (simplification, differentiation, the EML operator) built on top ([background](symbolic.md))

**Versions:** the playground on
<a href="https://acrion.github.io/zelph/play/">acrion.github.io/zelph/play</a>
always reflects the current development state (`main` branch), while
<a href="https://zelph.org/play/">zelph.org/play</a> tracks the latest release.
