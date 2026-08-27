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
// Stratified negation-as-failure
//
// Rules with negated conditions form a deferred stratum: they are evaluated
// only when the positive rules have reached quiescence, so ¬(pattern) tests
// absence against the SATURATED positive fact base. These tests pin the
// probe scripts that demonstrated the former race (a ¬-rule firing before
// the facts matching its negated pattern were derived) as regressions.
// ---------------------------------------------------------------------------

// NOTE on negative checks: entering a rule ECHOES its definition on the
// Out channel -- "... => (A racewin A)" contains the consequence
// predicate name. A CHECK_FALSE on the bare predicate therefore
// false-positives on the echo. Negative checks must match the
// INSTANTIATED fact ("x racewin x"), never the predicate alone.

TEST_CASE("stratified: negation defers until positive quiescence (probe A)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A trigger A) => (A step1 A)
(A step1 A) => (A step2 A)
(A trigger A, ¬(A step2 A)) => (A racewin A)
x trigger x
)");
        CHECK(any_output_contains(collector, "x step2 x"));
        CHECK_FALSE(any_output_contains(collector, "x racewin x")); });
}

TEST_CASE("stratified: rule definition order does not matter (probe A')")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Same rules as probe A, but the negation rule is defined FIRST.
        // Under the racy engine, rule order (an unordered set internally)
        // could decide the outcome; under stratification it cannot.
        process_lines(interactive, R"(
(A trigger A, ¬(A step2 A)) => (A racewin A)
(A trigger A) => (A step1 A)
(A step1 A) => (A step2 A)
x trigger x
)");
        CHECK(any_output_contains(collector, "x step2 x"));
        CHECK_FALSE(any_output_contains(collector, "x racewin x")); });
}

TEST_CASE("stratified: deep positive chains are saturated first (probe B)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A s0 A) => (A s1 A)
(A s1 A) => (A s2 A)
(A s2 A) => (A s3 A)
(A s3 A) => (A s4 A)
(A s0 A, ¬(A s4 A)) => (A racewin A)
y s0 y
)");
        CHECK(any_output_contains(collector, "y s4 y"));
        CHECK_FALSE(any_output_contains(collector, "y racewin y")); });
}

TEST_CASE("stratified: completion-witness pattern is order-independent (probe C)")
{
    // Under the racy engine this case happened to be correct only because
    // the bad-rule was internally ordered before the seen/done chain --
    // pure hash-order luck. Stratification makes it correct by semantics.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A go A) => (A m A)
(A m A) => (A bad A)
(A m A) => (A seen A)
(A seen A) => (A done A)
(A done A, ¬(A bad A)) => (A verdict A)
z go z
)");
        CHECK(any_output_contains(collector, "z bad z"));
        CHECK_FALSE(any_output_contains(collector, "z verdict z")); });
}

TEST_CASE("stratified: latecomer facts replay the schedule (probe D)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A trigger A) => (A step1 A)
(A step1 A) => (A step2 A)
(A trigger A, ¬(A step2 A)) => (A racewin A)
x trigger x
)");
        // A fresh input after the fixpoint starts a new engine run; the
        // deferred schedule must hold there as well -- for the newcomer
        // AND (still) for x.
        collector.clear();
        interactive.process("x2 trigger x2");
        CHECK(any_output_contains(collector, "x2 step2 x2"));
        CHECK_FALSE(any_output_contains(collector, "racewin")); });
}

TEST_CASE("stratified: negation still fires when the pattern is truly absent")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Base facts BEFORE the trigger: stratification fixes races among
        // DERIVED facts; user assertions remain subject to plain NAF
        // semantics at the time of each run.
        process_lines(interactive, R"(
(A trigger A, ¬(A blocked A)) => (A allowed A)
gated blocked gated
gated trigger gated
free trigger free
)");
        CHECK(any_output_contains(collector, "free allowed free"));
        CHECK_FALSE(any_output_contains(collector, "gated allowed gated")); });
}

TEST_CASE("stratified: deferred consequences re-open the positive stratum")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The negation rule's output feeds an ordinary positive rule:
        // phase 2 -> delta -> phase 1 must cascade.
        process_lines(interactive, R"(
(A start A) => (A p A)
(A p A, ¬(A q A)) => (A r A)
(A r A) => (A s A)
w start w
)");
        CHECK(any_output_contains(collector, "w r w"));
        CHECK(any_output_contains(collector, "w s w")); });
}

TEST_CASE("stratified: a free variable inside a negation is quantified inside it")
{
    // `¬` has ONE reading: the condition succeeds exactly when no fact
    // matches, and a free variable inside it produces no binding that
    // leaves the rule. Both directions therefore answer the way their names
    // suggest on the same graph.
    //
    // There used to be a second reading, chosen by whether the pattern's
    // SUBJECT happened to be bound: with it free, the engine took the
    // subjects of that relation as a domain and let the negation succeed
    // once per candidate the pattern failed for, binding it. `is earliest`
    // then came out for all three intervals, one of them justified by the
    // self-fact `¬(b before b)` -- and adding a positive condition
    // elsewhere silently switched between the two.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a ~ interval
b ~ interval
c ~ interval
a before b
b before c
(A ~ interval, ¬(A before B)) => (A is latest)
(A ~ interval, ¬(B before A)) => (A is earliest)
)");
        CHECK(any_output_contains(collector, "c is latest"));
        CHECK_FALSE(any_output_contains(collector, "a is latest"));
        CHECK_FALSE(any_output_contains(collector, "b is latest"));

        CHECK(any_output_contains(collector, "a is earliest"));
        CHECK_FALSE(any_output_contains(collector, "b is earliest"));
        CHECK_FALSE(any_output_contains(collector, "c is earliest")); });
}

// What `¬(F)` means when it stands on its own line rather than in a rule
// condition. It used to mean the opposite of itself: the sugar builds its
// operand with zelph/fact and tags the result, so the line ASSERTED F and then
// marked the node it had just claimed as negated -- `.node` said "Negated by a
// rule: yes" on a fact that answered every positive query.
//
// It now reaches the mechanism zelph has always had for a negative claim: the
// probability argument of Zelph::fact, and Answer::is_wrong over it.
TEST_CASE("negation: ¬(F) on its own line claims that F does not hold")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
¬(a p b)
c p d
(X p Y) => (X q Y)
)");
        // The refuted fact answers nothing and no rule fires on it, while the
        // ordinary one beside it does both.
        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "c p d"));
        CHECK_FALSE(any_output_contains(collector, "a p b"));

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "c q d"));
        CHECK_FALSE(any_output_contains(collector, "a q b"));

        collector.clear();
        interactive.process(R"(%(if (zelph/exists "a" "p" "b") "holds" "does-not-hold"))");
        CHECK(any_output_contains(collector, "does-not-hold"));

        // And it prints as what it is. The echo used to come back as `a p b`,
        // which re-enters as the OPPOSITE of the line that produced it -- the
        // round trip is part of the notation, not a convenience.
        collector.clear();
        interactive.process("¬(x q y)");
        CHECK(any_output_contains(collector, "¬(x q y)"));

        // And a command can still ADDRESS it by printing it back. Every
        // command that takes a fact pattern resolves it by generating the same
        // code a statement generates, which runs through zelph/fact -- so the
        // moment a fact could be refuted, ".node" and ".explain" answered
        // "Unknown node" for exactly the facts a user has most reason to look
        // at. They look the pattern UP now instead of asserting it.
        collector.clear();
        interactive.process(".node a p b");
        CHECK(any_output_contains(collector, "Refuted (claimed not to hold): yes"));
        CHECK(any_output_contains(collector, "¬(a p b)"));

        // And the graph refuses to claim both. Zelph::fact has always had this
        // guard; nothing could reach it before, because no spelling created a
        // fact below probability 0.5.
        CHECK_THROWS(interactive.process("a p b")); });
}

TEST_CASE("negation: a pattern is looked up, not asserted, when a command resolves it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "c p d\n");
        collector.clear();

        // The other half of the same property, and the one that says the
        // lookup did not quietly become a second way to assert: naming a
        // pattern the graph does NOT hold still denotes its node -- the id is
        // the hash of the triple -- and must not put it into the graph.
        interactive.process(".explain (e p f)");
        CHECK(any_output_contains(collector, "not asserted"));

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "c p d"));
        CHECK_FALSE(any_output_contains(collector, "e p f")); });
}

TEST_CASE("negation: a refutation survives a save and a load")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_refuted_roundtrip.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "¬(a p b)\nc p d\n");
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    collector.clear();

    // The probability alone could not carry this: the weight store is keyed by
    // a hash of the edge, so a loaded file cannot be asked which of its entries
    // were fact probabilities. The marking fact is what the index is rebuilt
    // from, exactly as for a rule pattern.
    interactive.process("S p O");
    CHECK(answers_contain(collector, "c p d"));
    CHECK_FALSE(any_output_contains(collector, "a p b"));

    std::filesystem::remove(file);
}

TEST_CASE("stratified: ¬ over an inequality guard is refused, not silently dropped")
{
    // `!=` is a built-in constraint, not a fact to look up, and it is
    // dispatched by its predicate before the leaf branch that reads the
    // negation tag. So `¬(X != Y)` used to reach the guard WITHOUT its tag and
    // be evaluated as `X != Y`: the rule below derived `alice same-as bob` --
    // exactly the pairs that are DIFFERENT -- and printed `¬(alice != bob)` in
    // the justification of it.
    //
    // It is refused rather than given a reading, because a negated inequality
    // is an equality constraint and writing the SAME VARIABLE twice already
    // says that, with the advantage that unification then uses it to narrow
    // the search instead of testing afterwards.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
alice member Q1
bob member Q1
(X member C, Y member C, ¬(X != Y)) => (X same-as Y)
)");
        CHECK(any_output_contains(collector, "¬ cannot be applied to \"!=\""));

        // And the refusal is one line, not one per candidate pair.
        size_t refusals = 0;
        for (const auto& event : collector.events())
            if (event.text.find("¬ cannot be applied") != std::string::npos) ++refusals;
        CHECK(refusals == 1);

        // The rule derives nothing at all -- asked of the graph rather than of
        // the output, where the echo of the rule itself mentions the predicate.
        collector.clear();
        interactive.process("P same-as Q");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("stratified: ranging over a domain is a positive condition")
{
    // What the second reading of `¬` used to do implicitly is written down
    // instead: the positive condition says WHICH candidates are considered,
    // the negation only filters them, and the justification of each result
    // names both.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x flagged good
y flagged bad
z flagged good
(X flagged S, ¬(X flagged bad)) => (X unflagged ok)
)");
        collector.clear();
        interactive.process("Q unflagged ok");
        CHECK(answers_contain(collector, "x unflagged ok"));
        CHECK(answers_contain(collector, "z unflagged ok"));
        CHECK_FALSE(any_output_contains(collector, "y unflagged")); });
}

TEST_CASE("stratified: doc example -- last element of a chain via negation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
elem1 --> elem2
elem2 --> elem3
elem3 --> elem4
elem4 --> elem5
elem1 partoflist mylist
elem2 partoflist mylist
elem3 partoflist mylist
elem4 partoflist mylist
elem5 partoflist mylist
(A partoflist L, ¬(A --> X)) => (A "is last of" L)
)");
        CHECK(any_output_contains(collector, "elem5 \"is last of\" mylist"));
        CHECK_FALSE(any_output_contains(collector, "elem1 \"is last of\""));
        CHECK_FALSE(any_output_contains(collector, "elem4 \"is last of\"")); });
}

TEST_CASE("stratified: classic (naive) evaluation defers negation too")
{
    // run_both_modes exercises the semi-naive scheduler (in check mode);
    // the classic two-phase loop in Reasoning::run is a separate code path.
    static const std::string script = R"(
(A trigger A) => (A step1 A)
(A step1 A) => (A step2 A)
(A trigger A, ¬(A step2 A)) => (A racewin A)
x trigger x
)";

    SUBCASE("parallel")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        collector.clear();
        process_lines(interactive, script);
        CHECK(any_output_contains(collector, "x step2 x"));
        CHECK_FALSE(any_output_contains(collector, "x racewin x"));
    }
    SUBCASE("single-core")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".parallel");
        interactive.process(".semi-naive off");
        collector.clear();
        process_lines(interactive, script);
        CHECK(any_output_contains(collector, "x step2 x"));
        CHECK_FALSE(any_output_contains(collector, "x racewin x"));
    }
}

TEST_CASE("stratified: the deferred stratum re-runs until the alternation reaches its fixpoint")
{
    // Two deferred rounds are required: the first deferred pass derives
    // (w q w), the positive rule turns it into (w r w), and only then can
    // the second deferred rule fire at the NEXT stratum boundary.
    // Distilled from the symbolic-math regression where simplifying a
    // compiled EML tree needed the identity fallback on two nesting
    // levels of one term: the semi-naive scheduler used to run the
    // deferred stratum exactly once, so the final fact was only derived
    // by the check-mode safety pass -- a completeness violation.
    static const std::string script = R"(
(A start A) => (A p A)
(A p A, ¬(A blockp A)) => (A q A)
(A q A) => (A r A)
(A r A, ¬(A blockr A)) => (A s A)
w start w
)";

    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, script);
        CHECK(any_output_contains(collector, "w q w"));
        CHECK(any_output_contains(collector, "w r w"));
        CHECK(any_output_contains(collector, "w s w")); });

    SUBCASE("classic (naive) evaluation alternates too")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        collector.clear();
        process_lines(interactive, script);
        CHECK(any_output_contains(collector, "w s w"));
    }
}

// ---------------------------------------------------------------------------
// The payoff: the textbook primality rule, sound under stratification
// ---------------------------------------------------------------------------

TEST_CASE("primes-naf: textbook negation rule on the arithmetic modules")
{
    run_arithmetic_modules([](auto& collector, const auto& interactive)
                           {
        interactive.process(".import primes-naf");

        SUBCASE("2 is prime (base case)")
        {
            collector.clear();
            interactive.process("(&2 testprime &2) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&2 testprime &2) = prime"));
        }
        SUBCASE("13 is prime, result query is repeatable")
        {
            collector.clear();
            interactive.process("(&13 testprime &13) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&13 testprime &13) = prime"));
            CHECK_FALSE(any_output_contains(collector, "(&13 testprime &13) = composite"));

            collector.clear();
            interactive.process("(&13 testprime &13) = X");
            CHECK(answers_contain(collector, "(&13 testprime &13) = prime"));
        }
        SUBCASE("42 is composite and NOT prime (the race the old engine lost)")
        {
            collector.clear();
            interactive.process("(&42 testprime &42) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&42 testprime &42) = composite"));
            CHECK_FALSE(any_output_contains(collector, "&42 isprime &42"));
            CHECK_FALSE(any_output_contains(collector, "(&42 testprime &42) = prime"));
        }
        SUBCASE("9 is composite (square boundary E*E == N)")
        {
            collector.clear();
            interactive.process("(&9 testprime &9) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&9 testprime &9) = composite"));
            CHECK(any_output_contains(collector, "&9 hasdivisor &3"));
            CHECK_FALSE(any_output_contains(collector, "&9 isprime &9"));
        }
        SUBCASE("0 and 1 are neither prime nor composite (no verdict)")
        {
            collector.clear();
            interactive.process("(&1 testprime &1) = X");
            interactive.process("(&0 testprime &0) = X");
            interactive.run(true, false, false);
            CHECK_FALSE(any_output_contains(collector, "(&1 testprime &1) = prime"));
            CHECK_FALSE(any_output_contains(collector, "(&1 testprime &1) = composite"));
            CHECK_FALSE(any_output_contains(collector, "(&0 testprime &0) = prime"));
            CHECK_FALSE(any_output_contains(collector, "(&0 testprime &0) = composite"));
        } });
}

TEST_CASE("stratified: deferred alternation with sugar-form negated conditions")
{
    // Same two-round alternation as the test above, written entirely in
    // self-fact sugar -- including the combination of two desugarings:
    // ":pred X" nested inside "¬(...)" inside a conjunction. Not a bug
    // regression; pinned because this stacking of sugar forms is exercised
    // nowhere else, and grammar changes to either sugar must not break it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(:start A) => (:p A)
(:p A, ¬(:blockp A)) => (:q A)
(:q A) => (:r A)
(:r A, ¬(:blockr A)) => (:s A)
:start w
)");
        CHECK(any_output_contains(collector, "w q w"));
        CHECK(any_output_contains(collector, "w r w"));
        CHECK(any_output_contains(collector, "w s w")); });
}

TEST_CASE("negation: a group of conditions cannot be negated")
{
    // zelph negates a single fact PATTERN. A node tagged as a conjunction
    // was read as one before its negation tag was ever consulted, so
    // "¬(A is y, A is z)" evaluated as "A is y AND A is z" -- the exact
    // opposite of what was written, and without a word about it. Rejecting
    // the combination is checked where the tag is created, which catches
    // the ¬ sugar and the explicit "*(...) ~ negation" spelling alike.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process("(A is x, ¬(A is y, A is z)) => (A r s)"),
                             doctest::Contains("conjunction and a negation"),
                             std::runtime_error);

        CHECK_THROWS_WITH_AS(
            interactive.process("(A is x, *(*{(A is y) (A is z)} ~ conjunction) ~ negation) => (A r s)"),
            doctest::Contains("conjunction and a negation"),
            std::runtime_error);

        // The ordinary case is untouched.
        interactive.process("(A is yellow, ¬(A is green)) => (A notgreen green)");
        interactive.process("plant is green");
        interactive.process("plant is yellow");
        interactive.process("plant2 is yellow");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("X notgreen green");
        CHECK(answers_contain(collector, "plant2 notgreen green"));
        CHECK_FALSE(any_output_contains(collector, "plant notgreen")); });
}
