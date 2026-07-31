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
