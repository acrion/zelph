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

- **Arithmetic as Inference** – multiplication and division of arbitrarily large numbers, derived purely by rules ([background](math/arithmetic.md))
- **Number Theory and Meta-Rules** – a primality test using negation as failure, transitivity as a taught concept, contradiction detection ([background](logic.md))
- **SPARQL over Derived Facts** – queries over facts derived by reasoning ([background](sparql.md))
- **Neural Networks in the Graph** – represented and executed inside the semantic graph ([background](neural.md))
- **Symbolic Mathematics** – polynomial identities proved and disproved, symbolic differentiation, and an operator taught from scratch ([background](math/index.md))
- **A Single Operator: EML** – the natural logarithm of x and the mathematical constant e, derived from the single operator eml(x, y) = exp(x) − ln(y) along with the constant 1, and ln(x) compiled back into pure EML ([background](math/eml.md))
- **Refuting the Jacobian Conjecture** – a polynomial map whose Jacobian determinant equals a nonzero constant, along with three distinct points sharing the same image ([background](math/tutorial-jacobian.md))

**Versions:** the playgrounds on <a href="https://zelph.org/play/">zelph.org/play</a> and <a href="https://acrion.github.io/zelph/play/">acrion.github.io/zelph/play</a> are identical: both follow the `main` branch.
