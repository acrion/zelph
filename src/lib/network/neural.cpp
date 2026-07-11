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

#include "neural.hpp"

#include "zelph.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

using namespace zelph::network;

namespace
{
    // Computes the activations of all layers. Hidden layers use ReLU, the
    // output layer is linear (identity).
    std::vector<std::vector<double>> run_forward(const std::vector<std::vector<Node>>&   nodes,
                                                 const std::vector<std::vector<double>>& w,
                                                 const std::vector<double>&              input)
    {
        if (input.size() != nodes.front().size())
        {
            throw std::runtime_error("NeuralNet: input size " + std::to_string(input.size())
                                     + " does not match input layer size " + std::to_string(nodes.front().size()));
        }

        std::vector<std::vector<double>> act;
        act.reserve(nodes.size());
        act.push_back(input);

        for (size_t k = 0; k + 1 < nodes.size(); ++k)
        {
            const size_t n_pre     = nodes[k].size();
            const size_t n_post    = nodes[k + 1].size();
            const bool   is_output = (k + 2 == nodes.size());

            std::vector<double> out(n_post, 0.0);
            for (size_t j = 0; j < n_post; ++j)
            {
                const double* row = w[k].data() + j * n_pre;

                double sum = 0.0;
                for (size_t i = 0; i < n_pre; ++i)
                {
                    sum += row[i] * act[k][i];
                }
                out[j] = is_output ? sum : std::max(0.0, sum);
            }
            act.push_back(std::move(out));
        }
        return act;
    }
}

std::vector<Node> zelph::network::layer_members(const Zelph& z, const Node layer)
{
    adjacency_set members = z.get_fact_subjects(z.core.PartOf, layer);

    std::vector<Node> sorted(members.begin(), members.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

std::unique_ptr<NeuralNet> NeuralNet::compile(const Zelph& z, const std::vector<Node>& layers)
{
    if (layers.size() < 2)
    {
        throw std::runtime_error("NeuralNet::compile: need at least an input and an output layer");
    }

    auto nn = std::unique_ptr<NeuralNet>(new NeuralNet());
    nn->_nodes.reserve(layers.size());

    // in NeuralNet::compile, replacing the previous member-collection loop:
    for (const Node layer : layers)
    {
        std::vector<Node> sorted = layer_members(z, layer);
        if (sorted.empty())
        {
            throw std::runtime_error("NeuralNet::compile: layer " + z.get_name(layer, "", true)
                                     + " has no members (expected (neuron in layer) facts)");
        }

        ankerl::unordered_dense::map<Node, size_t> index;
        index.reserve(sorted.size());
        for (size_t i = 0; i < sorted.size(); ++i)
        {
            index.emplace(sorted[i], i);
        }

        nn->_index.push_back(std::move(index));
        nn->_nodes.push_back(std::move(sorted));
    }

    // Mask and weights come exclusively from the synapse store. Real
    // adjacency edges (fact structure) are deliberately NOT included:
    // with the former has_right_edge probing, structural edges between
    // neurons -- e.g. the object edge from a number to a longer number
    // sharing it as suffix -- silently entered the mask as trainable
    // weight-1 synapses.
    //
    // NOTE: per-edge probing acquires a lock per call. Fine for the
    // foundation; the optimization path for very large nets is a
    // lock-once scan over the weight store.
    for (size_t k = 0; k + 1 < nn->_nodes.size(); ++k)
    {
        const auto& pre  = nn->_nodes[k];
        const auto& post = nn->_nodes[k + 1];

        std::vector<double>  w(pre.size() * post.size(), 0.0);
        std::vector<uint8_t> m(pre.size() * post.size(), 0);

        for (size_t j = 0; j < post.size(); ++j)
        {
            for (size_t i = 0; i < pre.size(); ++i)
            {
                if (z.has_synapse(pre[i], post[j]))
                {
                    w[j * pre.size() + i] = z.edge_weight(pre[i], post[j], 1.0);
                    m[j * pre.size() + i] = 1;
                }
            }
        }

        nn->_w.push_back(std::move(w));
        nn->_mask.push_back(std::move(m));
    }

    return nn;
}

std::vector<double> NeuralNet::forward(const std::vector<double>& input) const
{
    return run_forward(_nodes, _w, input).back();
}

double NeuralNet::train_step(const std::vector<double>& input,
                             const std::vector<double>& target,
                             const double               learning_rate)
{
    const auto  act = run_forward(_nodes, _w, input);
    const auto& out = act.back();

    if (target.size() != out.size())
    {
        throw std::runtime_error("NeuralNet::train_step: target size " + std::to_string(target.size())
                                 + " does not match output layer size " + std::to_string(out.size()));
    }

    // Output delta; the output layer is linear, so dLoss/dPreActivation = y - t.
    double              loss = 0.0;
    std::vector<double> delta(out.size());
    for (size_t j = 0; j < out.size(); ++j)
    {
        const double d = out[j] - target[j];
        loss += 0.5 * d * d;
        delta[j] = d;
    }

    // Backpropagate layer by layer. prev_delta is accumulated with the
    // pre-update weights (row[i] is read before it is written).
    for (size_t k = _w.size(); k-- > 0;)
    {
        const auto&  pre    = act[k];
        const size_t n_pre  = _nodes[k].size();
        const size_t n_post = _nodes[k + 1].size();

        std::vector<double> prev_delta(n_pre, 0.0);

        for (size_t j = 0; j < n_post; ++j)
        {
            double*        row  = _w[k].data() + j * n_pre;
            const uint8_t* mask = _mask[k].data() + j * n_pre;

            for (size_t i = 0; i < n_pre; ++i)
            {
                if (k > 0)
                {
                    prev_delta[i] += row[i] * delta[j];
                }
                if (mask[i]) // only existing synapses are trainable
                {
                    row[i] -= learning_rate * delta[j] * pre[i];
                }
            }
        }

        if (k > 0)
        {
            // ReLU derivative of hidden layer k
            for (size_t i = 0; i < n_pre; ++i)
            {
                if (act[k][i] <= 0.0) prev_delta[i] = 0.0;
            }
            delta = std::move(prev_delta);
        }
    }

    return loss;
}

void NeuralNet::write_back(Zelph& z) const
{
    for (size_t k = 0; k < _w.size(); ++k)
    {
        const auto& pre  = _nodes[k];
        const auto& post = _nodes[k + 1];

        for (size_t j = 0; j < post.size(); ++j)
        {
            for (size_t i = 0; i < pre.size(); ++i)
            {
                if (_mask[k][j * pre.size() + i])
                {
                    z.set_edge_weight(pre[i], post[j], _w[k][j * pre.size() + i]);
                }
            }
        }
    }
}

std::vector<double> NeuralNet::encode(const size_t layer, const std::vector<std::pair<Node, double>>& active) const
{
    if (layer >= _nodes.size())
    {
        throw std::runtime_error("NeuralNet::encode: layer index " + std::to_string(layer) + " out of range");
    }

    std::vector<double> v(_nodes[layer].size(), 0.0);

    const auto& index = _index[layer];
    for (const auto& [node, activation] : active)
    {
        const auto it = index.find(node);
        if (it == index.end())
        {
            throw std::runtime_error("NeuralNet::encode: node " + std::to_string(node)
                                     + " is not a member of layer " + std::to_string(layer));
        }
        v[it->second] = activation;
    }
    return v;
}

double NeuralNet::train_nodes(const std::vector<std::pair<Node, double>>& input,
                              const std::vector<std::pair<Node, double>>& target,
                              const double                                learning_rate)
{
    return train_step(encode(0, input), encode(_nodes.size() - 1, target), learning_rate);
}

std::vector<std::pair<Node, double>> NeuralNet::eval_nodes(const std::vector<std::pair<Node, double>>& input) const
{
    const std::vector<double> out = forward(encode(0, input));

    const std::vector<Node>& outputs = _nodes.back();

    std::vector<std::pair<Node, double>> scored;
    scored.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        scored.emplace_back(outputs[i], out[i]);
    }
    return scored;
}
