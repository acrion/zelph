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
use crate::Node;

use std::marker::PhantomData;
use std::ptr;

/// Every weight of a compiled net, and how they split into matrices.
///
/// Training walks past its best point - the criterion that says "stop" can
/// only fire after the fact - so a snapshot taken at the best epoch is the
/// only way the weights that get saved are the good ones.
#[derive(Debug, Clone, PartialEq)]
pub struct Snapshot {
    sizes: Vec<usize>,
    weights: Vec<f64>,
}

impl Snapshot {
    /// The weights, matrices concatenated in layer order.
    pub fn weights(&self) -> &[f64] {
        &self.weights
    }

    /// Element count per weight matrix.
    pub fn shape(&self) -> &[usize] {
        &self.sizes
    }
}

/// A compiled feed-forward view of the graph.
///
/// The handle borrows its engine, so it cannot outlive the graph it is a view
/// of. Unlike the engine it is `Send + Sync`, and that is the whole point:
/// **a net may be evaluated from any number of threads at once, including
/// while another thread trains it.** The synchronisation is inside the C++
/// `NeuralNet`, which takes a shared lock for evaluation and an exclusive one
/// for a training step, so an evaluator always sees a whole set of weights -
/// one epoch's or the next one's, never half of each.
///
/// Training therefore takes `&self` as well. What it does *not* promise is
/// two trainers at once being useful: they serialise, and their gradients are
/// computed against weights the other one is moving.
pub struct Net<'e> {
    engine: *mut zelph_sys::zelph_engine,
    handle: zelph_sys::zelph_net,

    // The lifetime only, deliberately not PhantomData<&'e Engine>: Engine is
    // !Sync, and a net that inherited that could not be shared with the
    // search threads it exists for.
    _engine: PhantomData<&'e ()>,
}

// Sound because every operation reachable through &Net is one the C ABI
// documents as safe to call concurrently. Nothing here touches the graph.
unsafe impl Send for Net<'_> {}
unsafe impl Sync for Net<'_> {}

impl std::fmt::Debug for Net<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Net").field("handle", &self.handle).finish()
    }
}

impl<'e> Net<'e> {
    pub(crate) fn new(engine: *mut zelph_sys::zelph_engine, handle: zelph_sys::zelph_net) -> Net<'e> {
        Net {
            engine,
            handle,
            _engine: PhantomData,
        }
    }

    /// The single highest-scoring output neuron for a multi-hot input.
    ///
    /// This is the hot path of an evaluation, so it allocates nothing: one
    /// node and one score come back through the stack.
    pub fn best(&self, input: &[Node]) -> Result<Option<(Node, f64)>> {
        let mut node: zelph_sys::zelph_node = 0;
        let mut score: f64 = 0.0;
        let mut count: usize = 1;

        check(unsafe {
            zelph_sys::zelph_nn_eval_nodes(
                self.engine,
                self.handle,
                input.as_ptr().cast(),
                ptr::null(),
                input.len(),
                1,
                &mut node,
                &mut score,
                &mut count,
            )
        })?;

        Ok((count > 0).then_some((Node(node), score)))
    }

    /// Forward pass with a multi-hot input: every listed neuron is 1.0.
    ///
    /// The result is sorted by descending score, ties by ascending node.
    /// `top_k` of `None` returns the whole output layer.
    pub fn eval(&self, input: &[Node], top_k: Option<usize>) -> Result<Vec<(Node, f64)>> {
        self.eval_impl(input, None, top_k)
    }

    /// As [`eval`](Net::eval), but each input neuron carries its own
    /// activation - the way to feed a quantity rather than a presence.
    pub fn eval_graded(
        &self,
        input: &[Node],
        activations: &[f64],
        top_k: Option<usize>,
    ) -> Result<Vec<(Node, f64)>> {
        same_length(input, activations)?;
        self.eval_impl(input, Some(activations), top_k)
    }

    fn eval_impl(
        &self,
        input: &[Node],
        activations: Option<&[f64]>,
        top_k: Option<usize>,
    ) -> Result<Vec<(Node, f64)>> {
        let k = match top_k {
            Some(k) => i32::try_from(k).map_err(|_| {
                Error::new(ErrorKind::InvalidArgument, "top_k does not fit in an i32")
            })?,
            None => -1,
        };

        let mut count: usize = 0;
        let query = unsafe {
            zelph_sys::zelph_nn_eval_nodes(
                self.engine,
                self.handle,
                input.as_ptr().cast(),
                activations.map_or(ptr::null(), |a| a.as_ptr()),
                input.len(),
                k,
                ptr::null_mut(),
                ptr::null_mut(),
                &mut count,
            )
        };
        match check(query) {
            Ok(()) => return Ok(Vec::new()),
            Err(e) if e.kind() != ErrorKind::BufferTooSmall => return Err(e),
            Err(_) => {}
        }

        let mut nodes: Vec<Node> = vec![Node(0); count];
        let mut scores: Vec<f64> = vec![0.0; count];

        check(unsafe {
            zelph_sys::zelph_nn_eval_nodes(
                self.engine,
                self.handle,
                input.as_ptr().cast(),
                activations.map_or(ptr::null(), |a| a.as_ptr()),
                input.len(),
                k,
                nodes.as_mut_ptr().cast(),
                scores.as_mut_ptr(),
                &mut count,
            )
        })?;

        Ok(nodes.into_iter().zip(scores).take(count).collect())
    }

    /// One SGD step on a single sample, both sides multi-hot. Returns the
    /// loss *before* the update.
    pub fn train(&self, input: &[Node], target: &[Node], learning_rate: f64) -> Result<f64> {
        self.train_impl(input, None, target, None, learning_rate)
    }

    /// As [`train`](Net::train), with an activation per neuron on either
    /// side. `None` means "all 1.0" for that side.
    ///
    /// A regression target is exactly this: one output neuron carrying the
    /// value to predict.
    pub fn train_graded(
        &self,
        input: &[Node],
        input_activations: Option<&[f64]>,
        target: &[Node],
        target_activations: Option<&[f64]>,
        learning_rate: f64,
    ) -> Result<f64> {
        if let Some(a) = input_activations {
            same_length(input, a)?;
        }
        if let Some(a) = target_activations {
            same_length(target, a)?;
        }
        self.train_impl(
            input,
            input_activations,
            target,
            target_activations,
            learning_rate,
        )
    }

    fn train_impl(
        &self,
        input: &[Node],
        input_activations: Option<&[f64]>,
        target: &[Node],
        target_activations: Option<&[f64]>,
        learning_rate: f64,
    ) -> Result<f64> {
        let mut loss: f64 = 0.0;

        check(unsafe {
            zelph_sys::zelph_nn_train_nodes(
                self.engine,
                self.handle,
                input.as_ptr().cast(),
                input_activations.map_or(ptr::null(), |a| a.as_ptr()),
                input.len(),
                target.as_ptr().cast(),
                target_activations.map_or(ptr::null(), |a| a.as_ptr()),
                target.len(),
                learning_rate,
                &mut loss,
            )
        })?;

        Ok(loss)
    }

    /// Copy the trained weights into the graph's edge-weight store.
    ///
    /// Required before saving: what training changed lives in the compiled
    /// net, and the graph knows nothing about it until this call.
    pub fn write_back(&self) -> Result<()> {
        check(unsafe { zelph_sys::zelph_nn_write_back(self.engine, self.handle) })
    }

    /// Copy every weight out, with the shape needed to put them back.
    pub fn snapshot(&self) -> Result<Snapshot> {
        let mut shape_count: usize = 0;
        match check(unsafe {
            zelph_sys::zelph_nn_snapshot_shape(
                self.engine,
                self.handle,
                ptr::null_mut(),
                &mut shape_count,
            )
        }) {
            Ok(()) => {
                return Ok(Snapshot {
                    sizes: Vec::new(),
                    weights: Vec::new(),
                })
            }
            Err(e) if e.kind() != ErrorKind::BufferTooSmall => return Err(e),
            Err(_) => {}
        }

        let mut sizes: Vec<usize> = vec![0; shape_count];
        check(unsafe {
            zelph_sys::zelph_nn_snapshot_shape(
                self.engine,
                self.handle,
                sizes.as_mut_ptr(),
                &mut shape_count,
            )
        })?;
        sizes.truncate(shape_count);

        let mut weight_count: usize = sizes.iter().sum();
        let mut weights: Vec<f64> = vec![0.0; weight_count];
        check(unsafe {
            zelph_sys::zelph_nn_snapshot(
                self.engine,
                self.handle,
                weights.as_mut_ptr(),
                &mut weight_count,
            )
        })?;
        weights.truncate(weight_count);

        Ok(Snapshot { sizes, weights })
    }

    /// Put a snapshot back. The shape must match the compiled net.
    pub fn restore(&self, snapshot: &Snapshot) -> Result<()> {
        check(unsafe {
            zelph_sys::zelph_nn_restore(
                self.engine,
                self.handle,
                snapshot.weights.as_ptr(),
                snapshot.weights.len(),
                snapshot.sizes.as_ptr(),
                snapshot.sizes.len(),
            )
        })
    }
}

fn same_length(nodes: &[Node], activations: &[f64]) -> Result<()> {
    if nodes.len() == activations.len() {
        return Ok(());
    }

    Err(Error::new(
        ErrorKind::InvalidArgument,
        format!(
            "{} nodes but {} activations",
            nodes.len(),
            activations.len()
        ),
    ))
}
