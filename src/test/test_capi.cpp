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
    REQUIRE(zelph_nn_compile(engine, layers, 2, &net) == ZELPH_OK);

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
    REQUIRE(zelph_nn_compile(engine, layers, 2, &net) == ZELPH_OK);

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
        REQUIRE(zelph_nn_compile(engine, layers, 2, &net) == ZELPH_OK);

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
        REQUIRE(zelph_nn_compile(engine, layers, 2, &net) == ZELPH_OK);

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
    CHECK(zelph_nn_compile(engine, layers, 2, &net) == ZELPH_RUNTIME_ERROR);
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
    REQUIRE(zelph_nn_compile(engine, layers, 2, &net) == ZELPH_OK);

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
