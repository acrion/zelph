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
        }
        SUBCASE("a zint compiles only with a numeral magnitude, and the signed zero is zero")
        {
            // Each zint was taken as its own constant polynomial, regardless
            // of its content: the node representing (neg zint &0) differed
            // from the node representing the zero polynomial, so 0 = 0 was
            // disproven, and (pos zint x) was compiled into a constant.
            interactive.process("(neg zint &0) ≡ &0");
            interactive.process(":topoly (pos zint x)");
            interactive.process("(pos zint x) ≡ &0");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("((neg zint &0) ≡ &0) = V");
            interactive.process("(:topoly (pos zint x)) = P");
            interactive.process("((pos zint x) ≡ &0) = V");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "((neg zint &0) ≡ &0) = proven"));
        }
        SUBCASE("a cons list that is not a numeral compiles to nothing")
        {
            // Each cons list, including <x c>, was treated as the constant
            // (pos zint L), thereby enabling an identity over it to yield a
            // verdict. A list qualifies as a constant solely when each
            // element is a digit and the sequence terminates with nil.
            interactive.process(":topoly <x c>");
            interactive.process(":topoly (x + <x>)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("(:topoly <x c>) = P");
            interactive.process("(:topoly (x + <x>)) = P");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a numeral with a leading zero compiles to its canonical value")
        {
            // The leaf compiled to (pos zint L) with L as written, thus
            // <01> and &1 became different constants and the identity
            // between them was disproven. The internal zero in <010>
            // carries value and stays.
            interactive.process(R"js(%(tp (pl "1" "0")))js");
            interactive.process(R"js(%(tp (pl "0" "0")))js");
            interactive.process(R"js(%(tp (pl "0" "1" "0")))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "TP-LZ1-" (zelph/exists (tp (pl "1" "0")) "=" (zp "1"))))js");
            interactive.process(R"js(%(string "TP-LZ0-" (zelph/exists (tp (pl "0" "0")) "=" (zp "0"))))js");
            interactive.process(R"js(%(string "TP-LZIN-" (zelph/exists (tp (pl "0" "1" "0")) "=" (zelph/fact "pos" "zint" (pl "0" "1")))))js");
            CHECK(any_output_contains(collector, "TP-LZ1-true"));
            CHECK(any_output_contains(collector, "TP-LZ0-true"));
            CHECK(any_output_contains(collector, "TP-LZIN-true"));

            collector.clear();
            interactive.process(R"js(%(string "TP-LZRAW-" (zelph/exists (tp (pl "1" "0")) "=" (zelph/fact "pos" "zint" (pl "1" "0")))))js");
            CHECK(any_output_contains(collector, "TP-LZRAW-false"));
        }
        SUBCASE("a zint magnitude with a leading zero is canonical, a zero one is the zero polynomial")
        {
            interactive.process(R"js(%(tp (zelph/fact "pos" "zint" (pl "1" "0"))))js");
            interactive.process(R"js(%(tp (zelph/fact "neg" "zint" (pl "1" "0"))))js");
            interactive.process(R"js(%(tp (zelph/fact "neg" "zint" (pl "0" "0"))))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "TP-ZLZP-" (zelph/exists (tp (zelph/fact "pos" "zint" (pl "1" "0"))) "=" (zp "1"))))js");
            interactive.process(R"js(%(string "TP-ZLZN-" (zelph/exists (tp (zelph/fact "neg" "zint" (pl "1" "0"))) "=" (zn "1"))))js");
            interactive.process(R"js(%(string "TP-ZLZ0-" (zelph/exists (tp (zelph/fact "neg" "zint" (pl "0" "0"))) "=" (zp "0"))))js");
            CHECK(any_output_contains(collector, "TP-ZLZP-true"));
            CHECK(any_output_contains(collector, "TP-ZLZN-true"));
            CHECK(any_output_contains(collector, "TP-ZLZ0-true"));
        }
        SUBCASE("canonicalization stays behind the numeral test")
        {
            // The canonical form strips trailing zero cells from ANY list,
            // transforming <x 0> into <x>, thus it must only see lists
            // that represent numerals.
            interactive.process(":topoly <x 0>");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("(:topoly <x 0>) = P");
            CHECK(collect_answers(collector).empty());
            interactive.process(R"js(%(string "TP-NOCANON-" (zelph/exists (pl "x" "0") "canon" (pl "x"))))js");
            CHECK(any_output_contains(collector, "TP-NOCANON-false"));
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
        }
        SUBCASE("the exponent of ^ is read by value: (neg zint &0) and nil act as zero")
        {
            // ppow tests its exponent using (N cmp &0). The integer façade
            // compares a signed zero like (neg zint &0) as &0, causing x
            // raised to that power to compile to the constant 1. The canonical
            // (pos zint &0) next to a natural number does not conform to a
            // shape the façade routes, thus yielding nothing. The empty list
            // nil lies beyond the scope of the arithmetic contract, yet cmp
            // reads it as zero since digit recursions end at nil. reference.md
            // and the ppow comment in polynomial.zph state all three
            // scenarios, while polynomial.md covers only the first two; any
            // modification to the façade or to cmp must be mirrored in that
            // documentation alongside this test.
            interactive.process(":topoly (x ^ (neg zint &0))");
            interactive.process(":topoly (x ^ (pos zint &0))");
            interactive.process(":topoly (x ^ <>)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("(:topoly (x ^ (neg zint &0))) = P");
            interactive.process("(:topoly (x ^ (pos zint &0))) = P");
            interactive.process("(:topoly (x ^ <>)) = P");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "(:topoly (x ^ (neg zint &0))) = (pos zint &1)"));
            CHECK(answers_contain(collector, "(:topoly (x ^ nil)) = (pos zint &1)"));
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

        // Non-identity: never proven. That it is disproven rather than
        // silent is confirmed through the three-way test outlined
        // below.
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
        SUBCASE("a declared equation is not consulted: disproven is about polynomials")
        {
            // ≡ determines identity within the free ring: each symvar and
            // symconst functions as an independent indeterminate, and a
            // declared equation like (r * r) = &2 is ignored. Identity modulo
            // such equations would represent ideal membership, but the
            // compiler does not determine this. Thus, the verdict is
            // disproven, even though symbolic-core's knowledge folding
            // simplifies (r * r) to &2 in a session that loads it. Both
            // topoly.md and reference.md specify disproven in this way; a
            // topoly utilizing = facts would answer proven in this instance.
            interactive.process("r ~ symconst");
            interactive.process("(r * r) = &2");
            interactive.process("(r * r) ≡ &2");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [e (zelph/fact (zelph/fact "r" "*" "r") "≡" (zelph/number "2"))] (string "TI-DECL-" (zelph/exists e "=" (zelph/resolve "disproven")) "-NOTPROVEN-" (zelph/exists e "=" (zelph/resolve "proven")))))js");
            CHECK(any_output_contains(collector, "TI-DECL-true-NOTPROVEN-false"));
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
        SUBCASE("a leading zero is proven equal to its value, not disproven")
        {
            interactive.process("<1 0> ≡ &1");
            interactive.process("(<1 0> + x) ≡ (x + &1)");
            interactive.process("(<0 0> * x) ≡ &0");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("(<1 0> ≡ &1) = V");
            interactive.process("((<1 0> + x) ≡ (x + &1)) = V");
            interactive.process("((<0 0> * x) ≡ &0) = V");
            const auto answers = collect_answers(collector);
            CHECK(answers.size() == 3);
            for (const auto& a : answers)
            {
                CAPTURE(a);
                CHECK(a.find("= proven") != std::string::npos);
            }
        }
        SUBCASE("a side that is a list, not a numeral, yields neither verdict")
        {
            interactive.process("<x> ≡ x");
            interactive.process("<x y> ≡ &0");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("(<x> ≡ x) = V");
            interactive.process("(<x y> ≡ &0) = V");
            CHECK(collect_answers(collector).empty());
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
