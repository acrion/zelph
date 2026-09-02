# zelph

Safe Rust bindings for [zelph](https://zelph.org), a semantic graph that interns structurally identical subgraphs to the same node, with a forward-chaining reasoner over it and neural networks compiled out of it.

This crate covers what a host application needs: resolve names to nodes, assert facts, read them back, build lists and sets, compile a network out of the graph, evaluate it, train it, persist it. Rules, imports, display schemes and SPARQL stay in zelph’s own language and in the REPL.

```rust
use zelph::{Engine, Result};

fn main() -> Result<()> {
    let z = Engine::new()?;

    let socrates = z.resolve("Socrates")?;
    let is_a = z.resolve("~")?;
    let human = z.resolve("human")?;

    let fact = z.fact(socrates, is_a, &[human])?;
    assert_eq!(fact, z.fact(socrates, is_a, &[human])?); // a node is its hash
    Ok(())
}
```

Two properties shape the API. **A node is its hash**: values remain constant across calls and through a save and load cycle, and two structurally identical constructions collapse into one node. **The engine is single-threaded, a compiled network is not**: `Engine` is neither `Send` nor `Sync` and there is one per process, while `Net` is both and may be evaluated from many threads while another one trains it.

## Linking

zelph is a C++ library and this crate does not carry it. Point `ZELPH_BUILD_DIR` at a CMake build directory of the [zelph repository](https://github.com/acrion/zelph), or work inside a checkout, where the default is `build-release` in the repository root. A consumer that produces a binary also names `zelph-sys` as a direct dependency and emits an rpath for it; the reason and the three lines are in [the documentation](https://zelph.org/rust/).

## Licence

AGPL-3.0-or-later, or a commercial licence – see [acrion.ch/sales](https://acrion.ch/sales).
