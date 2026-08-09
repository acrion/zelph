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

//! Builds zelph and generates the raw bindings from its C header.
//!
//! The library is rebuilt on every `cargo build`, not merely located. A stale
//! library is the failure mode this project has already paid for once: it
//! silently invalidates every measurement taken against it, and nothing in the
//! output says so.
//!
//! Environment:
//!   ZELPH_BUILD_DIR   the CMake build directory to use and to keep current
//!                     (default: `build-release` in the repository root)
//!   ZELPH_NO_BUILD    set to skip the CMake step entirely, for a caller that
//!                     builds zelph itself and knows the tree is current

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let repo = manifest
        .parent()
        .and_then(Path::parent)
        .expect("zelph-sys lives two levels below the repository root")
        .to_path_buf();

    let header = repo.join("src/lib/capi/zelph_c.h");
    assert!(
        header.exists(),
        "cannot find {} - is {} a zelph checkout?",
        header.display(),
        repo.display()
    );

    let build_dir = env::var_os("ZELPH_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| repo.join("build-release"));

    if env::var_os("ZELPH_NO_BUILD").is_none() {
        build_zelph(&repo, &build_dir);
    }

    let lib_dir = find_library(&build_dir).unwrap_or_else(|| {
        panic!(
            "no zelph library under {} - configure and build it, or point ZELPH_BUILD_DIR at a tree that has one",
            build_dir.display()
        )
    });

    // The generated export header lives with the objects, not in the source
    // tree, and zelph_c.h includes it.
    let generated_include = build_dir.join("src/lib");

    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib=zelph");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());

    // Read by dependent crates as DEP_ZELPH_LIB_DIR / DEP_ZELPH_INCLUDE_DIR
    // (the `links = "zelph"` key is what makes cargo forward these). A
    // dependent that produces a binary needs the rpath too, and only it can
    // emit that for itself.
    println!("cargo:lib_dir={}", lib_dir.display());
    println!("cargo:include_dir={}", header.parent().unwrap().display());
    println!("cargo:generated_include_dir={}", generated_include.display());

    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-env-changed=ZELPH_BUILD_DIR");
    println!("cargo:rerun-if-env-changed=ZELPH_NO_BUILD");

    let bindings = bindgen::Builder::default()
        .header(header.to_string_lossy())
        .clang_arg(format!("-I{}", generated_include.display()))
        // Only zelph's own surface: without this the bindings would carry
        // every declaration stddef.h and stdint.h drag in.
        .allowlist_function("zelph_.*")
        .allowlist_type("zelph_.*")
        .allowlist_var("ZELPH_.*")
        .default_enum_style(bindgen::EnumVariation::ModuleConsts)
        .prepend_enum_name(false)
        .derive_default(true)
        .generate_comments(true)
        .generate()
        .expect("bindgen failed on zelph_c.h");

    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    bindings
        .write_to_file(out.join("bindings.rs"))
        .expect("could not write bindings.rs");
}

fn build_zelph(repo: &Path, build_dir: &Path) {
    if !build_dir.join("CMakeCache.txt").exists() {
        run(
            Command::new("cmake")
                .arg("-S")
                .arg(repo)
                .arg("-B")
                .arg(build_dir)
                .arg("-DCMAKE_BUILD_TYPE=Release"),
            "cmake configure",
        );
    }

    let mut build = Command::new("cmake");
    build
        .arg("--build")
        .arg(build_dir)
        .arg("--target")
        .arg("zelph_lib");
    if let Ok(jobs) = env::var("NUM_JOBS") {
        build.arg("-j").arg(jobs);
    }
    run(&mut build, "cmake build");
}

fn run(command: &mut Command, what: &str) {
    let status = command
        .status()
        .unwrap_or_else(|e| panic!("{what}: could not start ({e})"));
    assert!(status.success(), "{what} failed with {status}");
}

fn find_library(build_dir: &Path) -> Option<PathBuf> {
    // CMake puts the shared library where the project's output directories
    // point, which is `bin` here; `lib` is the default and stays a fallback.
    ["bin", "lib"]
        .iter()
        .map(|d| build_dir.join(d))
        .find(|dir| {
            ["libzelph.so", "libzelph.dylib", "zelph.dll", "zelph.lib"]
                .iter()
                .any(|name| dir.join(name).exists())
        })
}
