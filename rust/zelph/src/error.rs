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

use std::ffi::CStr;
use std::fmt;

/// What kind of refusal it was. The message carries the detail; this is what
/// a caller can branch on.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErrorKind {
    /// The caller's own side is wrong: a zero node, an unknown handle, an
    /// empty object list. Retrying without changing anything cannot help.
    InvalidArgument,
    /// An output buffer was too small. The safe API allocates for the caller,
    /// so this reaching Rust code means a size query and its use disagreed.
    BufferTooSmall,
    /// The engine reached the call and refused it: a file that does not
    /// parse, a layer without members, a name that does not resolve.
    Runtime,
    /// A status code this binding does not know, which means the C ABI has
    /// grown one and this crate has not been updated.
    Unknown(i32),
}

#[derive(Debug, Clone)]
pub struct Error {
    kind: ErrorKind,
    message: String,
}

impl Error {
    pub fn kind(&self) -> ErrorKind {
        self.kind
    }

    pub fn message(&self) -> &str {
        &self.message
    }

    pub(crate) fn new(kind: ErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.message.is_empty() {
            write!(f, "{:?}", self.kind)
        } else {
            write!(f, "{}", self.message)
        }
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// Turn a status code into a `Result`, reading the engine's message for the
/// failure. The message is thread-local and only valid until the next call,
/// so it is copied here and nowhere later.
pub(crate) fn check(status: i32) -> Result<()> {
    if status == zelph_sys::zelph_status::ZELPH_OK as i32 {
        return Ok(());
    }

    let kind = match status {
        s if s == zelph_sys::zelph_status::ZELPH_INVALID_ARGUMENT as i32 => ErrorKind::InvalidArgument,
        s if s == zelph_sys::zelph_status::ZELPH_BUFFER_TOO_SMALL as i32 => ErrorKind::BufferTooSmall,
        s if s == zelph_sys::zelph_status::ZELPH_RUNTIME_ERROR as i32 => ErrorKind::Runtime,
        s => ErrorKind::Unknown(s),
    };

    let message = unsafe {
        CStr::from_ptr(zelph_sys::zelph_last_error())
            .to_string_lossy()
            .into_owned()
    };

    Err(Error::new(kind, message))
}
