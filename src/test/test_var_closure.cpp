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

#include "directory_rewrite.hpp"
#include "io/output.hpp"
#include "network/reasoning.hpp"
#include "network/zelph.hpp"
#include "test_helpers.hpp"
#include "wikidata_dump.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

using namespace zelph::network;

// ---------------------------------------------------------------------------
// Variable-closure flag (Zelph::var_in_closure): the O(1) replacement for
// the reconstruction-based contains_variable_deep walk. Pins: (1) exact
// flag semantics across positions and depths, maintained bottom-up in
// fact(); (2) the store remains complete along the paths that write nodes
// around fact() -- a trusted import enters its triple, a load enters the
// record that the save wrote into the file; (3) the per-node fallback -- a
// node loaded from a file lacking that record has no store entry, and its
// absence must not be interpreted as "no variable": such a node provides
// answers through the historical walk, while the store supplies answers for
// every node that fact() records, both before and following the load.
// End-to-end template rejection stays pinned by the existing suite
// (junk-fact regressions in test_seminaive.cpp), which now runs through the
// flag.
// ---------------------------------------------------------------------------

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }

    std::unordered_set<Node> vars_of(Zelph& z, const Node n)
    {
        std::unordered_set<Node> vs;
        std::vector<Node>        history;
        collect_variables(&z, n, vs, 1, history);
        return vs;
    }
} // namespace

TEST_CASE("var_in_closure: exact flag across positions and depths")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");
    const Node V  = z.var();

    CHECK_FALSE(z.var_in_closure(a));
    CHECK(z.var_in_closure(V));
    CHECK_FALSE(z.var_in_closure(0));

    const Node data = z.fact(a, op, {b});
    CHECK_FALSE(z.var_in_closure(data));

    const Node tmpl = z.fact(V, op, {b}); // var in subject position
    CHECK(z.var_in_closure(tmpl));

    // The flag must propagate bottom-up through parents (depths 2 and 3).
    const Node mid = z.fact(tmpl, op, {a});
    CHECK(z.var_in_closure(mid));
    const Node outer = z.fact(mid, op, {b});
    CHECK(z.var_in_closure(outer));

    const Node t2 = z.fact(a, op, {V}); // var in object position
    CHECK(z.var_in_closure(t2));
    const Node t3 = z.fact(a, V, {b}); // variable relation
    CHECK(z.var_in_closure(t3));

    // Nesting DATA inside data stays unflagged.
    const Node data2 = z.fact(data, op, {b});
    CHECK_FALSE(z.var_in_closure(data2));
}

// A load writes every node without fact(), and the stores previously used to
// be disarmed by it for the rest of the session. They stay armed now, and a
// file lacking the record of the variable store -- every network published
// before the record's existence -- leaves the loaded nodes without entries.
// Read as "no variable", that absence made the loaded rule of a test in
// test_genuine_structure.cpp answer its condition as data and stop deriving.
// The loaded nodes are read by the walk; what the session writes afterwards,
// including over loaded nodes, is read by the store.
TEST_CASE("var_in_closure: a node loaded from a file without the store's record holds its variable")
{
    namespace fs     = std::filesystem;
    const auto saved = fs::temp_directory_path() / "zelph_var_closure_loaded_saved.bin";
    const auto older = fs::temp_directory_path() / "zelph_var_closure_loaded_older.bin";
    Node       tmpl = 0, data = 0, op = 0, b = 0;
    {
        Zelph z(null_handler());
        b            = z.node("b");
        op           = z.node("op");
        const Node a = z.node("a");
        const Node V = z.var();
        tmpl         = z.fact(V, op, {b});
        data         = z.fact(a, op, {b});
        z.save_to_file(saved.string());
    }
    zelph::test::write_without_directory(saved, older, true);

    Zelph z(null_handler());
    z.load_from_file(older.string());
    fs::remove(saved);
    fs::remove(older);
    REQUIRE(z.fact_stores_enabled());
    REQUIRE(z.may_hold_untracked_nodes());
    REQUIRE(z.exists(tmpl));

    z.set_logging(-1);
    CHECK(z.var_in_closure(tmpl)); // the walk
    CHECK_FALSE(z.var_in_closure(data));
    CHECK(z.var_closure_stats().walk_fallbacks == 2);

    // Written after the load: fact() reads its loaded component via the walk,
    // and the store answers for the node it records.
    const Node over  = z.fact(tmpl, op, {b});
    const Node fresh = z.fact(z.node("c"), op, {b});
    const auto walks = z.var_closure_stats().walk_fallbacks;
    CHECK(z.var_in_closure(over));
    CHECK_FALSE(z.var_in_closure(fresh));
    CHECK(z.var_closure_stats().walk_fallbacks == walks);
}

// A network in which no node is a variable holds no node with one, which
// characterizes a network of data. Loaded whole from a file lacking the
// record, it leaves the store complete: the load reads the ids of its nodes
// anyway, for the kinds of pattern such a file does not record either, and
// where none of them is a variable, an empty store speaks for every node
// (Zelph::note_loaded_kinds). Left in the state of the test above, each
// loaded node would be asked by the walk, and a save would leave the record
// out again. A view does not count, nor does a load that broke off: a node
// that was read may name one that was not, so the nodes read say nothing
// about the rest.
TEST_CASE("var_in_closure: a network without a variable, loaded whole from a file without the store's record, leaves the store complete")
{
    namespace fs     = std::filesystem;
    const auto saved = fs::temp_directory_path() / "zelph_var_closure_data_saved.bin";
    const auto older = fs::temp_directory_path() / "zelph_var_closure_data_older.bin";
    Node       data = 0, nested = 0;
    {
        Zelph      z(null_handler());
        const Node op = z.node("op");
        data          = z.fact(z.node("a"), op, {z.node("b")});
        nested        = z.fact(data, op, {z.node("c")});
        z.save_to_file(saved.string());
    }
    zelph::test::write_without_directory(saved, older, true);
    REQUIRE(zelph::test::variable_record_entries(older) == -1);

    Zelph z(null_handler());
    bool  complete = false;
    SUBCASE("the whole file")
    {
        z.load_from_file(older.string());
        complete = true;
    }
    SUBCASE("a view of it")
    {
        Zelph::BinChunkSelection left_side_only;
        left_side_only.right_explicit = true;
        z.load_from_file(older.string(), left_side_only, false);
    }
    SUBCASE("a load that broke off")
    {
        fs::resize_file(older, fs::file_size(older) - 8);
        CHECK_THROWS(z.load_from_file(older.string()));
    }
    fs::remove(saved);
    fs::remove(older);
    REQUIRE(z.fact_stores_enabled());
    REQUIRE(z.exists(data));
    CHECK(z.may_hold_untracked_nodes() == !complete);

    if (complete)
    {
        z.set_logging(-1);
        CHECK_FALSE(z.var_in_closure(data));
        CHECK_FALSE(z.var_in_closure(nested));
        CHECK(z.var_closure_stats().walk_fallbacks == 0);
    }
}

// A set pattern built outside a rule takes its id from the node counter,
// allowing two networks to hold distinct patterns under a single id, and the
// statements over them consequently share that same id as well. The discard
// of a load therefore clears both stores: kept, the entries of the session's
// `{V b} q r` answered for the loaded `{W c} q r`, which named a variable
// the loaded network does not hold, or, with the entry of the
// genuine-structure store alone, no variable at all.
TEST_CASE("var_in_closure: a load forgets what the stores held about an id that the loaded network gives another statement")
{
    namespace fs     = std::filesystem;
    const auto saved = fs::temp_directory_path() / "zelph_var_closure_same_id_saved.bin";
    const auto older = fs::temp_directory_path() / "zelph_var_closure_same_id_older.bin";
    Node       W = 0, loaded = 0;
    {
        Zelph z(null_handler());
        W                  = z.var();
        const Node pattern = z.set({W, z.node("c")});
        loaded             = z.fact(pattern, z.node("q"), {z.node("r")});
        z.save_to_file(saved.string());
    }
    zelph::test::write_without_directory(saved, older, true);

    Zelph z(null_handler());
    z.var();
    const Node V       = z.var();
    const Node pattern = z.set({V, z.node("b")});
    const Node held    = z.fact(pattern, z.node("q"), {z.node("r")});
    REQUIRE(held == loaded); // one id, two statements
    REQUIRE(V != W);
    REQUIRE(vars_of(z, held) == std::unordered_set<Node>{V});

    z.load_from_file(older.string());
    fs::remove(saved);
    fs::remove(older);
    CHECK(vars_of(z, loaded) == std::unordered_set<Node>{W});
    CHECK(z.var_in_closure(loaded));
}

// The same network from a file that carries the record: the load enters it,
// and the store answers for the loaded nodes just as it does for those
// fact() wrote, without a walk. The variables themselves are restored as
// well, so a statement written over a loaded pattern holds them.
TEST_CASE("var_in_closure: a node loaded from a file with the store's record is answered by the store")
{
    namespace fs    = std::filesystem;
    const auto path = fs::temp_directory_path() / "zelph_var_closure_loaded_record.bin";
    Node       tmpl = 0, data = 0, op = 0, b = 0, V = 0;
    {
        Zelph z(null_handler());
        b            = z.node("b");
        op           = z.node("op");
        const Node a = z.node("a");
        V            = z.var();
        tmpl         = z.fact(V, op, {b});
        data         = z.fact(a, op, {b});
        z.save_to_file(path.string());
    }
    CHECK(zelph::test::variable_record_entries(path) == 1);

    Zelph z(null_handler());
    z.load_from_file(path.string());
    fs::remove(path);
    REQUIRE(z.fact_stores_enabled());
    REQUIRE_FALSE(z.may_hold_untracked_nodes());

    z.set_logging(-1);
    CHECK(z.var_in_closure(tmpl));
    CHECK_FALSE(z.var_in_closure(data));
    const Node over = z.fact(tmpl, op, {b});
    CHECK(vars_of(z, over) == std::unordered_set<Node>{V});
    CHECK(z.var_closure_stats().walk_fallbacks == 0);
    CHECK(z.template_vars_stats().walks == 0);
}

// A rule whose conditions sit in a conjunction set and whose consequence is
// ground holds its variables solely within its text, which the store records
// independently of the variable sets (Impl's _rule_text_vars), and so does
// every fact written over such a rule, one of which is a mention. Loaded from a
// file lacking the record, the rule possesses no entry, and a fact written over
// it post-load queries the walk to determine whether the rule's text holds a
// variable: without that walk, the mention was interpreted as data, although
// the rule it refers to is not a claim.
//
// From a file containing the record, the rule's entry is restored upon load,
// and the store must be informed once more that it holds such entries (Impl's
// _rule_text_vars_any, which readers query before acquiring the lock): if
// left unset, the mention is read as data there as well.
TEST_CASE("var_in_closure: a fact over a loaded rule holds the rule's text variable, with the store's record and without it")
{
    namespace fs     = std::filesystem;
    const auto saved = fs::temp_directory_path() / "zelph_var_closure_rule_text_saved.bin";
    const auto older = fs::temp_directory_path() / "zelph_var_closure_rule_text_older.bin";
    Node       rule  = 0;
    {
        Zelph      z(null_handler());
        const Node zr  = z.node("zr");
        const Node X   = z.var();
        const Node Y   = z.var();
        const Node Z   = z.var();
        const Node set = z.conjunction_collection({z.fact(X, zr, {Y}), z.fact(Y, zr, {Z})});
        z.fact(set, z.core.IsA, {z.core.Conjunction});
        rule = z.fact(set, z.core.Causes, {z.fact(z.node("c"), z.node("zs"), {z.node("d")})});
        REQUIRE(z.var_in_closure(rule));
        REQUIRE_FALSE(z.var_in_closure(rule, Zelph::VariableReading::Instance));
        z.save_to_file(saved.string());
    }
    zelph::test::write_without_directory(saved, older, true);

    Zelph z(null_handler());
    SUBCASE("a file with the record")
    {
        z.load_from_file(saved.string());
        REQUIRE_FALSE(z.may_hold_untracked_nodes());
    }
    SUBCASE("a file without it")
    {
        z.load_from_file(older.string());
        REQUIRE(z.may_hold_untracked_nodes());
    }
    fs::remove(saved);
    fs::remove(older);

    const Node mention = z.fact(rule, z.node("noted"), {z.node("yes")});
    CHECK(z.var_in_closure(mention));
    CHECK_FALSE(z.var_in_closure(mention, Zelph::VariableReading::Instance));
}

// A set pattern built outside a rule, `{V b}` here, is an atom that holds the
// variables of its members, and a statement over it also holds them
// (Zelph::set_pattern_variables). A load clears the record of such patterns,
// which is repopulated only upon the first request for a pattern's variables,
// so fact() would skip the pattern as part of a statement written after the
// load, and the statement would be read as data from the store, which stays
// armed. The load says whether its graph may hold such a pattern -- the
// file's header does, or, for a file lacking that field, the ids the load
// read -- and fact() then asks every atom of that class.
TEST_CASE("template vars: a statement over a loaded set pattern holds the pattern's variables")
{
    namespace fs       = std::filesystem;
    const auto saved   = fs::temp_directory_path() / "zelph_var_closure_set_pattern_saved.bin";
    const auto older   = fs::temp_directory_path() / "zelph_var_closure_set_pattern_older.bin";
    const auto again   = fs::temp_directory_path() / "zelph_var_closure_set_pattern_again.bin";
    Node       pattern = 0, V = 0;
    {
        Zelph z(null_handler());
        V       = z.var();
        pattern = z.set({V, z.node("b")});
        REQUIRE(z.set_pattern_variables(pattern) != nullptr);
        z.save_to_file(saved.string());
    }
    zelph::test::write_without_directory(saved, older, true);

    Zelph z(null_handler());
    SUBCASE("a file that records the kinds of pattern it holds")
    {
        z.load_from_file(saved.string());
    }
    SUBCASE("a file without that record")
    {
        z.load_from_file(older.string());
    }
    SUBCASE("a file that a session saved after it had loaded the pattern")
    {
        // The session that saves holds the pattern solely in its loaded
        // form, and its file must still indicate this.
        {
            Zelph between(null_handler());
            between.load_from_file(saved.string());
            between.save_to_file(again.string());
        }
        z.load_from_file(again.string());
        fs::remove(again);
    }
    fs::remove(saved);
    fs::remove(older);
    REQUIRE(z.fact_stores_enabled());

    const Node over = z.fact(pattern, z.node("op"), {z.node("c")});
    CHECK(z.var_in_closure(over));
    CHECK(vars_of(z, over) == std::unordered_set<Node>{V});
}

// A condition `X in {b c}` writes the membership of a variable within the set
// constant, while a condition `(X op b) in {b c}` writes the membership of a
// statement containing one: patterns applied to the set, not its components.
// The flag store never counted them as part of the set's text, yet the walk
// did, so after the store was disarmed, the set read as a rule template, and
// unification rejected every membership within it as a candidate. A set
// constant around a rule's own collection that contains a variable, `{@{Y}}`,
// constitutes rule text in both readings.
TEST_CASE("var_in_closure: the walk reads a set constant's own members, not a pattern's membership in it")
{
    Zelph      z(null_handler());
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node op = z.node("op");
    const Node V  = z.var();

    const Node set = z.set({b, c});
    REQUIRE(z.is_set_constant(set));

    Node own_set = 0;
    {
        const Zelph::TemplateScope scope(z);
        own_set = z.set({z.collection({V})});
    }

    SUBCASE("the membership of a variable")
    {
        z.fact(V, z.core.PartOf, {set});
    }
    SUBCASE("the membership of a statement holding a variable")
    {
        z.fact(z.fact(V, op, {b}), z.core.PartOf, {set});
    }

    CHECK_FALSE(z.var_in_closure(set)); // the flag store
    CHECK(z.var_in_closure(own_set));

    z.disable_fact_stores();

    CHECK_FALSE(z.var_in_closure(set)); // the walk
    CHECK(z.var_in_closure(own_set));
}

TEST_CASE("template vars: exact variable sets across positions and depths")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");
    const Node V  = z.var();
    const Node W  = z.var();

    z.set_logging(-1);

    const Node data = z.fact(a, op, {b});
    CHECK(vars_of(z, data).empty());
    CHECK(vars_of(z, a).empty());
    CHECK(vars_of(z, V).count(V) == 1);

    const Node tmpl = z.fact(V, op, {b});
    {
        const auto vs = vars_of(z, tmpl);
        CHECK(vs.size() == 1);
        CHECK(vs.count(V) == 1);
    }

    // Union across positions, propagated bottom-up through parents.
    const Node two = z.fact(tmpl, op, {W});
    {
        const auto vs = vars_of(z, two);
        CHECK(vs.size() == 2);
        CHECK(vs.count(V) == 1);
        CHECK(vs.count(W) == 1);
    }
    const Node deep = z.fact(two, op, {a});
    {
        const auto vs = vars_of(z, deep);
        CHECK(vs.count(V) == 1);
        CHECK(vs.count(W) == 1);
    }

    CHECK(z.template_vars_stats().walks == 0); // never fell back to the walk
}

// A trusted import writes its triple around fact(), and previously it would
// disarm both stores for the rest of the session. It now keeps them armed and
// enters its triple into the variable store as fact() would: the union of
// what its components hold. Without this entry, the store, complete for every
// other node, would read the absence as "no variable", and an imported
// statement over a rule's pattern would answer as data. The nodes fact()
// recorded, both before and after the import, continue to answer from the
// store.
TEST_CASE("template vars: a trusted import enters its triple as fact() would")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node op = z.node("op");
    const Node V  = z.var();
    const Node W  = z.var();

    const Node tmpl   = z.fact(V, op, {b});
    const Node parent = z.fact(tmpl, op, {a});

    const Node data      = z.fact_import_trusted_single_object(a, op, c);
    const Node over_tmpl = z.fact_import_trusted_single_object(tmpl, op, c);
    const Node over_var  = z.fact_import_trusted_single_object(a, op, W);
    const Node over_both = z.fact_import_trusted_single_object(over_tmpl, op, W);
    REQUIRE(z.fact_stores_enabled());
    REQUIRE_FALSE(z.may_hold_untracked_nodes());

    z.set_logging(-1);
    CHECK(vars_of(z, parent) == std::unordered_set<Node>{V});
    CHECK(vars_of(z, data).empty());
    CHECK_FALSE(z.var_in_closure(data));
    CHECK(vars_of(z, over_tmpl) == std::unordered_set<Node>{V});
    CHECK(vars_of(z, over_var) == std::unordered_set<Node>{W});
    CHECK(vars_of(z, over_both) == std::unordered_set<Node>{V, W});
    CHECK(z.var_in_closure(over_tmpl));

    const Node tmpl2 = z.fact(V, op, {c}); // generated following the imports
    CHECK(vars_of(z, tmpl2) == std::unordered_set<Node>{V});
    CHECK(z.template_vars_stats().walks == 0);
    CHECK(z.var_closure_stats().walk_fallbacks == 0);
}

// ---------------------------------------------------------------------------
// The readers that enumerate the variable store (Zelph::unasserted_snapshot:
// `.list-predicate-usage`, `.list-predicate-value-usage`, get_fact_objects
// and get_fact_subjects, the closures, `.explain`, `.save-predicates`)
// require it to be complete: a node that holds a variable and possesses no
// entry is treated as a claim. Following each load, trusted import, and name
// merge, the store held none of the loaded, imported, or rebuilt patterns,
// and these readers counted the rules as data: `(X zp Y) => (X zq Y)` with
// `a zp b` listed `=> 0`, `zp 1`, `zq 1` before `.save` and `=> 1`, `zp 2`,
// `zq 2` after `.load`. The cases below compare the listing before and after
// each path. The second rule's conditions hold its variables while its
// consequence holds none, a distinction the store records independently of
// the variable sets (Impl's _rule_text_vars), and the record carries both.
// ---------------------------------------------------------------------------

namespace
{
    // `.list-predicate-usage` as predicate -> count: the initial and final
    // columns, because a network that holds Wikidata names lists them between
    // those two.
    std::map<std::string, std::string> predicate_usage(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process(".list-predicate-usage");
        std::map<std::string, std::string> usage;
        int                                rules = 0;
        for (const auto& event : collector.events())
        {
            if (event.text.rfind("------", 0) == 0)
                ++rules;
            else if (rules == 1 && event.text.find('\t') != std::string::npos)
                usage[event.text.substr(0, event.text.find('\t'))] = event.text.substr(event.text.rfind('\t') + 1);
        }
        return usage;
    }

    // The predicates from the rules and the data beneath, each accompanied by
    // its respective count.
    std::map<std::string, std::string> of_rules(const std::map<std::string, std::string>& usage)
    {
        std::map<std::string, std::string> kept;
        for (const char* const p : {"=>", "zp", "zq", "zr", "zs", "zt"})
            if (const auto it = usage.find(p); it != usage.end()) kept.insert(*it);
        return kept;
    }

    constexpr const char* usage_rules = "(X zp Y) => (X zq Y)\n"
                                        "(X zr Y, Y zr Z) => (c zs d)\n"
                                        "a zp b\n"
                                        "e zr f\n"
                                        "f zr g";

    const std::map<std::string, std::string> usage_of_rules{{"=>", "0"}, {"zp", "1"}, {"zq", "1"}, {"zr", "2"}, {"zs", "1"}};
}

// The store skips a statement consisting solely of counter ids, which is
// every data triple of a bulk import, and a `=>` fact is exempt from that
// (Zelph::enter_template_vars). This exemption ensures the most frequent
// contradiction rule remains a rule: the comma list of its conditions is
// a collection with a counter id, `=>` and `!` are core nodes, and its
// variables reside exclusively within its text, which the store records
// for the rule itself (Impl's _rule_text_vars). If treated like a data
// triple, the rule would lack an entry and count as a claim:
// `.list-predicate-usage` would list it under `=>`, and `A => B` would
// yield it as an answer.
TEST_CASE("template vars: a contradiction rule over a comma list is entered although its statement holds counter ids alone")
{
    using namespace zelph::test;

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, "(X zr Y, Y zr X) => !\na zr b");

    CHECK(of_rules(predicate_usage(collector, interactive)) == std::map<std::string, std::string>{{"=>", "0"}, {"zr", "1"}});

    collector.clear();
    interactive.process("A => B");
    CHECK(collect_answers(collector).empty());
}

TEST_CASE("variable record: .list-predicate-usage lists the same after .save and .load")
{
    using namespace zelph::test;
    namespace fs    = std::filesystem;
    const auto path = fs::temp_directory_path() / "zelph_usage_round_trip.bin";

    std::map<std::string, std::string> before;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, usage_rules);
        before = predicate_usage(collector, interactive);
        interactive.process(".save \"" + path.string() + "\"");
    }
    REQUIRE(of_rules(before) == usage_of_rules);
    CHECK(variable_record_entries(path) > 0);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + path.string() + "\"");
    fs::remove(path);
    CHECK(predicate_usage(collector, interactive) == before);
}

TEST_CASE("variable record: .list-predicate-usage lists the same after a trusted import")
{
    using namespace zelph::test;
    using namespace zelph::test::wikidata_dump;

    // Within a graph that holds more than just the core nodes, `.load` of a
    // dump brings it in via the trusted path instead of replacing the graph.
    const auto dump = write_lines("zelph_usage_trusted_import", {entity("Q1", text("en", "one"), claim("P279", "Q2")), entity("Q2", text("en", "two"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, usage_rules);
    REQUIRE(of_rules(predicate_usage(collector, interactive)) == usage_of_rules);

    interactive.process(".load \"" + dump.string() + "\"");
    remove_dump(dump);
    REQUIRE(interactive.graph()->fact_stores_enabled());
    REQUIRE(interactive.graph()->get_node("Q1", "wikidata") != 0);

    CHECK(of_rules(predicate_usage(collector, interactive)) == usage_of_rules);
}

TEST_CASE("variable record: .list-predicate-usage lists the same after a name merge")
{
    using namespace zelph::test;

    // The merge rebuilds both rules atop the merged atom, and each rebuilt
    // node assumes the entries of the node it replaces. The third rule has
    // the atom in its consequence alone, thus its `=>` fact is rebuilt, and
    // that fact holds its variables within its text: the entry it assumes
    // is the one from the rule text, without which the listing would have
    // counted the rule under `=>`.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, "((foo zp X) zr c) => (X zq foo)\n(X zr Y, Y zr foo) => (c zs d)\n(X zr Y, Y zr X) => (foo zs d)\nbar zt b\na zp b");
    const auto before = predicate_usage(collector, interactive);
    REQUIRE(before.at("=>") == "0");

    interactive.process(".name bar zelph foo");
    REQUIRE(interactive.graph()->fact_stores_enabled());
    CHECK(predicate_usage(collector, interactive) == before);
}

// Following the load of a file that lacks the record and whose network holds
// a rule, the store speaks for no loaded node, and it continues to record
// every node that the session writes. The readers that enumerate it also read
// those entries in that state: a rule typed after the load contributes no
// data to the listing, whereas the patterns of the loaded rule, which possess
// no entry, are counted, as the file leaves them. Read only when the store is
// complete, the entries were excluded, and the listing counted `(X ys Y)`
// beside `g ys h`.
TEST_CASE("variable record: a rule typed after the load of a file without the record is no data to the listing")
{
    using namespace zelph::test;
    namespace fs     = std::filesystem;
    const auto path  = fs::temp_directory_path() / "zelph_usage_typed_after_load.bin";
    const auto older = fs::temp_directory_path() / "zelph_usage_typed_after_load_older.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((X zp Y) zr c) => (X zq Y)\n(a zp b) zr c");
        interactive.process(".save \"" + path.string() + "\"");
    }
    write_without_directory(path, older, true);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + older.string() + "\"");
    fs::remove(path);
    fs::remove(older);
    REQUIRE(interactive.graph()->may_hold_untracked_nodes());

    process_lines(interactive, "(X ys Y) => (X yt Y)\ng ys h");
    const auto usage = predicate_usage(collector, interactive);
    CHECK(usage.at("ys") == "1");
    CHECK(usage.at("yt") == "0");
}

// The record claims that the store speaks for every node in the file, and a
// load encountering it answers "no variable" for every node it lacks. A
// session whose store does not speak for every node consequently saves no
// record: one that loaded a file lacking it (the loaded nodes possess no
// entries), and one whose stores are off (`.fact-stores off`, or a removal).
// Written regardless, the record of the first lacked the loaded rule, and a
// load of the saved file answered the rule's condition `(X zp Y) zr c` to
// `A zr B` as data. Without the record, the file loads with its nodes read by
// the walk.
TEST_CASE("variable record: a save leaves it out while the store does not speak for every node")
{
    using namespace zelph::test;
    namespace fs     = std::filesystem;
    const auto path  = fs::temp_directory_path() / "zelph_record_left_out.bin";
    const auto older = fs::temp_directory_path() / "zelph_record_left_out_older.bin";
    const auto again = fs::temp_directory_path() / "zelph_record_left_out_again.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((X zp Y) zr c) => (X zq Y)\n(a zp b) zr c");
        interactive.process(".save \"" + path.string() + "\"");
    }
    REQUIRE(variable_record_entries(path) > 0);
    write_without_directory(path, older, true);
    REQUIRE(variable_record_entries(older) == -1);

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        SUBCASE("a session that loaded a file without the record")
        {
            interactive.process(".load \"" + older.string() + "\"");
            REQUIRE(interactive.graph()->may_hold_untracked_nodes());
        }
        SUBCASE("a session whose stores are off")
        {
            interactive.process(".load \"" + path.string() + "\"");
            interactive.process(".fact-stores off");
        }
        interactive.process(".save \"" + again.string() + "\"");
    }
    CHECK(variable_record_entries(again) == -1);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + again.string() + "\"");
    fs::remove(path);
    fs::remove(older);
    fs::remove(again);
    collector.clear();
    interactive.process("A zr B");
    CHECK(collect_answers(collector) == std::vector<std::string>{"(a zp b) zr c"});
}

// A file in which no node is a variable gets the record regardless of the
// state of the store that saves it, and the record is empty: since no node
// within the file holds a variable, the load of it can answer "no variable"
// for each node (Impl::saveToFile). The sessions that reach it are those
// whose stores are off, either through the switch or due to a removal, and
// pruning by a pattern is the method used to reduce a network of data before
// its publication: once the prune concludes, the pattern is no longer
// present, and without the record, every load of the pruned network would ask
// its nodes individually. The file subsequently loads with the store
// complete.
TEST_CASE("variable record: a network without a variable is saved with an empty record while the stores are off")
{
    using namespace zelph::test;
    namespace fs    = std::filesystem;
    const auto path = fs::temp_directory_path() / "zelph_record_of_data.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "a zp b\nc zq d");
        SUBCASE("switched off")
        {
            interactive.process(".fact-stores off");
        }
        SUBCASE("after a prune by a pattern")
        {
            interactive.process(".prune-facts A zq B");
        }
        REQUIRE_FALSE(interactive.graph()->fact_stores_enabled());
        interactive.process(".save \"" + path.string() + "\"");
    }
    CHECK(variable_record_entries(path) == 0);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + path.string() + "\"");
    fs::remove(path);
    CHECK(interactive.graph()->fact_stores_enabled());
    CHECK_FALSE(interactive.graph()->may_hold_untracked_nodes());
    collector.clear();
    interactive.process("A zp B");
    CHECK(collect_answers(collector) == std::vector<std::string>{"a zp b"});
}

// A view names nodes it does not hold, so the ids of the nodes it does
// hold do not say that none of them holds a variable, and saving it writes
// no record on the strength of them. The session commands refuse to save a
// view; a host of the library can.
TEST_CASE("variable record: a view of a network without a variable is saved without the record")
{
    using namespace zelph::test;
    namespace fs     = std::filesystem;
    const auto saved = fs::temp_directory_path() / "zelph_record_of_view_saved.bin";
    const auto older = fs::temp_directory_path() / "zelph_record_of_view_older.bin";
    const auto again = fs::temp_directory_path() / "zelph_record_of_view_again.bin";
    {
        Zelph z(null_handler());
        z.fact(z.node("a"), z.node("op"), {z.node("b")});
        z.save_to_file(saved.string());
    }
    write_without_directory(saved, older, true);

    // That a file is not a view is what a load relies on when reading the
    // node ids of a file without the record (Zelph::note_loaded_kinds), so
    // the refusal of the session commands is integral to that argument.
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load-partial \"" + older.string() + "\"");
        CHECK_THROWS_WITH(interactive.process(".save \"" + again.string() + "\""), doctest::Contains("Blocked in partial load mode"));
        CHECK_FALSE(fs::exists(again));
    }

    {
        Zelph                    z(null_handler());
        Zelph::BinChunkSelection left_side_only;
        left_side_only.right_explicit = true;
        z.load_from_file(older.string(), left_side_only, false);
        REQUIRE(z.may_hold_untracked_nodes());
        z.save_to_file(again.string());
    }
    CHECK(variable_record_entries(again) == -1);

    // Nor does loading that file ask its nodes: it specifies the kinds of
    // pattern it holds, as every file of this engine does, and one of those
    // lacking the record is a view or holds a variable.
    Zelph z(null_handler());
    z.load_from_file(again.string());
    CHECK(z.may_hold_untracked_nodes());
    fs::remove(saved);
    fs::remove(older);
    fs::remove(again);
}

// The stores describe the graph a load discards, thus they are reset at the
// point where the load discards it, not at the point where it begins: a load
// that is refused before this, here due to a file that does not exist,
// leaves the session's graph, and with it the stores, which continue to
// speak for every node within it.
TEST_CASE("variable record: a load that is refused before it discards the graph leaves the stores as they were")
{
    using namespace zelph::test;

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, usage_rules);
    const auto before = predicate_usage(collector, interactive);
    REQUIRE(of_rules(before) == usage_of_rules);

    const auto missing = std::filesystem::temp_directory_path() / "zelph_no_such_network.bin";
    std::filesystem::remove(missing);
    CHECK_THROWS(interactive.process(".load \"" + missing.string() + "\""));
    CHECK(interactive.graph()->fact_stores_enabled());
    CHECK_FALSE(interactive.graph()->may_hold_untracked_nodes());
    CHECK(predicate_usage(collector, interactive) == before);
}

// `.fact-stores off` remains in effect until `.new`, including during a load:
// the switch enables a session to prevent the stores from growing during bulk
// work on a loaded graph, and a load that armed them again would reverse
// this. The record in the file is then not entered, and the load reads every
// node via the walk.
TEST_CASE("variable record: a load keeps the stores off once they are switched off")
{
    using namespace zelph::test;
    namespace fs    = std::filesystem;
    const auto path = fs::temp_directory_path() / "zelph_record_stores_off.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((X zp Y) zr c) => (X zq Y)\n(a zp b) zr c");
        interactive.process(".save \"" + path.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".fact-stores off");
    interactive.process(".load \"" + path.string() + "\"");
    fs::remove(path);
    CHECK_FALSE(interactive.graph()->fact_stores_enabled());
    collector.clear();
    interactive.process("A zr B");
    CHECK(collect_answers(collector) == std::vector<std::string>{"(a zp b) zr c"});
}
