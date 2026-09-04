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
// Term-to-polynomial compiler: topoly.zph.
//
// Assertions are STRUCTURAL via zelph/exists probes. Terms are built
// directly as ordinary facts over the shared vocabulary; polynomials
// via the zp/zn/pl/pv helpers of test_polynomial.cpp; requests via the
// tp helper ((T topoly T), idempotent). symbolic-core is deliberately
// NOT imported: the compiler must work from the vocabulary alone.
//
// The headline property: polynomial identity checking is node identity
// of compiled normal forms.
// ---------------------------------------------------------------------------

namespace
{
    template <typename Interactive>
    void import_topoly(Interactive& interactive)
    {
        interactive.process(".import integer-arithmetic");
        interactive.process(".import polynomial");
        interactive.process(".import topoly");
        interactive.process(R"js(%(defn zp [s] (zelph/fact "pos" "zint" (zelph/number s))))js");
        interactive.process(R"js(%(defn zn [s] (zelph/fact "neg" "zint" (zelph/number s))))js");
        interactive.process(R"js(%(defn pl [& xs] (var acc (zelph/resolve "nil")) (each x (reverse xs) (set acc (zelph/fact x "cons" acc))) acc))js");
        interactive.process(R"js(%(defn pv [v l] (zelph/fact v "poly" l)))js");
        interactive.process(R"js(%(defn tp [t] (zelph/fact t "topoly" t)))js");
    }
} // namespace

TEST_CASE("topoly: leaves and numeral promotion (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_topoly(interactive);
        interactive.process("x ~ symvar");
        interactive.process("c ~ symconst");

        SUBCASE("zint numerals are their own constant polynomials")
        {
            interactive.process(R"js(%(tp (zp "5")))js");
            interactive.process(R"js(%(tp (zn "3")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "TP-ZP-" (zelph/exists (tp (zp "5")) "=" (zp "5"))))js");
            interactive.process(R"js(%(string "TP-ZN-" (zelph/exists (tp (zn "3")) "=" (zn "3"))))js");
            CHECK(any_output_contains(collector, "TP-ZP-true"));
            CHECK(any_output_contains(collector, "TP-ZN-true"));
        }
        SUBCASE("natural numerals promote to (pos zint N), incl. zero")
        {
            interactive.process(R"js(%(tp (zelph/number "7")))js");
            interactive.process(R"js(%(tp (zelph/number "0")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "TP-NAT-" (zelph/exists (tp (zelph/number "7")) "=" (zp "7"))))js");
            interactive.process(R"js(%(string "TP-NAT0-" (zelph/exists (tp (zelph/number "0")) "=" (zp "0"))))js");
            CHECK(any_output_contains(collector, "TP-NAT-true"));
            CHECK(any_output_contains(collector, "TP-NAT0-true"));
        }
        SUBCASE("a symvar compiles to its polynomial; undeclared atoms stay silent")
        {
            interactive.process(R"js(%(tp "x"))js");
            interactive.process(R"js(%(tp "u"))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "0") (zp "1")))] (string "TP-VAR-" (zelph/exists (tp "x") "=" r))))js");
            interactive.process(R"js(%(let [r (pv "u" (pl (zp "0") (zp "1")))] (string "TP-UND-" (zelph/exists (tp "u") "=" r))))js");
            CHECK(any_output_contains(collector, "TP-VAR-true"));
            CHECK(any_output_contains(collector, "TP-UND-false"));
        }
        SUBCASE("a symconst compiles as an indeterminate")
        {
            interactive.process(R"js(%(tp "c"))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "c" (pl (zp "0") (zp "1")))] (string "TP-CONST-" (zelph/exists (tp "c") "=" r))))js");
            CHECK(any_output_contains(collector, "TP-CONST-true"));
        } });
}

TEST_CASE("topoly: operators delegate to the data layer (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_topoly(interactive);
        interactive.process("x ~ symvar");

        SUBCASE("addition, both orientations, identical node: x + 1 and 1 + x")
        {
            interactive.process(R"js(%(tp (zelph/fact "x" "+" (zelph/number "1"))))js");
            interactive.process(R"js(%(tp (zelph/fact (zelph/number "1") "+" "x")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "1") (zp "1")))] (string "TP-XA-" (zelph/exists (tp (zelph/fact "x" "+" (zelph/number "1"))) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "1") (zp "1")))] (string "TP-AX-" (zelph/exists (tp (zelph/fact (zelph/number "1") "+" "x")) "=" r))))js");
            CHECK(any_output_contains(collector, "TP-XA-true"));
            CHECK(any_output_contains(collector, "TP-AX-true"));
        }
        SUBCASE("negation and involution: neg of x, neg of (neg of x)")
        {
            interactive.process(R"js(%(tp (zelph/fact "neg" "of" "x")))js");
            interactive.process(R"js(%(tp (zelph/fact "neg" "of" (zelph/fact "neg" "of" "x"))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "0") (zn "1")))] (string "TP-NEG-" (zelph/exists (tp (zelph/fact "neg" "of" "x")) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "0") (zp "1")))] (string "TP-INV-" (zelph/exists (tp (zelph/fact "neg" "of" (zelph/fact "neg" "of" "x"))) "=" r))))js");
            CHECK(any_output_contains(collector, "TP-NEG-true"));
            CHECK(any_output_contains(collector, "TP-INV-true"));
        }
        SUBCASE("subtraction: completes natural partiality, cancels x - x")
        {
            interactive.process(R"js(%(tp (zelph/fact (zelph/number "3") "-" (zelph/number "5"))))js");
            interactive.process(R"js(%(tp (zelph/fact "x" "-" "x")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "TP-PART-" (zelph/exists (tp (zelph/fact (zelph/number "3") "-" (zelph/number "5"))) "=" (zn "2"))))js");
            interactive.process(R"js(%(string "TP-XX-" (zelph/exists (tp (zelph/fact "x" "-" "x")) "=" (zp "0"))))js");
            CHECK(any_output_contains(collector, "TP-PART-true"));
            CHECK(any_output_contains(collector, "TP-XX-true"));
        }
        SUBCASE("numeral folding inside a product: ((2 + 3) * x) = 5x")
        {
            interactive.process(R"js(%(tp (zelph/fact (zelph/fact (zelph/number "2") "+" (zelph/number "3")) "*" "x")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (tp (zelph/fact (zelph/fact (zelph/number "2") "+" (zelph/number "3")) "*" "x")) r (pv "x" (pl (zp "0") (zp "5")))] (string "TP-FOLD-" (zelph/exists t "=" r))))js");
            CHECK(any_output_contains(collector, "TP-FOLD-true"));
        } });
}

TEST_CASE("topoly: polynomial identities are node identity (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_topoly(interactive);
        interactive.process("x ~ symvar");
        interactive.process("y ~ symvar");
        interactive.process("x pouter y");

        SUBCASE("(1 + x)(1 - x) and 1 - x*x compile to the SAME node")
        {
            interactive.process(R"js(%(tp (zelph/fact (zelph/fact (zelph/number "1") "+" "x") "*" (zelph/fact (zelph/number "1") "-" "x"))))js");
            interactive.process(R"js(%(tp (zelph/fact (zelph/number "1") "-" (zelph/fact "x" "*" "x"))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "1") (zp "0") (zn "1")))] (string "TP-ID1-" (zelph/exists (tp (zelph/fact (zelph/fact (zelph/number "1") "+" "x") "*" (zelph/fact (zelph/number "1") "-" "x"))) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "1") (zp "0") (zn "1")))] (string "TP-ID2-" (zelph/exists (tp (zelph/fact (zelph/number "1") "-" (zelph/fact "x" "*" "x"))) "=" r))))js");
            CHECK(any_output_contains(collector, "TP-ID1-true"));
            CHECK(any_output_contains(collector, "TP-ID2-true"));
        }
        SUBCASE("commutativity through the compiler: x*y and y*x")
        {
            interactive.process(R"js(%(tp (zelph/fact "x" "*" "y")))js");
            interactive.process(R"js(%(tp (zelph/fact "y" "*" "x")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "0") (pv "y" (pl (zp "0") (zp "1")))))] (string "TP-XY-" (zelph/exists (tp (zelph/fact "x" "*" "y")) "=" r))))js");
            interactive.process(R"js(%(let [r (pv "x" (pl (zp "0") (pv "y" (pl (zp "0") (zp "1")))))] (string "TP-YX-" (zelph/exists (tp (zelph/fact "y" "*" "x")) "=" r))))js");
            CHECK(any_output_contains(collector, "TP-XY-true"));
            CHECK(any_output_contains(collector, "TP-YX-true"));
        }
        SUBCASE("nested addition across variables: x + (y + 1)")
        {
            interactive.process(R"js(%(tp (zelph/fact "x" "+" (zelph/fact "y" "+" (zelph/number "1")))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (tp (zelph/fact "x" "+" (zelph/fact "y" "+" (zelph/number "1")))) r (pv "x" (pl (pv "y" (pl (zp "1") (zp "1"))) (zp "1")))] (string "TP-NEST-" (zelph/exists t "=" r))))js");
            CHECK(any_output_contains(collector, "TP-NEST-true"));
        } });
}

TEST_CASE("topoly: polynomial identity ≡ via node identity (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import topoly");
        interactive.process("x ~ symvar");

        // (1+x)*(1-x) and 1 - x*x compile to the same canonical node.
        interactive.process("((&1 + x) * (&1 - x)) ≡ (&1 - (x * x))");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(let [l (zelph/fact (zelph/fact (zelph/number "1") "+" "x") "*" (zelph/fact (zelph/number "1") "-" "x")) r (zelph/fact (zelph/number "1") "-" (zelph/fact "x" "*" "x"))] (string "TI-EQ-" (zelph/exists (zelph/fact l "≡" r) "=" (zelph/resolve "proven")))))js");
        CHECK(any_output_contains(collector, "TI-EQ-true"));

        // Non-identity: silence, never a wrong verdict.
        interactive.process("((&1 + x) * (&1 - x)) ≡ (&1 + (x * x))");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(let [l (zelph/fact (zelph/fact (zelph/number "1") "+" "x") "*" (zelph/fact (zelph/number "1") "-" "x")) r (zelph/fact (zelph/number "1") "+" (zelph/fact "x" "*" "x"))] (string "TI-NEQ-" (zelph/exists (zelph/fact l "≡" r) "=" (zelph/resolve "proven")))))js");
        CHECK(any_output_contains(collector, "TI-NEQ-false")); });
}

TEST_CASE("topoly: identity verdicts are three-way (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_topoly(interactive);
        interactive.process("x ~ symvar");
        interactive.process("y ~ symvar");
        interactive.process("x pouter y");

        SUBCASE("differing normal forms are disproven, not silent")
        {
            // (x+1)^2 and x^2+1 both compile, to <1,2,1> and <1,0,1>.
            // Non-identity used to be indistinguishable from "a side did
            // not compile"; != on the two results decides it positively.
            interactive.process("((x + &1) ^ &2) ≡ ((x ^ &2) + &1)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [l (zelph/fact (zelph/fact "x" "+" (zelph/number "1")) "^" (zelph/number "2")) r (zelph/fact (zelph/fact "x" "^" (zelph/number "2")) "+" (zelph/number "1")) e (zelph/fact l "≡" r)] (string "TI-DIS-" (zelph/exists e "=" (zelph/resolve "disproven")) "-NOTPROVEN-" (zelph/exists e "=" (zelph/resolve "proven")))))js");
            CHECK(any_output_contains(collector, "TI-DIS-true-NOTPROVEN-false"));
        }
        SUBCASE("an identity stays proven and is not also disproven")
        {
            interactive.process("((x + &1) * (x - &1)) ≡ ((x ^ &2) - &1)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [l (zelph/fact (zelph/fact "x" "+" (zelph/number "1")) "*" (zelph/fact "x" "-" (zelph/number "1"))) r (zelph/fact (zelph/fact "x" "^" (zelph/number "2")) "-" (zelph/number "1")) e (zelph/fact l "≡" r)] (string "TI-PRO-" (zelph/exists e "=" (zelph/resolve "proven")) "-NOTDIS-" (zelph/exists e "=" (zelph/resolve "disproven")))))js");
            CHECK(any_output_contains(collector, "TI-PRO-true-NOTDIS-false"));
        }
        SUBCASE("an uncompilable side yields NEITHER verdict")
        {
            // 'u' has no sort, so it has no normal form. "I could not
            // compile this" must not be reported as "these differ" --
            // silence is the only honest answer, and the disproof rule
            // must not weaken it.
            interactive.process("((x + u) ^ &2) ≡ ((x ^ &2) + u)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [l (zelph/fact (zelph/fact "x" "+" "u") "^" (zelph/number "2")) r (zelph/fact (zelph/fact "x" "^" (zelph/number "2")) "+" "u") e (zelph/fact l "≡" r)] (string "TI-SIL-" (zelph/exists e "=" (zelph/resolve "disproven")) "-" (zelph/exists e "=" (zelph/resolve "proven")))))js");
            CHECK(any_output_contains(collector, "TI-SIL-false-false"));
        }
        SUBCASE("a missing variable order yields neither verdict")
        {
            // Cross-variable multiplication needs a declared pouter pair.
            // Without one the compilation stalls -- again silence, not a
            // disproof.
            interactive.process("z ~ symvar");
            interactive.process("((x * z) + &1) ≡ ((z * x) + &2)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [l (zelph/fact (zelph/fact "x" "*" "z") "+" (zelph/number "1")) r (zelph/fact (zelph/fact "z" "*" "x") "+" (zelph/number "2")) e (zelph/fact l "≡" r)] (string "TI-ORD-" (zelph/exists e "=" (zelph/resolve "disproven")) "-" (zelph/exists e "=" (zelph/resolve "proven")))))js");
            CHECK(any_output_contains(collector, "TI-ORD-false-false"));
        } });
}
