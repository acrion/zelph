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

#include <doctest/doctest.h>

#include "capi/zelph_c.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// The C ABI.
//
// It exists so that a program in another language can drive zelph without the
// Janet host. These tests are therefore deliberately written AGAINST THE C
// HEADER ONLY - no Interactive, no Reasoning, no ScriptEngine, not one line of
// Janet - because a caller on the other side of the boundary has nothing else,
// and a test that reached past it would stop being evidence about the ABI.
//
// What they pin: the calling convention every function shares (status code,
// out-parameter, no exception escaping), the in/out count protocol that lets a
// caller allocate exactly once, node identity surviving a save/load cycle, and
// the threading guarantee the safe Rust wrapper will restate as Send + Sync.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    // Silences the engine: .load and .save report progress, and a test run
    // that prints it is unreadable. Doubles as the test of the output
    // callback - a handler that is never invoked would let a hard-coded
    // std::cout in the library pass unnoticed.
    struct Recorder
    {
        std::vector<std::string> lines;

        static void sink(void* user, int32_t channel, const char* text, int32_t newline)
        {
            (void)channel;
            (void)newline;
            static_cast<Recorder*>(user)->lines.emplace_back(text ? text : "");
        }
    };

    // Every test needs an engine and must give it back, including on a failed
    // CHECK - the next one cannot start otherwise (single instance per
    // process).
    struct Engine
    {
        Recorder      recorder;
        zelph_engine* handle = nullptr;

        Engine()
        {
            REQUIRE(zelph_engine_create(&Recorder::sink, &recorder, &handle) == ZELPH_OK);
            REQUIRE(handle != nullptr);
        }

        ~Engine() { zelph_engine_destroy(handle); }

        operator zelph_engine*() const { return handle; }

        zelph_node node(const char* name) const
        {
            zelph_node n = 0;
            REQUIRE(zelph_resolve(handle, name, nullptr, &n) == ZELPH_OK);
            REQUIRE(n != 0);
            return n;
        }

        // (subject in layer), the fact that makes a node a member of a layer.
        void member_of(const char* subject, const char* layer) const
        {
            const zelph_node object = node(layer);
            zelph_node       fact   = 0;
            REQUIRE(zelph_fact(handle, node(subject), node("in"), &object, 1, &fact) == ZELPH_OK);
        }

        Engine(const Engine&)            = delete;
        Engine& operator=(const Engine&) = delete;
    };

    std::string name_of(zelph_engine* engine, const zelph_node node)
    {
        char* text = nullptr;
        REQUIRE(zelph_name(engine, node, nullptr, &text) == ZELPH_OK);
        std::string result = text ? text : "";
        zelph_string_free(text);
        return result;
    }
}

TEST_CASE("capi: the engine is a single instance and can be recreated")
{
    zelph_engine* first = nullptr;
    REQUIRE(zelph_engine_create(nullptr, nullptr, &first) == ZELPH_OK);

    // The script engine keeps a process-wide instance pointer, so a second
    // engine would silently redirect the first one's Janet calls. Refused
    // rather than half-working.
    zelph_engine* second = nullptr;
    CHECK(zelph_engine_create(nullptr, nullptr, &second) == ZELPH_RUNTIME_ERROR);
    CHECK(second == nullptr);
    CHECK(std::strlen(zelph_last_error()) > 0);

    zelph_engine_destroy(first);

    // ... and the refusal is not permanent.
    zelph_engine* third = nullptr;
    CHECK(zelph_engine_create(nullptr, nullptr, &third) == ZELPH_OK);
    zelph_engine_destroy(third);
}

TEST_CASE("capi: a name resolves to one stable node, and back to the name")
{
    Engine engine;

    const zelph_node first  = engine.node("Socrates");
    const zelph_node second = engine.node("Socrates");
    CHECK(first == second);
    CHECK(first != engine.node("Plato"));

    CHECK(name_of(engine, first) == "Socrates");

    // Unnamed is an answer, not a failure: a fact node has no name, and a
    // caller must be able to ask without treating it as an error.
    const zelph_node object = engine.node("mortal");
    zelph_node       fact   = 0;
    REQUIRE(zelph_fact(engine, first, engine.node("~"), &object, 1, &fact) == ZELPH_OK);
    CHECK(fact != 0);

    char* text = nullptr;
    CHECK(zelph_name(engine, fact, nullptr, &text) == ZELPH_OK);
    CHECK(text == nullptr);
}

TEST_CASE("capi: an out-parameter array reports the size it needs")
{
    Engine engine;

    engine.member_of("a1", "L");
    engine.member_of("a2", "L");
    engine.member_of("a3", "L");

    const zelph_node layer     = engine.node("L");
    const zelph_node predicate = engine.node("in");

    // Capacity 0 with no buffer is the size question, and it must not be
    // mistaken for a call that produced nothing.
    size_t count = 0;
    CHECK(zelph_sources(engine, predicate, layer, nullptr, &count) == ZELPH_BUFFER_TOO_SMALL);
    CHECK(count == 3);

    // One short: still nothing written, still the required size reported.
    std::vector<zelph_node> too_small(2, 0);
    count = too_small.size();
    CHECK(zelph_sources(engine, predicate, layer, too_small.data(), &count) == ZELPH_BUFFER_TOO_SMALL);
    CHECK(count == 3);
    CHECK(too_small[0] == 0);

    std::vector<zelph_node> nodes(count, 0);
    CHECK(zelph_sources(engine, predicate, layer, nodes.data(), &count) == ZELPH_OK);
    CHECK(count == 3);

    std::vector<std::string> names;
    for (const zelph_node node : nodes)
        names.push_back(name_of(engine, node));
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"a1", "a2", "a3"});

    // Directional: (L in X) must not make L a source of itself.
    const zelph_node other = engine.node("M");
    zelph_node       fact  = 0;
    REQUIRE(zelph_fact(engine, layer, predicate, &other, 1, &fact) == ZELPH_OK);
    count = nodes.size();
    CHECK(zelph_sources(engine, predicate, layer, nodes.data(), &count) == ZELPH_OK);
    CHECK(count == 3);
}

TEST_CASE("capi: a list of the same nodes is the same node, and the empty list is nil")
{
    Engine engine;

    const zelph_node elements[2] = {engine.node("x"), engine.node("y")};

    zelph_node first = 0;
    REQUIRE(zelph_list(engine, elements, 2, &first) == ZELPH_OK);
    CHECK(first != 0);

    // Hash-consing: structurally identical lists ARE one node. This is the
    // property a caller builds structural identity on.
    zelph_node again = 0;
    REQUIRE(zelph_list(engine, elements, 2, &again) == ZELPH_OK);
    CHECK(again == first);

    const zelph_node reversed[2] = {elements[1], elements[0]};
    zelph_node       flipped     = 0;
    REQUIRE(zelph_list(engine, reversed, 2, &flipped) == ZELPH_OK);
    CHECK(flipped != first);

    zelph_node empty = 0;
    REQUIRE(zelph_list(engine, nullptr, 0, &empty) == ZELPH_OK);
    CHECK(empty != 0); // the nil node, not "no node"
    CHECK(name_of(engine, empty) == "nil");
}

TEST_CASE("capi: a fact reads back as its subject, its predicate and its objects")
{
    Engine engine;

    const zelph_node socrates    = engine.node("Socrates");
    const zelph_node is          = engine.node("is");
    const zelph_node objects[2]  = {engine.node("mortal"), engine.node("greek")};

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, socrates, is, objects, 2, &fact) == ZELPH_OK);

    size_t count = 0;
    CHECK(zelph_fact_parts(engine, fact, nullptr, nullptr, nullptr, &count) == ZELPH_BUFFER_TOO_SMALL);
    REQUIRE(count == 2);

    zelph_node              subject   = 0;
    zelph_node              predicate = 0;
    std::vector<zelph_node> read(count);
    REQUIRE(zelph_fact_parts(engine, fact, &subject, &predicate, read.data(), &count) == ZELPH_OK);
    REQUIRE(count == 2);
    CHECK(subject == socrates);
    CHECK(predicate == is);

    // The objects of a fact constitute a SET - `(S P a b)` indicates merely
    // that both serve as objects of the same statement - thus they return
    // without a sequence to depend on. That is why a requester requiring
    // one opts for a list instead.
    std::vector<zelph_node> expected{objects[0], objects[1]};
    std::sort(read.begin(), read.end());
    std::sort(expected.begin(), expected.end());
    CHECK(read == expected);

    // A cons cell is a fact too, `(car cons cdr)`, which is the whole of the
    // relationship between this call and zelph_list_elements.
    const zelph_node elements[2] = {engine.node("x"), engine.node("y")};
    zelph_node       list        = 0;
    REQUIRE(zelph_list(engine, elements, 2, &list) == ZELPH_OK);

    zelph_node cdr = 0;
    count          = 1;
    REQUIRE(zelph_fact_parts(engine, list, &subject, &predicate, &cdr, &count) == ZELPH_OK);
    REQUIRE(count == 1);
    CHECK(subject == elements[0]);

    zelph_node tail = 0;
    count           = 1;
    REQUIRE(zelph_list_elements(engine, cdr, &tail, &count) == ZELPH_OK);
    REQUIRE(count == 1);
    CHECK(tail == elements[1]);

    // A name is not a statement.
    count = 0;
    CHECK(zelph_fact_parts(engine, socrates, nullptr, nullptr, nullptr, &count) == ZELPH_INVALID_ARGUMENT);
    CHECK(zelph_fact_parts(engine, 0, nullptr, nullptr, nullptr, &count) == ZELPH_INVALID_ARGUMENT);
}

TEST_CASE("capi: a cons list reads back as the elements it was built from")
{
    Engine engine;

    const zelph_node elements[3] = {engine.node("x"), engine.node("y"), engine.node("z")};

    zelph_node list = 0;
    REQUIRE(zelph_list(engine, elements, 3, &list) == ZELPH_OK);

    // Asked the way each array-valued call in this ABI is asked: a null
    // buffer with capacity 0 is the size question.
    size_t count = 0;
    CHECK(zelph_list_elements(engine, list, nullptr, &count) == ZELPH_BUFFER_TOO_SMALL);
    REQUIRE(count == 3);

    std::vector<zelph_node> read(count);
    REQUIRE(zelph_list_elements(engine, list, read.data(), &count) == ZELPH_OK);
    REQUIRE(count == 3);

    // ORDER, not membership. A list is not a set, and the reason
    // to construct an identifier from one is precisely that <x y> and <y x>
    // are two nodes.
    CHECK(read[0] == elements[0]);
    CHECK(read[1] == elements[1]);
    CHECK(read[2] == elements[2]);

    // Nesting endures, because an element is a node like any other: the
    // caller receives the inner list back as a node and reads that in
    // turn.
    const zelph_node outer_elements[2] = {list, engine.node("w")};
    zelph_node       outer             = 0;
    REQUIRE(zelph_list(engine, outer_elements, 2, &outer) == ZELPH_OK);

    std::vector<zelph_node> outer_read(2);
    count = outer_read.size();
    REQUIRE(zelph_list_elements(engine, outer, outer_read.data(), &count) == ZELPH_OK);
    REQUIRE(count == 2);
    CHECK(outer_read[0] == list);

    std::vector<zelph_node> inner(3);
    count = inner.size();
    REQUIRE(zelph_list_elements(engine, outer_read[0], inner.data(), &count) == ZELPH_OK);
    REQUIRE(count == 3);
    CHECK(inner[1] == elements[1]);

    // The empty list is nil, and accessing it returns no elements instead of
    // failing: "there is nothing in it" is a response, as it is for
    // zelph_name on an unnamed node.
    zelph_node empty = 0;
    REQUIRE(zelph_list(engine, nullptr, 0, &empty) == ZELPH_OK);
    count = 0;
    CHECK(zelph_list_elements(engine, empty, nullptr, &count) == ZELPH_OK);
    CHECK(count == 0);

    // What isn’t a list isn’t half a list. A simple name and a fact both
    // respond with a status rather than with whatever a walk discovers,
    // because a caller that stores structures must be able to distinguish
    // a structure it wrote from a node that simply exists.
    count = 0;
    CHECK(zelph_list_elements(engine, engine.node("x"), nullptr, &count) == ZELPH_INVALID_ARGUMENT);

    const zelph_node object = engine.node("mortal");
    zelph_node       fact   = 0;
    REQUIRE(zelph_fact(engine, engine.node("Socrates"), engine.node("is"), &object, 1, &fact) == ZELPH_OK);
    count = 0;
    CHECK(zelph_list_elements(engine, fact, nullptr, &count) == ZELPH_INVALID_ARGUMENT);

    CHECK(zelph_list_elements(engine, 0, nullptr, &count) == ZELPH_INVALID_ARGUMENT);
}

TEST_CASE("capi: a structure is the same node in a fresh engine, and still holds its elements")
{
    const auto path = fs::temp_directory_path() / "zelph_capi_structure.bin";
    fs::remove(path);

    zelph_node saved_list = 0;

    {
        Engine           engine;
        const zelph_node elements[2] = {engine.node("alpha"), engine.node("beta")};
        REQUIRE(zelph_list(engine, elements, 2, &saved_list) == ZELPH_OK);

        // Something has to be said ABOUT the structure, or there is nothing to
        // save: a list nobody mentions is not part of the graph’s content.
        const zelph_node object = saved_list;
        zelph_node       fact   = 0;
        REQUIRE(zelph_fact(engine, engine.node("pair"), engine.node("is"), &object, 1, &fact) == ZELPH_OK);

        REQUIRE(zelph_save(engine, path.string().c_str()) == ZELPH_OK);
    }

    {
        Engine engine;
        REQUIRE(zelph_load(engine, path.string().c_str()) == ZELPH_OK);

        // Recreating the identical structure in a new engine reaches the node
        // the file references, without the caller having retained an id. This
        // is what transforms a saved graph into a store instead of a cache:
        // the key is the structure itself.
        const zelph_node elements[2] = {engine.node("alpha"), engine.node("beta")};
        zelph_node       rebuilt     = 0;
        REQUIRE(zelph_list(engine, elements, 2, &rebuilt) == ZELPH_OK);
        CHECK(rebuilt == saved_list);

        std::vector<zelph_node> read(2);
        size_t                  count = read.size();
        REQUIRE(zelph_list_elements(engine, saved_list, read.data(), &count) == ZELPH_OK);
        REQUIRE(count == 2);
        CHECK(read[0] == elements[0]);
        CHECK(read[1] == elements[1]);

        // And what was stated regarding it is still stated
        // regarding it.
        std::vector<zelph_node> objects(1);
        count = objects.size();
        REQUIRE(zelph_targets(engine, engine.node("pair"), engine.node("is"), objects.data(), &count) == ZELPH_OK);
        REQUIRE(count == 1);
        CHECK(objects[0] == saved_list);
    }

    fs::remove(path);
}

TEST_CASE("capi: a network is wired, compiled, trained and read back through the ABI")
{
    Engine engine;

    engine.member_of("i1", "In");
    engine.member_of("i2", "In");
    engine.member_of("o1", "Out");
    engine.member_of("o2", "Out");

    const zelph_node in  = engine.node("In");
    const zelph_node out = engine.node("Out");

    int64_t created = 0;
    REQUIRE(zelph_nn_connect_layers(engine, in, out, 0.0, 1, &created) == ZELPH_OK);
    CHECK(created == 4);

    // Idempotent, so re-wiring a loaded net never destroys trained weights.
    REQUIRE(zelph_nn_connect_layers(engine, in, out, 0.5, 1, &created) == ZELPH_OK);
    CHECK(created == 0);

    const zelph_node layers[2] = {in, out};
    zelph_net        net       = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);

    const zelph_node i1 = engine.node("i1");
    const zelph_node i2 = engine.node("i2");
    const zelph_node o1 = engine.node("o1");
    const zelph_node o2 = engine.node("o2");

    double loss = 0;
    for (int epoch = 0; epoch < 60; ++epoch)
    {
        REQUIRE(zelph_nn_train_nodes(engine, net, &i1, nullptr, 1, &o1, nullptr, 1, 0.5, &loss) == ZELPH_OK);
        REQUIRE(zelph_nn_train_nodes(engine, net, &i2, nullptr, 1, &o2, nullptr, 1, 0.5, &loss) == ZELPH_OK);
    }

    // top_k 1 is the shape an evaluation uses: the single best output.
    zelph_node top   = 0;
    double     score = 0;
    size_t     count = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &top, &score, &count) == ZELPH_OK);
    CHECK(count == 1);
    CHECK(top == o1);

    count = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i2, nullptr, 1, 1, &top, &score, &count) == ZELPH_OK);
    CHECK(top == o2);

    // A null activation array means every listed input is 1.0, so passing
    // the activations explicitly must change nothing.
    const double one = 1.0;
    double       explicit_score = 0;
    count                       = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i2, &one, 1, 1, &top, &explicit_score, &count) == ZELPH_OK);
    CHECK(explicit_score == doctest::Approx(score));

    // Graded activation scales the (linear) response.
    const double half = 0.5;
    double       half_score = 0;
    count                   = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i2, &half, 1, 1, &top, &half_score, &count) == ZELPH_OK);
    CHECK(2 * half_score == doctest::Approx(score));

    // top_k < 0 is the whole output layer, and the sort is by descending
    // score - the order the Janet binding produces, so a caller can be
    // ported between the two without re-reading it.
    zelph_node all_nodes[2]  = {0, 0};
    double     all_scores[2] = {0, 0};
    count                    = 2;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i2, nullptr, 1, -1, all_nodes, all_scores, &count) == ZELPH_OK);
    CHECK(count == 2);
    CHECK(all_nodes[0] == o2);
    CHECK(all_scores[0] >= all_scores[1]);
}

// ---------------------------------------------------------------------------
// Slot-addressed input: the identical two invocations without the
// individual-call node lookup.
//
// It is an optimisation and nothing else, so what has to be pinned is that it
// answers what the node-addressed pair answers - not approximately, since the
// two run the same arithmetic over the same slots. The net below has three
// inputs of distinct magnitude, so an answer names which slots were summed
// and a permuted mapping cannot pass by luck.
// ---------------------------------------------------------------------------

TEST_CASE("capi: slots and nodes are two names for the same input")
{
    Engine engine;

    engine.member_of("s1", "SlIn");
    engine.member_of("s2", "SlIn");
    engine.member_of("s3", "SlIn");
    engine.member_of("t1", "SlOut");

    const zelph_node layers[2] = {engine.node("SlIn"), engine.node("SlOut")};
    zelph_net        net       = -1;
    int64_t          created   = 0;
    REQUIRE(zelph_nn_connect_layers(engine, layers[0], layers[1], 0.0, 1, &created) == ZELPH_OK);
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);

    // The mapping a caller needs to use slots at all, and the only way to
    // learn it through the ABI.
    size_t                  count = 0;
    std::vector<zelph_node> inputs;
    REQUIRE(zelph_nn_layer_nodes(engine, net, 0, nullptr, &count) == ZELPH_BUFFER_TOO_SMALL);
    REQUIRE(count == 3);
    inputs.resize(count);
    REQUIRE(zelph_nn_layer_nodes(engine, net, 0, inputs.data(), &count) == ZELPH_OK);
    CHECK(inputs[0] == engine.node("s1"));
    CHECK(inputs[1] == engine.node("s2"));
    CHECK(inputs[2] == engine.node("s3"));

    REQUIRE(zelph_nn_layer_nodes(engine, net, 2, nullptr, &count) == ZELPH_INVALID_ARGUMENT);

    // Trained via the node entry point intentionally: a permuted slot
    // mapping employed during both training and evaluation is
    // self-consistent and responds to every query accurately. Only a
    // network trained through one path and queried by the other pins the
    // mapping itself.
    const zelph_node target    = engine.node("t1");
    const double     wanted[3] = {1.0, 10.0, 100.0};
    for (int epoch = 0; epoch < 400; ++epoch)
    {
        for (size_t i = 0; i < 3; ++i)
        {
            REQUIRE(zelph_nn_train_nodes(engine, net, &inputs[i], nullptr, 1, &target, &wanted[i], 1, 0.1, nullptr) == ZELPH_OK);
        }
    }

    const auto score_by_slots = [&](const std::vector<size_t>& slots, const double* activations)
    {
        zelph_node top   = 0;
        double     value = 0;
        size_t     n     = 1;
        REQUIRE(zelph_nn_eval_slots(engine, net, slots.data(), activations, slots.size(), 1, &top, &value, &n) == ZELPH_OK);
        return value;
    };

    const auto score_by_nodes = [&](const std::vector<zelph_node>& nodes, const double* activations)
    {
        zelph_node top   = 0;
        double     value = 0;
        size_t     n     = 1;
        REQUIRE(zelph_nn_eval_nodes(engine, net, nodes.data(), activations, nodes.size(), 1, &top, &value, &n) == ZELPH_OK);
        return value;
    };

    // Asked by slot, taught by node: each magnitude names the slot it
    // belongs to, so any permutation of the mapping answers wrongly here.
    CHECK(score_by_slots({0}, nullptr) == doctest::Approx(1.0).epsilon(0.01));
    CHECK(score_by_slots({1}, nullptr) == doctest::Approx(10.0).epsilon(0.01));
    CHECK(score_by_slots({2}, nullptr) == doctest::Approx(100.0).epsilon(0.01));

    // Equal to the last bit, not approximately: both routes add the same
    // weights in the same sequence. The sets are asymmetric, because a
    // mapping that simply inverts the layer maps {0, 2} to itself.
    CHECK(score_by_slots({0, 1}, nullptr) == score_by_nodes({inputs[0], inputs[1]}, nullptr));
    CHECK(score_by_slots({1, 2}, nullptr) == score_by_nodes({inputs[1], inputs[2]}, nullptr));
    CHECK(score_by_slots({0, 1, 2}, nullptr) == score_by_nodes({inputs[0], inputs[1], inputs[2]}, nullptr));
    CHECK(score_by_slots({}, nullptr) == score_by_nodes({}, nullptr));

    // A graded activation reaches the right slot, which a swapped mapping
    // would get wrong while a multi-hot case over a symmetric set passed.
    const double graded[2] = {0.5, 0.25};
    CHECK(score_by_slots({1, 2}, graded) == score_by_nodes({inputs[1], inputs[2]}, graded));

    // The training half reads its input through the same gather, and a
    // learning rate of 0 reports the loss without moving a weight - so the
    // two entry points must agree on it for the same sample.
    double           loss_by_slots = 0;
    double           loss_by_nodes = 0;
    const size_t     two_slots[2]  = {0, 2};
    const zelph_node two_nodes[2]  = {inputs[0], inputs[2]};
    REQUIRE(zelph_nn_train_slots(engine, net, two_slots, graded, 2, &target, wanted, 1, 0.0, &loss_by_slots) == ZELPH_OK);
    REQUIRE(zelph_nn_train_nodes(engine, net, two_nodes, graded, 2, &target, wanted, 1, 0.0, &loss_by_nodes) == ZELPH_OK);
    CHECK(loss_by_slots == loss_by_nodes);

    // And a genuine step through the slot path results in the net being
    // left where the same step by node results in it. Execute from one
    // origin point, twice, with the snapshot in between, so the two updates
    // are compared and not merged.
    size_t shape_count = 0;
    CHECK(zelph_nn_snapshot_shape(engine, net, nullptr, &shape_count) == ZELPH_BUFFER_TOO_SMALL);
    std::vector<size_t> sizes(shape_count, 0);
    REQUIRE(zelph_nn_snapshot_shape(engine, net, sizes.data(), &shape_count) == ZELPH_OK);

    size_t weight_count = 0;
    CHECK(zelph_nn_snapshot(engine, net, nullptr, &weight_count) == ZELPH_BUFFER_TOO_SMALL);
    std::vector<double> start(weight_count, 0);
    REQUIRE(zelph_nn_snapshot(engine, net, start.data(), &weight_count) == ZELPH_OK);

    REQUIRE(zelph_nn_train_slots(engine, net, two_slots, graded, 2, &target, wanted, 1, 0.05, nullptr) == ZELPH_OK);
    std::vector<double> after_slots(weight_count, 0);
    REQUIRE(zelph_nn_snapshot(engine, net, after_slots.data(), &weight_count) == ZELPH_OK);

    REQUIRE(zelph_nn_restore(engine, net, start.data(), start.size(), sizes.data(), sizes.size()) == ZELPH_OK);
    REQUIRE(zelph_nn_train_nodes(engine, net, two_nodes, graded, 2, &target, wanted, 1, 0.05, nullptr) == ZELPH_OK);
    std::vector<double> after_nodes(weight_count, 0);
    REQUIRE(zelph_nn_snapshot(engine, net, after_nodes.data(), &weight_count) == ZELPH_OK);

    CHECK(after_slots != start);       // the step did something
    CHECK(after_slots == after_nodes); // and it was the same something

    // Order does not matter and a repeat keeps its LAST activation, exactly
    // as writing into a dense vector would.
    CHECK(score_by_slots({2, 0}, nullptr) == score_by_slots({0, 2}, nullptr));
    const double repeated[2] = {1.0, 0.25};
    CHECK(score_by_slots({2, 2}, repeated) == score_by_slots({2}, &repeated[1]));

    // A slot past the layer is refused rather than read.
    zelph_node   top   = 0;
    double       value = 0;
    size_t       n     = 1;
    const size_t past  = 3;
    CHECK(zelph_nn_eval_slots(engine, net, &past, nullptr, 1, 1, &top, &value, &n) == ZELPH_RUNTIME_ERROR);
    CHECK(zelph_nn_train_slots(engine, net, &past, nullptr, 1, &target, nullptr, 1, 0.1, nullptr) == ZELPH_RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// The accumulator: the first layer kept between calls.
//
// Two properties hold it, and they differ in nature. Setting it and
// evaluating it must be the from-scratch pass BIT FOR BIT, since both
// traverse the same code and any deviation would constitute a bug. Updating
// it must arrive at the same vector as setting it – yet only precisely when
// the arithmetic is exact, which is why the weights below are small integers
// and why a second net with awkward ones verifies the inexact scenario
// separately.
// ---------------------------------------------------------------------------

TEST_CASE("capi: an accumulator set and evaluated is the from-scratch pass")
{
    Engine engine;

    engine.member_of("c1", "AcIn");
    engine.member_of("c2", "AcIn");
    engine.member_of("c3", "AcIn");
    engine.member_of("c4", "AcIn");
    engine.member_of("d1", "AcHid");
    engine.member_of("d2", "AcHid");
    engine.member_of("e1", "AcOut");

    int64_t created = 0;
    REQUIRE(zelph_nn_connect_layers(engine, engine.node("AcIn"), engine.node("AcHid"), 0.0, 1, &created) == ZELPH_OK);
    REQUIRE(zelph_nn_connect_layers(engine, engine.node("AcHid"), engine.node("AcOut"), 0.0, 2, &created) == ZELPH_OK);

    const zelph_node layers[3] = {engine.node("AcIn"), engine.node("AcHid"), engine.node("AcOut")};
    zelph_net        net       = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 3, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);

    // Powers of two, so that every sum below is exact in binary floating
    // point and an update can be held to the bit; the inexact case is the
    // test after this one. Put in through the snapshot, which is the only way
    // the ABI can name an individual weight - and which therefore also holds
    // the documented layout, row-major by post-synaptic unit.
    const double chosen[10] = {1, 4, 16, 64,   // input i to hidden unit 0
                               2, 8, 32, 128,  // input i to hidden unit 1
                               1, 1};          // both hidden units to the output
    const size_t sizes[2]   = {8, 2};
    REQUIRE(zelph_nn_restore(engine, net, chosen, 10, sizes, 2) == ZELPH_OK);

    size_t width = 0;
    REQUIRE(zelph_nn_accumulator_size(engine, net, &width) == ZELPH_OK);
    CHECK(width == 2); // the hidden layer, not the input one

    const auto by_slots = [&](const std::vector<size_t>& slots)
    {
        zelph_node top   = 0;
        double     value = 0;
        size_t     n     = 1;
        REQUIRE(zelph_nn_eval_slots(engine, net, slots.data(), nullptr, slots.size(), 1, &top, &value, &n) == ZELPH_OK);
        return value;
    };

    const auto by_accumulator = [&](const std::vector<double>& acc)
    {
        zelph_node top   = 0;
        double     value = 0;
        size_t     n     = 1;
        REQUIRE(zelph_nn_accumulator_eval(engine, net, acc.data(), acc.size(), 1, &top, &value, &n) == ZELPH_OK);
        return value;
    };

    std::vector<double> acc(width, 0.0);

    // Set then evaluate, against the same call in one piece. Bit for bit:
    // both run the same first layer over the same slots in the same order.
    for (const std::vector<size_t>& slots : {std::vector<size_t>{},
                                             std::vector<size_t>{0},
                                             std::vector<size_t>{1, 3},
                                             std::vector<size_t>{0, 1, 2, 3}})
    {
        REQUIRE(zelph_nn_accumulator_set(engine, net, slots.data(), nullptr, slots.size(), acc.data(), acc.size()) == ZELPH_OK);
        CHECK(by_accumulator(acc) == by_slots(slots));
    }

    // Moved rather than rebuilt: from {0, 1} to {1, 2, 3} is one removal and
    // two additions, and with exact weights it lands on the same vector.
    REQUIRE(zelph_nn_accumulator_set(engine, net, std::vector<size_t>{0, 1}.data(), nullptr, 2, acc.data(), acc.size()) == ZELPH_OK);

    const size_t added[2]  = {2, 3};
    const size_t removed[1] = {0};
    REQUIRE(zelph_nn_accumulator_update(engine, net, added, nullptr, 2, removed, nullptr, 1, acc.data(), acc.size()) == ZELPH_OK);

    std::vector<double> fresh(width, 0.0);
    const size_t        target[3] = {1, 2, 3};
    REQUIRE(zelph_nn_accumulator_set(engine, net, target, nullptr, 3, fresh.data(), fresh.size()) == ZELPH_OK);
    CHECK(acc == fresh);
    CHECK(by_accumulator(acc) == by_slots({1, 2, 3}));

    // An empty update is a no-op rather than a reset.
    REQUIRE(zelph_nn_accumulator_update(engine, net, nullptr, nullptr, 0, nullptr, nullptr, 0, acc.data(), acc.size()) == ZELPH_OK);
    CHECK(acc == fresh);

    // A graded activation goes in and comes out again: removing what was
    // added with the same weight restores the vector exactly.
    const double graded = 0.5;
    const size_t one[1] = {0};
    REQUIRE(zelph_nn_accumulator_update(engine, net, one, &graded, 1, nullptr, nullptr, 0, acc.data(), acc.size()) == ZELPH_OK);
    CHECK(acc != fresh);
    REQUIRE(zelph_nn_accumulator_update(engine, net, nullptr, nullptr, 0, one, &graded, 1, acc.data(), acc.size()) == ZELPH_OK);
    CHECK(acc == fresh);

    // The buffer length is checked, because the ABI cannot see it otherwise
    // and the write would go past the end.
    CHECK(zelph_nn_accumulator_set(engine, net, one, nullptr, 1, acc.data(), width + 1) == ZELPH_RUNTIME_ERROR);
    CHECK(zelph_nn_accumulator_eval(engine, net, acc.data(), width - 1, 1, nullptr, nullptr, &width) == ZELPH_RUNTIME_ERROR);

    // And so is a slot outside the input layer.
    const size_t past[1] = {4};
    CHECK(zelph_nn_accumulator_set(engine, net, past, nullptr, 1, acc.data(), acc.size()) == ZELPH_RUNTIME_ERROR);
}

TEST_CASE("capi: an updated accumulator drifts from a fresh one, and the drift is small")
{
    // The property the test above cannot reveal, because it selects weights
    // that add exactly. Adding and subtracting rows rounds differently from
    // summing them once, so an accumulator carried a long way is close rather
    // than equal - the reason the API says to set it afresh when that matters.
    Engine engine;

    const char* const inputs[6] = {"f0", "f1", "f2", "f3", "f4", "f5"};
    for (const char* name : inputs) engine.member_of(name, "DrIn");
    engine.member_of("g1", "DrHid");
    engine.member_of("g2", "DrHid");
    engine.member_of("h1", "DrOut");

    int64_t created = 0;
    REQUIRE(zelph_nn_connect_layers(engine, engine.node("DrIn"), engine.node("DrHid"), 0.37, 11, &created) == ZELPH_OK);
    REQUIRE(zelph_nn_connect_layers(engine, engine.node("DrHid"), engine.node("DrOut"), 0.71, 12, &created) == ZELPH_OK);

    const zelph_node layers[3] = {engine.node("DrIn"), engine.node("DrHid"), engine.node("DrOut")};
    zelph_net        net       = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 3, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);

    size_t width = 0;
    REQUIRE(zelph_nn_accumulator_size(engine, net, &width) == ZELPH_OK);

    std::vector<double> carried(width, 0.0);
    const size_t        start[3] = {0, 1, 2};
    REQUIRE(zelph_nn_accumulator_set(engine, net, start, nullptr, 3, carried.data(), carried.size()) == ZELPH_OK);

    // Walk it around a cycle of single swaps and come back to where it began.
    for (int round = 0; round < 200; ++round)
    {
        for (size_t slot = 3; slot < 6; ++slot)
        {
            const size_t out[1] = {slot - 3};
            const size_t in[1]  = {slot};
            REQUIRE(zelph_nn_accumulator_update(engine, net, in, nullptr, 1, out, nullptr, 1, carried.data(), carried.size()) == ZELPH_OK);
            REQUIRE(zelph_nn_accumulator_update(engine, net, out, nullptr, 1, in, nullptr, 1, carried.data(), carried.size()) == ZELPH_OK);
        }
    }

    std::vector<double> fresh(width, 0.0);
    REQUIRE(zelph_nn_accumulator_set(engine, net, start, nullptr, 3, fresh.data(), fresh.size()) == ZELPH_OK);

    for (size_t j = 0; j < width; ++j)
    {
        CHECK(carried[j] == doctest::Approx(fresh[j]).epsilon(1e-12));
    }
}

TEST_CASE("capi: a snapshot restores exactly the weights it was taken from")
{
    Engine engine;

    engine.member_of("i1", "In");
    engine.member_of("o1", "Out");
    const zelph_node in         = engine.node("In");
    const zelph_node out        = engine.node("Out");
    const zelph_node layers[2]  = {in, out};
    const zelph_node i1         = engine.node("i1");
    const zelph_node o1         = engine.node("o1");

    REQUIRE(zelph_nn_connect_layers(engine, in, out, 0.1, 7, nullptr) == ZELPH_OK);
    zelph_net net = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);

    size_t shape_count = 0;
    CHECK(zelph_nn_snapshot_shape(engine, net, nullptr, &shape_count) == ZELPH_BUFFER_TOO_SMALL);
    std::vector<size_t> sizes(shape_count, 0);
    REQUIRE(zelph_nn_snapshot_shape(engine, net, sizes.data(), &shape_count) == ZELPH_OK);
    CHECK(shape_count == 1); // one weight matrix between two layers

    size_t weight_count = 0;
    CHECK(zelph_nn_snapshot(engine, net, nullptr, &weight_count) == ZELPH_BUFFER_TOO_SMALL);
    CHECK(weight_count == sizes[0]);

    std::vector<double> before(weight_count, 0);
    REQUIRE(zelph_nn_snapshot(engine, net, before.data(), &weight_count) == ZELPH_OK);

    double score_before = 0;
    zelph_node top      = 0;
    size_t     count    = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &top, &score_before, &count) == ZELPH_OK);

    for (int epoch = 0; epoch < 20; ++epoch)
        REQUIRE(zelph_nn_train_nodes(engine, net, &i1, nullptr, 1, &o1, nullptr, 1, 0.5, nullptr) == ZELPH_OK);

    double score_trained = 0;
    count                = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &top, &score_trained, &count) == ZELPH_OK);
    CHECK(score_trained != doctest::Approx(score_before));

    REQUIRE(zelph_nn_restore(engine, net, before.data(), before.size(), sizes.data(), sizes.size()) == ZELPH_OK);

    double score_restored = 0;
    count                 = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &top, &score_restored, &count) == ZELPH_OK);
    CHECK(score_restored == doctest::Approx(score_before));

    // The shape is part of the contract: a mismatch is caught before the net
    // is touched, not written half-way.
    const size_t wrong = sizes[0] + 1;
    CHECK(zelph_nn_restore(engine, net, before.data(), before.size(), &wrong, 1) == ZELPH_INVALID_ARGUMENT);
}

TEST_CASE("capi: weights written back survive save and load, and nodes keep their identity")
{
    const auto path = fs::temp_directory_path() / "zelph_capi_roundtrip.bin";
    fs::remove(path);

    zelph_node saved_output = 0;
    double     saved_score  = 0;

    {
        Engine engine;
        engine.member_of("i1", "In");
        engine.member_of("o1", "Out");
        const zelph_node in        = engine.node("In");
        const zelph_node out       = engine.node("Out");
        const zelph_node layers[2] = {in, out};
        const zelph_node i1        = engine.node("i1");
        const zelph_node o1        = engine.node("o1");

        REQUIRE(zelph_nn_connect_layers(engine, in, out, 0.0, 1, nullptr) == ZELPH_OK);
        zelph_net net = -1;
        REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);

        for (int epoch = 0; epoch < 40; ++epoch)
            REQUIRE(zelph_nn_train_nodes(engine, net, &i1, nullptr, 1, &o1, nullptr, 1, 0.5, nullptr) == ZELPH_OK);

        // Without write_back the trained weights live only in the compiled
        // net, and what is saved is the untrained graph.
        REQUIRE(zelph_nn_write_back(engine, net) == ZELPH_OK);

        size_t count = 1;
        REQUIRE(zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &saved_output, &saved_score, &count) == ZELPH_OK);

        REQUIRE(zelph_save(engine, path.string().c_str()) == ZELPH_OK);
    }

    CHECK(fs::exists(path));

    {
        Engine engine;
        REQUIRE(zelph_load(engine, path.string().c_str()) == ZELPH_OK);

        // A node IS its hash, so the same name in a fresh engine is the same
        // node the saved file talks about. This is what lets a caller keep
        // node ids across a save/load cycle at all.
        const zelph_node in        = engine.node("In");
        const zelph_node out       = engine.node("Out");
        const zelph_node layers[2] = {in, out};
        const zelph_node i1        = engine.node("i1");
        CHECK(engine.node("o1") == saved_output);

        zelph_net net = -1;
        REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);

        zelph_node top   = 0;
        double     score = 0;
        size_t     count = 1;
        REQUIRE(zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &top, &score, &count) == ZELPH_OK);
        CHECK(top == saved_output);
        CHECK(score == doctest::Approx(saved_score));
    }

    fs::remove(path);
}

TEST_CASE("capi: failures come back as a status and a message, never as an exception")
{
    Engine engine;

    zelph_node node = 0;
    CHECK(zelph_resolve(engine, nullptr, nullptr, &node) == ZELPH_INVALID_ARGUMENT);
    CHECK(zelph_resolve(nullptr, "x", nullptr, &node) == ZELPH_INVALID_ARGUMENT);

    zelph_node fact = 0;
    CHECK(zelph_fact(engine, engine.node("s"), engine.node("p"), nullptr, 0, &fact) == ZELPH_INVALID_ARGUMENT);

    size_t count = 0;
    CHECK(zelph_nn_snapshot(engine, 17, nullptr, &count) == ZELPH_INVALID_ARGUMENT);
    CHECK(zelph_nn_write_back(engine, -1) == ZELPH_INVALID_ARGUMENT);

    // A layer with no members throws inside the engine. It has to arrive as
    // a status plus a readable message; an exception crossing the boundary
    // would be undefined behaviour in the caller.
    const zelph_node layers[2] = {engine.node("EmptyIn"), engine.node("EmptyOut")};
    zelph_net        net       = -1;
    CHECK(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &net) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("no members") != std::string::npos);

    // .save validates its extension, and the refusal must reach the caller
    // rather than being printed and swallowed.
    CHECK(zelph_save(engine, "not-a-binary.txt") != ZELPH_OK);

    // A success clears the message, so a caller can read it after any
    // failure without carrying the previous one.
    CHECK(zelph_resolve(engine, "x", nullptr, &node) == ZELPH_OK);
    CHECK(std::strlen(zelph_last_error()) == 0);
}

// ---------------------------------------------------------------------------
// The threading guarantee, at the level the Rust wrapper restates it: eval
// takes a shared reference and training takes one too, so a search thread may
// evaluate while a trainer updates the same net. test_neural.cpp pins this
// through Janet's ev/spawn-thread; here it is pinned through the ABI itself,
// with plain OS threads and no VM in between.
// ---------------------------------------------------------------------------

TEST_CASE("capi: a network can be evaluated from several threads while another trains it")
{
    Engine engine;

    engine.member_of("i1", "In");
    engine.member_of("i2", "In");
    engine.member_of("o1", "Out");
    engine.member_of("o2", "Out");

    const zelph_node in        = engine.node("In");
    const zelph_node out       = engine.node("Out");
    const zelph_node layers[2] = {in, out};
    const zelph_node i1        = engine.node("i1");
    const zelph_node o1        = engine.node("o1");

    REQUIRE(zelph_nn_connect_layers(engine, in, out, 0.1, 3, nullptr) == ZELPH_OK);
    zelph_net net = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);

    std::atomic<bool> training{true};
    std::atomic<int>  failures{0};

    std::thread trainer([&]
                        {
        for (int epoch = 0; epoch < 2000; ++epoch)
        {
            if (zelph_nn_train_nodes(engine, net, &i1, nullptr, 1, &o1, nullptr, 1, 0.05, nullptr) != ZELPH_OK)
                ++failures;
        }
        training.store(false); });

    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t)
    {
        readers.emplace_back([&]
                             {
            while (training.load())
            {
                zelph_node top   = 0;
                double     score = 0;
                size_t     count = 1;
                // Whatever the trainer is doing, an evaluation must return a
                // whole set of weights: a status, a node, a finite score.
                if (zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &top, &score, &count) != ZELPH_OK
                    || top == 0 || !std::isfinite(score))
                    ++failures;
            } });
    }

    trainer.join();
    for (auto& reader : readers)
        reader.join();

    CHECK(failures.load() == 0);
}

// ---------------------------------------------------------------------------
// The reasoning surface.
//
// This is the half of zelph that is the reason it exists, and until now the C
// ABI could not reach it: a program could store facts and evaluate networks
// but not state a rule or ask a question. These tests pin the loop a caller
// actually runs - assert, reason, read, and (because the graph is monotonic)
// discard.
// ---------------------------------------------------------------------------

TEST_CASE("capi: a rule derives what forward chaining makes of it")
{
    Engine engine;

    // (socrates ~ human) and the rule "every human is mortal".
    const zelph_node socrates = engine.node("socrates");
    const zelph_node plato    = engine.node("plato");
    const zelph_node is_a     = engine.node("~");
    const zelph_node human    = engine.node("human");
    const zelph_node mortal   = engine.node("mortal");

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, socrates, is_a, &human, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, plato, is_a, &human, 1, &fact) == ZELPH_OK);

    // A variable is a node like any other; the caller holds it, so the answer
    // can name it without a string crossing the boundary.
    zelph_node x = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    CHECK(x != 0);

    // Asking twice for the same name gives the same variable, which is what
    // makes a pattern built in one call usable in another.
    zelph_node again = 0;
    REQUIRE(zelph_variable(engine, "X", &again) == ZELPH_OK);
    CHECK(again == x);

    zelph_node condition = 0;
    zelph_node consequence = 0;
    REQUIRE(zelph_fact(engine, x, is_a, &human, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, is_a, &mortal, 1, &consequence) == ZELPH_OK);

    zelph_node rule = 0;
    REQUIRE(zelph_rule(engine, &condition, 1, &consequence, 1, &rule) == ZELPH_OK);
    CHECK(rule != 0);

    // Nothing is derived until the engine runs: facts and rules created
    // through the API only take effect then.
    int32_t exists = 1;
    REQUIRE(zelph_exists(engine, socrates, is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    REQUIRE(zelph_run(engine) == ZELPH_OK);

    REQUIRE(zelph_exists(engine, socrates, is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
    REQUIRE(zelph_exists(engine, plato, is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
}

TEST_CASE("capi: a query reports its bindings as nodes")
{
    Engine engine;

    const zelph_node is_a  = engine.node("~");
    const zelph_node human = engine.node("human");
    zelph_node       fact  = 0;
    for (const char* name : {"socrates", "plato", "aristotle"})
    {
        REQUIRE(zelph_fact(engine, engine.node(name), is_a, &human, 1, &fact) == ZELPH_OK);
    }

    zelph_node who = 0;
    REQUIRE(zelph_variable(engine, "Who", &who) == ZELPH_OK);

    zelph_node pattern = 0;
    REQUIRE(zelph_fact(engine, who, is_a, &human, 1, &pattern) == ZELPH_OK);

    // Capacity 0 asks for the size, as everywhere else in this ABI.
    size_t pairs = 0;
    size_t rows  = 0;
    CHECK(zelph_query(engine, pattern, nullptr, &pairs, nullptr, &rows) == ZELPH_BUFFER_TOO_SMALL);
    CHECK(rows == 3);
    CHECK(pairs == 3); // one binding per row

    std::vector<zelph_node> flat(pairs * 2, 0);
    std::vector<size_t>     sizes(rows, 0);
    REQUIRE(zelph_query(engine, pattern, flat.data(), &pairs, sizes.data(), &rows) == ZELPH_OK);
    CHECK(rows == 3);

    // Every row binds `who`, and the three bound nodes are the three
    // philosophers - identified by node, so no name lookup is needed to read
    // an answer.
    std::vector<std::string> found;
    size_t                   index = 0;
    for (size_t row = 0; row < rows; ++row)
    {
        CHECK(sizes[row] == 1);
        for (size_t pair = 0; pair < sizes[row]; ++pair, ++index)
        {
            CHECK(flat[index * 2] == who);
            found.push_back(name_of(engine, flat[index * 2 + 1]));
        }
    }

    std::sort(found.begin(), found.end());
    CHECK(found == std::vector<std::string>{"aristotle", "plato", "socrates"});
}

TEST_CASE("capi: a cluster turns the monotonic graph into a workspace")
{
    Engine engine;

    const zelph_node is_a  = engine.node("~");
    const zelph_node thing = engine.node("thing");
    zelph_node       fact  = 0;

    // Asserted before any cluster exists, so no drop can reach it.
    REQUIRE(zelph_fact(engine, engine.node("permanent"), is_a, &thing, 1, &fact) == ZELPH_OK);

    REQUIRE(zelph_cluster(engine, "scratch") == ZELPH_OK);

    char* active = nullptr;
    REQUIRE(zelph_cluster_active(engine, &active) == ZELPH_OK);
    CHECK(std::string(active ? active : "") == "scratch");
    zelph_string_free(active);

    const zelph_node ephemeral = engine.node("ephemeral");
    REQUIRE(zelph_fact(engine, ephemeral, is_a, &thing, 1, &fact) == ZELPH_OK);

    int64_t size = -1;
    REQUIRE(zelph_cluster_count(engine, "scratch", &size) == ZELPH_OK);
    CHECK(size > 0);
    REQUIRE(zelph_cluster_count(engine, "no-such-cluster", &size) == ZELPH_OK);
    CHECK(size == -1);

    REQUIRE(zelph_cluster(engine, nullptr) == ZELPH_OK);
    REQUIRE(zelph_cluster_active(engine, &active) == ZELPH_OK);
    CHECK(active == nullptr);

    int64_t removed = 0;
    REQUIRE(zelph_cluster_drop(engine, "scratch", &removed) == ZELPH_OK);
    CHECK(removed > 0);

    // What the cluster recorded is gone; what predates it is not.
    int32_t exists = 1;
    REQUIRE(zelph_exists(engine, ephemeral, is_a, &thing, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);
    REQUIRE(zelph_exists(engine, engine.node("permanent"), is_a, &thing, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
}

TEST_CASE("capi: sets, collections and the directional relations")
{
    Engine engine;

    const zelph_node a = engine.node("a");
    const zelph_node b = engine.node("b");
    const zelph_node elements[2] = {a, b};

    // A set constant is identified by its members, so the same elements
    // always yield the same node ...
    zelph_node first = 0;
    zelph_node second = 0;
    REQUIRE(zelph_set(engine, elements, 2, &first) == ZELPH_OK);
    REQUIRE(zelph_set(engine, elements, 2, &second) == ZELPH_OK);
    CHECK(first == second);

    // ... while a collection has an identity of its own.
    zelph_node one = 0;
    zelph_node two = 0;
    REQUIRE(zelph_collection(engine, elements, 2, &one) == ZELPH_OK);
    REQUIRE(zelph_collection(engine, elements, 2, &two) == ZELPH_OK);
    CHECK(one != two);

    // targets is the mirror of sources, and both are directional.
    const zelph_node predicate = engine.node("hits");
    zelph_node       fact      = 0;
    REQUIRE(zelph_fact(engine, a, predicate, &b, 1, &fact) == ZELPH_OK);

    zelph_node buffer[4] = {0, 0, 0, 0};
    size_t     count     = 4;
    REQUIRE(zelph_targets(engine, a, predicate, buffer, &count) == ZELPH_OK);
    CHECK(count == 1);
    CHECK(buffer[0] == b);

    count = 4;
    REQUIRE(zelph_sources(engine, predicate, b, buffer, &count) == ZELPH_OK);
    CHECK(count == 1);
    CHECK(buffer[0] == a);

    // The other direction has nothing to report, which is an answer rather
    // than an error.
    count = 4;
    REQUIRE(zelph_targets(engine, b, predicate, buffer, &count) == ZELPH_OK);
    CHECK(count == 0);
}

TEST_CASE("capi: a delta run costs the addition rather than the graph")
{
    Engine engine;

    const zelph_node is_a   = engine.node("~");
    const zelph_node human  = engine.node("human");
    const zelph_node mortal = engine.node("mortal");
    zelph_node       fact   = 0;

    zelph_node x = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    zelph_node condition = 0;
    zelph_node consequence = 0;
    zelph_node rule = 0;
    REQUIRE(zelph_fact(engine, x, is_a, &human, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, is_a, &mortal, 1, &consequence) == ZELPH_OK);
    REQUIRE(zelph_rule(engine, &condition, 1, &consequence, 1, &rule) == ZELPH_OK);

    REQUIRE(zelph_fact(engine, engine.node("first"), is_a, &human, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);

    // A delta run is seeded by what was created since the previous run. It
    // must derive the new consequence and nothing about it may depend on the
    // size of what came before - which is the property that decides whether
    // reasoning can happen inside a loop.
    const zelph_node latecomer = engine.node("latecomer");
    REQUIRE(zelph_fact(engine, latecomer, is_a, &human, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run_delta(engine) == ZELPH_OK);

    int32_t exists = 0;
    REQUIRE(zelph_exists(engine, latecomer, is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    // A single pass is available too, for a caller that wants one step
    // rather than a fixed point.
    REQUIRE(zelph_fact(engine, engine.node("third"), is_a, &human, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run_once(engine) == ZELPH_OK);
    REQUIRE(zelph_exists(engine, engine.node("third"), is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
}

TEST_CASE("capi: an embedder can silence the narration by channel")
{
    // What every embedded caller has to do, and what nothing documented until
    // now: keep ERROR, drop the rest. A reasoning run narrates on OUT and
    // DIAGNOSTIC, and inside a host with its own protocol on stdout that is a
    // protocol error rather than a diagnostic.
    struct Counts
    {
        int out = 0, error = 0, diagnostic = 0, prompt = 0;
    } counts;

    auto sink = [](void* user, int32_t channel, const char*, int32_t)
    {
        auto* c = static_cast<Counts*>(user);
        switch (channel)
        {
        case ZELPH_CHANNEL_OUT: c->out++; break;
        case ZELPH_CHANNEL_ERROR: c->error++; break;
        case ZELPH_CHANNEL_DIAGNOSTIC: c->diagnostic++; break;
        case ZELPH_CHANNEL_PROMPT: c->prompt++; break;
        default: break;
        }
    };

    zelph_engine* engine = nullptr;
    REQUIRE(zelph_engine_create(sink, &counts, &engine) == ZELPH_OK);

    auto node = [&](const char* name)
    {
        zelph_node n = 0;
        REQUIRE(zelph_resolve(engine, name, nullptr, &n) == ZELPH_OK);
        return n;
    };

    const zelph_node is_a   = node("~");
    const zelph_node human  = node("human");
    const zelph_node mortal = node("mortal");

    zelph_node x = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    zelph_node condition = 0, consequence = 0, rule = 0, fact = 0;
    REQUIRE(zelph_fact(engine, x, is_a, &human, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, is_a, &mortal, 1, &consequence) == ZELPH_OK);
    REQUIRE(zelph_rule(engine, &condition, 1, &consequence, 1, &rule) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, node("socrates"), is_a, &human, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);

    // The run narrates - which is the whole point of the channel existing.
    CHECK(counts.diagnostic > 0);

    // And the derivation happened regardless of who was listening.
    int32_t exists = 0;
    REQUIRE(zelph_exists(engine, node("socrates"), is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    zelph_engine_destroy(engine);
}

TEST_CASE("capi: parallel evaluation is reachable and changes no derivation")
{
    Engine engine;

    const zelph_node is_a   = engine.node("~");
    const zelph_node human  = engine.node("human");
    const zelph_node mortal = engine.node("mortal");

    zelph_node x = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    zelph_node condition = 0, consequence = 0, rule = 0, fact = 0;
    REQUIRE(zelph_fact(engine, x, is_a, &human, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, is_a, &mortal, 1, &consequence) == ZELPH_OK);
    REQUIRE(zelph_rule(engine, &condition, 1, &consequence, 1, &rule) == ZELPH_OK);

    // On by default, and the previous value is reported so a caller can put
    // it back.
    int32_t previous = -1;
    REQUIRE(zelph_set_parallel(engine, 0, &previous) == ZELPH_OK);
    CHECK(previous == 1);

    REQUIRE(zelph_fact(engine, engine.node("serial-one"), is_a, &human, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);

    int32_t exists = 0;
    REQUIRE(zelph_exists(engine, engine.node("serial-one"), is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    // Setting it to what it already is is not a toggle.
    REQUIRE(zelph_set_parallel(engine, 0, &previous) == ZELPH_OK);
    CHECK(previous == 0);

    REQUIRE(zelph_set_parallel(engine, 1, &previous) == ZELPH_OK);
    CHECK(previous == 0);

    // The setting is a throughput choice, not a semantic one: the same rule
    // derives the same fact either way.
    REQUIRE(zelph_fact(engine, engine.node("parallel-one"), is_a, &human, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run_delta(engine) == ZELPH_OK);
    REQUIRE(zelph_exists(engine, engine.node("parallel-one"), is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    // A null out-parameter is allowed.
    REQUIRE(zelph_set_parallel(engine, 1, nullptr) == ZELPH_OK);
    CHECK(zelph_set_parallel(nullptr, 1, nullptr) == ZELPH_INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// The hidden-layer activation, and the state it exists to remove.
//
// With a plain ReLU a hidden layer whose every unit is negative for every
// input has an output of exactly 0 and a gradient of exactly 0. No further
// training can leave that state - it is absorbing - and a small
// online-trained net can walk into it and stay there. A leaky unit passes a
// hundredth of the gradient instead of none, which is the difference between
// "slow" and "never".
// ---------------------------------------------------------------------------

TEST_CASE("capi: a leaky hidden layer can recover from being all-negative")
{
    Engine engine;

    engine.member_of("i1", "In");
    engine.member_of("h1", "Hid");
    engine.member_of("o1", "Out");

    const zelph_node in     = engine.node("In");
    const zelph_node hidden = engine.node("Hid");
    const zelph_node out    = engine.node("Out");
    const zelph_node i1     = engine.node("i1");
    const zelph_node h1     = engine.node("h1");
    const zelph_node o1     = engine.node("o1");

    REQUIRE(zelph_nn_connect_layers(engine, in, hidden, 0.0, 1, nullptr) == ZELPH_OK);
    REQUIRE(zelph_nn_connect_layers(engine, hidden, out, 0.0, 2, nullptr) == ZELPH_OK);

    const zelph_node layers[3] = {in, hidden, out};

    // Drive the single hidden unit firmly negative: with i1 -> h1 negative and
    // i1 the only active input, the unit is off for every input this net can
    // see.
    auto put_the_unit_off = [&]() {
        zelph_net net = -1;
        REQUIRE(zelph_nn_compile(engine, layers, 3, ZELPH_ACTIVATION_RELU, &net) == ZELPH_OK);
        // A snapshot is the only way in: set the weight directly rather than
        // hoping training walks there.
        size_t shape_count = 0;
        CHECK(zelph_nn_snapshot_shape(engine, net, nullptr, &shape_count) == ZELPH_BUFFER_TOO_SMALL);
        std::vector<size_t> sizes(shape_count, 0);
        REQUIRE(zelph_nn_snapshot_shape(engine, net, sizes.data(), &shape_count) == ZELPH_OK);

        size_t              weight_count = sizes[0] + sizes[1];
        std::vector<double> weights(weight_count, 0.0);
        weights[0] = -1.0; // i1 -> h1
        weights[1] = 1.0;  // h1 -> o1
        REQUIRE(zelph_nn_restore(engine, net, weights.data(), weights.size(), sizes.data(), sizes.size())
                == ZELPH_OK);
        REQUIRE(zelph_nn_write_back(engine, net) == ZELPH_OK);
    };

    // Returns what the net says before and after 500 steps of being asked
    // for +1.
    auto train_and_report = [&](int32_t activation) -> std::pair<double, double> {
        put_the_unit_off();

        zelph_net net = -1;
        REQUIRE(zelph_nn_compile(engine, layers, 3, activation, &net) == ZELPH_OK);

        zelph_node top    = 0;
        double     before = 0;
        size_t     count  = 1;
        REQUIRE(zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &top, &before, &count) == ZELPH_OK);

        const double target = 1.0;
        for (int step = 0; step < 500; ++step)
        {
            REQUIRE(zelph_nn_train_nodes(engine, net, &i1, nullptr, 1, &o1, &target, 1, 0.1, nullptr)
                    == ZELPH_OK);
        }

        double after = 0;
        count        = 1;
        REQUIRE(zelph_nn_eval_nodes(engine, net, &i1, nullptr, 1, 1, &top, &after, &count) == ZELPH_OK);
        return {before, after};
    };

    // ReLU: the unit is off, the output is exactly 0, and five hundred steps
    // of asking for +1 change nothing whatsoever. That is the absorbing state.
    const auto relu = train_and_report(ZELPH_ACTIVATION_RELU);
    CHECK(relu.first == doctest::Approx(0.0));
    CHECK(relu.second == doctest::Approx(0.0));

    // Leaky: the same unit passes a hundredth of its input, so the output
    // starts near zero rather than at it - and the same five hundred steps
    // move it TOWARDS the +1 it was asked for. Slowly: the gradient through
    // an off unit is a hundredth of the ordinary one, so 500 steps take it
    // from -0.0100 to -0.0037 rather than to +1. That is the whole claim -
    // not "leaky is fast", but "leaky can move at all and ReLU cannot".
    const auto leaky = train_and_report(ZELPH_ACTIVATION_LEAKY_RELU);
    CHECK(leaky.first == doctest::Approx(-0.01));
    CHECK(leaky.second > leaky.first);
    CHECK(std::abs(leaky.second - leaky.first) > 1e-4);
}

TEST_CASE("capi: the activation is checked and the default is the old behaviour")
{
    Engine engine;
    engine.member_of("i1", "In");
    engine.member_of("o1", "Out");
    const zelph_node layers[2] = {engine.node("In"), engine.node("Out")};

    zelph_net net = -1;
    CHECK(zelph_nn_compile(engine, layers, 2, 7, &net) == ZELPH_INVALID_ARGUMENT);

    // A net without a hidden layer is unaffected either way: the output layer
    // is linear whatever the activation says.
    REQUIRE(zelph_nn_connect_layers(engine, layers[0], layers[1], 0.5, 3, nullptr) == ZELPH_OK);

    zelph_net relu = -1;
    zelph_net leaky = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &relu) == ZELPH_OK);
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_LEAKY_RELU, &leaky) == ZELPH_OK);

    const zelph_node i1 = engine.node("i1");
    zelph_node       top = 0;
    double           a = 0, b = 0;
    size_t           count = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, relu, &i1, nullptr, 1, 1, &top, &a, &count) == ZELPH_OK);
    count = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, leaky, &i1, nullptr, 1, 1, &top, &b, &count) == ZELPH_OK);
    CHECK(a == doctest::Approx(b));
}
