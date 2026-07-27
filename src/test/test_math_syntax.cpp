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
// math-syntax.zph: the $( ... ) term island -- conventional infix notation
// with precedence, desugaring to the exact graph structures the verbose
// syntax builds. Hash-consing makes that equivalence testable as NODE
// IDENTITY: every probe rebuilds the expected term verbosely via zelph/fact
// and checks that the island-built fact connects to it.
// ---------------------------------------------------------------------------

TEST_CASE("math-syntax: precedence and associativity (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import math-syntax");

        interactive.process("$( a + b*c ) ~ p1");
        interactive.process("$( (a + b)*c ) ~ p2");
        interactive.process("$( a - b - c ) ~ p3");
        interactive.process("$( a / b * c ) ~ p4");
        collector.clear();
        interactive.process(R"js(%(string "MS-PREC1-" (zelph/exists (zelph/fact "a" "+" (zelph/fact "b" "*" "c")) "~" "p1")))js");
        interactive.process(R"js(%(string "MS-PREC2-" (zelph/exists (zelph/fact (zelph/fact "a" "+" "b") "*" "c") "~" "p2")))js");
        interactive.process(R"js(%(string "MS-ASSOC1-" (zelph/exists (zelph/fact (zelph/fact "a" "-" "b") "-" "c") "~" "p3")))js");
        interactive.process(R"js(%(string "MS-ASSOC2-" (zelph/exists (zelph/fact (zelph/fact "a" "/" "b") "*" "c") "~" "p4")))js");
        CHECK(any_output_contains(collector, "MS-PREC1-true"));
        CHECK(any_output_contains(collector, "MS-PREC2-true"));
        CHECK(any_output_contains(collector, "MS-ASSOC1-true"));
        CHECK(any_output_contains(collector, "MS-ASSOC2-true")); });
}

TEST_CASE("math-syntax: numeric literals and unary minus (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import math-syntax");

        interactive.process("$( 2 + 3 ) ~ n1");
        interactive.process("$( -x ) ~ n2");
        interactive.process("$( 2 - -3 ) ~ n3");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(string "MS-NUM-" (zelph/exists (zelph/fact (zelph/number "2") "+" (zelph/number "3")) "~" "n1")))js");
        interactive.process(R"js(%(string "MS-NEG-" (zelph/exists (zelph/fact "neg" "of" "x") "~" "n2")))js");
        interactive.process(R"js(%(string "MS-NEGNUM-" (zelph/exists (zelph/fact (zelph/number "2") "-" (zelph/fact "neg" "of" (zelph/number "3"))) "~" "n3")))js");
        CHECK(any_output_contains(collector, "MS-NUM-true"));
        CHECK(any_output_contains(collector, "MS-NEG-true"));
        CHECK(any_output_contains(collector, "MS-NEGNUM-true")); });
}

TEST_CASE("math-syntax: powers build a ^ term (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import math-syntax");

        interactive.process("$( x^3 ) ~ w1");
        interactive.process("$( x^1 ) ~ w2");
        interactive.process("$( x^0 ) ~ w3");
        interactive.process("$( (a+b)^2 ) ~ w4");
        interactive.process("$( 2*x^2 ) ~ w5");
        collector.clear();
        interactive.process(R"js(%(string "MS-POW3-" (zelph/exists (zelph/fact "x" "^" (zelph/number "3")) "~" "w1")))js");
        interactive.process(R"js(%(string "MS-POW1-" (zelph/exists (zelph/fact "x" "^" (zelph/number "1")) "~" "w2")))js");
        interactive.process(R"js(%(string "MS-POW0-" (zelph/exists (zelph/fact "x" "^" (zelph/number "0")) "~" "w3")))js");
        interactive.process(R"js(%(let [s (zelph/fact "a" "+" "b")] (string "MS-POWP-" (zelph/exists (zelph/fact s "^" (zelph/number "2")) "~" "w4"))))js");
        interactive.process(R"js(%(string "MS-POWPREC-" (zelph/exists (zelph/fact (zelph/number "2") "*" (zelph/fact "x" "^" (zelph/number "2"))) "~" "w5")))js");
        CHECK(any_output_contains(collector, "MS-POW3-true"));
        CHECK(any_output_contains(collector, "MS-POW1-true"));
        CHECK(any_output_contains(collector, "MS-POW0-true"));
        CHECK(any_output_contains(collector, "MS-POWP-true"));
        CHECK(any_output_contains(collector, "MS-POWPREC-true")); });
}

TEST_CASE("math-syntax: function application; nested parens exercise the veto")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math-syntax");

        // The nested ')' of the inner application forces the host's
        // close-delimiter scan through two :incomplete vetoes.
        interactive.process("$( exp(ln(x)) ) ~ f1");
        interactive.process("$( exp(x + y)*z ) ~ f2");
        collector.clear();
        interactive.process(R"js(%(string "MS-FUN-" (zelph/exists (zelph/fact "exp" "of" (zelph/fact "ln" "of" "x")) "~" "f1")))js");
        interactive.process(R"js(%(string "MS-FUNM-" (zelph/exists (zelph/fact (zelph/fact "exp" "of" (zelph/fact "x" "+" "y")) "*" "z") "~" "f2")))js");
        CHECK(any_output_contains(collector, "MS-FUN-true"));
        CHECK(any_output_contains(collector, "MS-FUNM-true")); });
}

TEST_CASE("math-syntax: variables in islands write rules; sugar and verbose forms unify")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math-syntax");

        // Rule written with an island pattern; matching facts asserted
        // once with sugar, once verbosely -- both must fire the rule,
        // pinning cross-form node identity.
        interactive.process(R"((T r $( X + Y )) => (X partner Y))");
        interactive.process("t1 r $( a + b )");
        interactive.process("t2 r (c + d)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(string "MS-VAR1-" (zelph/exists "a" "partner" "b")))js");
        interactive.process(R"js(%(string "MS-VAR2-" (zelph/exists "c" "partner" "d")))js");
        CHECK(any_output_contains(collector, "MS-VAR1-true"));
        CHECK(any_output_contains(collector, "MS-VAR2-true"));

        CHECK_THROWS_AS(interactive.process("$( X ) ~ bad"), std::runtime_error); });
}

TEST_CASE("math-syntax: integration with the symbolic pipeline (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import diff");
        interactive.process(".import math-syntax");
        interactive.process("x ~ symvar");

        interactive.process(":simplify $( x + 0 )");
        interactive.process(":simplify $( (2+3)*(4+6) )");
        interactive.process("$( x*x ) diffby x");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(let [t (zelph/fact "x" "+" (zelph/number "0"))] (string "MS-SIMP-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
        interactive.process(R"js(%(let [t (zelph/fact (zelph/fact (zelph/number "2") "+" (zelph/number "3")) "*" (zelph/fact (zelph/number "4") "+" (zelph/number "6")))] (string "MS-FOLD-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "50")))))js");
        interactive.process(R"js(%(string "MS-DIFF-" (zelph/exists (zelph/fact (zelph/fact "x" "*" "x") "diffby" "x") "=" (zelph/fact "x" "+" "x"))))js");
        CHECK(any_output_contains(collector, "MS-SIMP-true"));
        CHECK(any_output_contains(collector, "MS-FOLD-true"));
        CHECK(any_output_contains(collector, "MS-DIFF-true")); });
}

TEST_CASE("math-syntax: default substrate works out of the box; blocked substrate fails honestly")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("bare import suffices for numeric literals")
        {
            collector.clear();
            interactive.process(".import math-syntax");
            CHECK(any_output_contains(collector, "math-syntax loaded"));

            interactive.process("$( 1 + 2 ) ~ ok");
            collector.clear();
            interactive.process(R"js(%(string "MS-DEF-" (zelph/exists (zelph/fact (zelph/number "1") "+" (zelph/number "2")) "~" "ok")))js");
            CHECK(any_output_contains(collector, "MS-DEF-true"));
        }
        SUBCASE("claiming the arithmetic ID blocks the default import")
        {
            interactive.process(".provides arithmetic");
            interactive.process(".import math-syntax");

            // Non-numeric islands still work ...
            interactive.process("$( a ) ~ ok");
            collector.clear();
            interactive.process(R"js(%(string "MS-BLK-" (zelph/exists "a" "~" "ok")))js");
            CHECK(any_output_contains(collector, "MS-BLK-true"));

            // ... numeric literals surface the zelph/number fallback error.
            CHECK_THROWS_AS(interactive.process("$( 1 + 1 ) ~ nope"), std::runtime_error);
        } });
}

TEST_CASE("math-syntax: polynomial identity with islands and the ? prefix (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import topoly");
        interactive.process(".import math-syntax");
        interactive.process("x ~ symvar");

        collector.clear();
        interactive.process("? $( (1+x)*(1-x) ) ≡ $( 1 - x^2 )");
        CHECK_FALSE(any_output_contains(collector, "⇐"));
        collector.clear();
        interactive.process(R"js(%(let [l (zelph/fact (zelph/fact (zelph/number "1") "+" "x") "*" (zelph/fact (zelph/number "1") "-" "x")) r (zelph/fact (zelph/number "1") "-" (zelph/fact "x" "^" (zelph/number "2")))] (string "MSI-PROVEN-" (zelph/exists (zelph/fact l "≡" r) "=" (zelph/resolve "proven")))))js");
        CHECK(any_output_contains(collector, "MSI-PROVEN-true")); });
}

TEST_CASE("math-syntax: terms render in island form exactly where it is needed (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import math-syntax");

        // NOTE on the observation vehicle: 'marker' is deliberately not '~'.
        // Declaring (term ~ concept) makes the term an INSTANCE, and the
        // proxy stage of node_to_string then renders it as the concept --
        // the display would say "r1 ~ r1" and reveal nothing about the term.

        SUBCASE("precedence removes parentheses, and the island seals the result")
        {
            collector.clear();
            interactive.process("$( 1 - x*x ) marker r1");
            CHECK(any_output_contains(collector, "$( &1 - x * x ) marker r1"));
        }
        SUBCASE("required parentheses stay -- and no island appears")
        {
            collector.clear();
            interactive.process("$( (1+x)*(1-x) ) marker r2");
            CHECK(any_output_contains(collector, "((&1 + x) * (&1 - x)) marker r2"));
            CHECK_FALSE(any_event_contains(collector, "$("));
        }
        SUBCASE("function application renders in call notation")
        {
            collector.clear();
            interactive.process("$( exp(x) + 1 ) marker r3");
            CHECK(any_output_contains(collector, "$( exp(x) + &1 ) marker r3"));
        }
        SUBCASE("unary minus reads back as its application form")
        {
            // $( -x ) builds (neg of x); neg(x) is what this grammar parses.
            collector.clear();
            interactive.process("$( -x + 1 ) marker r4");
            CHECK(any_output_contains(collector, "$( neg(x) + &1 ) marker r4"));
        } });
}

TEST_CASE("math-syntax: the engine's own output re-enters as the same node (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import math-syntax");

        // The round trip, taken literally: enter a term, read back what the
        // engine printed, feed THAT string in again. Hash-consing turns the
        // check into node identity -- if the rendering were not readable by
        // this module's own parser, the second statement would build a
        // different node, or fail outright.
        collector.clear();
        interactive.process("$( 1 - x*x )");

        // The FIRST Out event is the statement echo; the later ones are the
        // semi-naive check diagnostics that run_arithmetic_modules enables.
        std::string rendered;
        for (const auto& e : collector.events())
        {
            if (e.channel == zelph::io::OutputChannel::Out && !e.text.empty())
            {
                rendered = normalize(e.text);
                break;
            }
        }
        REQUIRE_FALSE(rendered.empty());

        interactive.process(rendered + " marker roundtrip");
        collector.clear();
        interactive.process(R"js(%(let [t (zelph/fact (zelph/number "1") "-" (zelph/fact "x" "*" "x"))] (string "MSD-RT-" (zelph/exists t "marker" "roundtrip"))))js");
        CHECK(any_output_contains(collector, "MSD-RT-true")); });
}

TEST_CASE("math-syntax: the '&' sigil is optional inside islands (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import math-syntax");

        // Rendered terms carry the sigil; hand-written ones usually do not.
        // Both spellings must reach the same node, so a rendering stays
        // readable no matter which numeral prefix the scheme registers.
        interactive.process("$( &6 + 7 ) marker s1");
        interactive.process("$( 6 + &7 ) marker s2");
        interactive.process("(&6 + &7) marker s3");
        collector.clear();
        interactive.process(R"js(%(let [t (zelph/fact (zelph/number "6") "+" (zelph/number "7"))] (string "MSD-AMP-" (and (zelph/exists t "marker" "s1") (zelph/exists t "marker" "s2") (zelph/exists t "marker" "s3")))))js");
        CHECK(any_output_contains(collector, "MSD-AMP-true")); });
}

TEST_CASE("math-syntax: call notation survives the round trip (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import math-syntax");

        collector.clear();
        interactive.process("$( exp(x) + 1 )");
        std::string rendered;
        for (const auto& e : collector.events())
        {
            if (e.channel == zelph::io::OutputChannel::Out && !e.text.empty())
            {
                rendered = normalize(e.text);
                break;
            }
        }
        REQUIRE_FALSE(rendered.empty());

        interactive.process(rendered + " marker approundtrip");
        collector.clear();
        interactive.process(R"js(%(let [t (zelph/fact (zelph/fact "exp" "of" "x") "+" (zelph/number "1"))] (string "MSD-APP-" (zelph/exists t "marker" "approundtrip"))))js");
        CHECK(any_output_contains(collector, "MSD-APP-true")); });
}
