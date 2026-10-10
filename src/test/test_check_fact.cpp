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

#include "io/output.hpp"
#include "network/reasoning.hpp"
#include "network/unification.hpp"
#include "network/zelph.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace zelph::network;

// ---------------------------------------------------------------------------
// check_fact / create_hash fast paths and the parse_relation memo prefilter:
// all three changes are semantics-neutral rewrites of hot probe machinery.
// These tests pin the exact-triple semantics across object-set shapes and
// STORAGE MODES (the hash must stay a pure function of the element set even
// though small sets now hash via direct sorted iteration and only large
// unordered sets keep the copy+sort normalization), plus the tricky
// parse_relation branches the prefilter must not disturb.
// ---------------------------------------------------------------------------

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }

    // The two object lists involved in an actual 62-bit collision of
    // `s p <objects>`; refer to the first collision test to understand
    // their discovery and the factors they rely on.
    const std::vector<int> collision_a{1, 2, 3, 5, 7, 9, 10, 12, 13, 15, 19, 22, 24, 26, 27, 31, 34, 36, 40, 43, 45, 48, 49, 50, 52, 53, 55, 56, 59, 61};
    const std::vector<int> collision_b{1, 5, 6, 7, 9, 11, 12, 13, 14, 16, 18, 21, 23, 24, 25, 27, 29, 31, 34, 36, 37, 46, 52, 54, 55, 59, 60, 61};

    // The two object lists involved in a real 62-bit collision at one level
    // higher: the facts `s p <objects>` differ, yet the triples
    // `s q <that fact>` share an identifier. Refer to the bulk-import
    // collision test.
    const std::vector<int> import_collision_a{1, 2, 7, 9, 10, 11, 12, 14, 15, 16, 18, 20, 21, 22, 24, 25, 27, 29, 30, 33, 35, 36, 39, 41, 42, 43, 49, 50, 51, 53, 57, 58, 59, 61};
    const std::vector<int> import_collision_b{0, 1, 3, 4, 5, 6, 7, 8, 12, 14, 15, 18, 19, 21, 22, 23, 24, 25, 27, 30, 31, 33, 35, 37, 39, 40, 43, 45, 47, 48, 50, 52, 54, 55, 56, 57, 60};

    std::string object_names(const std::vector<int>& indices)
    {
        std::string result;
        for (const int i : indices)
            result += (result.empty() ? "o" : " o") + std::to_string(i);
        return result;
    }

    adjacency_set object_set(const std::vector<Node>& o, const std::vector<int>& indices)
    {
        adjacency_set result;
        for (const int i : indices)
            result.insert(o[static_cast<size_t>(i)]);
        return result;
    }
} // namespace

TEST_CASE("check_fact: exact-triple semantics across object-set shapes")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node d  = z.node("d");
    const Node op = z.node("op");

    const Node f = z.fact(a, op, {b, c});
    CHECK(z.check_fact(a, op, {b, c}).is_known());
    CHECK(z.check_fact(a, op, {c, b}).is_known());          // insertion order irrelevant
    CHECK_FALSE(z.check_fact(a, op, {b}).is_known());       // subset: different node
    CHECK_FALSE(z.check_fact(a, op, {b, c, d}).is_known()); // superset: different node
    CHECK_FALSE(z.check_fact(b, op, {a, c}).is_known());    // roles swapped
    CHECK(z.check_fact(a, op, {b, c}).relation() == f);

    // Self-fact: subject == object draws no separate object edge; the
    // t == subject exemption in the probe must accept it.
    const Node self = z.fact(a, op, {a});
    CHECK(z.check_fact(a, op, {a}).is_known());
    CHECK(z.check_fact(a, op, {a}).relation() == self);
}

TEST_CASE("check_fact: hash is independent of object-set storage mode and iteration order")
{
    Zelph      z(null_handler());
    const Node s  = z.node("s");
    const Node op = z.node("op");

    // 200 objects: Set storage on both sides, with DIFFERENT insertion
    // orders -- unordered iteration differs, so this is red if the hash
    // ever becomes iteration-order-dependent (i.e. if the copy+sort
    // normalization for Set storage were dropped).
    adjacency_set     objs_up;
    adjacency_set     objs_down;
    std::vector<Node> nodes;
    for (int i = 0; i < 200; ++i)
        nodes.push_back(z.node("o" + std::to_string(i)));
    for (int i = 0; i < 200; ++i)
        objs_up.insert(nodes[static_cast<size_t>(i)]);
    for (int i = 199; i >= 0; --i)
        objs_down.insert(nodes[static_cast<size_t>(i)]);

    const Node big = z.fact(s, op, objs_up);
    CHECK(z.check_fact(s, op, objs_down).is_known());
    CHECK(z.check_fact(s, op, objs_down).relation() == big);

    // 50 objects: Vector storage (sorted payload) -- the direct-iteration
    // fast path; descending insertion order must land on the same node.
    adjacency_set mid_up;
    adjacency_set mid_down;
    for (int i = 0; i < 50; ++i)
        mid_up.insert(nodes[static_cast<size_t>(i)]);
    for (int i = 49; i >= 0; --i)
        mid_down.insert(nodes[static_cast<size_t>(i)]);

    const Node midf = z.fact(s, op, mid_down);
    CHECK(z.check_fact(s, op, mid_up).is_known());
    CHECK(z.check_fact(s, op, mid_up).relation() == midf);
}

// Set storage traverses elements according to the sequence they were added,
// meaning a set constructed through a different sequence will yield a
// distinct iteration order even though the contained elements remain
// identical. The sort() method imposes the ordering that smaller storages
// inherently possess, which a reader that must not depend on how the graph
// was built takes (.explain's joins, see Unification::enumerate_by_id), and
// the set stays a set.
TEST_CASE("adjacency_set: sort makes Set storage iterate ascending")
{
    adjacency_set     descending;
    std::vector<Node> inserted;
    for (Node n = 400; n > 200; --n)
    {
        descending.insert(n);
        inserted.push_back(n);
    }
    REQUIRE_FALSE(descending.iterates_sorted());
    CHECK_FALSE(std::is_sorted(descending.begin(), descending.end()));

    descending.sort();
    std::sort(inserted.begin(), inserted.end());
    CHECK(std::vector<Node>(descending.begin(), descending.end()) == inserted);
    CHECK(descending.size() == 200);
    CHECK(std::all_of(inserted.begin(), inserted.end(), [&](const Node n)
                      { return descending.count(n) == 1; }));
    CHECK(descending.count(200) == 0);

    adjacency_set small{30, 10, 20};
    small.sort();
    CHECK(std::vector<Node>(small.begin(), small.end()) == std::vector<Node>{10, 20, 30});
}

TEST_CASE("check_fact: the hash space is wide enough for six-figure graphs")
{
    // A node IS its hash, meaning the node width determines the size of
    // the hash space; consequently, two different facts with the same identifier
    // cannot both be stored: fact() never stores the second one (refer to the
    // next test), hence every collision still costs a statement. The wasm
    // build previously restricted Node to 32 bits (30 varying hash bits), which
    // made such collisions probable once the number of nodes reached a few
    // ten-thousand: the shipped Jacobian example (32k nodes) derived nothing
    // there, whereas the native build answered.
    //
    // A total of 40000 facts, spread across unique subjects and objects,
    // results in exactly 3 * 40000 nodes atop the core; due to hash-consing,
    // the count remains an exact invariant, meaning any collision manifests
    // either as a refusal or a shortfall.
    Zelph      z(null_handler());
    const Node op = z.node("op");
    z.fact(op, z.core.IsA, {z.core.RelationTypeCategory}); // declare up front, so the loop adds nothing but its own nodes

    const Node before = z.count();
    for (int i = 0; i < 40000; ++i)
    {
        const std::string n = std::to_string(i);
        z.fact(z.node("s" + n), op, {z.node("o" + n)});
    }

    CHECK(z.count() == before + 3 * 40000);
}

TEST_CASE("fact: a hash collision is refused, not merged")
{
    // Two different statements that share a node id, generated via ordinary
    // multi-object facts through the public interface: nothing is removed
    // or forced to get here. A fact's id is the 62-bit hash of its
    // statement, and no comparison examines the underlying structures, so
    // the second fact() used to trigger an assert that Release builds
    // compile out and then place its objects onto the FIRST fact's node --
    // both statements merged into a single entity that no one authored,
    // with no error reported.
    //
    // The pair was identified through a Pollard-rho search across subsets
    // of o0..o61 for the fact `s p <subset>`. Since the hash depends on
    // node ids, the pair holds for exactly these
    // ids: s, p, and o0..o61, which were created in this order on a fresh
    // engine. The precondition says so if that ever changes, and then the
    // pair must be searched again.
    Zelph             z(null_handler());
    const Node        s = z.node("s");
    const Node        p = z.node("p");
    std::vector<Node> o;
    for (int i = 0; i < 62; ++i)
        o.push_back(z.node("o" + std::to_string(i)));

    const adjacency_set A = object_set(o, collision_a);
    const adjacency_set B = object_set(o, collision_b);

    REQUIRE(Zelph::create_hash(p, s, A) == Zelph::create_hash(p, s, B));

    const Node first  = z.fact(s, p, A);
    const Node before = z.count();

    CHECK_THROWS_WITH(z.fact(s, p, B), doctest::Contains("62-bit hash"));
    CHECK(z.count() == before);

    // The refusal names both statements with every object. The two differ
    // solely in their objects, and the ordinary rendering shortens a list
    // this extensive into "(... 30 objects ...)", which would cause them
    // to appear identical.
    std::string message;
    try
    {
        z.fact(s, p, B);
    }
    catch (const std::runtime_error& e)
    {
        message = e.what();
    }
    CHECK(message.find("it holds \"s p " + object_names(collision_a) + "\"") != std::string::npos);
    CHECK(message.find("not \"s p " + object_names(collision_b) + "\"") != std::string::npos);
    CHECK(message.find("...") == std::string::npos);

    // The node still holds A and retains no part of B: since no
    // object exclusive to B was drawn onto it, B is not read as known
    // either.
    CHECK(z.check_fact(s, p, A).is_known());
    CHECK_FALSE(z.check_fact(s, p, B).is_known());
    for (const Node n : B)
    {
        if (A.count(n) == 0) CHECK_FALSE(z.has_left_edge(first, n));
    }
}

TEST_CASE("fact: a derivation that lands on a colliding node is refused and reported, not merged")
{
    // The identical pair, attained through the method by which inference
    // operates: a rule derives the second statement while the first
    // holds the node. The merge used to print a deduction concerning
    // the FIRST statement, as though the rule had derived it, and left
    // the node with the objects from both. Deduction turns a refusal of
    // fact() into a contradiction that carries its reason, and this is
    // precisely what must occur here too.
    //
    // After the core nodes, the REPL creates s, p, and o0..o61 in the
    // sequence specified by the first line's naming order, which gives the
    // ids the pair was searched for; the precondition verifies the final
    // one among them.
    zelph::test::run_both_modes([](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
                                {
        std::vector<int> all(62);
        for (int i = 0; i < 62; ++i)
            all[static_cast<size_t>(i)] = i;
        interactive.process("s p " + object_names(all));

        collector.clear();
        interactive.process(".node o61");
        REQUIRE(zelph::test::any_event_contains(collector, "Node ID: 74"));

        interactive.process("s p " + object_names(collision_a));
        interactive.process("(X go Y) => (X p " + object_names(collision_b) + ")");

        collector.clear();
        interactive.process("s go y");

        CHECK(zelph::test::has_contradiction(collector));
        CHECK(zelph::test::any_event_contains(collector, "62-bit hash")); });
}

TEST_CASE("fact: a derivation whose nested part lands on a colliding node stops the run, not merged")
{
    // The identical pair located one level deeper: the rule derives
    // `s q (s p B)`, with the part that collides being the nested `s p B`.
    // Deduction builds the parts of a consequence before validating the
    // consequence itself, and only a refusal of the consequence itself
    // results in a contradiction (previous test). A refusal occurring
    // during the construction of a part is not caught there, thus
    // halting the run with the identical message. Previously, the merge
    // would print a deduction of `s q (s p A)` instead, as though the rule
    // had derived that, and would leave the node of `s p A` containing the
    // objects from B as well.
    //
    // Ids and the precondition specified in the
    // previous test.
    zelph::test::run_both_modes([](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
                                {
        std::vector<int> all(62);
        for (int i = 0; i < 62; ++i)
            all[static_cast<size_t>(i)] = i;
        interactive.process("s p " + object_names(all));

        collector.clear();
        interactive.process(".node o61");
        REQUIRE(zelph::test::any_event_contains(collector, "Node ID: 74"));

        interactive.process("s p " + object_names(collision_a));
        interactive.process("(X go Y) => (X q (X p " + object_names(collision_b) + "))");

        CHECK_THROWS_WITH(interactive.process("s go y"), doctest::Contains("62-bit hash"));

        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);
        const Node        s = graph->get_node("s");
        const Node        p = graph->get_node("p");
        const Node        q = graph->get_node("q");
        std::vector<Node> o;
        for (int i = 0; i < 62; ++i)
            o.push_back(graph->get_node("o" + std::to_string(i)));

        const adjacency_set A     = object_set(o, collision_a);
        const adjacency_set B     = object_set(o, collision_b);
        const Answer        first = graph->check_fact(s, p, A);

        // The node still holds A and retains no part of B, and no
        // derivation occurred.
        REQUIRE(first.is_known());
        CHECK_FALSE(graph->check_fact(s, p, B).is_known());
        for (const Node n : B)
        {
            if (A.count(n) == 0) CHECK_FALSE(graph->has_left_edge(first.relation(), n));
        }
        CHECK_FALSE(graph->check_fact(s, q, {first.relation()}).is_known()); });
}

TEST_CASE("fact: a stored statement whose edges a partial view did not load is refused by name")
{
    // The ordinary method to reach the refusal: using `.load-partial`
    // without the right-hand adjacency results in every fact node
    // being present yet inaccessible, meaning that entering a fact the file
    // contains locates its node but cannot confirm it. Nothing is written
    // either way; what this pins is the message, previously an internal
    // error signalling a missing node.
    namespace fs       = std::filesystem;
    const auto network = fs::temp_directory_path() / "zelph_check_fact_partial_view.bin";

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");
    interactive.process(".save \"" + network.string() + "\"");
    interactive.process(".new");
    interactive.process(".load-partial \"" + network.string() + "\" right=none");

    CHECK_THROWS_WITH(interactive.process("a p b"), doctest::Contains("partially loaded view"));

    fs::remove(network);
}

TEST_CASE("fact: a refused statement leaves no node behind, so retrying it is refused the same way")
{
    // `a p a b` names `a` as the subject and also as one of several
    // objects, a configuration a fact is unable to express. Previously,
    // this refusal occurred during the middle of connecting the new node,
    // leaving the refused statement partially constructed alongside the
    // declaration of `p`. Subsequent retries encountered this incomplete
    // node: if `b` had already been connected before the throw, the
    // remnants read as a known fact lacking a predicate edge, prompting the
    // retry to say "this fact is known to be wrong". The order in which
    // either object is connected first depends on the node ids, hence both
    // orders.
    const auto check_refusal_leaves_nothing = [](const bool subject_first)
    {
        Zelph z(null_handler());
        Node  a = 0;
        Node  b = 0;
        if (subject_first)
        {
            a = z.node("a");
            b = z.node("b");
        }
        else
        {
            b = z.node("b");
            a = z.node("a");
        }
        const Node p = z.node("p");

        const Node before = z.count();
        CHECK_THROWS_WITH(z.fact(a, p, {a, b}), doctest::Contains("same subject and object"));
        CHECK(z.count() == before);
        CHECK_FALSE(z.check_fact(a, p, {a, b}).is_known());

        CHECK_THROWS_WITH(z.fact(a, p, {a, b}), doctest::Contains("same subject and object"));
        CHECK(z.count() == before);
    };

    SUBCASE("subject created before the other object")
    {
        check_refusal_leaves_nothing(true);
    }

    SUBCASE("other object created before the subject")
    {
        check_refusal_leaves_nothing(false);
    }
}

TEST_CASE("fact_import_trusted_single_object: a duplicate is not reported as a foreign node")
{
    // The bulk import tells a node that already holds the triple (a
    // duplicate: the Wikidata importer types a property once per thread, a
    // dump repeats a claim) from another node that holds different data,
    // which it counts and reports (next test). This test pins the duplicate
    // side: every shape a genuine duplicate can assume remains silent,
    // even when a node already has facts about it, making its
    // adjacency then exceed its individual triple.
    std::vector<std::string> reports;
    const auto               report_sink = [&](const zelph::io::OutputEvent& e)
    {
        if (e.channel == zelph::io::OutputChannel::Error || e.channel == zelph::io::OutputChannel::Diagnostic)
            reports.push_back(e.text);
    };

    Zelph      z(report_sink);
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node op = z.node("op");
    const Node q  = z.node("q");
    z.begin_bulk_import();
    z.fact_import_trusted_single_object(op, z.core.IsA, z.core.RelationTypeCategory);
    z.fact_import_trusted_single_object(q, z.core.IsA, z.core.RelationTypeCategory);

    const Node plain = z.fact_import_trusted_single_object(a, op, b);
    CHECK(z.fact_import_trusted_single_object(a, op, b) == plain);

    const Node self = z.fact_import_trusted_single_object(a, op, a);
    CHECK(z.fact_import_trusted_single_object(a, op, a) == self);

    const Node via_fact = z.fact(b, op, {c});
    CHECK(z.fact_import_trusted_single_object(b, op, c) == via_fact);

    const Node same_sp = z.fact(op, op, {c});
    CHECK(z.fact_import_trusted_single_object(op, op, c) == same_sp);

    z.fact(plain, q, {c}); // a fact ABOUT the fact
    z.fact(c, q, {plain}); // and one naming it as an object
    CHECK(z.fact_import_trusted_single_object(a, op, b) == plain);

    // The end of an import that left nothing unwritten conveys
    // nothing.
    CHECK(z.end_bulk_import() == 0);
    CHECK(reports.empty());
}

TEST_CASE("fact_import_trusted_single_object: a triple whose node holds another statement is left unwritten, and each import reports it")
{
    // Two different single-object triples sharing a node id, constructed
    // solely from ordinary facts: F1 and F2 are facts `s p <objects>`
    // across two different object lists, and `s q F1` and `s q F2` produce
    // identical hashes. The import previously discarded the second triple
    // silently, causing a prolonged import to omit the statement and leave
    // no trace of its absence. It remains unwritten, as writing it would
    // result in merging the two, yet the first such case of an
    // import is reported with both statements, and the end of the import
    // reports the total count.
    //
    // The pair emerged from a Pollard-rho search via Zelph::create_hash: a
    // subset X of o0..o61 gives the fact F = `s p X`, and the search stepped
    // from X to the id of `s q F`. Only the two facts composing the pair
    // need to be present, not the intermediate ones traversed during the
    // search. The identifiers correspond to s, p, o0..o61, and q, created in
    // this order on a fresh engine; the preconditions explicitly state this
    // if it ever changes, and in such a case, the pair must be searched
    // again.
    std::vector<std::string> reports;
    const auto               report_sink = [&](const zelph::io::OutputEvent& e)
    {
        if (e.channel == zelph::io::OutputChannel::Error || e.channel == zelph::io::OutputChannel::Diagnostic)
            reports.push_back(e.text);
    };

    Zelph             z(report_sink);
    const Node        s = z.node("s");
    const Node        p = z.node("p");
    std::vector<Node> o;
    for (int i = 0; i < 62; ++i)
        o.push_back(z.node("o" + std::to_string(i)));
    const Node q = z.node("q");

    const Node f1 = z.fact(s, p, object_set(o, import_collision_a));
    const Node f2 = z.fact(s, p, object_set(o, import_collision_b));
    REQUIRE(f1 != f2);
    REQUIRE(Zelph::create_hash(q, s, {f1}) == Zelph::create_hash(q, s, {f2}));

    z.begin_bulk_import();
    z.fact_import_trusted_single_object(q, z.core.IsA, z.core.RelationTypeCategory);
    const Node node = z.fact_import_trusted_single_object(s, q, f1);
    REQUIRE(reports.empty());

    CHECK(z.fact_import_trusted_single_object(s, q, f2) == node);
    REQUIRE(reports.size() == 1);

    SUBCASE("nothing of it is written, and the report names both statements")
    {
        // Neither adjacency map includes F2's object edge leading to
        // the node, which still holds the first triple.
        CHECK_FALSE(z.has_left_edge(node, f2));
        CHECK_FALSE(z.has_right_edge(f2, node));
        CHECK(z.check_fact(s, q, {f1}).is_known());
        CHECK_FALSE(z.check_fact(s, q, {f2}).is_known());

        // The nested facts are expressed using every object: the two
        // triples differ solely there, and the ordinary rendering
        // shortens a list of this length into "(... 34 objects ...)".
        INFO(reports[0]);
        CHECK(reports[0].find("it holds \"s q (s p " + object_names(import_collision_a) + ")\"") != std::string::npos);
        CHECK(reports[0].find("not \"s q (s p " + object_names(import_collision_b) + ")\"") != std::string::npos);
        CHECK(reports[0].find("...") == std::string::npos);
    }

    SUBCASE("a further case is only counted, and the end of the import reports the number")
    {
        // A file that is damaged may produce such a node per line, hence
        // the instances following the initial one are not reported
        // individually.
        z.fact_import_trusted_single_object(s, q, f2);
        CHECK(reports.size() == 1);

        CHECK(z.end_bulk_import() == 2);
        REQUIRE(reports.size() == 2);
        CHECK(reports[1].find("2 triples were left unwritten") != std::string::npos);
    }

    SUBCASE("the next import into the same engine reports its own first case and counts only its own")
    {
        // The qualifier import from Wikidata operates on the engine that
        // the main import has filled. A conflict in this context must be
        // reported, even though the main import already had one.
        CHECK(z.end_bulk_import() == 1);
        REQUIRE(reports.size() == 2);
        CHECK(reports[1].find("1 triple was left unwritten") != std::string::npos);

        z.begin_bulk_import();
        z.fact_import_trusted_single_object(s, q, f2);
        REQUIRE(reports.size() == 3);
        CHECK(reports[2].find("it holds") != std::string::npos);

        CHECK(z.end_bulk_import() == 1);
        CHECK(reports.size() == 4);
    }
}

namespace
{
    // One item, Q1, features two statements of type P1, with values Q2
    // and Q3 respectively. Each carries a qualifier, because the qualifier
    // import materializes only a statement that possesses one.
    const char* colliding_dump = R"json([
{"type":"item","id":"Q1","labels":{},"claims":{"P1":[{"mainsnak":{"snaktype":"value","property":"P1","datavalue":{"value":{"entity-type":"item","numeric-id":2,"id":"Q2"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2":[{"snaktype":"value","property":"P2","hash":"h1","datavalue":{"value":{"entity-type":"item","numeric-id":4,"id":"Q4"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}]},"qualifiers-order":["P2"],"id":"Q1$A","rank":"normal"},{"mainsnak":{"snaktype":"value","property":"P1","datavalue":{"value":{"entity-type":"item","numeric-id":3,"id":"Q3"},"type":"wikibase-entityid"},"datatype":"wikibase-item"},"type":"statement","qualifiers":{"P2":[{"snaktype":"value","property":"P2","hash":"h2","datavalue":{"value":{"entity-type":"item","numeric-id":4,"id":"Q4"},"type":"wikibase-entityid"},"datatype":"wikibase-item"}]},"qualifiers-order":["P2"],"id":"Q1$B","rank":"normal"}]},"sitelinks":{}}
]
)json";

    // The network in the bulk-import collision test, in the engine of an
    // Interactive: `.new` gives it the node ids of a fresh engine
    // instance. Subsequently, the four nodes corresponding to the two
    // colliding triples are allocated the Wikidata names that the dump uses
    // for them.
    void build_colliding_network(zelph::console::Interactive& interactive,
                                 const std::string&           subject,
                                 const std::string&           predicate,
                                 const std::string&           first,
                                 const std::string&           second)
    {
        interactive.process(".new");
        zelph::network::Reasoning* z = interactive.graph();

        const Node        s = z->node("s");
        const Node        p = z->node("p");
        std::vector<Node> o;
        for (int i = 0; i < 62; ++i)
            o.push_back(z->node("o" + std::to_string(i)));
        const Node q = z->node("q");

        const Node f1 = z->fact(s, p, object_set(o, import_collision_a));
        const Node f2 = z->fact(s, p, object_set(o, import_collision_b));
        REQUIRE(f1 != f2);
        REQUIRE(Zelph::create_hash(q, s, {f1}) == Zelph::create_hash(q, s, {f2}));

        z->set_name(s, subject, "wikidata", false);
        z->set_name(q, predicate, "wikidata", false);
        z->set_name(f1, first, "wikidata", false);
        z->set_name(f2, second, "wikidata", false);
    }

    std::size_t events_containing(const zelph::io::OutputCollector& collector, const std::string& text)
    {
        std::size_t n = 0;
        for (const auto& e : collector.events())
            if (e.text.find(text) != std::string::npos) ++n;
        return n;
    }
} // namespace

TEST_CASE("wikidata import: each import reports the triples it left unwritten")
{
    // The portion designated as the library half is pinned above; this area
    // corresponds to the importers' half. The main import and the qualifier
    // import enclose their respective workers using begin_bulk_import
    // and end_bulk_import. Absent these markers, the end of an import
    // conveyed no information regarding the number of triples it had left
    // unwritten, and a subsequent import following the initial one within
    // an engine did not report its first case either, as the count it
    // compared against belonged to the engine itself.
    //
    // In an actual dump, such a triple comes from a 64-bit hash collision
    // involving two of its statements, a situation no test file can
    // replicate. Thus, the network is built from the pair known to
    // collide, while the nodes of the two triples carry the Wikidata names
    // utilized in the dump: assigning a node a name in another language is
    // precisely what `.name <id> wikidata <name>` accomplishes for any
    // node, a fact included, and an import adds to the network it finds.
    const std::filesystem::path dump = std::filesystem::temp_directory_path() / "zelph_import_collision_test.json";
    {
        std::ofstream out(dump, std::ios::binary);
        out << colliding_dump;
    }
    std::filesystem::path cache = dump;
    cache.replace_extension(".bin");
    std::filesystem::remove(cache);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    SUBCASE("the main import")
    {
        // Q1 P1 Q2 is written, Q1 P1 Q3 shares the same identifier
        // and is not.
        build_colliding_network(interactive, "Q1", "P1", "Q2", "Q3");
        collector.clear();
        interactive.process(".load \"" + dump.string() + "\"");

        CHECK(events_containing(collector, "it holds") == 1);
        CHECK(events_containing(collector, "1 triple was left unwritten") == 1);
    }

    SUBCASE("the qualifier import, twice into the same engine")
    {
        // Q1 p:P1 Q1$A is written, Q1 p:P1 Q1$B shares the identical
        // identifier and is not. The second import meets the identical
        // scenario and reports it as its own.
        build_colliding_network(interactive, "Q1", "p:P1", "Q1$A", "Q1$B");
        collector.clear();
        interactive.process(".wikidata-qualifiers \"" + dump.string() + "\"");
        CHECK(events_containing(collector, "it holds") == 1);
        CHECK(events_containing(collector, "1 triple was left unwritten") == 1);

        collector.clear();
        interactive.process(".wikidata-qualifiers \"" + dump.string() + "\"");
        CHECK(events_containing(collector, "it holds") == 1);
        CHECK(events_containing(collector, "1 triple was left unwritten") == 1);
    }

    std::filesystem::remove(cache);
    std::filesystem::remove(dump);
}

TEST_CASE("parse_relation: memo prefilter keeps every branch's semantics")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");

    const Node f1 = z.fact(a, op, {b});
    CHECK(z.parse_relation(f1) == op);

    // Subject that is ITSELF a declared relation type: the bidirectional
    // exclusion must still pick the genuine predicate, not the subject.
    const Node op2 = z.node("op2");
    const Node f2  = z.fact(op, op2, {b});
    CHECK(z.parse_relation(f2) == op2);

    // subject == predicate: the relation==0 -> relation=subject fallback.
    const Node op3 = z.node("op3");
    const Node f3  = z.fact(op3, op3, {b});
    CHECK(z.parse_relation(f3) == op3);

    // A predicate auto-declared AFTER the memo was built (the calls above
    // built it) must be visible: red if the declaration hook fails to
    // invalidate the memo consumed here.
    const Node op4 = z.node("op4");
    const Node f4  = z.fact(a, op4, {b});
    CHECK(z.parse_relation(f4) == op4);
}

TEST_CASE("parse_relation_scoped: agrees with parse_relation across branch shapes under one lock scope")
{
    Zelph      z(null_handler());
    const Node a   = z.node("a");
    const Node b   = z.node("b");
    const Node op  = z.node("op");
    const Node f1  = z.fact(a, op, {b});
    const Node op2 = z.node("op2");
    const Node f2  = z.fact(op, op2, {b}); // subject is itself a declared relation type
    const Node op3 = z.node("op3");
    const Node f3  = z.fact(op3, op3, {b}); // subject == predicate fallback
    const Node sf  = z.fact(a, op, {a});    // self-fact: no separate object edge

    // Contract under test, too: the memo is fetched BEFORE the scope opens
    // (its lazy build takes the very locks the scope holds). NOTE: the
    // locking parse_relation must NOT be called while the scope is alive,
    // hence the hardcoded expected values instead of a direct comparison.
    const auto rel_types = z.relation_type_set();

    const Network::ReadScope scope = z.read_scope();
    CHECK(z.parse_relation_scoped(scope, *rel_types, f1) == op);
    CHECK(z.parse_relation_scoped(scope, *rel_types, f2) == op2); // bidirectional subject excluded
    CHECK(z.parse_relation_scoped(scope, *rel_types, f3) == op3); // relation==0 -> subject fallback
    CHECK(z.parse_relation_scoped(scope, *rel_types, sf) == op);
    CHECK(z.parse_relation_scoped(scope, *rel_types, a) == 0); // atom: no relation
    CHECK(scope.exists(f1));
    CHECK_FALSE(scope.exists(Node{0}));
}

TEST_CASE("collect_anchored_facts: exact anchored-candidate semantics")
{
    Zelph      z(null_handler());
    const Node a   = z.node("a");
    const Node b   = z.node("b");
    const Node c   = z.node("c");
    const Node op  = z.node("op");
    const Node op2 = z.node("op2");
    const Node op3 = z.node("op3");

    const Node f1 = z.fact(a, op, {b});
    const Node f2 = z.fact(a, op2, {c});

    // It reports the count of adjacency entries it read: every one
    // linked to the anchor, regardless of which entries the predicate
    // selects from them.
    adjacency_set out;
    CHECK(z.collect_anchored_facts(a, op, out) == z.right_count(a)); // subject-driven
    CHECK(z.right_count(a) >= 2);
    CHECK(out.count(f1) == 1);
    CHECK(out.count(f2) == 0); // different predicate
    CHECK(out.size() == 1);

    CHECK(z.collect_anchored_facts(b, op, out) == z.right_count(b)); // object-driven
    CHECK(out.count(f1) == 1);

    // Subject-role exclusion: g uses op2 as its SUBJECT (bidirectional),
    // so for relation op2 it must be filtered even though g -> op2 exists.
    const Node g = z.fact(op2, op3, {b});
    const Node h = z.fact(c, op2, {b});
    z.collect_anchored_facts(b, op2, out);
    CHECK(out.count(h) == 1);
    CHECK(out.count(g) == 0);

    z.collect_anchored_facts(Node{0}, op, out); // nonexistent anchor
    CHECK(out.empty());

    // As a subject: the facts that designate the anchor exclusively as
    // an object are excluded, yet it continues to read, and report, all
    // of the anchor's adjacency.
    const Node f3 = z.fact(c, op, {a});
    const Node f4 = z.fact(a, op, {a});
    CHECK(z.collect_anchored_facts(a, op, out) == z.right_count(a));
    CHECK(out.count(f3) == 1);
    CHECK(z.collect_anchored_facts(a, op, out, Zelph::AnchorRole::Subject) == z.right_count(a));
    CHECK(out.count(f1) == 1);
    CHECK(out.count(f3) == 0);
    CHECK(out.count(f4) == 1); // subject and object at once
    CHECK(out.size() == 2);
    z.collect_anchored_facts(b, op, out, Zelph::AnchorRole::Subject);
    CHECK(out.empty());

    // As an object: the facts that feature the anchor exclusively as their
    // subject are excluded. A subject functions as an object solely when it
    // serves as the single object within its fact, (a op a). A fact used as a
    // predicate exhibits an edge from each of its users, drawn in the same
    // manner as an object's, and these are not considered objects: (a op a)
    // used in (c (a op a) b) retains the anchor as its object.
    CHECK(z.collect_anchored_facts(a, op, out, Zelph::AnchorRole::Object) == z.right_count(a));
    CHECK(out.count(f1) == 0);
    CHECK(out.count(f3) == 1);
    CHECK(out.count(f4) == 1);
    CHECK(out.size() == 2);
    const Node f5 = z.fact(c, f4, {b});
    REQUIRE(z.get_right(f5).count(f4) == 1);
    REQUIRE(z.get_left(f4).count(f5) == 1);
    z.collect_anchored_facts(a, op, out, Zelph::AnchorRole::Object);
    CHECK(out.count(f4) == 1);
    CHECK(out.size() == 2);

    // Following a fact: solely those facts whose ids are greater
    // remain, and it continues to read, and report, all of the anchor's
    // adjacency.
    z.collect_anchored_facts(a, op, out);
    std::vector<Node> ids(out.begin(), out.end());
    std::sort(ids.begin(), ids.end());
    REQUIRE(ids.size() == 3);
    CHECK(z.collect_anchored_facts(a, op, out, Zelph::AnchorRole::Any, ids[0]) == z.right_count(a));
    CHECK(out.size() == 2);
    CHECK(out.count(ids[0]) == 0);
    CHECK(out.count(ids[1]) == 1);
    CHECK(out.count(ids[2]) == 1);
    z.collect_anchored_facts(a, op, out, Zelph::AnchorRole::Any, ids[2]);
    CHECK(out.empty());

    // Into a list: the same facts, each appearing only once, and the
    // identical count read. The contents previously held by the list are
    // replaced.
    const auto same_as_set = [&](const Node anchor, const Zelph::AnchorRole role, const Node after)
    {
        std::vector<Node> listed{f2};
        CHECK(z.collect_anchored_facts(anchor, op, listed, role, after) == z.right_count(anchor));
        z.collect_anchored_facts(anchor, op, out, role, after);
        std::vector<Node> kept(out.begin(), out.end());
        std::sort(kept.begin(), kept.end());
        std::sort(listed.begin(), listed.end());
        CHECK(std::adjacent_find(listed.begin(), listed.end()) == listed.end());
        CHECK(listed == kept);
    };
    for (const auto role : {Zelph::AnchorRole::Any, Zelph::AnchorRole::Subject, Zelph::AnchorRole::Object})
    {
        same_as_set(a, role, 0);
        same_as_set(a, role, ids[0]);
        same_as_set(b, role, 0);
    }
}

// ---------------------------------------------------------------------------
// PatternInfo / build_pattern_info: rule-static hoisting of the Unification
// constructor's pattern decomposition (semi-naive seeding). The condition-
// taking constructor DELEGATES to the PatternInfo overload, so equivalence
// of the two entry points reduces to build_pattern_info reproducing the
// former inline decomposition exactly -- pinned here across pattern shapes.
// End-to-end completeness of hoisted seeding is co-pinned suite-wide by
// `.semi-naive check`.
// ---------------------------------------------------------------------------

TEST_CASE("build_pattern_info: decomposition matches the get_fact_structures reading across pattern shapes")
{
    Zelph      z(null_handler());
    const Node A  = z.var();
    const Node B  = z.var();
    const Node C  = z.var();
    const Node op = z.node("op");

    // Flat pattern (A op B): variable subject -> no subject hint.
    const Node cond = z.fact(A, op, {B});
    {
        const PatternInfo pi = build_pattern_info(&z, cond, 1);
        CHECK(pi.condition == cond);
        CHECK(pi.relation == op);
        CHECK(pi.subject == A);
        CHECK(pi.objects.size() == 1);
        CHECK(pi.objects.count(B) == 1);
        CHECK(pi.subject_pred_hint == 0);
    }

    // Structured subject ((A f B) g C): the hint is the inner predicate.
    const Node f     = z.node("f");
    const Node g     = z.node("g");
    const Node inner = z.fact(A, f, {B});
    const Node cond2 = z.fact(inner, g, {C});
    {
        const PatternInfo pi = build_pattern_info(&z, cond2, 1);
        CHECK(pi.relation == g);
        CHECK(pi.subject == inner);
        CHECK(pi.subject_pred_hint == f);
        CHECK(pi.objects.count(C) == 1);
    }

    // Variable predicate (A W B): the raw variable is preserved -- the
    // constructor's relation-variable machinery must keep seeing it.
    const Node W     = z.var();
    const Node cond3 = z.fact(A, W, {B});
    {
        const PatternInfo pi = build_pattern_info(&z, cond3, 1);
        CHECK(pi.relation == W);
        CHECK(Zelph::is_var(pi.relation));
        CHECK(pi.subject == A);
    }

    // Multi-object pattern: the full object set is captured.
    const Node cond4 = z.fact(A, op, {B, C});
    {
        const PatternInfo pi = build_pattern_info(&z, cond4, 1);
        CHECK(pi.objects.size() == 2);
        CHECK(pi.objects.count(B) == 1);
        CHECK(pi.objects.count(C) == 1);
    }

    // Atom: no decomposable structure -> relation stays 0 (the constructor
    // then logs the fallback and leaves the relation list empty).
    {
        const PatternInfo pi = build_pattern_info(&z, op, 1);
        CHECK(pi.condition == op);
        CHECK(pi.relation == 0);
    }
}
