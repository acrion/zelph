// Copyright (c) 2026 acrion innovations GmbH
// Authors: Stefan Zipproth, s.zipproth@acrion.ch
//
// This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org
// SPDX-License-Identifier: AGPL-3.0-or-later

//! zelph is a shared library, and `cargo:rustc-link-arg` does not reach a
//! crate's dependents -- so the rpath has to be emitted by the crate that
//! actually produces a binary. This file is the three lines every such
//! consumer needs, and it is here so that they are executed by CI rather than
//! only in the project that discovers they were wrong.

fn main() {
    if let Ok(lib_dir) = std::env::var("DEP_ZELPH_LIB_DIR") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{lib_dir}");
    }
    println!("cargo:rerun-if-env-changed=DEP_ZELPH_LIB_DIR");
}
