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
