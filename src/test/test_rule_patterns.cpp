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
#include <utility>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// A rule's patterns are not data.
//
// Writing a rule materializes its conditions and its consequences as real
// fact nodes, because the engine has nothing else to match against. A pattern
// carrying a variable gives itself away as a template and is rejected as data
// everywhere. A GROUND one does not, and the consequences were severe and
// silent:
//
//     zelph> (a p b) => (c q d)
//     zelph> C q D
//     Answer: c q d          <- nobody said this
//
// It was not confined to queries -- the leaked pattern satisfied other rules'
// conditions -- and the shape that bites hardest is a ground CONSEQUENCE
// under a variable condition: "(A is bad) => (alarm is on)" answered "alarm
// is on" with nothing bad anywhere.
//
// The node itself cannot say which happened; asserting a statement and
// building it as a pattern produce the same node with the same edges. What
// can say it is the MOMENT of construction, and that is what the marking uses
// (see Zelph::mark_rule_patterns). Asserting or deriving the statement later
// revokes the mark, so the tests below come in pairs: the pattern is inert,
// and the moment it is claimed it behaves like any other fact.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;
}

TEST_CASE("rule patterns: a ground pattern answers no query")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process("A p B");
        CHECK_FALSE(answers_contain(collector, "a p b")); });
}

TEST_CASE("rule patterns: a ground pattern satisfies no other rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("(X q Y) => (X r Y)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X r Y");
        CHECK_FALSE(answers_contain(collector, "c r d")); });
}

TEST_CASE("rule patterns: the shape that bites -- a ground consequence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A is bad) => (alarm is on)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X is on");
        REQUIRE_FALSE(answers_contain(collector, "alarm is on"));

        // ... and the moment something IS bad, it is an ordinary fact.
        interactive.process("rust is bad");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X is on");
        CHECK(answers_contain(collector, "alarm is on")); });
}

TEST_CASE("rule patterns: being derived announces the statement")
{
    // Revoking the mark is the moment the statement BECOMES data, and
    // everything that reacts to a new fact has to hear about it. Without the
    // announcement, semi-naive seeding never offers it to the rules whose
    // conditions it now satisfies -- the node itself is old, so nothing else
    // would mention it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(A is bad) => (alarm is on)");
        interactive.process("(X is on) => (X needs attention)");
        collector.clear();
        interactive.process("rust is bad");
        CHECK(any_deduction_of(collector, "alarm is on"));
        CHECK(any_deduction_of(collector, "alarm needs attention"));

        collector.clear();
        interactive.process("S needs O");
        CHECK(answers_contain(collector, "alarm needs attention")); });
}

TEST_CASE("rule patterns: asserting the statement first keeps it data")
{
    // Order matters and must not: a statement that was CLAIMED before any
    // rule mentioned it is a claim, and the rule reusing its node changes
    // nothing about that. The construction only ever marks what it created.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("(a p b) => (c q d)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        // The rule fires on it, so the consequence is a claim too now.
        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: asserting it afterwards revokes the mark")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: the Janet read surface answers about claims")
{
    // The queries had taken this reading since the marking was introduced;
    // the Janet API kept the structural one, and SPARQL is built on it -- so
    // `SELECT ?s WHERE { ?s p b }` answered `a` for a graph in which nobody
    // had said `a p b`. All of it now asks the same question, and the
    // structural one keeps its own name.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b) => (c q d)
x p b
b p z
)");

        collector.clear();
        interactive.process(R"(%(string "SRC-" (string/join (sorted (map zelph/name (zelph/sources "p" "b"))) ",")))");
        CHECK(any_output_contains(collector, "SRC-x"));

        collector.clear();
        interactive.process(R"(%(string "TGT-" (string/join (sorted (map zelph/name (zelph/targets "a" "p"))) ",")))");
        CHECK(any_output_contains(collector, "TGT-"));
        CHECK_FALSE(any_output_contains(collector, "TGT-b"));

        // The closure must not walk THROUGH the pattern either: a would be an
        // ancestor of z only via the edge the rule merely writes down.
        collector.clear();
        interactive.process(R"(%(string "CLO-" (string/join (sorted (map zelph/name (zelph/closure-sources "z" "p"))) ",")))");
        CHECK(any_output_contains(collector, "CLO-b,x"));

        collector.clear();
        interactive.process(R"(%(string "EX-" (zelph/exists "a" "p" "b") "/" (zelph/mentioned "a" "p" "b")))");
        CHECK(any_output_contains(collector, "EX-false/true"));

        // The control: the asserted neighbour answers both ways.
        collector.clear();
        interactive.process(R"(%(string "EX2-" (zelph/exists "x" "p" "b") "/" (zelph/mentioned "x" "p" "b")))");
        CHECK(any_output_contains(collector, "EX2-true/true")); });
}

TEST_CASE("rule patterns: a fact asserted from Janet revokes the mark")
{
    // zelph/fact is the assertion API, so it claims the statement exactly as
    // typing it does -- but only the parser's top level used to say so. A
    // ground rule condition built from Janet therefore stayed a pattern,
    // unification skipped it, and the rule never fired. It went unnoticed
    // because zelph/exists answered true off the pattern the rule itself had
    // written; the polynomial suite asks precisely that question about the
    // all-ground pneg rule of stdlib/polynomial.zph.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process(R"(%(zelph/fact "a" "p" "b"))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process(R"(%(string "EX-" (zelph/exists "a" "p" "b")))");
        CHECK(any_output_contains(collector, "EX-true"));

        // Building a RULE from Janet claims nothing: the same statement
        // written as a second rule's condition stays a pattern.
        interactive.process("(m r n) => (s t u)");
        collector.clear();
        interactive.process(R"(%(string "EX2-" (zelph/exists "m" "r" "n")))");
        CHECK(any_output_contains(collector, "EX2-false")); });
}

TEST_CASE("rule patterns: zelph/rule marks what its argument forms built")
{
    // Typed, `(X p k) => (X likes (k is green))` maintains `k is green` as a
    // pattern. Constructed using zelph/rule, it became data the moment the
    // rule was written: `A is green` answered it and .explain designated it
    // as an axiom. In a Janet function, arguments are evaluated prior to the
    // call, so the function cannot tell which of the nodes it receives the
    // call built. zelph/rule operates as a macro, so its argument forms
    // execute within the rule's build, and whatever they build into the rule
    // -- during their dynamic extent, including any helper they invoke --
    // constitutes the rule's text, just as the parser's does. A node that
    // predated the call was not formed at that moment and remains unchanged.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto build = [&interactive]
        { interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" (zelph/fact "k" "is" "green"))))js"); };

        SUBCASE("a part the argument forms built is a pattern, also once the rule has fired")
        {
            build();
            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process(".explain (k is green)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
            CHECK_FALSE(any_output_contains(collector, "[axiom]"));

            collector.clear();
            interactive.process("a p k");
            CHECK(any_deduction_of(collector, "a likes (k is green)"));

            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());
        }

        SUBCASE("so is a part that a helper called in the argument forms built")
        {
            interactive.process(R"js(%(defn green [] (zelph/fact "k" "is" "green")))js");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" (green))))js");
            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());
        }

        SUBCASE("so are the cons cells of a list built there")
        {
            // As in the typed `(X p k) => (X has <ab>)`.
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "has" (zelph/list-chars "ab"))))js");
            collector.clear();
            interactive.process("A cons B");
            CHECK(collect_answers(collector).empty());
        }

        SUBCASE("a fact a helper asserts beside the part it returns is a claim")
        {
            // The build marks the parts of the rule, not every component
            // generated by its argument forms: the rule's `=>` facts say
            // which nodes it holds. A helper that asserts a single fact
            // and yields a different one builds solely the returned one
            // into the rule. The other remains a statement within the
            // program, just like any zelph/fact inside a block, and
            // marking it would conceal an actual assertion.
            interactive.process(R"js(%(defn green [] (zelph/fact "k" "is" "colour") (zelph/fact "k" "is" "green")))js");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" (green))))js");
            collector.clear();
            interactive.process("k is C");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k is colour"});

            collector.clear();
            interactive.process(".explain (k is colour)");
            CHECK(any_output_contains(collector, "[axiom]"));
            CHECK_FALSE(any_output_contains(collector, "rule pattern"));

            collector.clear();
            interactive.process(".explain (k is green)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        }

        SUBCASE("a part bound before the call stays a claim")
        {
            interactive.process(R"js(%(let [g (zelph/fact "k" "is" "green")] (zelph/rule [(zelph/fact 'X "p" "k")] (zelph/fact 'X "likes" g))))js");
            collector.clear();
            interactive.process("A is green");
            CHECK(answers_contain(collector, "k is green"));
        }

        SUBCASE("a part asserted before stays a claim")
        {
            interactive.process("k is green");
            build();
            collector.clear();
            interactive.process("A is green");
            CHECK(answers_contain(collector, "k is green"));
        }

        SUBCASE("claiming the part afterwards revokes the mark")
        {
            build();
            interactive.process("k is green");
            collector.clear();
            interactive.process("A is green");
            CHECK(answers_contain(collector, "k is green"));
        }

        SUBCASE("building the part there revokes no other rule's mark")
        {
            // A rule's text claims nothing. When typed following the rule
            // below, `(X q k) => (X loves (k is green))` leaves `k is green`
            // a pattern, and the identical rule constructed using zelph/rule
            // must do the same: the zelph/fact calls within its argument
            // forms execute during rule construction, thereby writing the
            // pattern without claiming it.
            interactive.process("(X p k) => (X likes (k is green))");
            interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "q" "k")] (zelph/fact 'X "loves" (zelph/fact "k" "is" "green"))))js");
            collector.clear();
            interactive.process("A is green");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process(".explain (k is green)");
            CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));
        }

        SUBCASE("the build hands what it made to the cluster that was active")
        {
            // The build executes within a dedicated scratch cluster. What
            // it retains is transferred to the user's cluster, ensuring
            // that .cluster-drop continues to take the rule back alongside
            // the rest of the experiment.
            interactive.process(".cluster exp");
            build();
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "active=" (or (zelph/cluster) "none"))))js");
            CHECK(any_output_contains(collector, "active=exp"));

            interactive.process(".cluster-drop exp");
            collector.clear();
            interactive.process(".list-rules");
            CHECK_FALSE(any_output_contains(collector, "likes"));
        } });
}

TEST_CASE("rule patterns: naming a rule does not claim what it says")
{
    // The counterpart of the case above, and the reason the revocation in
    // zelph/fact is suppressed for parser-generated code: a typed statement
    // builds its SUBTERMS through the very same call, and a subterm is not
    // claimed by the statement that names it. Writing something ABOUT a rule
    // must leave the rule's patterns exactly where they were -- otherwise
    // mentioning a rule silently turns its condition and its consequence
    // into data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("x documents ((a p b) => (c q d))");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process("A p B");
        CHECK_FALSE(answers_contain(collector, "a p b"));

        // The statement itself is data, of course.
        collector.clear();
        interactive.process("X documents Y");
        CHECK(collect_answers(collector).size() == 1); });
}

TEST_CASE("rule patterns: a read-only command does not claim the pattern it asks about")
{
    // .explain, .node and the prune commands parse their argument into the
    // same zelph/fact calls a statement does, inside a scratch cluster they
    // drop again. That is a QUESTION about the graph, not a claim -- but the
    // revocation keyed on it and the answer changed the answer: asking
    // ".explain (a p b)" about a rule's pattern turned it into data, so the
    // command reported "[axiom]" for a statement nobody had made and the
    // query started answering it afterwards.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "rule pattern"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]"));

        collector.clear();
        interactive.process("A p B");
        CHECK_FALSE(answers_contain(collector, "a p b"));

        collector.clear();
        interactive.process(R"(%(string "EX-" (zelph/exists "a" "p" "b")))");
        CHECK(any_output_contains(collector, "EX-false")); });
}

TEST_CASE("rule patterns: a propositional rule reaches its conclusion")
{
    // Both halves are needed for this one to be observable at all: the
    // ground condition has to be allowed to match (see "rules: a ground
    // condition is not a non-match" in test_reasoning.cpp), and the ground
    // consequence has to be a pattern rather than a fact, or it would have
    // been there before the rule ever fired.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(a p b) => (c q d)");
        collector.clear();
        interactive.process("a p b");
        CHECK(any_deduction_of(collector, "c q d"));

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: a pattern with variables is untouched")
{
    // The control. Nothing about the marking may reach the ordinary case,
    // where the template check has always done the work.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A p B) => (A q B)");
        interactive.process("x p y");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "x q y"));

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "x p y")); });
}

TEST_CASE("rule patterns: a mentioned rule whose variables sit in its conditions is not data")
{
    // `((X p Y) => (c q d)) is noted` carries the variables from its
    // condition and functions as a pattern. Its twin with two conditions
    // read as a ground statement: the conditions hang off a container, no
    // hash, and var_in_closure stopped there -- thus `S is O` answered a
    // mention with X and Y unbound, and the same occurred on the read
    // surface. Both twins serve as rule text for either.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((X p Y, X r Y) => (c q d)) is noted
((X p Y) => (c q d)) is noted
)");

        collector.clear();
        interactive.process("S is O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S is noted");
        CHECK(collect_answers(collector).empty());

        // The Janet read surface bypasses the same elements that queries
        // bypass, by taking a snapshot before adjacency locks
        // (Zelph::unasserted_snapshot).
        collector.clear();
        interactive.process(R"(%(string "SRC-" (length (zelph/sources "is" "noted"))))");
        CHECK(any_output_contains(collector, "SRC-0")); });
}

TEST_CASE("rule patterns: a firing does not derive its own consequence pattern while it holds a variable")
{
    // A firing keeps the condition set of a rule its consequence mentions
    // as it stands, including W, X, and Y, and this consequence holds
    // nothing else to substitute, so its instance is the rule's own
    // consequence pattern containing variables: rule text, which no query
    // answers and `.explain` calls not asserted. An engine that claims that
    // node prints a deduction contradicting its own answers. The firing
    // writes the witness alone, and subsequent passes contribute nothing,
    // since the pattern is the instance and counts as present.
    //
    // The control: a ground consequence is the rule's own pattern as well,
    // yet it holds no variable, and the act of deriving it transforms it
    // into data. Here it is positioned next to a fresh variable that
    // already possesses a witness, thus the check for a prior witness must
    // count it as missing, otherwise the firing that derives it is skipped.
    // The firing alone, without a fresh variable, is held by "rule
    // patterns: a propositional rule reaches its conclusion".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        // Single passes, as counts_over_passes runs them: an engine that
        // counts the mention's pattern as missing, or one that fails to
        // claim the ground consequence, makes a new witness with each
        // pass, and a run to the fixpoint would not end.
        interactive.process(".semi-naive off");
        interactive.process(".auto-run"); // a toggle: off

        SUBCASE("a mention whose variables the firing leaves to the mentioned rule")
        {
            interactive.process("(X p Y) => (((W r X, W s Y) => (c q d)) is noted) (W t X)");
            interactive.process("a p b");
            collector.clear();
            interactive.process(".run-once");
            CHECK(any_deduction_of(collector, "?? t a"));
            CHECK_FALSE(any_deduction_of(collector, "is noted"));

            const std::size_t        after = node_count(collector, interactive);
            std::vector<std::size_t> counts;
            for (int pass = 0; pass < 4; ++pass)
            {
                interactive.process(".run-once");
                counts.push_back(node_count(collector, interactive));
            }
            CHECK(counts == std::vector<std::size_t>(4, after));

            collector.clear();
            interactive.process("S is O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process(".explain (((W r X, W s Y) => (c q d)) is noted)");
            CHECK(any_output_contains(collector, "Fact is not asserted -- nothing to explain."));
        }
        SUBCASE("a ground consequence beside a fresh variable with a witness")
        {
            interactive.process("(A is m) => (k about z) (N near A)");
            interactive.process("z near g");
            interactive.process("g is m");
            collector.clear();
            interactive.process(".run-once");
            CHECK(any_deduction_of(collector, "k about z"));

            collector.clear();
            interactive.process("S about O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"k about z"});
        } });
}

TEST_CASE("rule patterns: a query in the `=>` position answers no rule whose variables sit in its conditions")
{
    // This query meets the rules themselves as its candidates. Unification
    // rejects rule text based on the closures associated with a candidate's
    // subject and consequence, and the subject of a rule with two conditions
    // is the container that its conditions hang off, which lacks a closure:
    // thus, `S => (g q h)` answered the rule currently in force and `S => (c
    // q d)` answered the mentioned one, both with X and Y unbound, whereas
    // the one-condition twins and zelph/sources answered neither.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y, X r Y) => (g q h)
(X p Y) => (e q f)
((X p Y, X r Y) => (c q d)) is noted
((X p Y) => (c q d)) is noted
)");

        for (const std::string consequence : {"g q h", "e q f", "c q d"})
        {
            INFO(consequence);
            collector.clear();
            interactive.process("S => (" + consequence + ")");
            CHECK(collect_answers(collector).empty());
        }

        collector.clear();
        interactive.process(R"(%(string "SRC-" (string/join (map (fn [c] (string (length (zelph/sources "=>" c)))) [(zelph/fact "g" "q" "h") (zelph/fact "e" "q" "f") (zelph/fact "c" "q" "d")]) ",")))");
        CHECK(any_output_contains(collector, "SRC-0,0,0")); });
}

TEST_CASE("rule patterns: a mentioned rule whose variables sit in its own collection is not data")
{
    // `((a p Y) => (c q d)) is weird` carries the variable from its
    // condition, making the mention a pattern, and a rule over rules does
    // not bind it. Its twin with the variable located within a collection of
    // the rule's own text read as a ground statement: the collection is not
    // a hash, and var_in_closure did not enter it, so `(G => H) => (G seen
    // H)` derived `(a p @{Y}) seen (c q d)` with Y unbound, and `S is O`
    // answered the mention. The collection is entered in the same way that a
    // rule's conditions are -- also where a set constant holds it, also
    // through the walk `.fact-stores off` leaves -- and all three mentions
    // are rule text.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((a p @{Y}) => (c q d)) is weird
((a p Y) => (c q d)) is weird
((a p {@{Y}}) => (c q d)) is odd
(G => H) => (G seen H)
)");
        interactive.run(true, false, false);

        const auto check_no_data = [&]
        {
            for (const std::string query : {"S seen O", "S is O"})
            {
                INFO(query);
                collector.clear();
                interactive.process(query);
                CHECK(collect_answers(collector).empty());
            }
        };
        check_no_data();

        interactive.process(".fact-stores off");
        check_no_data(); });
}

TEST_CASE("rule patterns: a typed rule whose variables sit in its own collection is no data to a rule over rules")
{
    // The typed rule constitutes rule text as well. A rule over rules
    // bound it as data and derived a statement over the typed rule's own
    // node, which then counted as mentioned and stopped firing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p @{Y}) => (c q d)
(G => (c q d)) => ((G => (c q d)) is seen)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S is seen");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p @{Y}) => (c q d)")); });
}

TEST_CASE("rule patterns: a mentioned rule whose variables sit in the collection of a membership is not data")
{
    // The identical scenario where the rule's own collection serves as the
    // object in a membership relation: `a in @{Y}` functions as the
    // condition, `c in @{Y}` acts as the consequence, and the first of them
    // appears among two conditions. The membership is the collection's
    // content and holds a variable solely via its member (refer to the case
    // below), thus all three rules read as ground: `(G => H) => (G seen H)`
    // derived `(a in @{a}) seen (c q d)`, and `S is O` answered every
    // mention. The rule's text is finalized once its `=>` fact is written,
    // and it holds Y through the collection. With the fact stores off from
    // the start, the walk that substitutes for them determines outcomes both
    // at firing and at query.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("fact stores on")
        {
        }
        SUBCASE("fact stores off")
        {
            interactive.process(".fact-stores off");
        }
        process_lines(interactive, R"(
((a in @{Y}) => (c q d)) is weird
((a p b) => (c in @{Y})) is weird
((a in @{Y}, b r c) => (c q d)) is odd
(G => H) => (G seen H)
)");
        interactive.run(true, false, false);

        for (const std::string query : {"S seen O", "S is O"})
        {
            INFO(query);
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        } });
}

TEST_CASE("rule patterns: a typed rule whose variables sit in the collection of a membership is no data to a rule over rules")
{
    // Its typed twin: the rule over rules bound `(a in @{Y}) => (c q d)` as
    // data, and the statement it derived over the typed rule's node made
    // that rule a mentioned one, which is no longer included in the list
    // and no longer fires.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("fact stores on")
        {
        }
        SUBCASE("fact stores off")
        {
            interactive.process(".fact-stores off");
        }
        process_lines(interactive, R"(
(a in @{Y}) => (c q d)
(G => (c q d)) => ((G => (c q d)) is seen)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S is seen");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a in @{")); });
}

TEST_CASE("rule patterns: a membership in a rule's own collection holds a variable only through its member")
{
    // A membership is its collection's content. Read through the
    // collection, `c in @{c Y}` holds Y -- or not, based on which of the
    // two memberships the parser wrote first -- and a rule over the
    // engine's marking facts lost the membership that the engine marks as
    // a pattern. The rule pattern marks stay visible to such a rule, just
    // like the other marks.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{c Y})
(X ~ K) => (X tagged K)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S tagged O");
        CHECK(answers_contain(collector, "(c in @{c}) tagged \"rule pattern\"")); });
}

TEST_CASE("rule patterns: a rule that binds a rule's own collection leaves that rule's text ground")
{
    // A rule over rules binds the collection associated with a ground rule
    // and writes a rule stating X's membership in what it bound. The rule
    // it generates refers to the data term of the collection, ensuring the
    // ground rule keeps its original text, which holds no variable, and
    // remains data to `(H => K) => (H seen K)`. If X were inserted into the
    // ground rule's own collection, the ground rule's text would thereafter
    // contain X, and a rule over rules would no longer see it, or would
    // perceive it only until the text-variable store becomes aware of the
    // write. The walk that stands in for the store following a removal, a
    // load, or `.fact-stores off` reads the identical text, so the answer
    // remains unchanged even after an unrelated `.remove`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        enum class Mode
        {
            EachRun,
            RunTogether,
            StoresOff
        } mode = Mode::EachRun;
        SUBCASE("each rule run as it is written")
        {
            mode = Mode::EachRun;
        }
        SUBCASE("the rules run together")
        {
            mode = Mode::RunTogether;
        }
        SUBCASE("fact stores off")
        {
            mode = Mode::StoresOff;
        }

        struct Shape
        {
            std::string rules;
            std::string seen;
        };
        for (const Shape& shape : {Shape{"(a p b) => (c q @{d})\n(G => (S q C)) => ((X r Y) => (X in C))", "(a p b) seen (c q @{d})"},
                                   Shape{"(a p b) => (c in @{d})\n(G => (S in C)) => ((X q Y) => (X in C))", "(a p b) seen (c in @{d c})"}})
        {
            CAPTURE(shape.rules);
            interactive.process(".new");
            const std::string rules = shape.rules + "\n(H => K) => (H seen K)";
            if (mode == Mode::StoresOff) interactive.process(".fact-stores off");
            if (mode == Mode::RunTogether)
            {
                interactive.process(".auto-run"); // a toggle: off
                process_lines(interactive, rules);
                interactive.run(true, false, false);
                interactive.process(".auto-run"); // on again
            }
            else
                process_lines(interactive, rules);

            collector.clear();
            interactive.process("S seen O");
            CHECK(collect_answers(collector) == std::vector<std::string>{shape.seen});

            process_lines(interactive, "zz qq zz\n.remove zz");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S seen O");
            CHECK(collect_answers(collector) == std::vector<std::string>{shape.seen});
        } });
}

TEST_CASE("rule patterns: a rule that binds a rule's own collection leaves that rule's text ground after .save and .load")
{
    // The same via a round trip, with the walk determining the outcome. The
    // session that wrote the network and the one that loaded it answer
    // identically, and both answer according to what the ground rules' texts
    // say: the rule over rules did not write any member into them. This
    // remains true for two ground rules whose collections contain identical
    // members: each construction names the data term of the collection it
    // bound, and neither leaves the other's text holding a variable.
    const auto file = fs::temp_directory_path() / "zelph_rule_text_bound_test.bin";

    // The answers to `S seen O` after `(H => K) => (H seen K)` has run
    // over the network that `lines` constructs: within the session
    // responsible for its creation, and after that network is saved and
    // loaded into an empty one.
    const auto seen = [&](zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& lines)
    {
        const auto answers = [&]
        {
            interactive.process("(H => K) => (H seen K)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S seen O");
            std::vector<std::string> out = collect_answers(collector);
            std::ranges::sort(out);
            return out;
        };

        process_lines(interactive, lines);
        interactive.run(true, false, false);
        interactive.process(".save \"" + file.string() + "\"");
        const std::vector<std::string> in_session = answers();

        interactive.process(".new");
        interactive.process(".load \"" + file.string() + "\"");
        return std::pair{in_session, answers()};
    };

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        SUBCASE("a fact over the collection")
        {
            const auto [in_session, after_load] = seen(collector, interactive, R"(
(a p b) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
)");
            CHECK(in_session == std::vector<std::string>{"(a p b) seen (c q @{d})"});
            CHECK(after_load == in_session);
        }
        SUBCASE("a membership in the collection")
        {
            const auto [in_session, after_load] = seen(collector, interactive, R"(
(a p b) => (c in @{d})
(G => (S in C)) => ((X q Y) => (X in C))
)");
            CHECK(in_session == std::vector<std::string>{"(a p b) seen (c in @{d c})"});
            CHECK(after_load == in_session);
        }
        SUBCASE("two ground rules whose collections hold the same members")
        {
            const auto [in_session, after_load] = seen(collector, interactive, R"(
(a p b) => (c q @{d})
(e p f) => (c q @{d})
(G => (S q C)) => ((X r Y) => (X in C))
)");
            CHECK(in_session == std::vector<std::string>{"(a p b) seen (c q @{d})", "(e p f) seen (c q @{d})"});
            CHECK(after_load == in_session);
        } });

    fs::remove(file);
}

TEST_CASE("rule patterns: a rule that binds a rule's conjunction set leaves that rule's conditions as written")
{
    // The rule structure binds the conjunction set of `(a p b, c q d) => (e f
    // g)` as C, and the generated rule states X's membership within C. Where
    // a membership is written, a conjunction set represents its data term;
    // thus, the generated rule writes into that term, while the ground rule
    // keeps its two conditions: it remains data for a rule over rules --
    // within the session, following a load, with the fact stores off -- and
    // continues to fire. If X were included among its conditions, the rule
    // would hold a variable in its conditions, and the generated rule's
    // firing on `k r k` would introduce k as a condition, which no fact could
    // match: the rule would never fire again.
    const auto file = fs::temp_directory_path() / "zelph_conjunction_set_bound_test.bin";

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(C => (S f O)) => ((X r Y) => (X in C))
)");
        interactive.run(true, false, false);

        SUBCASE("in the session")
        {
        }
        SUBCASE("after .save and .load")
        {
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".new");
            interactive.process(".load \"" + file.string() + "\"");
        }
        SUBCASE("fact stores off")
        {
            interactive.process(".fact-stores off");
        }

        interactive.process("(H => K) => (H seen K)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"{(c q d) (a p b)} seen (e f g)"});

        process_lines(interactive, "k r k\na p b\nc q d");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S f O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"e f g"});
        collector.clear();
        interactive.process("k in O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"k in @{k}"}); });

    fs::remove(file);
}

TEST_CASE("rule patterns: a condition set among the conditions is read as well")
{
    // Reasoning::evaluate steps into a condition set that constitutes one
    // of a rule's conditions, so the variables within it belong to the
    // rule. Here, they are its sole variables, situated beneath a ground
    // condition.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("((a r b, (C s D, C t D)) => (c q d)) is noted");

        collector.clear();
        interactive.process("S is noted");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: a construction's condition set is read as the typed one is")
{
    // A construction writes the rule's conditions as a set literal, and a set
    // whose immediate members are entirely ground qualifies as a set
    // constant, a hash, even if one of those members is a condition set
    // holding the rule's variables. The reading skipped over every hash: the
    // generated rule was treated as ground, and `S => (c q k)` and
    // zelph/sources answered it with C and D unbound, whereas its typed twin,
    // whose conditions the parser writes into a collection, received no
    // response from either. Both rules fire, and both constitute rule text,
    // to the variable store and to the walk `.fact-stores off` leaves.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((a r b, (C G D, C u D)) => (c q H))
t go k
(a r b, (C s D, C u D)) => (c q m)
a r b
x s y
x t y
x u y
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("c q V");
        CHECK(collect_answers(collector).size() == 2);
        CHECK(answers_contain(collector, "c q k"));
        CHECK(answers_contain(collector, "c q m"));

        const auto check_unanswered = [&]
        {
            for (const std::string consequence : {"c q k", "c q m"})
            {
                INFO(consequence);
                collector.clear();
                interactive.process("S => (" + consequence + ")");
                CHECK(collect_answers(collector).empty());
            }
        };
        check_unanswered();

        collector.clear();
        interactive.process(R"(%(string "SRC-" (string/join (map (fn [c] (string (length (zelph/sources "=>" c)))) [(zelph/fact "c" "q" "k") (zelph/fact "c" "q" "m")]) ",")))");
        CHECK(any_output_contains(collector, "SRC-0,0"));

        interactive.process(".fact-stores off");
        check_unanswered(); });
}

TEST_CASE("rule patterns: rule text in its conditions is not data after .save and .load")
{
    // A binary load disarms the variable store, and var_in_closure walks
    // the structure it reads back instead. The walk reads a rule's
    // conditions just as the store does, or the network would answer,
    // following a round trip, what the session that wrote it did not.
    const auto file = fs::temp_directory_path() / "zelph_rule_text_vars_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("((X p Y, X r Y) => (c q d)) is noted");
        interactive.process("x has {((A r B, A s B) => (c q d))}");
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process("S is O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S has O");
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process("S => O");
        CHECK(collect_answers(collector).empty());
    }

    fs::remove(file);
}

TEST_CASE("rule patterns: a mention whose consequence carries a variable stays a pattern")
{
    // The control: in this case, the structural closure reaches a
    // variable via the consequence, in both twins, thus no aspect of the
    // conditions decides it. Neither mention was ever answered.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((A q B, A r B) => (A s B)) is noted
((A q B) => (A s B)) is noted
)");

        collector.clear();
        interactive.process("S is noted");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: a rule inside a rule's collection is not data after a firing")
{
    // The rule's collection holds a nested rule whose variables reside
    // within its conditions. Upon firing, it writes the collection into the
    // data; its member remains the nested rule's text, never a statement,
    // hence `S in O` answers nothing -- after the typed rule fired and after
    // a generator that writes the same structure has run. The generator's
    // rule had emerged as a set constant around its partially substituted
    // nested rule, and `S in O` answered that membership.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{((A t B, A r B) => (c q k))})
a p b
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());

        process_lines(interactive, R"(
(G go H) => ((X p Y) => (X likes @{((A G B, A r B) => (c q H))}))
t go k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: .explain does not call a pattern an axiom")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]"));

        // Once it is derived, it has a proof like anything else.
        interactive.process("a p b");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "a p b"));
        CHECK_FALSE(any_output_contains(collector, "rule pattern")); });
}

TEST_CASE("rule patterns: a collection literal in a condition is not data")
{
    // A collection literal builds a container plus one PartOf fact per
    // member, and Zelph::collection gives that container a COUNTER id rather
    // than a triple hash -- so mark_rule_patterns dropped it at the is_hash
    // gate and never reached the membership facts. They then read as data:
    // the rule fired on the members of its own literal, which nobody had put
    // there, and .explain called them axioms.
    //
    // A collection has its own identity, so the container a rule writes is
    // one nothing else refers to: there is nothing for such a rule to match
    // and it must derive nothing at all.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in @{a b}) => (X flagged yes)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged yes");
        CHECK(collect_answers(collector).empty());

        // Both membership facts carry the mark, as ordinary graph structure.
        collector.clear();
        interactive.process(R"(S ~ "rule pattern")");
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("rule patterns: a SET constant in a condition is not a pattern")
{
    // The counterpart, and the reason the two literals had to be told apart.
    // A set constant IS its members -- `a in {a b}` holds by construction,
    // not because anybody claimed it -- so quantifying over them is exactly
    // what the rule means and marking them would be wrong.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in {a b}) => (X flagged yes)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "a flagged yes"));
        CHECK(answers_contain(collector, "b flagged yes"));

        collector.clear();
        interactive.process(R"(S ~ "rule pattern")");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: what a rule writes inside a set constant is rule text")
{
    // A set constant's inherent memberships define its essence and stay
    // unmarked. The content the rule penned within it constitutes its text,
    // just as with any other: `k in m` exists solely due to the rule's
    // inscription, and answered a query from the moment it was written. The
    // firing derives the set constant, whose membership constitutes data;
    // `k in m` persists as a pattern.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a fact")
        {
            interactive.process("(X q Y) => (X likes {(k in m)})");
            interactive.process("a q b");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S in O");
            CHECK_FALSE(answers_contain(collector, "k in m"));
            CHECK(answers_contain(collector, "(k in m) in {(k in m)}"));
            collector.clear();
            interactive.process("S likes O");
            CHECK(answers_contain(collector, "a likes {(k in m)}"));
        }
        SUBCASE("a collection")
        {
            interactive.process("(X q Y) => (X likes {@{c}})");
            collector.clear();
            interactive.process("S in O");
            CHECK_FALSE(answers_contain(collector, "c in @{c}"));
        } });
}

TEST_CASE("rule patterns: a rule a statement mentions writes its collections as rule text")
{
    // `((X p Y) => (X q @{c})) is noted` writes the rule it mentions, and
    // the collection that rule holds is contained within its text, just as
    // in a typed rule: its membership answers no query. The other ground
    // components of the mention retain their status -- a ground rule that is
    // mentioned may serve as data and as a premise.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("((X p Y) => (X q @{c})) is noted");
        collector.clear();
        interactive.process("S in O");
        CHECK_FALSE(any_output_starts_with(collector, "Answer:"));

        interactive.process("((a p b) => (c q d)) is odd");
        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "a p b")); });
}

TEST_CASE("rule patterns: a container the rule did not build keeps its facts")
{
    // The control for the walk's `fresh` gate. A set the rule merely REFERS
    // to -- here by the name `s`, which the data made a container -- is not
    // this construction's doing, so its membership facts stay data and the
    // rule fires on them. Red if the container walk marked whatever it found.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x in s
a in s
(X in s) => (X flagged yes)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "x flagged yes"));
        CHECK(answers_contain(collector, "a flagged yes"));

        collector.clear();
        interactive.process(R"(S ~ "rule pattern")");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("rule patterns: the mark survives .save and .load")
{
    // The record is a fact in the graph, not something the session
    // remembers -- so a round trip has to keep it, and the in-memory index
    // has to be read back off the graph afterwards.
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("(a p b) => (c q d)");
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d"));

        // .load disables auto-run.
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d"));
    }

    fs::remove(file);
}

TEST_CASE("rule patterns: a composite predicate in a pattern is not data either")
{
    // The descent through a rule's patterns walked the subject and the
    // objects but not the PREDICATE. For every ordinary rule that costs
    // nothing -- a named atom has no fact structure to descend into -- but a
    // COMPOSITE predicate is a ground fact node like any other, and writing
    // the rule is what brought it into being:
    //
    //     (a (b r s) c) => (d q e)
    //
    // left `b r s` behind as data, so `(X r Y) => (X leaked Y)` derived
    // `b leaked s` from a fact nobody had claimed. Exactly the leak afc0f3e
    // closed for the other two positions.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a (b r s) c) => (d q2 e)
(X r Y) => (X leaked Y)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S leaked O");
        CHECK(collect_answers(collector).empty());

        // It carries the marker, like the condition and the consequence.
        collector.clear();
        interactive.process("S Q O");
        CHECK(answers_contain(collector, "(b r s) ~ \"rule pattern\""));
        CHECK(answers_contain(collector, "(a (b r s) c) ~ \"rule pattern\"")); });
}

TEST_CASE("rule patterns: asserting a composite predicate revokes its mark too")
{
    // The counterpart: once the statement is CLAIMED it is data, and the rule
    // that quantifies over it fires -- the mark is revoked exactly as it is
    // for a subject or an object pattern.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a (b r s) c) => (d q2 e)
b r s
(X r Y) => (X leaked Y)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S leaked O");
        CHECK(answers_contain(collector, "b leaked s")); });
}
