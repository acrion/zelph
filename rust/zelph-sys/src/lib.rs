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

//! Raw declarations of zelph's C ABI, generated from `zelph_c.h` by bindgen at
//! build time so the two cannot drift.
//!
//! Nothing here is safe to call. Use the `zelph` crate, which is the reason
//! this one exists.

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::{CStr, CString};
    use std::ptr;

    // The point of this test is not the arithmetic - it is that the symbols
    // link, the struct layouts agree and a status code comes back as a
    // number rather than as a crash. If bindgen and the header ever diverge,
    // this fails here rather than three layers up.
    #[test]
    fn the_generated_declarations_link_and_call() {
        unsafe {
            let mut engine: *mut zelph_engine = ptr::null_mut();
            assert_eq!(
                zelph_engine_create(None, ptr::null_mut(), &mut engine),
                zelph_status::ZELPH_OK as i32
            );
            assert!(!engine.is_null());

            let name = CString::new("Socrates").unwrap();
            let mut node: zelph_node = 0;
            assert_eq!(
                zelph_resolve(engine, name.as_ptr(), ptr::null(), &mut node),
                zelph_status::ZELPH_OK as i32
            );
            assert_ne!(node, 0);

            let mut text: *mut std::os::raw::c_char = ptr::null_mut();
            assert_eq!(
                zelph_name(engine, node, ptr::null(), &mut text),
                zelph_status::ZELPH_OK as i32
            );
            assert!(!text.is_null());
            assert_eq!(CStr::from_ptr(text).to_str().unwrap(), "Socrates");
            zelph_string_free(text);

            // A rejected call must arrive as a status plus a message, which is
            // the whole contract of this ABI.
            assert_eq!(
                zelph_resolve(engine, ptr::null(), ptr::null(), &mut node),
                zelph_status::ZELPH_INVALID_ARGUMENT as i32
            );
            assert!(!CStr::from_ptr(zelph_last_error()).to_bytes().is_empty());

            zelph_engine_destroy(engine);
        }
    }
}
