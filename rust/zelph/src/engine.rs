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

use crate::error::{check, Error, ErrorKind, Result};
use crate::net::Net;
use crate::Node;

use std::ffi::{c_char, c_void, CStr, CString};
use std::marker::PhantomData;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::ptr;

/// Which stream a line the engine emitted belongs to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Channel {
    Out,
    Error,
    Diagnostic,
    Prompt,
}

impl Channel {
    fn from_raw(value: i32) -> Channel {
        match value {
            v if v == zelph_sys::zelph_channel::ZELPH_CHANNEL_ERROR as i32 => Channel::Error,
            v if v == zelph_sys::zelph_channel::ZELPH_CHANNEL_DIAGNOSTIC as i32 => Channel::Diagnostic,
            v if v == zelph_sys::zelph_channel::ZELPH_CHANNEL_PROMPT as i32 => Channel::Prompt,
            _ => Channel::Out,
        }
    }
}

type OutputCallback = Box<dyn FnMut(Channel, &str, bool)>;

/// A zelph instance: the graph, and the networks compiled out of it.
///
/// **One per process.** The C ABI refuses a second one while the first is
/// alive, because the script engine underneath keeps a process-wide instance
/// pointer. `Engine` is deliberately neither `Send` nor `Sync`: graph
/// mutation belongs to the thread that created it, exactly as it does in the
/// Janet host. What *is* shareable is a compiled [`Net`], which is where a
/// parallel search needs to reach.
pub struct Engine {
    raw: *mut zelph_sys::zelph_engine,

    // Kept alive for exactly as long as the engine can call it. The Box is
    // what gives the closure a stable address to hand to C.
    _output: Option<Box<OutputCallback>>,

    // Neither Send nor Sync, and stated rather than inherited.
    _not_thread_safe: PhantomData<*const ()>,
}

impl Engine {
    /// Create an engine that writes to the process's standard streams.
    pub fn new() -> Result<Engine> {
        Engine::create(None)
    }

    /// Create an engine that hands every line it emits to `output` instead.
    ///
    /// This is what a host with its own protocol on stdout needs - a chess
    /// engine speaking UCI, say, for which an unexpected line is a protocol
    /// error rather than a diagnostic.
    pub fn with_output<F>(output: F) -> Result<Engine>
    where
        F: FnMut(Channel, &str, bool) + 'static,
    {
        Engine::create(Some(Box::new(Box::new(output))))
    }

    fn create(output: Option<Box<OutputCallback>>) -> Result<Engine> {
        let mut raw: *mut zelph_sys::zelph_engine = ptr::null_mut();

        let (callback, user_data): (zelph_sys::zelph_output_fn, *mut c_void) = match &output {
            Some(boxed) => (
                Some(output_trampoline),
                &**boxed as *const OutputCallback as *mut c_void,
            ),
            None => (None, ptr::null_mut()),
        };

        check(unsafe { zelph_sys::zelph_engine_create(callback, user_data, &mut raw) })?;

        Ok(Engine {
            raw,
            _output: output,
            _not_thread_safe: PhantomData,
        })
    }

    /// Resolve a name to its node in the engine's current language, creating
    /// the node if it does not exist yet.
    ///
    /// A node *is* its hash, so the answer is stable: the same name yields
    /// the same number in this engine, in the next one, and after a
    /// [`load`](Engine::load) of a graph saved elsewhere.
    pub fn resolve(&self, name: &str) -> Result<Node> {
        self.resolve_impl(name, None)
    }

    /// As [`resolve`](Engine::resolve), but binds the name to an explicit
    /// language rather than to the current one.
    pub fn resolve_in(&self, name: &str, lang: &str) -> Result<Node> {
        self.resolve_impl(name, Some(lang))
    }

    fn resolve_impl(&self, name: &str, lang: Option<&str>) -> Result<Node> {
        let name = cstring(name)?;
        let lang = lang.map(cstring).transpose()?;
        let mut node: zelph_sys::zelph_node = 0;

        check(unsafe {
            zelph_sys::zelph_resolve(
                self.raw,
                name.as_ptr(),
                lang.as_ref().map_or(ptr::null(), |l| l.as_ptr()),
                &mut node,
            )
        })?;

        Ok(Node(node))
    }

    /// Assert the fact `(subject predicate object...)` and return its node.
    pub fn fact(&self, subject: Node, predicate: Node, objects: &[Node]) -> Result<Node> {
        let mut fact: zelph_sys::zelph_node = 0;

        check(unsafe {
            zelph_sys::zelph_fact(
                self.raw,
                subject.0,
                predicate.0,
                objects.as_ptr().cast(),
                objects.len(),
                &mut fact,
            )
        })?;

        Ok(Node(fact))
    }

    /// Build a cons list; the first element becomes the outermost cell.
    ///
    /// Structurally identical lists are one node, which is what makes a
    /// structure usable as an identifier without anyone agreeing on one.
    pub fn list(&self, elements: &[Node]) -> Result<Node> {
        let mut node: zelph_sys::zelph_node = 0;

        check(unsafe {
            zelph_sys::zelph_list(self.raw, elements.as_ptr().cast(), elements.len(), &mut node)
        })?;

        Ok(Node(node))
    }

    /// The name of a node, or `None` when it has none - a fact node, for
    /// instance. Not having a name is an answer, not a failure.
    pub fn name(&self, node: Node) -> Result<Option<String>> {
        let mut text: *mut c_char = ptr::null_mut();

        check(unsafe { zelph_sys::zelph_name(self.raw, node.0, ptr::null(), &mut text) })?;

        if text.is_null() {
            return Ok(None);
        }

        let name = unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned();
        unsafe { zelph_sys::zelph_string_free(text) };
        Ok(Some(name))
    }

    /// Every subject of a fact `(X predicate target)`.
    ///
    /// Directional: a fact `(target predicate X)` does not contribute, so
    /// asking for the members of a layer cannot accidentally return the
    /// layer's own memberships.
    pub fn sources(&self, predicate: Node, target: Node) -> Result<Vec<Node>> {
        let mut count: usize = 0;

        // Capacity 0 is the size question. It answers BufferTooSmall unless
        // there is nothing to report, which is the one case that needs no
        // second call.
        match check(unsafe {
            zelph_sys::zelph_sources(self.raw, predicate.0, target.0, ptr::null_mut(), &mut count)
        }) {
            Ok(()) => return Ok(Vec::new()),
            Err(e) if e.kind() != ErrorKind::BufferTooSmall => return Err(e),
            Err(_) => {}
        }

        let mut nodes: Vec<Node> = vec![Node(0); count];
        check(unsafe {
            zelph_sys::zelph_sources(
                self.raw,
                predicate.0,
                target.0,
                nodes.as_mut_ptr().cast(),
                &mut count,
            )
        })?;

        nodes.truncate(count);
        Ok(nodes)
    }

    /// Load a saved graph (`.bin`) or import a data dump, as the `.load`
    /// command does.
    pub fn load(&self, path: impl AsRef<Path>) -> Result<()> {
        let path = path_arg(path.as_ref())?;
        check(unsafe { zelph_sys::zelph_load(self.raw, path.as_ptr()) })
    }

    /// Save the graph, as the `.save` command does. The path must end in
    /// `.bin`.
    ///
    /// What a compiled net has learnt lives in the net until
    /// [`Net::write_back`] puts it into the graph, so a save without that
    /// call persists the untrained weights.
    pub fn save(&self, path: impl AsRef<Path>) -> Result<()> {
        let path = path_arg(path.as_ref())?;
        check(unsafe { zelph_sys::zelph_save(self.raw, path.as_ptr()) })
    }

    /// Fully connect two layers with raw synapses, weights drawn uniformly
    /// from `[-scale, scale]`. Returns how many were created.
    ///
    /// Existing synapses keep their weights, so this is idempotent: wiring a
    /// loaded net again costs nothing and destroys nothing.
    pub fn connect_layers(&self, from: Node, to: Node, scale: f64, seed: u64) -> Result<i64> {
        let mut created: i64 = 0;

        check(unsafe {
            zelph_sys::zelph_nn_connect_layers(self.raw, from.0, to.0, scale, seed, &mut created)
        })?;

        Ok(created)
    }

    /// Compile a feed-forward view of the sub-graph spanned by these layers,
    /// input first, output last.
    ///
    /// The net is a *view*: its neurons are graph nodes and its weights are
    /// graph edges, which is why saving the graph saves the network without
    /// a network file format existing at all.
    pub fn compile(&self, layers: &[Node]) -> Result<Net<'_>> {
        let mut handle: zelph_sys::zelph_net = -1;

        check(unsafe {
            zelph_sys::zelph_nn_compile(
                self.raw,
                layers.as_ptr().cast(),
                layers.len(),
                &mut handle,
            )
        })?;

        Ok(Net::new(self.raw, handle))
    }
}

impl std::fmt::Debug for Engine {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Engine")
            .field("captures_output", &self._output.is_some())
            .finish_non_exhaustive()
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        unsafe { zelph_sys::zelph_engine_destroy(self.raw) };
    }
}

/// The C side calls this; it must not unwind into a foreign frame, so a
/// panicking callback is caught and dropped rather than allowed to cross.
extern "C" fn output_trampoline(
    user_data: *mut c_void,
    channel: i32,
    text: *const c_char,
    newline: i32,
) {
    if user_data.is_null() {
        return;
    }

    let _ = catch_unwind(AssertUnwindSafe(|| {
        let callback = unsafe { &mut *(user_data as *mut OutputCallback) };
        let text = if text.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned()
        };
        callback(Channel::from_raw(channel), &text, newline != 0);
    }));
}

fn cstring(value: &str) -> Result<CString> {
    CString::new(value).map_err(|_| {
        Error::new(
            ErrorKind::InvalidArgument,
            "the string contains a NUL byte, which C cannot carry",
        )
    })
}

fn path_arg(path: &Path) -> Result<CString> {
    let text = path.to_str().ok_or_else(|| {
        Error::new(
            ErrorKind::InvalidArgument,
            format!("path {} is not valid UTF-8", path.display()),
        )
    })?;
    cstring(text)
}
