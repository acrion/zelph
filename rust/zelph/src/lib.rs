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
//! - **A node is a stable number.** [`Node`] values remain consistent across
//!   function calls and throughout a save/load cycle. A named node takes the
//!   subsequent value from the engine's node counter, and a saved file
//!   preserves it alongside its names, ensuring the number remains valid for
//!   a graph loaded from that file, rather than for an engine constructing
//!   the same graph independently. A fact, a list, or a set constant
//!   *is* the hash of its parts, so structurally identical constructs
//!   converge to a single node in every engine, and the number assigned to a
//!   blank node is derived from a hash of its document and either its label
//!   or ordinal.
//! - **The engine is single-threaded, a compiled net is not.** [`Engine`] is
//!   neither `Send` nor `Sync`; [`Net`] is both, and may be evaluated from
//!   many threads while another trains it.

mod engine;
mod error;
mod net;

pub use engine::{Activation, Channel, Engine, Kind};
pub use error::{Error, ErrorKind, Result};
pub use net::{Net, Snapshot};

/// A node of the graph.
///
/// The value corresponds to the node's id: the hash of its parts for a fact,
/// a list, or a set constant; a hash derived from its document and either
/// its label or ordinal for a blank node; and a value from the engine's node
/// counter for a named node, which a saved file records alongside its names
/// (see [`Engine::resolve`]).
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
