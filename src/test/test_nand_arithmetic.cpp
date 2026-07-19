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
// binary-nand-arithmetic: module-specific pins
//
// End-to-end coverage of all operations comes from the generalized tests
// via run_arithmetic_modules. The cases here pin what is unique to this
// module: the single-axiom NAND table completed by stratified
// negation-as-failure, and the gate-synthesized digit tables.
//
// NOTE on expected substrings: plain atom nodes are rendered with
// surrounding spaces, while &-literals and <...> lists attach to their
// delimiters (cf. the NOTE in test_primes.cpp). A gate fact therefore
// normalizes to "( 1 nand 1 ) out 0", NOT "(1 nand 1) out 0". Queries
// also echo their own input ("( 1 nand 1 ) out X"), so negative checks
// must include the concrete digit object ("out 1"), which cannot
// false-positive on the echoed "out X".
// ---------------------------------------------------------------------------

TEST_CASE("nand-arithmetic: NAF completes the gate table without corrupting the axiom row")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import binary-nand-arithmetic");
        interactive.run(true, false, false);

        // The axiom row must not acquire a second, NAF-derived output.
        // This is the ordering catastrophe the module comment warns
        // about: were rule G0 defined before the axiom fact, a run could
        // derive ((1 nand 1) out 1), and monotonicity could never
        // retract it.
        collector.clear();
        interactive.process("(1 nand 1) out X");
        CHECK(any_output_contains(collector, "( 1 nand 1 ) out 0"));
        CHECK_FALSE(any_output_contains(collector, "( 1 nand 1 ) out 1"));

        // The three NAF-completed rows output 1 -- and never 0.
        collector.clear();
        interactive.process("(0 nand 0) out X");
        interactive.process("(0 nand 1) out X");
        interactive.process("(1 nand 0) out X");
        CHECK(any_output_contains(collector, "( 0 nand 0 ) out 1"));
        CHECK(any_output_contains(collector, "( 0 nand 1 ) out 1"));
        CHECK(any_output_contains(collector, "( 1 nand 0 ) out 1"));
        CHECK_FALSE(any_output_contains(collector, "( 0 nand 0 ) out 0"));
        CHECK_FALSE(any_output_contains(collector, "( 0 nand 1 ) out 0"));
        CHECK_FALSE(any_output_contains(collector, "( 1 nand 0 ) out 0")); });
}

TEST_CASE("nand-arithmetic: gate-synthesized digit tables match the hand-written truth tables")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import binary-nand-arithmetic");
        interactive.run(true, false, false);

        // Spot-check the carry-heavy corner of each synthesized table
        // (expected values from binary-arithmetic.zph).
        collector.clear();
        interactive.process("((1 d+ 1) tci 1) sum X");
        interactive.process("((1 d+ 1) tci 1) co X");
        CHECK(any_output_contains(collector, "(( 1 d+ 1 ) tci 1 ) sum 1"));
        CHECK(any_output_contains(collector, "(( 1 d+ 1 ) tci 1 ) co 1"));
        CHECK_FALSE(any_output_contains(collector, "(( 1 d+ 1 ) tci 1 ) sum 0"));

        collector.clear();
        interactive.process("((0 d- 1) tbi 1) diff X");
        interactive.process("((0 d- 1) tbi 1) bo X");
        CHECK(any_output_contains(collector, "(( 0 d- 1 ) tbi 1 ) diff 0"));
        CHECK(any_output_contains(collector, "(( 0 d- 1 ) tbi 1 ) bo 1"));

        collector.clear();
        interactive.process("((1 dx 1) tci 1) pd X");
        interactive.process("((1 dx 1) tci 1) mco X");
        CHECK(any_output_contains(collector, "(( 1 dx 1 ) tci 1 ) pd 0"));
        CHECK(any_output_contains(collector, "(( 1 dx 1 ) tci 1 ) mco 1"));

        // Comparison: gt/lt via gates, eq via node identity.
        collector.clear();
        interactive.process("(1 dcmp 0) res X");
        interactive.process("(0 dcmp 1) res X");
        interactive.process("(1 dcmp 1) res X");
        CHECK(any_output_contains(collector, "( 1 dcmp 0 ) res gt"));
        CHECK(any_output_contains(collector, "( 0 dcmp 1 ) res lt"));
        CHECK(any_output_contains(collector, "( 1 dcmp 1 ) res eq"));
        CHECK_FALSE(any_output_contains(collector, "( 1 dcmp 1 ) res gt"));
        CHECK_FALSE(any_output_contains(collector, "( 1 dcmp 1 ) res lt")); });
}