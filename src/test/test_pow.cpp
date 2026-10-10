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
// Exponentiation as a real operator: numerically over the naturals, over
// polynomial normal forms, in the simplifier and under differentiation.
// The design decision these tests pin: x^2 and x*x are DIFFERENT nodes,
// and their equality is proven by the polynomial layer rather than
// assumed by the parser.
// ---------------------------------------------------------------------------

TEST_CASE("pow: natural exponentiation (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        SUBCASE("3^4 = 81")
        {
            collector.clear();
            interactive.process("(&3 ^ &4) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&3 ^ &4) = &81)"));
        }
        SUBCASE("exponent zero is one, for every base")
        {
            collector.clear();
            interactive.process("(&7 ^ &0) = X");
            interactive.process("(&0 ^ &0) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&7 ^ &0) = &1)"));
            CHECK(any_output_contains(collector, "((&0 ^ &0) = &1)"));
        }
        SUBCASE("exponent one is the base")
        {
            collector.clear();
            interactive.process("(&35 ^ &1) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&35 ^ &1) = &35)"));
        }
        SUBCASE("a zero base with a positive exponent is zero")
        {
            collector.clear();
            interactive.process("(&0 ^ &5) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&0 ^ &5) = &0)"));
        } });
}

TEST_CASE("pow: polynomial identities involving powers" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        interactive.process("<x> ~ polyring");

        SUBCASE("the power and the product are PROVEN equal, not assumed")
        {
            collector.clear();
            interactive.process("? $( x^2 ) ≡ $( x*x )");
            CHECK(any_output_contains(collector, "= proven"));
        }
        SUBCASE("the binomial square expands")
        {
            collector.clear();
            interactive.process("? $( (1+x)^2 ) ≡ $( 1 + 2*x + x^2 )");
            CHECK(any_output_contains(collector, "= proven"));
        }
        SUBCASE("a wrong identity stays unanswered")
        {
            collector.clear();
            interactive.process("? $( (1+x)^2 ) ≡ $( 1 + x^2 )");
            CHECK_FALSE(any_output_contains(collector, "= proven"));
        }
        SUBCASE("exponent zero compiles to the constant one")
        {
            collector.clear();
            interactive.process("? $( x^0 ) ≡ $( 1 )");
            CHECK(any_output_contains(collector, "= proven"));
        }
        SUBCASE("the flagship identity still holds with ^ as an operator")
        {
            collector.clear();
            interactive.process("? $( (1+x)*(1-x) ) ≡ $( 1 - x^2 )");
            CHECK(any_output_contains(collector, "= proven"));
        } });
}

TEST_CASE("pow: the simplifier knows the neutral exponents")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        interactive.process("x ~ symvar");

        interactive.process("? :simplify $( x^1 )");
        interactive.process("? :simplify $( x^0 )");
        collector.clear();
        interactive.process(R"js(%(let [t (zelph/fact "x" "^" (zelph/number "1"))] (string "POW-S1-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
        interactive.process(R"js(%(let [t (zelph/fact "x" "^" (zelph/number "0"))] (string "POW-S0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "1")))))js");
        CHECK(any_output_contains(collector, "POW-S1-true"));
        CHECK(any_output_contains(collector, "POW-S0-true")); });
}

TEST_CASE("pow: the exponent is simplified like any other subterm (all arithmetic modules)" * doctest::test_suite("slow"))
{
    // The exponent was previously maintained precisely as written,
    // meaning a power with an exponent expressed as a numeral in
    // non-canonical form, a composite that folds, or the zint operand
    // form remained unchanged: (x ^ <01>) is (x ^ &1), yet it
    // constituted its own normal form, and (x ^ (pos zint &2)) kept a
    // notation that the simplifier normalizes to &2 in all other
    // contexts. Upon simplification, the exponent reaches the node
    // corresponding to its canonical spelling, and neutral exponents
    // and numeric folding are then applied to it.
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-integers");
        interactive.process(".import symbolic-pow");
        interactive.process("x ~ symvar");
        interactive.process("y ~ symvar");

        for (const auto& [term, normal] : {std::pair<std::string, std::string>{"(x ^ <01>)", "x"},
                                           {"(x ^ <00>)", "&1"},
                                           {"(x ^ (&1 + &2))", "(x ^ &3)"},
                                           {"(&2 ^ (&1 + &2))", "&8"},
                                           {"(x ^ (pos zint &2))", "(x ^ &2)"},
                                           {"(x ^ y)", "(x ^ y)"},
                                           {"(x ^ <a b>)", "(x ^ <a b>)"}})
        {
            CAPTURE(term);
            interactive.process(":simplify " + term);
            CHECK_FALSE(has_contradiction(collector));
            collector.clear();
            interactive.process("(:simplify " + term + ") = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify " + term + ") = " + normal));
        }

        // A subterm that lacks a normal form leaves its parent to lack one
        // as well, including the exponent: z is undeclared.
        interactive.process(":simplify (x ^ z)");
        collector.clear();
        interactive.process("(:simplify (x ^ z)) = X");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("pow: the power rule differentiates" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        interactive.process("x ~ symvar");

        SUBCASE("d(x^3)/dx = 3*x^2")
        {
            interactive.process("$( x^3 ) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact "x" "^" (zelph/number "3")) "diffby" "x") d (zelph/fact (zelph/number "3") "*" (zelph/fact "x" "^" (zelph/number "2")))] (string "POW-D3-" (zelph/exists t "=" d))))js");
            CHECK(any_output_contains(collector, "POW-D3-true"));
        }
        SUBCASE("d(x^1)/dx = 1 -- the exponent-one case composes")
        {
            interactive.process("$( x^1 ) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact "x" "^" (zelph/number "1")) "diffby" "x")] (string "POW-D1-" (zelph/exists t "=" (zelph/number "1")))))js");
            CHECK(any_output_contains(collector, "POW-D1-true"));
        }
        SUBCASE("d(x^0)/dx = 0 -- its own rule, since n-1 has no natural value")
        {
            interactive.process("$( x^0 ) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact "x" "^" (zelph/number "0")) "diffby" "x")] (string "POW-D0-" (zelph/exists t "=" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "POW-D0-true"));
        }
        SUBCASE("a negative signed zero as exponent acts as zero; (pos zint &0) stays silent")
        {
            // The power rule reads the exponent as expressed, via cmp. The
            // integer facade compares a natural against a negative zint and
            // reads every spelling of zero as the zero, thus (neg zint &0)
            // meets N == &0 and takes the exponent-zero rule -- yielding the
            // correct result, x^0 = 1. (pos zint N) adjacent to a natural
            // does not conform to the facade's routing shape, so the
            // canonical (pos zint &0) fails both comparisons and gets no
            // derivative. The documentation specifies this exception; this
            // pins it.
            interactive.process("(x ^ (neg zint &0)) diffby x");
            interactive.process("(x ^ (pos zint &0)) diffby x");
            collector.clear();
            interactive.process("((x ^ (neg zint &0)) diffby x) = D");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "((x ^ (neg zint &0)) diffby x) = &0"));
            collector.clear();
            interactive.process("((x ^ (pos zint &0)) diffby x) = D");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("the empty list as exponent is read as zero")
        {
            // The empty list does not qualify as a numeral and falls beyond
            // the scope of the arithmetic contract. It serves as the
            // exhausted tail of the digit recursions, though, so cmp reads
            // it as zero, and the power rule's exponent comparison with it.
            // The files symbolic-pow.zph and symbolic.md ("Exponentiation")
            // state this; if nil were ever to become silent instead, this
            // test and those sentences would need to be revised in unison.
            interactive.process("(x ^ nil) diffby x");
            collector.clear();
            interactive.process("((x ^ nil) diffby x) = D");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "((x ^ nil) diffby x) = &0"));
        } });
}

TEST_CASE("pow: powers render as powers" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");

        SUBCASE("^ binds tighter than * and +, so its parentheses vanish")
        {
            collector.clear();
            interactive.process("$( x^3 + 1 ) marker p1");
            CHECK(any_output_contains(collector, "$( x ^ &3 + &1 ) marker p1"));
        }
        SUBCASE("a composite base keeps its parentheses -- and needs no island")
        {
            collector.clear();
            interactive.process("$( (x+1)^2 ) marker p2");
            CHECK(any_output_contains(collector, "((x + &1) ^ &2) marker p2"));
            CHECK_FALSE(any_event_contains(collector, "$("));
        } });
}
