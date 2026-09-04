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

TEST_CASE("primes: trial-division primality via rules (all arithmetic modules)" * doctest::test_suite("slow"))
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

TEST_CASE("primes: the two implementations agree with each other and with arithmetic")
{
    // `primes.zph` and `primes-naf.zph` solve the same problem twice, on
    // purpose and by different means: a positive fold that exits at the
    // smallest divisor, and the textbook negation-as-failure formulation that
    // needs the complete scan and a stratified evaluation. Two independent
    // answers to one question are the cheapest correctness check there is --
    // and the one thing neither module's own tests can do, since they check
    // it against itself.
    //
    // Ground truth is in the table, so a shared misconception in both would
    // not pass either.
    struct Case
    {
        int         n;
        const char* verdict;
    };
    static const Case expected[] = {
        {2, "prime"}, {3, "prime"}, {4, "composite"}, {5, "prime"}, {6, "composite"}, {7, "prime"}, {8, "composite"}, {9, "composite"}, {10, "composite"}, {11, "prime"}, {12, "composite"}, {13, "prime"}, {14, "composite"}, {15, "composite"}, {16, "composite"}, {17, "prime"}, {18, "composite"}, {19, "prime"}, {20, "composite"}};

    const auto verdicts = [](const char* module)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(std::string(".import ") + module);

        std::vector<std::string> out;
        for (const Case& c : expected)
        {
            collector.clear();
            interactive.process("? :testprime &" + std::to_string(c.n));

            std::string verdict;
            for (const auto& answer : collect_answers(collector))
            {
                if (answer.find("prime") != std::string::npos
                    && answer.find("composite") == std::string::npos)
                    verdict = "prime";
                else if (answer.find("composite") != std::string::npos)
                    verdict = "composite";
            }
            out.push_back(verdict);
        }
        return out;
    };

    const std::vector<std::string> fold = verdicts("primes");
    const std::vector<std::string> naf  = verdicts("primes-naf");

    REQUIRE(fold.size() == naf.size());
    for (std::size_t i = 0; i < fold.size(); ++i)
    {
        CAPTURE(expected[i].n);
        CHECK(fold[i] == expected[i].verdict); // against arithmetic
        CHECK(naf[i] == fold[i]);              // against each other
    }
}
