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

//! Safe Rust bindings for zelph.
//!
//! zelph is a semantic graph that interns structurally identical subgraphs to
//! the same node, with a forward-chaining reasoner over it and neural
//! networks compiled out of it. This crate covers the part a *host
//! application* needs - names to nodes, facts, networks, persistence - and
//! leaves rules, imports and queries to zelph's own language and REPL.
//!
//! ```no_run
//! use zelph::{Engine, Result};
//!
//! # fn main() -> Result<()> {
//! let z = Engine::new()?;
//!
//! let layer = z.resolve("In")?;
//! let part_of = z.resolve("in")?;
//! let neuron = z.resolve("i1")?;
//! z.fact(neuron, part_of, &[layer])?;      // (i1 in In)
//! # Ok(())
//! # }
//! ```
//!
//! Two properties shape the API and are worth knowing before reading it:
//!
//! - **A node is its hash.** [`Node`] values are stable across calls and
//!   across a save/load cycle, and structurally identical constructions
//!   collapse to one node. Identity is derived, never assigned.
//! - **The engine is single-threaded, a compiled net is not.** [`Engine`] is
//!   neither `Send` nor `Sync`; [`Net`] is both, and may be evaluated from
//!   many threads while another trains it.

mod engine;
mod error;
mod net;

pub use engine::{Channel, Engine};
pub use error::{Error, ErrorKind, Result};
pub use net::{Net, Snapshot};

/// A node of the graph.
///
/// The value *is* the structure's hash, which is why it can be compared,
/// stored and written to disk without a table mapping it to anything.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Node(pub u64);

impl Node {
    /// 0 is not a node, so this is the "no node" case a C caller sees.
    pub fn is_null(self) -> bool {
        self.0 == 0
    }
}

impl std::fmt::Display for Node {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}
