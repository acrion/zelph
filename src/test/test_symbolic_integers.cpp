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

#include <string>
#include <utility>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Symbolic subtraction/negation (symbolic-minus.zph) and Z numerals in
// the symbolic layer (symbolic-integers.zph).
//
// Assertions follow the test_symbolic.cpp pattern: STRUCTURAL
// zelph/exists probes where a term is checked, and Answer: lines --
// counted by collect_answers, compared by answers_contain -- where the
// number of answers is the key focus. All operations span the three
// natural substrates: both modules must remain
// representation-agnostic.
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
        SUBCASE("a minuend with a leading zero meets (X - &0) once")
        {
            // The rewrite (X - &0) => X kept <01> as written, whereas the
            // natural module's N - 0 result is canonical: two answers, &1
            // and <01>, until the leaf <1 0> simplified to &1.
            collector.clear();
            interactive.process(":simplify (<1 0> - &0)");
            CHECK_FALSE(has_contradiction(collector));
            collector.clear();
            interactive.process("(:simplify (<1 0> - &0)) = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify (<01> - &0)) = &1"));
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
        SUBCASE("(x - x) cancels to &0")
        {
            // Up to 1.0.1, this was an intentional exclusion: when the Z
            // facade is active, a zint difference folded to the zint zero
            // (pos zint &0), and a local X - X rule would have added the
            // natural &0 as a second answer. Given that nonnegative
            // integers each possess a single node, both are the same
            // node &0.
            interactive.process(":simplify (x - x)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "-" "x")] (string "SM-XX-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "SM-XX-true"));
            collector.clear();
            interactive.process("(:simplify (x - x)) = X");
            CHECK(collect_answers(collector).size() == 1);
        }
        SUBCASE("x - (x - a) is a")
        {
            // The step Eq. (5) in the EML paper corresponds to a single
            // transformation once eml is reduced according to its
            // definition: e - (e - ln z) results in ln z.
            interactive.process("c ~ symconst");
            interactive.process(":simplify (x - (x - c))");
            interactive.process(":simplify (&2 - (&2 - &5))");
            collector.clear();
            interactive.process("(:simplify (x - (x - c))) = X");
            interactive.process("(:simplify (&2 - (&2 - &5))) = X");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "(:simplify (x - (x - c))) = c"));
            CHECK(answers_contain(collector, "(:simplify (&2 - (&2 - &5))) = &5"));
        }
        SUBCASE("only equal terms cancel: (x + c) - (c + x) stays")
        {
            // X - X compares nodes. Terms that are equal only up to
            // commutativity are for the polynomial layer to
            // identify.
            interactive.process("c ~ symconst");
            interactive.process(":simplify ((x + c) - (c + x))");
            collector.clear();
            interactive.process("(:simplify ((x + c) - (c + x))) = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify ((x + c) - (c + x))) = ((x + c) - (c + x))"));
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
        SUBCASE("constant composite: d(c - c)/dx = &0, not the raw (&0 - &0)")
        {
            interactive.process("(c - c) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "SMD-CC-" (zelph/exists (zelph/fact (zelph/fact "c" "-" "c") "diffby" "x") "=" (zelph/number "0"))))js");
            interactive.process(R"js(%(string "SMD-CC-NOT-" (zelph/exists (zelph/fact (zelph/fact "c" "-" "c") "diffby" "x") "=" (zelph/fact (zelph/number "0") "-" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "SMD-CC-true"));
            CHECK(any_output_contains(collector, "SMD-CC-NOT-false"));
        }
        SUBCASE("d(neg u)/dx = neg(du/dx)")
        {
            // neg is an application, so diff's generic decompose rule for
            // `of` forwards the request onward, yet diff lacks a derivative
            // definition for it: neg possesses no hasderivative fact, and
            // its assemble rule is symbolic-minus's. Without
            // symbolic-integers, -1 lacks a defined value, thus the
            // derivative of (neg of x) stays (neg of &1).
            interactive.process("(neg of x) diffby x");
            interactive.process("(neg of c) diffby x");
            collector.clear();
            interactive.process("((neg of x) diffby x) = D");
            interactive.process("((neg of c) diffby x) = D");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "((neg of x) diffby x) = (neg of &1)"));
            CHECK(answers_contain(collector, "((neg of c) diffby x) = &0"));
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

        SUBCASE("a negative zint is its own normal form, (pos zint &5) is &5")
        {
            // Each value corresponds to a single node: an explicit
            // (pos zint N) denotes the natural number N and reduces to
            // it when reached at the leaf level.
            interactive.process(":simplify (pos zint &5)");
            interactive.process(":simplify (neg zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zp "5")] (string "SI-LEAF-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "5")))))js");
            interactive.process(R"js(%(let [t (zn "5")] (string "SI-NLEAF-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            CHECK(any_output_contains(collector, "SI-LEAF-true"));
            CHECK(any_output_contains(collector, "SI-NLEAF-true"));
        }
        SUBCASE("a zero typed with a negative sign is the natural zero")
        {
            // The expression (neg zint &0) does not qualify as canonical,
            // nor does any element on the stack produce it, yet it remains
            // acceptable as input. Serving as its own normal form, it met
            // the natural zero in both the neutral and absorbing rules,
            // resulting in two answers for ((neg zint &0) + &0).
            interactive.process(":simplify ((neg zint &0) + &0)");
            interactive.process(":simplify (x + (neg zint &0))");
            interactive.process(":simplify ((neg zint &0) * ((neg zint &3) * x))");
            interactive.process(":simplify (x + (neg zint <0 0>))");
            collector.clear();
            interactive.process("(:simplify ((neg zint &0) + &0)) = X");
            interactive.process("(:simplify (x + (neg zint &0))) = X");
            interactive.process("(:simplify ((neg zint &0) * ((neg zint &3) * x))) = X");
            interactive.process("(:simplify (x + (neg zint <0 0>))) = X");
            CHECK(collect_answers(collector).size() == 4);
            CHECK(answers_contain(collector, "(:simplify ((neg zint &0) + &0)) = &0"));
            CHECK(answers_contain(collector, "(:simplify (x + (neg zint &0))) = x"));
            CHECK(answers_contain(collector, "(:simplify ((neg zint &0) * ((neg zint &3) * x))) = &0"));
            CHECK(answers_contain(collector, "(:simplify (x + (neg zint <00>))) = x"));
        }
        SUBCASE("a zint magnitude with a leading zero simplifies to the canonical integer")
        {
            // The zint counterpart of a numeral leaf featuring a leading
            // zero: the magnitude is read in canonical form, meaning (neg
            // zint <1 0>) and (neg zint &1) represent the same normal form,
            // and their addition folds to (neg zint &2) rather than
            // producing a raw sum of the written magnitudes.
            interactive.process(":simplify (neg zint <1 0>)");
            interactive.process(":simplify (pos zint <1 0>)");
            interactive.process(":simplify ((neg zint <1 0>) + (neg zint &1))");
            collector.clear();
            interactive.process("(:simplify (neg zint <1 0>)) = X");
            interactive.process("(:simplify (pos zint <1 0>)) = X");
            interactive.process("(:simplify ((neg zint <1 0>) + (neg zint &1))) = X");
            CHECK(collect_answers(collector).size() == 3);
            CHECK(answers_contain(collector, "(:simplify (neg zint <01>)) = (neg zint &1)"));
            CHECK(answers_contain(collector, "(:simplify (pos zint <01>)) = &1"));
            CHECK(answers_contain(collector, "(:simplify ((neg zint <01>) + (neg zint &1))) = (neg zint &2)"));
        }
        SUBCASE("a zint whose magnitude is not a numeral is its own normal form")
        {
            // A zint of this type experiences no rewrite and falls to the
            // identity fallback, akin to a list that does not meet the
            // criteria for a numeral. Differentiation and topoly treat a
            // zint with such a magnitude as outside the vocabulary and
            // remain silent.
            interactive.process(":simplify (pos zint x)");
            interactive.process(":simplify (neg zint <x 0>)");
            collector.clear();
            interactive.process("(:simplify (pos zint x)) = X");
            interactive.process("(:simplify (neg zint <x 0>)) = X");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "(:simplify (pos zint x)) = (pos zint x)"));
            CHECK(answers_contain(collector, "(:simplify (neg zint <x 0>)) = (neg zint <x 0>)"));
        }
        SUBCASE("a zint with a sign other than pos or neg has no normal form")
        {
            // This kind of expression is not an integer, and both
            // differentiation and the facade only process pos and neg. A
            // reduced form for each zint leaf, regardless of sign, would result
            // in (foo zint &1) being its own normal form via the identity
            // fallback.
            interactive.process(":simplify (foo zint &1)");
            interactive.process(":simplify ((foo zint &1) + x)");
            collector.clear();
            interactive.process("(:simplify (foo zint &1)) = X");
            interactive.process("(:simplify ((foo zint &1) + x)) = X");
            CHECK(collect_answers(collector).empty());
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
            interactive.process(R"js(%(let [t (zelph/fact "x" "*" (zp "0"))] (string "SI-MUL0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
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
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact (zp "2") "+" (zn "5")) "*" (zn "4"))] (string "SI-FCASC-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "12")))))js");
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
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zn "3"))] (string "SI-NEGN-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "3")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zp "0"))] (string "SI-NEG0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" (zp "0"))] (string "SI-NEG0-NOT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zn "0")))))js");
            CHECK(any_output_contains(collector, "SI-NEGP-true"));
            CHECK(any_output_contains(collector, "SI-NEGN-true"));
            CHECK(any_output_contains(collector, "SI-NEG0-true"));
            CHECK(any_output_contains(collector, "SI-NEG0-NOT-false"));
        }
        SUBCASE("the negation of a zint whose magnitude is not a numeral is not unwrapped")
        {
            // A zint whose magnitude fails to be a numeral is its own normal
            // form, not a negative number, yet ZN took (neg zint x) as the
            // negative of x and simplified (neg of (neg zint x)) to x. A
            // numeral magnitude featuring a leading zero is still unwrapped,
            // into its canonical form, as the leaf rules have already read it.
            interactive.process(":simplify (neg of (neg zint x))");
            interactive.process(":simplify (neg of (neg zint <x 0>))");
            interactive.process(":simplify (neg of (neg zint <1 0>))");
            collector.clear();
            interactive.process("(:simplify (neg of (neg zint x))) = X");
            interactive.process("(:simplify (neg of (neg zint <x 0>))) = X");
            interactive.process("(:simplify (neg of (neg zint <1 0>))) = X");
            CHECK(collect_answers(collector).size() == 3);
            CHECK(answers_contain(collector, "(:simplify (neg of (neg zint x))) = (neg of (neg zint x))"));
            CHECK(answers_contain(collector, "(:simplify (neg of (neg zint <x 0>))) = (neg of (neg zint <x 0>))"));
            CHECK(answers_contain(collector, "(:simplify (neg of (neg zint <01>))) = &1"));
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
        SUBCASE("single-valuedness at the facade/SR overlap: ((+5) - (+5)) = &0 only")
        {
            interactive.process(":simplify ((pos zint &5) - (pos zint &5))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "5") "-" (zp "5"))] (string "SI-ZZ-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "5") "-" (zp "5"))] (string "SI-ZZ-NOT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zp "0")))))js");
            CHECK(any_output_contains(collector, "SI-ZZ-true"));
            CHECK(any_output_contains(collector, "SI-ZZ-NOT-false"));
        }
        SUBCASE("where the two number worlds meet, a request answers once")
        {
            // Up to version 1.0.1, the neutral and absorbing rules were
            // duplicated, appearing once within symbolic-core for the natural
            // &0 and &1, and again in this module for (pos zint &0) and
            // (pos zint &1). On a red form where the two operands came from
            // separate number worlds, both rules triggered, generating
            // separate nodes -- ((pos zint &1) * &1) turned into
            // (pos zint &1) via one rule and into &1 via the other, and the
            // request yielded both outcomes. Since there is one node per
            // nonnegative value, the answer is the natural numeral.
            for (const auto& [term, value] : {std::pair<std::string, std::string>{"(((&0 - &1) * (&0 - &1)) * &1)", "&1"},
                                              {"((pos zint &0) + &0)", "&0"},
                                              {"((pos zint &1) * &1)", "&1"},
                                              {"((pos zint &0) * &0)", "&0"}})
            {
                CAPTURE(term);
                interactive.process(":simplify " + term);
                collector.clear();
                interactive.process("(:simplify " + term + ") = X");
                CHECK(collect_answers(collector).size() == 1);
                CHECK(answers_contain(collector, "(:simplify " + term + ") = " + value));
            }
        }
        SUBCASE("the negation of a list that is not a numeral is not a negative integer")
        {
            // ZP turns (neg of N) into (neg zint N) whenever N > &0, and
            // the comparison used to determine this for any list whose
            // higher-order cells consist of digits: (neg of <x 1>)
            // became the negative integer having magnitude <x 1>.
            interactive.process(":simplify (neg of <x 1>)");
            collector.clear();
            interactive.process("(:simplify (neg of <x 1>)) = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify (neg of <x 1>)) = (neg of <x 1>)"));
        }
        SUBCASE("an explicit (pos zint N) is divided as the natural it denotes")
        {
            // In the simplifier, normalization of leaves occurs initially,
            // resulting in the red form (&7 / &2), which natural division
            // resolves. Floor, truncation, and Euclidean division yield
            // identical outcomes for nonnegative operands; however, when a
            // negative operand is involved, they diverge, and in such
            // cases, the division stays unevaluated.
            interactive.process(":simplify ((pos zint &7) / (pos zint &2))");
            interactive.process(":simplify ((neg zint &7) / &2)");
            collector.clear();
            interactive.process("(:simplify ((pos zint &7) / (pos zint &2))) = X");
            interactive.process("(:simplify ((neg zint &7) / &2)) = X");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "(:simplify ((pos zint &7) / (pos zint &2))) = &3"));
            CHECK(answers_contain(collector, "(:simplify ((neg zint &7) / &2)) = ((neg zint &7) / &2)"));
        }
        SUBCASE("an exponent written as (pos zint N) normalizes like every other leaf")
        {
            // The exponent functions as a subterm, meaning the zint
            // operand form gets converted to the natural numeral at that
            // location, just as it does elsewhere. Differentiation reads
            // the exponent by value and requires a natural numeral;
            // whatever it answers here must not be wrong.
            interactive.process(".import diff");
            interactive.process(".import symbolic-pow");
            interactive.process(":simplify (x ^ (pos zint &2))");
            interactive.process("(x ^ (pos zint &2)) diffby x");
            collector.clear();
            interactive.process("(:simplify (x ^ (pos zint &2))) = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify (x ^ (pos zint &2))) = (x ^ &2)"));
            collector.clear();
            interactive.process("((x ^ (pos zint &2)) diffby x) = D");
            const std::vector<std::string> derivative = collect_answers(collector);
            CHECK(derivative.size() <= 1);
            if (!derivative.empty()) CHECK(answers_contain(collector, "((x ^ (pos zint &2)) diffby x) = (&2 * x)"));
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

        SUBCASE("an explicit (pos zint &2) coefficient: d((+2) * x)/dx = &2")
        {
            // The raw product-rule derivative is
            // ((&0 * x) + ((pos zint &2) * &1)); the leaf (pos zint &2)
            // simplifies to &2, and symbolic-core's identities complete the
            // remaining steps.
            interactive.process("((pos zint &2) * x) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "SID-POS-" (zelph/exists (zelph/fact (zelph/fact (zp "2") "*" "x") "diffby" "x") "=" (zelph/number "2"))))js");
            CHECK(any_output_contains(collector, "SID-POS-true"));
        }
        SUBCASE("a zint is a constant only if its magnitude is a numeral")
        {
            // A zint whose magnitude fails to be a numeral resides
            // beyond the vocabulary. Without the numeral guard, it was
            // taken as a constant, and d(x * (pos zint x))/dx answered
            // (pos zint x) where silence is owed -- v1.0.1's answer as
            // well.
            interactive.process("(x * (pos zint x)) diffby x");
            interactive.process("(pos zint <x 1>) diffby x");
            interactive.process("(neg zint <x 1>) diffby x");
            collector.clear();
            interactive.process("((x * (pos zint x)) diffby x) = D");
            interactive.process("((pos zint <x 1>) diffby x) = D");
            interactive.process("((neg zint <x 1>) diffby x) = D");
            CHECK(collect_answers(collector).empty());
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
        }
        SUBCASE("a coefficient that folds to +1 gives one derivative")
        {
            // The product rule assembles the derivative of (x * c'), where
            // c' is defined as ((&0 - &1) * (&0 - &1)), with the summand
            // (&1 * c'). In v1.0.1, c' simplified to (pos zint &1): the
            // product rule's natural &1 encountered a zint one, the
            // neutral rules from each domain answered, and the derivative
            // yielded two answers. Today, c' simplifies to &1.
            interactive.process("(x * ((&0 - &1) * (&0 - &1))) diffby x");
            collector.clear();
            interactive.process("((x * ((&0 - &1) * (&0 - &1))) diffby x) = D");
            CHECK(collect_answers(collector).size() == 1);
        } });
}