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

#include "test_helpers.hpp"
#include "wikidata_dump.hpp"

#include "network/reasoning.hpp"

#include <filesystem>
#include <regex>
#include <set>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// .prune-facts / .prune-nodes -- the destructive commands, where being wrong
// is expensive and being wrong QUIETLY is worse. The pattern they take is
// evaluated, and evaluating a zelph statement materializes it, so a removal
// command was able to insert the very fact it had been asked to delete.
// ---------------------------------------------------------------------------

TEST_CASE("pruning: a pattern that matches nothing leaves nothing behind")
{
    // ".prune-facts Q42 typo Q7" used to CREATE that fact -- a deletion
    // command adding data on a typo, on a graph where a typo is likely.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE(".prune-facts")
        {
            interactive.process(".prune-facts nowhere pred thing");
            collector.clear();
            interactive.process("nowhere pred X");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE(".prune-nodes")
        {
            interactive.process(".prune-nodes A typo thing");
            collector.clear();
            interactive.process("X typo Y");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        } });
}

TEST_CASE("pruning: a pattern without variables removes exactly that fact")
{
    // The scan behind .prune-facts binds variables to find matches, so a
    // fully ground pattern -- the most obvious way to delete one specific
    // fact -- matched nothing and reported "Pruned 0".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b");
        interactive.process("c rel d");
        collector.clear();
        interactive.process(".prune-facts a rel b");
        CHECK(any_output_contains(collector, "Pruned 1 matching facts"));

        collector.clear();
        interactive.process("X rel Y");
        CHECK(answers_contain(collector, "c rel d"));
        CHECK_FALSE(any_output_contains(collector, "a rel b")); });
}

TEST_CASE("pruning: a variable pattern still removes every match")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b");
        interactive.process("c rel d");
        interactive.process("a other x");
        collector.clear();
        interactive.process(".prune-facts A rel B");
        CHECK(any_output_contains(collector, "Pruned 2 matching facts"));

        collector.clear();
        interactive.process("X rel Y");
        CHECK_FALSE(any_output_starts_with(collector, "Answer:"));

        // A different predicate is untouched.
        collector.clear();
        interactive.process("X other Y");
        CHECK(answers_contain(collector, "a other x")); });
}

// A negated pattern is valid when its pattern is ABSENT, meaning it does not
// match any fact for removal. ".prune-facts ¬(X p b)" read its operand in a
// positive manner and removed every `X p b` -- the facts that the line says are
// not present.
TEST_CASE("pruning: a condition operator is refused as a pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t p b");
        interactive.process("u p b");

        CHECK_THROWS_WITH(interactive.process(".prune-facts ¬(X p b)"), doctest::Contains("condition operator"));
        CHECK_THROWS_WITH(interactive.process(".prune-nodes ¬(X p b)"), doctest::Contains("condition operator"));

        collector.clear();
        interactive.process("X p Y");
        CHECK(collect_answers(collector).size() == 2); });
}

// Following "X go Y, ¬(a p b)", the operand `a p b` is present solely as
// the query's pattern, and the hint said it was present only as a rule's
// pattern: a diagnosis that sends the reader to seek a rule that does not
// exist.
TEST_CASE("pruning: a query's operand is no claim, and the hint names the query")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        interactive.process("X go Y, ¬(a p b)");

        collector.clear();
        interactive.process(".prune-facts (a p b)");
        CHECK(any_output_contains(collector, "Pruned 0"));
        CHECK(any_event_contains(collector, "or as one a query wrote beneath a condition operator")); });
}

TEST_CASE("pruning: .prune-nodes deletes the node's names with the node")
{
    // It used to drop the node from the adjacency maps but keep its name
    // mapping, so the name still resolved -- ".node a" then displayed a
    // node that is no longer in the graph.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b");
        interactive.process("a other x");
        interactive.process(".prune-nodes A rel b");

        collector.clear();
        CHECK_THROWS_AS(interactive.process(".node a"), std::runtime_error); });
}

TEST_CASE("pruning: a batch loses its names in every language, and only its own")
{
    // The reverse name map can only be searched BY VALUE, so a bulk removal
    // takes one pass over it for the whole batch instead of one per node.
    // Two things that pass has to get right, and neither is exercised by a
    // single-node removal: several languages hold several reverse entries for
    // one node, and a pass that tests the value against a set must not take
    // the entries of the nodes around it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        interactive.process("alpha rel beta");
        interactive.process("gamma rel beta");
        interactive.process("survivor rel other");
        interactive.process(".name alpha en AlphaEN");
        interactive.process(".name alpha de AlphaDE");
        interactive.process(".name gamma en GammaEN");
        interactive.process(".name survivor en SurvEN");
        interactive.process(".name survivor de SurvDE");

        interactive.process(".prune-nodes A rel beta");

        interactive.process(".lang en");
        CHECK_THROWS_AS(interactive.process(".node AlphaEN"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".node GammaEN"), std::runtime_error);
        interactive.process(".node SurvEN"); // still there, or this throws

        interactive.process(".lang de");
        CHECK_THROWS_AS(interactive.process(".node AlphaDE"), std::runtime_error);
        interactive.process(".node SurvDE"); });
}

TEST_CASE("pruning: pruning the holder of a shared label re-elects the lowest surviving carrier")
{
    // Every item possessing a Wikidata label carries it, and the label
    // resolves to the one ranked first. The published networks undergo
    // pruning, and the holder was typically among those that went, while
    // other carriers stayed: "man", "city", "Paris", and "John" resolved to
    // nothing in -small, and "Berlin" would have kept 56 carriers and no
    // holder. The name -> node entry is directed to the carrier ranked first
    // among those left.
    using namespace wikidata_dump;
    const auto dump = write_lines("zelph_prune_holder",
                                  {entity("Q8441", text("en", "man"), claim("P31", "Q100")),
                                   entity("Q29719014", text("en", "man"), claim("P31", "Q101")),
                                   entity("Q709369", text("en", "man"), claim("P31", "Q102"))});

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    auto* const graph = interactive.graph();
    REQUIRE(holder_of(interactive, "man") == "Q8441");

    interactive.process(".lang wikidata");
    interactive.process(".prune-nodes A P31 Q100");

    CHECK(holder_of(interactive, "man") == "Q709369");
    CHECK(ids_of(interactive, graph->nodes_named("man", "en")) == std::vector<std::string>{"Q709369", "Q29719014"});
    CHECK(graph->label_index_consistent());

    interactive.process(".lang en");
    collector.clear();
    interactive.process(".node man");
    CHECK(any_event_contains(collector, "Q709369"));

    SUBCASE("a label left with one carrier leaves the index")
    {
        interactive.process(".lang wikidata");
        interactive.process(".prune-nodes A P31 Q102");

        CHECK(holder_of(interactive, "man") == "Q29719014");
        CHECK(ids_of(interactive, graph->nodes_named("man", "en")) == std::vector<std::string>{"Q29719014"});
        CHECK(graph->label_index_consistent());
    }

    remove_dump(dump);
}

TEST_CASE("pruning: a label keeps resolving through .save, .load, a prune and .cleanup")
{
    // The networks that have been published are cut the way this case cuts
    // one, with each stage in a process of its own: the import saves; a prune
    // phase that loads, prunes, and saves; the final stage that loads, runs
    // .cleanup, and saves. The carriers of a label are not saved as such, so
    // each load finds them, and the label must move to the next carrier in
    // every removal the recipe uses: .prune-nodes, and .cleanup taking a
    // holder a prune left isolated.
    using namespace wikidata_dump;
    namespace fs = std::filesystem;

    const auto     dump     = write_lines("zelph_prune_chain",
                                          {entity("Q8441", text("en", "man"), claim("P31", "Q100")),
                                           entity("Q29719014", text("en", "man"), claim("P31", "Q101")),
                                           entity("Q709369", text("en", "man"), claim("P31", "Q102"))});
    const fs::path imported = fs::temp_directory_path() / "zelph_prune_chain_imported.bin";
    const fs::path pruned   = fs::temp_directory_path() / "zelph_prune_chain_pruned.bin";

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + dump.string() + "\"");
    interactive.process(".save \"" + imported.string() + "\"");
    interactive.process(".new");
    interactive.process(".load \"" + imported.string() + "\"");
    interactive.process(".lang wikidata");
    REQUIRE(holder_of(interactive, "man") == "Q8441");

    SUBCASE(".prune-nodes")
    {
        interactive.process(".prune-nodes A P31 Q100");
    }

    SUBCASE(".cleanup of the holder a prune left isolated")
    {
        interactive.process(".prune-facts Q8441 P31 Q100");
        interactive.process(".cleanup");
        CHECK(entity_node(interactive, "Q8441") == 0);
        CHECK(interactive.graph()->get_node(zelph::network::Zelph::wikidata_labels_marker_name(), "zelph", true) != 0);
    }

    CHECK(holder_of(interactive, "man") == "Q709369");

    interactive.process(".save \"" + pruned.string() + "\"");
    interactive.process(".new");
    interactive.process(".load \"" + pruned.string() + "\"");
    CHECK(holder_of(interactive, "man") == "Q709369");
    CHECK(ids_of(interactive, interactive.graph()->nodes_named("man", "en")) == std::vector<std::string>{"Q709369", "Q29719014"});
    CHECK(interactive.graph()->label_index_consistent());

    fs::remove(imported);
    fs::remove(pruned);
    remove_dump(dump);
}

TEST_CASE("pruning: after a partial load a removal still erases a name whose forward entry was not loaded")
{
    // A removal finds a node's reverse name entries (name -> node) by
    // leveraging its forward ones (node -> name): each writer maintains the
    // latter for every former. A partial load does not: it reads the two name
    // sections independently, and `nameOfNode=none` causes all reverse
    // entries to be included while omitting any forward ones. When removing a
    // node, one must still walk the reverse map as previously, otherwise the
    // entry persists beyond the node's lifetime, and the name ends up
    // resolving to an ID that no longer exists.
    //
    // In a partial session, the REPL blocks removals; the engine API does
    // not, and the caller safeguarded by this is an embedder that loads a
    // slice and removes from it.
    namespace fs        = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "zelph_partial_names_removal.bin";

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("alpha rel beta");
    interactive.process("gamma rel beta");
    interactive.process(".save \"" + file.string() + "\"");
    interactive.process(".load-partial \"" + file.string() + "\" nameOfNode=none");

    auto* const graph = interactive.graph();
    REQUIRE(graph != nullptr);
    const zelph::network::Node alpha = graph->get_node("alpha", "zelph");
    REQUIRE(alpha != 0);
    REQUIRE(graph->get_name(alpha, "zelph", false).empty());

    const uint64_t before = graph->name_map_scans();
    graph->remove_node(alpha);
    CHECK(graph->name_map_scans() > before);
    CHECK(graph->get_node("alpha", "zelph") == 0);
    CHECK(graph->get_node("gamma", "zelph") != 0);

    fs::remove(file);
}

TEST_CASE("pruning: a prune walks no name map")
{
    // A guard positioned on a COST, not on a result. It pinned a fixed count
    // of reverse name map traversals per command, the August fix for a
    // full-dump prune operating at 1.11 nodes per second because `remove_node`
    // walked the whole map once per node (two months for a script required to
    // finish overnight). Removal now reads a node's own names via the forward
    // map, meaning a prune performs zero walks, regardless of whether it kills
    // one node or twenty; a walk returning is the regression this case is
    // meant to detect. The OUTCOME remains unchanged when that shape
    // reappears, which is precisely why the tests above fail to detect it.
    //
    // The fact that the counter is wired at all is held by the partial-load
    // case mentioned earlier, the sole state that continues to walk.
    //
    // The counter operates without reliance on specific hardware, thus
    // eliminating the need for a quiet machine.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        interactive.process("one rel small");

        const uint64_t before_small = graph->name_map_scans();
        interactive.process(".prune-nodes A rel small");
        CHECK(graph->name_map_scans() == before_small);

        for (int i = 0; i < 20; ++i)
            interactive.process("n" + std::to_string(i) + " rel big");

        const uint64_t before_big = graph->name_map_scans();
        interactive.process(".prune-nodes A rel big");
        CHECK(graph->name_map_scans() == before_big);
        CHECK_THROWS_AS(interactive.process(".node n7"), std::runtime_error); });
}

TEST_CASE("pruning: removing the last name of a language erases the language")
{
    // A language remains registered as long as either name map knows it, and a
    // registered language encompasses more than just a line in .stat: when
    // `wikidata` is registered, it causes every node bearing an `en` name to
    // display as "Q… - label". Previously, a prune that took the last node
    // named in a language would leave both maps of that language intact,
    // though empty, yet still registered.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("alpha rel beta");
        interactive.process(".name alpha de AlphaDE");

        const auto languages = [&]
        {
            collector.clear();
            interactive.process(".stat");
            for (const auto& event : collector.events())
            {
                const std::string text = normalize(event.text);
                const auto        pos  = text.find("Languages: ");
                if (pos != std::string::npos) return std::stoi(text.substr(pos + 11));
            }
            return -1;
        };

        const int before = languages();
        REQUIRE(before >= 2); // zelph and de

        interactive.process(".prune-nodes A rel beta");
        CHECK(languages() == before - 1);

        interactive.process(".lang de");
        CHECK_THROWS_AS(interactive.process(".node AlphaDE"), std::runtime_error); });
}

TEST_CASE("pruning: .prune-nodes insists on the one variable it deletes by")
{
    // With two variables it deleted the SUBJECT bindings and left the
    // object ones alone, silently -- half a deletion, on a command whose
    // help calls it destructive and irreversible.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        interactive.process("a rel b");
        CHECK_THROWS_WITH_AS(interactive.process(".prune-nodes A rel B"),
                             doctest::Contains("exactly one variable"),
                             std::runtime_error);

        // The documented forms are unaffected.
        interactive.process(".prune-nodes A rel b"); });
}

TEST_CASE("removal: a node takes what it is a part of with it")
{
    // Removing a node used to disconnect it and leave every fact it took
    // part in standing. A fact minus its OBJECT is not recognisable as
    // incomplete: the subject is the only neighbour left and a subject
    // links to its fact bidirectionally, which is exactly the shape of a
    // self-fact. So `outside rel d` became indistinguishable from
    // `outside rel outside`, answered `outside rel X` as that, and went
    // into the .bin on the next .save -- the engine asserting something
    // nobody stated.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("outside rel d");
        interactive.process("keep rel other");
        interactive.process(".remove d");

        collector.clear();
        interactive.process("S rel O");
        CHECK(answers_contain(collector, "keep rel other"));
        CHECK_FALSE(any_output_contains(collector, "outside"));

        // Not a self-fact either, which is the form the leftover took.
        CHECK_FALSE(any_output_contains(collector, ":rel outside")); });
}

TEST_CASE("removal: a rule goes with a node its condition names")
{
    // A rule that merely loses a condition keeps firing on the ones that
    // remain, i.e. it concludes MORE than it was written to conclude.
    // Removing 'yellow' left `(X has petals) => (X is flower)` behind,
    // which then derived that a rose is a flower.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X is yellow, X has petals) => (X is flower)");
        interactive.process("(X has thorns, X has petals) => (X is rose)");
        interactive.process(".remove yellow");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X has thorns)"));
        CHECK_FALSE(any_output_contains(collector, "(X is flower)"));

        // And it does not fire on what is left of it.
        interactive.process("rose has petals");
        collector.clear();
        interactive.process("rose is Y");
        CHECK_FALSE(any_output_contains(collector, "flower")); });
}

TEST_CASE("removal: a condition another rule shares is not dragged along")
{
    // The cascade runs strictly UPWARDS: a doomed fact takes the facts it
    // occurs in, never its own subject, predicate or objects. Walking down
    // into the parts would delete a ground condition two rules share, and
    // with it the second rule -- which has nothing to do with the node
    // being removed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(m is n, X has p) => (X is q)");
        interactive.process("(m is n, Y has r) => (Y is s)");
        interactive.process(".remove p");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(Y has r)"));
        CHECK(any_output_contains(collector, "(m is n)"));
        CHECK_FALSE(any_output_contains(collector, "(X is q)")); });
}

TEST_CASE("cleanup: a composite predicate is not a broken fact")
{
    // .cleanup's zombie scan walked everything POINTING AT a predicate and
    // read each as a fact using it. For an atomic predicate the two are the
    // same set; a COMPOSITE one is a fact as well, so its own subject and
    // objects point at it too -- and those atoms have no subject of their
    // own, which is exactly the scan's definition of a zombie. So .cleanup
    // purged `a` and `b`, and both facts went with them: a maintenance
    // command deleting the data it was asked to tidy.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
z (a p b) w
)");
        collector.clear();
        interactive.process(".cleanup");
        CHECK(any_output_contains(collector, "Purged 0 zombie facts"));

        collector.clear();
        interactive.process("S P O");
        CHECK(answers_contain(collector, "a p b"));
        CHECK(answers_contain(collector, "z (a p b) w")); });
}

TEST_CASE("cleanup: a fact whose subject is its predicate is not a broken fact")
{
    // In `p p b`, a single pair of edges connecting the fact to `p` carries
    // both its subject and its relation. During the search for a subject, the
    // zombie scan bypassed the relation, discovered no subject, and purged a
    // complete fact.
    SUBCASE("in the session that wrote it")
    {
        run_both_modes([](auto& collector, auto& interactive)
                       {
            interactive.process("p p b");
            collector.clear();
            interactive.process(".cleanup");
            CHECK(any_output_contains(collector, "Purged 0 zombie facts"));

            collector.clear();
            interactive.process("S p O");
            CHECK(answers_contain(collector, "p p b")); });
    }

    // A network that has been loaded retrieves the fact's structure back
    // from its edges, which must yield the same answer as the structure the
    // session stored.
    SUBCASE("after .save and .load")
    {
        const std::filesystem::path out = std::filesystem::temp_directory_path() / "zelph_cleanup_self_predicate.bin";
        std::filesystem::remove(out);

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("p p b");
        interactive.process(".save " + out.string());
        interactive.process(".new");
        interactive.process(".load " + out.string());
        std::filesystem::remove(out);

        collector.clear();
        interactive.process(".cleanup");
        CHECK(any_output_contains(collector, "Purged 0 zombie facts"));

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "p p b"));
    }
}

// `.cleanup` removes a predicate that no longer serves as a relation for
// any fact, and removing its node takes every fact of which the node is a
// component. A rule is built from facts, thus a predicate node that was
// solely held by a rule vanished along with the rule -- or the rule lost a
// part and read as something else.
TEST_CASE("cleanup: a rule keeps every node it is built from")
{
    const auto listed = [](const zelph::console::Interactive& interactive, zelph::io::OutputCollector& collector)
    {
        collector.clear();
        interactive.process(".list-rules");
        std::string text;
        for (const auto& e : collector.events())
            if (e.channel == zelph::io::OutputChannel::Out) text += e.text + "\n";
        return text;
    };

    // `x (a p b) y` establishes `(a p b)` as a relation type. After this fact
    // is pruned, nothing uses `(a p b)` as a relation any more, and only the
    // rule holds it.
    const auto check = [&](const std::string& rule, const std::string& expected)
    {
        CAPTURE(rule);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, rule + "\nx (a p b) y\n.prune-facts x (a p b) y\n");
        const std::string before = listed(interactive, collector);
        REQUIRE(before.find(expected) != std::string::npos);

        interactive.process(".cleanup");
        CHECK(listed(interactive, collector) == before);
    };

    SUBCASE("a condition")
    {
        check("(a p b) => (c q d)", "(a p b) => (c q d)");
    }
    SUBCASE("a consequence")
    {
        check("(c q d) => (a p b)", "(c q d) => (a p b)");
    }
    SUBCASE("a member of a condition set")
    {
        check("(X go Y, a p b) => (X ok Y)", "(a p b)");
    }
    SUBCASE("a statement nested in a condition")
    {
        check("((a p b) says Z) => (Z ok yes)", "((a p b) says Z) => (Z ok yes)");
    }
    SUBCASE("the operand of a negated condition")
    {
        check("(X go Y, ¬(a p b)) => (X ok Y)", "¬(a p b)");
    }
    SUBCASE("the operand of a neural condition")
    {
        check("(X go Y, ≈knet(a p b)) => (X ok Y)", "≈knet(a p b)");
    }
    SUBCASE("the operand of a path condition")
    {
        check("((a p b) r⁺ Y) => (Y ok yes)", "((a p b) r⁺ Y)");
        check("((a p b) r∗ Y) => (Y ok yes)", "((a p b) r∗ Y)");
    }
    SUBCASE("a rule another rule derived")
    {
        check("(X gen Y) => ((a p b) => (X q Y))\nk gen m", "(a p b) => (k q m)");
    }

    // The node need not be a statement: `p` functions as a relation type via
    // `a p b`, and once that fact went, removing `p` turned `(X is p)` into
    // `(X is X)`, a rule that fires on different data.
    SUBCASE("an atom the rule names outside the relation")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "(X is p) => (X ok yes)\na p b\n.prune-facts a p b\n");
        const std::string before = listed(interactive, collector);
        REQUIRE(before.find("(X is p)") != std::string::npos);

        interactive.process(".cleanup");
        CHECK(listed(interactive, collector) == before);
    }

    // The scan does not count `(p p X)` as a use of `p`, since the single
    // pair of edges to the fact conveys both subject and relation, hence
    // .cleanup removed `p` and the rule's condition, with no prune applied
    // beforehand.
    SUBCASE("a condition whose subject is its relation")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("(p p X) => (X ok yes)");
        const std::string before = listed(interactive, collector);

        interactive.process(".cleanup");
        CHECK(listed(interactive, collector) == before);

        process_lines(interactive, "p p b\n");
        collector.clear();
        interactive.process("S ok O");
        CHECK(answers_contain(collector, "b ok yes"));
    }

    // A rule mentioned by a statement constitutes data, not a rule in force,
    // and the statement keeps its components just as it keeps those of any
    // other fact.
    SUBCASE("a rule a statement mentions")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "x says ((a p b) => (c q d))\nu (a p b) v\n.prune-facts u (a p b) v\n.cleanup\n");
        collector.clear();
        interactive.process("S says O");
        CHECK(answers_contain(collector, "x says ((a p b) => (c q d))"));
    }

    // A rule concerning a predicate that currently has no fact utilizing it:
    // its condition is a use of the predicate, thus the predicate stays, and
    // the rule fires upon the first fact.
    SUBCASE("a rule over a predicate no fact uses yet")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("(X p Y) => (X q Y)");
        const std::string before = listed(interactive, collector);

        interactive.process(".cleanup");
        CHECK(listed(interactive, collector) == before);

        process_lines(interactive, "a p b\n");
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "a q b"));
    }
}

// The same removal broke data: a fact naming the predicate lost a part,
// and a statement used as a relation, which constitutes a fact
// independently, was erased despite being asserted.
TEST_CASE("cleanup: a predicate a statement still holds is not unused")
{
    SUBCASE("a statement about the predicate")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "a p b\np is transitive\n.prune-facts a p b\n.cleanup\n");
        collector.clear();
        interactive.process("S is O");
        CHECK(answers_contain(collector, "p is transitive"));
    }

    // `x (a p b) y` claims `(a p b)` as well, and the prune leaves that claim.
    SUBCASE("a statement used as a predicate")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "x (a p b) y\n.prune-facts x (a p b) y\n");
        collector.clear();
        interactive.process("S p O");
        REQUIRE(answers_contain(collector, "a p b"));

        interactive.process(".cleanup");
        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "a p b"));
    }

    // As in a rule's `(p p X)`, the nested `(r r c)` does not contribute to
    // the tally of `r` usage, and `r` was eliminated from beneath it.
    SUBCASE("a nested fact whose subject is its relation")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "x says (r r c)\n.cleanup\n");
        collector.clear();
        interactive.process("S says O");
        CHECK(answers_contain(collector, "x says (r r c)"));
    }

    // Purpose of the removal: a predicate that had its final fact disappear
    // and now is held solely by its own declaration.
    SUBCASE("a predicate nothing holds still goes")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "a p b\n.prune-facts a p b\n");
        collector.clear();
        interactive.process(".cleanup");
        CHECK(any_output_contains(collector, "Removed 1 unused predicates"));
    }
}

TEST_CASE("removal: a fact using a composite predicate is not part of it")
{
    // The cascade asked parse_fact what `a p b` consists of, and a fact that
    // uses it as its PREDICATE points at it without being pointed back at --
    // exactly like an object. So every such fact counted among its objects,
    // removing ONE of them doomed the predicate fact, and the predicate fact
    // took every other user with it: ".prune-facts (x (a p b) y)" reported a
    // single removal and left a graph without `z (a p b) w` and without
    // `a p b`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x (a p b) y
z (a p b) w
)");
        collector.clear();
        interactive.process(".prune-facts (x (a p b) y)");
        CHECK(any_output_contains(collector, "Pruned 1"));

        collector.clear();
        interactive.process("S (a p b) O");
        CHECK(answers_contain(collector, "z (a p b) w"));
        CHECK_FALSE(answers_contain(collector, "x (a p b) y"));

        // The predicate fact itself is untouched -- it is a part of the
        // removed fact, and the cascade runs strictly upwards.
        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b")); });
}

TEST_CASE("removal: a nested fact goes with the node inside it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) q c");
        interactive.process("x q c");
        interactive.process(".remove b");

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "x q c"));
        CHECK_FALSE(any_output_contains(collector, "a p")); });
}

TEST_CASE("removal: nothing incomplete reaches the .bin")
{
    // The leftover was structural, so .save wrote it out and .load read it
    // back -- the false assertion outlived the session that caused it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_remove_roundtrip.bin";
        std::filesystem::remove(out);

        interactive.process("s5 rel o5");
        interactive.process("keep rel other");
        interactive.process(".remove o5");
        interactive.process(".save " + out.string());
        interactive.process(".new");
        interactive.process(".load " + out.string());

        collector.clear();
        interactive.process("S rel O");
        CHECK(answers_contain(collector, "keep rel other"));
        CHECK_FALSE(any_output_contains(collector, "s5"));

        std::filesystem::remove(out); });
}

TEST_CASE("removal: the report counts what actually went")
{
    // Removing a node takes the facts it is part of, so counting the
    // removal CALLS understates what happened -- and for .cluster-drop it
    // understated twice over, since a later entry of the same cluster may
    // already have gone with an earlier one. The rollback accounting is the
    // whole point of that command, so the number has to be the real one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE(".remove says how much went with the node")
        {
            interactive.process("outer rel d");
            interactive.process("second rel d");
            collector.clear();
            interactive.process(".remove d");
            // The node itself plus the two facts it was the object of.
            CHECK(any_output_contains(collector, "and 2 node(s) it was part of"));
        }
        SUBCASE(".cluster-drop removes every node it recorded")
        {
            interactive.process(".cluster rollback");
            interactive.process("c rel e");
            interactive.process(".cluster default");

            collector.clear();
            interactive.process(".cluster");
            // Whatever the cluster recorded has to be what the drop reports,
            // so the expected number is read off rather than assumed.
            std::string recorded;
            for (const auto& e : collector.events())
            {
                const std::string  n   = normalize(e.text);
                const std::size_t  pos = n.find("rollback: ");
                if (pos == std::string::npos) continue;
                const std::size_t end = n.find(' ', pos + 10);
                recorded              = n.substr(pos + 10, end - pos - 10);
            }
            REQUIRE_FALSE(recorded.empty());

            collector.clear();
            interactive.process(".cluster-drop rollback");
            CHECK(any_output_contains(collector, "removed " + recorded + " node(s)"));
        } });
}

TEST_CASE("pruning: the pattern reads the way .explain reads it")
{
    // The prune commands built their pattern by QUOTING every non-variable
    // token, which reduces it to a triple of literal names. A pattern in
    // parentheses -- the form .explain and the documentation use -- became
    // a fact of the names "(a", "rel" and "b)", and the command then
    // reported "Pruned 0", which its own help describes as the legitimate
    // outcome of a pattern that matches nothing. Every structured pattern
    // went the same way.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("parentheses around the whole pattern")
        {
            interactive.process("a rel b");
            interactive.process("c rel d");
            collector.clear();
            interactive.process(".prune-facts (a rel b)");
            CHECK(any_output_contains(collector, "Pruned 1 matching facts"));

            collector.clear();
            interactive.process("X rel Y");
            CHECK(answers_contain(collector, "c rel d"));
            CHECK_FALSE(any_output_contains(collector, "a rel b"));
        }
        SUBCASE("parentheses around a pattern with a variable")
        {
            interactive.process("s4 rel o4");
            interactive.process("s4 rel o5");
            collector.clear();
            interactive.process(".prune-nodes (s4 rel X)");
            CHECK(any_output_contains(collector, "Pruned 2 matching facts and 2 nodes"));
        }
        SUBCASE("a nested fact as subject")
        {
            interactive.process("(a p b) q c");
            interactive.process("x q c");
            collector.clear();
            interactive.process(".prune-facts (a p b) q X");
            CHECK(any_output_contains(collector, "Pruned 1 matching facts"));

            collector.clear();
            interactive.process("S q O");
            CHECK(answers_contain(collector, "x q c"));
        }
        SUBCASE("a list as object")
        {
            interactive.process("f maps <a b>");
            interactive.process("g maps <c d>");
            collector.clear();
            interactive.process(".prune-facts f maps <a b>");
            CHECK(any_output_contains(collector, "Pruned 1 matching facts"));

            collector.clear();
            interactive.process("S maps O");
            CHECK(answers_contain(collector, "g maps <c d>"));
        }
        SUBCASE("a quoted predicate still survives the re-quoting")
        {
            interactive.process("a \"is not\" b");
            interactive.process("c \"is not\" d");
            collector.clear();
            interactive.process(".prune-facts a \"is not\" b");
            CHECK(any_output_contains(collector, "Pruned 1 matching facts"));

            collector.clear();
            interactive.process("X \"is not\" Y");
            CHECK(answers_contain(collector, "c \"is not\" d"));
        } });
}

TEST_CASE("removal: a variable member does not take the container with it")
{
    // Removing a PartOf fact dooms its container, and rightly so: a set IS
    // its elements, and a rule's condition list is such a set (see "a rule
    // goes with a node its condition names" above).
    //
    // A VARIABLE member is not an element, though. `X in {a b}` is how a rule
    // quantifies over the members rather than a claim about them, and both
    // is_set_constant and the renderer skip such a member for that reason.
    // Dooming the container for it destroyed a SHARED set constant -- and
    // this was reachable without any removal command at all: the parse-time
    // duplicate check builds every rule in a scratch cluster and rolls it
    // back, so entering an alpha-equivalent rule a second time deleted the
    // first one and left "No rules found".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in {a b}) => (X flagged yes)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(any_output_contains(collector, "(X in {a b})"));

        // The duplicate is recognised and rolled back -- and the original
        // survives that rollback.
        interactive.process("(A in {a b}) => (A flagged yes)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "in {a b}"));
        CHECK_FALSE(any_output_contains(collector, "No rules found"));

        // And it still works.
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "a flagged yes"));
        CHECK(answers_contain(collector, "b flagged yes")); });
}

TEST_CASE("removal: a real element still takes the container with it")
{
    // The control for the case above: an element that is NOT a variable
    // invalidates the set it is removed from, exactly as before, and the rule
    // written against that set goes with it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("z rel {a b}");
        interactive.process("(X in {a b}) => (X flagged yes)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(any_output_contains(collector, "(X in {a b})"));

        interactive.process(".prune-facts (a in {a b})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

TEST_CASE("pruning: a pattern that is not a bare variable is pruned too")
{
    // .prune-facts takes a rule condition and removes what it matches, and
    // the matching itself was never the problem -- unification resolves a
    // structured subject, object and (since it learned to) predicate alike.
    // What did not follow was the reconstruction of WHAT to remove: it
    // substituted only where the pattern was a bare variable and then asked
    // check_fact about the pattern node itself. So the command reported
    // "Pruned 0 matching facts" for facts it had just matched, in all three
    // positions.
    SUBCASE("composite predicate")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a (b r s) c
d (e r s) f
g p h
)");
        collector.clear();
        interactive.process(".prune-facts (X (Y r s) Z)");
        CHECK(any_output_contains(collector, "Pruned 2"));

        // The fact of another predicate is untouched.
        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "g p h"));
    }

    SUBCASE("composite subject")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(b r s) p c
(e r s) p f
)");
        collector.clear();
        interactive.process(".prune-facts ((Y r s) p Z)");
        CHECK(any_output_contains(collector, "Pruned 2"));
    }

    SUBCASE("composite object")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
c p (b r s)
f p (e r s)
)");
        collector.clear();
        interactive.process(".prune-facts (X p (Y r s))");
        CHECK(any_output_contains(collector, "Pruned 2"));
    }

    SUBCASE("a list pattern in subject position")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("<a b> p c");

        collector.clear();
        interactive.process(".prune-facts ((H cons T) p Z)");
        CHECK(any_output_contains(collector, "Pruned 1"));
    }

    SUBCASE("the plain shapes are unchanged")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
d p e
)");
        collector.clear();
        interactive.process(".prune-facts (X p Y)");
        CHECK(any_output_contains(collector, "Pruned 2"));

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());
    }

    SUBCASE("a ground composite predicate is unchanged")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
x (a p b) y
)");
        collector.clear();
        interactive.process(".prune-facts (X (a p b) Y)");
        CHECK(any_output_contains(collector, "Pruned 1"));
    }
}

TEST_CASE("pruning: a rule's own ground pattern is not data to prune")
{
    // Two notions of matching inside ONE command. The variable form goes
    // through unification, which skips a rule's ground patterns, so
    // ".prune-facts (S p O)" correctly prunes nothing where the only "a p b"
    // is a rule's condition. The ground form asked check_fact -- the
    // STRUCTURAL probe -- and deleted the node, and a rule goes with a node
    // its condition is built from, so the rule was gone as well. Silent data
    // loss on a command the user aimed at data that was never there.
    SUBCASE("the ground form prunes nothing and the rule survives")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process(".prune-facts (a p b)");
        CHECK(any_output_contains(collector, "Pruned 0"));
        CHECK(any_event_contains(collector, "only as a rule's own pattern"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (c q d)"));

        // The consequence pattern is no more data than the condition is.
        collector.clear();
        interactive.process(".prune-nodes (c q d)");
        CHECK(any_output_contains(collector, "Pruned 0"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (c q d)"));
    }

    SUBCASE("the variable form agrees, as it always did")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process(".prune-facts (S p O)");
        CHECK(any_output_contains(collector, "Pruned 0"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (c q d)"));
    }

    // Asserting the statement revokes the marking: it becomes data, and the
    // prune commands take it. What they take is the CLAIM. The node also
    // serves as the rule's condition, and removing it took the rule with it
    // -- a rule no one had requested to eliminate, disappeared silently,
    // and every later run performed without it. The statement reverts to
    // being the rule's pattern, just as a dropped cluster leaves it.
    SUBCASE("asserting the statement makes it data again, and pruning it leaves the rule")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
(a p b) => (c q d)
)");
        collector.clear();
        interactive.process(".prune-facts (a p b)");
        CHECK(any_output_contains(collector, "Pruned 1"));
        CHECK_FALSE(any_event_contains(collector, "only as a rule's own pattern"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (c q d)"));

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());

        // Asserting it once more transforms it into data once more,
        // and the rule fires.
        process_lines(interactive, "a p b\n");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "c q d"));
    }

    SUBCASE("the variable form leaves the rule as well")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
(a p b) => (e q f)
)");
        collector.clear();
        interactive.process(".prune-facts (S p O)");
        CHECK(any_output_contains(collector, "Pruned 1"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (e q f)"));
    }

    // While the scratch cluster holding its pattern remained active, the
    // variable form operated, recording within it the marking that makes
    // the statement a pattern again, and this marking went with the
    // scratch. The session continued interpreting it as a pattern from the
    // index, yet the .save did not write any marking, and the .load read
    // the statement back as asserted.
    SUBCASE("the variable form's withdrawal survives a .save and .load")
    {
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_prune_withdrawal_roundtrip.bin";
        std::filesystem::remove(out);

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
(a p b) => (e q f)
)");
        interactive.process(".prune-facts (S p b)");
        interactive.process(".save " + out.string());
        interactive.process(".new");
        interactive.process(".load " + out.string());

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (e q f)"));

        std::filesystem::remove(out);
    }

    // A withdrawal equates to a removal, yet dropping a cluster undoes no
    // removal. The marking that makes the statement a pattern again was
    // logged within the cluster the user had active nonetheless, and
    // .cluster-drop took it: the session continued interpreting the
    // statement as a pattern, even as the next .save wrote no marking, and
    // a .load retrieved it as asserted.
    SUBCASE("a withdrawal survives dropping the cluster it was made in")
    {
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_prune_withdrawal_cluster.bin";

        for (const std::string prune : {".prune-facts (a p b)", ".prune-facts (S p b)"})
        {
            CAPTURE(prune);
            std::filesystem::remove(out);

            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
a p b
(a p b) => (e q f)
)");
            interactive.process(".cluster exp");
            interactive.process(prune);
            interactive.process(".cluster-drop exp");
            interactive.process(".save " + out.string());
            interactive.process(".new");
            interactive.process(".load " + out.string());

            collector.clear();
            interactive.process("S p O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(a p b) => (e q f)"));
        }

        std::filesystem::remove(out);
    }

    // A statement enclosed within a condition or a consequence is also part
    // of the rule: the rule is built from it, and removing its node took the
    // rule with it, regardless of the command's form -- the variable form
    // left a remnant of the rule within the graph besides. Only the
    // conditions, the consequences, and the members of condition sets were
    // counted as belonging to the rule, whereas the identical nested
    // statement, if NOT asserted, was reported as the rule's own pattern and
    // remained untouched. If asserted before the rule, the statement was
    // never marked as its pattern; if asserted afterwards, the claim
    // nullified that designation. A rule situated within a consequence stands
    // as a distinct rule, with its condition already counted.
    SUBCASE("a statement nested inside a rule leaves the rule as well")
    {
        const auto listed_rules = [](const zelph::console::Interactive& interactive, zelph::io::OutputCollector& collector)
        {
            collector.clear();
            interactive.process(".list-rules");
            std::string listed;
            for (const auto& e : collector.events())
                if (e.channel == zelph::io::OutputChannel::Out) listed += e.text + "\n";
            return listed;
        };
        const char* const rules[] = {
            "((a p b) says Z) => (Z ok yes)",
            "(X q Y) => ((a p b) says X)",
            "(X (a p b) Y) => (X ok Y)",
            "(Z says (a p b)) => (Z ok yes)",
            "(((a p b) says Z) about W) => (Z ok W)",
            "(X q Y, ((a p b) says Z)) => (X r Z)",
            "(X q Y, ¬((a p b) says X)) => (X r Y)",
            "(X q Y) => ((a p b) => (X ok yes))",
        };
        for (const std::string rule : rules)
        {
            for (const std::string prune : {".prune-facts (a p b)", ".prune-facts (A p b)"})
            {
                for (const std::string asserted : {"before", "after", "never"})
                {
                    CAPTURE(rule);
                    CAPTURE(prune);
                    CAPTURE(asserted);
                    zelph::io::OutputCollector  collector;
                    zelph::console::Interactive interactive(collector.sink());
                    if (asserted == "before") process_lines(interactive, "a p b\n");
                    process_lines(interactive, rule + "\n");
                    if (asserted == "after") process_lines(interactive, "a p b\n");
                    const std::string before = listed_rules(interactive, collector);
                    REQUIRE(before.find("(a p b)") != std::string::npos);

                    collector.clear();
                    interactive.process(prune);
                    CHECK(any_output_contains(collector, asserted == "never" ? "Pruned 0" : "Pruned 1"));
                    if (asserted == "never" && prune == ".prune-facts (a p b)") CHECK(any_event_contains(collector, "only as a rule's own pattern"));

                    CHECK(listed_rules(interactive, collector) == before);

                    collector.clear();
                    interactive.process("S p O");
                    CHECK(collect_answers(collector).empty());
                }
            }
        }
    }

    // The statement the claim was withdrawn from is once more the rule's
    // pattern, and the rule fires upon a fact regarding it.
    SUBCASE("a rule a nested statement was pruned from still fires")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
a p b
((a p b) says Z) => (Z ok yes)
)");
        collector.clear();
        interactive.process(".prune-facts (a p b)");
        CHECK(any_output_contains(collector, "Pruned 1"));

        collector.clear();
        interactive.process(".explain (a p b) 0");
        CHECK(any_output_contains(collector, "a p b  [rule pattern; not asserted]"));

        process_lines(interactive, "(a p b) says d\n");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S ok O");
        CHECK(answers_contain(collector, "d ok yes"));
    }

    SUBCASE("a DERIVED ground fact is data, and its rule stays")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
x r y
(X r Y) => (a p b)
)");
        collector.clear();
        interactive.process(".prune-facts (a p b)");
        CHECK(any_output_contains(collector, "Pruned 1"));

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X r Y) => (a p b)"));
    }
}

// ---------------------------------------------------------------------------
// What holds a rule's pattern once its claim is withdrawn. Without a rule,
// `.prune-facts (a p b)` removes the fact through the cascade remove_node
// runs, and every statement that holds it also ceases to exist. Beside
// `(a p b) => (c r d)`, only the claim went: `(a p b) q z` went on answering
// `S q O`, a claimed statement that retains an unclaimed part, which the
// claims walk deems impossible. The holders now disappear just as they would
// without the rule, while the rule's text stays. Both forms of the command
// are checked whenever a rule is present, as they arrive at the withdrawal
// via distinct routes.
// ---------------------------------------------------------------------------

namespace
{
    constexpr const char* kRulePatternPrunes[] = {".prune-facts (a p b)", ".prune-facts (A p b)"};

    std::string listed_rules(const zelph::console::Interactive& interactive, zelph::io::OutputCollector& collector)
    {
        collector.clear();
        interactive.process(".list-rules");
        std::string listed;
        for (const auto& e : collector.events())
            if (e.channel == zelph::io::OutputChannel::Out) listed += e.text + "\n";
        return listed;
    }
} // namespace

TEST_CASE("pruning: what holds a rule's pattern goes with its claim")
{
    // The reference the rule case is held to.
    SUBCASE("without a rule, the statements holding the fact go with it")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(a p b) q z
x says ((a p b) q z)
)");
        interactive.process(".prune-facts (a p b)");

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());
    }

    SUBCASE("a statement holding the fact goes")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(a p b) => (c r d)
(a p b) q z
)");
            collector.clear();
            interactive.process(prune);
            CHECK(any_output_contains(collector, "Pruned 1"));

            collector.clear();
            interactive.process("S q O");
            CHECK(collect_answers(collector).empty());
        }
    }

    SUBCASE("a statement holding the holder goes as well")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(a p b) => (c r d)
x says ((a p b) q z)
)");
            interactive.process(prune);

            collector.clear();
            interactive.process("S says O");
            CHECK(collect_answers(collector).empty());
        }
    }

    // A pattern lacking variables does not bind anything for .prune-nodes
    // to remove, thus it prunes the single fact along the route the ground
    // .prune-facts takes, and the holders follow accordingly.
    SUBCASE("the ground form of .prune-nodes takes the holders as well")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(a p b) => (c r d)
(a p b) q z
)");
        const std::string before = listed_rules(interactive, collector);
        collector.clear();
        interactive.process(".prune-nodes (a p b)");
        CHECK(any_output_contains(collector, "Pruned 1 matching facts and 0 nodes"));
        CHECK(listed_rules(interactive, collector) == before);

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());
    }

    // The auto-run feature is disabled, meaning the rule did not trigger
    // before the prune operation, and the subsequent firing represents its
    // initial execution.
    SUBCASE("the rule stays, and fires once the fact is asserted again")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".auto-run");
            process_lines(interactive, R"(
(a p b) => (c r d)
(a p b) q z
)");
            interactive.process(prune);
            CHECK(listed_rules(interactive, collector).find("(a p b) => (c r d)") != std::string::npos);

            process_lines(interactive, "a p b\n.run\n");
            collector.clear();
            interactive.process("S r O");
            CHECK(answers_contain(collector, "c r d"));
        }
    }

    // A prune withdraws claims; it does not retract what was derived from
    // them during their validity.
    SUBCASE("derived facts stay as they are")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(a p b) => (c r d)
(a p b) q z
)");
            collector.clear();
            interactive.process("S r O");
            REQUIRE(answers_contain(collector, "c r d"));

            interactive.process(prune);
            collector.clear();
            interactive.process("S r O");
            CHECK(answers_contain(collector, "c r d"));
        }
    }

    // The expression `(a p b) q z` functions as a claim, and serves as the
    // condition within a second rule. It remains within that rule, and
    // since it is a claim holding the withdrawn fact, its claim goes too,
    // just as the fact's own claim did.
    SUBCASE("a holder that is part of another rule stays as that rule's pattern")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".auto-run");
            process_lines(interactive, R"(
(a p b) => (c r d)
((a p b) q z) => (e f g)
(a p b) q z
)");
            const std::string before = listed_rules(interactive, collector);
            interactive.process(prune);
            CHECK(listed_rules(interactive, collector) == before);

            collector.clear();
            interactive.process("S q O");
            CHECK(collect_answers(collector).empty());

            // Asserting it once more claims it and the fact it holds, with
            // both rules being triggered.
            process_lines(interactive, "(a p b) q z\n.run\n");
            collector.clear();
            interactive.process("S f O");
            CHECK(answers_contain(collector, "e f g"));
            collector.clear();
            interactive.process("S r O");
            CHECK(answers_contain(collector, "c r d"));
        }
    }

    // The holder whose claim went in turn is a claim that has been
    // withdrawn, just as the fact is, and what holds it goes in the same
    // manner as what holds the fact.
    SUBCASE("a statement holding that rule's pattern goes")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(a p b) => (c r d)
((a p b) q z) => (e f g)
x says ((a p b) q z)
)");
            interactive.process(prune);

            collector.clear();
            interactive.process("S says O");
            CHECK(collect_answers(collector).empty());
        }
    }

    // Saying a rule claims the rule itself as a node and nothing inside it,
    // thus the statement does not hold the fact as a claim.
    SUBCASE("a rule that a statement mentions stays, and so does the statement")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
a p b
x says ((a p b) => (c r d))
)");
            interactive.process(prune);

            collector.clear();
            interactive.process("S says O");
            CHECK(answers_contain(collector, "x says ((a p b) => (c r d))"));

            collector.clear();
            interactive.process("S p O");
            CHECK(collect_answers(collector).empty());
        }
    }

    // The negation functions as a tag attached to the condition's
    // statement, and so it is a holder of the fact; lacking this tag, the
    // rule activates upon `a p b` rather than upon its absence.
    SUBCASE("a negated condition stays negated")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
a p b
(X go Y, ¬(a p b)) => (X ok Y)
)");
            const std::string before = listed_rules(interactive, collector);
            interactive.process(prune);
            CHECK(listed_rules(interactive, collector) == before);
        }
    }

    // A statement used as a predicate is declared a relation type, and
    // the declaration holds the statement as one among other facts
    // concerning it. It was kept as rule text whether a rule read the
    // statement as a predicate or not, thus leaving behind a claimed
    // `(a p b) ~ ->`; subsequently, the .cleanup every prune recommends
    // identified the predicate as unused and removed its node, the rule's
    // condition, and the rule with it.
    SUBCASE("a declaration no rule reads goes, and .cleanup keeps the rule")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(a p b) => (c r d)
x (a p b) y
)");
            const std::string before = listed_rules(interactive, collector);
            interactive.process(prune);

            collector.clear();
            interactive.process("S ~ O");
            CHECK_FALSE(answers_contain(collector, "(a p b) ~ ->"));

            interactive.process(".cleanup");
            CHECK(listed_rules(interactive, collector) == before);
        }
    }

    // When a rule reads the statement as a predicate, the declaration
    // enables the rule to treat its condition as a fact in the first place:
    // it remains attached to the rule, whereas the data using the predicate
    // goes.
    SUBCASE("a declaration a rule reads stays, and the rule fires on new data")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(X (a p b) Y) => (X r Y)
x (a p b) y
)");
            interactive.process(prune);

            collector.clear();
            interactive.process("S (a p b) O");
            CHECK(collect_answers(collector).empty());

            process_lines(interactive, "u (a p b) v\n");
            collector.clear();
            interactive.process("S r O");
            CHECK(answers_contain(collector, "u r v"));
        }
    }

    // A collection consists of its members, thus eliminating the fact took
    // the collection and the rule holding it: the fact was not counted as
    // part of the rule, and `.list-rules` answered "No rules found."
    // Expressed using only ground members, the collection is a set
    // constant; if a variable is present among them, it becomes the rule's
    // own collection.
    SUBCASE("a rule's own collection holding the fact stays, and the rule with it")
    {
        for (const std::string rule : {"(X r Y) => (X about {(a p b)})", "(X r Y) => (X about {(a p b) Y})"})
        {
            for (const std::string prune : kRulePatternPrunes)
            {
                CAPTURE(rule);
                CAPTURE(prune);
                zelph::io::OutputCollector  collector;
                zelph::console::Interactive interactive(collector.sink());
                process_lines(interactive, "a p b\n" + rule + "\n");
                const std::string before = listed_rules(interactive, collector);
                REQUIRE(before.find("(a p b)") != std::string::npos);

                interactive.process(prune);
                CHECK(listed_rules(interactive, collector) == before);
            }
        }
    }

    // A set constant is a single node regardless of where its members are
    // specified, enabling data to hold the exact collection that a rule
    // holds. The collection stays with the rule, and the data holding it
    // goes as a holder of the fact.
    SUBCASE("data holding a collection the rule holds as well goes")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(X r Y) => (X about {(a p b)})
k has {(a p b)}
)");
            const std::string before = listed_rules(interactive, collector);
            interactive.process(prune);
            CHECK(listed_rules(interactive, collector) == before);

            collector.clear();
            interactive.process("S has O");
            CHECK(collect_answers(collector).empty());
        }
    }

    // Without a rule, the membership goes with the fact, the collection
    // goes with its member, and the statement with the collection.
    SUBCASE("a data collection holding the fact goes, as it does without a rule")
    {
        std::vector<std::pair<std::string, std::string>> cases{{"", ".prune-facts (a p b)"}};
        for (const std::string prune : kRulePatternPrunes)
            cases.emplace_back("(a p b) => (c r d)\n", prune);

        for (const auto& [rule, prune] : cases)
        {
            CAPTURE(rule);
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, rule + "k has {e (a p b)}\n");
            interactive.process(prune);

            collector.clear();
            interactive.process("S has O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("S in O");
            CHECK(collect_answers(collector).empty());
        }
    }

    // The membership functions as a holder and goes. Without a rule, the
    // container that loses the member departs alongside it; here, `k`
    // constitutes a component of the second rule and remains, as do the facts
    // associated with it. The membership was treated as rule text whenever
    // its container formed part of any rule: it stayed, and the walk advanced
    // from `k`, eliminating every fact holding it.
    SUBCASE("a membership in a node a rule names goes, and the node keeps its facts")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(a p b) => (c r d)
(X likes k) => (X happy yes)
(a p b) in k
z likes k
)");
            const std::string before = listed_rules(interactive, collector);
            interactive.process(prune);
            CHECK(listed_rules(interactive, collector) == before);

            collector.clear();
            interactive.process("S in O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("S likes O");
            CHECK(answers_contain(collector, "z likes k"));
        }
    }

    // A contradiction record is a set constant comprising the facts it is
    // about, and it goes with any of them, just as it would without a
    // rule; thus, reasserting the fact results in reporting the
    // contradiction once more. Alongside the rule, the record remained
    // unchanged, and the new assertion remained silent.
    SUBCASE("a contradiction record goes with the fact")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(a p b) => (c r d)
(X p Y, X s Y) => !
a p b
a s b
)");
            interactive.process(prune);

            collector.clear();
            interactive.process("S in O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            process_lines(interactive, "a p b\n");
            CHECK(any_event_contains(collector, "Found one or more contradictions"));
        }
    }

    // A prune represents a one-off adjustment to the network and does not
    // retain any trace, thus a statement that writes the fact afterwards
    // claims it anew.
    SUBCASE("a statement writing the fact afterwards claims it again")
    {
        for (const std::string prune : kRulePatternPrunes)
        {
            CAPTURE(prune);
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
(a p b) => (c r d)
(a p b) q z
)");
            interactive.process(prune);
            process_lines(interactive, "(a p b) q y\n");

            collector.clear();
            interactive.process("S p O");
            CHECK(answers_contain(collector, "a p b"));
        }
    }
}

// Documented and known behaviour, maintained deliberately. The variable
// form of `.prune-facts` eliminates the facts it matched and leaves
// untouched anything that holds them, in contrast to the ground form,
// which runs the cascade of `remove_node`. A contradiction record is a set
// holding the facts it pertains to, thus it stays, without the removed
// fact, and the run that asserting the fact again starts cannot write the
// record anew: the prior membership continues to occupy its node. The
// error names that node, and `.remove` with its identifier takes it and
// the record away. Implementing a cascade in the variable form would
// change the bulk prune path used to construct the published Wikidata
// networks; `rules.md` and `.help .prune-facts` describe the behaviour
// instead.
TEST_CASE("pruning: the variable form leaves a contradiction record behind")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
(X p Y, X r Y) => !
a r b
a p b
)");

    // The reference: the ground form takes the record along, and the
    // contradiction is reported once more.
    SUBCASE("the ground form takes the record along")
    {
        interactive.process(".prune-facts a p b");
        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        process_lines(interactive, "a p b\n");
        CHECK(any_event_contains(collector, "Found one or more contradictions"));
    }

    // The error that asserting the fact once more ends in.
    const auto assert_again = [&]
    {
        try
        {
            interactive.process("a p b");
        }
        catch (const std::exception& e)
        {
            return std::string(e.what());
        }
        return std::string();
    };

    SUBCASE("the variable form leaves the record without the fact")
    {
        interactive.process(".prune-facts A p b");
        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "(a r b) in @{(a r b)}"));
    }

    SUBCASE("asserting the fact again then fails, naming a node")
    {
        interactive.process(".prune-facts A p b");
        CHECK(std::regex_search(assert_again(), std::regex(R"(node [0-9]+ already exists)")));
    }

    SUBCASE(".remove of that node takes the record, and the contradiction is found anew")
    {
        interactive.process(".prune-facts A p b");
        const std::string error = assert_again();
        std::smatch       named;
        REQUIRE(std::regex_search(error, named, std::regex(R"(node ([0-9]+) already exists)")));

        interactive.process(".remove " + named[1].str());
        collector.clear();
        interactive.process(".run");
        CHECK(any_event_contains(collector, "Found one or more contradictions"));

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "(a p b) in {(a p b) (a r b)}"));
        CHECK_FALSE(answers_contain(collector, "(a r b) in @{(a r b)}"));
    }
}

// A refuted fact is the SECOND way is_asserted_fact says no, and the hint knew
// only the first: it reported "exists only as a rule's own pattern" about a
// statement no rule mentions, which names the wrong mechanism and sends the
// reader looking for a rule that is not there. What the command DOES is right
// either way -- the prune commands remove claims, and a refutation is the claim
// that the fact does not hold, so there is no positive claim of it to take.
//
// Whether the commands should extend to a refuted fact at all is a separate,
// undecided question; this pins only that the reason given is the true one.
TEST_CASE("pruning: a refuted fact is not reported as a rule's pattern")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
¬(a p b)
c q d
)");
    collector.clear();
    interactive.process(".prune-facts (a p b)");
    CHECK(any_output_contains(collector, "Pruned 0"));
    CHECK(any_event_contains(collector, "REFUTED"));
    CHECK_FALSE(any_event_contains(collector, "only as a rule's own pattern"));

    // The control, so the hint is read as being about the refutation and not
    // about the command: an ordinary fact beside it prunes and says nothing.
    collector.clear();
    interactive.process(".prune-facts (c q d)");
    CHECK(any_output_contains(collector, "Pruned 1"));
    CHECK_FALSE(any_event_contains(collector, "REFUTED"));
}

// ---------------------------------------------------------------------------
// .prune-nodes <variable> (<conditions>) -- a conjunction plus the one thing
// a conjunction cannot say on its own: which of its variables names the
// victims. The motivating case is the Wikidata prune script, where
// ".prune-nodes A P31 Qx" matches DIRECT instances only and 60 hand-listed
// catalogue classes stood in for one walk down the hierarchy.
//
// The focus operator was the first idea and does not work: "*A" inside a
// conjunction makes the condition EVALUATE to the focused node, so the
// condition is replaced by a bare variable and disappears from the set.
// ---------------------------------------------------------------------------

namespace
{
    // star -> body, dwarf -> star; sirius and altair are instances below
    // body, mercury is not.
    constexpr const char* kSky =
        "star P279 body\n"
        "dwarf P279 star\n"
        "sirius P31 dwarf\n"
        "altair P31 star\n"
        "mercury P31 planet\n";
} // namespace

TEST_CASE("pruning: a conjunction deletes what its named variable binds")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, kSky);
        collector.clear();

        interactive.process(".prune-nodes A (A P31 C, C P279∗ body)");

        // Two victims and the two P31 facts they were the subject of -- the
        // same numbers the equivalent single-fact form reports, which it only
        // does because remove_node's collateral is counted as facts.
        CHECK(any_output_contains(collector, "Pruned 2 matching facts and 2 nodes"));

        collector.clear();
        interactive.process("A P31 B");
        CHECK(answers_contain(collector, "mercury P31 planet"));
        CHECK_FALSE(any_output_contains(collector, "sirius"));
        CHECK_FALSE(any_output_contains(collector, "altair"));

        // The other conditions are the FILTER that selected the victims, not
        // a second deletion list: the class hierarchy they walked survives.
        collector.clear();
        interactive.process("A P279 B");
        CHECK(answers_contain(collector, "star P279 body"));
        CHECK(answers_contain(collector, "dwarf P279 star")); });
}

TEST_CASE("pruning: which condition reaches the terminal does not matter")
{
    // optimize_order schedules a path condition after whatever binds an end,
    // so the terminal is reached through the closure rather than through
    // unification -- and that terminal returned early in prune mode (the
    // v1 restriction ≈ still keeps). Written in either order, one of the two
    // terminals fires, and both have to collect.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, kSky);
        collector.clear();

        interactive.process(".prune-nodes A (C P279∗ body, A P31 C)");
        CHECK(any_output_contains(collector, "Pruned 2 matching facts and 2 nodes"));

        collector.clear();
        interactive.process("A P31 B");
        CHECK(answers_contain(collector, "mercury P31 planet"));
        CHECK_FALSE(any_output_contains(collector, "sirius")); });
}

TEST_CASE("pruning: the named variable may be any of the conjunction's")
{
    // The whole point of naming it: the same conjunction deletes the classes
    // instead of their instances when C is named.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, kSky);
        collector.clear();

        interactive.process(".prune-nodes C (A P31 C, C P279∗ body)");

        collector.clear();
        interactive.process("A P31 B");
        CHECK(answers_contain(collector, "mercury P31 planet"));
        CHECK_FALSE(any_output_contains(collector, "star"));
        CHECK_FALSE(any_output_contains(collector, "dwarf")); });
}

TEST_CASE("pruning: a named variable the pattern does not have is refused")
{
    // A typo in the one token that decides what gets deleted has to be an
    // error, not a run that binds nothing and reports "Pruned 0".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, kSky);
        collector.clear();

        std::string message;
        try
        {
            interactive.process(".prune-nodes Z (A P31 C, C P279∗ body)");
        }
        catch (const std::exception& ex)
        {
            message = ex.what();
        }

        CHECK(message.find("no variable Z") != std::string::npos);
        // And it says which ones there are, since that is what a typo needs.
        CHECK(message.find("A, C") != std::string::npos);

        // Nothing was deleted on the way to the message.
        collector.clear();
        interactive.process("A P31 B");
        CHECK(answers_contain(collector, "sirius P31 dwarf")); });
}

TEST_CASE("pruning: .prune-facts does not take a named variable")
{
    // A leading variable names what gets DELETED, which is .prune-nodes'
    // business; .prune-facts removes the facts its pattern matches, and a
    // conjunction matches several per solution.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, kSky);
        collector.clear();

        CHECK_THROWS_WITH_AS(interactive.process(".prune-facts A (A P31 C, C P279∗ body)"),
                             doctest::Contains("takes a pattern only"),
                             std::runtime_error);

        collector.clear();
        interactive.process("A P31 B");
        CHECK(answers_contain(collector, "sirius P31 dwarf")); });
}

TEST_CASE("pruning: the single-fact form keeps its reading")
{
    // ".prune-nodes A rel b" is three tokens whose first is a variable, i.e.
    // exactly what the new form starts with. The parenthesis is what tells
    // them apart -- a statement needs three elements, so a variable followed
    // by a bracketed pattern is never one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a rel b\nc rel b\nkeep other b\n");
        collector.clear();

        interactive.process(".prune-nodes A rel b");
        CHECK(any_output_contains(collector, "Pruned 2 matching facts and 2 nodes"));

        collector.clear();
        interactive.process("S other O");
        CHECK(answers_contain(collector, "keep other b"));

        // And the two-variable refusal still points at the way out.
        CHECK_THROWS_WITH_AS(interactive.process(".prune-nodes S other O"),
                             doctest::Contains("exactly one variable"),
                             std::runtime_error); });
}

TEST_CASE("pruning: a named variable works over a single condition too")
{
    // Nothing about the reading needs a conjunction, and a rule with one
    // exception fewer is a rule easier to document.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, kSky);
        collector.clear();

        interactive.process(".prune-nodes A (A P31 star)");
        CHECK(any_output_contains(collector, "Pruned 1 matching facts and 1 nodes"));

        collector.clear();
        interactive.process("A P31 B");
        CHECK(answers_contain(collector, "sirius P31 dwarf"));
        CHECK_FALSE(any_output_contains(collector, "altair")); });
}

TEST_CASE("pruning: a conjunction that matches nothing leaves nothing behind")
{
    // Evaluating the pattern MATERIALIZES it, which is why the construction
    // runs in a scratch cluster -- and a conjunction builds more of it than a
    // single fact does (a set node, a PartOf fact per member, the tags).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a rel b\n");
        collector.clear();

        interactive.process(".prune-nodes X (X rel nothing, X other thing)");
        CHECK(any_output_contains(collector, "Pruned 0 matching facts and 0 nodes"));

        collector.clear();
        interactive.process("S rel O");
        CHECK(answers_contain(collector, "a rel b"));

        // The pattern's own vocabulary is not in the graph afterwards.
        CHECK_THROWS_AS(interactive.process(".node nothing"), std::runtime_error); });
}

// ---------------------------------------------------------------------------
// remove_node drops the structure cache TARGETED rather than wholesale, so a
// bulk removal can keep what it did not touch. The cases below are the ones a
// stale entry would corrupt: a victim whose components are shared with a
// survivor, and a removal whose cascade reaches a fact through another fact.
//
// Measured motivation, counters only: one prune removing 99 893 nodes did
// 99 900 full cache clears and 199 797 cold structure walks; it now does 4 and
// 99 904.
// ---------------------------------------------------------------------------

TEST_CASE("removal: a bulk prune leaves the survivors structurally intact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Every victim shares its predicate and its object with the next, so
        // a cached reading kept from before an earlier removal would be read
        // back for a node whose adjacency has since changed.
        process_lines(interactive, R"(
v1 rel target
v2 rel target
v3 rel target
keep rel target
v1 other keep
v2 other keep
)");
        collector.clear();
        interactive.process(".prune-nodes A other keep");
        // Two victims, and FOUR facts: the two matched "other" facts plus the
        // "rel target" fact each victim took with it.
        CHECK(any_output_contains(collector, "Pruned 4 matching facts and 2 nodes"));

        // v1 and v2 are gone with both of their facts; v3 and keep are not.
        collector.clear();
        interactive.process("S rel target");
        const auto answers = collect_answers(collector);
        CHECK(answers_contain(collector, "v3 rel target"));
        CHECK(answers_contain(collector, "keep rel target"));
        CHECK(answers.size() == 2);

        // And nothing is left that reads as a self-fact -- the shape an
        // incomplete fact degenerates into.
        CHECK_FALSE(any_output_contains(collector, ":rel target"));

        collector.clear();
        interactive.process("S other O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("removal: a cascade through a nested fact survives a bulk prune")
{
    // The cascade runs upwards through facts, which is exactly where a
    // structure read from the cache decides what goes: `(x (a p b) y)` is
    // reached from `a p b`, which is reached from `b`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c p b
x (a p b) y
u (c p b) w
survivor p elsewhere
)");
        collector.clear();
        interactive.process(".prune-nodes B p b");

        // Both nested facts go with the inner facts that go with b.
        collector.clear();
        interactive.process("S (a p b) O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S (c p b) O");
        CHECK(collect_answers(collector).empty());

        // The untouched branch is still there and still reads as itself.
        collector.clear();
        interactive.process("S p elsewhere");
        CHECK(answers_contain(collector, "survivor p elsewhere")); });
}

TEST_CASE("removal: a bulk prune survives a save/load round trip")
{
    // A fact minus one of its parts is not recognisable as incomplete -- it
    // reads as a self-fact and goes into the .bin. That is what the wholesale
    // clear was protecting, so the targeted one has to be pinned against it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_bulk_prune_roundtrip.bin";
        std::filesystem::remove(out);

        process_lines(interactive, R"(
v1 rel doomed
v2 rel doomed
v3 rel doomed
keep rel fine
)");
        interactive.process(".prune-nodes A rel doomed");
        interactive.process(".save " + out.string());
        interactive.process(".new");
        interactive.process(".load " + out.string());

        collector.clear();
        interactive.process("S rel O");
        CHECK(answers_contain(collector, "keep rel fine"));
        CHECK(collect_answers(collector).size() == 1);

        std::filesystem::remove(out); });
}

// ---------------------------------------------------------------------------
// Two implementations of one removal
// ---------------------------------------------------------------------------

namespace
{
    // Every node the graph still holds. Node ids are hashes or creation
    // positions, so two sessions that build the same script build the same
    // ids -- which makes this an EXACT comparison of two resulting graphs,
    // not a sampled one.
    std::set<zelph::network::Node> live_nodes(zelph::network::Reasoning* const graph)
    {
        std::set<zelph::network::Node> out;
        const auto                     view = graph->get_all_nodes_view();
        for (auto it = view.begin(); it != view.end(); ++it)
            out.insert(it->first);
        return out;
    }

    // Awkward on purpose: a fact ABOUT a fact, a set constant holding a
    // victim (the container rule), a nested fact, a multi-object fact, a rule
    // whose condition names the pattern (a rule pattern is not data), and
    // enough victims to reach every worker of the pool.
    const char* const removal_fixture = R"(
keep rel other
v1 p b
(v1 p b) q c
v2 in {v2 keep}
(v3 r d) s e
v3 multi o1 o2
(X rel target) => (X flagged yes)
%(loop [i :range [1 61]] (zelph/fact (string "v" i) "rel" "target"))
)";
} // namespace

TEST_CASE("pruning: the batched cascade removes exactly what removing one by one removes")
{
    // A prune collects the doomed closures of a whole batch on the thread
    // pool and erases the union; `.remove` still walks one node at a time and
    // erases as it goes. The two must agree, and nothing about the OUTCOME
    // can say whether they do -- which is why the only honest test is to run
    // both and compare the graphs they leave behind.
    //
    // What could make them differ is exactly what the batch argument has to
    // rule out: a closure taken against the UNMUTATED graph reaches nodes
    // that the sequential order had already deleted, and a victim that is
    // part of another victim's closure is walked by whichever comes first.
    std::set<zelph::network::Node> batched;
    std::set<zelph::network::Node> one_by_one;
    size_t                         before = 0;

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, removal_fixture);
        before = live_nodes(interactive.graph()).size();
        interactive.process(".prune-nodes A rel target");
        batched = live_nodes(interactive.graph());
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, removal_fixture);

        auto* const graph  = interactive.graph();
        const auto  rel    = graph->get_node("rel");
        const auto  target = graph->get_node("target");
        REQUIRE(rel != 0);
        REQUIRE(target != 0);

        // The same victims the pattern binds, removed the old way.
        std::vector<zelph::network::Node> victims;
        for (const auto v : graph->get_fact_subjects(rel, target))
            victims.push_back(v);
        REQUIRE(victims.size() == 60);

        for (const auto v : victims)
        {
            if (!graph->exists(v)) continue; // taken by an earlier one
            graph->remove_node(v);
        }
        one_by_one = live_nodes(graph);
    }

    // Not vacuous: both really destroyed most of the graph, and something
    // survived. Two paths that removed NOTHING would agree just as well.
    REQUIRE(before > 100);
    REQUIRE(batched.size() < before / 2);
    REQUIRE(!batched.empty());

    CHECK(batched.size() == one_by_one.size());
    CHECK(batched == one_by_one);
}

TEST_CASE("pruning: a batch reports its progress from INSIDE the collection phase" * doctest::test_suite("slow"))
{
    // A guard on a DIAGNOSTIC, and it is worth one because the diagnostic is
    // the only thing a multi-day operation offers from outside the process.
    //
    // The progress line used to advance once per batch of 100 000 victims.
    // On the full Wikidata dump one such batch took twelve minutes -- ~90 % of
    // it in the collection phase, which said nothing -- so the log stood silent
    // for twelve minutes at a time and a slow run was indistinguishable from a
    // stuck one. The batch cannot simply be made smaller: its size is what
    // bounds the fact-structure cache and what the union argument in
    // collect_doomed rests on.
    //
    // 100 001 victims because the reporting threshold IS the batch size, and
    // the interesting case is the one where a single batch has to speak while
    // it works. The fixture is built through the Janet API rather than by
    // typing 100 001 statements: parsing them costs 23 s, creating them 0.5 s,
    // and what is under test is the removal.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("%(let [hub (zelph/resolve \"hub\") rel (zelph/resolve \"rel\")] "
                            "(for i 0 100001 (zelph/fact (zelph/resolve (string \"n\" i)) rel hub)))");
        collector.clear();

        interactive.process(".prune-nodes A rel hub");

        // In order, because the ORDER is the whole point: an "examined" line
        // that only appears after the batch has been erased would report
        // exactly what the old code already reported.
        std::vector<size_t> examined_before_first_removal;
        bool                removal_seen = false;
        for (const auto& e : collector.events())
        {
            const auto pos = e.text.find(" of 100001 node(s) ");
            if (pos == std::string::npos) continue;

            if (e.text.find("node(s) removed") != std::string::npos)
            {
                removal_seen = true;
                continue;
            }
            if (e.text.find("node(s) examined") == std::string::npos) continue;
            if (removal_seen) continue;

            examined_before_first_removal.push_back(std::stoull(e.text.substr(0, pos)));
        }

        // The batch spoke while it was collecting, not only after completion,
        // and that -- not how OFTEN it spoke -- is the property. The line is
        // emitted by a polling loop that yields while the workers run
        // (reasoning_pruning.cpp), so the number of lines measures the
        // scheduler: a machine whose workers cross several steps between two
        // observations reports fewer. Windows CI reported 2 where this once
        // demanded 5. The regression it guards against produces ZERO, because
        // the old code reported after the batch rather than during it.
        REQUIRE(removal_seen); // not vacuous: the prune really ran
        CHECK(examined_before_first_removal.size() >= 1);

        // Monotone and inside the job it reports against -- a counter that
        // restarts per worker, or overshoots the batch, is a wrong number
        // rather than a missing one.
        size_t previous = 0;
        for (const size_t seen : examined_before_first_removal)
        {
            CHECK(seen > previous);
            CHECK(seen <= 100001);
            previous = seen;
        } });
}

TEST_CASE("removal: a removed edge takes its probability with it")
{
    // Network::remove used to disconnect() every edge, and disconnect erases
    // the edge's weight along with it. Doing the whole removal under one lock
    // triple means erasing those weights by hand, and nothing about the GRAPH
    // says whether that was done: a stale weight is invisible until the same
    // edge comes back.
    //
    // It can come back exactly, which is what makes this testable. A fact
    // node's id IS the hash of its triple, and a weight is keyed on the hash
    // of the edge it belongs to, so re-asserting a removed fact lands on the
    // same node and the same weight key. A weight that outlived its edge is
    // then inherited by the new one.
    zelph::io::OutputCollector collector;
    zelph::network::Zelph      z(collector.sink());

    const auto a = z.node("a");
    const auto p = z.node("p");
    const auto b = z.node("b");
    z.fact(p, z.core.IsA, {z.core.RelationTypeCategory});

    // Asserted as WRONG: probability 0 is what puts an entry in the weight map
    // at all -- connect() stores nothing for the default probability of 1.
    const auto wrong = z.fact(a, p, {b}, 0);
    REQUIRE(wrong != 0);
    REQUIRE(z.check_fact(a, p, {b}).is_wrong());

    z.remove_node(wrong);
    REQUIRE(!z.exists(wrong));

    // The same triple again, this time with nothing said about its
    // probability. It must come back as an ordinary fact.
    const auto again = z.fact(a, p, {b});
    REQUIRE(again == wrong); // the same node: the id is the hash of the triple

    const auto answer = z.check_fact(a, p, {b});
    CHECK(answer.is_known());
    CHECK(!answer.is_wrong()); // a weight that outlived its edge would say WRONG
    CHECK(answer.is_correct());
}
