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

#include "network/reasoning.hpp"

#include <filesystem>
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

TEST_CASE("pruning: what a prune costs in name-map walks does not grow with its victims")
{
    // A guard on a COST, not on a result, and the reason it exists is
    // measured: `remove_node` erased names per node, each erase walking the
    // whole reverse map end to end, so pruning the full Wikidata dump ran at
    // 1.11 nodes per second with 97.7 % of the time in that walk -- two
    // months for a script that has to finish overnight. Nothing about the
    // OUTCOME changes when that shape comes back, which is precisely why the
    // tests above cannot see it.
    //
    // What is asserted is the shape rather than a number: a prune costs the
    // same few walks whether it kills one node or twenty. The constant part
    // is the command's own housekeeping (the pattern node it materialised),
    // which is per command and therefore harmless.
    //
    // The counter is hardware-independent, so this needs no quiet machine.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        auto* const graph = interactive.graph();
        REQUIRE(graph != nullptr);

        interactive.process("one rel small");

        const uint64_t before_small = graph->name_map_scans();
        interactive.process(".prune-nodes A rel small");
        const uint64_t small = graph->name_map_scans() - before_small;

        for (int i = 0; i < 20; ++i)
            interactive.process("n" + std::to_string(i) + " rel big");

        const uint64_t before_big = graph->name_map_scans();
        interactive.process(".prune-nodes A rel big");
        const uint64_t big = graph->name_map_scans() - before_big;

        REQUIRE(small > 0); // the counter is wired at all
        CHECK(big == small); });
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

    SUBCASE("asserting the statement makes it data again")
    {
        // The control: asserting revokes the marking, and from then on the
        // documented cascade applies -- the fact goes, and the rule built on
        // it goes with it.
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
        CHECK(any_output_contains(collector, "No rules found"));
    }

    SUBCASE("a DERIVED ground fact is data")
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

TEST_CASE("pruning: a batch reports its progress from INSIDE the collection phase")
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
