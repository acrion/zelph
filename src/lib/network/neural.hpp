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
#include <vector>

namespace zelph::network
{
    class Zelph;

    // Members of a layer: subjects of (neuron in layer) facts, ordered by
    // ascending node id. This is the canonical neuron order used by
    // NeuralNet::compile and by the layer-wiring helpers.
    ZELPH_EXPORT std::vector<Node> layer_members(const Zelph& z, Node layer);

    // (class doc comment unchanged)
    class ZELPH_EXPORT NeuralNet
    {
    public:
        static std::unique_ptr<NeuralNet> compile(const Zelph& z, const std::vector<Node>& layers);

        size_t                   layer_count() const { return _nodes.size(); }
        const std::vector<Node>& layer_nodes(size_t layer) const { return _nodes.at(layer); }

        std::vector<double> forward(const std::vector<double>& input) const;

        double train_step(const std::vector<double>& input,
                          const std::vector<double>& target,
                          double                     learning_rate);

        void write_back(Zelph& z) const;

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

        std::vector<std::vector<Node>>    _nodes;
        std::vector<std::vector<double>>  _w;
        std::vector<std::vector<uint8_t>> _mask;

        // node -> index within each layer (for the node-addressed API)
        std::vector<ankerl::unordered_dense::map<Node, size_t>> _index;
    };
}