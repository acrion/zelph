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
