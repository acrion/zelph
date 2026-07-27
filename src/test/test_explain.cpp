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
// .explain: proof reconstruction from the saturated graph. No provenance is
// tracked during inference -- these tests pin that the backward search alone
// recovers justifications, labels axioms honestly, respects the depth limit,
// and prints shared subproofs once.
// ---------------------------------------------------------------------------

TEST_CASE("explain: axioms and single-step derivations")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X likes Y) => (Y liked-by X)");
        interactive.process("alice likes bob");
        interactive.run(true, false, false);

        SUBCASE("input facts are axioms")
        {
            collector.clear();
            interactive.process(".explain alice likes bob");
            CHECK(any_output_contains(collector, "[axiom]"));
        }
        SUBCASE("derived facts show their premise")
        {
            collector.clear();
            interactive.process(".explain bob liked-by alice");
            CHECK(any_output_contains(collector, "alice likes bob"));
            CHECK(any_output_contains(collector, "[axiom]"));
        }
        SUBCASE("unasserted facts are reported, not invented")
        {
            collector.clear();
            interactive.process(".explain bob likes alice");
            CHECK(any_output_contains(collector, "not asserted"));
        } });
}

TEST_CASE("explain: depth limit and the ? companion idiom (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process("? (&6 + &7)");

        SUBCASE("bare .explain explains the last output node")
        {
            collector.clear();
            interactive.process(".explain");
            CHECK(any_output_contains(collector, "&13"));
        }
        SUBCASE("depth 1 truncates, depth 0 does not")
        {
            collector.clear();
            interactive.process(".explain ((&6 + &7) = &13) 1");
            CHECK(any_output_contains(collector, "depth limit"));

            collector.clear();
            interactive.process(".explain ((&6 + &7) = &13) 0");
            CHECK_FALSE(any_output_contains(collector, "depth limit"));
        } });
}

TEST_CASE("explain: shared subproofs print once")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // (a q b) is DERIVED and used twice: directly as a premise of
        // (a done b), and again as the premise of (a r b). Hash-consing
        // makes both occurrences the same node, so the second one must
        // reference the first instead of re-printing its subtree.
        // Axiom leaves are NOT subject to this: "[axiom]" already is the
        // complete expansion and stays readable when repeated.
        interactive.process("(X p Y) => (X q Y)");
        interactive.process("(X q Y) => (X r Y)");
        interactive.process("(X q Y, X r Y) => (X done Y)");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (a done b) 0");
        CHECK(any_output_contains(collector, "[see above]")); });
}

TEST_CASE("explain: NAF premises render as absent (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import primes-naf");
        interactive.process("? :testprime &7");

        collector.clear();
        interactive.process(".explain (:testprime &7) = prime 0");
        CHECK(any_output_contains(collector, "[absent]")); });
}
