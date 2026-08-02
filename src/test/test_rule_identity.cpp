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

TEST_CASE("rule identity: talking about a rule does not assert it")
{
    // A fact node exists exactly when its edges exist, and the edges of
    // `((X p Y) => (X q Y)) is nice` include those of the rule it mentions.
    // So the rule FIRED: entering `a p b` derived `a q b`, although nobody
    // had claimed the rule -- only that it is nice. Statements about
    // statements are what zelph leads with, and this made a statement about
    // a RULE impossible to write.
    //
    // An asserted rule is a part of nothing; a mentioned one is the subject,
    // the predicate or an object of the statement that mentions it. That is
    // decidable from the graph, so nothing has to be remembered and a
    // save/load round trip cannot lose it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("as the subject of an ordinary fact")
        {
            interactive.process("((X p Y) => (X q Y)) is nice");
            collector.clear();
            interactive.process(".list-rules");
            CHECK_FALSE(any_output_contains(collector, "=> (X q Y)"));

            interactive.process("a p b");
            collector.clear();
            interactive.process("a q Z");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("as the conclusion of another rule")
        {
            // The inner rule must not hold for EVERY predicate just because
            // the outer rule mentions it -- `foo` was never declared
            // transitive.
            interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
            process_lines(interactive, R"(
a foo b
b foo c
)");
            collector.clear();
            interactive.process("a foo Z");
            CHECK(answers_contain(collector, "a foo b"));
            CHECK_FALSE(any_output_contains(collector, "a foo c"));
        }
        SUBCASE("a rule that was actually asserted still fires")
        {
            interactive.process("(X p Y) => (X q Y)");
            interactive.process("a p b");
            collector.clear();
            interactive.process("a q Z");
            CHECK(answers_contain(collector, "a q b"));
        }
        SUBCASE("mentioning a rule with variables leaves the asserted one alone")
        {
            // Each statement names its own variables, so the mentioned rule
            // is a different node from the asserted one and both keep their
            // meaning.
            interactive.process("(X p Y) => (X q Y)");
            interactive.process("((X p Y) => (X q Y)) is nice");
            interactive.process("a p b");
            collector.clear();
            interactive.process("a q Z");
            CHECK(answers_contain(collector, "a q b"));
        } });
}

TEST_CASE("rule identity: a mentioned rule survives a save/load round trip as a mention")
{
    // The distinction is a property of the graph, not of anything the
    // session remembers, so it does not have to be written to the .bin --
    // and cannot be lost by not writing it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_mentioned_rule.bin";
        std::filesystem::remove(out);

        interactive.process("((X p Y) => (X q Y)) is nice");
        interactive.process(".save " + out.string());
        interactive.process(".new");
        interactive.process(".load " + out.string());
        interactive.process(".auto-run");

        interactive.process("a p b");
        collector.clear();
        interactive.process("a q Z");
        CHECK_FALSE(any_output_starts_with(collector, "Answer:"));

        std::filesystem::remove(out); });
}

TEST_CASE("rule identity: two rules differing only in their container node are one rule")
{
    // A container is not hash-consed, so two spellings of `@{Y}` are two
    // NODES -- and a node with no fact structure of its own used to be
    // compared by identity, which made the two rules different. That is not a
    // cosmetic problem: rebuild_rule alpha-renames an inner rule and rebuilds
    // its container with the renamed variable, so a rule GENERATOR produced
    // another copy of its rule on every run and the fixpoint never arrived.
    //
    // The container is now read by its members, exactly as a conjunction set
    // is. Both directions are checked: same shape collapses, different
    // members stay apart.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X p Y) => (X likes {Y})");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);

        // Alpha-equivalent, and its container is a different node.
        interactive.process("(A p B) => (A likes {B})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);

        // A container with a DIFFERENT member is a different rule.
        interactive.process("(A p B) => (A likes {A})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2);

        // Two members are not one member either.
        interactive.process("(A p B) => (A likes {A B})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3); });
}

TEST_CASE("rule identity: a set constant keeps deciding by identity")
{
    // A set constant hash-conses, so `{a b}` written twice IS one node and
    // identity settles it. The container test must not reach it -- the
    // is_hash gate in front of the adjacency scan is what keeps that scan off
    // every hash node the walk passes.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in {a b}) => (X flagged yes)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);

        interactive.process("(A in {a b}) => (A flagged yes)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);

        interactive.process("(A in {a c}) => (A flagged yes)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2); });
}
