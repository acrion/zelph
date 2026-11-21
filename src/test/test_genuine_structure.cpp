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

#include <algorithm>
#include <filesystem>

#include "directory_rewrite.hpp"
#include "io/output.hpp"
#include "network/fact_structure.hpp"
#include "network/reasoning.hpp"
#include "network/zelph.hpp"
#include "test_helpers.hpp"

using namespace zelph::network;

// ---------------------------------------------------------------------------
// Genuine-structure store: fact() records the exact triple of every node it
// creates; get_fact_structures answers fs_cache misses from that store and
// walks the adjacency only for nodes without an entry. Pins:
//  (1) the stored reading IS the genuine triple across shapes;
//  (2) subject == predicate facts decompose like any other -- both roles
//      share one outgoing edge, which used to leave them without a reading;
//  (3) a path around fact() (trusted import, load, merge) keeps the store
//      armed: what fact() recorded answers from it, before and after, and
//      only the nodes it never saw are walked, with identical readings;
//  (4) growth-only full clears (relation-type declarations) do NOT disarm.
// Reading equivalence end-to-end is co-pinned by the whole suite (every
// gfs consumer now runs through the store) plus `.semi-naive check`.
// ---------------------------------------------------------------------------

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }

    bool has_reading(const FactStructureList& structs, const Node subject, const Node object)
    {
        for (const auto& fs : structs)
            if (fs.subject == subject && fs.objects.count(object) == 1) return true;
        return false;
    }
} // namespace

TEST_CASE("genuine store: created facts answer with their exact triple")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node op = z.node("op");

    z.set_logging(-1); // counter-only mode

    const Node f = z.fact(a, op, {b, c});
    {
        const auto s = get_fact_structures(&z, f, 1);
        REQUIRE(s->size() == 1);
        CHECK((*s)[0].subject == a);
        CHECK((*s)[0].predicate == op);
        CHECK((*s)[0].objects.count(b) == 1);
        CHECK((*s)[0].objects.count(c) == 1);
        CHECK((*s)[0].objects.size() == 2);
    }

    // Self-fact: stored objects arrive as {subject} -- identical to the
    // walk's self-referential repair.
    const Node self = z.fact(a, op, {a});
    {
        const auto s = get_fact_structures(&z, self, 1);
        REQUIRE(s->size() == 1);
        CHECK((*s)[0].subject == a);
        CHECK((*s)[0].objects.count(a) == 1);
    }

    // Same-predicate parent (the masquerade shape): both readings exact.
    const Node mid = z.fact(f, op, {c});
    CHECK(has_reading(*get_fact_structures(&z, mid, 1), f, c));
    CHECK(has_reading(*get_fact_structures(&z, f, 1), a, b));

    CHECK(z.genuine_stats().hits > 0);
}

TEST_CASE("genuine store: a subject == predicate fact decomposes like any other")
{
    Zelph      z(null_handler());
    const Node b   = z.node("b");
    const Node op3 = z.node("op3");

    // insert_fact writes fact's left side as {subject, predicate}, so when the
    // two are the same node both roles share ONE entry. The walk used to read
    // that as "subject, hence not the predicate" and returned no reading at
    // all -- and since unification reads every candidate through this
    // decomposition, such a fact answered no query and matched no rule.
    // `~ ~ ->` is exactly this shape and exists in every network.
    const Node f3 = z.fact(op3, op3, {b});
    CHECK(has_reading(*get_fact_structures(&z, f3, 1), op3, b));
    CHECK(z.parse_relation(f3) == op3);

    // The reading has to hash back to the node, which is what keeps a
    // parent fact that happens to be a relation type from being read as
    // its own predicate.
    const auto s = get_fact_structures(&z, f3, 1);
    REQUIRE(s->size() == 1);
    CHECK(Zelph::create_hash((*s)[0].predicate, (*s)[0].subject, (*s)[0].objects) == f3);
}

TEST_CASE("genuine store: revoking a rule-pattern mark does not disarm it")
{
    // The marking fact is engine bookkeeping and a leaf, but it used to be
    // deleted with remove_node -- and that goes through the WHOLESALE
    // invalidation funnel, which disarms both fact stores for the rest of the
    // session. Every later fs_cache miss then paid the full adjacency walk.
    //
    // It cost a factor of three on the Jacobian workload: eight patterns are
    // revoked while it runs, the first of them killed the store, and
    // `genuine: hits=456368 walks=0` became `hits=11133 walks=667540`. The
    // import went from 1.5 s to 4.9 s. Red if the removal takes the general
    // path again.
    Zelph      z(null_handler());
    const Node a = z.node("a");
    const Node b = z.node("b");
    const Node c = z.node("c");
    const Node d = z.node("d");
    const Node p = z.node("p");
    const Node q = z.node("q");

    const Node condition   = z.fact(a, p, {b});
    const Node consequence = z.fact(c, q, {d});
    const Node rule        = z.fact(condition, z.core.Causes, {consequence});

    z.mark_rule_patterns(rule, {condition, consequence});
    REQUIRE(z.is_rule_pattern(consequence));
    REQUIRE(z.fact_stores_enabled());

    REQUIRE(z.unmark_rule_pattern(consequence));
    CHECK_FALSE(z.is_rule_pattern(consequence));

    // The mark is gone from the graph too -- that part was never in doubt.
    const Node pred = z.rule_pattern_predicate(false);
    REQUIRE(pred != 0);
    CHECK_FALSE(z.check_fact(consequence, z.core.IsA, {pred}).is_known());

    // The point: the stores are still authoritative, and still recording.
    CHECK(z.fact_stores_enabled());

    // Probed on a fact created AFTERWARDS, which a disarmed store would not
    // have recorded.
    z.set_logging(-1); // counter-only mode
    z.reset_genuine_stats();
    const Node e     = z.node("e");
    const Node later = z.fact(a, p, {e});
    CHECK(has_reading(*get_fact_structures(&z, later, 1), a, e));
    CHECK(z.genuine_stats().hits == 1);
    CHECK(z.genuine_stats().walks == 0);
}

TEST_CASE("genuine store: a trusted import keeps it armed, and only the imported node is walked")
{
    // The bulk path writes its triple without fact(), so this store does not
    // record it. It previously disarmed both stores for the rest of the
    // session, which put every subsequent reconstruction on the walk: the
    // Jacobian workload after a .load of a two-entity dump ran 27.1 G instead
    // of 7.4 G instructions. A recorded triple remains valid regardless of
    // how the graph grows -- a fact node's id IS the hash of its triple -- so
    // the store continues to answer for what fact() wrote before the import
    // and after it, and the walk reads the node the import wrote.
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node op = z.node("op");

    const Node f        = z.fact(a, op, {b});
    const Node imported = z.fact_import_trusted_single_object(a, op, c);
    REQUIRE(imported != 0);
    CHECK(z.fact_stores_enabled());

    z.set_logging(-1);
    CHECK(has_reading(*get_fact_structures(&z, f, 1), a, b)); // the store
    CHECK(z.genuine_stats().hits == 1);
    CHECK(z.genuine_stats().walks == 0);

    CHECK(has_reading(*get_fact_structures(&z, imported, 1), a, c)); // the walk
    CHECK(z.genuine_stats().walks == 1);

    const Node g = z.fact(b, op, {a}); // created afterwards: recorded
    CHECK(has_reading(*get_fact_structures(&z, g, 1), b, a));
    CHECK(z.genuine_stats().hits == 2);
    CHECK(z.genuine_stats().walks == 1);
}

TEST_CASE("genuine store: growth-only full clears do not disarm")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");

    const Node f = z.fact(a, op, {b});
    CHECK(has_reading(*get_fact_structures(&z, f, 1), a, b));

    // Relation-type declaration: wholesale fs_cache clear -- but the
    // genuine store is growth-immune and must keep answering.
    z.fact(z.node("p2"), z.core.IsA, {z.core.RelationTypeCategory});

    z.set_logging(-1);
    CHECK(has_reading(*get_fact_structures(&z, f, 1), a, b));
    CHECK(z.genuine_stats().hits == 1);  // answered by the store...
    CHECK(z.genuine_stats().walks == 0); // ...not by the walk
}

TEST_CASE("genuine store: a fact in predicate position reconstructs like any other")
{
    // Whatever stands in predicate position is declared a relation type, so
    // the walk can name it. The hard part is the other direction: fact()
    // draws "F -> P" for the predicate and "O -> F" for an object, i.e. the
    // USERS of a predicate land in its left set right next to its objects.
    // Reading a user as another object made the reconstructed triple of
    // (a p b) grow an extra object as soon as someone used it as a
    // predicate -- which is precisely the state a network is in after
    // .load, when the store no longer answers.
    Zelph      z(null_handler());
    const Node a = z.node("a");
    const Node b = z.node("b");
    const Node p = z.node("p");
    const Node x = z.node("x");
    const Node y = z.node("y");

    const Node inner = z.fact(a, p, {b});
    const Node outer = z.fact(x, inner, {y}); // inner is the PREDICATE here

    CHECK(z.parse_relation(outer) == inner);
    CHECK(has_reading(*get_fact_structures(&z, outer, 1), x, y));

    // Disarm, so everything below is the walk. `outer` must not leak into
    // the objects of `inner`.
    z.disable_fact_stores();
    z.set_logging(-1);

    const auto inner_structs = get_fact_structures(&z, inner, 1);
    CHECK(has_reading(*inner_structs, a, b));
    REQUIRE(inner_structs->size() == 1);
    CHECK(inner_structs->front().predicate == p);
    CHECK(inner_structs->front().objects.size() == 1); // b, and nothing else
    CHECK(inner_structs->front().objects.count(outer) == 0);

    CHECK(has_reading(*get_fact_structures(&z, outer, 1), x, y));
    CHECK(z.genuine_stats().hits == 0);
    CHECK(z.genuine_stats().walks > 0);
}

TEST_CASE("command: .fact-stores reports, disarms one-way, reasoning stays correct on the walk path")
{
    using namespace zelph::test;
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process(".fact-stores");
        CHECK(any_output_contains(collector, "Fact-path stores: on"));

        collector.clear();
        interactive.process(".fact-stores off");
        CHECK(any_output_contains(collector, "Fact-path stores: off"));

        // Rules and facts created AFTER the switch have no store entries
        // anywhere; every consumer must fall back to the walks and stay
        // complete (co-pinned suite-wide by `.semi-naive check`).
        process_lines(interactive, R"(
(A foo B) => (A linked B)
p foo q
)");
        CHECK(any_output_starts_with(collector, "( p linked q )"));

        CHECK_THROWS_AS(interactive.process(".fact-stores on"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".fact-stores banana"), std::runtime_error); 

        interactive.process(".new"); // fresh engine instance -> stores re-armed
        collector.clear();
        interactive.process(".fact-stores");
        CHECK(any_output_contains(collector, "Fact-path stores: on")); });
}

TEST_CASE("save/load: a fact in predicate position answers the same on both sides")
{
    using namespace zelph::test;
    namespace fs = std::filesystem;

    // The end-to-end shape of the two mechanisms above. No store records the
    // triple of a loaded node before the walk has read it, so the reloaded
    // network answers through the walk; before the predicate was declared and
    // the users were filtered out, this query answered on the typed network and
    // stayed silent on the reloaded one -- the same file, read back as
    // something else.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto path = fs::temp_directory_path() / "zelph_composite_predicate.bin";

    process_lines(interactive, R"(
a p b
x (a p b) y
)");
    collector.clear();
    interactive.process("X (a p b) Y");
    REQUIRE(answers_contain(collector, "x (a p b) y"));

    interactive.process(".save \"" + path.string() + "\"");
    interactive.process(".new");
    interactive.process(".load \"" + path.string() + "\"");

    collector.clear();
    interactive.process("X (a p b) Y");
    CHECK(answers_contain(collector, "x (a p b) y"));

    // The inner fact must not have grown an object from its own user.
    collector.clear();
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    fs::remove(path);
}

// The stores stay armed through a .load, and an armed variable store answers
// "no variable" via ABSENCE. No fact() call wrote a loaded node, so that
// absence applies only where the file's record of the store says so: kept
// armed without the record, and without telling the loaded nodes apart, the
// loaded rule below answered its own condition `(X zp Y) zr c` to `A zr B`
// as data, and after the load it derived nothing from new data --
// `(e zp f) zr c` left `e zq f` underived, since the condition was read as
// ground and matched only itself. A file containing the record draws its
// answers from it; a file lacking it, where its network holds a variable as
// this one does, has its loaded nodes read by the walk, and whatever the
// session writes afterwards by the store.
TEST_CASE("save/load: a loaded rule is no data and keeps deriving while the stores stay armed")
{
    using namespace zelph::test;
    namespace fs = std::filesystem;

    const auto path  = fs::temp_directory_path() / "zelph_loaded_rule_armed_stores.bin";
    const auto older = fs::temp_directory_path() / "zelph_loaded_rule_armed_stores_older.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((X zp Y) zr c) => (X zq Y)\n(a zp b) zr c");
        interactive.process(".save \"" + path.string() + "\"");
    }
    write_without_directory(path, older, true);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    SUBCASE("a file with the record of the variable store")
    {
        interactive.process(".load \"" + path.string() + "\"");
        REQUIRE_FALSE(interactive.graph()->may_hold_untracked_nodes());
    }
    SUBCASE("a file without it")
    {
        interactive.process(".load \"" + older.string() + "\"");
        REQUIRE(interactive.graph()->may_hold_untracked_nodes());
    }
    fs::remove(path);
    fs::remove(older);
    REQUIRE(interactive.graph()->fact_stores_enabled());

    collector.clear();
    interactive.process("A zr B");
    CHECK(collect_answers(collector) == std::vector<std::string>{"(a zp b) zr c"});

    process_lines(interactive, "(e zp f) zr c\n.run");
    collector.clear();
    interactive.process("A zq B");
    auto derived = collect_answers(collector);
    std::ranges::sort(derived);
    CHECK(derived == std::vector<std::string>{"a zq b", "e zq f"});
}

// The discard of a load also empties the genuine-structure store, even though
// an entry within it remains valid for its node: following the load of a file
// lacking the record, an entry says that the stores recorded the node's
// variables (Impl::recorded_by_stores), and a loaded node retains the id that
// the same statement held in the session before the load. A session that
// holds a rule and loads a network holding the same rule would find the
// loaded patterns "recorded", with no trace of them in the variable store,
// which the discard has erased: had the entries been preserved, the pattern
// `(X zp Y) zr c` would have answered `A zr B` as data, and the rule would
// have derived nothing from a fact written after the load.
TEST_CASE("save/load: a rule that the session held before it loads the same rule from a file without the record is no data")
{
    using namespace zelph::test;
    namespace fs = std::filesystem;

    const auto path  = fs::temp_directory_path() / "zelph_reloaded_rule.bin";
    const auto older = fs::temp_directory_path() / "zelph_reloaded_rule_older.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((X zp Y) zr c) => (X zq Y)\n(a zp b) zr c");
        interactive.process(".save \"" + path.string() + "\"");
    }
    write_without_directory(path, older, true);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, "((X zp Y) zr c) => (X zq Y)\n(a zp b) zr c");
    interactive.process(".load \"" + older.string() + "\"");
    fs::remove(path);
    fs::remove(older);
    REQUIRE(interactive.graph()->may_hold_untracked_nodes());

    collector.clear();
    interactive.process("A zr B");
    CHECK(collect_answers(collector) == std::vector<std::string>{"(a zp b) zr c"});

    process_lines(interactive, "(e zp f) zr c\n.run");
    collector.clear();
    interactive.process("A zq B");
    auto derived = collect_answers(collector);
    std::ranges::sort(derived);
    CHECK(derived == std::vector<std::string>{"a zq b", "e zq f"});
}

// A node that fact() did not write -- specifically, one a .load read -- is
// read by the walk, and the structure cache keeps that reading solely until
// its next wholesale clear, triggered by a relation-type declaration. Re-read,
// the node was walked once more: over a math stdlib loaded from a file
// containing the variable store's record, the Jacobian workload walked 178,010
// structures and ran 11.0 G user instructions instead of 7.4 G when run
// sequentially; with the walked structures kept, it now walks 2,800 and runs
// 7.5 G. A walk whose one reading hashes back to the node has read the node's
// own triple, the one fact() records, and the store keeps it: the second read
// constitutes a hit. A session with stores disabled maintains no such entry,
// representing the memory switch, nor does one that loaded a file lacking the
// record, where an entry in this store says that the stores had recorded the
// node's variables as well (Impl::recorded_by_stores): if kept, the loaded
// rule from the earlier test would answer its own condition as data once a
// query had walked it.
TEST_CASE("genuine store: a loaded node the walk has read is answered by the store")
{
    namespace fs     = std::filesystem;
    const auto path  = fs::temp_directory_path() / "zelph_genuine_adopted.bin";
    const auto older = fs::temp_directory_path() / "zelph_genuine_adopted_older.bin";
    {
        Zelph z(null_handler());
        z.fact(z.node("a"), z.node("p"), {z.node("b")});
        // A pattern in addition to the fact: a network lacking a variable
        // would load with the variable store complete regardless of what
        // its file records.
        z.fact(z.var(), z.node("p"), {z.node("b")});
        z.save_to_file(path.string());
    }
    zelph::test::write_without_directory(path, older, true);

    Zelph    z(null_handler());
    uint64_t walks_expected = 1;
    SUBCASE("a file with the record of the variable store")
    {
        z.load_from_file(path.string());
    }
    SUBCASE("a file without it")
    {
        z.load_from_file(older.string());
        walks_expected = 2;
    }
    SUBCASE("a session whose stores are off")
    {
        z.load_from_file(path.string());
        z.disable_fact_stores();
        walks_expected = 2;
    }
    fs::remove(path);
    fs::remove(older);

    const Node f = Zelph::create_hash(z.get_node("p"), z.get_node("a"), adjacency_set{z.get_node("b")});
    REQUIRE(z.exists(f));
    z.set_logging(-1);
    CHECK(has_reading(*get_fact_structures(&z, f, 1), z.get_node("a"), z.get_node("b")));
    z.fact(z.node("p2"), z.core.IsA, {z.core.RelationTypeCategory}); // clears the structure cache
    CHECK(has_reading(*get_fact_structures(&z, f, 1), z.get_node("a"), z.get_node("b")));
    CHECK(z.genuine_stats().walks == walks_expected);
}

TEST_CASE("genuine store: a scratch cluster the engine discards does not disarm it")
{
    // zelph/dedup-rule builds every parsed rule inside a scratch cluster and
    // drops it again when the rule turns out to exist; .explain does the same
    // with its pattern. That drop went through remove_node, i.e. through the
    // WHOLESALE funnel -- so re-entering a rule that was already there cost
    // the session its genuine-structure store, on a four-fact graph as much
    // as on a Wikidata network. Same defect as the rule-pattern revocation
    // above, one call site further out.
    Zelph      z(null_handler());
    const Node a = z.node("a");
    const Node p = z.node("p");

    REQUIRE(z.fact_stores_enabled());

    z.set_active_cluster("__scratch");
    const Node x       = z.var();
    const Node y       = z.var();
    const Node pattern = z.fact(x, p, {y});
    z.deactivate_cluster();
    REQUIRE(z.exists(pattern));

    CHECK(z.drop_scratch_cluster("__scratch") > 0);

    // What the drop has to achieve is unchanged: the scratch is gone.
    CHECK_FALSE(z.exists(pattern));
    CHECK_FALSE(z.exists(x));
    CHECK_FALSE(z.exists(y));

    // ...and the stores are still authoritative, which is the point.
    CHECK(z.fact_stores_enabled());

    z.set_logging(-1); // counter-only mode
    z.reset_genuine_stats();
    const Node e     = z.node("e");
    const Node later = z.fact(a, p, {e});
    CHECK(has_reading(*get_fact_structures(&z, later, 1), a, e));
    CHECK(z.genuine_stats().hits == 1);
    CHECK(z.genuine_stats().walks == 0);
}

TEST_CASE("genuine store: a scratch node something was built on takes the general path")
{
    // The fallback, and it is not optional: the leaf removal skips the
    // upward cascade, so a member that something OUTSIDE the cluster was
    // built on has to send the whole drop back to drop_cluster -- disarm
    // included. An optimisation of a removal, never a weakening of one.
    Zelph      z(null_handler());
    const Node p = z.node("p");
    const Node q = z.node("q");

    z.set_active_cluster("__scratch");
    const Node x       = z.var();
    const Node y       = z.var();
    const Node pattern = z.fact(x, p, {y});
    z.deactivate_cluster();

    // Built OUTSIDE the cluster, on a node inside it.
    const Node about = z.fact(pattern, q, {z.node("noted")});
    REQUIRE(z.exists(about));

    CHECK(z.drop_scratch_cluster("__scratch") > 0);

    // The cascade ran: what stood on the removed pattern is gone with it.
    CHECK_FALSE(z.exists(pattern));
    CHECK_FALSE(z.exists(about));
}
