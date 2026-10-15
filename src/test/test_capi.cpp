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

#include "test_helpers.hpp"

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

    const zelph_node socrates   = engine.node("Socrates");
    const zelph_node is         = engine.node("is");
    const zelph_node objects[2] = {engine.node("mortal"), engine.node("greek")};

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

TEST_CASE("capi: a fact asserted through the C API claims a rule's pattern")
{
    // zelph_fact called graph->fact and performed no other action, thus
    // asserting the ground condition of a loaded rule left it the rule's
    // pattern: the fact was absent for zelph_exists, and the rule never
    // fired. The C API builds no pattern of its own (zelph_rule marks
    // nothing), so the rule comes from a file, authored in the way a
    // session would produce it.
    const auto file = fs::temp_directory_path() / "zelph_capi_claim.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("(a p b) => (c q d)");
        interactive.process("(e p f) => !");
        interactive.process(".save \"" + file.string() + "\"");
    }

    SUBCASE("the asserted fact itself")
    {
        Engine engine;
        REQUIRE(zelph_load(engine, file.string().c_str()) == ZELPH_OK);

        const zelph_node b    = engine.node("b");
        zelph_node       fact = 0;
        REQUIRE(zelph_fact(engine, engine.node("a"), engine.node("p"), &b, 1, &fact) == ZELPH_OK);

        int32_t exists = 0;
        REQUIRE(zelph_exists(engine, engine.node("a"), engine.node("p"), &b, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);

        REQUIRE(zelph_run(engine) == ZELPH_OK);
        const zelph_node d = engine.node("d");
        REQUIRE(zelph_exists(engine, engine.node("c"), engine.node("q"), &d, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);
    }

    SUBCASE("a pattern nested in it")
    {
        // A caller may access the pattern without asserting it: it
        // reads the rule. zelph_sources employing `=>` and `!` answers
        // the conditions of the contradiction rule, and the
        // read surface hands out such a node without claiming it.
        Engine engine;
        REQUIRE(zelph_load(engine, file.string().c_str()) == ZELPH_OK);

        zelph_node pattern = 0;
        size_t     count   = 1;
        REQUIRE(zelph_sources(engine, engine.node("=>"), engine.node("!"), &pattern, &count) == ZELPH_OK);
        REQUIRE(count == 1);

        const zelph_node f      = engine.node("f");
        int32_t          exists = -1;
        REQUIRE(zelph_exists(engine, engine.node("e"), engine.node("p"), &f, 1, &exists) == ZELPH_OK);
        REQUIRE(exists == 0);

        zelph_node fact = 0;
        REQUIRE(zelph_fact(engine, engine.node("x"), engine.node("says"), &pattern, 1, &fact) == ZELPH_OK);

        REQUIRE(zelph_exists(engine, engine.node("e"), engine.node("p"), &f, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);
    }

    fs::remove(file);
}

// zelph_exists responds in the same manner as zelph/exists and as a ground
// rule condition does: the engine's bookkeeping is not there for it. The marking of a
// contradiction record and the mark of a rule's pattern, both of which arrive
// with a loaded network, resulted in an answer of 1. zelph_sources enumerates
// these, enabling a caller to access their nodes.
TEST_CASE("capi: zelph_exists finds no bookkeeping, as a ground rule condition does not")
{
    const auto file = fs::temp_directory_path() / "zelph_capi_exists_bookkeeping.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        zelph::test::process_lines(interactive, "x p y\nx q y\n(A p B, A q B) => !\n(a p b) => (c q d)\n");
        interactive.process(".save \"" + file.string() + "\"");
    }

    Engine engine;
    REQUIRE(zelph_load(engine, file.string().c_str()) == ZELPH_OK);
    const zelph_node isa     = engine.node("~");
    zelph_node       refuted = 0;
    zelph_node       mark    = 0;
    REQUIRE(zelph_resolve(engine, "refuted", "zelph", &refuted) == ZELPH_OK);
    REQUIRE(zelph_resolve(engine, "rule pattern", "zelph", &mark) == ZELPH_OK);

    zelph_node record = 0;
    size_t     count  = 1;
    REQUIRE(zelph_sources(engine, isa, refuted, &record, &count) == ZELPH_OK);
    REQUIRE(count == 1);
    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, record, isa, &refuted, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    zelph_node patterns[4] = {};
    count                  = 4;
    REQUIRE(zelph_sources(engine, isa, mark, patterns, &count) == ZELPH_OK);
    REQUIRE(count == 2);
    for (size_t i = 0; i < count; ++i)
    {
        exists = -1;
        REQUIRE(zelph_exists(engine, patterns[i], isa, &mark, 1, &exists) == ZELPH_OK);
        CHECK(exists == 0);
    }

    // The control: a fact that someone
    // claimed.
    const zelph_node y = engine.node("y");
    REQUIRE(zelph_exists(engine, engine.node("x"), engine.node("p"), &y, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    fs::remove(file);
}

// A marking (F ~ refuted) of a ground fact is a refutation, which comes with
// its own call, just as zelph/fact within a Janet block refuses the marking
// and names zelph/refute: written through zelph_fact, the marking took a
// claimed fact from every query without comment. A marking whose fact holds
// a variable is a pattern, and this is the mechanism by which a caller
// requests refutations.
TEST_CASE("capi: zelph_fact refuses a ground refutation marking and names zelph_refute")
{
    Engine           engine;
    const zelph_node a       = engine.node("a");
    const zelph_node p       = engine.node("p");
    const zelph_node b       = engine.node("b");
    const zelph_node isa     = engine.node("~");
    zelph_node       refuted = 0;
    REQUIRE(zelph_resolve(engine, "refuted", "zelph", &refuted) == ZELPH_OK);

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, a, p, &b, 1, &fact) == ZELPH_OK);

    zelph_node marking = 0;
    CHECK(zelph_fact(engine, fact, isa, &refuted, 1, &marking) == ZELPH_RUNTIME_ERROR);
    CHECK(marking == 0);
    CHECK(std::string(zelph_last_error()).find("zelph_refute") != std::string::npos);

    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, a, p, &b, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    zelph_node x = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    zelph_node pattern = 0;
    REQUIRE(zelph_fact(engine, x, p, &b, 1, &pattern) == ZELPH_OK);
    CHECK(zelph_fact(engine, pattern, isa, &refuted, 1, &marking) == ZELPH_OK);
}

// When the marking is rejected, a C caller has no way left to express that a
// fact does not hold. zelph_refute says it as `¬(F)` does on a standalone
// line and zelph/refute does in Janet: refused for a fact the graph claims,
// accepted for one that nobody claimed -- a new one, a rule's ground pattern,
// or one previously refuted -- which then answers nothing and refuses an
// assertion.
TEST_CASE("capi: zelph_refute says that a fact does not hold, and refuses it for a claimed one")
{
    const auto file = fs::temp_directory_path() / "zelph_capi_refute.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("(a p b) => (c q d)");
        interactive.process(".save \"" + file.string() + "\"");
    }

    Engine engine;
    REQUIRE(zelph_load(engine, file.string().c_str()) == ZELPH_OK);
    const zelph_node a = engine.node("a");
    const zelph_node p = engine.node("p");
    const zelph_node e = engine.node("e");
    const zelph_node b = engine.node("b");
    const zelph_node f = engine.node("f");

    zelph_node fact   = 0;
    int32_t    exists = -1;

    SUBCASE("a fact the graph claims")
    {
        REQUIRE(zelph_fact(engine, e, p, &f, 1, &fact) == ZELPH_OK);
        CHECK(zelph_refute(engine, e, p, &f, 1, &fact) == ZELPH_RUNTIME_ERROR);
        CHECK(std::string(zelph_last_error()).find("known to be true") != std::string::npos);
        REQUIRE(zelph_exists(engine, e, p, &f, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);
    }

    SUBCASE("a new fact, refuted twice")
    {
        REQUIRE(zelph_refute(engine, e, p, &f, 1, &fact) == ZELPH_OK);
        CHECK(fact != 0);
        CHECK(zelph_refute(engine, e, p, &f, 1, &fact) == ZELPH_OK);
        REQUIRE(zelph_exists(engine, e, p, &f, 1, &exists) == ZELPH_OK);
        CHECK(exists == 0);
        CHECK(zelph_fact(engine, e, p, &f, 1, &fact) == ZELPH_RUNTIME_ERROR);
        CHECK(std::string(zelph_last_error()).find("known to be wrong") != std::string::npos);
    }

    SUBCASE("a rule's ground pattern")
    {
        REQUIRE(zelph_refute(engine, a, p, &b, 1, &fact) == ZELPH_OK);
        CHECK(zelph_fact(engine, a, p, &b, 1, &fact) == ZELPH_RUNTIME_ERROR);
        REQUIRE(zelph_run(engine) == ZELPH_OK);
        const zelph_node d = engine.node("d");
        REQUIRE(zelph_exists(engine, engine.node("c"), engine.node("q"), &d, 1, &exists) == ZELPH_OK);
        CHECK(exists == 0);
    }

    fs::remove(file);
}

// zelph_refute of a marking wrote the marking at probability 0, and the marking
// still refuted its fact: a claimed `a p b` left every query, via the call that
// says it is not refuted. Refused, as `¬((a p b) ~ refuted)` is
// (test_contradiction_records.cpp).
TEST_CASE("capi: zelph_refute refuses a refutation marking and leaves its fact claimed")
{
    Engine           engine;
    const zelph_node a   = engine.node("a");
    const zelph_node p   = engine.node("p");
    const zelph_node b   = engine.node("b");
    const zelph_node isa = engine.node("~");

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, a, p, &b, 1, &fact) == ZELPH_OK);
    zelph_node refuted = 0;
    REQUIRE(zelph_resolve(engine, "refuted", "zelph", &refuted) == ZELPH_OK);

    zelph_node marking = 0;
    CHECK(zelph_refute(engine, fact, isa, &refuted, 1, &marking) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("cannot itself be refuted") != std::string::npos);

    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, a, p, &b, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
    CHECK(zelph_fact(engine, a, p, &b, 1, &fact) == ZELPH_OK);
}

// During an embedder's dry run, a refutation that zelph_refute makes inside a
// cluster is taken back by zelph_cluster_drop, removing both the index entry
// and the 0 from the fact's edge, enabling the rule's ground pattern to be
// asserted subsequently and the rule to fire.
TEST_CASE("capi: a refutation zelph_refute makes inside a cluster is taken back by its drop")
{
    const auto file = fs::temp_directory_path() / "zelph_capi_refute_cluster.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("(a p b) => (c q d)");
        interactive.process(".save \"" + file.string() + "\"");
    }

    Engine engine;
    REQUIRE(zelph_load(engine, file.string().c_str()) == ZELPH_OK);
    fs::remove(file);
    const zelph_node a = engine.node("a");
    const zelph_node p = engine.node("p");
    const zelph_node b = engine.node("b");

    REQUIRE(zelph_cluster(engine, "dry") == ZELPH_OK);
    zelph_node fact = 0;
    REQUIRE(zelph_refute(engine, a, p, &b, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, a, p, &b, 1, &fact) == ZELPH_RUNTIME_ERROR);
    REQUIRE(zelph_cluster(engine, nullptr) == ZELPH_OK);
    REQUIRE(zelph_cluster_drop(engine, "dry", nullptr) == ZELPH_OK);

    CHECK(zelph_fact(engine, a, p, &b, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);
    const zelph_node d      = engine.node("d");
    int32_t          exists = -1;
    REQUIRE(zelph_exists(engine, engine.node("c"), engine.node("q"), &d, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
}

// A statement holding a rule's marking pattern claims it, and the claim
// refuted the fact that the marking marks. Requested via zelph_sources, the
// marking functions as a node just like any other, hence the refusal
// originates within the claim itself: the fact that the graph claims keeps
// answering.
TEST_CASE("capi: a fact holding a rule's marking pattern of a claimed fact is refused")
{
    const auto file = fs::temp_directory_path() / "zelph_capi_marking_pattern.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("a p b");
        interactive.process("((a p b) ~ refuted) => !");
        interactive.process(".save \"" + file.string() + "\"");
    }

    Engine engine;
    REQUIRE(zelph_load(engine, file.string().c_str()) == ZELPH_OK);
    fs::remove(file);

    zelph_node marking = 0;
    size_t     count   = 1;
    REQUIRE(zelph_sources(engine, engine.node("=>"), engine.node("!"), &marking, &count) == ZELPH_OK);
    REQUIRE(count == 1);

    zelph_node fact = 0;
    CHECK(zelph_fact(engine, engine.node("x"), engine.node("says"), &marking, 1, &fact) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("known to be true") != std::string::npos);

    const zelph_node b      = engine.node("b");
    int32_t          exists = -1;
    REQUIRE(zelph_exists(engine, engine.node("a"), engine.node("p"), &b, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    // And the refused statement is not written: it used to be, before the
    // claim reached the marking, and it stayed.
    REQUIRE(zelph_exists(engine, engine.node("x"), engine.node("says"), &marking, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);
}

// A statement containing a refuted fact claims it, and claiming a fact known
// to be wrong is refused, as asserting it is. Handed in by node -- the fact
// that zelph_refute returns -- it was accepted: the claim took the mention's
// mark, and the fact was both claimed and refuted simultaneously.
TEST_CASE("capi: a fact holding a refuted fact by node is refused")
{
    Engine engine;

    const zelph_node nil     = engine.node("nil");
    zelph_node       refuted = 0;
    REQUIRE(zelph_refute(engine, engine.node("mary"), engine.node("waits"), &nil, 1, &refuted) == ZELPH_OK);

    zelph_node fact = 0;
    CHECK(zelph_fact(engine, engine.node("x"), engine.node("says"), &refuted, 1, &fact) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("known to be wrong") != std::string::npos);

    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, engine.node("x"), engine.node("says"), &refuted, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);
    REQUIRE(zelph_exists(engine, engine.node("mary"), engine.node("waits"), &nil, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);
}

// The C ABI offered no way to write a statement without asserting it: a
// program translating "John believes that Mary waits." could only claim
// `mary waits nil` via zelph_fact. zelph_mention writes the mention; a
// later zelph_fact of the same statement constitutes the claim that turns
// it into data, and a mention of what is asserted leaves it asserted.
TEST_CASE("capi: zelph_mention writes a statement without claiming it")
{
    Engine engine;

    const zelph_node nil       = engine.node("nil");
    zelph_node       mentioned = 0;
    REQUIRE(zelph_mention(engine, engine.node("mary"), engine.node("waits"), &nil, 1, &mentioned) == ZELPH_OK);
    CHECK(mentioned != 0);

    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, engine.node("mary"), engine.node("waits"), &nil, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    zelph_node claimed = 0;
    REQUIRE(zelph_fact(engine, engine.node("mary"), engine.node("waits"), &nil, 1, &claimed) == ZELPH_OK);
    CHECK(claimed == mentioned);
    REQUIRE(zelph_exists(engine, engine.node("mary"), engine.node("waits"), &nil, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    zelph_node again = 0;
    REQUIRE(zelph_mention(engine, engine.node("mary"), engine.node("waits"), &nil, 1, &again) == ZELPH_OK);
    CHECK(again == mentioned);
    REQUIRE(zelph_exists(engine, engine.node("mary"), engine.node("waits"), &nil, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    SUBCASE("the arguments are checked as zelph_fact checks them")
    {
        zelph_node out = 0;
        CHECK(zelph_mention(engine, engine.node("mary"), engine.node("waits"), &nil, 0, &out) == ZELPH_INVALID_ARGUMENT);
        CHECK(zelph_mention(engine, 0, engine.node("waits"), &nil, 1, &out) == ZELPH_INVALID_ARGUMENT);
    }
}

// A rule authored by zelph_mention is a mention like any other statement it
// produces, but since nothing held the `=>` the call returned, the rule was
// in force and fired during the next run. zelph_fact of that same rule
// constitutes the claim that puts it in force, just as the claim of any
// other mention does.
TEST_CASE("capi: a rule written with zelph_mention is not in force until zelph_fact claims it")
{
    Engine           engine;
    const zelph_node a      = engine.node("a");
    const zelph_node p      = engine.node("p");
    const zelph_node b      = engine.node("b");
    const zelph_node c      = engine.node("c");
    const zelph_node q      = engine.node("q");
    const zelph_node d      = engine.node("d");
    const zelph_node causes = engine.node("=>");

    zelph_node condition = 0;
    REQUIRE(zelph_mention(engine, a, p, &b, 1, &condition) == ZELPH_OK);
    zelph_node consequence = 0;
    REQUIRE(zelph_mention(engine, c, q, &d, 1, &consequence) == ZELPH_OK);
    zelph_node mentioned = 0;
    REQUIRE(zelph_mention(engine, condition, causes, &consequence, 1, &mentioned) == ZELPH_OK);

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, a, p, &b, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);
    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, c, q, &d, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    zelph_node claimed = 0;
    REQUIRE(zelph_fact(engine, condition, causes, &consequence, 1, &claimed) == ZELPH_OK);
    CHECK(claimed == mentioned);
    REQUIRE(zelph_run(engine) == ZELPH_OK);
    REQUIRE(zelph_exists(engine, c, q, &d, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
}

// zelph_fact refuses the ground marking `(F ~ refuted)`, a refutation, while
// zelph_rule takes nodes that were built before it, so a rule over the
// refutation of F gets its condition from zelph_mention, which writes the
// marking down without claiming it, just as zelph/mention does in Janet. The
// rule is in force and fires once zelph_refute claims the refutation, similar
// to how the typed "((john waits nil) ~ refuted) => (mary sleeps nil)" does.
TEST_CASE("capi: a rule's ground refutation condition is written with zelph_mention")
{
    Engine           engine;
    const zelph_node john    = engine.node("john");
    const zelph_node waits   = engine.node("waits");
    const zelph_node mary    = engine.node("mary");
    const zelph_node sleeps  = engine.node("sleeps");
    const zelph_node nil     = engine.node("nil");
    const zelph_node isa     = engine.node("~");
    zelph_node       refuted = 0;
    REQUIRE(zelph_resolve(engine, "refuted", "zelph", &refuted) == ZELPH_OK);

    zelph_node waiting = 0;
    REQUIRE(zelph_mention(engine, john, waits, &nil, 1, &waiting) == ZELPH_OK);
    zelph_node marking = 0;
    REQUIRE(zelph_mention(engine, waiting, isa, &refuted, 1, &marking) == ZELPH_OK);
    zelph_node sleeping = 0;
    REQUIRE(zelph_mention(engine, mary, sleeps, &nil, 1, &sleeping) == ZELPH_OK);
    zelph_node condition_set = 0;
    REQUIRE(zelph_rule(engine, &marking, 1, &sleeping, 1, &condition_set) == ZELPH_OK);

    REQUIRE(zelph_run(engine) == ZELPH_OK);
    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, mary, sleeps, &nil, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);
    REQUIRE(zelph_exists(engine, waiting, isa, &refuted, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    zelph_node fact = 0;
    REQUIRE(zelph_refute(engine, john, waits, &nil, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);
    REQUIRE(zelph_exists(engine, mary, sleeps, &nil, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
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
    const double one            = 1.0;
    double       explicit_score = 0;
    count                       = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, net, &i2, &one, 1, 1, &top, &explicit_score, &count) == ZELPH_OK);
    CHECK(explicit_score == doctest::Approx(score));

    // Graded activation scales the (linear) response.
    const double half       = 0.5;
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
    // clang-format off
    // Four weights per row, and each comment is assigned to the row above it.
    // One entry per line is how the formatter interprets this, and it puts
    // every comment next to a number it does not describe.
    const double chosen[10] = {1, 4, 16, 64,   // input i to hidden unit 0
                               2, 8, 32, 128,  // input i to hidden unit 1
                               1, 1};          // both hidden units to the output
    // clang-format on
    const size_t sizes[2] = {8, 2};
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

    const size_t added[2]   = {2, 3};
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
    for (const char* name : inputs)
        engine.member_of(name, "DrIn");
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
    const zelph_node in        = engine.node("In");
    const zelph_node out       = engine.node("Out");
    const zelph_node layers[2] = {in, out};
    const zelph_node i1        = engine.node("i1");
    const zelph_node o1        = engine.node("o1");

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

    double     score_before = 0;
    zelph_node top          = 0;
    size_t     count        = 1;
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

// zelph_variable keeps track of a variable through its name across multiple
// invocations. The file holds no variable, so following the load, the engine's
// counter reassigns the first variable ID, which X still retains: Y reappears
// as the identical node as X. When a load still merged the file into the
// graph, Y failed completely, since X's node persisted after the load with
// that same ID.
TEST_CASE("capi: a load that replaces the graph forgets the variables remembered by name")
{
    const auto path = fs::temp_directory_path() / "zelph_capi_load_variables.bin";
    fs::remove(path);

    Engine           engine;
    const zelph_node a    = engine.node("a");
    const zelph_node b    = engine.node("b");
    zelph_node       fact = 0;
    REQUIRE(zelph_fact(engine, a, engine.node("p"), &b, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_save(engine, path.string().c_str()) == ZELPH_OK);

    zelph_node before = 0;
    REQUIRE(zelph_variable(engine, "X", &before) == ZELPH_OK);
    REQUIRE(zelph_load(engine, path.string().c_str()) == ZELPH_OK);

    zelph_node x = 0;
    zelph_node y = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "Y", &y) == ZELPH_OK);
    CHECK(x != y);

    fs::remove(path);
}

// The neurons in a compiled net are node ids, which a loaded graph gives to
// other nodes or leaves unassigned: an evaluation answered with whatever
// carried them, and a write-back operation set weights between them. A
// handle taken before the load is refused, and no later compile can reclaim
// its number.
TEST_CASE("capi: a load that replaces the graph refuses the net handles taken before it")
{
    const auto path = fs::temp_directory_path() / "zelph_capi_load_nets.bin";
    fs::remove(path);

    Engine engine;
    engine.member_of("i1", "In");
    engine.member_of("o1", "Out");
    const zelph_node layers[2] = {engine.node("In"), engine.node("Out")};
    REQUIRE(zelph_nn_connect_layers(engine, layers[0], layers[1], 0.0, 1, nullptr) == ZELPH_OK);
    zelph_net before = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &before) == ZELPH_OK);
    REQUIRE(zelph_save(engine, path.string().c_str()) == ZELPH_OK);
    REQUIRE(zelph_load(engine, path.string().c_str()) == ZELPH_OK);

    const zelph_node i1    = engine.node("i1");
    zelph_node       top   = 0;
    double           score = 0;
    size_t           count = 1;
    CHECK(zelph_nn_eval_nodes(engine, before, &i1, nullptr, 1, 1, &top, &score, &count) == ZELPH_INVALID_ARGUMENT);
    CHECK(std::string(zelph_last_error()).find("invalid network handle") != std::string::npos);
    CHECK(zelph_nn_write_back(engine, before) == ZELPH_INVALID_ARGUMENT);

    const zelph_node again_layers[2] = {engine.node("In"), engine.node("Out")};
    zelph_net        again           = -1;
    REQUIRE(zelph_nn_compile(engine, again_layers, 2, ZELPH_ACTIVATION_RELU, &again) == ZELPH_OK);
    CHECK(again != before);
    count = 1;
    CHECK(zelph_nn_eval_nodes(engine, again, &i1, nullptr, 1, 1, &top, &score, &count) == ZELPH_OK);
    CHECK(top == engine.node("o1"));

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
    CHECK(zelph_refute(engine, engine.node("s"), engine.node("p"), nullptr, 0, &fact) == ZELPH_INVALID_ARGUMENT);
    const zelph_node o = engine.node("o");
    CHECK(zelph_refute(engine, engine.node("s"), engine.node("p"), &o, 0, &fact) == ZELPH_INVALID_ARGUMENT);
    CHECK(zelph_refute(engine, 0, engine.node("p"), &o, 1, &fact) == ZELPH_INVALID_ARGUMENT);

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
// The other half of the threading rule: the graph is tied to the thread that
// created the engine. The script engine beneath it keeps the state that a
// statement, a rule build, and the scratch clusters rely on, once per
// process, so a second thread that wrote facts while the primary thread was
// active had its thousands of facts lost to the primary thread's scratch
// drops, and every one of its calls returned ZELPH_OK. Writing the graph,
// building a rule, running the engine, and switching clusters are now
// disallowed on a different thread; reading the graph and utilizing a
// compiled net are not.
// ---------------------------------------------------------------------------

namespace
{
    // The outcome of a call made on a dedicated thread, with the message
    // zelph_last_error() held on that thread: the message is specific to the
    // thread, thus it must be read on the thread that encountered the
    // failure.
    struct OffThread
    {
        int32_t     status = ZELPH_OK;
        std::string message;
    };

    template <typename F>
    OffThread off_thread(F&& call)
    {
        OffThread   result;
        std::thread caller([&]
                           {
            result.status  = call();
            result.message = zelph_last_error(); });
        caller.join();
        return result;
    }

    int64_t cluster_size(zelph_engine* engine, const char* name)
    {
        int64_t size = -2;
        REQUIRE(zelph_cluster_count(engine, name, &size) == ZELPH_OK);
        return size;
    }
}

TEST_CASE("capi: a fact written from another thread is refused, and nothing is written")
{
    Engine engine;

    const zelph_node a = engine.node("a");
    const zelph_node p = engine.node("p");
    const zelph_node b = engine.node("b");

    // Each node generated from this point forward is recorded, meaning the
    // cluster's size indicates if the refused call wrote anything.
    REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);

    zelph_node      fact    = 0;
    const OffThread refused = off_thread([&]
                                         { return zelph_fact(engine, a, p, &b, 1, &fact); });

    CHECK(refused.status == ZELPH_RUNTIME_ERROR);
    CHECK(refused.message.find("zelph_fact: main thread only") != std::string::npos);
    CHECK(fact == 0);
    CHECK(cluster_size(engine, "probe") == 0);

    int32_t exists = 1;
    REQUIRE(zelph_exists(engine, a, p, &b, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);
}

// zelph_mention writes the graph in the same manner as zelph_fact, thus
// adhering to the thread rule: a mention originating from a different
// thread is refused and writes nothing.
TEST_CASE("capi: a mention written from another thread is refused, and nothing is written")
{
    Engine engine;

    const zelph_node a = engine.node("a");
    const zelph_node p = engine.node("p");
    const zelph_node b = engine.node("b");
    REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);

    zelph_node      fact    = 0;
    const OffThread refused = off_thread([&]
                                         { return zelph_mention(engine, a, p, &b, 1, &fact); });

    CHECK(refused.status == ZELPH_RUNTIME_ERROR);
    CHECK(refused.message.find("zelph_mention: main thread only") != std::string::npos);
    CHECK(fact == 0);
    CHECK(cluster_size(engine, "probe") == 0);
}

TEST_CASE("capi: a rule built from another thread is refused, and nothing is written")
{
    Engine engine;

    const zelph_node p = engine.node("p");
    const zelph_node q = engine.node("q");
    zelph_node       x = 0;
    zelph_node       y = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "Y", &y) == ZELPH_OK);

    zelph_node condition   = 0;
    zelph_node consequence = 0;
    REQUIRE(zelph_fact(engine, x, p, &y, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, q, &y, 1, &consequence) == ZELPH_OK);

    REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);

    zelph_node      rule    = 0;
    const OffThread refused = off_thread([&]
                                         { return zelph_rule(engine, &condition, 1, &consequence, 1, &rule); });

    CHECK(refused.status == ZELPH_RUNTIME_ERROR);
    CHECK(refused.message.find("zelph_rule: main thread only") != std::string::npos);
    CHECK(rule == 0);
    CHECK(cluster_size(engine, "probe") == 0);
}

TEST_CASE("capi: a run from another thread is refused, and derives nothing")
{
    Engine engine;

    const zelph_node socrates = engine.node("socrates");
    const zelph_node is_a     = engine.node("~");
    const zelph_node human    = engine.node("human");
    const zelph_node mortal   = engine.node("mortal");

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, socrates, is_a, &human, 1, &fact) == ZELPH_OK);

    zelph_node x = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    zelph_node condition   = 0;
    zelph_node consequence = 0;
    REQUIRE(zelph_fact(engine, x, is_a, &human, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, is_a, &mortal, 1, &consequence) == ZELPH_OK);
    zelph_node rule = 0;
    REQUIRE(zelph_rule(engine, &condition, 1, &consequence, 1, &rule) == ZELPH_OK);

    const OffThread refused = off_thread([&]
                                         { return zelph_run(engine); });
    CHECK(refused.status == ZELPH_RUNTIME_ERROR);
    CHECK(refused.message.find("zelph_run: main thread only") != std::string::npos);

    int32_t exists = 1;
    REQUIRE(zelph_exists(engine, socrates, is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    // The identical run on the engine's own thread derives it, thus
    // the refusal is what maintained the fact's exclusion.
    REQUIRE(zelph_run(engine) == ZELPH_OK);
    REQUIRE(zelph_exists(engine, socrates, is_a, &mortal, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
}

TEST_CASE("capi: a cluster switch from another thread is refused, and the active cluster stays")
{
    Engine engine;
    REQUIRE(zelph_cluster(engine, "mine") == ZELPH_OK);

    const OffThread refused = off_thread([&]
                                         { return zelph_cluster(engine, "theirs"); });
    CHECK(refused.status == ZELPH_RUNTIME_ERROR);
    CHECK(refused.message.find("zelph_cluster: main thread only") != std::string::npos);

    char* active = nullptr;
    REQUIRE(zelph_cluster_active(engine, &active) == ZELPH_OK);
    CHECK(std::string(active ? active : "") == "mine");
    zelph_string_free(active);
    CHECK(cluster_size(engine, "theirs") == -1);
}

TEST_CASE("capi: on another thread a name resolves only when it already has a node")
{
    // A thread evaluating a compiled net resolves the inputs' names; a name
    // linked to a node provides a response identical to that on the
    // engine's thread. A name requiring a new node constitutes a write
    // operation and is refused.
    Engine           engine;
    const zelph_node known = engine.node("known");

    REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);

    zelph_node      found  = 0;
    const OffThread lookup = off_thread([&]
                                        { return zelph_resolve(engine, "known", nullptr, &found); });
    CHECK(lookup.status == ZELPH_OK);
    CHECK(found == known);

    // The refusal names the name: the call performs a lookup that found
    // nothing, so a caller looking names up can tell an absent name from a call
    // that has no business on this thread at all, which reads "zelph_resolve:
    // main thread only" and is what it stated previously.
    zelph_node      created = 0;
    const OffThread refused = off_thread([&]
                                         { return zelph_resolve(engine, "unheard-of", nullptr, &created); });
    CHECK(refused.status == ZELPH_RUNTIME_ERROR);
    CHECK(refused.message == "zelph_resolve: 'unheard-of' has no node, and creating one is main thread only.");
    CHECK(created == 0);
    CHECK(cluster_size(engine, "probe") == 0);

    // The name of a node, requested from a different thread, is not
    // denied: reading is permitted.
    std::string     name;
    const OffThread read = off_thread([&]
                                      {
        char* text = nullptr;
        const int32_t status = zelph_name(engine, known, nullptr, &text);
        name = text ? text : "";
        zelph_string_free(text);
        return status; });
    CHECK(read.status == ZELPH_OK);
    CHECK(name == "known");
}

TEST_CASE("capi: on another thread the empty name gets the error the engine's thread gives")
{
    // No node may hold the empty name, thus the call constitutes an error on
    // every thread, and it is the identical error: refusing it as a write
    // that only the engine's thread can perform would unfairly attribute
    // blame to the thread for a name that is invalid everywhere.
    Engine engine;

    zelph_node        own_node    = 0;
    const int32_t     own_status  = zelph_resolve(engine, "", nullptr, &own_node);
    const std::string own_message = zelph_last_error();
    REQUIRE(own_status != ZELPH_OK);

    zelph_node      node    = 0;
    const OffThread refused = off_thread([&]
                                         { return zelph_resolve(engine, "", nullptr, &node); });
    CHECK(refused.status == own_status);
    CHECK(refused.message == own_message);
    CHECK(node == 0);
}

TEST_CASE("capi: trained weights are written back from another thread")
{
    // `zelph_nn_write_back` replaces solely the weights associated with the
    // synapses from which the net was built, applying each under the
    // protection of the weight store's lock, and creates nothing, making
    // it permissible across any thread. A net compiled afterwards reads the
    // weights directly from the graph, which is the mechanism through which
    // the test detects their presence.
    Engine engine;

    engine.member_of("i1", "In");
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
    for (int epoch = 0; epoch < 50; ++epoch)
        REQUIRE(zelph_nn_train_nodes(engine, net, &i1, nullptr, 1, &o1, nullptr, 1, 0.1, nullptr) == ZELPH_OK);

    const auto weights_of = [&](const zelph_net handle)
    {
        size_t count = 0;
        CHECK(zelph_nn_snapshot(engine, handle, nullptr, &count) == ZELPH_BUFFER_TOO_SMALL);
        std::vector<double> weights(count, 0.0);
        REQUIRE(zelph_nn_snapshot(engine, handle, weights.data(), &count) == ZELPH_OK);
        return weights;
    };

    const OffThread written = off_thread([&]
                                         { return zelph_nn_write_back(engine, net); });
    CHECK(written.status == ZELPH_OK);

    zelph_net again = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &again) == ZELPH_OK);
    CHECK(weights_of(again) == weights_of(net));
}

TEST_CASE("capi: every call that writes the graph or changes the engine is refused on another thread")
{
    // A single thread calls each refused entry point exactly once, and the
    // list names every call that was NOT refused as main thread only,
    // specifying what was returned in its place. The arguments are those that
    // the engine's own thread accepts: actual nodes, existing
    // clusters, and a file that is loadable. A preliminary argument check
    // occurs before the thread check, meaning that a call containing a null
    // node or a missing file would be rejected on that basis and would never
    // proceed to the thread check.
    const auto loadable = fs::temp_directory_path() / "zelph_capi_threads_loadable.bin";
    const auto unsaved  = fs::temp_directory_path() / "zelph_capi_threads_unsaved.bin";
    fs::remove(unsaved);

    Engine engine;

    const zelph_node a = engine.node("a");
    const zelph_node p = engine.node("p");
    const zelph_node q = engine.node("q");
    const zelph_node b = engine.node("b");
    zelph_node       x = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    zelph_node condition   = 0;
    zelph_node consequence = 0;
    REQUIRE(zelph_fact(engine, x, p, &b, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, q, &b, 1, &consequence) == ZELPH_OK);

    engine.member_of("i1", "In");
    engine.member_of("o1", "Out");
    const zelph_node in        = engine.node("In");
    const zelph_node out       = engine.node("Out");
    const zelph_node layers[2] = {in, out};
    REQUIRE(zelph_nn_connect_layers(engine, in, out, 0.1, 3, nullptr) == ZELPH_OK);

    REQUIRE(zelph_save(engine, loadable.string().c_str()) == ZELPH_OK);
    REQUIRE(zelph_cluster(engine, "other") == ZELPH_OK);
    REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);

    std::vector<std::string> accepted;
    std::thread              caller([&]
                                    {
        const auto expect = [&](const char* function, const int32_t status, const std::string& refusal)
        {
            const std::string message = zelph_last_error();
            if (status != ZELPH_RUNTIME_ERROR || message.rfind(refusal, 0) != 0)
                accepted.push_back(std::string(function) + " -> " + std::to_string(status) + " " + message);
        };
        const auto refused = [&](const char* function, const int32_t status)
        { expect(function, status, std::string(function) + ": main thread only"); };

        const zelph_node elements[2] = {a, b};
        zelph_node       node        = 0;
        refused("zelph_fact", zelph_fact(engine, a, p, &b, 1, &node));
        refused("zelph_refute", zelph_refute(engine, a, q, &b, 1, &node));
        refused("zelph_list", zelph_list(engine, elements, 2, &node));
        refused("zelph_set", zelph_set(engine, elements, 2, &node));
        refused("zelph_collection", zelph_collection(engine, elements, 2, &node));
        refused("zelph_negate", zelph_negate(engine, condition, &node));
        refused("zelph_variable", zelph_variable(engine, "Y", &node));
        refused("zelph_clear_variables", zelph_clear_variables(engine));
        refused("zelph_rule", zelph_rule(engine, &condition, 1, &consequence, 1, &node));
        refused("zelph_run", zelph_run(engine));
        refused("zelph_run_once", zelph_run_once(engine));
        refused("zelph_run_delta", zelph_run_delta(engine));
        int32_t previous = -1;
        refused("zelph_set_parallel", zelph_set_parallel(engine, 0, &previous));
        size_t pairs = 0;
        size_t rows  = 0;
        refused("zelph_query", zelph_query(engine, condition, nullptr, &pairs, nullptr, &rows));
        refused("zelph_cluster", zelph_cluster(engine, "other"));
        char* active = nullptr;
        refused("zelph_cluster_active", zelph_cluster_active(engine, &active));
        zelph_string_free(active);
        int64_t removed = -1;
        refused("zelph_cluster_drop", zelph_cluster_drop(engine, "other", &removed));
        int64_t count = -1;
        refused("zelph_cluster_count", zelph_cluster_count(engine, "probe", &count));
        refused("zelph_load", zelph_load(engine, loadable.string().c_str()));
        refused("zelph_save", zelph_save(engine, unsaved.string().c_str()));
        zelph_net net = -1;
        refused("zelph_nn_compile", zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &net));
        refused("zelph_nn_connect_layers", zelph_nn_connect_layers(engine, in, out, 0.1, 3, nullptr));
        expect("zelph_resolve",
               zelph_resolve(engine, "never-named", nullptr, &node),
               "zelph_resolve: 'never-named' has no node, and creating one is main thread only"); });
    caller.join();

    CHECK(accepted == std::vector<std::string>{});

    // On the engine's own thread, observe what the calls would have
    // altered.
    CHECK(cluster_size(engine, "probe") == 0);
    CHECK(cluster_size(engine, "other") == 0);
    char* active = nullptr;
    REQUIRE(zelph_cluster_active(engine, &active) == ZELPH_OK);
    CHECK(std::string(active ? active : "") == "probe");
    zelph_string_free(active);
    zelph_node x_again = 0;
    REQUIRE(zelph_variable(engine, "X", &x_again) == ZELPH_OK);
    CHECK(x_again == x);
    CHECK_FALSE(fs::exists(unsaved));

    fs::remove(loadable);
}

// ---------------------------------------------------------------------------
// The reasoning surface.
//
// This is the half of zelph that is the reason it exists, and up until now
// the C ABI has been unable to access it: a program could store facts and
// evaluate networks yet could not state a rule or ask a question. These
// tests pin the loop a caller actually executes -- assert, reason, read, and
// (since the graph otherwise only grows) remove.
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

    zelph_node condition   = 0;
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

TEST_CASE("capi: a rule with several consequences is one rule, so they share a fresh variable")
{
    // zelph_rule associated every consequence with the condition set by a
    // causation fact unique to it, forming a rule for each: they fired
    // independently, and a fresh variable named by two consequences received
    // a witness in each instance, even though the header claims a single
    // rule that deduces every consequence.
    Engine engine;

    const zelph_node p = engine.node("p");
    const zelph_node q = engine.node("q");
    const zelph_node r = engine.node("r");

    zelph_node x = 0, y = 0, n = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "Y", &y) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "N", &n) == ZELPH_OK);

    zelph_node condition = 0;
    zelph_node heads[2]  = {0, 0};
    REQUIRE(zelph_fact(engine, x, p, &y, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, q, &n, 1, &heads[0]) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, n, r, &y, 1, &heads[1]) == ZELPH_OK);

    SUBCASE("the consequences meet in one witness")
    {
        zelph_node rule = 0;
        REQUIRE(zelph_rule(engine, &condition, 1, heads, 2, &rule) == ZELPH_OK);

        const zelph_node b    = engine.node("b");
        zelph_node       fact = 0;
        REQUIRE(zelph_fact(engine, engine.node("a"), p, &b, 1, &fact) == ZELPH_OK);
        REQUIRE(zelph_run(engine) == ZELPH_OK);

        zelph_node witness = 0;
        size_t     count   = 1;
        REQUIRE(zelph_targets(engine, engine.node("a"), q, &witness, &count) == ZELPH_OK);
        REQUIRE(count == 1);

        int32_t exists = 0;
        REQUIRE(zelph_exists(engine, witness, r, &b, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);
    }

    SUBCASE("a consequence that is no node leaves no rule behind")
    {
        // The consequences are read before any writing takes place,
        // meaning the refusal of the second one finds no condition set
        // and no first causation fact to leave behind.
        REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);
        const zelph_node bad[2] = {heads[0], 0};
        zelph_node       rule   = 0;
        CHECK(zelph_rule(engine, &condition, 1, bad, 2, &rule) == ZELPH_INVALID_ARGUMENT);

        int64_t size = -1;
        REQUIRE(zelph_cluster_count(engine, "probe", &size) == ZELPH_OK);
        CHECK(size == 0);
    }
}

TEST_CASE("capi: a rule whose consequence has a predicate no condition binds is refused, and nothing is written")
{
    // zelph_rule writes the rule that a typed line or a Janet builder writes,
    // and those refuse a consequence whose predicate is not bound by any
    // condition: for `X W nil` the rule derived nothing, and for `W W nil` a
    // firing gave W a new node, which stood as a relation type no one named
    // and printed as "->". zelph_rule wrote both rules without verification.
    Engine engine;

    const zelph_node waits = engine.node("waits");
    const zelph_node nil   = engine.node("nil");

    zelph_node x = 0, w = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "W", &w) == ZELPH_OK);

    zelph_node condition   = 0;
    zelph_node consequence = 0;
    REQUIRE(zelph_fact(engine, x, waits, &nil, 1, &condition) == ZELPH_OK);
    SUBCASE("the predicate alone") { REQUIRE(zelph_fact(engine, x, w, &nil, 1, &consequence) == ZELPH_OK); }
    SUBCASE("the predicate and the subject") { REQUIRE(zelph_fact(engine, w, w, &nil, 1, &consequence) == ZELPH_OK); }

    REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);
    zelph_node rule = 0;
    CHECK(zelph_rule(engine, &condition, 1, &consequence, 1, &rule) == ZELPH_RUNTIME_ERROR);
    const std::string message = zelph_last_error();
    CHECK(message.find("zelph_rule: The predicate of") != std::string::npos);
    CHECK(message.find("is the variable W, which no condition binds") != std::string::npos);

    int64_t size = -1;
    REQUIRE(zelph_cluster_count(engine, "probe", &size) == ZELPH_OK);
    CHECK(size == 0);

    // Nothing fires: the run writes nothing beside the fact.
    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, engine.node("ann"), waits, &nil, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_cluster_count(engine, "probe", &size) == ZELPH_OK);
    const int64_t written = size;
    REQUIRE(zelph_run(engine) == ZELPH_OK);
    REQUIRE(zelph_cluster_count(engine, "probe", &size) == ZELPH_OK);
    CHECK(size == written);
}

TEST_CASE("capi: a rule whose consequence predicate a condition binds is written and fires")
{
    // The refusal concerns a predicate that remains unbound by any condition:
    // whenever a condition names it, it becomes bound upon every firing.
    Engine engine;

    const zelph_node waits = engine.node("waits");
    const zelph_node nil   = engine.node("nil");
    const zelph_node ok    = engine.node("ok");

    zelph_node x = 0, w = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "W", &w) == ZELPH_OK);

    zelph_node condition   = 0;
    zelph_node consequence = 0;
    REQUIRE(zelph_fact(engine, x, w, &nil, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, w, &ok, 1, &consequence) == ZELPH_OK);

    zelph_node rule = 0;
    REQUIRE(zelph_rule(engine, &condition, 1, &consequence, 1, &rule) == ZELPH_OK);

    const zelph_node ann  = engine.node("ann");
    zelph_node       fact = 0;
    REQUIRE(zelph_fact(engine, ann, waits, &nil, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);

    int32_t exists = 0;
    REQUIRE(zelph_exists(engine, ann, waits, &ok, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
}

// zelph_fact writes the `=>` fact that zelph_rule generates, with no
// verification performed at that location: expressed in this form,
// `(X waits nil) => (W W nil)` derived `-> -> nil` from `ann waits nil`. The
// call is unable to reject it, since an inner rule of a generator shares the
// same shape (next case), and the C ABI runs no code whose termination might
// be awaited; thus, the next run checks it, and any rejection becomes the
// run's error. Once the rule vanishes, the following run derives nothing.
TEST_CASE("capi: a `=>` written with zelph_fact whose consequence predicate no condition binds is refused by the next run")
{
    Engine engine;

    const zelph_node waits  = engine.node("waits");
    const zelph_node nil    = engine.node("nil");
    const zelph_node causes = engine.node("=>");

    zelph_node x = 0, w = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "W", &w) == ZELPH_OK);

    zelph_node condition = 0, consequence = 0, rule = 0;
    REQUIRE(zelph_fact(engine, x, waits, &nil, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, w, w, &nil, 1, &consequence) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, condition, causes, &consequence, 1, &rule) == ZELPH_OK);

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, engine.node("ann"), waits, &nil, 1, &fact) == ZELPH_OK);

    CHECK(zelph_run(engine) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("zelph_fact: The predicate of \"W W nil\" in the rule's consequence is the variable W") != std::string::npos);

    int32_t exists = 1;
    REQUIRE(zelph_exists(engine, condition, causes, &consequence, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);
    int64_t size = -1;
    REQUIRE(zelph_cluster_count(engine, "probe", &size) == ZELPH_OK);
    CHECK(size == 0);
}

// Before the subsequent run, a zelph_save stored the rule in the file
// unchecked, and a load retrieved it as a rule the graph already holds, which
// no later run checks. The save checks first, just as a run does: the refusal
// constitutes its error, the file remains unwritten, and the rule disappears,
// allowing the next save to succeed.
TEST_CASE("capi: a `=>` written with zelph_fact whose consequence predicate no condition binds is refused by the next save")
{
    Engine engine;

    const zelph_node waits  = engine.node("waits");
    const zelph_node nil    = engine.node("nil");
    const zelph_node causes = engine.node("=>");

    zelph_node x = 0, w = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "W", &w) == ZELPH_OK);

    zelph_node condition = 0, consequence = 0, rule = 0;
    REQUIRE(zelph_fact(engine, x, waits, &nil, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, w, w, &nil, 1, &consequence) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, condition, causes, &consequence, 1, &rule) == ZELPH_OK);

    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_capi_fact_rule_save.bin").string();
    std::filesystem::remove(path);

    CHECK(zelph_save(engine, path.c_str()) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("zelph_fact: The predicate of \"W W nil\" in the rule's consequence is the variable W") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(path));

    int32_t exists = 1;
    REQUIRE(zelph_exists(engine, condition, causes, &consequence, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    CHECK(zelph_save(engine, path.c_str()) == ZELPH_OK);
    CHECK(std::filesystem::exists(path));
    std::filesystem::remove(path);
}

// A rule authored by zelph_mention is out of force until zelph_fact claims it.
// Without verification, that claim put a rule in force whose consequence
// predicate no condition binds, and the next run fired it. The next run checks
// the claim just as it checks a rule zelph_fact generates, yet the rule
// predated the call: the rejection retracts the claim, and the rule stays a
// mention. A further run derives nothing, and writing it again as a mention
// creates nothing.
TEST_CASE("capi: a mentioned `=>` that zelph_fact claims into force is checked by the next run, and the refusal takes back the claim alone")
{
    Engine engine;

    const zelph_node waits  = engine.node("waits");
    const zelph_node nil    = engine.node("nil");
    const zelph_node causes = engine.node("=>");

    zelph_node x = 0, w = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "W", &w) == ZELPH_OK);

    zelph_node condition = 0, consequence = 0, mentioned = 0;
    REQUIRE(zelph_mention(engine, x, waits, &nil, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_mention(engine, w, w, &nil, 1, &consequence) == ZELPH_OK);
    REQUIRE(zelph_mention(engine, condition, causes, &consequence, 1, &mentioned) == ZELPH_OK);

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, engine.node("ann"), waits, &nil, 1, &fact) == ZELPH_OK);

    SUBCASE("with a run between the mention and the claim")
    {
        REQUIRE(zelph_run(engine) == ZELPH_OK);
    }
    SUBCASE("claimed before the next run") {}

    zelph_node claimed = 0;
    REQUIRE(zelph_fact(engine, condition, causes, &consequence, 1, &claimed) == ZELPH_OK);
    REQUIRE(claimed == mentioned);

    CHECK(zelph_run(engine) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("zelph_fact: The predicate of \"W W nil\" in the rule's consequence is the variable W") != std::string::npos);

    REQUIRE(zelph_cluster(engine, "probe") == ZELPH_OK);
    CHECK(zelph_run(engine) == ZELPH_OK);
    zelph_node again = 0;
    REQUIRE(zelph_mention(engine, condition, causes, &consequence, 1, &again) == ZELPH_OK);
    CHECK(again == mentioned);
    int64_t size = -1;
    REQUIRE(zelph_cluster_count(engine, "probe", &size) == ZELPH_OK);
    CHECK(size == 0);
}

// The inner rule of a generator takes its predicate from the generator's
// binding. Constructed through zelph_fact calls, it maintains the shape
// refused above until the generator holds it, which occurs by the time of
// the run.
TEST_CASE("capi: a rule generator built from zelph_fact calls passes the run and fires")
{
    Engine engine;

    const zelph_node causes  = engine.node("=>");
    const zelph_node inverse = engine.node("inverse");
    const zelph_node child   = engine.node("child");

    zelph_node r = 0, s = 0, a = 0, b = 0;
    REQUIRE(zelph_variable(engine, "R", &r) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "S", &s) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "A", &a) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "B", &b) == ZELPH_OK);

    zelph_node inner_condition = 0, inner_consequence = 0, inner = 0, condition = 0, generator = 0;
    REQUIRE(zelph_fact(engine, a, r, &b, 1, &inner_condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, b, s, &a, 1, &inner_consequence) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, inner_condition, causes, &inner_consequence, 1, &inner) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, r, inverse, &s, 1, &condition) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, condition, causes, &inner, 1, &generator) == ZELPH_OK);

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, engine.node("parent"), inverse, &child, 1, &fact) == ZELPH_OK);
    const zelph_node bob = engine.node("bob");
    REQUIRE(zelph_fact(engine, engine.node("ann"), engine.node("parent"), &bob, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);

    const zelph_node ann    = engine.node("ann");
    int32_t          exists = 0;
    REQUIRE(zelph_exists(engine, bob, child, &ann, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
}

TEST_CASE("capi: zelph_negate returns the negated condition, and the pattern stays positive")
{
    // zelph_negate wrote `~ negation` ABOUT the pattern and handed the
    // pattern back, causing a rule built with it to name the pattern itself
    // -- and every other rule over the same node did the same, subsequently
    // reading it as negated. It now returns the fact ((pattern) ~ negation),
    // which a rule takes in place of the pattern.
    Engine engine;

    const zelph_node go = engine.node("go");
    const zelph_node t  = engine.node("t");
    const zelph_node k  = engine.node("k");
    const zelph_node b  = engine.node("b");

    zelph_node fact = 0;
    REQUIRE(zelph_fact(engine, t, go, &k, 1, &fact) == ZELPH_OK);

    zelph_node x = 0;
    zelph_node y = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "Y", &y) == ZELPH_OK);

    zelph_node pattern = 0;
    REQUIRE(zelph_fact(engine, x, engine.node("p"), &b, 1, &pattern) == ZELPH_OK);

    zelph_node negated = 0;
    REQUIRE(zelph_negate(engine, pattern, &negated) == ZELPH_OK);
    CHECK(negated != pattern);

    // It is the fact `((X p b) ~ negation)`.
    zelph_node subject   = 0;
    zelph_node predicate = 0;
    zelph_node object    = 0;
    size_t     count     = 1;
    REQUIRE(zelph_fact_parts(engine, negated, &subject, &predicate, &object, &count) == ZELPH_OK);
    REQUIRE(count == 1);
    CHECK(subject == pattern);
    CHECK(predicate == engine.node("~"));
    CHECK(name_of(engine, object) == "negation");

    // A rule governing the negated condition, another rule governing
    // the pattern itself.
    zelph_node walks = 0;
    REQUIRE(zelph_fact(engine, x, go, &y, 1, &walks) == ZELPH_OK);
    zelph_node ok  = 0;
    zelph_node bad = 0;
    REQUIRE(zelph_fact(engine, x, engine.node("ok"), &y, 1, &ok) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, x, engine.node("bad"), &y, 1, &bad) == ZELPH_OK);

    const zelph_node negating[2] = {walks, negated};
    const zelph_node positive[2] = {walks, pattern};
    zelph_node       rule        = 0;
    REQUIRE(zelph_rule(engine, negating, 2, &ok, 1, &rule) == ZELPH_OK);
    REQUIRE(zelph_rule(engine, positive, 2, &bad, 1, &rule) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);

    int32_t exists = 0;
    REQUIRE(zelph_exists(engine, t, engine.node("ok"), &k, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);
    REQUIRE(zelph_exists(engine, t, engine.node("bad"), &k, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);
}

TEST_CASE("capi: zelph_negate refuses a negated condition and a node that is not a statement")
{
    // Applying a second negation to a condition that is already negated
    // requires a second negation level, and treating a name as negated
    // results in a fact that no rule reads as negated. Both were written
    // and returned as though they were conditions.
    Engine engine;

    zelph_node       pattern = 0;
    const zelph_node b       = engine.node("b");
    REQUIRE(zelph_fact(engine, engine.node("a"), engine.node("p"), &b, 1, &pattern) == ZELPH_OK);
    zelph_node negated = 0;
    REQUIRE(zelph_negate(engine, pattern, &negated) == ZELPH_OK);

    zelph_node twice = 0;
    CHECK(zelph_negate(engine, negated, &twice) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("does not nest") != std::string::npos);

    zelph_node name = 0;
    CHECK(zelph_negate(engine, engine.node("a"), &name) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("applies to a statement") != std::string::npos);
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

// `a p b c` and `a p c` both state `a p c`, and the query reported the
// binding Who = c once for each of them: two rows that a caller could not
// distinguish. A query answers each binding only once (see
// test_reasoning.cpp, "a binding that a narrower and a wider fact both state
// is answered once").
TEST_CASE("capi: a query reports a binding two facts state once")
{
    Engine engine;

    const zelph_node a         = engine.node("a");
    const zelph_node p         = engine.node("p");
    const zelph_node objects[] = {engine.node("b"), engine.node("c")};
    zelph_node       fact      = 0;
    REQUIRE(zelph_fact(engine, a, p, objects, 2, &fact) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, a, p, &objects[1], 1, &fact) == ZELPH_OK);

    zelph_node who = 0;
    REQUIRE(zelph_variable(engine, "Who", &who) == ZELPH_OK);
    zelph_node pattern = 0;
    REQUIRE(zelph_fact(engine, a, p, &who, 1, &pattern) == ZELPH_OK);

    size_t pairs = 0;
    size_t rows  = 0;
    CHECK(zelph_query(engine, pattern, nullptr, &pairs, nullptr, &rows) == ZELPH_BUFFER_TOO_SMALL);
    CHECK(rows == 2);

    std::vector<zelph_node> flat(pairs * 2, 0);
    std::vector<size_t>     sizes(rows, 0);
    REQUIRE(zelph_query(engine, pattern, flat.data(), &pairs, sizes.data(), &rows) == ZELPH_OK);
    std::vector<std::string> found;
    for (size_t pair = 0; pair < pairs; ++pair)
        found.push_back(name_of(engine, flat[pair * 2 + 1]));
    std::sort(found.begin(), found.end());
    CHECK(found == std::vector<std::string>{"b", "c"});
}

TEST_CASE("capi: a cluster turns a graph that otherwise only grows into a workspace")
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

// A caller that poses a query inside a cluster it subsequently drops leaves
// nothing behind, as long as the drop takes back what the query wrote. The
// pattern was the first fact of `b` as a predicate and established it as a
// relation type, and the drop took the pattern while `b ~ ->` remained
// unchanged.
TEST_CASE("capi: a question asked in a cluster that is then dropped takes back the relation-type declaration it wrote")
{
    Engine engine;

    const zelph_node is_a  = engine.node("~");
    const zelph_node arrow = engine.node("->");
    const zelph_node b     = engine.node("b");
    zelph_node       fact  = 0;
    REQUIRE(zelph_fact(engine, engine.node("a"), engine.node("likes"), &b, 1, &fact) == ZELPH_OK);

    int32_t declared = 1;
    REQUIRE(zelph_exists(engine, b, is_a, &arrow, 1, &declared) == ZELPH_OK);
    REQUIRE(declared == 0);

    REQUIRE(zelph_cluster(engine, "question") == ZELPH_OK);
    zelph_node x = 0;
    zelph_node y = 0;
    REQUIRE(zelph_variable(engine, "X", &x) == ZELPH_OK);
    REQUIRE(zelph_variable(engine, "Y", &y) == ZELPH_OK);
    zelph_node pattern = 0;
    REQUIRE(zelph_fact(engine, x, b, &y, 1, &pattern) == ZELPH_OK);

    size_t pairs = 0;
    size_t rows  = 0;
    CHECK(zelph_query(engine, pattern, nullptr, &pairs, nullptr, &rows) == ZELPH_OK);
    CHECK(rows == 0);

    REQUIRE(zelph_cluster(engine, nullptr) == ZELPH_OK);
    int64_t removed = 0;
    REQUIRE(zelph_cluster_drop(engine, "question", &removed) == ZELPH_OK);
    // The drop takes X, Y, the pattern, and the declaration of the
    // predicate it relies on.
    CHECK(removed == 4);

    REQUIRE(zelph_exists(engine, b, is_a, &arrow, 1, &declared) == ZELPH_OK);
    CHECK(declared == 0);

    // The subsequent initial fact of `b` as a predicate declares it
    // as usual.
    const zelph_node d = engine.node("d");
    REQUIRE(zelph_fact(engine, engine.node("c"), b, &d, 1, &fact) == ZELPH_OK);
    REQUIRE(zelph_exists(engine, b, is_a, &arrow, 1, &declared) == ZELPH_OK);
    CHECK(declared == 1);
}

TEST_CASE("capi: a cluster name beginning with \"__\" is refused")
{
    // The engine generates scratch clusters with a prefix of "__". If a caller
    // attempts to use a cluster name starting with "__", it would be
    // interpreted as a scratch cluster and discarded together with the caller's
    // work in it, so zelph_cluster rejects such a name just as .cluster does:
    // the caller can see that the argument is invalid, and the currently active
    // cluster remains active.
    Engine engine;
    REQUIRE(zelph_cluster(engine, "scratch") == ZELPH_OK);

    CHECK(zelph_cluster(engine, "__rule") == ZELPH_INVALID_ARGUMENT);
    CHECK(std::string(zelph_last_error()).find("reserved for the engine's own scratch clusters") != std::string::npos);

    char* active = nullptr;
    REQUIRE(zelph_cluster_active(engine, &active) == ZELPH_OK);
    CHECK(std::string(active ? active : "") == "scratch");
    zelph_string_free(active);

    int64_t size = 0;
    REQUIRE(zelph_cluster_count(engine, "__rule", &size) == ZELPH_OK);
    CHECK(size == -1);
}

TEST_CASE("capi: sets, collections and the directional relations")
{
    Engine engine;

    const zelph_node a           = engine.node("a");
    const zelph_node b           = engine.node("b");
    const zelph_node elements[2] = {a, b};

    // A set constant is identified by its members, so the same elements
    // always yield the same node ...
    zelph_node first  = 0;
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
    zelph_node condition   = 0;
    zelph_node consequence = 0;
    zelph_node rule        = 0;
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
        case ZELPH_CHANNEL_OUT:
            c->out++;
            break;
        case ZELPH_CHANNEL_ERROR:
            c->error++;
            break;
        case ZELPH_CHANNEL_DIAGNOSTIC:
            c->diagnostic++;
            break;
        case ZELPH_CHANNEL_PROMPT:
            c->prompt++;
            break;
        default:
            break;
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
    const zelph_node o1     = engine.node("o1");

    REQUIRE(zelph_nn_connect_layers(engine, in, hidden, 0.0, 1, nullptr) == ZELPH_OK);
    REQUIRE(zelph_nn_connect_layers(engine, hidden, out, 0.0, 2, nullptr) == ZELPH_OK);

    const zelph_node layers[3] = {in, hidden, out};

    // Drive the single hidden unit firmly negative: with i1 -> h1 negative and
    // i1 the only active input, the unit is off for every input this net can
    // see.
    auto put_the_unit_off = [&]()
    {
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
    auto train_and_report = [&](int32_t activation) -> std::pair<double, double>
    {
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

    zelph_net relu  = -1;
    zelph_net leaky = -1;
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_RELU, &relu) == ZELPH_OK);
    REQUIRE(zelph_nn_compile(engine, layers, 2, ZELPH_ACTIVATION_LEAKY_RELU, &leaky) == ZELPH_OK);

    const zelph_node i1  = engine.node("i1");
    zelph_node       top = 0;
    double           a = 0, b = 0;
    size_t           count = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, relu, &i1, nullptr, 1, 1, &top, &a, &count) == ZELPH_OK);
    count = 1;
    REQUIRE(zelph_nn_eval_nodes(engine, leaky, &i1, nullptr, 1, 1, &top, &b, &count) == ZELPH_OK);
    CHECK(a == doctest::Approx(b));
}

// Two implementations of one question have to agree, and this one did not.
// "Does this fact exist" is answered on the Janet side by check_fact, then by
// a fact carrying FURTHER objects, then by asking whether anyone CLAIMED what
// was found. The C side asked only the first, so it said "no" about a triple
// the engine's own rules fire on -- an ABI contradicting the engine it speaks
// for, which is worse than either answer alone.
TEST_CASE("capi: existence is the question the engine answers, not the exact hash")
{
    Engine engine;

    const zelph_node a = engine.node("a");
    const zelph_node p = engine.node("p");
    const zelph_node b = engine.node("b");
    const zelph_node c = engine.node("c");

    // One fact with two objects -- not two facts. "a p b" is a different node
    // and does not exist; the question is whether it HOLDS.
    const zelph_node objects[2] = {b, c};
    zelph_node       wide       = 0;
    REQUIRE(zelph_fact(engine, a, p, objects, 2, &wide) == ZELPH_OK);

    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, a, p, &b, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    // And the control that says the answer is the engine's rather than a
    // convenient one: a rule with exactly that condition derives from it.
    zelph_node       cond  = 0;
    zelph_node       cons  = 0;
    zelph_node       rule  = 0;
    const zelph_node fired = engine.node("fired");
    REQUIRE(zelph_fact(engine, a, p, &b, 1, &cond) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, a, fired, &b, 1, &cons) == ZELPH_OK);
    REQUIRE(zelph_rule(engine, &cond, 1, &cons, 1, &rule) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);

    int32_t derived = -1;
    REQUIRE(zelph_exists(engine, a, fired, &b, 1, &derived) == ZELPH_OK);
    CHECK(derived == exists);

    // A fact nobody entered is still absent: the containment reading widens
    // what counts as holding, it does not stop asking.
    const zelph_node d      = engine.node("d");
    int32_t          absent = -1;
    REQUIRE(zelph_exists(engine, a, p, &d, 1, &absent) == ZELPH_OK);
    CHECK(absent == 0);
}

// zelph_exists answers the question zelph/exists answers, and both answer
// what a ground rule condition matches. A condition reads a statement within
// it using the subset reading -- `(a p b c) bad k` states `(a p c) bad k` --
// yet the two existence checks widened only the top level, and an unclaimed
// exact node, a statement simply written down, concealed the claimed wider
// fact even at that level.
TEST_CASE("capi: zelph_exists reads a statement among its arguments as a ground rule condition does")
{
    Engine engine;

    const zelph_node a   = engine.node("a");
    const zelph_node p   = engine.node("p");
    const zelph_node b   = engine.node("b");
    const zelph_node c   = engine.node("c");
    const zelph_node bad = engine.node("bad");
    const zelph_node k   = engine.node("k");

    const zelph_node objects[2] = {b, c};
    zelph_node       wide       = 0;
    zelph_node       stated     = 0;
    REQUIRE(zelph_fact(engine, a, p, objects, 2, &wide) == ZELPH_OK);
    REQUIRE(zelph_fact(engine, wide, bad, &k, 1, &stated) == ZELPH_OK);

    // The narrower statements, written down without claiming them.
    zelph_node narrow_b = 0;
    zelph_node narrow_c = 0;
    REQUIRE(zelph_mention(engine, a, p, &b, 1, &narrow_b) == ZELPH_OK);
    REQUIRE(zelph_mention(engine, a, p, &c, 1, &narrow_c) == ZELPH_OK);

    // The control: a rule with the nested condition fires.
    const zelph_node x     = engine.node("x");
    const zelph_node y     = engine.node("y");
    const zelph_node fired = engine.node("fired");
    zelph_node       cond  = 0;
    zelph_node       cons  = 0;
    zelph_node       rule  = 0;
    REQUIRE(zelph_mention(engine, narrow_c, bad, &k, 1, &cond) == ZELPH_OK);
    REQUIRE(zelph_mention(engine, x, fired, &y, 1, &cons) == ZELPH_OK);
    REQUIRE(zelph_rule(engine, &cond, 1, &cons, 1, &rule) == ZELPH_OK);
    REQUIRE(zelph_run(engine) == ZELPH_OK);

    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, x, fired, &y, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    SUBCASE("a statement as the subject")
    {
        exists = -1;
        REQUIRE(zelph_exists(engine, narrow_c, bad, &k, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);
    }

    SUBCASE("the narrower statement itself, written down beside the claimed wider one")
    {
        exists = -1;
        REQUIRE(zelph_exists(engine, a, p, &b, 1, &exists) == ZELPH_OK);
        CHECK(exists == 1);
    }

    SUBCASE("a statement no fact states")
    {
        const zelph_node d      = engine.node("d");
        zelph_node       narrow = 0;
        REQUIRE(zelph_mention(engine, a, p, &d, 1, &narrow) == ZELPH_OK);
        exists = -1;
        REQUIRE(zelph_exists(engine, narrow, bad, &k, 1, &exists) == ZELPH_OK);
        CHECK(exists == 0);
    }

    SUBCASE("a question that holds a variable")
    {
        // The identical question from C: no fact holding a variable is
        // claimed, and the matcher read the question as a pattern that
        // `a p b c` fits.
        zelph_node variable = 0;
        REQUIRE(zelph_variable(engine, "V", &variable) == ZELPH_OK);
        exists = -1;
        REQUIRE(zelph_exists(engine, a, p, &variable, 1, &exists) == ZELPH_OK);
        CHECK(exists == 0);
    }
}

// The one case in this file that builds its fixture with the REPL, and the
// reason is the point of the case: a rule-pattern marking and a refutation
// cannot be made from the C side at all -- they ride on the parser -- while a
// caller reaches them the moment it loads a network somebody else saved. So
// the data is written outside and every assertion is made through the header,
// which is what the rest of this file is protecting.
//
// The script engine keeps a process-wide instance, and two live ones corrupt
// the heap. The REPL engine is scoped OUT before the C one is made.
TEST_CASE("capi: a loaded network's unclaimed facts are not reported as existing")
{
    const auto file = fs::temp_directory_path() / "zelph_capi_unclaimed.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        zelph::test::process_lines(interactive, R"(
c q d
(a p b) => (x r y)
¬(e p f)
)");
        interactive.process(".save \"" + file.string() + "\"");
    }

    Engine engine;
    REQUIRE(zelph_load(engine, file.string().c_str()) == ZELPH_OK);

    const zelph_node p = engine.node("p");
    const zelph_node q = engine.node("q");

    const zelph_node b = engine.node("b");
    const zelph_node d = engine.node("d");
    const zelph_node f = engine.node("f");

    // The rule's own condition: present as structure, claimed by nobody.
    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, engine.node("a"), p, &b, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    // Refuted: the graph claims the opposite of it.
    REQUIRE(zelph_exists(engine, engine.node("e"), p, &f, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);

    // And an ordinary fact from the same file still answers, so the two above
    // are about the markings and not about the load.
    REQUIRE(zelph_exists(engine, engine.node("c"), q, &d, 1, &exists) == ZELPH_OK);
    CHECK(exists == 1);

    fs::remove(file);
}

TEST_CASE("capi: a membership in a collection of a rule's text is refused with the error code, and nothing is written")
{
    // Once a rule has been authored, its text becomes immutable. If a caller
    // accesses the node of a rule's own collection -- here via a name given
    // to it -- and writes a member into it, this would alter the rule's
    // meaning. In response, zelph_fact rejects the action with
    // ZELPH_RUNTIME_ERROR and issues a message naming the collection, while
    // performing no write operation.
    const auto file = fs::temp_directory_path() / "zelph_capi_rule_text.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("(a p b) => (c q @{d})");

        // The collection lacks a name of its own; `.node` identifies it
        // through its id.
        const auto listed = [&](const std::string& label)
        {
            const std::string prefix = "- " + label + " (ID ";
            for (const auto& event : collector.events())
                if (const std::size_t at = event.text.find(prefix); at != std::string::npos)
                    return event.text.substr(at + prefix.size(), event.text.find(')', at + prefix.size()) - at - prefix.size());
            return std::string{};
        };
        collector.clear();
        interactive.process(".node c");
        const std::string consequence = listed("c q @{d}");
        REQUIRE_FALSE(consequence.empty());
        collector.clear();
        interactive.process(".node " + consequence);
        const std::string collection = listed("@{d}");
        REQUIRE_FALSE(collection.empty());
        interactive.process(".name " + collection + " zelph bucket");
        interactive.process(".save \"" + file.string() + "\"");
    }

    Engine engine;
    REQUIRE(zelph_load(engine, file.string().c_str()) == ZELPH_OK);
    fs::remove(file);

    zelph_node collection = 0;
    REQUIRE(zelph_resolve(engine, "bucket", "zelph", &collection) == ZELPH_OK);
    const zelph_node in = engine.node("in");
    const zelph_node zz = engine.node("zz");

    zelph_node fact = 0;
    CHECK(zelph_fact(engine, zz, in, &collection, 1, &fact) == ZELPH_RUNTIME_ERROR);
    CHECK(fact == 0);
    const std::string error = zelph_last_error();
    CHECK(error.find("bucket is a collection of a rule's own text") != std::string::npos);

    int32_t exists = -1;
    REQUIRE(zelph_exists(engine, zz, in, &collection, 1, &exists) == ZELPH_OK);
    CHECK(exists == 0);
    zelph_node  members[4] = {};
    std::size_t count      = 4;
    REQUIRE(zelph_sources(engine, in, collection, members, &count) == ZELPH_OK);
    CHECK(std::find(members, members + count, zz) == members + count);

    // `d` is already a member, so stating it again introduces no new node;
    // however, a statement constitutes a claim, and it would turn the rule's
    // literal member into data. It is rejected just like any other write,
    // and the member remains as rule text, which no query answers.
    const zelph_node d = engine.node("d");
    CHECK(zelph_fact(engine, d, in, &collection, 1, &fact) == ZELPH_RUNTIME_ERROR);
    CHECK(std::string(zelph_last_error()).find("is fixed once the rule is written") != std::string::npos);

    zelph_node o = 0;
    REQUIRE(zelph_variable(engine, "O", &o) == ZELPH_OK);
    zelph_node pattern = 0;
    REQUIRE(zelph_fact(engine, d, in, &o, 1, &pattern) == ZELPH_OK);
    std::size_t pairs = 0;
    std::size_t rows  = 0;
    CHECK(zelph_query(engine, pattern, nullptr, &pairs, nullptr, &rows) == ZELPH_OK);
    CHECK(rows == 0);
}
