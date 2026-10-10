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

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

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

// The pairing of a rule's conditions and the pairing of its variables
// constitute a single search, not two separate ones: in
// (A p B, B p A) => (A q B), the conditions match each other under A -> A and
// under A -> B, yet only the first holds for the conclusion as well. The
// conditions were paired first, and that pairing was preserved, meaning
// whether the conclusion matched depended on the order in which the set held
// its members -- on node ids. An equivalent rule was subsequently taken for a
// new rule, and a rule .explain was given as text for a missing rule,
// appearing on certain calls but not on others. The variants spell the rule
// using different variable names and present the conditions in both orders,
// thereby altering the ids.
TEST_CASE("rule identity: symmetric conditions are paired together with the conclusion")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A p B, B p A) => (A q B)");
        for (const auto& [x, y] : std::vector<std::pair<std::string, std::string>>{{"C", "D"}, {"X", "Y"}, {"U", "V"}, {"M", "N"}, {"P", "Q"}, {"K", "L"}, {"E", "F"}, {"G", "H"}})
        {
            interactive.process("(" + x + " p " + y + ", " + y + " p " + x + ") => (" + x + " q " + y + ")");
            interactive.process("(" + y + " p " + x + ", " + x + " p " + y + ") => (" + x + " q " + y + ")");
        }
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1); });
}

TEST_CASE("rule identity: a rule whose variables sit only in a nested conjunction is compared by its structure")
{
    // The template-variable store captures only the variables associated
    // with hash components, and a conjunction set does not qualify as one --
    // it holds its members through membership facts. Thus, the inner rule
    // `((X q H), (X p H)) => !`, along with any fact constructed from it,
    // lacks an entry, and rule identity read it as a variable-free node,
    // comparing it by identity. The second statement of such a rule
    // constituted a second rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto enters_once = [&](const std::string& rule)
        {
            interactive.process(rule);
            collector.clear();
            interactive.process(".stat");
            const std::string before = node_count(collector);
            REQUIRE_FALSE(before.empty());

            interactive.process(rule);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
            collector.clear();
            interactive.process(".stat");
            CHECK(node_count(collector) == before);
        };

        SUBCASE("the conjunction belongs to the consequence")
        {
            enters_once("(G go H) => (((X q H), (X p H)) => !)");
        }
        SUBCASE("the conjunction belongs to a rule one level further down")
        {
            enters_once("(G go H) => ((a is up) => (((X q H), (X p H)) => !))");
        }
        SUBCASE("the conjunction belongs to a switch's rule")
        {
            enters_once("(K is on) => ((X p Y, X r Y) => (X likes Y))");
        }
        SUBCASE("the conjunction holds a collection of the generator")
        {
            enters_once("(G go H) => ((X s Y) => (X p @{(Z q Y)})) ((X p @{(Z q H)}, X r Y) => (X likes Y))");
        }

        // A set constant does not have an entry in the store either, and it
        // hash-conses, so it was compared by its node. Each statement builds a
        // set constant of its own around its individual collection, and the
        // second statement was a second rule.
        SUBCASE("the conjunction belongs to a rule inside a set constant")
        {
            enters_once("(G go H) => ((X p H) => (X likes {(((Y q H), (Y p H)) => !)}))");
        }
        SUBCASE("a collection inside a set constant")
        {
            enters_once("(G go H) => ((X p H) => (X likes {@{(Y q H)}}))");
        } });
}

TEST_CASE("rule identity: a set of conditions is compared by its members wherever it stands")
{
    // The engine reads a conjunction set via its tag, and within a term
    // position -- either the conditions of a rule nested in a consequence, or
    // a set that a consequence refers to -- it remains rule text regardless.
    // Compared there by its node, a counter that each statement builds anew,
    // a nested rule whose conditions constitute such a set never matched its
    // twin, and each statement of the rule added one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string rule;
        SUBCASE("in a switch with a ground condition") { rule = "(k is on) => ((k partof I, I q k) => (k is green))"; }
        SUBCASE("talked about in a consequence") { rule = "(X q Y) => ((X p Y, X r Y) is noted)"; }

        interactive.process(rule);
        collector.clear();
        interactive.process(".stat");
        const std::string before = node_count(collector);
        REQUIRE_FALSE(before.empty());

        interactive.process(rule);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);
        collector.clear();
        interactive.process(".stat");
        CHECK(node_count(collector) == before); });
}

TEST_CASE("rule identity: a set of conditions with one member is that condition")
{
    // A rule written in Janet, in the manner that janet.md builds one,
    // holds a condition set consisting of a single member, and the engine
    // reads it as that specific condition. It prints as `((X p Y)) => ...`,
    // which the parser reads as the single condition `(X p Y)` without a
    // set: as two distinct rules, the printed line re-entered as a second
    // rule, and likewise the plain one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(let [condition (zelph/collection (zelph/fact 'X "p" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/fact "k" "q" 'Y)))))");
        interactive.process("((X p Y)) => (X likes (k q Y))");
        interactive.process("(X p Y) => (X likes (k q Y))");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);

        interactive.process("a p m");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a likes (k q m)"}); });
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
        SUBCASE("negation is part of a rule nested in a rule")
        {
            // Once derived, the inner rule becomes a rule, so its condition
            // is compared as a condition, including the negation tag.
            // Compared as the subject of a standard fact, it lost the tag,
            // and the second statement was dropped as a duplicate of the
            // first.
            interactive.process("(G go H) => (¬(X p H) => (X q H))");
            interactive.process("(G go H) => ((X p H) => (X q H))");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "¬"));
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("negation is part of a conjunction nested in a rule")
        {
            // The same applies to a member of the inner rule's
            // conjunction, compared by its members as conditions.
            interactive.process("(G go H) => (((X q H), ¬(X p H)) => !)");
            interactive.process("(G go H) => (((X q H), (X p H)) => !)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "¬"));
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a set constant around a collection is not a collection around it")
        {
            // A set constant featuring a rule's own collection beneath it
            // is compared by its members, just as the collection itself is,
            // and `{@{c}}` along with `@{@{c}}` possess identical members.
            // They remain two distinct types of term, and the two rules
            // derive two different facts.
            interactive.process("(X p Y) => (X likes {@{c}})");
            interactive.process("(X p Y) => (X likes @{@{c}})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);

            interactive.process("b p d");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("b likes S");
            CHECK(answers_contain(collector, "b likes {@{c}}"));
            CHECK(answers_contain(collector, "b likes @{@{c}}"));
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

TEST_CASE("rule identity: a rule is not built while another one is being built")
{
    // zelph/dedup-rule builds its rule within a scratch cluster and marks
    // the ground patterns that the scratch recorded. A second build nested
    // within the first one activated the same scratch, merged it into itself
    // upon completion, and lost the recordings made by the outer build, so
    // the outer rule's patterns were never marked. It is rejected before
    // activating anything, and the outer build hands the scratch back as it
    // does in response to any error.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/dedup-rule (fn [] (zelph/dedup-rule (fn [] (zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X "q" 'Y)))))))js"),
                          doctest::Contains("a rule cannot be built while another rule is being built"));

        collector.clear();
        interactive.process(R"js(%(zelph/out (string "active=" (or (zelph/cluster) "none"))))js");
        CHECK(any_output_contains(collector, "active=none")); });
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
        }
        SUBCASE("a statement about a rule with variables is structure, not data")
        {
            // The statement is stored, yet no condition ever matches it:
            // Unification::extract_bindings dismisses each fact containing a
            // variable anywhere inside it as a rule template, since binding a
            // pattern's variable to a rule's own variable would build
            // partially instantiated, unusable nodes. Consequently, neither a
            // query nor a rule perceives the information previously stated
            // about the rule, and logic.md names this as the sole deviation
            // from "rules are data". Adopting HiLog-style quoting of rules
            // would introduce a capability designed to intentionally modify
            // this test.
            interactive.process("((X p Y) => (X q Y)) is nice");
            interactive.process("(R is nice) => (R flagged yes)");
            interactive.process(".run");
            collector.clear();
            interactive.process("S is nice");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
            collector.clear();
            interactive.process("S flagged yes");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("a statement about an ordinary fact is data")
        {
            // The control for the case above: the same statement concerning a
            // fact, devoid of variables, answers the query and fires the
            // rule, thus the exclusion of the rule stems from its variables,
            // not from the nesting structure.
            interactive.process("(a p b) is nice");
            interactive.process("(R is nice) => (R flagged yes)");
            interactive.process(".run");
            collector.clear();
            interactive.process("S is nice");
            CHECK(answers_contain(collector, "(a p b) is nice"));
            collector.clear();
            interactive.process("S flagged yes");
            CHECK(answers_contain(collector, "(a p b) flagged yes"));
        } });
}

TEST_CASE("rule identity: a rule a statement only mentions is no twin of the rule typed after it")
{
    // A switch mentions the rule it writes once it is active. Typed
    // independently, the identical rule was treated as a duplicate of that
    // mention and rolled back -- since a mention is not in force, nothing
    // derived anything. Only a rule in force serves as a twin; naming the
    // mention is what pattern resolution is for (.explain, the .prune
    // commands).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("with a collection literal")
        {
            interactive.process("(K is on) => ((X p Y) => (X q @{c}))");
            interactive.process("(X p Y) => (X q @{c})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);

            interactive.process("a p b");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S q O");
            CHECK(answers_contain(collector, "a q @{c}"));
        }
        SUBCASE("without one")
        {
            interactive.process("(K is on) => ((A r B) => (A s B))");
            interactive.process("(X r Y) => (X s Y)");
            interactive.process("c r d");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S s O");
            CHECK(answers_contain(collector, "c s d"));
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

TEST_CASE("rule identity: a rule's own collection is compared by its members, any other collection by its node")
{
    // Whether a collection is a rule's own is determined at the moment it
    // is written: one that the rule's text wrote is part of that text and
    // compared by its members, thereby identifying a twin whose collection
    // is a new node. Any other collection constitutes data named by the
    // rule, and is the same term only as the same node: a rule operating on
    // a data collection is not equivalent to a typed rule over a literal
    // sharing the same members.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("in a consequence")
        {
            interactive.process("(X p Y) => (X likes @{c})");
            collector.clear();
            interactive.process(".stat");
            const std::string before = node_count(collector);
            REQUIRE_FALSE(before.empty());

            interactive.process("(X p Y) => (X likes @{c})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
            collector.clear();
            interactive.process(".stat");
            CHECK(node_count(collector) == before);

            interactive.process("a p b");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S likes O");
            CHECK(answers_contain(collector, "a likes @{c}"));
        }
        SUBCASE("in a condition")
        {
            // The condition's collection is written with the rule and named
            // by nothing else, thus a data collection possessing identical
            // members fails to match it.
            interactive.process("(X in @{a b}) => (X flagged yes)");
            interactive.process("(X in @{a b}) => (X flagged yes)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);

            interactive.process("c in @{a b}");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S flagged O");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("written by a switch, then typed")
        {
            interactive.process("(K is on) => ((X p Y) => (X q @{c d}))");
            interactive.process("k is on");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            REQUIRE(listed_rules(collector) == 2);

            interactive.process("(A p B) => (A q @{c d})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a rule over a data collection is no twin of a typed literal")
        {
            // The generator binds C to the data collection `@{x}` and writes
            // `(X r Y) => (X in C)`; the typed rule's `@{k}` has the members
            // that the collection acquired, and is its own all the same.
            process_lines(interactive, R"(
d has @{x}
(S has C) => ((k in C) => (S flagged yes))
(S has C) => ((X r Y) => (X in C))
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            const std::size_t before = listed_rules(collector);

            interactive.process("(X r Y) => (X in @{k})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == before + 1);

            interactive.process("a r b");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S in O");
            CHECK(answers_contain(collector, "a in @{k a}"));
        }
        SUBCASE("a rule over a data collection with the literal's members is no twin of it")
        {
            // `@{k}` is data that the generated rule names and writes into;
            // the typed rule's `@{k}` is its own, containing identical members
            // after the generated rule has written X into the data. Compared
            // by members, just as each collection was, the typed rule was
            // deemed a duplicate and rolled back, and the rule a user wrote
            // was gone.
            process_lines(interactive, R"(
d has @{k}
(S has C) => ((X r Y) => (X in C))
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            const std::size_t before = listed_rules(collector);

            interactive.process("(X r Y) => (X in @{k})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == before + 1);
        } });
}

TEST_CASE("rule identity: a set constant is decided by identity unless a rule's own collection hangs below it")
{
    // A set constant hash-conses over its members, so `{a b}` written twice
    // constitutes a single node, and identity settles it.
    //
    // Every statement contains a node of its own representing the collection
    // a rule writes, and likewise, the set constant around it forms its own
    // node: `{@{c}}` is compared by its members, just as the collection
    // `@{c}` by itself is. Compared by its node, the second statement of the
    // rule was a second rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("only atoms below it")
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
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a collection below it")
        {
            interactive.process("(X p Y) => (X likes {@{c}})");
            collector.clear();
            interactive.process(".stat");
            const std::string before = node_count(collector);
            REQUIRE_FALSE(before.empty());

            interactive.process("(X p Y) => (X likes {@{c}})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
            collector.clear();
            interactive.process(".stat");
            CHECK(node_count(collector) == before);

            // A collection that includes a different member constitutes
            // a different rule.
            interactive.process("(X p Y) => (X likes {@{d}})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        } });
}

TEST_CASE("rule identity: a rule sharing a set constant survives its duplicate's rollback")
{
    // zelph/dedup-rule builds every parsed rule in a scratch cluster and drops
    // that scratch again once it finds an alpha-equivalent twin. So the twin
    // branch performs a REMOVAL, on nodes the duplicate has in common with the
    // rule already in the graph -- and a set constant is shared by
    // construction, since the literal hash-conses.
    //
    // Removing a PartOf fact dooms its container, which is right for an
    // element and wrong for the variable of a pattern. Getting that wrong
    // deleted the ORIGINAL rule and left "No rules found" (002dcbc), and this
    // is the side that made it reachable without any removal command at all:
    // typing the same rule twice was enough.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in {a b}) => (X flagged yes)");
        interactive.process("(A in {a b}) => (A flagged yes)");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);
        CHECK_FALSE(any_output_contains(collector, "No rules found"));

        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "a flagged yes"));
        CHECK(answers_contain(collector, "b flagged yes")); });
}

TEST_CASE("rule identity: a rule whose text changed is found by its text as it stands")
{
    // The fingerprint of a rule is calculated just once, upon completion, and
    // stored within an index (Zelph::find_equivalent_rule). A name merge
    // changes its text afterwards without creating a new rule node: it
    // rewrites the members of its collection. This merge marks the index
    // stale, prompting the subsequent lookup to recalculate the fingerprint
    // for every rule. Under the old fingerprint, the rule typed with its
    // current text was a second rule beside it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // bug1 is merged into bug2: the two membership facts combine into a
        // single one, the rule node remains.
        process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
(X filed Y) => (Y in @{bug1 bug2})
.name bug1 en z
.name bug2 en z
)");

        collector.clear();
        interactive.process(".list-rules");
        const std::size_t before = listed_rules(collector);

        interactive.process("(X reported Y) => (Y in @{bug2})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == before); });
}

TEST_CASE("rule identity: a statement that binds a rule's collection or condition set leaves the rule as written")
{
    // A rule over rule structure binds a ground rule's collection or its
    // condition set, and states a membership within it. The membership is
    // directed toward the data term of what it bound, so the ground rule's
    // text remains the original text it was written with: when typed again,
    // it is the same rule, it prints as written, and it fires. If the
    // member had been written directly into the rule's text, the rule would
    // express something not authored by anyone -- `(a p b) => (c in @{d c
    // zz})`, a condition `x y z` that no fact matches -- and the rule, when
    // retyped, would become a distinct second rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a member written into the collection")
        {
            process_lines(interactive, R"(
(a p b) => (c in @{d})
a p b
(G => (S in C)) => (zz in C)
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".list-rules");
            const std::size_t before = listed_rules(collector);
            CHECK(any_output_contains(collector, "(a p b) => (c in @{d c})"));

            interactive.process("(a p b) => (c in @{d})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == before);

            // The data term holds the members of the rule and the content
            // written by the rule over rules.
            collector.clear();
            interactive.process("S in O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"c in @{d c zz}", "d in @{d c zz}", "zz in @{d c zz}"});
        }
        SUBCASE("a condition written into the condition set")
        {
            process_lines(interactive, R"(
(a p b, c q d) => (e r f)
(G => H) => ((x y z) in G)
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".list-rules");
            const std::size_t before = listed_rules(collector);

            interactive.process("(a p b, c q d) => (e r f)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == before);

            process_lines(interactive, "a p b\nc q d");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S r O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"e r f"});
        } });
}

TEST_CASE("rule identity: a generated rule a name merge rebuilt is found as a twin")
{
    // A name merge rebuilds the rules that hold a merged node using fresh
    // identifiers, which fact() never queued for the index. The rule typed
    // with the rebuilt rule's text is that rule, not a second one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X G @{c}))
likes go k
)");
        interactive.run(true, false, false);
        process_lines(interactive, R"(
m is here
.name k en x
.name m en x
)");
        collector.clear();
        interactive.process(".list-rules");
        const std::size_t before = listed_rules(collector);
        REQUIRE(before == 2);

        interactive.process("(X p m) => (X likes @{c})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == before); });
}
