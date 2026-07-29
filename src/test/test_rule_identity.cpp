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
// Rule identity: a rule statement that says nothing new is skipped.
//
// Facts hash-cons, so asserting one twice is free. Rules did not: they carry
// variables, variables are fresh per statement, and a node built from fresh
// variables is a fresh node. The second occurrence of a rule was therefore a
// second rule -- same consequences, twice the unification work. That bites
// hardest where it is least expected: .load restores the rules but not the
// Janet side of a module, so `.load x.bin` + `.import <same module>` (the
// documented way to get &-literals back) used to DOUBLE the entire rule set.
//
// The tests below pin both directions. Missing a duplicate only costs what
// the old behaviour cost; treating two DIFFERENT rules as one would silently
// delete knowledge, so the negative cases carry the weight here.
// ---------------------------------------------------------------------------

namespace
{
    // The "Nodes: N" line of .stat -- the cheapest way to see whether a
    // statement left anything in the graph.
    std::string node_count(const zelph::io::OutputCollector& collector)
    {
        for (const auto& e : collector.events())
        {
            const std::string t = normalize(e.text);
            if (t.rfind("Nodes:", 0) == 0) return t;
        }
        return {};
    }

    // How many rules ".list-rules" just listed. Comparing counts rather
    // than rendered text keeps these tests independent of the identifier
    // markup, which the listing does not strip.
    std::size_t listed_rules(const zelph::io::OutputCollector& collector)
    {
        std::size_t n = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).find("=>") != std::string::npos) ++n;
        return n;
    }
}

TEST_CASE("rule identity: the same rule entered twice stays one rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);

        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);

        // ... and the surviving rule still works.
        interactive.process("rel is transitive");
        interactive.process("a rel b");
        interactive.process("b rel c");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("a rel X");
        CHECK(answers_contain(collector, "a rel c")); });
}

TEST_CASE("rule identity: renaming the variables does not make a new rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X p Y) => (X q Y)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);

        interactive.process("(_alpha p _beta) => (_alpha q _beta)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1); });
}

TEST_CASE("rule identity: the rollback leaves no debris")
{
    // The duplicate is built before it can be recognised, so the check has
    // to undo the construction -- patterns, conjunction set, AND the
    // variables they are made of. Anything left behind would show up as
    // graph growth for a statement that changed nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        collector.clear();
        interactive.process(".stat");
        const std::string before = node_count(collector);
        REQUIRE_FALSE(before.empty());

        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        collector.clear();
        interactive.process(".stat");
        CHECK(node_count(collector) == before); });
}

TEST_CASE("rule identity: different rules stay different")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a different predicate")
        {
            interactive.process("(X p Y) => (X q Y)");
            interactive.process("(X p Y) => (X r Y)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a different sharing pattern between the variables")
        {
            // Same shape, different rule: the first joins on Y, the second
            // does not. A fingerprint alone cannot tell them apart -- only
            // the bijection can.
            interactive.process("(X p Y, Y p Z) => (X q Z)");
            interactive.process("(X p Y, Z p W) => (X q W)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a single-condition rule is not a set")
        {
            // Two rules with ONE condition each and the same consequence
            // SHAPE. Assuming the => subject is always a condition set
            // makes both look like "{} => (v lcmp v)" and collapses them --
            // which silently deleted a recursion rule of common-arithmetic.
            interactive.process("((A cons R) lcmp (B cons S)) => (R lcmp S)");
            interactive.process("(N cmp M) => (N lcmp M)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("negation is part of the rule")
        {
            interactive.process("(A is yellow, ¬(A is green)) => (A mark yellow)");
            interactive.process("(A is yellow, A is green) => (A mark yellow)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "¬"));
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a != guard is part of the rule")
        {
            interactive.process("(A p X, A p Y, X != Y) => (A pair X Y)");
            interactive.process("(A p X, A p Y) => (A pair X Y)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        } });
}

TEST_CASE("rule identity: a rule built inside a cluster stays in that cluster")
{
    // The check runs the construction in a scratch cluster of its own. What
    // survives has to be handed back to the cluster the user activated,
    // otherwise .cluster-drop would no longer roll the rule back.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        interactive.process("(X cp Y) => (X cq Y)");
        interactive.process(".cluster-drop exp");
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "(X cq Y)")); });
}
