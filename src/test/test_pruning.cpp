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

#include <filesystem>

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
