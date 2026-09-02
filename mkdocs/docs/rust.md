# The Rust Layer

Two crates live in `rust/` and sit on the [C ABI](capi.md):

| crate | what it is |
| --- | --- |
| `zelph-sys` | raw declarations, generated from `zelph_c.h` via bindgen during build time. It also builds the library and indicates its location. Nothing within it is safe, and nothing within it is hand-written. |
| `zelph` | safe API. Statuses are wrapped in `Result`, out-parameters are transformed into return values, strings are owned on both sides, and a compiled network borrows the engine it came from. |

Anyone developing a host application relies on `zelph`. `zelph-sys` is named in `Cargo.toml` as well, for the reason given below.

## What it covers, and what it does not

The same line as the C ABI: the part a host application needs – resolve names to nodes, assert facts, read them back, build lists and sets, compile a network out of the graph, evaluate it, train it, persist it. Rules, imports, display schemes and SPARQL stay in zelph's own language and in the REPL, where they are written once and read by anyone.

Two properties shape everything else:

- **A node is its hash.** `Node` values are stable across calls and across a save and load cycle, and two structurally identical constructions collapse into one node. Identity is derived, never handed out.
- **The engine is single-threaded, a compiled network is not.** `Engine` is neither `Send` nor `Sync`, and there is one per process. `Net` is both, and may be evaluated from many threads while another one trains it.

## Depending on it

There is no crates.io release yet, so a project points at a checkout:

```toml
[dependencies]
zelph     = { path = "../zelph/rust/zelph" }
zelph-sys = { path = "../zelph/rust/zelph-sys" }
```

**The second line is not redundant, and leaving it out costs an afternoon.** `zelph-sys` declares `links = "zelph"`, and cargo hands the variables such a crate publishes – among them `DEP_ZELPH_LIB_DIR` – to its *direct* dependents only. A binary that reaches `zelph-sys` through `zelph` alone never sees them, links without complaint, and fails at startup with `libzelph.so: cannot open shared object file`.

The binary crate also needs a `build.rs` consisting of three lines, because a shared library must be findable at run time and `cargo:rustc-link-arg` does not extend to a crate's dependents:

```rust
fn main() {
    if let Ok(lib_dir) = std::env::var("DEP_ZELPH_LIB_DIR") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{lib_dir}");
    }
    println!("cargo:rerun-if-env-changed=DEP_ZELPH_LIB_DIR");
}
```

## The build

`zelph-sys` builds zelph on each `cargo build` instead of simply detecting its presence. A stale library is the failure this project has already paid for once: it quietly invalidates every measurement made relative to it and nothing in the output conveys this.

| variable | effect |
| --- | --- |
| `ZELPH_BUILD_DIR` | the CMake build directory to employ and maintain. Default: `build-release` in the repository root. |
| `ZELPH_NO_BUILD` | set it to skip the CMake step, for a caller that builds zelph itself and knows the tree is current. |

## An example

```rust
use zelph::{Engine, Result};

fn main() -> Result<()> {
    let z = Engine::new()?;

    let socrates = z.resolve("Socrates")?;
    let is_a = z.resolve("~")?;
    let human = z.resolve("human")?;

    let fact = z.fact(socrates, is_a, &[human])?;
    assert!(z.exists(socrates, is_a, &[human])?);
    assert_eq!(fact, z.fact(socrates, is_a, &[human])?); // a node is its hash

    assert_eq!(z.name(socrates)?.as_deref(), Some("Socrates"));
    Ok(())
}
```

A failure arrives as an `Error` carrying an `ErrorKind` and the engine's own message. `InvalidArgument` says the call itself was wrong and retrying it unaltered will not assist; `Runtime` says the engine reached the call and refused it; `Unknown` says the C ABI has grown a status this crate is not yet aware of.

## How it is tested

Three items operate in CI, and the third exists because the first two cannot reach it:

- `cargo test` across the workspace – the safe API from outside the crate, plus one test in `zelph-sys` that calls the generated declarations, which is what proves that bindgen, the header and the library still agree;
- `cargo doc` with warnings treated as errors, so a broken link in the documentation fails the build;
- `rust/downstream-check` – a crate that is deliberately **not** a workspace member. It depends on `zelph` by path the way an outside project does, produces a binary, and is run. A binary that links but cannot find its shared library at startup is exactly what a build-only check misses, and that contract used to be exercised only when a separate project was built.

The library is also compiled with hidden symbol visibility, so what it offers matches what a Windows DLL offers. Everything the C ABI declares is fully exported; the Rust test suite runs against that setting.
