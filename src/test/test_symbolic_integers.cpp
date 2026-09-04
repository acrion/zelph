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
// Symbolic subtraction/negation (symbolic-minus.zph) and Z numerals in
// the symbolic layer (symbolic-integers.zph).
//
// Assertions are STRUCTURAL via zelph/exists probes (the
// test_symbolic.cpp pattern). Everything runs across all three natural
// substrates: both modules must be representation-agnostic.
//
// The first two test cases deliberately do NOT import the integer
// modules: they pin the natural-only behavior of symbolic-minus,
// including the honest partial results that symbolic-integers later
// completes -- the completion is pinned by the mirror cases below.
// ---------------------------------------------------------------------------

namespace
{
    template <typename Interactive>
    void define_z_helpers(Interactive& interactive)
    {
        interactive.process(R"js(%(defn zp [s] (zelph/fact "pos" "zint" (zelph/number s))))js");
        interactive.process(R"js(%(defn zn [s] (zelph/fact "neg" "zint" (zelph/number s))))js");
    }
} // namespace

TEST_CASE("symbolic-minus: subtraction and negation in the simplifier (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import symbolic-minus");
        interactive.process("x ~ symvar");

        SUBCASE("neutral element: (x - &0) simplifies to x")
        {
            interactive.process(":simplify (x - &0)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "-" (zelph/number "0"))] (string "SM-SUB0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            CHECK(any_output_contains(collector, "SM-SUB0-true"));
        }
        SUBCASE("inherited numeric folding: (&5 - &3) simplifies to &2")
        {
            interactive.process(":simplify (&5 - &3)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "5") "-" (zelph/number "3"))] (string "SM-FOLD-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "2")))))js");
            CHECK(any_output_contains(collector, "SM-FOLD-true"));
        }
        SUBCASE("natural partiality stays visible: (&3 - &5) is its own normal form")
        {
            interactive.process(":simplify (&3 - &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "3") "-" (zelph/number "5"))] (string "SM-PART-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            CHECK(any_output_contains(collector, "SM-PART-true"));
        }
        SUBCASE("involution via inverseof: (neg of (neg of x)) simplifies to x")
        {
            interactive.process(":simplify (neg of (neg of x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zelph/fact "neg" "of" "x"))] (string "SM-INV-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            CHECK(any_output_contains(collector, "SM-INV-true"));
        }
        SUBCASE("(neg of &0) simplifies to &0")
        {
            interactive.process(":simplify (neg of &0)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zelph/number "0"))] (string "SM-NEG0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "SM-NEG0-true"));
        }
        SUBCASE("deliberate omission: (x - x) has no local cancellation")
        {
            // Cancellation of equal symbolic terms is the polynomial
            // normal-form layer's job (see the module header for the
            // single-valuedness clash a local X - X rule would cause).
            interactive.process(":simplify (x - x)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "-" "x")] (string "SM-XX-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            CHECK(any_output_contains(collector, "SM-XX-true"));
        } });
}

TEST_CASE("symbolic-minus: differentiation of - and neg (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import diff");
        interactive.process(".import symbolic-minus");
        process_lines(interactive, R"(
x ~ symvar
c ~ symconst
)");

        SUBCASE("sum rule mirror: d(x - c)/dx = &1")
        {
            interactive.process("(x - c) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "SMD-XC-" (zelph/exists (zelph/fact (zelph/fact "x" "-" "c") "diffby" "x") "=" (zelph/number "1"))))js");
            CHECK(any_output_contains(collector, "SMD-XC-true"));
        }
        SUBCASE("honest partial result without Z: d(c - x)/dx = (&0 - &1), unreduced")
        {
            // Over the naturals alone, 0 - 1 has no value; the exposed
            // derivative is the honest unreduced term. The mirror case in
            // the symbolic-integers tests pins the completion to
            // (neg zint &1).
            interactive.process("(c - x) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "SMD-CX-" (zelph/exists (zelph/fact (zelph/fact "c" "-" "x") "diffby" "x") "=" (zelph/fact (zelph/number "0") "-" (zelph/number "1")))))js");
            CHECK(any_output_contains(collector, "SMD-CX-true"));
        }
        SUBCASE("constant composite: d(c - c)/dx = &0, single-valued through both paths")
        {
            interactive.process("(c - c) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "SMD-CC-" (zelph/exists (zelph/fact (zelph/fact "c" "-" "c") "diffby" "x") "=" (zelph/number "0"))))js");
            interactive.process(R"js(%(string "SMD-CC-NOT-" (zelph/exists (zelph/fact (zelph/fact "c" "-" "c") "diffby" "x") "=" (zelph/fact (zelph/number "0") "-" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "SMD-CC-true"));
            CHECK(any_output_contains(collector, "SMD-CC-NOT-false"));
        } });
}

TEST_CASE("symbolic-integers: Z numerals in the simplifier (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import integer-arithmetic");
        interactive.process(".import symbolic-core");
        interactive.process(".import symbolic-minus");
        interactive.process(".import symbolic-integers");
        define_z_helpers(interactive);
        interactive.process("x ~ symvar");

        SUBCASE("zint leaves are their own normal forms")
        {
            interactive.process(":simplify (pos zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zp "5")] (string "SI-LEAF-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            CHECK(any_output_contains(collector, "SI-LEAF-true"));
        }
        SUBCASE("neutral/absorbing Z elements against symbolic operands")
        {
            interactive.process(":simplify (x + (pos zint &0))");
            interactive.process(":simplify ((pos zint &1) * x)");
            interactive.process(":simplify (x * (pos zint &0))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "+" (zp "0"))] (string "SI-PLUS0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "1") "*" "x")] (string "SI-MUL1-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "x" "*" (zp "0"))] (string "SI-MUL0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zp "0")))))js");
            CHECK(any_output_contains(collector, "SI-PLUS0-true"));
            CHECK(any_output_contains(collector, "SI-MUL1-true"));
            CHECK(any_output_contains(collector, "SI-MUL0-true"));
        }
        SUBCASE("constant folding over Z via facade + SN bridge, incl. fresh mid-simplification fact")
        {
            interactive.process(":simplify ((pos zint &2) + (neg zint &5))");
            interactive.process(":simplify ((pos zint &2) - (pos zint &5))");
            interactive.process(":simplify (((pos zint &2) + (neg zint &5)) * (neg zint &4))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "2") "+" (zn "5"))] (string "SI-FADD-" (zelph/exists (zelph/fact t "simplify" t) "=" (zn "3")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "2") "-" (zp "5"))] (string "SI-FSUB-" (zelph/exists (zelph/fact t "simplify" t) "=" (zn "3")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact (zp "2") "+" (zn "5")) "*" (zn "4"))] (string "SI-FCASC-" (zelph/exists (zelph/fact t "simplify" t) "=" (zp "12")))))js");
            CHECK(any_output_contains(collector, "SI-FADD-true"));
            CHECK(any_output_contains(collector, "SI-FSUB-true"));
            CHECK(any_output_contains(collector, "SI-FCASC-true"));
        }
        SUBCASE("negation of Z numerals, canonical zero preserved")
        {
            interactive.process(":simplify (neg of (pos zint &3))");
            interactive.process(":simplify (neg of (neg zint &3))");
            interactive.process(":simplify (neg of (pos zint &0))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zp "3"))] (string "SI-NEGP-" (zelph/exists (zelph/fact t "simplify" t) "=" (zn "3")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zn "3"))] (string "SI-NEGN-" (zelph/exists (zelph/fact t "simplify" t) "=" (zp "3")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zp "0"))] (string "SI-NEG0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zp "0"))] (string "SI-NEG0-NOT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zn "0")))))js");
            CHECK(any_output_contains(collector, "SI-NEGP-true"));
            CHECK(any_output_contains(collector, "SI-NEGN-true"));
            CHECK(any_output_contains(collector, "SI-NEG0-true"));
            CHECK(any_output_contains(collector, "SI-NEG0-NOT-false"));
        }
        SUBCASE("N -> Z promotion completes natural partiality")
        {
            interactive.process(":simplify (&3 - &5)");
            interactive.process(":simplify (&5 - &3)");
            interactive.process(":simplify (neg of &4)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "3") "-" (zelph/number "5"))] (string "SI-PROM-" (zelph/exists (zelph/fact t "simplify" t) "=" (zn "2")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "5") "-" (zelph/number "3"))] (string "SI-NATKEEP-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "2")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zelph/number "4"))] (string "SI-PROMNEG-" (zelph/exists (zelph/fact t "simplify" t) "=" (zn "4")))))js");
            CHECK(any_output_contains(collector, "SI-PROM-true"));
            CHECK(any_output_contains(collector, "SI-NATKEEP-true"));
            CHECK(any_output_contains(collector, "SI-PROMNEG-true"));
        }
        SUBCASE("single-valuedness at the facade/SR overlap: ((+5) - (+5)) = (pos zint &0) only")
        {
            interactive.process(":simplify ((pos zint &5) - (pos zint &5))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "5") "-" (zp "5"))] (string "SI-ZZ-" (zelph/exists (zelph/fact t "simplify" t) "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "5") "-" (zp "5"))] (string "SI-ZZ-NOT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "SI-ZZ-true"));
            CHECK(any_output_contains(collector, "SI-ZZ-NOT-false"));
        } });
}

TEST_CASE("symbolic-integers: differentiation over Z coefficients (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import integer-arithmetic");
        interactive.process(".import symbolic-core");
        interactive.process(".import diff");
        interactive.process(".import symbolic-minus");
        interactive.process(".import symbolic-integers");
        define_z_helpers(interactive);
        process_lines(interactive, R"(
x ~ symvar
c ~ symconst
)");

        SUBCASE("Z coefficients pass through diff's natural &0/&1 seeds: d((+2) * x)/dx = (+2)")
        {
            // The raw product-rule derivative mixes worlds:
            // ((&0 * x) + ((pos zint &2) * &1)); the NATURAL identity
            // rules reduce it to the zint coefficient.
            interactive.process("((pos zint &2) * x) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "SID-POS-" (zelph/exists (zelph/fact (zelph/fact (zp "2") "*" "x") "diffby" "x") "=" (zp "2"))))js");
            CHECK(any_output_contains(collector, "SID-POS-true"));
        }
        SUBCASE("negative coefficient: d((-3) * x)/dx = (-3)")
        {
            interactive.process("((neg zint &3) * x) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "SID-NEG-" (zelph/exists (zelph/fact (zelph/fact (zn "3") "*" "x") "diffby" "x") "=" (zn "3"))))js");
            CHECK(any_output_contains(collector, "SID-NEG-true"));
        }
        SUBCASE("the gap closed: d(c - x)/dx = (neg zint &1)")
        {
            // Mirror of the honest natural result pinned in the
            // symbolic-minus tests: with Z loaded, the raw (&0 - &1)
            // promotes to the signed numeral.
            interactive.process("(c - x) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "SID-CX-" (zelph/exists (zelph/fact (zelph/fact "c" "-" "x") "diffby" "x") "=" (zn "1"))))js");
            CHECK(any_output_contains(collector, "SID-CX-true"));
        } });
}