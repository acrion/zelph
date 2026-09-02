# zelph-sys

Raw FFI declarations for the C ABI of [zelph](https://zelph.org), generated from `zelph_c.h` by bindgen at build time. Nothing here is safe and nothing is written by hand.

Most callers want the [`zelph`](https://crates.io/crates/zelph) crate, which turns statuses into `Result`, out-parameters into return values, and owns strings on both sides.

## Linking

zelph is a C++ library and this crate does not carry it. Point `ZELPH_BUILD_DIR` to a CMake build directory of the [zelph repository](https://github.com/acrion/zelph), or operate within a checkout, where the default is `build-release` in the repository root. Set `ZELPH_NO_BUILD` when the caller is building zelph itself and is aware the tree is current.

This crate declares `links = "zelph"` and publishes `DEP_ZELPH_LIB_DIR`, which cargo hands to its **direct** dependents only. A binary requiring the rpath therefore names this crate itself, not merely `zelph`; see [the documentation](https://zelph.org/rust/).

## Licence

AGPL-3.0-or-later, or a commercial licence – see [acrion.ch/sales](https://acrion.ch/sales).
