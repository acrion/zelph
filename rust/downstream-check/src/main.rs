// Copyright (c) 2026 acrion innovations GmbH
// Authors: Stefan Zipproth, s.zipproth@acrion.ch
//
// This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org
// SPDX-License-Identifier: AGPL-3.0-or-later

//! Does the Rust layer work when it is embedded from outside?
//!
//! The engine itself is covered by zelph's own test suite and the crate's, and
//! nothing here tries to test reasoning again. What is tested is the part that
//! only an outside consumer exercises: that `zelph-sys` builds the library and
//! reports where it went, that the linker finds it, that `DEP_ZELPH_LIB_DIR`
//! reaches the binary's own build script, and that the result RUNS -- a
//! binary that links but cannot find its shared object at startup is exactly
//! the failure a build-only check misses.
//!
//! It stays small on purpose: everything it calls belongs to the surface an
//! outside project actually uses.

use zelph::{Engine, Result};

fn main() -> Result<()> {
    let z = Engine::new()?;

    let socrates = z.resolve("Socrates")?;
    let is_a = z.resolve("~")?;
    let human = z.resolve("human")?;
    let fact = z.fact(socrates, is_a, &[human])?;

    assert_ne!(socrates.0, 0, "a resolved name is never node 0");
    assert!(z.exists(socrates, is_a, &[human])?, "the fact just asserted is in the graph");

    // A node is its hash: the same construction reaches the same node, which
    // is the property every consumer's own identity handling rests on.
    assert_eq!(fact, z.fact(socrates, is_a, &[human])?);

    // Names travel back out over the ABI, which is where string ownership
    // would show up if it were wrong.
    assert_eq!(z.name(socrates)?.as_deref(), Some("Socrates"));

    println!("downstream-check: the zelph crate links, loads and answers");
    Ok(())
}
