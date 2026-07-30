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
// The operator spellings of the core nodes ("~", "=>", "!", "cons", "nil",
// "in", ...) are not entries of the name maps: the parser knows them, and so
// does Zelph::node(), which consults that table after the language map.
//
// Two consequences used to be wrong, and both are about the same asymmetry.
//
//  * The NON-creating lookup (Zelph::get_node) did not consult the table, so
//    every command that takes a node by name refused the predicate the line
//    above had just used: ".node ~" answered "No node found with name '~'".
//  * The display consulted it only AFTER falling back to other languages, so
//    a core node that had acquired a foreign name was printed under it. In
//    the zelph language "!" came out as "contradiction" -- and re-entering
//    that line addressed a different node, breaking the rule that zelph's
//    output is re-enterable as input.
// ---------------------------------------------------------------------------

TEST_CASE("core names: a core predicate can be addressed by its operator")
{
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        process_lines(interactive, R"(
socrates ~ human
plato ~ human
socrates likes plato
)");

        collector.clear();
        interactive.process(".node ~");
        CHECK(any_event_contains(collector, "Core node: ~"));

        // Same resolution path as .node, exercised through a command that
        // walks the adjacency instead of describing the node.
        collector.clear();
        interactive.process(".out ~ 3");
        CHECK(any_output_contains(collector, "Outgoing connected nodes of"));

        // A spelled-out core name works as well as a symbolic one.
        collector.clear();
        interactive.process(".node nil");
        CHECK(any_event_contains(collector, "Core node: nil")); });
}

TEST_CASE("core names: an unknown name is still unknown")
{
    run_single_core_mode([](auto&, auto& interactive)
                         {
        interactive.process("socrates ~ human");

        // The fallback must not turn every miss into a hit.
        CHECK_THROWS(interactive.process(".node aristotle")); });
}

TEST_CASE("core names: a name in the current language wins over the operator")
{
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        // The Wikidata integration script maps the core predicates onto
        // property IDs exactly like this.
        process_lines(interactive, R"(
socrates ~ human
.name ~ wikidata P31
.lang wikidata
)");

        collector.clear();
        interactive.process("X ~ human");
        // Echo and answer are in the wikidata language, where the node has a
        // name of its own: P31, not the operator.
        CHECK(any_event_contains(collector, "X P31 human")); });
}

TEST_CASE("core names: the core vocabulary survives .cleanup and .remove")
{
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        interactive.process("socrates ~ human");

        // Four core nodes carry no edges in a fresh network (the
        // contradiction marker, nil, and the conjunction and negation tags),
        // which made them look isolated. Cleaning them away left a network
        // that failed on the next rule concluding "!".
        interactive.process(".cleanup");

        // Reachable by name since core spellings resolve; it must not be
        // possible to delete the engine's vocabulary on purpose either.
        CHECK_THROWS(interactive.process(".remove ~"));

        collector.clear();
        process_lines(interactive, R"(
(A ~ human, A ~ dog) => !
socrates ~ dog
)");
        CHECK(has_contradiction(collector));
        CHECK_FALSE(any_event_contains(collector, "does not exist")); });
}

TEST_CASE("core names: pruning skips the core vocabulary instead of aborting")
{
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        interactive.process("socrates ~ human");

        // "A ~ ->" binds A to every declared relation type -- which includes
        // the core predicates. Deleting those would break the engine;
        // refusing the command would leave the prune half-done, since nodes
        // are removed one after another.
        collector.clear();
        interactive.process(".prune-nodes A ~ ->");
        CHECK(any_output_contains(collector, "Kept 4 core node(s)"));

        collector.clear();
        interactive.process("X ~ human");
        CHECK(answers_contain(collector, "socrates ~ human")); });
}

TEST_CASE("core names: a foreign name does not replace the operator in output")
{
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        process_lines(interactive, R"(
.lang en
contradiction is unsatisfiable
.lang zelph
.name ! en contradiction
)");

        collector.clear();
        interactive.process("! P O");

        // The fact now hangs off the core node, which has an English name and
        // none in zelph. Printing that English name would produce a line that
        // reads back as a new node.
        CHECK(answers_contain(collector, "! is unsatisfiable"));
        CHECK_FALSE(any_output_contains(collector, "contradiction is unsatisfiable")); });
}
