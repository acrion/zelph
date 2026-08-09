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

/// How a hidden layer's pre-activation becomes its activation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Activation {
    /// `max(0, x)`, and what every net compiled before this option existed
    /// was trained with.
    Relu = 0,

    /// `max(0.01 x, x)`. The gradient is never exactly zero - which matters
    /// because with a plain ReLU a hidden layer whose every unit is negative
    /// for every input has an output of exactly 0 AND a gradient of exactly
    /// 0. That state is absorbing: no further training can leave it, and a
    /// small online-trained net can walk into it and stay there.
    LeakyRelu = 1,
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
    /// input first, output last, with ReLU hidden layers.
    ///
    /// The net is a *view*: its neurons are graph nodes and its weights are
    /// graph edges, which is why saving the graph saves the network without
    /// a network file format existing at all.
    pub fn compile(&self, layers: &[Node]) -> Result<Net<'_>> {
        self.compile_with(layers, Activation::Relu)
    }

    /// As [`compile`](Engine::compile), with the hidden-layer activation
    /// named.
    ///
    /// A net trained with one activation must be evaluated with the same one:
    /// it is a property of the compiled view, and getting it wrong changes
    /// every output that came from a unit below zero.
    pub fn compile_with(&self, layers: &[Node], activation: Activation) -> Result<Net<'_>> {
        let mut handle: zelph_sys::zelph_net = -1;

        check(unsafe {
            zelph_sys::zelph_nn_compile(
                self.raw,
                layers.as_ptr().cast(),
                layers.len(),
                activation as i32,
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

/// A row of a query's answer: which variable was bound to what.
pub type Bindings = Vec<(Node, Node)>;

impl Engine {
    /// A variable, for use inside a rule or a query pattern.
    ///
    /// Remembered by name, so asking twice for `"A"` gives the same node -
    /// which is what makes a pattern built in one call queryable in another.
    /// [`clear_variables`](Engine::clear_variables) forgets them.
    pub fn variable(&self, name: &str) -> Result<Node> {
        let name = cstring(name)?;
        let mut node: zelph_sys::zelph_node = 0;

        check(unsafe { zelph_sys::zelph_variable(self.raw, name.as_ptr(), &mut node) })?;
        Ok(Node(node))
    }

    pub fn clear_variables(&self) -> Result<()> {
        check(unsafe { zelph_sys::zelph_clear_variables(self.raw) })
    }

    /// A set constant: identified by its members, so the same elements always
    /// yield the same node and membership cannot be extended.
    pub fn set(&self, elements: &[Node]) -> Result<Node> {
        let mut node: zelph_sys::zelph_node = 0;
        check(unsafe {
            zelph_sys::zelph_set(self.raw, elements.as_ptr().cast(), elements.len(), &mut node)
        })?;
        Ok(Node(node))
    }

    /// A collection: a container with its own identity, so two calls with the
    /// same elements yield two different nodes.
    pub fn collection(&self, elements: &[Node]) -> Result<Node> {
        let mut node: zelph_sys::zelph_node = 0;
        check(unsafe {
            zelph_sys::zelph_collection(
                self.raw,
                elements.as_ptr().cast(),
                elements.len(),
                &mut node,
            )
        })?;
        Ok(Node(node))
    }

    /// Mark a fact pattern as a negation, i.e. negation as failure.
    ///
    /// Evaluated against the SATURATED positive fact base, never against
    /// in-flight state - zelph's stratification rule, and what makes "no
    /// defender remains" expressible at all.
    pub fn negate(&self, pattern: Node) -> Result<Node> {
        let mut node: zelph_sys::zelph_node = 0;
        check(unsafe { zelph_sys::zelph_negate(self.raw, pattern.0, &mut node) })?;
        Ok(Node(node))
    }

    /// Does this fact exist? Creates nothing.
    pub fn exists(&self, subject: Node, predicate: Node, objects: &[Node]) -> Result<bool> {
        let mut exists: i32 = 0;
        check(unsafe {
            zelph_sys::zelph_exists(
                self.raw,
                subject.0,
                predicate.0,
                objects.as_ptr().cast(),
                objects.len(),
                &mut exists,
            )
        })?;
        Ok(exists != 0)
    }

    /// Every object connected from `subject` through `predicate` - the mirror
    /// of [`sources`](Engine::sources).
    pub fn targets(&self, subject: Node, predicate: Node) -> Result<Vec<Node>> {
        let mut count: usize = 0;

        match check(unsafe {
            zelph_sys::zelph_targets(self.raw, subject.0, predicate.0, ptr::null_mut(), &mut count)
        }) {
            Ok(()) => return Ok(Vec::new()),
            Err(e) if e.kind() != ErrorKind::BufferTooSmall => return Err(e),
            Err(_) => {}
        }

        let mut nodes: Vec<Node> = vec![Node(0); count];
        check(unsafe {
            zelph_sys::zelph_targets(
                self.raw,
                subject.0,
                predicate.0,
                nodes.as_mut_ptr().cast(),
                &mut count,
            )
        })?;

        nodes.truncate(count);
        Ok(nodes)
    }

    /// An inference rule: when every condition holds, deduce every
    /// consequence. Returns the condition set.
    ///
    /// Nothing is derived until the engine [`run`](Engine::run)s.
    pub fn rule(&self, conditions: &[Node], consequences: &[Node]) -> Result<Node> {
        let mut node: zelph_sys::zelph_node = 0;
        check(unsafe {
            zelph_sys::zelph_rule(
                self.raw,
                conditions.as_ptr().cast(),
                conditions.len(),
                consequences.as_ptr().cast(),
                consequences.len(),
                &mut node,
            )
        })?;
        Ok(Node(node))
    }

    /// Forward chaining to a fixed point.
    pub fn run(&self) -> Result<()> {
        check(unsafe { zelph_sys::zelph_run(self.raw) })
    }

    /// A single inference pass.
    pub fn run_once(&self) -> Result<()> {
        check(unsafe { zelph_sys::zelph_run_once(self.raw) })
    }

    /// Whether the unification engine may spread a relation's candidates over
    /// worker threads. On by default; returns what it was.
    ///
    /// A throughput/latency trade rather than a semantic one - the derived
    /// facts are the same either way. Parallelism pays on a large graph and
    /// costs on a small one, where dispatch dominates the scan it replaces.
    /// A caller reasoning about many small fact bases in a loop is exactly the
    /// case that wants it off.
    pub fn set_parallel(&self, enabled: bool) -> Result<bool> {
        let mut previous: i32 = 0;
        check(unsafe {
            zelph_sys::zelph_set_parallel(self.raw, i32::from(enabled), &mut previous)
        })?;
        Ok(previous != 0)
    }

    /// Inference seeded by what was created since the previous run, so the
    /// cost follows the addition rather than the graph. That difference is
    /// what decides whether reasoning can happen inside a loop.
    pub fn run_delta(&self) -> Result<()> {
        check(unsafe { zelph_sys::zelph_run_delta(self.raw) })
    }

    /// Answer a query pattern - a fact containing variables - as one set of
    /// bindings per match.
    pub fn query(&self, pattern: Node) -> Result<Vec<Bindings>> {
        let mut pairs: usize = 0;
        let mut rows: usize = 0;

        match check(unsafe {
            zelph_sys::zelph_query(
                self.raw,
                pattern.0,
                ptr::null_mut(),
                &mut pairs,
                ptr::null_mut(),
                &mut rows,
            )
        }) {
            Ok(()) => return Ok(Vec::new()),
            Err(e) if e.kind() != ErrorKind::BufferTooSmall => return Err(e),
            Err(_) => {}
        }

        let mut flat: Vec<Node> = vec![Node(0); pairs * 2];
        let mut sizes: Vec<usize> = vec![0; rows];
        check(unsafe {
            zelph_sys::zelph_query(
                self.raw,
                pattern.0,
                flat.as_mut_ptr().cast(),
                &mut pairs,
                sizes.as_mut_ptr(),
                &mut rows,
            )
        })?;

        let mut answer = Vec::with_capacity(rows);
        let mut index = 0;
        for &size in sizes.iter().take(rows) {
            let mut row = Bindings::with_capacity(size);
            for _ in 0..size {
                row.push((flat[index * 2], flat[index * 2 + 1]));
                index += 1;
            }
            answer.push(row);
        }

        Ok(answer)
    }

    /// Activate a named cluster, or deactivate tracking with `None`.
    ///
    /// Nodes CREATED while a cluster is active are recorded in it, which is
    /// what makes [`drop_cluster`](Engine::drop_cluster) a rollback - and
    /// what turns a monotonic graph into a workspace.
    pub fn cluster(&self, name: Option<&str>) -> Result<()> {
        let name = name.map(cstring).transpose()?;
        check(unsafe {
            zelph_sys::zelph_cluster(self.raw, name.as_ref().map_or(ptr::null(), |n| n.as_ptr()))
        })
    }

    /// The active cluster, or `None` for the default.
    pub fn active_cluster(&self) -> Result<Option<String>> {
        let mut text: *mut c_char = ptr::null_mut();
        check(unsafe { zelph_sys::zelph_cluster_active(self.raw, &mut text) })?;

        if text.is_null() {
            return Ok(None);
        }

        let name = unsafe { CStr::from_ptr(text) }.to_string_lossy().into_owned();
        unsafe { zelph_sys::zelph_string_free(text) };
        Ok(Some(name))
    }

    /// Remove every node the cluster recorded and report how many went.
    /// Nodes that existed before it was activated were never recorded, so a
    /// drop cannot reach them.
    pub fn drop_cluster(&self, name: &str) -> Result<i64> {
        let name = cstring(name)?;
        let mut removed: i64 = 0;
        check(unsafe { zelph_sys::zelph_cluster_drop(self.raw, name.as_ptr(), &mut removed) })?;
        Ok(removed)
    }

    /// How many nodes a cluster holds, or `None` when there is no such
    /// cluster.
    pub fn cluster_size(&self, name: &str) -> Result<Option<u64>> {
        let name = cstring(name)?;
        let mut count: i64 = -1;
        check(unsafe { zelph_sys::zelph_cluster_count(self.raw, name.as_ptr(), &mut count) })?;
        Ok((count >= 0).then_some(count as u64))
    }
}
