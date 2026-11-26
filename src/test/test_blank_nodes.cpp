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

#include "concurrency/thread_count.hpp"
#include "directory_rewrite.hpp"
#include "network/reasoning.hpp"
#include "platform/platform_utils.hpp"
#include "string/node_to_string.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace zelph::test;
using zelph::network::Node;
using zelph::network::Zelph;

// ---------------------------------------------------------------------------
// Blank nodes: a resource in RDF that lacks an IRI, belonging to a node id
// class of its own. The identifier of such a node is derived from the
// document's identity and its label, or from its position, and from nothing
// the engine counts, ensuring that a reader can assign each blank node its id
// in a single pass, the same document always yields identical nodes upon
// every import, and no two documents ever share a single one.
// mkdocs/docs/rdf-spec/blank-nodes.md specifies all of it.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    const std::string doc = "http://example.org/doc";

    // What Python computes for the same bytes: hashlib.blake2b(digest_size=8,
    // person=b"zelph:blank:v1") over the bytes specified, a length or an
    // ordinal as struct.pack("<Q", n), and of the digest, read with
    // int.from_bytes(digest, "little"), the low 58 bits under the class bit
    // 1 << 58. The identity starting with 200 times "a" makes the hashed bytes
    // exceed the 128 of a single BLAKE2b block.
    const std::string long_identity = "http://example.org/" + std::string(200, 'a');

    fs::path write_script(const std::string& name, const std::string& body)
    {
        const auto path = fs::temp_directory_path() / ("zelph_blank_nodes_" + name);
        std::ofstream(path) << body;
        return path;
    }

    // The sole response to a query, excluding the "Answer: " prefix.
    std::string answer_of(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& query)
    {
        collector.clear();
        interactive.process(query);
        const std::vector<std::string> answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        return answers.front();
    }

    // The way the line starts in which a run says what it left unwritten
    // because a blank node would have been a predicate.
    const std::string note_start = "Note: statements whose predicate would be a blank node were not written, for ";

    // The lines of that kind among what was written, in the order in
    // which they were written.
    std::vector<std::string> unwritten_notes(const zelph::io::OutputCollector& collector)
    {
        std::vector<std::string> notes;
        for (const auto& event : collector.events())
            if (event.text.rfind(note_start, 0) == 0) notes.push_back(event.text);
        return notes;
    }

    // The number of nodes that `line` introduces into a session having
    // read `setup`, including whatever the run behind the line derives
    // from it. The session is a separate one, and a process holds one
    // engine at a time, so the caller has none open.
    std::size_t nodes_added(const std::string& setup, const std::string& line)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, ".identity http://example.org/doc\n" + setup);
        const auto before = interactive.graph()->count();
        interactive.process(line);
        return interactive.graph()->count() - before;
    }
}

// --- the derivation of an id -------------------------------------------------

// The specification's table indicates which bytes are included in the hash,
// and a different resulting id would make the nodes of every saved network
// foreign to the next engine. Each id in this context was computed by an
// alternative implementation, so a change to the bytes, the personalization,
// the digest length, or the method of reading the digest fails at the input
// it concerns.
TEST_CASE("blank nodes: the id of a labelled node is the one that hashlib.blake2b derives")
{
    CHECK(Zelph::blank_id(doc, "b0") == 0x068feb5243a0018dull);
    CHECK(Zelph::blank_id(doc, "b1") == 0x05a67d20418a1f7dull);

    SUBCASE("the identity of the document enters the id")
    {
        CHECK(Zelph::blank_id("http://example.org/other", "b0") == 0x06447ae5244b6adaull);
        CHECK(Zelph::blank_id("", "b0") == 0x043c42a80664c7d2ull);
    }

    SUBCASE("identity and label are hashed as their UTF-8 bytes")
    {
        CHECK(Zelph::blank_id(doc, "Grüße-東京") == 0x066eb781380dfdeeull);
        CHECK(Zelph::blank_id("http://example.org/Grüße", "b0") == 0x04b41d6663b0f901ull);
    }

    SUBCASE("a text longer than one block of the hash")
    {
        CHECK(Zelph::blank_id(long_identity, "b0") == 0x079d4b309e46228aull);
        CHECK(Zelph::blank_id(doc, std::string(300, 'x')) == 0x079c00c43b0e1660ull);
    }

    SUBCASE("the label of an unknown value of Wikidata, under the identity its data has")
    {
        CHECK(Zelph::blank_id("http://www.wikidata.org/", "9bdf84525ed773079f49f76ed44d41be") == 0x05a2290e2fc47c4aull);
    }

    SUBCASE("the lengths are part of the bytes, so the border between identity and label is too")
    {
        // The tag byte between identity and label is 0x00 for a label, so
        // a zero byte at the end of the identity and another at the start
        // of the label produce identical byte sequences once the two
        // length values are excluded: 'a', 0, 0, 'b' either way. Only the
        // lengths maintain the distinction between the two, which a pair
        // such as ("ab", "c") and ("a", "bc") does not demonstrate,
        // because the tag byte stands at different positions in those
        // cases.
        using namespace std::string_literals;
        CHECK(Zelph::blank_id("a\0"s, "b"s) == 0x062540cf8b370ce9ull);
        CHECK(Zelph::blank_id("a"s, "\0b"s) == 0x071613c2be4da4faull);
        CHECK(Zelph::blank_id(doc, "") == 0x04b482b825c8581dull);
    }
}

// An anonymous node carries no label, and its identifier originates in its
// position among the anonymous nodes within its document. The readers
// responsible for determining these positions do not yet exist; the function
// they are destined to invoke is held here.
TEST_CASE("blank nodes: the id of an anonymous node comes from its ordinal")
{
    CHECK(Zelph::blank_id(doc, Zelph::BlankSource::anonymous, 0) == 0x0589e7ef2255771dull);
    CHECK(Zelph::blank_id(doc, Zelph::BlankSource::anonymous, 1) == 0x07b7a61a17e5739bull);
    CHECK(Zelph::blank_id(long_identity, Zelph::BlankSource::anonymous, 7) == 0x05cfbf91d7ca1e57ull);

    SUBCASE("the ordinal is hashed as a 64-bit number")
    {
        CHECK(Zelph::blank_id(doc, Zelph::BlankSource::anonymous, std::uint64_t{1} << 32) == 0x049c0fc46ea3d98full);
        CHECK(Zelph::blank_id(doc, Zelph::BlankSource::anonymous, ~std::uint64_t{0}) == 0x06fa329a4e3b097aull);
    }
}

// The cells of a collection are tallied independently of the anonymous
// nodes, ensuring that a collection, the majority of whose cells never
// become nodes, does not move the ordinal position of an anonymous node
// located after it.
TEST_CASE("blank nodes: the id of a collection cell comes from an ordinal of its own")
{
    CHECK(Zelph::blank_id(doc, Zelph::BlankSource::collection_cell, 0) == 0x04c8af2d76d6fcd1ull);
    CHECK(Zelph::blank_id(doc, Zelph::BlankSource::collection_cell, 1) == 0x060725574ec5d843ull);
    CHECK(Zelph::blank_id(doc, Zelph::BlankSource::collection_cell, ~std::uint64_t{0}) == 0x078a286f9522ed28ull);
}

// No label that a document writes may reach the id of any of its anonymous
// nodes. The empty label is hashed as its length alone, eight zero bytes --
// precisely the bytes that an ordinal of 0 is hashed as: following the
// identity, the three differ only in the tag byte.
TEST_CASE("blank nodes: the tag byte keeps a label, an anonymous node and a collection cell apart")
{
    const Node labelled  = Zelph::blank_id(doc, "");
    const Node anonymous = Zelph::blank_id(doc, Zelph::BlankSource::anonymous, 0);
    const Node cell      = Zelph::blank_id(doc, Zelph::BlankSource::collection_cell, 0);

    CHECK(labelled == 0x04b482b825c8581dull);
    CHECK(anonymous == 0x0589e7ef2255771dull);
    CHECK(cell == 0x04c8af2d76d6fcd1ull);
    CHECK(labelled != anonymous);
    CHECK(labelled != cell);
    CHECK(anonymous != cell);

    SUBCASE("a zero byte within a label is a byte of it")
    {
        CHECK(Zelph::blank_id(doc, std::string(8, '\0')) == 0x074f5e96a7b1479cull);
    }

    SUBCASE("a label has no ordinal")
    {
        CHECK_THROWS_AS(Zelph::blank_id(doc, Zelph::BlankSource::label, 0), std::invalid_argument);
    }
}

// --- the id class ------------------------------------------------------------

// A blank node is identified solely by the bits 63..58 of its identifier,
// specifically 000001, and by no other means. This range resides within the
// class of atoms, positioned between the ids assigned by the node counter and
// the range kept for literal value nodes.
TEST_CASE("blank nodes: the class is the range from 2^58 to below 2^59")
{
    CHECK_FALSE(Zelph::is_blank((std::uint64_t{1} << 58) - 1));
    CHECK(Zelph::is_blank(std::uint64_t{1} << 58));
    CHECK(Zelph::is_blank((std::uint64_t{1} << 59) - 1));
    CHECK_FALSE(Zelph::is_blank(std::uint64_t{1} << 59));
    CHECK_FALSE(Zelph::is_blank(1));

    SUBCASE("every id the derivation gives lies in the class")
    {
        for (int i = 0; i < 1000; ++i)
        {
            CHECK(Zelph::is_blank(Zelph::blank_id(doc, "b" + std::to_string(i))));
            CHECK(Zelph::is_blank(Zelph::blank_id(doc, Zelph::BlankSource::anonymous, static_cast<std::uint64_t>(i))));
            CHECK(Zelph::is_blank(Zelph::blank_id(doc, Zelph::BlankSource::collection_cell, static_cast<std::uint64_t>(i))));
        }
    }

    SUBCASE("to the tests of the other classes a blank node is an atom")
    {
        // The engine reads the structure of a statement via these tests, and no
        // one of them may take a blank node for anything other than an atom.
        const Node blank = Zelph::blank_id(doc, "b0");
        CHECK_FALSE(Zelph::is_hash(blank));
        CHECK_FALSE(Zelph::is_var(blank));
        CHECK_FALSE(Zelph::is_recipe(blank));
        CHECK_FALSE(Zelph::is_value_recipe(blank));
        CHECK_FALSE(Zelph::is_template_id(blank));
    }
}

// The kind of a node is read from the class associated with its id. The
// boundaries are the points at which one class ends and another commences, so
// each such juncture is asked here.
TEST_CASE("blank nodes: node_kind answers the class of an id")
{
    using Kind = Zelph::NodeKind;

    CHECK(Zelph::node_kind(1) == Kind::named);
    CHECK(Zelph::node_kind((std::uint64_t{1} << 58) - 1) == Kind::named);
    CHECK(Zelph::node_kind(std::uint64_t{1} << 58) == Kind::blank);
    CHECK(Zelph::node_kind((std::uint64_t{1} << 59) - 1) == Kind::blank);
    CHECK(Zelph::node_kind(0x1000000000000000ull) == Kind::set_pattern);
    CHECK(Zelph::node_kind(0x1FFFFFFFFFFFFFFFull) == Kind::set_pattern);
    CHECK(Zelph::node_kind(0x2000000000000000ull) == Kind::recipe);
    CHECK(Zelph::node_kind(0x2FFFFFFFFFFFFFFFull) == Kind::recipe);
    CHECK(Zelph::node_kind(0x3000000000000000ull) == Kind::rule_template);
    CHECK(Zelph::node_kind(0x3FFFFFFFFFFFFFFFull) == Kind::rule_template);
    CHECK(Zelph::node_kind(0x4000000000000000ull) == Kind::fact);
    CHECK(Zelph::node_kind(0x7FFFFFFFFFFFFFFFull) == Kind::fact);
    CHECK(Zelph::node_kind(0x8000000000000000ull) == Kind::variable);
    CHECK(Zelph::node_kind(~std::uint64_t{0}) == Kind::variable);
}

// --- a blank node in the graph -----------------------------------------------

// Two occurrences of a label within a document constitute a single resource, so
// the second instance must reach the node established by the first one.
TEST_CASE("blank nodes: blank() creates the node once and finds it afterwards")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();

    const Node id = Zelph::blank_id(doc, "b0");
    CHECK_FALSE(graph->exists(id));

    const auto before = graph->count();
    CHECK(graph->blank(doc, "b0") == id);
    CHECK(graph->exists(id));
    CHECK(graph->count() == before + 1);

    CHECK(graph->blank(doc, "b0") == id);
    CHECK(graph->blank(id) == id);
    CHECK(graph->count() == before + 1);

    SUBCASE("finding the node again leaves the statements it stands in")
    {
        // A node that is created possesses no edges, and a node that is
        // discovered retains the ones it already holds.
        const Node knows = graph->node("knows");
        const Node bob   = graph->node("bob");
        graph->fact(id, knows, {bob});
        graph->fact(bob, knows, {id});

        CHECK(graph->blank(doc, "b0") == id);
        CHECK(graph->check_fact(id, knows, {bob}).is_correct());
        CHECK(graph->check_fact(bob, knows, {id}).is_correct());
        CHECK(graph->get_fact_objects(id, knows).count(bob) == 1);
    }

    SUBCASE("an anonymous node and a collection cell are found by their ordinal")
    {
        const Node anonymous = graph->blank(doc, Zelph::BlankSource::anonymous, 3);
        CHECK(anonymous == Zelph::blank_id(doc, Zelph::BlankSource::anonymous, 3));
        CHECK(graph->blank(doc, Zelph::BlankSource::collection_cell, 3) == Zelph::blank_id(doc, Zelph::BlankSource::collection_cell, 3));
        CHECK(graph->exists(anonymous));
        CHECK(graph->count() == before + 3);
    }

    SUBCASE("an id of another class is refused")
    {
        CHECK_THROWS_AS(graph->blank(Node{42}), std::invalid_argument);
        CHECK_THROWS_AS(graph->blank(Zelph::create_hash(zelph::network::adjacency_set{id})), std::invalid_argument);
    }
}

// The 58 bits do not rely on a counter: a session must assign identifiers to
// its named nodes just as it does without blank nodes, since the work of a run
// depends on the numbering. Two sessions that vary solely in a blank node
// created in between give the next name the identical id.
TEST_CASE("blank nodes: creating one takes no id from the node counter")
{
    // One engine at a time: the script engine beneath it is a single
    // instance per process.
    Node plain_next = 0;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive plain(collector.sink());
        plain.process("a p b");
        plain_next = plain.graph()->node("c");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive with(collector.sink());
    with.process("a p b");
    with.graph()->blank(doc, "b0");
    with.graph()->blank(doc, Zelph::BlankSource::anonymous, 0);
    CHECK(with.graph()->node("c") == plain_next);
}

// A statement may hold a blank node as both its subject and as an
// object, and rules and queries match it just as they match any atom.
TEST_CASE("blank nodes: a blank node is a subject and an object, and rules match it")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.identity http://example.org/doc
(X knows Y) => (Y knownby X)
_:b0 knows bob
bob knows _:b1
)");

    collector.clear();
    interactive.process("A knownby B");
    CHECK(answers_contain(collector, "bob knownby _:@068feb5243a0018d"));
    CHECK(answers_contain(collector, "_:@05a67d20418a1f7d knownby bob"));
    CHECK(collect_answers(collector).size() == 2);
}

// A blank node is a resource and not a relation. As a predicate, it would be
// declared a relation type by its first statement, which transforms the node
// into something other than what it is.
TEST_CASE("blank nodes: a statement with a blank node as its predicate is refused")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
x q _:b0
)");
    const Node blank  = Zelph::blank_id(doc, "b0");
    const auto before = graph->count();

    CHECK_THROWS_WITH(interactive.process("x _:b0 y"),
                      doctest::Contains("fact(): _:@068feb5243a0018d is a blank node, and a blank node cannot be the predicate of a statement."));

    SUBCASE("nothing of the statement is written, the declaration of the predicate included")
    {
        // `y` is the single node that the line produced before the refusal.
        CHECK(graph->count() == before + 1);
        CHECK_FALSE(graph->check_fact(blank, graph->core.IsA, {graph->core.RelationTypeCategory}).is_known());
    }

    SUBCASE("with several objects")
    {
        CHECK_THROWS_WITH(interactive.process("x _:b0 y z"), doctest::Contains("cannot be the predicate of a statement"));

        // A predicate is marked as one whose statements might involve
        // more objects than a condition names before the statement is
        // looked up, and the refusal follows thereafter: the blank node
        // kept the mark of a statement that was never written, `.stat`
        // counted it, and a save stored it.
        CHECK_FALSE(graph->may_hold_wider_statements(blank));
        CHECK_FALSE(graph->may_hold_wider_statements());
        collector.clear();
        interactive.process(".stat");
        CHECK_FALSE(any_event_contains(collector, "Facts with multiple objects"));
    }

    SUBCASE("in a query and in the condition of a rule, which would write the pattern")
    {
        CHECK_THROWS_WITH(interactive.process("X _:b0 Y"), doctest::Contains("cannot be the predicate of a statement"));
        CHECK_THROWS_WITH(interactive.process("(X _:b0 Y) => (X r Y)"), doctest::Contains("cannot be the predicate of a statement"));
    }

    SUBCASE("through the library")
    {
        const Node x = graph->node("x");
        CHECK_THROWS_WITH_AS(graph->fact(x, blank, {x}), doctest::Contains("cannot be the predicate of a statement"), std::runtime_error);
    }

    SUBCASE("the self-fact sugar reads a blank token in predicate position as the blank node")
    {
        // The sugar takes its predicate as a single token. Read as a
        // name, the token built a predicate that is NAMED "_:b0".
        CHECK_THROWS_WITH(interactive.process(":_:b0 x"), doctest::Contains("cannot be the predicate of a statement"));
        CHECK(graph->get_node("_:b0", "zelph") == 0);
    }
}

// A rule may bind a blank node to a variable serving as the predicate in a
// consequence. The statement that such a firing would produce is not among
// those the graph can hold, and the firing leaves it unwritten. Reported as
// a refusal, it was a contradiction found in data that contradicts nothing,
// it left a record of one in the graph, and it ended the firing before the
// other consequences were written.
TEST_CASE("blank nodes: a firing that would make a blank node a predicate leaves the statement unwritten")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
.auto-run
)");
    collector.clear();
    interactive.process("(X p Y) => (a Y b) (X seen Y)");
    const std::string rule = last_out_text(collector);
    interactive.process("x p _:b0");
    const Node blank = Zelph::blank_id(doc, "b0");

    collector.clear();
    interactive.process(".run");

    SUBCASE("it is no contradiction")
    {
        CHECK_FALSE(has_contradiction(collector));
        CHECK_FALSE(any_event_contains(collector, "! ⇐"));
        CHECK_FALSE(any_event_contains(collector, "refused"));
        CHECK(any_event_contains(collector, "Total contradictions found: 0."));
        CHECK(graph->met_contradictions().empty());
    }

    SUBCASE("the other consequences of the same application are written")
    {
        CHECK(answer_of(collector, interactive, "x seen Y") == "x seen _:@068feb5243a0018d");
    }

    SUBCASE("nothing of the statement is in the graph")
    {
        const Node a = graph->get_node("a", "zelph");
        const Node b = graph->get_node("b", "zelph");
        REQUIRE(a != 0);
        REQUIRE(b != 0);
        CHECK_FALSE(graph->check_fact(a, blank, {b}).is_known());

        // Neither the record of a contradiction nor the declaration that
        // would make the blank node a relation type.
        CHECK(graph->count_contradiction_records() == 0);
        CHECK_FALSE(graph->check_fact(blank, graph->core.IsA, {graph->core.RelationTypeCategory}).is_known());
    }

    SUBCASE("the run says what it left out, by rule and blank node")
    {
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note_start + "1 pair of rule and blank node: " + rule + " with _:@068feb5243a0018d."});
    }

    SUBCASE("a binding that makes the predicate a name is written as before")
    {
        interactive.process("x p q");
        interactive.process(".run");
        CHECK(answer_of(collector, interactive, "a q B") == "a q b");
    }
}

// The same applies when the binding makes the blank node a predicate
// further down: inside a statement that the consequence contains, and
// inside a rule that the consequence derives. The instantiation meets the
// refusal of such a statement only during the phase when it builds the
// consequence, and reported it as a contradiction too.
TEST_CASE("blank nodes: a blank node as the predicate of a nested statement or of a derived rule leaves the consequence unwritten")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
.auto-run
)");

    SUBCASE("a statement nested in the consequence")
    {
        collector.clear();
        interactive.process("(X p Y) => (k says (a Y b)) (X seen Y)");
        const std::string rule = last_out_text(collector);
        interactive.process("x p _:b0");

        collector.clear();
        interactive.process(".run");
        CHECK_FALSE(has_contradiction(collector));
        CHECK_FALSE(any_event_contains(collector, "refused"));
        CHECK(any_event_contains(collector, "Total contradictions found: 0."));
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note_start + "1 pair of rule and blank node: " + rule + " with _:@068feb5243a0018d."});

        // The other consequence is written, whereas the nesting one is not.
        CHECK(answer_of(collector, interactive, "x seen Y") == "x seen _:@068feb5243a0018d");
        collector.clear();
        interactive.process("k says Z");
        CHECK(collect_answers(collector).empty());
        CHECK(graph->count_contradiction_records() == 0);
    }

    SUBCASE("what the instantiation built before it met the nested statement is gone")
    {
        // The subject of `about` is built first, as the fact `x r x`,
        // and the statement that follows it is the one that is refused.
        // Kept with the scratch, `x r x` stayed in the graph as a fact
        // not derived by any line, and `.explain` identified it as an
        // axiom. A consequence that nests rule text alone, such as `(c
        // d e)`, builds nothing before the refusal and shows none of
        // this.
        interactive.process("(X p Y) => (k says ((X r X) about (a Y b))) (X seen Y)");
        interactive.process("x p _:b0");
        collector.clear();
        interactive.process(".run");
        CHECK_FALSE(has_contradiction(collector));
        CHECK(unwritten_notes(collector).size() == 1);
        CHECK(answer_of(collector, interactive, "x seen Y") == "x seen _:@068feb5243a0018d");
        for (const char* query : {"x r Z", "S about O", "k says Z"})
        {
            CAPTURE(query);
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        }
    }

    SUBCASE("a statement nested within a set that the consequence writes")
    {
        interactive.process("(X p Y) => (k says {(a Y b)}) (X seen Y)");
        interactive.process("x p _:b0");
        collector.clear();
        interactive.process(".run");
        CHECK_FALSE(has_contradiction(collector));
        CHECK(unwritten_notes(collector).size() == 1);
        CHECK(answer_of(collector, interactive, "x seen Y") == "x seen _:@068feb5243a0018d");
    }

    SUBCASE("a rule that the consequence derives")
    {
        collector.clear();
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
        const std::string rule = last_out_text(collector);
        interactive.process("_:b0 is transitive");

        collector.clear();
        interactive.process(".run");
        CHECK_FALSE(has_contradiction(collector));
        CHECK_FALSE(any_event_contains(collector, "refused"));
        CHECK(any_event_contains(collector, "Total contradictions found: 0."));
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note_start + "1 pair of rule and blank node: " + rule + " with _:@068feb5243a0018d."});
        CHECK(graph->get_rules().size() == 1);
        CHECK(graph->count_contradiction_records() == 0);

        // A relation capable of serving as a predicate still gets its rule.
        interactive.process("knows is transitive");
        interactive.process(".run");
        CHECK(graph->get_rules().size() == 2);
    }
}

// Whatever was built before the firing encountered the blank node, nothing
// remains of a consequence left unwritten: the line that makes the rule
// fire adds exactly the nodes it adds under the rule without that
// consequence, and none more. A record, a mark, or a part of a nested
// statement left behind would each constitute a node.
TEST_CASE("blank nodes: a consequence left unwritten adds no node to the graph")
{
    SUBCASE("with the blank node as its own predicate")
    {
        const std::size_t with    = nodes_added("(X p Y) => (a Y b) (X seen Y)\n", "x p _:b0");
        const std::size_t without = nodes_added("(X p Y) => (X seen Y)\n", "x p _:b0");
        CHECK(with == without);
    }

    SUBCASE("with the blank node as the predicate of a nested statement")
    {
        // The firing constructs `x r x` before encountering the statement
        // that is refused, so a consequence dropped only in part would
        // still leave that node.
        const std::size_t with    = nodes_added("(X p Y) => (k says ((X r X) about (a Y b))) (X seen Y)\n", "x p _:b0");
        const std::size_t without = nodes_added("(X p Y) => (X seen Y)\n", "x p _:b0");
        CHECK(with == without);
    }

    SUBCASE("with the blank node as a predicate in a derived rule")
    {
        // Under the generator, the line adds the two nodes it
        // introduces without the generator: the blank node and its
        // associated statement.
        const std::size_t with    = nodes_added("(R is transitive) => ((X R Y, Y R Z) => (X R Z))\nknows is transitive\n", "_:b0 is transitive");
        const std::size_t without = nodes_added("knows is transitive\n", "_:b0 is transitive");
        CHECK(with == without);
        CHECK(without == 2);
    }
}

// A fresh variable of a rule gets its witness at the location where a
// written consequence holds it. A consequence that is never written must
// not be requested when a firing seeks the witness from an earlier run, or
// else no witness is ever found: each pass made a new one beside the
// consequence that is written, and the statement holding it once more. The
// passes are single ones under the classic evaluation, which would not end
// otherwise.
TEST_CASE("blank nodes: a consequence left unwritten makes no witness that the next run would make again")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto stable = [&](const std::string& rule)
    {
        const std::vector<std::size_t> counts = counts_over_passes(collector, interactive, ".identity http://example.org/doc\n" + rule + "\nx p _:b0\n");

        // Required, and not merely checked: the lines behind a call run
        // to the fixpoint, which would not conclude if each pass makes a
        // new witness.
        REQUIRE(counts[1] == counts[0]);
        REQUIRE(counts[2] == counts[0]);
        REQUIRE(counts[3] == counts[0]);
    };

    SUBCASE("beside a consequence that is written and holds the witness")
    {
        stable("(X p Y) => (a Y Z) (Z r X)");
        collector.clear();
        interactive.process("Z r x");
        CHECK(collect_answers(collector).size() == 1);
    }

    SUBCASE("where the unwritten statement is nested")
    {
        stable("(X p Y) => (k says (a Y Z)) (Z r X)");
        collector.clear();
        interactive.process("Z r x");
        CHECK(collect_answers(collector).size() == 1);
    }

    SUBCASE("where the unwritten statement is the subject of its consequence")
    {
        // The names that serve as the predicate and the object of this
        // consequence are untouched by any binding, allowing the search
        // to bypass its prediction step for a consequence lacking a
        // blank node.
        stable("(X p Y) => ((a Y Z) noted yes) (Z r X)");
        collector.clear();
        interactive.process("Z r x");
        CHECK(collect_answers(collector).size() == 1);
    }

    SUBCASE("where the witness itself would be the subject of the unwritten statement")
    {
        stable("(X p Y) => (Z Y X) (X seen Y)");
        CHECK(answer_of(collector, interactive, "x seen Y") == "x seen _:@068feb5243a0018d");
    }

    SUBCASE("a rule whose one consequence is left unwritten still says so")
    {
        // The search for a witness finds every consequence of this
        // firing accounted for and concludes it before the consequence
        // is reached, so the pair is noted by the search as well.
        process_lines(interactive, R"(
.identity http://example.org/doc
(X p Y) => (Z Y X)
)");
        collector.clear();
        interactive.process("x p _:b0");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note_start + "1 pair of rule and blank node: (X p Y) => (Z Y X) with _:@068feb5243a0018d."});

        const auto after_first = interactive.graph()->count();
        interactive.process(".run");
        CHECK(interactive.graph()->count() == after_first);
    }

    SUBCASE("to the fixpoint, under each evaluation")
    {
        stable("(X p Y) => (a Y Z) (Z r X)");
        const auto after_passes = interactive.graph()->count();
        for (const char* evaluation : {".semi-naive off", ".semi-naive on", ".semi-naive check"})
        {
            CAPTURE(evaluation);
            interactive.process(evaluation);
            interactive.process(".run");
            CHECK(interactive.graph()->count() == after_passes);
        }
        collector.clear();
        interactive.process("Z r x");
        CHECK(collect_answers(collector).size() == 1);
    }
}

// A consequence left unwritten is considered to exist when a firing seeks
// the witness of a prior one, and when it is the only one holding the fresh
// variable, that search ends the firing at its first application. The rule
// that the same firing derives and a `!` among its consequences are owed all
// the same, since the search examines neither. Ended outright, the firing
// wrote no rule and raised nothing, whereas it had done both as long as such
// a consequence constituted a contradiction.
TEST_CASE("blank nodes: a firing that ends at the search for a witness still derives its rule and raises its contradiction")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();

    SUBCASE("a rule beside the unwritten consequence")
    {
        // Single passes first: the rule is written by the first and what
        // it derives by the second at the latest, and nothing follows.
        const std::vector<std::size_t> counts = counts_over_passes(collector, interactive, ".identity http://example.org/doc\nknows p _:b0\n(X p Y) => ((A X B) => (A r B)) (W Y X)\nu knows v\n");
        REQUIRE(counts[2] == counts[1]);
        REQUIRE(counts[3] == counts[1]);

        collector.clear();
        interactive.process(".run");
        CHECK_FALSE(has_contradiction(collector));
        CHECK(graph->get_rules().size() == 2);
        CHECK(answer_of(collector, interactive, "u r O") == "u r v");

        // No witness was made for it: the statement that would hold one
        // is the unwritten one.
        collector.clear();
        interactive.process("S O knows");
        CHECK(collect_answers(collector).empty());
    }

    SUBCASE("a contradiction beside the unwritten consequence")
    {
        // The first pass is the one that reports it: a contradiction
        // that the network has recorded is not reported again.
        process_lines(interactive, ".identity http://example.org/doc\n.auto-run\nx p _:b0\n(X p Y) => ! (W Y X)\n");
        collector.clear();
        interactive.process(".run-once");
        CHECK(has_contradiction(collector));
        CHECK(any_event_contains(collector, "! ⇐ (x p _:@068feb5243a0018d)"));
        CHECK(unwritten_notes(collector).size() == 1);
        CHECK(graph->count_contradiction_records() == 1);

        // Only that record stays: no witness is created, neither in this
        // pass nor in any subsequent one.
        const auto nodes = graph->count();
        interactive.process(".run-once");
        interactive.process(".run");
        CHECK(graph->count() == nodes);
        CHECK(graph->count_contradiction_records() == 1);
    }
}

// A rule that a firing derives is examined by no search for a witness, so a
// firing that such a search ended met neither the rule nor the blank node
// within it: the pair came with the run that made the witness and with none
// following it, neither among the pairs of a quiet run nor in the line that
// a load allows again.
TEST_CASE("blank nodes: a derived rule left unwritten beside a witness is met by every run")
{
    const auto file = fs::temp_directory_path() / "zelph_blank_nodes_unwritten_rule.bin";

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    interactive.process(".identity http://example.org/doc");
    collector.clear();
    interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z)) (W marks R)");
    const std::string note = note_start + "1 pair of rule and blank node: " + last_out_text(collector) + " with _:@068feb5243a0018d.";

    // This run makes the witness, and its firing meets the rule.
    collector.clear();
    interactive.process("_:b0 is transitive");
    REQUIRE(unwritten_notes(collector) == std::vector<std::string>{note});

    SUBCASE("a quiet run returns the pair in each run")
    {
        const auto nodes = graph->count();
        for (int run = 0; run < 3; ++run)
        {
            CAPTURE(run);
            interactive.process(".run quiet");
            const std::vector<std::pair<Node, Node>> pairs = graph->unwritten();
            CHECK(pairs.size() == 1);
            CHECK((pairs.empty() ? Node{0} : pairs.front().second) == Zelph::blank_id(doc, "b0"));
        }
        CHECK(graph->count() == nodes);
    }

    SUBCASE("after a load the line comes again, and no second witness with it")
    {
        interactive.process(".save " + file.string());
        interactive.process(".load " + file.string());
        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note});
        collector.clear();
        interactive.process("W marks R");
        CHECK(collect_answers(collector).size() == 1);
        fs::remove(file);
    }
}

// A consequence can nest two statements whose predicates the binding
// makes two different blank nodes. The firing meets the refusal of the
// first one it builds and stops there, and the search for an earlier
// witness, which predicts the instance without building it, has to name
// that same one: it named the last it walked, so the rule below named two
// pairs in its first run, a later run a single one, and that one was not
// the pair of the same rule without its fresh variable.
TEST_CASE("blank nodes: a consequence with two blank predicates names the pair of the first on every route")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
.auto-run
_:b1 p _:b2
)");
    const std::string first_met = zelph::string::blank_reference(Zelph::blank_id(doc, "b2")); // bound to Y, within `(a Y b)`

    // The blank nodes associated with the pairs produced by a quiet run.
    const auto pairs_of_a_quiet_run = [&]
    {
        interactive.process(".run quiet");
        std::vector<std::string> blanks;
        for (const auto& pair : graph->unwritten())
            blanks.push_back(zelph::string::blank_reference(pair.second));
        return blanks;
    };

    SUBCASE("by the firing alone")
    {
        collector.clear();
        interactive.process("(X p Y) => (k says ((a Y b) about (c X d)))");
        const std::string rule = last_out_text(collector);
        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note_start + "1 pair of rule and blank node: " + rule + " with " + first_met + "."});
        CHECK(pairs_of_a_quiet_run() == std::vector<std::string>{first_met});
    }

    SUBCASE("by the firing and by the search for its witness")
    {
        collector.clear();
        interactive.process("(X p Y) => (W says ((a Y b) about (c X d))) (W r X)");
        const std::string rule = last_out_text(collector);

        // The first run looks for a witness, finds none, and fires.
        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note_start + "1 pair of rule and blank node: " + rule + " with " + first_met + "."});

        // A later run finds the witness and ends at the search.
        CHECK(pairs_of_a_quiet_run() == std::vector<std::string>{first_met});
        CHECK(pairs_of_a_quiet_run() == std::vector<std::string>{first_met});
    }

    SUBCASE("where the statement that holds the other one would be refused as well")
    {
        // `(a Y (c X d))` is built after its object `(c X d)`, so the
        // predicate of the object is met first, on both routes.
        const std::string inner = zelph::string::blank_reference(Zelph::blank_id(doc, "b1")); // bound to X
        interactive.process("(X p Y) => (W says (a Y (c X d))) (W r X)");
        collector.clear();
        interactive.process(".run");
        const std::vector<std::string> notes = unwritten_notes(collector);
        REQUIRE(notes.size() == 1);
        CHECK(notes.front().find("1 pair of rule and blank node") != std::string::npos);
        CHECK(notes.front().find(" with " + inner + ".") != std::string::npos);
        CHECK(pairs_of_a_quiet_run() == std::vector<std::string>{inner});
    }

    SUBCASE("the predicate of the consequence itself comes before one nested in it")
    {
        // Before building anything of it, the firing tests the
        // consequence's own predicate.
        interactive.process("(X p Y) => ((c X d) Y W) (W r X)");
        collector.clear();
        interactive.process(".run");
        const std::vector<std::string> notes = unwritten_notes(collector);
        REQUIRE(notes.size() == 1);
        CHECK(notes.front().find(" with " + first_met + ".") != std::string::npos);
        CHECK(pairs_of_a_quiet_run() == std::vector<std::string>{first_met});
    }
}

// What a run reports consists of each pair of a rule and a blank node,
// exactly once, rather than each firing: a single application is met by
// both the delta's firing and a classic pass equally, with nothing in the
// graph indicating that the first occurred, and one rule reaches the same
// blank node via every fact that binds it. The line thus presents
// identical output regardless of how the run evaluates or across how many
// threads, which would not hold true for a number of firings. The facts
// amount to as many as required for a scan to span the pool.
TEST_CASE("blank nodes: the note on unwritten statements is the same on one thread and several and in every evaluation mode")
{
    // The rules are positioned according to the order of their text as a
    // line shows it: "p" comes before "pq", a reversal that would occur if
    // the marks around a name in the rendering were applied.
    const std::string expected = note_start
                               + "3 pairs of rule and blank node: (X p Y) => (a Y b) with _:@05a67d20418a1f7d, _:@068feb5243a0018d; "
                                 "(X pq Y) => (c Y d) with _:@068feb5243a0018d.";

    const auto note_under = [&](const char* threads, const char* evaluation, const bool on_pool)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
.identity http://example.org/doc
.auto-run
(X p Y) => (a Y b)
(X pq Y) => (c Y d)
t p _:b1
u pq _:b0
)");
        interactive.process(threads);
        interactive.process(evaluation);
        interactive.process(R"(%(for i 0 )" + std::to_string(zelph::concurrency::thread_count::parallel_scan_min_facts)
                            + R"( (zelph/fact (string "s" i) "p" (zelph/blank "http://example.org/doc" "b0"))))");

        collector.clear();
        interactive.run(true, false, false);
        CHECK(any_event_contains(collector, "Parallel unifications activated for 0 distinct fixed relations.") != on_pool);
        CHECK_FALSE(has_contradiction(collector));

        const std::vector<std::string> notes = unwritten_notes(collector);
        REQUIRE(notes.size() == 1);
        return notes.front();
    };

    CHECK(note_under(".parallel", ".semi-naive on", false) == expected);
    CHECK(note_under(".parallel", ".semi-naive off", false) == expected);
    CHECK(note_under(".parallel", ".semi-naive check", false) == expected);
    CHECK(note_under(".threads 4", ".semi-naive on", true) == expected);
    CHECK(note_under(".threads 4", ".semi-naive off", true) == expected);
    CHECK(note_under(".threads 4", ".semi-naive check", true) == expected);
}

// The line lists the pairs in an order that remains unchanged by any run,
// by the rule's text and then by the blank node's id, and terminates
// after ten entries, indicating the count of the remaining ones: a rule
// that meets a blank node in every statement of a large document would
// otherwise fill the screen.
TEST_CASE("blank nodes: the note lists ten pairs in a fixed order and counts the others")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.identity http://example.org/doc
.auto-run
(X p Y) => (a Y b)
)");

    std::vector<Node> blanks;
    for (int i = 0; i < 12; ++i)
    {
        interactive.process("x p _:m" + std::to_string(i));
        blanks.push_back(Zelph::blank_id(doc, "m" + std::to_string(i)));
    }
    std::sort(blanks.begin(), blanks.end());

    // The rule stands once, and its blank nodes follow it.
    std::string expected = note_start + "12 pairs of rule and blank node: (X p Y) => (a Y b) with ";
    for (std::size_t i = 0; i < 10; ++i)
        expected += std::string(i == 0 ? "" : ", ") + zelph::string::blank_reference(blanks[i]);
    expected += "; and 2 more.";

    collector.clear();
    interactive.process(".run");
    CHECK(unwritten_notes(collector) == std::vector<std::string>{expected});

    SUBCASE("the ids are the ones of the labels, lowest first")
    {
        CHECK(blanks.front() == 0x047a86c7e5427f93ull); // _:m1
        CHECK(blanks[9] == 0x0737b7828432fdc6ull);      // _:m3
    }

    SUBCASE("the pairs behind the tenth are named by that line as well")
    {
        // They are counted in it, so the next run has nothing new to
        // say.
        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector).empty());
    }
}

// A rule stands in the line once, followed by its blank nodes, and the ten
// entries the line outputs are pairs rather than rules: a second rule gets
// what the first rule left from the ten. With its text repeated for each
// pair, one rule that met ten blank nodes stood in the line ten times.
TEST_CASE("blank nodes: the note writes a rule once and counts its ten in pairs across rules")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.identity http://example.org/doc
.auto-run
(X p Y) => (a Y b)
(X q Y) => (c Y d)
)");

    const auto written = [&](const char* predicate, const char* label, const int number)
    {
        std::vector<Node> blanks;
        for (int i = 0; i < number; ++i)
        {
            interactive.process(std::string("x ") + predicate + " _:" + label + std::to_string(i));
            blanks.push_back(Zelph::blank_id(doc, label + std::to_string(i)));
        }
        std::sort(blanks.begin(), blanks.end());
        return blanks;
    };
    const std::vector<Node> first  = written("p", "m", 8);
    const std::vector<Node> second = written("q", "n", 4);

    std::string expected = note_start + "12 pairs of rule and blank node: (X p Y) => (a Y b) with ";
    for (std::size_t i = 0; i < first.size(); ++i)
        expected += std::string(i == 0 ? "" : ", ") + zelph::string::blank_reference(first[i]);
    expected += "; (X q Y) => (c Y d) with " + zelph::string::blank_reference(second[0]) + ", " + zelph::string::blank_reference(second[1]) + "; and 2 more.";

    collector.clear();
    interactive.process(".run");
    CHECK(unwritten_notes(collector) == std::vector<std::string>{expected});
}

// Each traversal of the graph meets the same pairs once more, and auto-run
// triggers one following each input line. A line that returns every time
// would say nothing after its initial occurrence, so a run names the pairs
// that no prior run in the session has named, and none when it met no new
// pair.
TEST_CASE("blank nodes: a session names each unwritten pair once")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.identity http://example.org/doc
(X p Y) => (a Y b)
)");
    const std::string first  = note_start + "1 pair of rule and blank node: (X p Y) => (a Y b) with _:@068feb5243a0018d.";
    const std::string second = note_start + "1 pair of rule and blank node: (X p Y) => (a Y b) with _:@05a67d20418a1f7d.";

    collector.clear();
    interactive.process("x p _:b0");
    CHECK(unwritten_notes(collector) == std::vector<std::string>{first});

    SUBCASE("an input line that brings no new pair gets no line, under auto-run")
    {
        collector.clear();
        interactive.process("y p _:b0");
        interactive.process("z knows w");
        CHECK(unwritten_notes(collector).empty());
    }

    SUBCASE("nor does a run by hand")
    {
        collector.clear();
        interactive.process(".run");
        interactive.process(".run");
        CHECK(unwritten_notes(collector).empty());
    }

    SUBCASE("a new pair is named, and only that one")
    {
        collector.clear();
        interactive.process("x p _:b1");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{second});

        SUBCASE("another rule with a blank node that was named makes a new pair")
        {
            collector.clear();
            interactive.process("(X p Y) => (c Y d)");
            const std::vector<std::string> notes = unwritten_notes(collector);
            REQUIRE(notes.size() == 1);
            CHECK(notes.front() == note_start + "2 pairs of rule and blank node: (X p Y) => (c Y d) with _:@05a67d20418a1f7d, _:@068feb5243a0018d.");
        }
    }

    SUBCASE(".new starts over")
    {
        interactive.process(".new");
        interactive.process("(X p Y) => (a Y b)");
        collector.clear();
        interactive.process("x p _:b0");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{first});
    }
}

// What a session has named is preserved alongside the graph and not stored
// in any file, so a load that supersedes the graph lets the line reappear,
// and so does an engine that loads a network saved after the line was
// written: the pairs are located within it during the next run as they
// were located previously.
TEST_CASE("blank nodes: a load forgets which unwritten pairs were named, and the file says nothing of them")
{
    const auto        file = fs::temp_directory_path() / "zelph_blank_nodes_unwritten.bin";
    const std::string note = note_start + "1 pair of rule and blank node: (X p Y) => (a Y b) with _:@068feb5243a0018d.";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
.identity http://example.org/doc
(X p Y) => (a Y b)
)");
        collector.clear();
        interactive.process("x p _:b0");
        REQUIRE(unwritten_notes(collector) == std::vector<std::string>{note});

        interactive.process(".save " + file.string());
        interactive.process(".load " + file.string());

        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note});
        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector).empty());
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + file.string());
    collector.clear();
    interactive.process(".run");
    CHECK(unwritten_notes(collector) == std::vector<std::string>{note});

    fs::remove(file);
}

// A pair is considered named once its line has been written. A quiet
// run writes none, and during the first pass of a `?` line, every line
// that is not a finding is discarded: had that pair been counted as
// named there, it would have been reported to nobody.
TEST_CASE("blank nodes: an unwritten pair is not lost when a run that prints nothing meets it first")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.identity http://example.org/doc
.auto-run
(X p Y) => (a Y b) (X seen Y)
x p _:b0
)");
    const std::string note = note_start + "1 pair of rule and blank node: (X p Y) => (X seen Y) (a Y b) with _:@068feb5243a0018d.";

    SUBCASE("a quiet run")
    {
        collector.clear();
        interactive.process(".run quiet");
        CHECK(collector.events().empty());

        interactive.process(".run");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note});
    }

    SUBCASE("a quiet run from Janet")
    {
        collector.clear();
        interactive.process("%(zelph/run :quiet)");
        CHECK(unwritten_notes(collector).empty());

        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note});
    }

    SUBCASE("the first pass of a ? line writes it, as the finding it is")
    {
        collector.clear();
        interactive.process("? x seen _:b0");
        const std::vector<std::string> notes = unwritten_notes(collector);
        REQUIRE(notes.size() == 1);
        CHECK(notes.front() == note);

        const auto written = std::find_if(collector.events().begin(), collector.events().end(), [&](const auto& event)
                                          { return event.text == note; });
        REQUIRE(written != collector.events().end());
        CHECK(written->finding);

        // Written there, it is named and does not come back.
        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector).empty());
    }
}

// The line serves as a warning and does not constitute a deduction, so no
// setting of `.deductions` prevents it from appearing, not even `off`,
// under which a run prints no derivation whatsoever.
TEST_CASE("blank nodes: the note on unwritten statements is written in every deduction mode")
{
    for (const char* mode : {"all", "focus", "quiet", "off"})
    {
        CAPTURE(mode);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(std::string(".deductions ") + mode);
        process_lines(interactive, R"(
.identity http://example.org/doc
(X p Y) => (a Y b)
)");
        collector.clear();
        interactive.process("x p _:b0");
        CHECK(unwritten_notes(collector).size() == 1);
    }
}

// A program that runs the engine quietly receives no line, so the run
// provides it with the pairs: everything this run met, regardless of
// whether a line has named them or not, in the order of the line.
TEST_CASE("blank nodes: a quiet run returns the pairs it left unwritten")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
.auto-run
(X p Y) => (a Y b)
x p _:b0
y p _:b0
x p _:b1
)");

    CHECK(janet_value(collector, interactive, "(length ((zelph/run :quiet) :unwritten))") == "2");
    CHECK(janet_value(collector, interactive, "(= (get-in (zelph/run :quiet) [:unwritten 0 :predicate]) (zelph/blank \"http://example.org/doc\" \"b1\"))") == "true");
    CHECK(janet_value(collector, interactive, "(= (get-in (zelph/run :quiet) [:unwritten 1 :predicate]) (zelph/blank \"http://example.org/doc\" \"b0\"))") == "true");
    CHECK(janet_value(collector, interactive, "(let [r (zelph/run :quiet)] (= (get-in r [:unwritten 0 :rule]) (get-in r [:unwritten 1 :rule])))") == "true");
    CHECK(janet_value(collector, interactive, "(sort (keys (zelph/run :quiet)))") == "@[:contradictions :naf-final :unwritten]");

    SUBCASE("the engine hands out the same pairs, a rule in force with each blank node")
    {
        interactive.process(".run quiet");
        const std::vector<std::pair<Node, Node>> pairs = graph->unwritten();
        REQUIRE(pairs.size() == 2);
        CHECK(graph->get_rules().count(pairs[0].first) == 1);
        CHECK(pairs[1].first == pairs[0].first);
        CHECK(pairs[0].second == Zelph::blank_id(doc, "b1"));
        CHECK(pairs[1].second == Zelph::blank_id(doc, "b0"));
    }

    SUBCASE("pairs that a line has named are returned all the same")
    {
        interactive.process(".run");
        CHECK(janet_value(collector, interactive, "(length ((zelph/run :quiet) :unwritten))") == "2");
    }

    SUBCASE("a run that leaves nothing unwritten returns none")
    {
        interactive.process(".new");
        interactive.process("x p y");
        CHECK(janet_value(collector, interactive, "(length ((zelph/run :quiet) :unwritten))") == "0");
    }

    SUBCASE("the pairs are those of this run, and a pair an earlier run met is not among them")
    {
        // Retained across runs, a pair whose statement has vanished
        // would be returned by every later run, and named by the first
        // one that prints.
        interactive.process(".run quiet");
        interactive.process(".prune-facts (X p _:b1)");
        CHECK(janet_value(collector, interactive, "(length ((zelph/run :quiet) :unwritten))") == "1");

        collector.clear();
        interactive.process(".run");
        CHECK(unwritten_notes(collector) == std::vector<std::string>{note_start + "1 pair of rule and blank node: (X p Y) => (a Y b) with _:@068feb5243a0018d."});
    }

    SUBCASE("several rules stand in the order of their text, as in the line")
    {
        interactive.process("(X pq Y) => (c Y d)");
        interactive.process("u pq _:b0");
        interactive.process(".run quiet");
        const std::vector<std::pair<Node, Node>> pairs = graph->unwritten();
        REQUIRE(pairs.size() == 3);
        CHECK(graph->format(pairs[0].first) == "(X p Y) => (a Y b)");
        CHECK(graph->format(pairs[1].first) == "(X p Y) => (a Y b)");
        CHECK(graph->format(pairs[2].first) == "(X pq Y) => (c Y d)");

        // The ids of these two rules stand in the opposite order to
        // their texts, so pairs ordered by identifiers would not pass
        // the lines above. A change to the way a rule is hashed could
        // invert the identifiers, and this line then says that the case
        // needs two other rules.
        REQUIRE(pairs[0].first > pairs[2].first);
    }
}

// A name conflict merges two nodes. A blank node merged into another node
// becomes that node, and another node merged into a blank node acquires an
// identifier that says "blank node of this document": neither outcome
// reflects the meaning of a name.
TEST_CASE("blank nodes: a blank node merges with no other node")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
_:b0 knows bob
ada knows carol
)");
    const Node blank = Zelph::blank_id(doc, "b0");
    const Node ada   = graph->get_node("ada", "zelph");
    REQUIRE(ada != 0);

    SUBCASE("the name of another node is not given to a blank node")
    {
        CHECK_THROWS_WITH(interactive.process(".name _:b0 ada"),
                          doctest::Contains("Merging the two nodes is impossible because node " + std::to_string(blank) + " is a blank node, and a blank node merges with no other."));
    }

    SUBCASE("the name of a blank node is not given to another node")
    {
        interactive.process(".name _:b0 Ada");
        CHECK(graph->get_node("Ada", "zelph") == blank);
        CHECK_THROWS_WITH(interactive.process(".name ada Ada"), doctest::Contains("is a blank node, and a blank node merges with no other."));
    }

    SUBCASE("through the library, with the names of two languages")
    {
        // The second overload of set_name repeats the refusals of the first
        // before modifying a name.
        graph->set_name(blank, "Ada", "en", false);
        CHECK_THROWS_WITH_AS(graph->set_name("ada", "Ada", "en"), doctest::Contains("is a blank node, and a blank node merges with no other."), std::runtime_error);
    }

    // Both nodes remain as they were, with the statements they
    // had.
    CHECK(graph->exists(blank));
    CHECK(graph->exists(ada));
    CHECK(answer_of(collector, interactive, "X knows bob") == "_:@068feb5243a0018d knows bob");
    CHECK(answer_of(collector, interactive, "X knows carol") == "ada knows carol");
}

// A blank node may carry a name, which is what a label of it becomes. Its
// label is not stored and would not indicate the document it originates
// from, so it is printed by its id whatever it carries, and never as an
// unnamed node is.
TEST_CASE("blank nodes: a blank node prints as its id, with or without a name")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
_:b0 knows bob
)");
    const Node blank = Zelph::blank_id(doc, "b0");
    CHECK(graph->format(blank) == "_:@068feb5243a0018d");

    SUBCASE("a name does not change the printed form, and the name resolves to the node")
    {
        interactive.process(".name _:b0 Ada");
        CHECK(graph->get_node("Ada", "zelph") == blank);
        CHECK(graph->format(blank) == "_:@068feb5243a0018d");
        CHECK(answer_of(collector, interactive, "Ada knows X") == "_:@068feb5243a0018d knows bob");
    }

    SUBCASE("as the subject of a `~` statement it does not print as its class")
    {
        // An unnamed subject of such a statement is shown as the class it
        // belongs to as an instance, which for a blank node would name
        // another node.
        interactive.process("_:b0 ~ person");
        CHECK(answer_of(collector, interactive, "X ~ person") == "_:@068feb5243a0018d ~ person");
    }

    SUBCASE("a blank node that stands in no statement")
    {
        CHECK(graph->format(graph->blank(doc, "lonely")) == zelph::string::blank_reference(Zelph::blank_id(doc, "lonely")));
    }

    SUBCASE("the digits are sixteen and lowercase")
    {
        CHECK(zelph::string::blank_reference(0x04000000000000ABull) == "_:@04000000000000ab");
    }
}

// The node counter must not number an id from the blank class, as doing so
// would result in a named node being printed and refused as a blank node. A
// counter cannot be driven there by creating nodes, so the test takes a
// network that an engine saved and sets the counter in its header to the
// limit, 2^58: this is what an engine leaves in a file once a create was
// refused there, since the counter is raised before it is checked. Nothing
// else in the file is changed.
TEST_CASE("blank nodes: the node counter is refused at 2^58")
{
    const auto file = fs::temp_directory_path() / "zelph_blank_nodes_counter.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("x knows bob");
        interactive.process(".save " + file.string());
    }
    rewrite_header(file, [](zelph::network::ZelphImpl::Builder header)
                   { header.setLast(std::uint64_t{1} << 58); });

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + file.string());
    CHECK(answer_of(collector, interactive, "X knows bob") == "x knows bob");

    CHECK_THROWS_WITH(interactive.process("carol knows bob"), doctest::Contains("Network::create: the node counter reached the ids reserved for blank nodes"));

    SUBCASE("a blank node needs no counter and is still created")
    {
        interactive.process(".identity http://example.org/doc");
        interactive.process("_:b0 knows bob");
        collector.clear();
        interactive.process("X knows bob");
        CHECK(answers_contain(collector, "_:@068feb5243a0018d knows bob"));
    }

    fs::remove(file);
}

// The limit represents the first id of the blank class, meaning the last id
// the counter gives is the one immediately preceding it, and three
// functions advance the counter, each with its own test: a node that takes
// the counter value as its id, and a collection of a rule's text and a set
// pattern, which take it as their payload. A counter beginning at the
// limit, as in the case above, is refused even by a test that lets the
// limit itself pass, and by one function standing in for all three. The
// file here leaves the counter two positions below the limit, which is the
// location an engine that has numbered that many nodes would leave it. It
// is written without its chunk directory, as the header is packed and its
// length varies with the counter.
TEST_CASE("blank nodes: each function that advances the node counter gives 2^58 - 1 and is refused at 2^58")
{
    const std::uint64_t limit = std::uint64_t{1} << 58;
    const auto          saved = fs::temp_directory_path() / "zelph_blank_nodes_boundary_saved.bin";
    const auto          file  = fs::temp_directory_path() / "zelph_blank_nodes_boundary.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("x knows bob");
        interactive.process(".save " + saved.string());
    }
    write_without_directory(saved, file, false, [](zelph::network::ZelphImpl::Builder header)
                            { header.setLast((std::uint64_t{1} << 58) - 2); });

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + file.string());
    auto* const graph = interactive.graph();
    const Node  bob   = graph->get_node("bob", "zelph");
    REQUIRE(bob != 0);

    SUBCASE("a node that takes its id from the counter")
    {
        CHECK(graph->node("carol") == limit - 1);
        CHECK_FALSE(Zelph::is_blank(limit - 1));
        CHECK_THROWS_WITH_AS(graph->node("dave"), doctest::Contains("Network::create: the node counter reached the ids reserved for blank nodes"), std::logic_error);
    }

    SUBCASE("a collection of a rule's text")
    {
        const Zelph::TemplateScope writing(*graph);
        const Node                 first = graph->collection({bob});
        CHECK(Zelph::node_kind(first) == Zelph::NodeKind::rule_template);
        CHECK(first == (0x3000000000000000ull | (limit - 1)));
        CHECK_THROWS_WITH_AS(graph->collection({bob}), doctest::Contains("Network::create_written_template: the node counter outgrew the payload of a written template"), std::logic_error);
    }

    SUBCASE("a set pattern")
    {
        const Node first = graph->set({graph->var()});
        CHECK(Zelph::node_kind(first) == Zelph::NodeKind::set_pattern);
        CHECK(first == (0x1000000000000000ull | (limit - 1)));
        CHECK_THROWS_WITH_AS(graph->set({graph->var()}), doctest::Contains("Network::create_value_set_pattern: the node counter outgrew the payload of a set pattern"), std::logic_error);
    }

    fs::remove(saved);
    fs::remove(file);
}

// A .bin stores a blank node just as it stores any atom, by means of its
// identifier within the facts that hold it, and needs no section for it.
// Following a load, the identifier continues to indicate that the node is a
// blank node, and the same label under the same identity refers to the
// identical node once more.
TEST_CASE("blank nodes: a blank node survives save and load through its facts")
{
    const auto file = fs::temp_directory_path() / "zelph_blank_nodes_saved.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
.identity http://example.org/doc
_:b0 knows bob
)");
        interactive.process(".save " + file.string());
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + file.string());
    CHECK(interactive.graph()->exists(Zelph::blank_id(doc, "b0")));
    CHECK(answer_of(collector, interactive, "X knows bob") == "_:@068feb5243a0018d knows bob");

    SUBCASE("the label names the loaded node once the session has the identity of the document")
    {
        interactive.process(".identity http://example.org/doc");
        interactive.process("_:b0 likes carol");
        CHECK(answer_of(collector, interactive, "X likes carol") == "_:@068feb5243a0018d likes carol");
    }

    SUBCASE("under the identity of this session the label is another node")
    {
        // The session that performs the loading possesses an independent random
        // identity, which is what keeps its blank nodes distinct from those of
        // the session that saved them.
        interactive.process("_:b0 likes carol");
        CHECK(answer_of(collector, interactive, "X likes carol") != "_:@068feb5243a0018d likes carol");
    }

    fs::remove(file);
}

// --- the two forms in a script -----------------------------------------------

// A token starting with an underscore was a variable without exception,
// meaning a line containing `_:b0` constituted a query that asserted
// nothing. An underscore immediately followed by a colon now initiates a
// blank node, and every other token beginning with an underscore remains
// the variable it previously was.
TEST_CASE("blank nodes: _:label is a blank node and no variable")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.identity http://example.org/doc
alice knows bob
)");

    collector.clear();
    interactive.process("_:b0 knows bob");
    CHECK(collect_answers(collector).empty());
    CHECK(last_out_text(collector) == "_:@068feb5243a0018d knows bob");

    collector.clear();
    interactive.process("X knows bob");
    CHECK(answers_contain(collector, "alice knows bob"));
    CHECK(answers_contain(collector, "_:@068feb5243a0018d knows bob"));

    SUBCASE("a token with a leading underscore and no colon behind it stays a variable")
    {
        collector.clear();
        interactive.process("_b0 knows bob");
        CHECK(collect_answers(collector).size() == 2);

        collector.clear();
        interactive.process("_ knows bob");
        CHECK(collect_answers(collector).size() == 2);
    }

    SUBCASE("the same label is the same node, and another label another node")
    {
        interactive.process("_:b0 knows carol");
        interactive.process("_:b1 knows carol");
        collector.clear();
        interactive.process("X knows carol");
        CHECK(answers_contain(collector, "_:@068feb5243a0018d knows carol"));
        CHECK(answers_contain(collector, "_:@05a67d20418a1f7d knows carol"));
    }

    SUBCASE("a statement of blank nodes alone asks nothing")
    {
        collector.clear();
        interactive.process("_:b0 knows _:b1");
        CHECK(collect_answers(collector).empty());
        CHECK(last_out_text(collector) == "_:@068feb5243a0018d knows _:@05a67d20418a1f7d");
    }
}

// The helper that the grammar's twin in C++ consults, and via it the
// self-fact sugar and the pattern of `.prune-nodes`: a blank token leaves
// this test for a variable just as it leaves the grammar's.
TEST_CASE("blank nodes: a blank token is no variable to string::is_var")
{
    CHECK(zelph::string::is_var("_b0"));
    CHECK(zelph::string::is_var("_"));
    CHECK(zelph::string::is_var("X"));
    CHECK_FALSE(zelph::string::is_var("_:b0"));
    CHECK_FALSE(zelph::string::is_var("_:"));
    CHECK_FALSE(zelph::string::is_var("_:@068feb5243a0018d"));

    CHECK(zelph::string::is_blank_token("_:b0"));
    CHECK(zelph::string::is_blank_token("_:"));
    CHECK_FALSE(zelph::string::is_blank_token("_b0"));
    CHECK_FALSE(zelph::string::is_blank_token("_"));
    CHECK_FALSE(zelph::string::is_blank_token("b0"));
}

// The label of a script is held to Turtle's production for a blank node
// label, ensuring that every spelling accepted by the script
// corresponds to a form that a Turtle document writes for the same
// node.
TEST_CASE("blank nodes: a label is one that Turtle accepts")
{
    using zelph::string::is_blank_label;

    CHECK(is_blank_label("b0"));
    CHECK(is_blank_label("0"));
    CHECK(is_blank_label("_x"));
    CHECK(is_blank_label("a-b"));
    CHECK(is_blank_label("a.b"));
    CHECK(is_blank_label("a..b"));
    CHECK(is_blank_label("a-"));
    CHECK(is_blank_label("Grüße"));
    CHECK(is_blank_label("東京"));
    CHECK(is_blank_label("a\xC2\xB7"
                         "b")); // U+00B7, which can come after the first character
    CHECK(is_blank_label("9bdf84525ed773079f49f76ed44d41be"));

    CHECK_FALSE(is_blank_label(""));
    CHECK_FALSE(is_blank_label("-a"));
    CHECK_FALSE(is_blank_label(".a"));
    CHECK_FALSE(is_blank_label("a."));
    CHECK_FALSE(is_blank_label("a:b"));
    CHECK_FALSE(is_blank_label(":a"));
    CHECK_FALSE(is_blank_label("a b"));
    CHECK_FALSE(is_blank_label("a@b"));
    CHECK_FALSE(is_blank_label("@068feb5243a0018d"));
    CHECK_FALSE(is_blank_label("\xC2\xB7"
                               "a")); // U+00B7 cannot serve as a leading character
    CHECK_FALSE(is_blank_label("a\xD7"
                               "b"));     // not UTF-8
    CHECK_FALSE(is_blank_label("a\xC3")); // a sequence that concludes prematurely
    // An overlong form: two and three bytes representing "A", which
    // UTF-8 writes as a single byte. Decoded without the test, it
    // becomes a letter that Turtle recognizes, under bytes that are a
    // different label to the hash.
    CHECK_FALSE(is_blank_label("\xC1\x81"));
    CHECK_FALSE(is_blank_label("a\xE0\x81\x81"));
    CHECK_FALSE(is_blank_label("a\xC3\x97"
                               "b")); // U+00D7, the multiplication sign, lies between two ranges
}

// A token starting with "_:" commits to representing a blank node. One
// that is malformed is an error that names the token, and gets no further
// interpretation as a variable, as it was previously, nor as a name.
TEST_CASE("blank nodes: a malformed blank token is an error that names the token")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph  = interactive.graph();
    const auto                  before = graph->count();

    CHECK_THROWS_WITH(interactive.process("x q _:"), doctest::Contains("\"_:\" is not a blank node: a label has to follow the \"_:\""));
    CHECK_THROWS_WITH(interactive.process("x q _:a:b"), doctest::Contains("\"_:a:b\" is not a blank node: the label behind \"_:\" has to be one that Turtle accepts"));
    CHECK_THROWS_WITH(interactive.process("x q _:b0."), doctest::Contains("\"_:b0.\" is not a blank node"));
    CHECK_THROWS_WITH(interactive.process("_:-a q x"), doctest::Contains("\"_:-a\" is not a blank node"));
    CHECK_THROWS_WITH(interactive.process("x q {a _:}"), doctest::Contains("\"_:\" is not a blank node"));
    CHECK_THROWS_WITH(interactive.process("(X q _:a:b) => (X r y)"), doctest::Contains("\"_:a:b\" is not a blank node"));

    // A statement composed of two parts is refused for its shape,
    // accompanied by a message that repeats its tokens, and a token
    // that is not a blank node is reported before that: the message
    // advised the self-fact ":father _:a:b".
    for (const char* line : {"(_:a:b father, x likes y) => (x is z)", "(x likes y, father _:a:b) => (x is z)", "(_:a:b, x likes y) => (x is z)", "((k says _:a:b) father, x likes y) => (x is z)"})
    {
        CAPTURE(line);
        std::string message;
        try
        {
            interactive.process(line);
        }
        catch (const std::exception& e)
        {
            message = e.what();
        }
        CHECK(message.find("\"_:a:b\" is not a blank node: the label behind \"_:\" has to be one that Turtle accepts") != std::string::npos);
        CHECK(message.find("self-fact") == std::string::npos);
        CHECK(message.find("left off") == std::string::npos);
        CHECK(message.find("only a subject") == std::string::npos);
    }

    // A short statement whose blank token is a blank node keeps its
    // message.
    CHECK_THROWS_WITH(interactive.process("(_:b0 father, x likes y) => (x is z)"), doctest::Contains("is a subject and a predicate with the object left off"));

    // The error arises before any part of the line is constructed.
    CHECK(graph->count() == before);

    SUBCASE("a label that Turtle accepts and a script could not write as a name")
    {
        interactive.process(".identity http://example.org/doc");
        collector.clear();
        interactive.process("_:a.b q _:Grüße-東京");
        CHECK(last_out_text(collector) == zelph::string::blank_reference(Zelph::blank_id(doc, "a.b")) + " q _:@066eb781380dfdee");
    }
}

// A line containing solely a blank node marks the beginning of a
// statement and remains incomplete, just as a line containing only a name
// does. If treated as a complete statement, it would evaluate to the node
// and state nothing.
TEST_CASE("blank nodes: a lone blank node waits for the rest of its statement")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".identity http://example.org/doc");

    collector.clear();
    interactive.process("_:b0");
    CHECK(interactive.is_accumulating());
    CHECK(collector.events().empty());

    interactive.process("knows bob");
    CHECK_FALSE(interactive.is_accumulating());
    CHECK(last_out_text(collector) == "_:@068feb5243a0018d knows bob");

    SUBCASE("the reference by id waits too")
    {
        collector.clear();
        interactive.process("_:@068feb5243a0018d");
        CHECK(interactive.is_accumulating());
        interactive.process("likes carol");
        CHECK(last_out_text(collector) == "_:@068feb5243a0018d likes carol");
    }
}

// Each location within a statement that takes a value takes a blank node: a
// list, a set, a collection, a nested statement, the condition and the
// consequence of a rule, a comma list.
TEST_CASE("blank nodes: a blank node stands wherever a value stands")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".identity http://example.org/doc");

    const auto echo = [&](const std::string& line)
    {
        collector.clear();
        interactive.process(line);
        return last_out_text(collector);
    };

    CHECK(echo("recipe ingredients < flour _:b0 >") == "recipe ingredients <flour _:@068feb5243a0018d>");
    CHECK(echo("x p {_:b0}") == "x p {_:@068feb5243a0018d}");
    CHECK(echo("x p @{_:b0}") == "x p @{_:@068feb5243a0018d}");
    CHECK(echo("(_:b0 likes tea) source x") == "(_:@068feb5243a0018d likes tea) source x");

    SUBCASE("under a focus, where the statement evaluates to it")
    {
        CHECK(echo("x says (bob trusts *_:b0)") == "x says _:@068feb5243a0018d");
        CHECK(answer_of(collector, interactive, "bob trusts X") == "bob trusts _:@068feb5243a0018d");
    }

    SUBCASE("a line that a focus makes evaluate to a blank node echoes nothing, as with a name")
    {
        // Focusing on a name passes it along, which is not a node, and
        // the line produces no output. A blank node possesses no text
        // that serves as its name and is transmitted as the node itself,
        // which the line then printed by its id.
        CHECK(echo("x says *foo").empty());
        CHECK(echo("x says *_:b0").empty());

        // The statement is written all the same, just as it is with a
        // name.
        collector.clear();
        interactive.process("x says Z");
        CHECK(answers_contain(collector, "x says foo"));
        CHECK(answers_contain(collector, "x says _:@068feb5243a0018d"));

        // Nor is a line containing a variable adjacent to such a focus
        // evaluated as a query.
        collector.clear();
        interactive.process("X says *_:b0");
        CHECK(collect_answers(collector).empty());
    }

    SUBCASE("in the consequence of a rule")
    {
        interactive.process("(X likes cats) => (X ~ _:b0)");
        interactive.process("tom likes cats");
        CHECK(answer_of(collector, interactive, "tom ~ X") == "tom ~ _:@068feb5243a0018d");
    }

    SUBCASE("in a condition, where it matches itself alone")
    {
        interactive.process("(_:b0 knows X) => (X ~ known)");
        interactive.process("_:b0 knows bob");
        interactive.process("_:b1 knows carol");
        CHECK(answer_of(collector, interactive, "X ~ known") == "bob ~ known");
    }

    SUBCASE("as an argument of an opaque predicate it is the node, as a name is")
    {
        // An argument consisting of a single token refers to a node, whereas
        // one that builds a term is expressed as a mention. A blank node is a
        // token.
        interactive.process("says ~ opaque");
        CHECK(echo("x says _:b0") == "x says _:@068feb5243a0018d");
        CHECK(answer_of(collector, interactive, "x says X") == "x says _:@068feb5243a0018d");
    }

    SUBCASE("a rule that a statement holds and whose consequence is a blank node asks, as with a name")
    {
        // A `=>` within a statement where the consequence cannot be
        // asserted inquires into which implications exist and makes no
        // claim. A blank node serves as such a consequence, just as a name
        // does.
        interactive.process("x notes ((X p Y) => _:b0)");
        collector.clear();
        interactive.process("x notes Z");
        CHECK(collect_answers(collector).empty());

        interactive.process("x notes ((X p Y) => foo)");
        collector.clear();
        interactive.process("x notes Z");
        CHECK(collect_answers(collector).empty());

        // Whether such a statement poses a query is determined twice:
        // once during reading, based on its syntax, and again when the
        // code executes, based on the nodes. The lines above are
        // resolved by the second alone. Under a negation, the first
        // determination governs how the line is built: read as a claim,
        // it is not a query and gives no answer, and it leaves the text
        // of its rule behind. With a name, it asks, and a blank node
        // must serve as the identical leaf in that reading.
        auto* const graph = interactive.graph();
        const auto  asked = [&](const std::string& line)
        {
            const auto before = graph->count();
            collector.clear();
            interactive.process(line);
            return std::make_pair(collect_answers(collector).size(), graph->count() - before);
        };
        const auto with_name  = asked("¬(x notes ((X p Y) => bar))");
        const auto with_blank = asked("¬(x notes ((X p Y) => _:b9))");
        CHECK(with_name.first == 1);
        CHECK(with_blank == with_name);
    }

    SUBCASE("a refused statement leaves the node of a new label, as it leaves the node of a new name")
    {
        // A statement composed of plain tokens builds no part that a
        // refusal would need to retract, thus operating outside the scratch
        // cluster that governs a statement containing a nested part. A
        // blank node is such a token: when treated as a part, every
        // statement that names one incurs the cost of that cluster.
        auto* const graph  = interactive.graph();
        const auto  before = graph->count();
        CHECK_THROWS_WITH(interactive.process("x q fresh x"), doctest::Contains("facts with same subject and object"));
        CHECK(graph->get_node("fresh", "zelph") != 0);
        const auto with_name = graph->count();
        CHECK(with_name > before);

        CHECK_THROWS_WITH(interactive.process("x q _:fresh x"), doctest::Contains("facts with same subject and object"));
        CHECK(graph->exists(Zelph::blank_id(doc, "fresh")));
        CHECK(graph->count() == with_name + 1);
    }

    SUBCASE("in a query with several conditions")
    {
        interactive.process("_:b0 knows bob");
        interactive.process("bob knows carol");
        const std::string answer = answer_of(collector, interactive, "_:b0 knows X, X knows Y");
        CHECK(answer.find("_:@068feb5243a0018d knows bob") != std::string::npos);
        CHECK(answer.find("bob knows carol") != std::string::npos);
    }
}

// A blank node is printed in a form that reads back as the same node. The
// form says the id, thus referring to that node across every document and
// creating none: the digits give neither a document nor a label to which a
// newly created node would belong.
TEST_CASE("blank nodes: _:@ and the digits of an id refer to an existing blank node")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
_:b0 knows bob
)");

    SUBCASE("a printed line reads back as the statement it shows")
    {
        const std::string printed = answer_of(collector, interactive, "X knows bob");
        const auto        before  = graph->count();
        collector.clear();
        interactive.process(printed);
        CHECK(last_out_text(collector) == printed);
        CHECK(graph->count() == before);
    }

    SUBCASE("in a document of another identity it is the same node")
    {
        interactive.process(".identity http://example.org/other");
        interactive.process("_:@068feb5243a0018d likes carol");
        CHECK(answer_of(collector, interactive, "X likes carol") == "_:@068feb5243a0018d likes carol");
        CHECK(graph->get_fact_objects(Zelph::blank_id(doc, "b0"), graph->get_node("likes", "zelph")).size() == 1);
    }

    SUBCASE("digits of either case")
    {
        interactive.process("_:@068FEB5243A0018D likes carol");
        CHECK(answer_of(collector, interactive, "X likes carol") == "_:@068feb5243a0018d likes carol");
    }

    SUBCASE("an id the graph does not hold is an error, and no node is created")
    {
        const auto before = graph->count();
        CHECK_THROWS_WITH(interactive.process("_:@05a67d20418a1f7d knows bob"),
                          doctest::Contains("The graph holds no blank node _:@05a67d20418a1f7d. This form refers to a blank node by its id and creates none."));
        CHECK(graph->count() == before);
        CHECK_FALSE(graph->exists(0x05a67d20418a1f7dull));
    }

    SUBCASE("digits that are not the id of a blank node")
    {
        CHECK_THROWS_WITH(interactive.process("x q _:@12"), doctest::Contains("\"_:@12\" does not refer to a blank node"));
        CHECK_THROWS_WITH(interactive.process("x q _:@068feb5243a0018dd"), doctest::Contains("\"_:@068feb5243a0018dd\" does not refer to a blank node"));
        CHECK_THROWS_WITH(interactive.process("x q _:@068feb5243a0018g"), doctest::Contains("does not refer to a blank node"));
        // The id of a named node, and that of a fact.
        CHECK_THROWS_WITH(interactive.process("x q _:@000000000000000b"), doctest::Contains("does not refer to a blank node"));
        CHECK_THROWS_WITH(interactive.process("x q _:@4000000000000001"), doctest::Contains("does not refer to a blank node"));
    }
}

// Within quotes, the text is a name, as all quoted text is. A node bearing that
// name must be printed in quotation marks, or its line would be read back as a
// blank node.
TEST_CASE("blank nodes: in quotes the same text is a name")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    interactive.process(".identity http://example.org/doc");

    collector.clear();
    interactive.process("\"_:b0\" knows bob");
    CHECK(last_out_text(collector) == "\"_:b0\" knows bob");

    const Node named = graph->get_node("_:b0", "zelph");
    REQUIRE(named != 0);
    CHECK_FALSE(Zelph::is_blank(named));
    CHECK_FALSE(graph->exists(Zelph::blank_id(doc, "b0")));

    // The line that has been printed refers once more to the identical node.
    const auto before = graph->count();
    interactive.process(last_out_text(collector));
    CHECK(graph->count() == before);
}

// --- the document of a line --------------------------------------------------

// The session's identity is drawn at random, ensuring that the blank nodes
// of two sessions never intersect within a network saved by one and loaded
// by the other: both would write `_:b0` for different resources.
TEST_CASE("blank nodes: the lines of a session belong to a document with a random identity")
{
    SUBCASE("within the session the label is one node, and it is a blank node")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("_:b0 knows bob");
        const std::string answer  = answer_of(collector, interactive, "X knows bob");
        const std::string subject = answer.substr(0, answer.find(' '));
        CHECK(subject.rfind("_:@", 0) == 0);

        interactive.process("_:b0 likes carol");
        CHECK(answer_of(collector, interactive, "X likes carol") == subject + " likes carol");
    }

    SUBCASE("another session gives the same label another node")
    {
        // Sequentially: a process holds one engine at a time.
        const auto in_a_session = []
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process("_:b0 knows bob");
            return answer_of(collector, interactive, "X knows bob");
        };
        CHECK(in_a_session() != in_a_session());
    }

    SUBCASE(".new keeps the identity, as its lines belong to the same input")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("_:b0 knows bob");
        const std::string before = answer_of(collector, interactive, "X knows bob");

        interactive.process(".new");
        interactive.process("_:b0 knows bob");
        CHECK(answer_of(collector, interactive, "X knows bob") == before);
    }

    SUBCASE("the identity is written as 32 lowercase hexadecimal digits, and .identity shows it")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        collector.clear();
        interactive.process(".identity");
        const std::string shown = last_out_text(collector);
        const std::string head  = "The current identity is '";
        REQUIRE(shown.rfind(head, 0) == 0);
        REQUIRE(shown.back() == '\'');
        const std::string identity = shown.substr(head.size(), shown.size() - head.size() - 1);
        CHECK(identity.size() == 32);
        CHECK(identity.find_first_not_of("0123456789abcdef") == std::string::npos);

        // It is the identity of the session that a label is hashed
        // with.
        interactive.process("_:b0 knows bob");
        CHECK(answer_of(collector, interactive, "X knows bob") == zelph::string::blank_reference(Zelph::blank_id(identity, "b0")) + " knows bob");
    }
}

// A script processed through a pipe possesses no file by which to be
// recognized, and its nodes change with each execution unless it specifies its
// own identity. `.identity` sets the session's identity to a text selected by
// the script.
TEST_CASE("blank nodes: .identity sets the identity of the session")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".identity http://example.org/doc");
    interactive.process("_:b0 knows bob");
    CHECK(answer_of(collector, interactive, "X knows bob") == "_:@068feb5243a0018d knows bob");

    SUBCASE("a later .identity is the document of the lines behind it")
    {
        interactive.process(".identity http://example.org/other");
        interactive.process("_:b0 knows carol");
        CHECK(answer_of(collector, interactive, "X knows carol") == "_:@06447ae5244b6ada knows carol");
    }

    SUBCASE("an identity with a blank is written in quotes")
    {
        interactive.process(".identity \"my notes\"");
        interactive.process("_:b0 knows carol");
        CHECK(answer_of(collector, interactive, "X knows carol") == zelph::string::blank_reference(Zelph::blank_id("my notes", "b0")) + " knows carol");
    }

    SUBCASE(".new keeps it")
    {
        interactive.process(".new");
        interactive.process("_:b0 knows bob");
        CHECK(answer_of(collector, interactive, "X knows bob") == "_:@068feb5243a0018d knows bob");
    }

    SUBCASE("without an argument it shows the identity that holds for its line, as .lang shows the language")
    {
        collector.clear();
        interactive.process(".identity");
        CHECK(last_out_text(collector) == "The current identity is 'http://example.org/doc'");

        // Asked, it is not changed.
        interactive.process("_:b0 knows carol");
        CHECK(answer_of(collector, interactive, "X knows carol") == "_:@068feb5243a0018d knows carol");
    }

    SUBCASE("it takes at most one argument")
    {
        CHECK_THROWS_WITH(interactive.process(".identity a b"), doctest::Contains("Usage: .identity [iri]"));

        // A text that is empty and enclosed in quotes constitutes no
        // argument for a command, thus setting nothing either.
        collector.clear();
        interactive.process(".identity \"\"");
        CHECK(last_out_text(collector) == "The current identity is 'http://example.org/doc'");

        // Neither line produced any outcome.
        interactive.process("_:b0 knows carol");
        CHECK(answer_of(collector, interactive, "X knows carol") == "_:@068feb5243a0018d knows carol");
    }

    SUBCASE("a backslash in the identity is a character of it")
    {
        // The identity is embedded within the code of every statement
        // that names a label, inside a Janet string, where a backslash
        // begins an escape.
        interactive.process(".identity C:\\docs\\x");
        interactive.process("_:b0 knows carol");
        CHECK(Zelph::blank_id("C:\\docs\\x", "b0") == 0x07199a1478de1808ull);
        CHECK(answer_of(collector, interactive, "X knows carol") == "_:@07199a1478de1808 knows carol");
    }

    SUBCASE(".help knows the command")
    {
        collector.clear();
        interactive.process(".help .identity");
        CHECK(any_event_contains(collector, ".identity [iri]"));
        collector.clear();
        interactive.process(".help");
        CHECK(any_event_contains(collector, ".identity [iri]"));
    }
}

// A script file is a document in its own right, identified by the file IRI
// of its canonical path: the same file yields identical nodes across every
// run and in every session, regardless of whether it is imported or named
// on the command line, and a duplicate located elsewhere constitutes a
// different document.
TEST_CASE("blank nodes: the lines of a script file belong to the document that the file is")
{
    const auto script  = write_script("document.zph", "_:b0 knows bob\n");
    const Node in_file = Zelph::blank_id(zelph::platform::file_iri(script), "b0");

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".identity http://example.org/doc");

    SUBCASE("imported as a module")
    {
        interactive.process(".import " + script.string());
        CHECK(answer_of(collector, interactive, "X knows bob") == zelph::string::blank_reference(in_file) + " knows bob");
    }

    SUBCASE("run as a session script")
    {
        interactive.process_file(script.string());
        CHECK(answer_of(collector, interactive, "X knows bob") == zelph::string::blank_reference(in_file) + " knows bob");
    }

    SUBCASE("the lines behind the import belong to the session again")
    {
        interactive.process(".import " + script.string());
        interactive.process("_:b0 knows carol");
        CHECK(answer_of(collector, interactive, "X knows carol") == "_:@068feb5243a0018d knows carol");
    }

    SUBCASE("a file that another file imports is a document of its own")
    {
        const auto outer = write_script("outer.zph", ".import " + script.string() + "\n_:b0 likes carol\n");
        interactive.process(".import " + outer.string());
        CHECK(answer_of(collector, interactive, "X knows bob") == zelph::string::blank_reference(in_file) + " knows bob");
        CHECK(answer_of(collector, interactive, "X likes carol") == zelph::string::blank_reference(Zelph::blank_id(zelph::platform::file_iri(outer), "b0")) + " likes carol");
        fs::remove(outer);
    }

    SUBCASE("a copy at another place is another document")
    {
        const auto copy = write_script("document_copy.zph", "_:b0 knows bob\n");
        interactive.process(".import " + script.string());
        interactive.process(".import " + copy.string());
        collector.clear();
        interactive.process("X knows bob");
        CHECK(collect_answers(collector).size() == 2);
        fs::remove(copy);
    }

    SUBCASE("imported through a path that is not canonical")
    {
        // The identity corresponds to the file, regardless of how the
        // path to it is expressed: derived from the path as given, the
        // same file would constitute a different document for each
        // spelling.
        const auto dir = fs::temp_directory_path() / "zelph_blank_nodes_dir";
        fs::create_directories(dir);
        const auto inside = dir / "inside.zph";
        std::ofstream(inside) << "_:b0 knows erin\n";
        interactive.process(".import " + (dir / ".." / dir.filename() / "inside.zph").string());
        CHECK(answer_of(collector, interactive, "X knows erin") == zelph::string::blank_reference(Zelph::blank_id(zelph::platform::file_iri(inside), "b0")) + " knows erin");
        fs::remove_all(dir);
    }

    SUBCASE("a command within the file names the node of the file by its label")
    {
        // The label of a command is part of the document associated with
        // its line, just as the label of a statement is. Read under the
        // identity of the session, the command would not find the node
        // that the preceding line wrote.
        const auto with_command = write_script("with_command.zph", "_:b0 knows frank\n.name _:b0 en Ada\n");
        interactive.process(".import " + with_command.string());
        CHECK(interactive.graph()->get_node("Ada", "en") == Zelph::blank_id(zelph::platform::file_iri(with_command), "b0"));
        fs::remove(with_command);
    }

    fs::remove(script);
}

// A file that names its blank nodes by the place where it is stored names
// other nodes once it is moved or installed in a different location.
// `.identity` within the file gives the file an identity of its own
// choosing, from that line onward until the file's end, just as `.lang` in
// a module governs the lines of the module. It sets nothing beyond the
// file: the session that reads it keeps its own identity, which only a line
// of the session itself sets.
TEST_CASE("blank nodes: .identity in a script file sets the identity of that file")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".identity http://example.org/doc");

    const auto under = [](const std::string& identity, const std::string& label)
    { return zelph::string::blank_reference(Zelph::blank_id(identity, label)); };

    SUBCASE("from its line on, in a module")
    {
        const auto module = write_script("identity_module.zph", "_:b0 likes carol\n.identity http://example.org/other\n_:b0 likes dave\n");
        interactive.process(".import " + module.string());
        CHECK(answer_of(collector, interactive, "X likes carol") == under(zelph::platform::file_iri(module), "b0") + " likes carol");
        CHECK(answer_of(collector, interactive, "X likes dave") == "_:@06447ae5244b6ada likes dave");

        SUBCASE("and the session keeps the identity it had")
        {
            interactive.process("_:b0 knows erin");
            CHECK(answer_of(collector, interactive, "X knows erin") == "_:@068feb5243a0018d knows erin");
        }
        fs::remove(module);
    }

    SUBCASE("in a session script, which sets its own identity and not that of the session")
    {
        const auto script = write_script("identity_session.zph", ".identity http://example.org/other\n_:b0 likes dave\n");
        interactive.process_file(script.string());
        CHECK(answer_of(collector, interactive, "X likes dave") == "_:@06447ae5244b6ada likes dave");
        interactive.process("_:b0 knows erin");
        CHECK(answer_of(collector, interactive, "X knows erin") == "_:@068feb5243a0018d knows erin");
        fs::remove(script);
    }

    SUBCASE("it ends with the file that set it, for the file that imports that file as well")
    {
        const auto inner = write_script("identity_inner.zph", ".identity http://example.org/other\n_:b0 likes dave\n");
        const auto outer = write_script("identity_outer.zph", ".import " + inner.string() + "\n_:b0 likes carol\n");
        interactive.process(".import " + outer.string());
        CHECK(answer_of(collector, interactive, "X likes dave") == "_:@06447ae5244b6ada likes dave");
        CHECK(answer_of(collector, interactive, "X likes carol") == under(zelph::platform::file_iri(outer), "b0") + " likes carol");
        fs::remove(inner);
        fs::remove(outer);
    }

    SUBCASE("without an argument it shows the identity of the file")
    {
        const auto asking = write_script("identity_asking.zph", ".identity\n.identity http://example.org/other\n.identity\n");
        collector.clear();
        interactive.process(".import " + asking.string());
        CHECK(any_output_contains(collector, "The current identity is '" + zelph::platform::file_iri(asking) + "'"));
        CHECK(any_output_contains(collector, "The current identity is 'http://example.org/other'"));
        fs::remove(asking);
    }

    SUBCASE("a .new in a session script keeps the identity the file has set")
    {
        // A session script is re-read from the line behind its `.new`,
        // and the lines located there belong to the same file.
        const auto script = write_script("identity_new.zph", ".identity http://example.org/other\n_:b0 likes carol\n.new\n_:b0 likes dave\n");
        interactive.process_file(script.string());
        CHECK(answer_of(collector, interactive, "X likes dave") == "_:@06447ae5244b6ada likes dave");
        collector.clear();
        interactive.process("X likes carol");
        CHECK(collect_answers(collector).empty());
        fs::remove(script);
    }
}

// The file IRI is generated upon the first label in a file requesting it,
// at which point a line within the file might have changed the working
// directory. A script named by a relative path was subsequently sought from
// the new directory, not located, and identified by a path pointing to no
// existing file: the same script thus named different blank nodes than when
// named by its absolute path, and a second run into a saved network caused
// them to be created anew.
TEST_CASE("blank nodes: the identity of a script file does not follow a working directory that the file changes")
{
    const auto dir = fs::temp_directory_path() / "zelph_blank_nodes_cwd";
    fs::create_directories(dir / "sub");
    const auto script = dir / "sub" / "moves.zph";
    std::ofstream(script) << "%(os/cd \"..\")\n_:b0 knows bob\n";
    const std::string expected = zelph::string::blank_reference(Zelph::blank_id(zelph::platform::file_iri(script), "b0")) + " knows bob";

    // The process's directory is the sole element here that persists
    // beyond the case, so it returns to its original location
    // regardless of what occurs.
    struct WorkingDirectory
    {
        fs::path before = fs::current_path();
        ~WorkingDirectory()
        {
            std::error_code ec;
            fs::current_path(before, ec);
        }
    } working_directory;
    fs::current_path(dir);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    SUBCASE("named by a relative path, as on the command line")
    {
        interactive.process_file((fs::path("sub") / "moves.zph").string());
    }

    SUBCASE("imported by a relative path")
    {
        interactive.process(".import " + (fs::path("sub") / "moves.zph").string());
    }

    SUBCASE("named by its absolute path")
    {
        interactive.process_file(script.string());
    }

    CHECK(answer_of(collector, interactive, "X knows bob") == expected);

    fs::current_path(working_directory.before);
    fs::remove_all(dir);
}

// The same file must provide identical nodes across every session, or a
// network saved during one run of a script would not meet the nodes of the
// next run. Nothing from a session enters the id: neither its identity,
// nor anything it created previously.
TEST_CASE("blank nodes: a script file names the same blank nodes in every session")
{
    const auto script = write_script("every_session.zph", "_:b0 knows bob\n");

    const auto in_a_session = [&](const std::string& before)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, before);
        interactive.process(".import " + script.string());
        return answer_of(collector, interactive, "X knows bob");
    };

    const std::string first = in_a_session("");
    CHECK(first == zelph::string::blank_reference(Zelph::blank_id(zelph::platform::file_iri(script), "b0")) + " knows bob");
    CHECK(in_a_session(".identity http://example.org/doc\nx p y\n_:b0 q z\n") == first);

    fs::remove(script);
}

// The identity of a file enters the id of each of its blank nodes, hence
// the text of the IRI must be the same for the same file, however the
// path to it is written, and must not change between engine versions.
TEST_CASE("blank nodes: the file IRI of a script is made from its canonical path")
{
    // The name is "Grüße 100%.zph", given as a UTF-8 literal: a narrow
    // literal handed to a path is interpreted using the system's code
    // page on Windows, thereby referencing a different file in that
    // context.
    const auto dir = fs::temp_directory_path() / "zelph_blank_nodes_iri dir#1";
    fs::create_directories(dir);
    const auto file = dir / fs::path(u8"Gr\u00FC\u00DFe 100%.zph");
    std::ofstream(file) << "a p b\n";

    // A blank, "#" and "%" undergo escaping, while a character outside ASCII
    // remains unchanged. The part preceding it denotes the temporary directory
    // of the machine.
    const std::string expected = zelph::platform::file_iri(file);
    const std::string tail     = "/zelph_blank_nodes_iri%20dir%231/Gr\xC3\xBC\xC3\x9F"
                                 "e%20100%25.zph";
    REQUIRE(expected.size() > tail.size());
    CHECK(expected.compare(expected.size() - tail.size(), tail.size(), tail) == 0);
    CHECK(expected.rfind("file:///", 0) == 0);
    CHECK(expected.find(' ') == std::string::npos);

    SUBCASE("a relative path and a path through \"..\" give the same text")
    {
        CHECK(zelph::platform::file_iri(dir / ".." / dir.filename() / file.filename()) == expected);
        CHECK(zelph::platform::file_iri(fs::relative(file, fs::current_path())) == expected);
    }

    SUBCASE("a symbolic link gives the text of the file it leads to")
    {
        const auto      link = fs::temp_directory_path() / "zelph_blank_nodes_iri_link.zph";
        std::error_code ec;
        fs::remove(link, ec);
        ec.clear();
        fs::create_symlink(file, link, ec);
        if (ec)
        {
            // Not every account is permitted to generate a link:
            // Windows requests a privilege unique to it. The run then
            // says that this was not verified, unlike before when it
            // used to pass without comment.
            MESSAGE("no symbolic link could be created (" << ec.message() << "), so the file IRI of a link was not checked");
        }
        else
        {
            CHECK(zelph::platform::file_iri(link) == expected);
            fs::remove(link);
        }
    }

    // The root that an absolute path lacking a drive gets on this
    // system: none when a path starts with "/", and the current drive on
    // Windows, which follows the empty host after a third slash.
#ifdef _WIN32
    const std::u8string drive = fs::current_path().root_name().generic_u8string();
    const std::string   root  = "/" + std::string(drive.begin(), drive.end());
#else
    const std::string root;
#endif

    SUBCASE("a file that does not exist is identified by its absolute path, normalized")
    {
        CHECK(zelph::platform::file_iri("/no/such dir/x.zph") == "file://" + root + "/no/such%20dir/x.zph");
        CHECK(zelph::platform::file_iri("/no/such dir/sub/../x.zph") == "file://" + root + "/no/such%20dir/x.zph");
    }

    SUBCASE("which characters are escaped, and with which digits")
    {
        // The IRI's text is incorporated into every id, making the set of
        // escaped characters and the case of the digits as fixed as the
        // hash. An escape containing a letter among its digits shows the
        // case, whereas those of a blank, "#" and "%" do not.
        const auto last_part = [&](const std::string& name)
        {
            const std::string iri = zelph::platform::file_iri(fs::path("/no/such") / name);
            return iri.substr(iri.rfind('/') + 1);
        };

        // What the path of an IRI takes as it stands: the unreserved
        // characters and the delimiters that separate nothing there.
        CHECK(last_part("a-b.c_d~e0Z") == "a-b.c_d~e0Z");
        CHECK(last_part("!$&'()+,;=@") == "!$&'()+,;=@");

        // Every other ASCII character receives its escape.
        CHECK(last_part("a b#c%d") == "a%20b%23c%25d");
        CHECK(last_part("[a]{b}^c`d") == "%5Ba%5D%7Bb%7D%5Ec%60d");
        CHECK(last_part("x\x7Fy") == "x%7Fy");
#ifndef _WIN32
        // Characters that a file name cannot hold on Windows, where "\" acts as
        // the separator between two parts of a path and ":" comes after a
        // drive.
        CHECK(last_part("a\"b<c>d?e|f\\g*h:i") == "a%22b%3Cc%3Ed%3Fe%7Cf%5Cg*h:i");
        CHECK(last_part("a\x01"
                        "b\x1F"
                        "c")
              == "a%01b%1Fc");
#endif
    }

#ifdef _WIN32
    SUBCASE("a drive is written with its letter and colon behind the third slash")
    {
        // The drive associated with the file, which need not be the current
        // one.
        const std::u8string name = fs::canonical(file).root_name().generic_u8string();
        const std::string   drive_of_file(name.begin(), name.end());
        REQUIRE(drive_of_file.size() == 2);
        CHECK(expected.rfind("file:///" + drive_of_file + "/", 0) == 0);
    }

    SUBCASE("a network path brings its host along, behind two slashes")
    {
        // No share of this name exists, and none needs to: a path that
        // cannot be made canonical is entered as the absolute path it
        // is, just as the path of a missing file is, and the host is
        // this machine, so nothing awaits the network.
        CHECK(zelph::platform::file_iri(fs::path(L"\\\\localhost\\zelph-no-such-share\\such dir\\x.zph")) == "file://localhost/zelph-no-such-share/such%20dir/x.zph");
        CHECK(zelph::platform::file_iri(fs::path(L"\\\\localhost\\zelph-no-such-share\\such dir\\sub\\..\\x.zph")) == "file://localhost/zelph-no-such-share/such%20dir/x.zph");
    }
#endif

    fs::remove_all(dir);
}

// --- commands ----------------------------------------------------------------

// The commands that take a node take a blank node in either of the two forms
// a statement writes it, because the line that zelph prints for a blank node
// is what a user holds to reference it. A command creates no node, thus a
// label is known to it solely after a statement has written it.
TEST_CASE("blank nodes: a command takes a blank node as its argument")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
_:b0 knows bob
)");
    const Node        blank  = Zelph::blank_id(doc, "b0");
    const std::string number = std::to_string(blank);

    SUBCASE(".node by the label and by the id")
    {
        collector.clear();
        interactive.process(".node _:b0");
        CHECK(any_event_contains(collector, "Node ID: " + number));
        CHECK(any_event_contains(collector, "Representation: _:@068feb5243a0018d"));

        collector.clear();
        interactive.process(".node _:@068feb5243a0018d");
        CHECK(any_event_contains(collector, "Node ID: " + number));
    }

    SUBCASE(".out and .in")
    {
        collector.clear();
        interactive.process(".out _:b0");
        CHECK(any_event_contains(collector, "_:@068feb5243a0018d knows bob"));
        collector.clear();
        interactive.process(".in _:@068feb5243a0018d");
        CHECK(any_event_contains(collector, "_:@068feb5243a0018d knows bob"));
    }

    SUBCASE(".name and .delname")
    {
        interactive.process(".name _:b0 en Ada");
        CHECK(graph->get_node("Ada", "en") == blank);
        interactive.process(".delname _:@068feb5243a0018d en");
        CHECK(graph->get_node("Ada", "en") == 0);
    }

    SUBCASE(".remove")
    {
        interactive.process(".remove _:b0");
        CHECK_FALSE(graph->exists(blank));
    }

    SUBCASE(".explain says that it is a node and no fact")
    {
        // The reading of the argument as a pattern yields the node, and
        // the command treated it as a fact that was not asserted.
        CHECK_THROWS_WITH(interactive.process(".explain _:b0"), doctest::Contains("'_:b0' is a node, not a fact"));
        CHECK_THROWS_WITH(interactive.process(".explain _:@068feb5243a0018d"), doctest::Contains("is a node, not a fact"));
    }

    SUBCASE("a pattern of a command holds a blank node")
    {
        interactive.process(".prune-facts (_:b0 knows X)");
        collector.clear();
        interactive.process("X knows bob");
        CHECK(collect_answers(collector).empty());
    }

    SUBCASE("a label the graph has no node for is unknown, and the command creates none")
    {
        const auto before = graph->count();
        CHECK_THROWS_WITH(interactive.process(".node _:nobody"), doctest::Contains("Unknown node '_:nobody'"));
        CHECK_THROWS_WITH(interactive.process(".out _:nobody"), doctest::Contains("Unknown node"));
        CHECK_THROWS_WITH(interactive.process(".remove _:nobody"), doctest::Contains("Unknown node '_:nobody'"));
        CHECK_THROWS_WITH(interactive.process(".name _:nobody en Ada"), doctest::Contains("Unknown node '_:nobody'"));
        CHECK(graph->count() == before);
        CHECK(graph->get_node("_:nobody", "zelph") == 0);
    }

    SUBCASE("a malformed token is reported as such")
    {
        CHECK_THROWS_WITH(interactive.process(".node _:a:b"), doctest::Contains("\"_:a:b\" is not a blank node"));
        CHECK_THROWS_WITH(interactive.process(".node _:@12"), doctest::Contains("\"_:@12\" does not refer to a blank node"));
    }

    SUBCASE("the new name of .name is no blank node")
    {
        // Bare, the token functions as a blank node at any position it
        // occupies, thus it is incapable of serving as the text of a name.
        const Node bob = graph->get_node("bob", "zelph");
        CHECK_THROWS_WITH(interactive.process(".name bob _:b1"), doctest::Contains("'_:b1' is a blank node and not a name"));
        CHECK(graph->get_node("bob", "zelph") == bob);

        interactive.process(".name bob \"_:b1\"");
        CHECK(graph->get_node("_:b1", "zelph") == bob);

        // Whether the new name appears within quotes is determined by
        // examining its token directly. Interpreted from the whole
        // line, the name was initially treated as the bare token
        // positioned ahead of it here, and then refused.
        interactive.process(".name _:b0 \"_:b0\"");
        CHECK(graph->get_node("_:b0", "zelph") == blank);
        CHECK_THROWS_WITH(interactive.process(".name \"_:b0\" _:b0"), doctest::Contains("'_:b0' is a blank node and not a name"));
    }

    SUBCASE("in quotes the argument is a name")
    {
        interactive.process("\"_:b0\" knows carol");
        collector.clear();
        interactive.process(".node \"_:b0\"");
        CHECK(any_event_contains(collector, "Node ID: " + std::to_string(graph->get_node("_:b0", "zelph"))));
    }
}

// --- Janet -------------------------------------------------------------------

// A Janet program names the document directly, as its code does not
// belong to any line of a script. The two strings give the node that the
// script form gives for the identical document and label.
TEST_CASE("blank nodes: zelph/blank and zelph/blank-at find or create a blank node")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
_:b0 knows bob
)");
    const auto before = graph->count();

    CHECK(janet_value(collector, interactive, "(zelph/blank \"http://example.org/doc\" \"b0\")") == "<zelph/node _:@068feb5243a0018d>");
    CHECK(graph->count() == before);

    CHECK(janet_value(collector, interactive, "(zelph/blank \"http://example.org/doc\" \"b1\")") == "<zelph/node _:@05a67d20418a1f7d>");
    CHECK(graph->exists(0x05a67d20418a1f7dull));

    CHECK(janet_value(collector, interactive, "(zelph/blank-at \"http://example.org/doc\" 0)") == "<zelph/node _:@0589e7ef2255771d>");
    CHECK(janet_value(collector, interactive, "(zelph/blank-at \"http://example.org/doc\" 1)") == "<zelph/node _:@07b7a61a17e5739b>");
    CHECK(graph->exists(0x07b7a61a17e5739bull));
    CHECK(graph->count() == before + 3);

    SUBCASE("a text is hashed with its length, a zero byte included")
    {
        CHECK(janet_value(collector, interactive, "(zelph/blank \"http://example.org/doc\" \"\\0\\0\\0\\0\\0\\0\\0\\0\")") == "<zelph/node _:@074f5e96a7b1479c>");
    }

    SUBCASE("the node is one that a statement can be built with")
    {
        interactive.process("%(zelph/fact (zelph/blank \"http://example.org/doc\" \"b1\") \"knows\" \"bob\")");
        collector.clear();
        interactive.process("X knows bob");
        CHECK(answers_contain(collector, "_:@05a67d20418a1f7d knows bob"));
    }

    SUBCASE("an ordinal is a whole number that is not negative")
    {
        CHECK_THROWS(interactive.process("%(zelph/blank-at \"http://example.org/doc\" -1)"));
        CHECK_THROWS(interactive.process("%(zelph/blank-at \"http://example.org/doc\" 1.5)"));
    }
}

// The identifier reference in Janet: what the script form `_:@...` calls.
TEST_CASE("blank nodes: zelph/blank-id finds a blank node the graph holds and creates none")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    auto* const                 graph = interactive.graph();
    process_lines(interactive, R"(
.identity http://example.org/doc
_:b0 knows bob
)");

    CHECK(janet_value(collector, interactive, "(zelph/blank-id \"068feb5243a0018d\")") == "<zelph/node _:@068feb5243a0018d>");
    CHECK(janet_value(collector, interactive, "(zelph/blank-id \"068FEB5243A0018D\")") == "<zelph/node _:@068feb5243a0018d>");

    // An id that the graph does not hold is answered with nil, just as
    // zelph/lookup responds to a name lacking a node: a program
    // inquires whether the node exists, and an error would be the sole
    // means by which it could discover that it does not.
    const auto before = graph->count();
    CHECK(janet_value(collector, interactive, "(zelph/blank-id \"05a67d20418a1f7d\")") == "nil");
    CHECK(janet_value(collector, interactive, "(zelph/blank-id \"05A67D20418A1F7D\")") == "nil");
    CHECK_FALSE(graph->exists(0x05a67d20418a1f7dull));
    CHECK_THROWS_WITH(interactive.process("%(zelph/blank-id \"12\")"), doctest::Contains("\"12\" is not the id of a blank node"));
    CHECK_THROWS_WITH(interactive.process("%(zelph/blank-id \"_:@068feb5243a0018d\")"), doctest::Contains("is not the id of a blank node"));
    CHECK_THROWS_WITH(interactive.process("%(zelph/blank-id \"000000000000000b\")"), doctest::Contains("is not the id of a blank node"));
    CHECK(graph->count() == before);
}

// A program traversing a graph must distinguish a blank node from a named
// node and from a statement, and without a name lookup. The kind
// corresponds to the class of the id, so each class that a script can make
// is asked here by making one.
TEST_CASE("blank nodes: zelph/node-kind answers the kind of a node")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
.identity http://example.org/doc
_:b0 knows bob
)");

    CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/blank \"http://example.org/doc\" \"b0\"))") == ":blank");
    CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/resolve \"bob\"))") == ":named");
    CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/resolve \"~\"))") == ":named");
    CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/fact \"alice\" \"knows\" \"bob\"))") == ":fact");
    CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/set \"a\" \"b\"))") == ":fact");
    CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/var))") == ":variable");

    SUBCASE("a collection outside a rule takes its id from the counter")
    {
        CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/collection \"a\" \"b\"))") == ":named");
    }

    SUBCASE("the kinds of rule text")
    {
        // A set that holds a variable, built outside a rule, qualifies
        // as a set pattern, and a collection that a rule writes serves
        // as its template.
        CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/set (zelph/fact 'X \"knows\" \"bob\")))") == ":set-pattern");
        CHECK(janet_value(collector, interactive, "(zelph/node-kind (zelph/rule-text (fn [] (zelph/collection \"a\" 'Y))))") == ":template");

        // A recipe is the term that a firing builds for a collection of
        // its rule's text.
        interactive.process("(X made Y) => (X in @{Y})");
        interactive.process("carol made dave");
        CHECK(janet_value(collector, interactive, "(map zelph/node-kind (zelph/targets \"carol\" \"in\"))") == "@[:recipe]");
    }

    SUBCASE("a value that is no node")
    {
        CHECK(janet_value(collector, interactive, "(zelph/node-kind nil)") == "nil");
        CHECK(janet_value(collector, interactive, "(zelph/node-kind \"bob\")") == "nil");
    }
}
