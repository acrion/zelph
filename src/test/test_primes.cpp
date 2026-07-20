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

// NOTE on expected substrings: atom results (prime, composite, gt, ...)
// are rendered with surrounding spaces ("=   prime )"), so the closing
// parenthesis is preceded by a space after normalization. Expected
// substrings for atom-valued results therefore OMIT the trailing ")" --
// the same convention as "(&42 cmp &9) = gt" in test_numbers.cpp.
// Number results (&46) have no trailing space, which is why patterns
// like "((&12 + &34) = &46)" may keep theirs.

TEST_CASE("primes: trial-division primality via rules (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, const auto& interactive)
                           {
        interactive.process(".import primes");

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

            collector.clear();
            interactive.process("(&13 testprime &13) = X");
            CHECK(answers_contain(collector, "(&13 testprime &13) = prime"));
        }
        SUBCASE("9 is composite with witness 3 (square boundary E*E == N)")
        {
            collector.clear();
            interactive.process("(&9 testprime &9) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&9 testprime &9) = composite"));
            CHECK(any_output_contains(collector, "&9 hasdivisor &3"));
            
            // Full triple, not the "&9 isprime" fragment: the fact renders in
            // self-fact sugar (":isprime &9"), which the equivalence layer in
            // test_helpers only derives from a complete S P S pattern -- a
            // bare fragment would silently lose its guarding effect.
            CHECK_FALSE(any_output_contains(collector, "&9 isprime &9"));
        }
        SUBCASE("15 is composite; the search halts at the smallest divisor")
        {
            collector.clear();
            interactive.process("(&15 testprime &15) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&15 testprime &15) = composite"));
            CHECK(any_output_contains(collector, "&15 hasdivisor &3"));
            // Lazy candidate generation: once 3 is found, no further
            // candidate is tested -- 5 is deliberately NOT enumerated.
            CHECK_FALSE(any_output_contains(collector, "&15 hasdivisor &5"));
        }
        SUBCASE("0 and 1 are neither prime nor composite (no verdict)")
        {
            collector.clear();
            interactive.process("(&1 testprime &1) = X");
            interactive.process("(&0 testprime &0) = X");
            interactive.run(true, false, false);
            // The queries themselves are echoed ("... = X"), so testing for
            // "testprime &1) =" would false-positive on the echo. Check the
            // two possible verdicts instead.
            CHECK_FALSE(any_output_contains(collector, "(&1 testprime &1) = prime"));
            CHECK_FALSE(any_output_contains(collector, "(&1 testprime &1) = composite"));
            CHECK_FALSE(any_output_contains(collector, "(&0 testprime &0) = prime"));
            CHECK_FALSE(any_output_contains(collector, "(&0 testprime &0) = composite"));
        } });
}
