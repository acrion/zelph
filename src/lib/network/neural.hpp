/*
Copyright (c) 2025, 2026 acrion innovations GmbH
Authors: Stefan Zipproth, s.zipproth@acrion.ch

This file is part of zelph, see https://github.com/acrion/zelph and https://zelph.org

zelph is offered under a commercial and under the AGPL license.
For commercial licensing, contact us at https://acrion.ch/sales. For AGPL licensing, see below.

AGPL licensing:

zelph is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

zelph is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with zelph. If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "network_types.hpp"

#include <ankerl/unordered_dense.h>

#include <zelph_export.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <shared_mutex>
#include <vector>

namespace zelph::network
{
    class Zelph;

    // Members of a layer: subjects of (neuron in layer) facts, ordered by
    // ascending node id. This is the canonical neuron order used by
    // NeuralNet::compile and by the layer-wiring helpers.
    ZELPH_EXPORT std::vector<Node> layer_members(const Zelph& z, Node layer);

    // Fully connect two layers with raw synapses, weights drawn uniformly
    // from [-scale, scale] (all zero when scale is 0). Existing synapses are
    // left untouched, so trained weights survive re-wiring and the call is
    // idempotent. Returns how many synapses were created; throws when either
    // layer has no members.
    ZELPH_EXPORT int64_t connect_layers(const Zelph& z, Node from_layer, Node to_layer, double scale, uint64_t seed);

    /// How a hidden layer's pre-activation becomes its activation.
    ///
    /// This is a property of the compiled VIEW, not of the graph: two nets
    /// over the same weights may use different activations, and a net trained
    /// with one must be evaluated with the same one or its output changes.
    /// That is why it is a compile argument and why the default is the one
    /// every existing net was trained with.
    enum class Activation
    {
        /// max(0, x). What every net compiled before this option existed
        /// used, and the default.
        Relu,

        /// max(leak * x, x). The gradient is never exactly zero, which
        /// matters more than it sounds: with plain ReLU a hidden layer whose
        /// every unit is negative for every input has an output of exactly 0
        /// AND a gradient of exactly 0, so no further training can move it.
        /// That state is absorbing, and a small online-trained net can walk
        /// into it and stay there.
        LeakyRelu
    };

    /// The slope below zero for Activation::LeakyRelu.
    constexpr double leaky_relu_slope = 0.01;

    // (class doc comment unchanged)
    // Thread safety: any number of threads may evaluate concurrently
    // (forward, eval_nodes, weights, write_back); a training step
    // (train_step, train_nodes) or set_weights excludes them for its
    // duration. A compiled net may therefore be evaluated from a search
    // thread while another thread trains it. compile() itself is not
    // synchronised - build the net before sharing the handle.
    class ZELPH_EXPORT NeuralNet
    {
    public:
        /// So that both `Activation::LeakyRelu` and `NeuralNet::Activation::
        /// LeakyRelu` name the same thing - the enum is declared next to
        /// `connect_layers` because it is an argument to compilation rather
        /// than a member of the result.
        using Activation = zelph::network::Activation;

        static std::unique_ptr<NeuralNet> compile(const Zelph&             z,
                                                  const std::vector<Node>& layers,
                                                  Activation               activation = Activation::Relu);

        Activation activation() const { return _activation; }

        size_t                   layer_count() const { return _nodes.size(); }
        const std::vector<Node>& layer_nodes(size_t layer) const { return _nodes.at(layer); }

        // `active_input`, when given, lists the indices of the non-zero input
        // slots. It is an optimisation only: the result is the value the
        // dense pass produces, because the skipped terms are multiplications
        // by zero.
        std::vector<double> forward(const std::vector<double>& input,
                                    const std::vector<size_t>* active_input = nullptr) const;

        double train_step(const std::vector<double>& input,
                          const std::vector<double>& target,
                          double                     learning_rate,
                          const std::vector<size_t>* active_input = nullptr);

        void write_back(Zelph& z) const;

        // Copy of every weight matrix, and the inverse. Training is a walk
        // that passes its best point and then leaves it: the criterion that
        // says "stop" can only fire after the fact, so without a way back the
        // weights that get saved are always some epochs past the good ones.
        // A COPY, not a reference: the caller may read it while another
        // thread trains, and a reference into _w would be a race the caller
        // cannot guard against. See the threading note on this class.
        std::vector<std::vector<double>> weights() const;
        void                                    set_weights(const std::vector<std::vector<double>>& w);

        // --- Node-addressed access (graph-driven training) ---
        //
        // These address neurons by their graph node instead of by index,
        // which is what graph-driven training needs: a sample gathered from
        // the graph (e.g. via a reasoning query) is a set of nodes, and the
        // node IS the neuron.

        // Multi-hot encoding: each (node, activation) pair sets that node's
        // slot in the given layer; all other slots are 0. Graded activations
        // (values other than 1) allow feeding quantitative graph data, e.g.
        // edge weights of another net. Throws if a node is not a member of
        // the layer.
        std::vector<double> encode(size_t layer, const std::vector<std::pair<Node, double>>& active) const;

        // train_step with node-addressed input/target. A typical call encodes
        // one fact: input {S, P}, target {O}. Returns the loss before the
        // update.
        double train_nodes(const std::vector<std::pair<Node, double>>& input,
                           const std::vector<std::pair<Node, double>>& target,
                           double                                      learning_rate);

        // forward with node-addressed input; returns (node, score) pairs for
        // the output layer in neuron index order (unsorted).
        std::vector<std::pair<Node, double>> eval_nodes(const std::vector<std::pair<Node, double>>& input) const;

        // Membership test, used by the reasoning engine's ≈ evaluation.
        bool has_node(size_t layer, Node n) const { return _index.at(layer).count(n) != 0; }

    private:
        NeuralNet() = default;

        // The same resolution encode() carries out, in a single pass and
        // without a dense vector: (slot, activation) for each listed neuron,
        // ascending by slot and one entry per slot. A repeated node retains
        // its LAST activation, because that is what writing into a dense
        // vector achieves. Throws when a node is not a member of the layer,
        // as encode() does.
        void gather_active(size_t                                      layer,
                           const std::vector<std::pair<Node, double>>& active,
                           std::vector<std::pair<size_t, double>>&     out) const;

        // Guards _w, the only member that changes after compile(). _nodes,
        // _mask and _index are written once by compile() and read-only
        // afterwards, so they need no protection.
        //
        // Taken shared by the const entry points (forward, eval_nodes,
        // write_back, weights) and exclusively by the mutating ones
        // (train_step, set_weights). It is NOT taken by train_nodes, which
        // delegates to train_step - locking at both levels would deadlock.
        mutable std::shared_mutex _mtx;

        Activation                     _activation{Activation::Relu};
        std::vector<std::vector<Node>> _nodes;

        // Weight matrix k, and the mask beside it, hold one entry per
        // (pre, post) pair. Every matrix is stored row-major by post unit,
        // element (i, j) at j * n_pre + i - EXCEPT matrix 0, which is stored
        // transposed, element (i, j) at i * n_post + j.
        //
        // The input layer is accessed sparsely, and a sparse pass reads it
        // by pre unit: one active input then contributes one contiguous run
        // of n_post weights instead of one scattered load out of each of
        // n_post rows. On a 780x32 net that is 34 rows of 32 against 1088
        // loads spread over 200 KB, and it is worth about a third of the
        // cost of an evaluation.
        //
        // The layout is internal. weights() transposes matrix 0 back on the
        // way out and set_weights() transposes it on the way in, so a caller
        // observes one layout for all of them.
        std::vector<std::vector<double>>  _w;
        std::vector<std::vector<uint8_t>> _mask;

        // node -> index within each layer (for the node-addressed API)
        std::vector<ankerl::unordered_dense::map<Node, size_t>> _index;
    };
}