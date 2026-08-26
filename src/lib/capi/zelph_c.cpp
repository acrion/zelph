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

#include "capi/zelph_c.h"

#include "interactive.hpp"
#include "network/adjacency_set.hpp"
#include "network/neural.hpp"
#include "network/answer.hpp"
#include "network/reasoning.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

// The engine is the whole zelph stack, which is what Interactive composes:
// the graph, the Janet script engine (needed because rules and stdlib
// modules are Janet-driven) and the command executor. Building on it rather
// than on Reasoning alone is what lets zelph_load and zelph_save BE the
// .load and .save commands instead of a second implementation of them.
struct zelph_engine
{
    explicit zelph_engine(zelph::io::OutputHandler handler)
        : interactive(std::move(handler))
    {
    }

    zelph::console::Interactive interactive;

    // Variables are remembered by name, as the Janet bindings remember them
    // for the duration of a statement: a pattern built in one call has to be
    // queryable in another, and that only works if "A" is the same node both
    // times.
    std::map<std::string, zelph::network::Node> variables;

    // Guards the handle table only. The nets themselves are internally
    // synchronised (see NeuralNet), so a lookup taken shared is all an
    // evaluating thread needs.
    mutable std::shared_mutex                              nets_mutex;
    std::vector<std::unique_ptr<zelph::network::NeuralNet>> nets;

    zelph_engine(const zelph_engine&)            = delete;
    zelph_engine& operator=(const zelph_engine&) = delete;
};

namespace
{
    // Per thread, so two threads evaluating concurrently cannot overwrite
    // each other's diagnosis.
    thread_local std::string t_last_error;

    // Exactly one engine at a time: ScriptEngine keeps a process-wide
    // instance pointer for its Janet C functions, so a second engine would
    // silently redirect every zelph/... call of the first one.
    std::atomic<bool> g_engine_alive{false};

    int32_t fail(const int32_t code, std::string message)
    {
        t_last_error = std::move(message);
        return code;
    }

    int32_t succeed()
    {
        t_last_error.clear();
        return ZELPH_OK;
    }

    // No exception crosses the boundary: a C caller has no way to catch one,
    // and unwinding through a foreign frame is undefined.
    template <typename F>
    int32_t guarded(F&& body)
    {
        try
        {
            return body();
        }
        catch (const std::exception& e)
        {
            return fail(ZELPH_RUNTIME_ERROR, e.what());
        }
        catch (...)
        {
            return fail(ZELPH_RUNTIME_ERROR, "unknown error");
        }
    }

    // The in/out count convention of every array-returning function here:
    // report the size when the buffer is too small, so a caller can ask with
    // capacity 0 and then allocate exactly once.
    template <typename T>
    int32_t write_array(const std::vector<T>& values, T* buffer, size_t* count)
    {
        const size_t capacity = *count;
        *count                = values.size();

        if (values.size() > capacity || (!buffer && !values.empty()))
            return fail(ZELPH_BUFFER_TOO_SMALL,
                        "buffer holds " + std::to_string(capacity) + " elements, " + std::to_string(values.size()) + " needed");

        std::copy(values.begin(), values.end(), buffer);
        return succeed();
    }

    zelph::network::NeuralNet* find_net(zelph_engine* engine, const zelph_net handle)
    {
        std::shared_lock<std::shared_mutex> lock(engine->nets_mutex);
        if (handle < 0 || static_cast<size_t>(handle) >= engine->nets.size())
            return nullptr;
        return engine->nets[static_cast<size_t>(handle)].get();
    }

    // The output of a forward pass, in the order zelph/nn-eval-nodes
    // yields: descending by score, ties broken by ascending node, thus a
    // top-k from either is the identical set in the identical sequence.
    // Common to both the node-addressed and the slot-addressed entry point.
    int32_t write_ranked(std::vector<std::pair<zelph::network::Node, double>> scored,
                         const int32_t                                        top_k,
                         zelph_node*                                          out_nodes,
                         double*                                              out_scores,
                         size_t*                                              count)
    {
        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b)
                  { return a.second != b.second ? a.second > b.second : a.first < b.first; });

        const size_t wanted = top_k < 0 ? scored.size() : std::min(static_cast<size_t>(top_k), scored.size());

        const size_t capacity = *count;
        *count                = wanted;
        if (wanted > capacity || (!out_nodes && wanted > 0))
            return fail(ZELPH_BUFFER_TOO_SMALL,
                        "buffer holds " + std::to_string(capacity) + " elements, " + std::to_string(wanted) + " needed");

        for (size_t i = 0; i < wanted; ++i)
        {
            out_nodes[i] = scored[i].first;
            if (out_scores) out_scores[i] = scored[i].second;
        }
        return succeed();
    }

    // Node-addressed activations, the shape NeuralNet's node API takes. A
    // null activation array means "every listed neuron is 1.0", which is the
    // multi-hot case and the only one a sparse feature vector needs.
    std::vector<std::pair<zelph::network::Node, double>> activations(const zelph_node* nodes,
                                                                     const double*     values,
                                                                     const size_t      count)
    {
        std::vector<std::pair<zelph::network::Node, double>> out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i)
            out.emplace_back(nodes[i], values ? values[i] : 1.0);
        return out;
    }

    std::string language(const zelph_engine* engine, const char* lang)
    {
        return lang ? std::string(lang) : engine->interactive.graph()->lang();
    }
}

const char* zelph_last_error(void)
{
    return t_last_error.c_str();
}

void zelph_string_free(char* text)
{
    std::free(text);
}

int32_t zelph_engine_create(const zelph_output_fn output, void* user_data, zelph_engine** out_engine)
{
    if (!out_engine) return fail(ZELPH_INVALID_ARGUMENT, "out_engine is null");
    *out_engine = nullptr;

    bool expected = false;
    if (!g_engine_alive.compare_exchange_strong(expected, true))
        return fail(ZELPH_RUNTIME_ERROR, "an engine already exists in this process (the script engine is a singleton)");

    const int32_t status = guarded([&]
                                   {
        zelph::io::OutputHandler handler = zelph::io::default_output_handler;
        if (output)
            handler = [output, user_data](const zelph::io::OutputEvent& event)
            { output(user_data, static_cast<int32_t>(event.channel), event.text.c_str(), event.newline ? 1 : 0); };

        *out_engine = new zelph_engine(std::move(handler));
        return succeed(); });

    if (status != ZELPH_OK)
        g_engine_alive.store(false);

    return status;
}

void zelph_engine_destroy(zelph_engine* engine)
{
    if (!engine) return;
    delete engine;
    g_engine_alive.store(false);
}

int32_t zelph_resolve(zelph_engine* engine, const char* name, const char* lang, zelph_node* out_node)
{
    if (!engine || !name || !out_node) return fail(ZELPH_INVALID_ARGUMENT, "engine, name and out_node are required");

    return guarded([&]
                   {
        *out_node = engine->interactive.graph()->node(name, language(engine, lang));
        return succeed(); });
}

int32_t zelph_fact(zelph_engine*     engine,
                   const zelph_node  subject,
                   const zelph_node  predicate,
                   const zelph_node* objects,
                   const size_t      object_count,
                   zelph_node*       out_fact)
{
    if (!engine || !objects || !out_fact) return fail(ZELPH_INVALID_ARGUMENT, "engine, objects and out_fact are required");
    if (!subject || !predicate) return fail(ZELPH_INVALID_ARGUMENT, "subject and predicate must be nodes");
    if (object_count == 0) return fail(ZELPH_INVALID_ARGUMENT, "a fact needs at least one object");

    return guarded([&]
                   {
        zelph::network::adjacency_set set;
        for (size_t i = 0; i < object_count; ++i)
        {
            if (!objects[i]) return fail(ZELPH_INVALID_ARGUMENT, "object " + std::to_string(i) + " is not a node");
            set.insert(objects[i]);
        }

        *out_fact = engine->interactive.graph()->fact(subject, predicate, set);
        return succeed(); });
}

int32_t zelph_fact_parts(zelph_engine*    engine,
                         const zelph_node fact,
                         zelph_node*      out_subject,
                         zelph_node*      out_predicate,
                         zelph_node*      out_objects,
                         size_t*          count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");
    if (!fact)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "fact is 0");
    }

    return guarded([&]
                   {
        auto* graph = engine->interactive.graph();

        const zelph::network::Node predicate = graph->exists(fact) ? graph->parse_relation(fact) : 0;
        if (!predicate)
        {
            *count = 0;
            return fail(ZELPH_INVALID_ARGUMENT, "node is not a fact");
        }

        zelph::network::adjacency_set objects;
        const zelph::network::Node    subject = graph->parse_fact(fact, objects, 0);
        if (!subject)
        {
            *count = 0;
            return fail(ZELPH_INVALID_ARGUMENT, "node is not a fact");
        }

        std::vector<zelph_node> values;
        values.reserve(objects.size());
        for (const zelph::network::Node object : objects)
            values.push_back(object);

        const int32_t status = write_array(values, out_objects, count);
        if (status != ZELPH_OK) return status;

        // Written only once the objects are, so that a caller which asked for
        // the size first never reads a subject it has no objects to go with.
        if (out_subject) *out_subject = subject;
        if (out_predicate) *out_predicate = predicate;

        return succeed(); });
}

int32_t zelph_list(zelph_engine* engine, const zelph_node* elements, const size_t count, zelph_node* out_node)
{
    if (!engine || !out_node) return fail(ZELPH_INVALID_ARGUMENT, "engine and out_node are required");
    if (count > 0 && !elements) return fail(ZELPH_INVALID_ARGUMENT, "elements is null");

    return guarded([&]
                   {
        auto* graph = engine->interactive.graph();

        // The empty cons list IS nil - the node every non-empty list ends
        // at - which is what zelph/list returns for no arguments too.
        if (count == 0)
        {
            *out_node = graph->core.Nil;
            return succeed();
        }

        std::vector<zelph::network::Node> nodes;
        nodes.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            if (!elements[i]) return fail(ZELPH_INVALID_ARGUMENT, "element " + std::to_string(i) + " is not a node");
            nodes.push_back(elements[i]);
        }

        *out_node = graph->list(nodes);
        return succeed(); });
}

int32_t zelph_list_elements(zelph_engine* engine, const zelph_node list, zelph_node* out_nodes, size_t* count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");
    if (!list)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "list is 0");
    }

    return guarded([&]
                   {
        auto* graph = engine->interactive.graph();

        std::vector<zelph_node> values;

        // A cons cell is the fact (car cons cdr): the car is its subject and
        // the cdr its single object. Traversing it is therefore parse_fact,
        // not a list operation - which is why a caller outside the library
        // could not do this with the calls that existed.
        zelph::network::Node cell = list;
        while (cell != graph->core.Nil)
        {
            if (!graph->exists(cell) || graph->parse_relation(cell) != graph->core.Cons)
            {
                *count = 0;
                return fail(ZELPH_INVALID_ARGUMENT, "node is not a cons list");
            }

            zelph::network::adjacency_set objects;
            const zelph::network::Node    car = graph->parse_fact(cell, objects, 0);
            if (!car || objects.empty())
            {
                *count = 0;
                return fail(ZELPH_INVALID_ARGUMENT, "cons cell is malformed");
            }

            values.push_back(car);
            cell = *objects.begin();
        }

        return write_array(values, out_nodes, count); });
}

int32_t zelph_name(zelph_engine* engine, const zelph_node node, const char* lang, char** out_name)
{
    if (!engine || !out_name) return fail(ZELPH_INVALID_ARGUMENT, "engine and out_name are required");
    *out_name = nullptr;
    if (!node) return fail(ZELPH_INVALID_ARGUMENT, "node is 0");

    return guarded([&]
                   {
        const std::string name = engine->interactive.graph()->get_name(node, language(engine, lang), true);
        if (name.empty())
            return succeed(); // unnamed is not an error, it is an answer

        char* copy = static_cast<char*>(std::malloc(name.size() + 1));
        if (!copy) return fail(ZELPH_RUNTIME_ERROR, "out of memory");
        std::memcpy(copy, name.c_str(), name.size() + 1);
        *out_name = copy;
        return succeed(); });
}

int32_t zelph_sources(zelph_engine*    engine,
                      const zelph_node predicate,
                      const zelph_node target,
                      zelph_node*      out_nodes,
                      size_t*          count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");
    if (!predicate || !target)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "predicate and target must be nodes");
    }

    return guarded([&]
                   {
        const zelph::network::adjacency_set sources = engine->interactive.graph()->get_fact_subjects(predicate, target);

        std::vector<zelph_node> values;
        values.reserve(sources.size());
        for (const zelph::network::Node source : sources)
            values.push_back(source);

        return write_array(values, out_nodes, count); });
}

int32_t zelph_load(zelph_engine* engine, const char* path)
{
    if (!engine || !path) return fail(ZELPH_INVALID_ARGUMENT, "engine and path are required");

    return guarded([&]
                   {
        engine->interactive.execute_command({".load", path});
        return succeed(); });
}

int32_t zelph_save(zelph_engine* engine, const char* path)
{
    if (!engine || !path) return fail(ZELPH_INVALID_ARGUMENT, "engine and path are required");

    return guarded([&]
                   {
        engine->interactive.execute_command({".save", path});
        return succeed(); });
}

int32_t zelph_nn_compile(zelph_engine*     engine,
                         const zelph_node* layers,
                         const size_t      layer_count,
                         const int32_t     activation,
                         zelph_net*        out_handle)
{
    if (!engine || !layers || !out_handle) return fail(ZELPH_INVALID_ARGUMENT, "engine, layers and out_handle are required");
    if (layer_count < 2) return fail(ZELPH_INVALID_ARGUMENT, "a network needs at least 2 layers");
    if (activation != ZELPH_ACTIVATION_RELU && activation != ZELPH_ACTIVATION_LEAKY_RELU)
        return fail(ZELPH_INVALID_ARGUMENT, "unknown activation " + std::to_string(activation));

    return guarded([&]
                   {
        std::vector<zelph::network::Node> nodes;
        nodes.reserve(layer_count);
        for (size_t i = 0; i < layer_count; ++i)
        {
            if (!layers[i]) return fail(ZELPH_INVALID_ARGUMENT, "layer " + std::to_string(i) + " is not a node");
            nodes.push_back(layers[i]);
        }

        auto net = zelph::network::NeuralNet::compile(
            *engine->interactive.graph(),
            nodes,
            activation == ZELPH_ACTIVATION_LEAKY_RELU
                ? zelph::network::Activation::LeakyRelu
                : zelph::network::Activation::Relu);

        std::unique_lock<std::shared_mutex> lock(engine->nets_mutex);
        engine->nets.push_back(std::move(net));
        *out_handle = static_cast<zelph_net>(engine->nets.size() - 1);
        return succeed(); });
}

int32_t zelph_nn_connect_layers(zelph_engine*    engine,
                                const zelph_node from_layer,
                                const zelph_node to_layer,
                                const double     scale,
                                const uint64_t   seed,
                                int64_t*         out_created)
{
    if (!engine) return fail(ZELPH_INVALID_ARGUMENT, "engine is required");
    if (!from_layer || !to_layer) return fail(ZELPH_INVALID_ARGUMENT, "both layers must be nodes");

    return guarded([&]
                   {
        const int64_t created = zelph::network::connect_layers(*engine->interactive.graph(), from_layer, to_layer, scale, seed);
        if (out_created) *out_created = created;
        return succeed(); });
}

int32_t zelph_nn_eval_nodes(zelph_engine*     engine,
                            const zelph_net   handle,
                            const zelph_node* input_nodes,
                            const double*     input_activations,
                            const size_t      input_count,
                            const int32_t     top_k,
                            zelph_node*       out_nodes,
                            double*           out_scores,
                            size_t*           count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");
    if (input_count > 0 && !input_nodes) return fail(ZELPH_INVALID_ARGUMENT, "input_nodes is null");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");
    }

    return guarded([&]
                   { return write_ranked(net->eval_nodes(activations(input_nodes, input_activations, input_count)),
                                         top_k,
                                         out_nodes,
                                         out_scores,
                                         count); });
}

int32_t zelph_nn_layer_nodes(zelph_engine*   engine,
                             const zelph_net handle,
                             const size_t    layer,
                             zelph_node*     out_nodes,
                             size_t*         count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");
    }

    return guarded([&]
                   {
        if (layer >= net->layer_count())
        {
            *count = 0;
            return fail(ZELPH_INVALID_ARGUMENT,
                        "layer " + std::to_string(layer) + " of a network with " + std::to_string(net->layer_count()));
        }
        return write_array(net->layer_nodes(layer), out_nodes, count); });
}

int32_t zelph_nn_eval_slots(zelph_engine*   engine,
                            const zelph_net handle,
                            const size_t*   input_slots,
                            const double*   input_activations,
                            const size_t    input_count,
                            const int32_t   top_k,
                            zelph_node*     out_nodes,
                            double*         out_scores,
                            size_t*         count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");
    if (input_count > 0 && !input_slots) return fail(ZELPH_INVALID_ARGUMENT, "input_slots is null");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");
    }

    return guarded([&]
                   { return write_ranked(net->eval_slots(input_slots, input_activations, input_count),
                                         top_k,
                                         out_nodes,
                                         out_scores,
                                         count); });
}

int32_t zelph_nn_train_nodes(zelph_engine*     engine,
                             const zelph_net   handle,
                             const zelph_node* input_nodes,
                             const double*     input_activations,
                             const size_t      input_count,
                             const zelph_node* target_nodes,
                             const double*     target_activations,
                             const size_t      target_count,
                             const double      learning_rate,
                             double*           out_loss)
{
    if (!engine) return fail(ZELPH_INVALID_ARGUMENT, "engine is required");
    if (input_count > 0 && !input_nodes) return fail(ZELPH_INVALID_ARGUMENT, "input_nodes is null");
    if (target_count > 0 && !target_nodes) return fail(ZELPH_INVALID_ARGUMENT, "target_nodes is null");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net) return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");

    return guarded([&]
                   {
        const double loss = net->train_nodes(activations(input_nodes, input_activations, input_count),
                                             activations(target_nodes, target_activations, target_count),
                                             learning_rate);
        if (out_loss) *out_loss = loss;
        return succeed(); });
}

int32_t zelph_nn_train_slots(zelph_engine*     engine,
                             const zelph_net   handle,
                             const size_t*     input_slots,
                             const double*     input_activations,
                             const size_t      input_count,
                             const zelph_node* target_nodes,
                             const double*     target_activations,
                             const size_t      target_count,
                             const double      learning_rate,
                             double*           out_loss)
{
    if (!engine) return fail(ZELPH_INVALID_ARGUMENT, "engine is required");
    if (input_count > 0 && !input_slots) return fail(ZELPH_INVALID_ARGUMENT, "input_slots is null");
    if (target_count > 0 && !target_nodes) return fail(ZELPH_INVALID_ARGUMENT, "target_nodes is null");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net) return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");

    return guarded([&]
                   {
        const double loss = net->train_slots(input_slots, input_activations, input_count,
                                             activations(target_nodes, target_activations, target_count),
                                             learning_rate);
        if (out_loss) *out_loss = loss;
        return succeed(); });
}

int32_t zelph_nn_accumulator_size(zelph_engine* engine, const zelph_net handle, size_t* out_size)
{
    if (!engine || !out_size) return fail(ZELPH_INVALID_ARGUMENT, "engine and out_size are required");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net) return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");

    return guarded([&]
                   {
        *out_size = net->accumulator_size();
        return succeed(); });
}

int32_t zelph_nn_accumulator_set(zelph_engine*   engine,
                                 const zelph_net handle,
                                 const size_t*   slots,
                                 const double*   activations,
                                 const size_t    count,
                                 double*         accumulator,
                                 const size_t    accumulator_size)
{
    if (!engine || !accumulator) return fail(ZELPH_INVALID_ARGUMENT, "engine and accumulator are required");
    if (count > 0 && !slots) return fail(ZELPH_INVALID_ARGUMENT, "slots is null");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net) return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");

    return guarded([&]
                   {
        net->accumulator_set(slots, activations, count, accumulator, accumulator_size);
        return succeed(); });
}

int32_t zelph_nn_accumulator_update(zelph_engine*   engine,
                                    const zelph_net handle,
                                    const size_t*   added,
                                    const double*   added_activations,
                                    const size_t    added_count,
                                    const size_t*   removed,
                                    const double*   removed_activations,
                                    const size_t    removed_count,
                                    double*         accumulator,
                                    const size_t    accumulator_size)
{
    if (!engine || !accumulator) return fail(ZELPH_INVALID_ARGUMENT, "engine and accumulator are required");
    if (added_count > 0 && !added) return fail(ZELPH_INVALID_ARGUMENT, "added is null");
    if (removed_count > 0 && !removed) return fail(ZELPH_INVALID_ARGUMENT, "removed is null");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net) return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");

    return guarded([&]
                   {
        net->accumulator_update(added, added_activations, added_count,
                                removed, removed_activations, removed_count,
                                accumulator, accumulator_size);
        return succeed(); });
}

int32_t zelph_nn_accumulator_eval(zelph_engine*   engine,
                                  const zelph_net handle,
                                  const double*   accumulator,
                                  const size_t    accumulator_size,
                                  const int32_t   top_k,
                                  zelph_node*     out_nodes,
                                  double*         out_scores,
                                  size_t*         count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");
    if (!accumulator) return fail(ZELPH_INVALID_ARGUMENT, "accumulator is null");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");
    }

    return guarded([&]
                   { return write_ranked(net->accumulator_eval(accumulator, accumulator_size),
                                         top_k,
                                         out_nodes,
                                         out_scores,
                                         count); });
}

int32_t zelph_nn_write_back(zelph_engine* engine, const zelph_net handle)
{
    if (!engine) return fail(ZELPH_INVALID_ARGUMENT, "engine is required");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net) return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");

    return guarded([&]
                   {
        net->write_back(*engine->interactive.graph());
        return succeed(); });
}

int32_t zelph_nn_snapshot_shape(zelph_engine* engine, const zelph_net handle, size_t* out_sizes, size_t* count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");
    }

    return guarded([&]
                   {
        const auto          weights = net->weights();
        std::vector<size_t> sizes;
        sizes.reserve(weights.size());
        for (const auto& matrix : weights)
            sizes.push_back(matrix.size());

        return write_array(sizes, out_sizes, count); });
}

int32_t zelph_nn_snapshot(zelph_engine* engine, const zelph_net handle, double* out_weights, size_t* count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");
    }

    return guarded([&]
                   {
        const auto          weights = net->weights();
        std::vector<double> flat;
        for (const auto& matrix : weights)
            flat.insert(flat.end(), matrix.begin(), matrix.end());

        return write_array(flat, out_weights, count); });
}

int32_t zelph_nn_restore(zelph_engine*   engine,
                         const zelph_net handle,
                         const double*   weights,
                         const size_t    weight_count,
                         const size_t*   sizes,
                         const size_t    size_count)
{
    if (!engine || !sizes) return fail(ZELPH_INVALID_ARGUMENT, "engine and sizes are required");
    if (weight_count > 0 && !weights) return fail(ZELPH_INVALID_ARGUMENT, "weights is null");

    zelph::network::NeuralNet* net = find_net(engine, handle);
    if (!net) return fail(ZELPH_INVALID_ARGUMENT, "invalid network handle");

    size_t total = 0;
    for (size_t i = 0; i < size_count; ++i)
        total += sizes[i];
    if (total != weight_count)
        return fail(ZELPH_INVALID_ARGUMENT,
                    "sizes sum to " + std::to_string(total) + ", weight_count is " + std::to_string(weight_count));

    return guarded([&]
                   {
        std::vector<std::vector<double>> matrices;
        matrices.reserve(size_count);
        size_t offset = 0;
        for (size_t i = 0; i < size_count; ++i)
        {
            matrices.emplace_back(weights + offset, weights + offset + sizes[i]);
            offset += sizes[i];
        }

        net->set_weights(matrices);
        return succeed(); });
}

/* --------------------------------------------------------------- reasoning */

int32_t zelph_variable(zelph_engine* engine, const char* name, zelph_node* out_node)
{
    if (!engine || !name || !out_node) return fail(ZELPH_INVALID_ARGUMENT, "engine, name and out_node are required");

    return guarded([&]
                   {
        auto it = engine->variables.find(name);
        if (it != engine->variables.end())
        {
            *out_node = it->second;
            return succeed();
        }

        auto* graph = engine->interactive.graph();
        const zelph::network::Node variable = graph->var();
        graph->set_name(variable, name, graph->lang(), false);
        engine->variables[name] = variable;

        *out_node = variable;
        return succeed(); });
}

int32_t zelph_clear_variables(zelph_engine* engine)
{
    if (!engine) return fail(ZELPH_INVALID_ARGUMENT, "engine is required");

    engine->variables.clear();
    return succeed();
}

namespace
{
    // Shared by zelph_set and zelph_collection: both take a set of nodes and
    // differ only in whether the result has an identity of its own.
    template <typename F>
    int32_t node_set(const zelph_node* elements, size_t count, zelph_node* out_node, F&& build)
    {
        std::unordered_set<zelph::network::Node> set;
        for (size_t i = 0; i < count; ++i)
        {
            if (!elements[i]) return fail(ZELPH_INVALID_ARGUMENT, "element " + std::to_string(i) + " is not a node");
            set.insert(elements[i]);
        }

        *out_node = build(set);
        return succeed();
    }
}

int32_t zelph_set(zelph_engine* engine, const zelph_node* elements, const size_t count, zelph_node* out_node)
{
    if (!engine || !out_node) return fail(ZELPH_INVALID_ARGUMENT, "engine and out_node are required");
    if (count > 0 && !elements) return fail(ZELPH_INVALID_ARGUMENT, "elements is null");

    return guarded([&]
                   { return node_set(elements, count, out_node, [&](const auto& set)
                                     { return engine->interactive.graph()->set(set); }); });
}

int32_t zelph_collection(zelph_engine* engine, const zelph_node* elements, const size_t count, zelph_node* out_node)
{
    if (!engine || !out_node) return fail(ZELPH_INVALID_ARGUMENT, "engine and out_node are required");
    if (count > 0 && !elements) return fail(ZELPH_INVALID_ARGUMENT, "elements is null");

    return guarded([&]
                   { return node_set(elements, count, out_node, [&](const auto& set)
                                     { return engine->interactive.graph()->collection(set); }); });
}

int32_t zelph_negate(zelph_engine* engine, const zelph_node pattern, zelph_node* out_node)
{
    if (!engine || !out_node) return fail(ZELPH_INVALID_ARGUMENT, "engine and out_node are required");
    if (!pattern) return fail(ZELPH_INVALID_ARGUMENT, "pattern is not a node");

    return guarded([&]
                   {
        auto* graph = engine->interactive.graph();
        graph->fact(pattern, graph->core.IsA, {graph->core.Negation});
        *out_node = pattern;
        return succeed(); });
}

int32_t zelph_exists(zelph_engine*     engine,
                     const zelph_node  subject,
                     const zelph_node  predicate,
                     const zelph_node* objects,
                     const size_t      object_count,
                     int32_t*          out_exists)
{
    if (!engine || !objects || !out_exists) return fail(ZELPH_INVALID_ARGUMENT, "engine, objects and out_exists are required");
    if (!subject || !predicate) return fail(ZELPH_INVALID_ARGUMENT, "subject and predicate must be nodes");
    if (object_count == 0) return fail(ZELPH_INVALID_ARGUMENT, "a fact needs at least one object");

    return guarded([&]
                   {
        zelph::network::adjacency_set set;
        for (size_t i = 0; i < object_count; ++i)
        {
            if (!objects[i]) return fail(ZELPH_INVALID_ARGUMENT, "object " + std::to_string(i) + " is not a node");
            set.insert(objects[i]);
        }

        *out_exists = engine->interactive.graph()->check_fact(subject, predicate, set).is_correct() ? 1 : 0;
        return succeed(); });
}

int32_t zelph_targets(zelph_engine*    engine,
                      const zelph_node subject,
                      const zelph_node predicate,
                      zelph_node*      out_nodes,
                      size_t*          count)
{
    if (!engine || !count) return fail(ZELPH_INVALID_ARGUMENT, "engine and count are required");
    if (!subject || !predicate)
    {
        *count = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "subject and predicate must be nodes");
    }

    return guarded([&]
                   {
        const zelph::network::adjacency_set targets = engine->interactive.graph()->get_fact_objects(subject, predicate);

        std::vector<zelph_node> values;
        values.reserve(targets.size());
        for (const zelph::network::Node target : targets)
            values.push_back(target);

        return write_array(values, out_nodes, count); });
}

int32_t zelph_rule(zelph_engine*     engine,
                   const zelph_node* conditions,
                   const size_t      condition_count,
                   const zelph_node* consequences,
                   const size_t      consequence_count,
                   zelph_node*       out_condition_set)
{
    if (!engine || !conditions || !consequences || !out_condition_set)
        return fail(ZELPH_INVALID_ARGUMENT, "engine, conditions, consequences and out_condition_set are required");
    if (condition_count == 0) return fail(ZELPH_INVALID_ARGUMENT, "a rule needs at least one condition");
    if (consequence_count == 0) return fail(ZELPH_INVALID_ARGUMENT, "a rule needs at least one consequence");

    return guarded([&]
                   {
        auto* graph = engine->interactive.graph();

        std::unordered_set<zelph::network::Node> set;
        for (size_t i = 0; i < condition_count; ++i)
        {
            if (!conditions[i]) return fail(ZELPH_INVALID_ARGUMENT, "condition " + std::to_string(i) + " is not a node");
            set.insert(conditions[i]);
        }

        // The conditions become a collection marked as a conjunction, and
        // each consequence is linked to it by the causation predicate. That
        // IS the rule - there is no rule object beyond the graph.
        const zelph::network::Node condition_set = graph->collection(set);
        graph->fact(condition_set, graph->core.IsA, {graph->core.Conjunction});

        for (size_t i = 0; i < consequence_count; ++i)
        {
            if (!consequences[i]) return fail(ZELPH_INVALID_ARGUMENT, "consequence " + std::to_string(i) + " is not a node");
            graph->fact(condition_set, graph->core.Causes, {consequences[i]});
        }

        *out_condition_set = condition_set;
        return succeed(); });
}

namespace
{
    int32_t run_command(zelph_engine* engine, const char* command)
    {
        if (!engine) return fail(ZELPH_INVALID_ARGUMENT, "engine is required");

        return guarded([&]
                       {
            engine->interactive.execute_command({command});
            return succeed(); });
    }
}

int32_t zelph_run(zelph_engine* engine)
{
    return run_command(engine, ".run");
}

int32_t zelph_run_once(zelph_engine* engine)
{
    return run_command(engine, ".run-once");
}

int32_t zelph_run_delta(zelph_engine* engine)
{
    return run_command(engine, ".run-delta");
}

int32_t zelph_set_parallel(zelph_engine* engine, int32_t enabled, int32_t* out_previous)
{
    if (!engine) return fail(ZELPH_INVALID_ARGUMENT, "engine is required");

    return guarded([&]
                   {
        auto* graph = engine->interactive.graph();
        const bool previous = graph->use_parallel();
        if (out_previous) *out_previous = previous ? 1 : 0;
        if (previous != (enabled != 0)) graph->toggle_parallel();
        return succeed(); });
}

int32_t zelph_query(zelph_engine*    engine,
                    const zelph_node pattern,
                    zelph_node*      pairs,
                    size_t*          pair_count,
                    size_t*          row_sizes,
                    size_t*          row_count)
{
    if (!engine || !pair_count || !row_count) return fail(ZELPH_INVALID_ARGUMENT, "engine, pair_count and row_count are required");
    if (!pattern)
    {
        *pair_count = 0;
        *row_count  = 0;
        return fail(ZELPH_INVALID_ARGUMENT, "pattern is not a node");
    }

    return guarded([&]
                   {
        auto* graph = engine->interactive.graph();

        std::vector<std::shared_ptr<zelph::network::Variables>> results;
        graph->set_query_collector(&results);
        graph->apply_rule(0, pattern);
        graph->set_query_collector(nullptr);

        std::vector<zelph_node> flat;
        std::vector<size_t>     sizes;
        sizes.reserve(results.size());
        for (const auto& row : results)
        {
            sizes.push_back(row->size());
            for (const auto& [variable, bound] : *row)
            {
                flat.push_back(variable);
                flat.push_back(bound);
            }
        }

        // Both buffers have to fit before either is written, so a caller that
        // sized only one of them gets a clean refusal rather than half an
        // answer.
        const size_t pair_capacity = *pair_count;
        const size_t row_capacity  = *row_count;
        *pair_count                = flat.size() / 2;
        *row_count                 = sizes.size();

        if (*pair_count > pair_capacity || *row_count > row_capacity
            || (!pairs && *pair_count > 0) || (!row_sizes && *row_count > 0))
            return fail(ZELPH_BUFFER_TOO_SMALL,
                        "need " + std::to_string(*pair_count) + " pairs in "
                            + std::to_string(*row_count) + " rows");

        std::copy(flat.begin(), flat.end(), pairs);
        std::copy(sizes.begin(), sizes.end(), row_sizes);
        return succeed(); });
}

int32_t zelph_cluster(zelph_engine* engine, const char* name)
{
    if (!engine) return fail(ZELPH_INVALID_ARGUMENT, "engine is required");

    return guarded([&]
                   {
        auto* graph = engine->interactive.graph();
        if (!name || std::string(name) == "default")
            graph->deactivate_cluster();
        else
            graph->set_active_cluster(name);
        return succeed(); });
}

int32_t zelph_cluster_active(zelph_engine* engine, char** out_name)
{
    if (!engine || !out_name) return fail(ZELPH_INVALID_ARGUMENT, "engine and out_name are required");
    *out_name = nullptr;

    return guarded([&]
                   {
        const std::string name = engine->interactive.graph()->active_cluster_name();
        if (name.empty() || name == "default")
            return succeed(); // the default is "no cluster", not a name

        char* copy = static_cast<char*>(std::malloc(name.size() + 1));
        if (!copy) return fail(ZELPH_RUNTIME_ERROR, "out of memory");
        std::memcpy(copy, name.c_str(), name.size() + 1);
        *out_name = copy;
        return succeed(); });
}

int32_t zelph_cluster_drop(zelph_engine* engine, const char* name, int64_t* out_removed)
{
    if (!engine || !name) return fail(ZELPH_INVALID_ARGUMENT, "engine and name are required");

    return guarded([&]
                   {
        const size_t removed = engine->interactive.graph()->drop_cluster(name);
        if (out_removed) *out_removed = static_cast<int64_t>(removed);
        return succeed(); });
}

int32_t zelph_cluster_count(zelph_engine* engine, const char* name, int64_t* out_count)
{
    if (!engine || !name || !out_count) return fail(ZELPH_INVALID_ARGUMENT, "engine, name and out_count are required");

    return guarded([&]
                   {
        *out_count = -1;
        for (const auto& [cluster, count] : engine->interactive.graph()->list_clusters())
        {
            if (cluster == name)
            {
                *out_count = static_cast<int64_t>(count);
                break;
            }
        }
        return succeed(); });
}
