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

#include "script/script_engine_impl.hpp"

#include "network/neural.hpp"
#include "network/reasoning.hpp"

#include <janet.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace zelph
{
    network::NeuralNet* ScriptEngine::Impl::get_net(int32_t handle)
    {
        // The returned pointer stays valid after unlocking: the vector owns
        // the nets via unique_ptr and entries are never removed during a
        // session, so only the vector itself needs protection (push_back may
        // reallocate the vector's buffer concurrently).
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (handle < 0 || static_cast<size_t>(handle) >= _neural_nets.size()) return nullptr;
        return _neural_nets[static_cast<size_t>(handle)].get();
    }

    // Read a Janet array/tuple of numbers into a vector<double>.
    std::vector<double> ScriptEngine::Impl::janet_number_vector(Janet v, const char* what)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(v, &data, &len))
            janet_panicf("%s: expected an array or tuple of numbers", what);

        std::vector<double> out;
        out.reserve(static_cast<size_t>(len));
        for (int32_t i = 0; i < len; ++i)
        {
            if (!janet_checktype(data[i], JANET_NUMBER))
                janet_panicf("%s: element %d is not a number", what, i);
            out.push_back(janet_unwrap_number(data[i]));
        }
        return out;
    }

    // Create a raw weighted edge (synapse) from -> to, creating the nodes if
    // necessary. Raw edges carry no predicate and are invisible to reasoning.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_connect(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_nil();

        network::Node from = s_instance->resolve_janet_arg(argv[0]);
        network::Node to   = s_instance->resolve_janet_arg(argv[1]);
        if (!from || !to) janet_panicf("zelph/nn-connect: could not resolve nodes");

        const double w = argc >= 3 ? janet_getnumber(argv, 2) : 1.0;

        s_instance->_n->set_synapse(from, to, w);
        return janet_wrap_nil();
    }

    // Weight of the raw edge from -> to, or nil if no such edge exists.
    Janet ScriptEngine::Impl::janet_cfun_zelph_weight(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        network::Node a = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node b = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!a || !b) return janet_wrap_nil();

        // Synapse entry (or explicitly stored fact probability): its value.
        // Real edge without stored entry: canonical weight 1.
        // Neither: nil.
        if (s_instance->_n->has_synapse(a, b))
            return janet_wrap_number(s_instance->_n->edge_weight(a, b, 1.0));
        if (s_instance->_n->has_right_edge(a, b))
            return janet_wrap_number(1.0);
        return janet_wrap_nil();
    }

    // Set the weight of an existing raw edge.
    Janet ScriptEngine::Impl::janet_cfun_zelph_set_weight(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 3);
        if (!s_instance) return janet_wrap_nil();

        network::Node a = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node b = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!a || !b) janet_panicf("zelph/set-weight: could not resolve nodes");

        std::string err;
        try
        {
            s_instance->_n->set_edge_weight(a, b, janet_getnumber(argv, 2));
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/set-weight: %s (use zelph/nn-connect to create a synapse)", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Compile a feed-forward view of a sub-graph. Argument: indexed collection
    // of layer nodes, input first, output last. Returns an integer handle.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_compile(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, 2);
        if (!s_instance) return janet_wrap_nil();

        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(argv[0], &data, &len) || len < 2)
            janet_panicf("zelph/nn-compile: expected an array of at least 2 layer nodes");

        std::vector<network::Node> layers;
        layers.reserve(static_cast<size_t>(len));
        for (int32_t i = 0; i < len; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg_no_create(data[i]);
            if (!n) janet_panicf("zelph/nn-compile: layer at index %d could not be resolved", i);
            layers.push_back(n);
        }

        // The hidden-layer activation. Optional, and defaulting to the one
        // every net compiled before this argument existed was trained with -
        // a net evaluated with a different activation is a different net.
        network::Activation activation = network::Activation::Relu;
        if (argc >= 2 && !janet_checktype(argv[1], JANET_NIL))
        {
            const std::string name = janet_checktype(argv[1], JANET_KEYWORD)
                                       ? reinterpret_cast<const char*>(janet_unwrap_keyword(argv[1]))
                                       : reinterpret_cast<const char*>(janet_getstring(argv, 1));
            if (name == "leaky-relu")
                activation = network::Activation::LeakyRelu;
            else if (name != "relu")
                janet_panicf("zelph/nn-compile: unknown activation '%s' - use :relu or :leaky-relu", name.c_str());
        }

        std::string err;
        try
        {
            auto net = network::NeuralNet::compile(*s_instance->_n, layers, activation);

            std::lock_guard<std::mutex> lock(s_instance->_state_mutex);
            s_instance->_neural_nets.push_back(std::move(net));
            return janet_wrap_integer(static_cast<int32_t>(s_instance->_neural_nets.size() - 1));
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-compile: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Neurons of a compiled layer in index order (defines input/output order).
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_nodes(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-nodes: invalid network handle");

        const int32_t layer = janet_getinteger(argv, 1);
        if (layer < 0 || static_cast<size_t>(layer) >= net->layer_count())
            janet_panicf("zelph/nn-nodes: layer index out of range");

        const auto& nodes  = net->layer_nodes(static_cast<size_t>(layer));
        JanetArray* result = janet_array(static_cast<int32_t>(nodes.size()));
        for (network::Node n : nodes)
        {
            janet_array_push(result, zelph_wrap_node(n));
        }
        return janet_wrap_array(result);
    }

    // Forward pass. inputs: numbers in zelph/nn-nodes order of layer 0.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_eval(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-eval: invalid network handle");

        std::vector<double> in = janet_number_vector(argv[1], "zelph/nn-eval");

        std::string err;
        try
        {
            const std::vector<double> out    = net->forward(in);
            JanetArray*               result = janet_array(static_cast<int32_t>(out.size()));
            for (const double v : out)
            {
                janet_array_push(result, janet_wrap_number(v));
            }
            return janet_wrap_array(result);
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-eval: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // One SGD step on a single sample; returns the loss before the update.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_train(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, 4);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-train: invalid network handle");

        std::vector<double> in  = janet_number_vector(argv[1], "zelph/nn-train");
        std::vector<double> tgt = janet_number_vector(argv[2], "zelph/nn-train");
        const double        lr  = argc >= 4 ? janet_getnumber(argv, 3) : 0.01;

        std::string err;
        try
        {
            return janet_wrap_number(net->train_step(in, tgt, lr));
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-train: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Copy the compiled net's weights out, as an array of arrays of numbers.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_snapshot(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-snapshot: invalid network handle");

        const auto& w     = net->weights();
        JanetArray* outer = janet_array(static_cast<int32_t>(w.size()));
        for (const auto& matrix : w)
        {
            JanetArray* inner = janet_array(static_cast<int32_t>(matrix.size()));
            for (const double v : matrix)
            {
                janet_array_push(inner, janet_wrap_number(v));
            }
            janet_array_push(outer, janet_wrap_array(inner));
        }
        return janet_wrap_array(outer);
    }

    // Put a snapshot back. Shapes must match the compiled net.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_restore(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-restore: invalid network handle");

        const Janet* outer;
        int32_t      outer_len;
        if (!janet_indexed_view(argv[1], &outer, &outer_len))
            janet_panicf("zelph/nn-restore: expected an array of weight matrices");

        std::vector<std::vector<double>> w;
        w.reserve(static_cast<size_t>(outer_len));
        for (int32_t k = 0; k < outer_len; ++k)
        {
            w.push_back(janet_number_vector(outer[k], "zelph/nn-restore"));
        }

        std::string err;
        try
        {
            net->set_weights(w);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-restore: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Write trained weights back into the graph's edge-weight store.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_write_back(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-write-back: invalid network handle");

        net->write_back(*s_instance->_n);
        return janet_wrap_nil();
    }

    // Parse an indexed collection whose elements are either a node-like value
    // (activation 1) or a [node activation] pair, into (Node, activation)
    // pairs. Node-like values are resolved without creating nodes. Graded
    // activations allow feeding quantitative graph data (e.g. edge weights of
    // another compiled net) as training samples.
    std::vector<std::pair<network::Node, double>> ScriptEngine::Impl::janet_node_activations(Janet v, const char* what)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(v, &data, &len))
            janet_panicf("%s: expected an array or tuple of nodes or [node activation] pairs", what);

        std::vector<std::pair<network::Node, double>> out;
        out.reserve(static_cast<size_t>(len));

        for (int32_t i = 0; i < len; ++i)
        {
            Janet  element    = data[i];
            double activation = 1.0;

            const Janet* pair;
            int32_t      pair_len;
            if ((janet_checktype(element, JANET_TUPLE) || janet_checktype(element, JANET_ARRAY))
                && janet_indexed_view(element, &pair, &pair_len))
            {
                if (pair_len != 2 || !janet_checktype(pair[1], JANET_NUMBER))
                    janet_panicf("%s: element %d must be a node or a [node activation] pair", what, i);
                element    = pair[0];
                activation = janet_unwrap_number(pair[1]);
            }

            network::Node n = s_instance->resolve_janet_arg_no_create(element);
            if (!n) janet_panicf("%s: element %d could not be resolved to an existing node", what, i);
            out.emplace_back(n, activation);
        }
        return out;
    }

    // Fully connect two layers with raw synapses. Existing edges are left
    // untouched, so trained weights survive re-wiring and the call is
    // idempotent. Intended for dense hidden layers; data-driven sparse wiring
    // should use zelph/nn-connect per edge instead.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_connect_layers(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 4);
        if (!s_instance) return janet_wrap_nil();

        network::Node from_layer = s_instance->resolve_janet_arg_no_create(argv[0]);
        network::Node to_layer   = s_instance->resolve_janet_arg_no_create(argv[1]);
        if (!from_layer || !to_layer) janet_panicf("zelph/nn-connect-layers: could not resolve layer nodes");

        const double   scale = argc >= 3 ? janet_getnumber(argv, 2) : 0.1;
        const uint64_t seed  = argc >= 4 ? static_cast<uint64_t>(janet_getnumber(argv, 3)) : 42u;

        std::string err;
        try
        {
            const int64_t created = network::connect_layers(*s_instance->_n, from_layer, to_layer, scale, seed);
            return janet_wrap_number(static_cast<double>(created));
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-connect-layers: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // One SGD step with node-addressed input/target.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_train_nodes(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, 4);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-train-nodes: invalid network handle");

        auto         in  = janet_node_activations(argv[1], "zelph/nn-train-nodes");
        auto         tgt = janet_node_activations(argv[2], "zelph/nn-train-nodes");
        const double lr  = argc >= 4 ? janet_getnumber(argv, 3) : 0.01;

        std::string err;
        try
        {
            return janet_wrap_number(net->train_nodes(in, tgt, lr));
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-train-nodes: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Forward pass with node-addressed input; returns scored output nodes.
    Janet ScriptEngine::Impl::janet_cfun_zelph_nn_eval_nodes(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_nil();

        network::NeuralNet* net = s_instance->get_net(janet_getinteger(argv, 0));
        if (!net) janet_panicf("zelph/nn-eval-nodes: invalid network handle");

        auto          in    = janet_node_activations(argv[1], "zelph/nn-eval-nodes");
        const int32_t top_k = argc >= 3 ? janet_getinteger(argv, 2) : -1;

        std::string err;
        try
        {
            auto scored = net->eval_nodes(in);

            std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b)
                      { return a.second != b.second ? a.second > b.second : a.first < b.first; });

            const size_t n = top_k < 0 ? scored.size() : std::min(static_cast<size_t>(top_k), scored.size());

            JanetArray* result = janet_array(static_cast<int32_t>(n));
            for (size_t i = 0; i < n; ++i)
            {
                Janet pair[2] = {zelph_wrap_node(scored[i].first), janet_wrap_number(scored[i].second)};
                janet_array_push(result, janet_wrap_tuple(janet_tuple_n(pair, 2)));
            }
            return janet_wrap_array(result);
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/nn-eval-nodes: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }
}
