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
        CHECK(any_output_contains(collector, "elem5 is last of mylist"));
        CHECK_FALSE(any_output_contains(collector, "elem1 is last of"));
        CHECK_FALSE(any_output_contains(collector, "elem4 is last of")); });
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
            CHECK_FALSE(any_output_contains(collector, "&42 isprime"));
            CHECK_FALSE(any_output_contains(collector, "(&42 testprime &42) = prime"));
        }
        SUBCASE("9 is composite (square boundary E*E == N)")
        {
            collector.clear();
            interactive.process("(&9 testprime &9) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&9 testprime &9) = composite"));
            CHECK(any_output_contains(collector, "&9 hasdivisor &3"));
            CHECK_FALSE(any_output_contains(collector, "&9 isprime"));
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
