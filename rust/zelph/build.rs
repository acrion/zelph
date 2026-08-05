// Copyright (c) 2025, 2026 acrion innovations GmbH
// Authors: Stefan Zipproth, s.zipproth@acrion.ch
//
// This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org
//
// zelph is offered under a commercial and under the AGPL license.
// For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.
//
// AGPL licensing:
//
// zelph is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// zelph is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with zelph. If not, see <https://www.gnu.org/licenses/>.

//! zelph is a shared library, so every executable that ends up linking it -
//! this crate's own tests included - needs to find it at run time.
//! `cargo:rustc-link-arg` applies to the crate being built and not to its
//! dependents, so the rpath cannot be inherited from zelph-sys; it is
//! re-emitted here from the directory that crate reports.
//!
//! A binary crate depending on `zelph` needs the same two lines, or an
//! LD_LIBRARY_PATH at run time.

fn main() {
    if let Ok(lib_dir) = std::env::var("DEP_ZELPH_LIB_DIR") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{lib_dir}");
    }
    println!("cargo:rerun-if-env-changed=DEP_ZELPH_LIB_DIR");
}
