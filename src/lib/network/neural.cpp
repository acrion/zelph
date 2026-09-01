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
#include <random>
#include <shared_mutex>
#include <stdexcept>

using namespace zelph::network;

namespace
{
    // One active input supplies the entire row of the transposed input
    // matrix. `out` gathers the pre-activations of the layer behind it,
    // so the contributions of each unit’s sum remain in ascending input
    // order and the result matches the value a row-major pass produces.
    //
    // An activation equal to 1.0 - the multi-hot case, and the sole one a
    // sparse feature vector has - eliminates the multiplication. That is
    // exact rather than approximate: x * 1.0 is x for every double.
    inline void add_input_row(const double* wt, const size_t n_post, const size_t i, const double v, double* out)
    {
        const double* col = wt + i * n_post;
        if (v == 1.0)
        {
            for (size_t j = 0; j < n_post; ++j)
                out[j] += col[j];
        }
        else
        {
            for (size_t j = 0; j < n_post; ++j)
                out[j] += col[j] * v;
        }
    }

    // The mirror of add_input_row, for an input that has ceased being
    // active. Removing the row is what enables an accumulator to exist at
    // all; it is not the same rounding as excluding the row from a fresh
    // sum, which is the one property an accumulator lacks.
    inline void sub_input_row(const double* wt, const size_t n_post, const size_t i, const double v, double* out)
    {
        const double* col = wt + i * n_post;
        if (v == 1.0)
        {
            for (size_t j = 0; j < n_post; ++j)
                out[j] -= col[j];
        }
        else
        {
            for (size_t j = 0; j < n_post; ++j)
                out[j] -= col[j] * v;
        }
    }

    // The pre-activation of one unit in a dense layer, whose weights are
    // row-major by post unit. Shared by every caller so that the sequence of
    // additions - and therefore the last bit of the result - cannot drift
    // between the dense entry point and the node-addressed one.
    inline double dense_unit(const double* row, const double* in, const size_t n_pre)
    {
        double sum = 0.0;
        for (size_t i = 0; i < n_pre; ++i)
        {
            sum += row[i] * in[i];
        }
        return sum;
    }

    inline void apply_activation(double* v, const size_t n, const NeuralNet::Activation activation)
    {
        if (activation == NeuralNet::Activation::LeakyRelu)
        {
            for (size_t j = 0; j < n; ++j)
                v[j] = std::max(leaky_relu_slope * v[j], v[j]);
        }
        else
        {
            for (size_t j = 0; j < n; ++j)
                v[j] = std::max(0.0, v[j]);
        }
    }

    // Computes the activations of all layers. Hidden layers use the net's
    // activation, the output layer is linear (identity).
    //
    // `act` is the caller’s scratch rather than a return value, and it keeps
    // its capacity between calls: a training run makes one of these per
    // sample, and allocating a vector per layer each time costs more than the
    // arithmetic on a small net. `act[0]` stays empty - the input is where
    // the caller already has it, and copying the whole input width into the
    // scratch is exactly the kind of O(width) step a sparse pass exists to
    // avoid. Layer 0 therefore reads `input` and every later layer `act[k]`.
    void run_forward(const std::vector<std::vector<Node>>&   nodes,
                     const std::vector<std::vector<double>>& w,
                     const std::vector<double>&              input,
                     const std::vector<size_t>*              active_input,
                     const NeuralNet::Activation             activation,
                     std::vector<std::vector<double>>&       act)
    {
        if (input.size() != nodes.front().size())
        {
            throw std::runtime_error("NeuralNet: input size " + std::to_string(input.size())
                                     + " does not match input layer size " + std::to_string(nodes.front().size()));
        }

        act.resize(nodes.size());

        for (size_t k = 0; k + 1 < nodes.size(); ++k)
        {
            const size_t n_pre     = nodes[k].size();
            const size_t n_post    = nodes[k + 1].size();
            const bool   is_output = (k + 2 == nodes.size());

            std::vector<double>& out = act[k + 1];
            out.assign(n_post, 0.0);

            if (k == 0)
            {
                // Only the input layer is known to be sparse; hidden
                // activations are dense after an activation that most units
                // pass. It is also the layer stored transposed.
                const double* wt = w[0].data();
                if (active_input != nullptr)
                {
                    for (const size_t i : *active_input)
                        add_input_row(wt, n_post, i, input[i], out.data());
                }
                else
                {
                    for (size_t i = 0; i < n_pre; ++i)
                        add_input_row(wt, n_post, i, input[i], out.data());
                }
            }
            else
            {
                for (size_t j = 0; j < n_post; ++j)
                {
                    out[j] = dense_unit(w[k].data() + j * n_pre, act[k].data(), n_pre);
                }
            }

            if (!is_output) apply_activation(out.data(), n_post, activation);
        }
    }

    // Most callers enumerate their features in increasing order, and then
    // there is nothing to sort. When one does not, sorting is what
    // maintains the summation order and the slot count identical to the
    // dense pass, so it cannot be omitted.
    void normalise(std::vector<std::pair<size_t, double>>& active)
    {
        std::stable_sort(active.begin(), active.end(), [](const auto& a, const auto& b)
                         { return a.first < b.first; });

        // Stable, so the final entry in a sequence of identical slots is the
        // final one the caller provided – which is the one that would remain
        // when stored in a dense vector.
        auto keep = active.begin();
        for (auto it = active.begin(); it != active.end(); ++it)
        {
            if (keep != active.begin() && (keep - 1)->first == it->first)
                *(keep - 1) = *it;
            else
                *keep++ = *it;
        }
        active.erase(keep, active.end());
    }

    // [pre][post] to [post][pre] and back - the identical permutation
    // regardless of direction, which is why a single function handles both
    // ways.
    std::vector<double> transpose(const std::vector<double>& m, const size_t rows, const size_t cols)
    {
        std::vector<double> out(m.size());
        for (size_t r = 0; r < rows; ++r)
        {
            for (size_t c = 0; c < cols; ++c)
                out[c * rows + r] = m[r * cols + c];
        }
        return out;
    }
}

std::vector<Node> zelph::network::layer_members(const Zelph& z, const Node layer)
{
    adjacency_set members = z.get_fact_subjects(z.core.PartOf, layer);

    std::vector<Node> sorted(members.begin(), members.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

int64_t zelph::network::connect_layers(const Zelph& z, const Node from_layer, const Node to_layer, const double scale, const uint64_t seed)
{
    const std::vector<Node> pre  = layer_members(z, from_layer);
    const std::vector<Node> post = layer_members(z, to_layer);
    if (pre.empty() || post.empty())
    {
        throw std::runtime_error("connect_layers: a layer has no members (expected (neuron in layer) facts)");
    }

    std::mt19937_64                        rng(seed);
    std::uniform_real_distribution<double> dist(-scale, scale);

    int64_t created = 0;
    for (const Node a : pre)
    {
        for (const Node b : post)
        {
            if (z.has_synapse(a, b)) continue; // preserve existing synapses and their weights

            z.set_synapse(a, b, scale == 0.0 ? 0.0 : dist(rng));
            ++created;
        }
    }

    return created;
}

std::unique_ptr<NeuralNet> NeuralNet::compile(const Zelph&             z,
                                              const std::vector<Node>& layers,
                                              const Activation         activation)
{
    if (layers.size() < 2)
    {
        throw std::runtime_error("NeuralNet::compile: need at least an input and an output layer");
    }

    auto nn = std::unique_ptr<NeuralNet>(new NeuralNet());
    nn->_activation = activation;
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
                    // Matrix 0 is kept transposed; see the layout note on _w.
                    const size_t at = k == 0 ? i * post.size() + j : j * pre.size() + i;

                    w[at] = z.edge_weight(pre[i], post[j], 1.0);
                    m[at] = 1;
                }
            }
        }

        nn->_w.push_back(std::move(w));
        nn->_mask.push_back(std::move(m));
    }

    return nn;
}

std::vector<double> NeuralNet::forward(const std::vector<double>& input,
                                       const std::vector<size_t>* active_input) const
{
    thread_local std::vector<std::vector<double>> act;

    std::shared_lock lock(_mtx);
    run_forward(_nodes, _w, input, active_input, _activation, act);
    return act.back();
}

std::vector<std::vector<double>> NeuralNet::weights() const
{
    std::shared_lock lock(_mtx);

    std::vector<std::vector<double>> out = _w;
    // Matrix 0 is kept transposed; a caller observes one arrangement for
    // all of them, so this is where the internal one concludes. The call
    // copies anyway.
    out.front() = transpose(out.front(), _nodes[0].size(), _nodes[1].size());
    return out;
}

double NeuralNet::train_step(const std::vector<double>& input,
                             const std::vector<double>& target,
                             const double               learning_rate,
                             const std::vector<size_t>* active_input)
{
    thread_local std::vector<std::vector<double>> act;

    std::unique_lock lock(_mtx);
    run_forward(_nodes, _w, input, active_input, _activation, act);
    const std::vector<double>& out = act.back();

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
        // act[0] is empty: run_forward accesses the input layer where the
        // caller already has it.
        const std::vector<double>& pre    = k == 0 ? input : act[k];
        const size_t               n_pre  = _nodes[k].size();
        const size_t               n_post = _nodes[k + 1].size();

        if (k == 0)
        {
            // At the input layer prev_delta is not needed and the update is
            // proportional to pre[i], so an inactive input contributes
            // nothing: `w -= lr * delta * 0` leaves the weight bit for bit as
            // it was. Skipping those is what makes a wide sparse input layer
            // trainable. The matrix is the transposed one, so one input’s
            // entire row of updates is contiguous.
            const auto update = [&](const size_t i)
            {
                double*        col  = _w[0].data() + i * n_post;
                const uint8_t* mask = _mask[0].data() + i * n_post;
                const double   p    = pre[i];

                for (size_t j = 0; j < n_post; ++j)
                {
                    if (mask[j]) // only existing synapses are trainable
                    {
                        col[j] -= learning_rate * delta[j] * p;
                    }
                }
            };

            if (active_input != nullptr)
            {
                for (const size_t i : *active_input)
                    update(i);
            }
            else
            {
                for (size_t i = 0; i < n_pre; ++i)
                    update(i);
            }
            break; // k == 0 is the last iteration of a decrementing loop
        }

        std::vector<double> prev_delta(n_pre, 0.0);

        for (size_t j = 0; j < n_post; ++j)
        {
            double*        row  = _w[k].data() + j * n_pre;
            const uint8_t* mask = _mask[k].data() + j * n_pre;

            for (size_t i = 0; i < n_pre; ++i)
            {
                prev_delta[i] += row[i] * delta[j];

                if (mask[i]) // only existing synapses are trainable
                {
                    row[i] -= learning_rate * delta[j] * pre[i];
                }
            }
        }

        // Derivative of hidden layer k. A leaky unit that is off still
        // passes `slope` of the gradient, which is the whole point: with
        // a hard zero here a layer that has gone negative everywhere can
        // never come back.
        for (size_t i = 0; i < n_pre; ++i)
        {
            if (act[k][i] <= 0.0)
            {
                prev_delta[i] = _activation == Activation::LeakyRelu
                                  ? prev_delta[i] * leaky_relu_slope
                                  : 0.0;
            }
        }
        delta = std::move(prev_delta);
    }

    return loss;
}

void NeuralNet::set_weights(const std::vector<std::vector<double>>& w)
{
    std::unique_lock lock(_mtx);
    if (w.size() != _w.size())
    {
        throw std::runtime_error("NeuralNet::set_weights: expected " + std::to_string(_w.size())
                                 + " weight matrices, got " + std::to_string(w.size()));
    }
    for (size_t k = 0; k < _w.size(); ++k)
    {
        if (w[k].size() != _w[k].size())
        {
            throw std::runtime_error("NeuralNet::set_weights: matrix " + std::to_string(k) + " has "
                                     + std::to_string(w[k].size()) + " entries, expected " + std::to_string(_w[k].size()));
        }
    }
    // The mask is a property of the graph, not of the weights, so restoring
    // cannot resurrect a synapse the graph does not have: entries outside the
    // mask are never trained and never read.
    _w = w;

    // What weights() delivered was matrix 0 in the caller’s layout, so this
    // is the reverse of that step and the round trip is exact.
    _w.front() = transpose(_w.front(), _nodes[1].size(), _nodes[0].size());
}

void NeuralNet::write_back(Zelph& z) const
{
    std::shared_lock lock(_mtx);
    for (size_t k = 0; k < _w.size(); ++k)
    {
        const auto& pre  = _nodes[k];
        const auto& post = _nodes[k + 1];

        for (size_t j = 0; j < post.size(); ++j)
        {
            for (size_t i = 0; i < pre.size(); ++i)
            {
                // Matrix 0 is kept transposed; see the layout note on _w.
                const size_t at = k == 0 ? i * post.size() + j : j * pre.size() + i;

                if (_mask[k][at])
                {
                    z.set_edge_weight(pre[i], post[j], _w[k][at]);
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

void NeuralNet::gather_active(const size_t                                layer,
                              const std::vector<std::pair<Node, double>>& active,
                              std::vector<std::pair<size_t, double>>&     out) const
{
    const auto& index = _index.at(layer);

    out.clear();
    out.reserve(active.size());

    bool ascending = true;
    for (const auto& [node, activation] : active)
    {
        const auto it = index.find(node);
        if (it == index.end())
        {
            throw std::runtime_error("NeuralNet: node " + std::to_string(node)
                                     + " is not a member of layer " + std::to_string(layer));
        }
        if (!out.empty() && it->second <= out.back().first) ascending = false;
        out.emplace_back(it->second, activation);
    }

    if (!ascending) normalise(out);
}

void NeuralNet::gather_slots(const size_t*                           slots,
                             const double*                           activations,
                             const size_t                            count,
                             std::vector<std::pair<size_t, double>>& out) const
{
    const size_t width = _nodes.front().size();

    out.clear();
    out.reserve(count);

    bool ascending = true;
    for (size_t i = 0; i < count; ++i)
    {
        if (slots[i] >= width)
        {
            throw std::runtime_error("NeuralNet: input slot " + std::to_string(slots[i])
                                     + " is outside an input layer of " + std::to_string(width));
        }
        if (i > 0 && slots[i] <= slots[i - 1]) ascending = false;
        out.emplace_back(slots[i], activations ? activations[i] : 1.0);
    }

    if (!ascending) normalise(out);
}

double NeuralNet::train_gathered(const std::vector<std::pair<size_t, double>>& active,
                                 const std::vector<std::pair<Node, double>>&   target,
                                 const double                                  learning_rate)
{
    // The dense input vector train_step takes is read at the active slots
    // and nowhere else, so it is constructed once per thread and retained: an
    // epoch over a wide sparse layer would otherwise allocate and zero the
    // entire input width once per sample. Reinstating only the slots that
    // were modified keeps the cost of a sample proportional to what it
    // activates, which is the property these entry points are for.
    thread_local std::vector<double> dense;
    thread_local std::vector<size_t> slots;

    if (dense.size() != _nodes.front().size()) dense.assign(_nodes.front().size(), 0.0);

    slots.clear();
    slots.reserve(active.size());
    for (const auto& [slot, activation] : active)
    {
        dense[slot] = activation;
        slots.push_back(slot);
    }

    // Zero again on the way out, including when train_step throws - the
    // buffer is reused, and a slot left set would silently append to the
    // next sample.
    struct Restore
    {
        std::vector<double>&       v;
        const std::vector<size_t>& written;

        ~Restore()
        {
            for (const size_t slot : written)
                v[slot] = 0.0;
        }
    } restore{dense, slots};

    return train_step(dense, encode(_nodes.size() - 1, target), learning_rate, &slots);
}

double NeuralNet::train_nodes(const std::vector<std::pair<Node, double>>& input,
                              const std::vector<std::pair<Node, double>>& target,
                              const double                                learning_rate)
{
    thread_local std::vector<std::pair<size_t, double>> active;

    gather_active(0, input, active);
    return train_gathered(active, target, learning_rate);
}

double NeuralNet::train_slots(const size_t*                               slots,
                              const double*                               activations,
                              const size_t                                count,
                              const std::vector<std::pair<Node, double>>& target,
                              const double                                learning_rate)
{
    thread_local std::vector<std::pair<size_t, double>> active;

    gather_slots(slots, activations, count, active);
    return train_gathered(active, target, learning_rate);
}

void NeuralNet::first_layer(const std::vector<std::pair<size_t, double>>& active, double* out) const
{
    const size_t  n_post = _nodes[1].size();
    const double* wt     = _w[0].data();

    std::fill(out, out + n_post, 0.0);
    for (const auto& [i, v] : active)
        add_input_row(wt, n_post, i, v, out);
}

const std::vector<double>& NeuralNet::remaining_layers(std::vector<double>& cur) const
{
    // One buffer per thread, reused: the layers after the first are dense,
    // so this is the only place an evaluation needs scratch at all.
    thread_local std::vector<double> next;

    if (_nodes.size() > 2) apply_activation(cur.data(), _nodes[1].size(), _activation);

    for (size_t k = 1; k + 1 < _nodes.size(); ++k)
    {
        const size_t n_pre  = _nodes[k].size();
        const size_t n_post = _nodes[k + 1].size();

        next.assign(n_post, 0.0);
        for (size_t j = 0; j < n_post; ++j)
        {
            next[j] = dense_unit(_w[k].data() + j * n_pre, cur.data(), n_pre);
        }

        if (k + 2 != _nodes.size()) apply_activation(next.data(), n_post, _activation);

        cur.swap(next);
    }

    return cur;
}

const std::vector<double>& NeuralNet::forward_sparse(const std::vector<std::pair<size_t, double>>& active) const
{
    // The dense input vector the layer-addressed entry point handles is what
    // a sparse call would spend most of its time on: allocating and zeroing
    // one slot per input neuron, of which a sparse sample touches a handful.
    // Nothing here requires it - the input layer reads the activation out of
    // the gathered list - so it is never constructed, and the buffer is kept
    // per thread and reused.
    thread_local std::vector<double> cur;

    cur.resize(_nodes[1].size());
    first_layer(active, cur.data());
    return remaining_layers(cur);
}

std::vector<std::pair<Node, double>> NeuralNet::scored_output(const std::vector<double>& out) const
{
    const std::vector<Node>& outputs = _nodes.back();

    std::vector<std::pair<Node, double>> scored;
    scored.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        scored.emplace_back(outputs[i], out[i]);
    }
    return scored;
}

void NeuralNet::check_accumulator_size(const size_t size) const
{
    if (size != accumulator_size())
    {
        throw std::runtime_error("NeuralNet: an accumulator of this net holds "
                                 + std::to_string(accumulator_size()) + " values, not " + std::to_string(size));
    }
}

void NeuralNet::accumulator_set(const size_t* slots,
                                const double* activations,
                                const size_t  count,
                                double*       accumulator,
                                const size_t  size) const
{
    thread_local std::vector<std::pair<size_t, double>> active;

    check_accumulator_size(size);
    gather_slots(slots, activations, count, active);

    std::shared_lock lock(_mtx);
    first_layer(active, accumulator);
}

void NeuralNet::accumulator_update(const size_t* added,
                                   const double* added_activations,
                                   const size_t  added_count,
                                   const size_t* removed,
                                   const double* removed_activations,
                                   const size_t  removed_count,
                                   double*       accumulator,
                                   const size_t  size) const
{
    // Two buffers instead of one: both lists are collected prior to either
    // being applied, so a slot the caller misidentified is rejected with
    // the accumulator unaltered instead of partially updated.
    thread_local std::vector<std::pair<size_t, double>> gone;
    thread_local std::vector<std::pair<size_t, double>> arrived;

    check_accumulator_size(size);
    gather_slots(removed, removed_activations, removed_count, gone);
    gather_slots(added, added_activations, added_count, arrived);

    const size_t     n_post = _nodes[1].size();
    std::shared_lock lock(_mtx);

    const double* wt = _w[0].data();
    for (const auto& [i, v] : gone)
        sub_input_row(wt, n_post, i, v, accumulator);
    for (const auto& [i, v] : arrived)
        add_input_row(wt, n_post, i, v, accumulator);
}

std::vector<std::pair<Node, double>> NeuralNet::accumulator_eval(const double* accumulator,
                                                                 const size_t  size) const
{
    thread_local std::vector<double> cur;

    check_accumulator_size(size);
    cur.assign(accumulator, accumulator + size);

    std::shared_lock lock(_mtx);
    return scored_output(remaining_layers(cur));
}

std::vector<std::pair<Node, double>> NeuralNet::eval_nodes(const std::vector<std::pair<Node, double>>& input) const
{
    thread_local std::vector<std::pair<size_t, double>> active;

    gather_active(0, input, active);

    std::shared_lock lock(_mtx);
    return scored_output(forward_sparse(active));
}

std::vector<std::pair<Node, double>> NeuralNet::eval_slots(const size_t* slots,
                                                           const double* activations,
                                                           const size_t  count) const
{
    thread_local std::vector<std::pair<size_t, double>> active;

    gather_slots(slots, activations, count, active);

    std::shared_lock lock(_mtx);
    return scored_output(forward_sparse(active));
}
