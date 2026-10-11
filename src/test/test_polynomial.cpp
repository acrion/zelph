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

#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Polynomial normal forms: polynomial.zph (addition, negation,
// subtraction, canonicalization).
//
// Assertions are STRUCTURAL via zelph/exists probes. Polynomials are
// built by Janet helpers: zp/zn (zint constants), pl (cons list from
// V^0-first elements), pv (variable-tagged polynomial). Everything runs
// across all three natural substrates.
//
// The headline property pinned throughout: canonical forms are unique
// hash-consed NODES, so algebraic equalities (commutativity, collapse,
// cancellation) manifest as node identity in the probes.
// ---------------------------------------------------------------------------

namespace
{
    template <typename Interactive>
    void import_polynomial(Interactive& interactive)
    {
        interactive.process(".import integer-arithmetic");
        interactive.process(".import polynomial");
        interactive.process(R"js(%(defn zp [s] (zelph/fact "pos" "zint" (zelph/number s))))js");
        interactive.process(R"js(%(defn zn [s] (zelph/fact "neg" "zint" (zelph/number s))))js");
        interactive.process(R"js(%(defn pl [& xs] (var acc (zelph/resolve "nil")) (each x (reverse xs) (set acc (zelph/fact x "cons" acc))) acc))js");
        interactive.process(R"js(%(defn pv [v l] (zelph/fact v "poly" l)))js");
    }

    // The `! ⇐` lines from the collected output, one for each
    // announced contradiction, each having undergone
    // normalization.
    std::vector<std::string> contradiction_lines(const zelph::io::OutputCollector& collector)
    {
        std::vector<std::string> lines;
        for (const auto& e : collector.events())
        {
            std::istringstream in(e.text);
            for (std::string line; std::getline(in, line);)
            {
                line = normalize(line);
                if (line.starts_with("! ⇐")) lines.push_back(line);
            }
        }
        return lines;
    }
} // namespace

TEST_CASE("polynomial: constant and same-variable addition (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_polynomial(interactive);

        SUBCASE("constants delegate to the Z facade: (+2) padd (-5) = (-3)")
        {
            interactive.process(R"js(%(zelph/fact (zp "2") "padd" (zn "5")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "P-ZZ-" (zelph/exists (zelph/fact (zp "2") "padd" (zn "5")) "=" (zn "3"))))js");
            CHECK(any_output_contains(collector, "P-ZZ-true"));
        }
        SUBCASE("(1 + 2x) padd (3 + 4x) = (4 + 6x)")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "1") (zp "2"))) "padd" (pv "x" (pl (zp "3") (zp "4")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "1") (zp "2"))) "padd" (pv "x" (pl (zp "3") (zp "4"))))] (string "P-XX-" (zelph/exists t "=" (pv "x" (pl (zp "4") (zp "6")))))))js");
            CHECK(any_output_contains(collector, "P-XX-true"));
        }
        SUBCASE("unequal degrees pass through, inner zero preserved: x^2 padd x")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "0") (zp "0") (zp "1"))) "padd" (pv "x" (pl (zp "0") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "0") (zp "0") (zp "1"))) "padd" (pv "x" (pl (zp "0") (zp "1"))))] (string "P-DEG-" (zelph/exists t "=" (pv "x" (pl (zp "0") (zp "1") (zp "1")))))))js");
            CHECK(any_output_contains(collector, "P-DEG-true"));
        }
        SUBCASE("total cancellation: (1 + x) padd (-1 - x) = the zero polynomial node")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "padd" (pv "x" (pl (zn "1") (zn "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "padd" (pv "x" (pl (zn "1") (zn "1"))))] (string "P-CANCEL-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "padd" (pv "x" (pl (zn "1") (zn "1"))))] (string "P-CANCEL-NOT-" (zelph/exists t "=" (pv "x" (pl (zp "0")))))))js");
            CHECK(any_output_contains(collector, "P-CANCEL-true"));
            CHECK(any_output_contains(collector, "P-CANCEL-NOT-false"));
        }
        SUBCASE("leading-coefficient cancellation collapses: (2 + x) padd (3 - x) = (+5)")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "2") (zp "1"))) "padd" (pv "x" (pl (zp "3") (zn "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "2") (zp "1"))) "padd" (pv "x" (pl (zp "3") (zn "1"))))] (string "P-COLLAPSE-" (zelph/exists t "=" (zp "5")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "2") (zp "1"))) "padd" (pv "x" (pl (zp "3") (zn "1"))))] (string "P-COLLAPSE-NOT-" (zelph/exists t "=" (pv "x" (pl (zp "5")))))))js");
            CHECK(any_output_contains(collector, "P-COLLAPSE-true"));
            CHECK(any_output_contains(collector, "P-COLLAPSE-NOT-false"));
        } });
}

TEST_CASE("polynomial: cross-variable and constant-composite addition (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_polynomial(interactive);
        interactive.process("x pouter y");
        interactive.process("y pouter z");

        SUBCASE("x padd y in both orders yields the IDENTICAL node")
        {
            // Commutativity at the normal-form level is node identity:
            // both orders must connect to (x poly (y cons (1 cons nil)))
            // -- and the head addition 0 + y hash-conses back to y itself.
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "0") (zp "1"))) "padd" (pv "y" (pl (zp "0") (zp "1")))))js");
            interactive.process(R"js(%(zelph/fact (pv "y" (pl (zp "0") (zp "1"))) "padd" (pv "x" (pl (zp "0") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (pv "y" (pl (zp "0") (zp "1"))) (zp "1")))] (string "P-XY-" (zelph/exists (zelph/fact (pv "x" (pl (zp "0") (zp "1"))) "padd" (pv "y" (pl (zp "0") (zp "1")))) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (pv "y" (pl (zp "0") (zp "1"))) (zp "1")))] (string "P-YX-" (zelph/exists (zelph/fact (pv "y" (pl (zp "0") (zp "1"))) "padd" (pv "x" (pl (zp "0") (zp "1")))) "=" r))))js");
            CHECK(any_output_contains(collector, "P-XY-true"));
            CHECK(any_output_contains(collector, "P-YX-true"));
        }
        SUBCASE("constant into composite, both orders: 5 padd (1 + x) = (6 + x)")
        {
            interactive.process(R"js(%(zelph/fact (zp "5") "padd" (pv "x" (pl (zp "1") (zp "1")))))js");
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "padd" (zp "5")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "6") (zp "1")))] (string "P-CX-" (zelph/exists (zelph/fact (zp "5") "padd" (pv "x" (pl (zp "1") (zp "1")))) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "6") (zp "1")))] (string "P-XC-" (zelph/exists (zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "padd" (zp "5")) "=" r))))js");
            CHECK(any_output_contains(collector, "P-CX-true"));
            CHECK(any_output_contains(collector, "P-XC-true"));
        }
        SUBCASE("derived pouter via transitivity: x padd z nests z under x")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "0") (zp "1"))) "padd" (pv "z" (pl (zp "0") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (pv "z" (pl (zp "0") (zp "1"))) (zp "1")))] (string "P-XZ-" (zelph/exists (zelph/fact (pv "x" (pl (zp "0") (zp "1"))) "padd" (pv "z" (pl (zp "0") (zp "1")))) "=" r))))js");
            CHECK(any_output_contains(collector, "P-XZ-true"));
        }
        SUBCASE("nested coefficients cascade: (y + x) padd (1 + x) = ((1 + y) + 2x)")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (pv "y" (pl (zp "0") (zp "1"))) (zp "1"))) "padd" (pv "x" (pl (zp "1") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (pv "y" (pl (zp "0") (zp "1"))) (zp "1"))) "padd" (pv "x" (pl (zp "1") (zp "1")))) r (pv "x" (pl (pv "y" (pl (zp "1") (zp "1"))) (zp "2")))] (string "P-NEST-" (zelph/exists t "=" r))))js");
            CHECK(any_output_contains(collector, "P-NEST-true"));
        } });
}

// ---------------------------------------------------------------------------
// The variable order must adhere to strict ordering: declaring two
// variables outer to each other results in their product having two
// distinct normal forms. The file `polynomial.zph` reports such a pair
// as a contradiction. The rule includes two conditions and a guard,
// ensuring the report is recorded in the graph and made only once,
// naming the two pouter facts instead of the self-loops that
// transitivity derives from them. Before this rule, nothing reported a
// cycle.
// ---------------------------------------------------------------------------

TEST_CASE("polynomial: a pouter cycle is reported once, naming both declarations")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        import_polynomial(interactive);
        interactive.process("y pouter x");
        collector.clear();
        interactive.process("x pouter y");

        const std::vector<std::string> reported = contradiction_lines(collector);
        REQUIRE(reported.size() == 1);
        CHECK(reported.front().find("(y pouter x)") != std::string::npos);
        CHECK(reported.front().find("(x pouter y)") != std::string::npos); });
}

// Why the rule features two conditions: the single-condition form
// (V pouter V) => ! leaves no record, thus it is re-announced
// following each subsequent input line, whose run re-derives
// everything.
TEST_CASE("polynomial: a reported pouter cycle is not announced again on later input")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        import_polynomial(interactive);
        collector.clear();
        interactive.process("y pouter x");
        interactive.process("x pouter y");
        REQUIRE_FALSE(contradiction_lines(collector).empty());

        collector.clear();
        interactive.process("a q b");
        interactive.process("c q d");
        interactive.run(true, false, false);
        CHECK(contradiction_lines(collector).empty()); });
}

// x pouter y, y pouter z, z pouter x: transitivity derives the reverse
// of each declared pair, thus each of the three pairs is reported, and
// each appears exactly once.
TEST_CASE("polynomial: a longer pouter cycle is reported through the pairs transitivity closes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        import_polynomial(interactive);
        interactive.process("x pouter y");
        interactive.process("y pouter z");
        collector.clear();
        interactive.process("z pouter x");

        const std::vector<std::string> reported = contradiction_lines(collector);
        CHECK(reported.size() == 3);
        for (const auto& [v, w] : {std::pair{"x", "y"}, std::pair{"y", "z"}, std::pair{"x", "z"}})
        {
            const std::string forward  = std::string("(") + v + " pouter " + w + ")";
            const std::string backward = std::string("(") + w + " pouter " + v + ")";
            int               naming   = 0;
            for (const auto& line : reported)
                if (line.find(forward) != std::string::npos && line.find(backward) != std::string::npos) ++naming;
            INFO(forward, " / ", backward);
            CHECK(naming == 1);
        }

        collector.clear();
        interactive.process("a q b");
        CHECK(contradiction_lines(collector).empty()); });
}

// The closure x pouter z, which transitivity derives, is no cycle. A
// rule that matched any two chained pouter facts would issue a report
// here.
TEST_CASE("polynomial: a strict pouter order reports nothing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        import_polynomial(interactive);
        collector.clear();
        interactive.process("x pouter y");
        interactive.process("y pouter z");
        interactive.run(true, false, false);
        CHECK(contradiction_lines(collector).empty());
        CHECK_FALSE(has_contradiction(collector));

        // Non-vacuity: the closure exists, so a rule had something to match.
        collector.clear();
        interactive.process(R"js(%(string "PO-CLOSURE-" (zelph/exists "x" "pouter" "z")))js");
        CHECK(any_output_contains(collector, "PO-CLOSURE-true")); });
}

// The standard route: math.zph derives the adjacent pouter pairs
// through the ring declaration, polynomial.zph their closure; none of
// these may be reported, and the identity is still proven.
TEST_CASE("polynomial: a declared ring reports no cycle and still proves an identity" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        collector.clear();
        interactive.process("<x y z> ~ polyring");
        interactive.process("? $( (x+y)*(x-y) ) ≡ $( x^2 - y^2 )");
        CHECK(contradiction_lines(collector).empty());
        CHECK_FALSE(has_contradiction(collector));

        collector.clear();
        interactive.process(R"js(%(let [l (zelph/fact (zelph/fact "x" "+" "y") "*" (zelph/fact "x" "-" "y")) r (zelph/fact (zelph/fact "x" "^" (zelph/number "2")) "-" (zelph/fact "y" "^" (zelph/number "2")))] (string "PO-RING-" (zelph/exists (zelph/fact l "≡" r) "=" (zelph/resolve "proven")))))js");
        CHECK(any_output_contains(collector, "PO-RING-true")); });
}

TEST_CASE("polynomial: negation and subtraction (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_polynomial(interactive);

        SUBCASE("pneg of the zero polynomial is itself")
        {
            interactive.process(R"js(%(let [p (zp "0")] (zelph/fact p "pneg" p)))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [p (zp "0")] (string "PN-ZERO-" (zelph/exists (zelph/fact p "pneg" p) "=" (zp "0")))))js");
            interactive.process(R"js(%(let [p (zp "0")] (string "PN-ZERO-NOT-" (zelph/exists (zelph/fact p "pneg" p) "=" (zn "0")))))js");
            CHECK(any_output_contains(collector, "PN-ZERO-true"));
            CHECK(any_output_contains(collector, "PN-ZERO-NOT-false"));
        }
        SUBCASE("pneg elementwise: -(2 - 3x) = (-2 + 3x)")
        {
            interactive.process(R"js(%(let [p (pv "x" (pl (zp "2") (zn "3")))] (zelph/fact p "pneg" p)))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [p (pv "x" (pl (zp "2") (zn "3")))] (string "PN-ELEM-" (zelph/exists (zelph/fact p "pneg" p) "=" (pv "x" (pl (zn "2") (zp "3")))))))js");
            CHECK(any_output_contains(collector, "PN-ELEM-true"));
        }
        SUBCASE("psub cascades pneg + padd + collapse: (3 + x) psub (1 + x) = (+2)")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "3") (zp "1"))) "psub" (pv "x" (pl (zp "1") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "3") (zp "1"))) "psub" (pv "x" (pl (zp "1") (zp "1"))))] (string "PS-COLLAPSE-" (zelph/exists t "=" (zp "2")))))js");
            CHECK(any_output_contains(collector, "PS-COLLAPSE-true"));
        }
        SUBCASE("psub self-cancellation: P psub P = the zero polynomial node")
        {
            interactive.process(R"js(%(let [p (pv "x" (pl (zp "1") (zp "2")))] (zelph/fact p "psub" p)))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [p (pv "x" (pl (zp "1") (zp "2")))] (string "PS-SELF-" (zelph/exists (zelph/fact p "psub" p) "=" (zp "0")))))js");
            CHECK(any_output_contains(collector, "PS-SELF-true"));
        } });
}

TEST_CASE("polynomial: multiplication -- constants, zero factor, scaling (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_polynomial(interactive);

        SUBCASE("constants delegate to the Z facade: sign handling")
        {
            interactive.process(R"js(%(zelph/fact (zp "3") "pmul" (zn "4")))js");
            interactive.process(R"js(%(zelph/fact (zn "3") "pmul" (zn "4")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "pmul" (zn "4"))] (string "PM-PN-" (zelph/exists t "=" (zn "12")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zn "3") "pmul" (zn "4"))] (string "PM-NN-" (zelph/exists t "=" (zp "12")))))js");
            CHECK(any_output_contains(collector, "PM-PN-true"));
            CHECK(any_output_contains(collector, "PM-NN-true"));
        }
        SUBCASE("zero factor absorbs composites in both orientations, never elementwise")
        {
            interactive.process(R"js(%(zelph/fact (zp "0") "pmul" (pv "x" (pl (zp "1") (zp "1")))))js");
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "pmul" (zp "0")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "0") "pmul" (pv "x" (pl (zp "1") (zp "1"))))] (string "PM-ZL-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "pmul" (zp "0"))] (string "PM-ZR-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "0") "pmul" (pv "x" (pl (zp "1") (zp "1"))))] (string "PM-ZL-NOT-" (zelph/exists t "=" (pv "x" (pl (zp "0") (zp "0")))))))js");
            CHECK(any_output_contains(collector, "PM-ZL-true"));
            CHECK(any_output_contains(collector, "PM-ZR-true"));
            CHECK(any_output_contains(collector, "PM-ZL-NOT-false"));
        }
        SUBCASE("nonzero constant scales elementwise: both orientations, identical node")
        {
            interactive.process(R"js(%(zelph/fact (zp "2") "pmul" (pv "x" (pl (zp "1") (zp "2")))))js");
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "1") (zp "2"))) "pmul" (zp "2")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "2") (zp "4")))] (string "PM-SC-" (zelph/exists (zelph/fact (zp "2") "pmul" (pv "x" (pl (zp "1") (zp "2")))) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "2") (zp "4")))] (string "PM-CS-" (zelph/exists (zelph/fact (pv "x" (pl (zp "1") (zp "2"))) "pmul" (zp "2")) "=" r))))js");
            CHECK(any_output_contains(collector, "PM-SC-true"));
            CHECK(any_output_contains(collector, "PM-CS-true"));
        }
        SUBCASE("negative scalar flips signs: (-2) pmul (2 - 3x) = (-4 + 6x)")
        {
            interactive.process(R"js(%(zelph/fact (zn "2") "pmul" (pv "x" (pl (zp "2") (zn "3")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zn "2") "pmul" (pv "x" (pl (zp "2") (zn "3"))))] (string "PM-NSC-" (zelph/exists t "=" (pv "x" (pl (zn "4") (zp "6")))))))js");
            CHECK(any_output_contains(collector, "PM-NSC-true"));
        }
        SUBCASE("inner zeros are preserved: 3 pmul x^2 = 3x^2")
        {
            interactive.process(R"js(%(zelph/fact (zp "3") "pmul" (pv "x" (pl (zp "0") (zp "0") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "pmul" (pv "x" (pl (zp "0") (zp "0") (zp "1"))))] (string "PM-INZ-" (zelph/exists t "=" (pv "x" (pl (zp "0") (zp "0") (zp "3")))))))js");
            CHECK(any_output_contains(collector, "PM-INZ-true"));
        } });
}

TEST_CASE("polynomial: multiplication -- same-main-variable schoolbook (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_polynomial(interactive);

        SUBCASE("shift through a zero head product: x pmul x = x^2")
        {
            interactive.process(R"js(%(let [p (pv "x" (pl (zp "0") (zp "1")))] (zelph/fact p "pmul" p)))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [p (pv "x" (pl (zp "0") (zp "1")))] (string "PM-XX-" (zelph/exists (zelph/fact p "pmul" p) "=" (pv "x" (pl (zp "0") (zp "0") (zp "1")))))))js");
            CHECK(any_output_contains(collector, "PM-XX-true"));
        }
        SUBCASE("(1 + x)^2 = 1 + 2x + x^2")
        {
            interactive.process(R"js(%(let [p (pv "x" (pl (zp "1") (zp "1")))] (zelph/fact p "pmul" p)))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [p (pv "x" (pl (zp "1") (zp "1")))] (string "PM-SQ-" (zelph/exists (zelph/fact p "pmul" p) "=" (pv "x" (pl (zp "1") (zp "2") (zp "1")))))))js");
            CHECK(any_output_contains(collector, "PM-SQ-true"));
        }
        SUBCASE("cancellation creates an inner zero: (1 + x)(1 - x) = 1 - x^2")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "pmul" (pv "x" (pl (zp "1") (zn "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "pmul" (pv "x" (pl (zp "1") (zn "1"))))] (string "PM-DIF-" (zelph/exists t "=" (pv "x" (pl (zp "1") (zp "0") (zn "1")))))))js");
            interactive.process(R"js(%(let [t (zelph/fact (pv "x" (pl (zp "1") (zp "1"))) "pmul" (pv "x" (pl (zp "1") (zn "1"))))] (string "PM-DIF-NOT-" (zelph/exists t "=" (pv "x" (pl (zp "1") (zn "1")))))))js");
            CHECK(any_output_contains(collector, "PM-DIF-true"));
            CHECK(any_output_contains(collector, "PM-DIF-NOT-false"));
        }
        SUBCASE("commutativity is node identity: (2 + x)(3 - x) in both orders")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "2") (zp "1"))) "pmul" (pv "x" (pl (zp "3") (zn "1")))))js");
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "3") (zn "1"))) "pmul" (pv "x" (pl (zp "2") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "6") (zp "1") (zn "1")))] (string "PM-AB-" (zelph/exists (zelph/fact (pv "x" (pl (zp "2") (zp "1"))) "pmul" (pv "x" (pl (zp "3") (zn "1")))) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "6") (zp "1") (zn "1")))] (string "PM-BA-" (zelph/exists (zelph/fact (pv "x" (pl (zp "3") (zn "1"))) "pmul" (pv "x" (pl (zp "2") (zp "1")))) "=" r))))js");
            CHECK(any_output_contains(collector, "PM-AB-true"));
            CHECK(any_output_contains(collector, "PM-BA-true"));
        } });
}

TEST_CASE("polynomial: multiplication -- cross-variable nesting (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_polynomial(interactive);
        interactive.process("x pouter y");

        SUBCASE("x pmul y nests y under x, both orders identical")
        {
            interactive.process(R"js(%(zelph/fact (pv "x" (pl (zp "0") (zp "1"))) "pmul" (pv "y" (pl (zp "0") (zp "1")))))js");
            interactive.process(R"js(%(zelph/fact (pv "y" (pl (zp "0") (zp "1"))) "pmul" (pv "x" (pl (zp "0") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "0") (pv "y" (pl (zp "0") (zp "1")))))] (string "PM-XY-" (zelph/exists (zelph/fact (pv "x" (pl (zp "0") (zp "1"))) "pmul" (pv "y" (pl (zp "0") (zp "1")))) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "0") (pv "y" (pl (zp "0") (zp "1")))))] (string "PM-YX-" (zelph/exists (zelph/fact (pv "y" (pl (zp "0") (zp "1"))) "pmul" (pv "x" (pl (zp "0") (zp "1")))) "=" r))))js");
            CHECK(any_output_contains(collector, "PM-XY-true"));
            CHECK(any_output_contains(collector, "PM-YX-true"));
        }
        SUBCASE("composite scalar: y pmul (1 + x) = y + yx")
        {
            interactive.process(R"js(%(zelph/fact (pv "y" (pl (zp "0") (zp "1"))) "pmul" (pv "x" (pl (zp "1") (zp "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [y (pv "y" (pl (zp "0") (zp "1"))) r (pv "x" (pl y y))] (string "PM-YS-" (zelph/exists (zelph/fact y "pmul" (pv "x" (pl (zp "1") (zp "1")))) "=" r))))js");
            CHECK(any_output_contains(collector, "PM-YS-true"));
        }
        SUBCASE("full cascade: (y + x)(1 + x) = y + (1 + y)x + x^2")
        {
            interactive.process(R"js(%(let [y (pv "y" (pl (zp "0") (zp "1")))] (zelph/fact (pv "x" (pl y (zp "1"))) "pmul" (pv "x" (pl (zp "1") (zp "1"))))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [y (pv "y" (pl (zp "0") (zp "1"))) t (zelph/fact (pv "x" (pl y (zp "1"))) "pmul" (pv "x" (pl (zp "1") (zp "1")))) r (pv "x" (pl y (pv "y" (pl (zp "1") (zp "1"))) (zp "1")))] (string "PM-NEST-" (zelph/exists t "=" r))))js");
            CHECK(any_output_contains(collector, "PM-NEST-true"));
        } });
}
