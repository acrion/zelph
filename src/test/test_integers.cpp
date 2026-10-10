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
// Signed integer arithmetic: integer-arithmetic.zph
//
// The z-operations are validated using read-only zelph/exists probes,
// identified by unique markers (following the test_symbolic.cpp
// pattern), verifying the presence of expected = and relational facts
// and ensuring the absence of wrong ones. The facade cases add answer
// lines, compared via answers_contain (which normalizes spacing and
// the self-fact sugar), and enforce exact answer counts when silence is
// significant.
//
// Everything runs across all three arithmetic modules: the integer
// layer delegates every magnitude computation to the loaded natural
// module and must therefore be representation-agnostic.
// ---------------------------------------------------------------------------

namespace
{
    // Import the module and define zp/zn probe helpers building
    // canonical zint terms. zelph/fact is idempotent; zelph/exists
    // never creates facts.
    template <typename Interactive>
    void import_integers(Interactive& interactive)
    {
        interactive.process(".import integer-arithmetic");
        interactive.process(R"js(%(defn zp [s] (zelph/fact "pos" "zint" (zelph/number s))))js");
        interactive.process(R"js(%(defn zn [s] (zelph/fact "neg" "zint" (zelph/number s))))js");
    }
} // namespace

TEST_CASE("integers: signed addition (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_integers(interactive);

        SUBCASE("same signs: (+2) + (+3) = +5 and (-2) + (-3) = -5")
        {
            interactive.process("(pos zint &2) z+ (pos zint &3)");
            interactive.process("(neg zint &2) z+ (neg zint &3)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "2") "z+" (zp "3"))] (string "ZADD-PP-" (zelph/exists t "=" (zp "5")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zn "2") "z+" (zn "3"))] (string "ZADD-NN-" (zelph/exists t "=" (zn "5")))))js");
            CHECK(any_output_contains(collector, "ZADD-PP-true"));
            CHECK(any_output_contains(collector, "ZADD-NN-true"));
        }
        SUBCASE("mixed signs, positive dominates: (+5) + (-3) = +2, both orders")
        {
            interactive.process("(pos zint &5) z+ (neg zint &3)");
            interactive.process("(neg zint &3) z+ (pos zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "5") "z+" (zn "3"))] (string "ZADD-PN-" (zelph/exists t "=" (zp "2")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zn "3") "z+" (zp "5"))] (string "ZADD-NP-" (zelph/exists t "=" (zp "2")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "5") "z+" (zn "3"))] (string "ZADD-PN-NOT-" (zelph/exists t "=" (zn "2")))))js");
            CHECK(any_output_contains(collector, "ZADD-PN-true"));
            CHECK(any_output_contains(collector, "ZADD-NP-true"));
            CHECK(any_output_contains(collector, "ZADD-PN-NOT-false"));
        }
        SUBCASE("mixed signs, negative dominates: (+3) + (-5) = -2")
        {
            interactive.process("(pos zint &3) z+ (neg zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "z+" (zn "5"))] (string "ZADD-PNNEG-" (zelph/exists t "=" (zn "2")))))js");
            CHECK(any_output_contains(collector, "ZADD-PNNEG-true"));
        }
        SUBCASE("cancellation: (+7) + (-7) = +0, and (neg zint &0) is never produced")
        {
            interactive.process("(pos zint &7) z+ (neg zint &7)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "7") "z+" (zn "7"))] (string "ZADD-ZERO-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "7") "z+" (zn "7"))] (string "ZADD-NEGZERO-" (zelph/exists t "=" (zn "0")))))js");
            CHECK(any_output_contains(collector, "ZADD-ZERO-true"));
            CHECK(any_output_contains(collector, "ZADD-NEGZERO-false"));
        } });
}

TEST_CASE("integers: signed subtraction via delegation (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_integers(interactive);

        SUBCASE("(+5) - (+3) = +2 and (+3) - (+5) = -2")
        {
            interactive.process("(pos zint &5) z- (pos zint &3)");
            interactive.process("(pos zint &3) z- (pos zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "5") "z-" (zp "3"))] (string "ZSUB-PP-" (zelph/exists t "=" (zp "2")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "z-" (zp "5"))] (string "ZSUB-NEG-" (zelph/exists t "=" (zn "2")))))js");
            CHECK(any_output_contains(collector, "ZSUB-PP-true"));
            CHECK(any_output_contains(collector, "ZSUB-NEG-true"));
        }
        SUBCASE("subtracting a negative adds: (+3) - (-4) = +7")
        {
            interactive.process("(pos zint &3) z- (neg zint &4)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "z-" (zn "4"))] (string "ZSUB-NN-" (zelph/exists t "=" (zp "7")))))js");
            CHECK(any_output_contains(collector, "ZSUB-NN-true"));
        }
        SUBCASE("(-3) - (+5) = -8")
        {
            interactive.process("(neg zint &3) z- (pos zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zn "3") "z-" (zp "5"))] (string "ZSUB-NP-" (zelph/exists t "=" (zn "8")))))js");
            CHECK(any_output_contains(collector, "ZSUB-NP-true"));
        }
        SUBCASE("subtracting zero: (-4) - (+0) = -4 (direct rule, no delegation)")
        {
            interactive.process("(neg zint &4) z- (pos zint &0)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zn "4") "z-" (zp "0"))] (string "ZSUB-ZERO-" (zelph/exists t "=" (zn "4")))))js");
            CHECK(any_output_contains(collector, "ZSUB-ZERO-true"));
        }
        SUBCASE("self-cancellation: (+7) - (+7) = +0, never (neg zint &0)")
        {
            interactive.process("(pos zint &7) z- (pos zint &7)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "7") "z-" (zp "7"))] (string "ZSUB-CANCEL-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "7") "z-" (zp "7"))] (string "ZSUB-CANCEL-NOT-" (zelph/exists t "=" (zn "0")))))js");
            CHECK(any_output_contains(collector, "ZSUB-CANCEL-true"));
            CHECK(any_output_contains(collector, "ZSUB-CANCEL-NOT-false"));
        } });
}

TEST_CASE("integers: signed multiplication (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_integers(interactive);

        SUBCASE("sign table: (+3)(+4), (-3)(-4), (+3)(-4), (-3)(+4)")
        {
            interactive.process("(pos zint &3) zx (pos zint &4)");
            interactive.process("(neg zint &3) zx (neg zint &4)");
            interactive.process("(pos zint &3) zx (neg zint &4)");
            interactive.process("(neg zint &3) zx (pos zint &4)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "zx" (zp "4"))] (string "ZMUL-PP-" (zelph/exists t "=" (zp "12")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zn "3") "zx" (zn "4"))] (string "ZMUL-NN-" (zelph/exists t "=" (zp "12")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "zx" (zn "4"))] (string "ZMUL-PN-" (zelph/exists t "=" (zn "12")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zn "3") "zx" (zp "4"))] (string "ZMUL-NP-" (zelph/exists t "=" (zn "12")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zn "3") "zx" (zn "4"))] (string "ZMUL-NN-NOT-" (zelph/exists t "=" (zn "12")))))js");
            CHECK(any_output_contains(collector, "ZMUL-PP-true"));
            CHECK(any_output_contains(collector, "ZMUL-NN-true"));
            CHECK(any_output_contains(collector, "ZMUL-PN-true"));
            CHECK(any_output_contains(collector, "ZMUL-NP-true"));
            CHECK(any_output_contains(collector, "ZMUL-NN-NOT-false"));
        }
        SUBCASE("zero absorbs canonically: (+0)(-4) = +0 and (-4)(+0) = +0")
        {
            interactive.process("(pos zint &0) zx (neg zint &4)");
            interactive.process("(neg zint &4) zx (pos zint &0)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "0") "zx" (zn "4"))] (string "ZMUL-ZL-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zn "4") "zx" (zp "0"))] (string "ZMUL-ZR-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "0") "zx" (zn "4"))] (string "ZMUL-ZL-NOT-" (zelph/exists t "=" (zn "0")))))js");
            CHECK(any_output_contains(collector, "ZMUL-ZL-true"));
            CHECK(any_output_contains(collector, "ZMUL-ZR-true"));
            CHECK(any_output_contains(collector, "ZMUL-ZL-NOT-false"));
        } 
        SUBCASE("same-sign zero delegates the natural zero product: (+3)(+0) = +0, both orders")
        {
            interactive.process("(pos zint &3) zx (pos zint &0)");
            interactive.process("(pos zint &0) zx (pos zint &3)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "zx" (zp "0"))] (string "ZMUL-PZ-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "0") "zx" (zp "3"))] (string "ZMUL-ZP-" (zelph/exists t "=" (zp "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "3") "zx" (zp "0")) raw (zelph/fact "pos" "zint" (zelph/fact "0" "cons" (zelph/number "0")))] (string "ZMUL-PZ-RAW-" (zelph/exists t "=" raw))))js");
            CHECK(any_output_contains(collector, "ZMUL-PZ-true"));
            CHECK(any_output_contains(collector, "ZMUL-ZP-true"));
            CHECK(any_output_contains(collector, "ZMUL-PZ-RAW-false"));
        } });
}

TEST_CASE("integers: signed comparison (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_integers(interactive);

        SUBCASE("mixed signs decide by sign alone: (+2) > (-9) despite magnitudes")
        {
            interactive.process("(pos zint &2) zcmp (neg zint &9)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "ZCMP-MIX-" (zelph/exists (zp "2") ">" (zn "9"))))js");
            interactive.process(R"js(%(string "ZCMP-MIXBR-" (zelph/exists (zelph/fact (zp "2") "zcmp" (zn "9")) "=" (zelph/resolve "gt"))))js");
            CHECK(any_output_contains(collector, "ZCMP-MIX-true"));
            CHECK(any_output_contains(collector, "ZCMP-MIXBR-true"));
        }
        SUBCASE("two negatives reverse the magnitude order: (-3) > (-5)")
        {
            interactive.process("(neg zint &3) zcmp (neg zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "ZCMP-NN-" (zelph/exists (zn "3") ">" (zn "5"))))js");
            interactive.process(R"js(%(string "ZCMP-NN-NOT-" (zelph/exists (zn "3") "<" (zn "5"))))js");
            CHECK(any_output_contains(collector, "ZCMP-NN-true"));
            CHECK(any_output_contains(collector, "ZCMP-NN-NOT-false"));
        }
        SUBCASE("equality: (-5) zcmp (-5) yields == and bridges to eq")
        {
            interactive.process("(neg zint &5) zcmp (neg zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "ZCMP-EQ-" (zelph/exists (zn "5") "==" (zn "5"))))js");
            interactive.process(R"js(%(string "ZCMP-EQBR-" (zelph/exists (zelph/fact (zn "5") "zcmp" (zn "5")) "=" (zelph/resolve "eq"))))js");
            CHECK(any_output_contains(collector, "ZCMP-EQ-true"));
            CHECK(any_output_contains(collector, "ZCMP-EQBR-true"));
        } });
}

TEST_CASE("integers: composability and cross-module cascades (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_integers(interactive);

        SUBCASE("z results feed user rules: product compared with its factor")
        {
            // (-3)*(+4) = -12, then a user rule asserts the comparison
            // trigger; -12 < -3 must follow (negative reversal).
            process_lines(interactive, R"(
((A zx B) = P) => (P zcmp A)
(neg zint &3) zx (pos zint &4)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "ZCASC-" (zelph/exists (zn "12") "<" (zn "3"))))js");
            CHECK(any_output_contains(collector, "ZCASC-true"));
        }
        SUBCASE("magnitude work persists as ordinary natural-module knowledge")
        {
            // A mixed-sign addition internally asserts (&5 - &3) and
            // (&5 cmp &3); both results must exist as plain natural facts.
            interactive.process("(pos zint &5) z+ (neg zint &3)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "ZNAT-SUB-" (zelph/exists (zelph/fact (zelph/number "5") "-" (zelph/number "3")) "=" (zelph/number "2"))))js");
            interactive.process(R"js(%(string "ZNAT-CMP-" (zelph/exists (zelph/number "5") ">" (zelph/number "3"))))js");
            CHECK(any_output_contains(collector, "ZNAT-SUB-true"));
            CHECK(any_output_contains(collector, "ZNAT-CMP-true"));
        }
        SUBCASE("transitivity meta-rule spans computed integer comparisons")
        {
            process_lines(interactive, R"(
(R is transitive, A R B, B R C) => (A R C)
> is transitive
(pos zint &2) zcmp (neg zint &1)
(neg zint &1) zcmp (neg zint &3)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "ZTRANS-" (zelph/exists (zp "2") ">" (zn "3"))))js");
            CHECK(any_output_contains(collector, "ZTRANS-true"));
        } });
}

TEST_CASE("integers: uniform operator facade on shared predicates (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        import_integers(interactive);

        SUBCASE("+ routes to z+: ((+2) + (-5)) = -3 under the shared = idiom")
        {
            interactive.process("(pos zint &2) + (neg zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "2") "+" (zn "5"))] (string "ZFAC-ADD-" (zelph/exists t "=" (zn "3")))))js");
            CHECK(any_output_contains(collector, "ZFAC-ADD-true"));
        }
        SUBCASE("- routes to z-: ((-2) - (-5)) = 3, the natural numeral")
        {
            // A nonnegative result is exposed as the natural numeral, not as
            // (pos zint N): one node per value spanning N and Z, ensuring
            // that no consumer of = facts meets the same value more than
            // once.
            interactive.process("(neg zint &2) - (neg zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zn "2") "-" (zn "5"))] (string "ZFAC-SUB-" (zelph/exists t "=" (zelph/number "3")) "-ZP-" (zelph/exists t "=" (zp "3")))))js");
            CHECK(any_output_contains(collector, "ZFAC-SUB-true-ZP-false"));
        }
        SUBCASE("* routes to zx: ((-3) * (-4)) = 12, the natural numeral")
        {
            interactive.process("(neg zint &3) * (neg zint &4)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zn "3") "*" (zn "4"))] (string "ZFAC-MUL-" (zelph/exists t "=" (zelph/number "12")) "-ZP-" (zelph/exists t "=" (zp "12")))))js");
            CHECK(any_output_contains(collector, "ZFAC-MUL-true-ZP-false"));
        }
        SUBCASE("a natural next to a negative is routed, in both orders")
        {
            // Nonnegative integers ARE identical to natural numerals, hence
            // mixed arithmetic must recognize them as valid operands for the
            // z-operations.
            process_lines(interactive, R"(
&2 + (neg zint &5)
(neg zint &2) + &5
&7 - (neg zint &2)
(neg zint &2) * &3
)");
            collector.clear();
            interactive.process("(&2 + (neg zint &5)) = X");
            interactive.process("((neg zint &2) + &5) = X");
            interactive.process("(&7 - (neg zint &2)) = X");
            interactive.process("((neg zint &2) * &3) = X");
            CHECK(collect_answers(collector).size() == 4);
            CHECK(answers_contain(collector, "(&2 + (neg zint &5)) = (neg zint &3)"));
            CHECK(answers_contain(collector, "((neg zint &2) + &5) = &3"));
            CHECK(answers_contain(collector, "(&7 - (neg zint &2)) = &9"));
            CHECK(answers_contain(collector, "((neg zint &2) * &3) = (neg zint &6)"));
        }
        SUBCASE("cmp routes to zcmp: relational fact plus = bridge")
        {
            interactive.process("(pos zint &2) cmp (neg zint &9)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "ZFAC-CMPR-" (zelph/exists (zp "2") ">" (zn "9"))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zp "2") "cmp" (zn "9"))] (string "ZFAC-CMPB-" (zelph/exists t "=" (zelph/resolve "gt")))))js");
            CHECK(any_output_contains(collector, "ZFAC-CMPR-true"));
            CHECK(any_output_contains(collector, "ZFAC-CMPB-true"));
        }
        SUBCASE("a signed zero compares equal to zero, in every shape")
        {
            // The value (neg zint &0) arises from no rule but is permitted as
            // input. It was treated as a negative number, and determining a
            // comparison with a negative number relies solely on the sign, both
            // in zcmp and in the facade's former rule for comparing a natural
            // number against a negative one: (&0 cmp (neg zint &0)) answered
            // gt, and the relational fact (&0 > (neg zint &0)) was derived. The
            // facade now lifts a negative operand whose magnitude is zero to
            // the zero (pos zint &0) before forwarding it. Assessing the
            // magnitude by value results in (neg zint <0 0>) being zero as
            // well.
            process_lines(interactive, R"(
&0 cmp (neg zint &0)
(neg zint &0) cmp &0
(pos zint &0) cmp (neg zint &0)
(neg zint &0) cmp (neg zint &0)
&0 cmp (neg zint <0 0>)
&3 cmp (neg zint &0)
(neg zint &0) cmp (neg zint &3)
(neg zint &3) cmp (neg zint &0)
(pos zint <1 0>) cmp (neg zint &1)
)");
            collector.clear();
            process_lines(interactive, R"(
(&0 cmp (neg zint &0)) = R
((neg zint &0) cmp &0) = R
((pos zint &0) cmp (neg zint &0)) = R
((neg zint &0) cmp (neg zint &0)) = R
(&0 cmp (neg zint <0 0>)) = R
(&3 cmp (neg zint &0)) = R
((neg zint &0) cmp (neg zint &3)) = R
)");
            CHECK(collect_answers(collector).size() == 7);
            CHECK(answers_contain(collector, "(&0 cmp (neg zint &0)) = eq"));
            CHECK(answers_contain(collector, "((neg zint &0) cmp &0) = eq"));
            CHECK(answers_contain(collector, "((pos zint &0) cmp (neg zint &0)) = eq"));
            CHECK(answers_contain(collector, "((neg zint &0) cmp (neg zint &0)) = eq"));
            CHECK(answers_contain(collector, "(&0 cmp (neg zint <00>)) = eq"));
            CHECK(answers_contain(collector, "(&3 cmp (neg zint &0)) = gt"));
            CHECK(answers_contain(collector, "((neg zint &0) cmp (neg zint &3)) = gt"));

            collector.clear();
            interactive.process(R"js(%(string "ZFAC-SZ-EQ-" (zelph/exists (zelph/number "0") "==" (zn "0"))))js");
            interactive.process(R"js(%(string "ZFAC-SZ-GT-" (zelph/exists (zelph/number "0") ">" (zn "0"))))js");
            interactive.process(R"js(%(string "ZFAC-SZ-PEQ-" (zelph/exists (zp "0") "==" (zn "0"))))js");
            CHECK(any_output_contains(collector, "ZFAC-SZ-EQ-true"));
            CHECK(any_output_contains(collector, "ZFAC-SZ-GT-false"));
            CHECK(any_output_contains(collector, "ZFAC-SZ-PEQ-true"));

            // zcmp derives its relational facts concerning the LIFTED
            // operands. When the lift equals the written term, that is
            // already the fact regarding the operands as written; only an
            // operand whose lift differs -- a signed zero, a magnitude
            // featuring a leading zero -- depends on the rules of the
            // zint/zint cmp route, which re-express >, <, and == for the
            // operands as written. == is pinned above; these pin > and <.
            // <1 0> constitutes a numeral in every substrate, including
            // binary.
            collector.clear();
            interactive.process(R"js(%(string "ZFAC-SZ-RGT-" (zelph/exists (zn "0") ">" (zn "3"))))js");
            interactive.process(R"js(%(string "ZFAC-SZ-RLT-" (zelph/exists (zn "3") "<" (zn "0"))))js");
            interactive.process("(pos zint <1 0>) > X");
            CHECK(any_output_contains(collector, "ZFAC-SZ-RGT-true"));
            CHECK(any_output_contains(collector, "ZFAC-SZ-RLT-true"));
            CHECK(answers_contain(collector, "(pos zint <01>) > (neg zint &1)"));
        }
        SUBCASE("arithmetic with a signed zero gives the canonical zero, never (neg zint &0)")
        {
            // Routed as it stood, a signed zero remained after applying the
            // sign rules: (&3 * (neg zint &0)) and ((neg zint &0) + (neg zint
            // &0)) both answered (neg zint &0), representing a second node for
            // the value zero.
            process_lines(interactive, R"(
&3 * (neg zint &0)
(neg zint &0) * &3
(pos zint &3) * (neg zint &0)
(neg zint &0) + (neg zint &0)
(neg zint &0) + (neg zint &3)
(neg zint &0) - (pos zint &0)
(neg zint &0) - &0
&5 - (neg zint &0)
)");
            collector.clear();
            process_lines(interactive, R"(
(&3 * (neg zint &0)) = X
((neg zint &0) * &3) = X
((pos zint &3) * (neg zint &0)) = X
((neg zint &0) + (neg zint &0)) = X
((neg zint &0) + (neg zint &3)) = X
((neg zint &0) - (pos zint &0)) = X
((neg zint &0) - &0) = X
(&5 - (neg zint &0)) = X
)");
            CHECK(collect_answers(collector).size() == 8);
            CHECK(answers_contain(collector, "(&3 * (neg zint &0)) = &0"));
            CHECK(answers_contain(collector, "((neg zint &0) * &3) = &0"));
            CHECK(answers_contain(collector, "((pos zint &3) * (neg zint &0)) = &0"));
            CHECK(answers_contain(collector, "((neg zint &0) + (neg zint &0)) = &0"));
            CHECK(answers_contain(collector, "((neg zint &0) + (neg zint &3)) = (neg zint &3)"));
            CHECK(answers_contain(collector, "((neg zint &0) - (pos zint &0)) = &0"));
            CHECK(answers_contain(collector, "((neg zint &0) - &0) = &0"));
            CHECK(answers_contain(collector, "(&5 - (neg zint &0)) = &5"));
        }
        SUBCASE("an operand that is not a number is not routed")
        {
            // Each operand must pass the numeral test before being lifted. A
            // sign by itself made &2 compare greater than (neg zint x), and
            // (pos zint x) compare greater than (neg zint &2); and a signed
            // zero routed as (pos zint &0) would let z-'s rule for a zero
            // subtrahend return <x c> for (<x c> - (neg zint &0)).
            //
            // An atom magnitude such as the x in (neg zint x) is excluded by
            // the (A cons R) shape of the lift rules before any test runs; a
            // LIST magnitude is excluded solely by the numeral test, hence
            // the four <x 1> comparisons pin the test on the zint side.
            // Absent this, they answered by sign
            // ((&2 cmp (neg zint <x 1>)) = gt); a + fact with such an
            // operand remains silent even then, thus contributing no
            // pinning here. The empty list is not a numeral either, and no
            // facade shape accepts it.
            process_lines(interactive, R"(
&2 cmp (neg zint x)
(neg zint x) + &2
(pos zint x) cmp (neg zint &2)
<x c> - (neg zint &0)
(pos zint x) - (neg zint &0)
&2 cmp (neg zint <x 1>)
(neg zint <x 1>) cmp &2
(neg zint <x 1>) cmp (pos zint &2)
(pos zint <x 1>) cmp (neg zint &2)
<> + (neg zint &3)
<> cmp (neg zint &3)
)");
            collector.clear();
            process_lines(interactive, R"(
(&2 cmp (neg zint x)) = R
((neg zint x) + &2) = R
((pos zint x) cmp (neg zint &2)) = R
(<x c> - (neg zint &0)) = R
((pos zint x) - (neg zint &0)) = R
(&2 cmp (neg zint <x 1>)) = R
((neg zint <x 1>) cmp &2) = R
((neg zint <x 1>) cmp (pos zint &2)) = R
((pos zint <x 1>) cmp (neg zint &2)) = R
X > (neg zint <x 1>)
(neg zint <x 1>) < X
(pos zint <x 1>) > X
(nil + (neg zint &3)) = R
(nil cmp (neg zint &3)) = R
)");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a magnitude with a leading zero is read in canonical form")
        {
            // The z-operations take their operands as canonical, and z+
            // performs the addition of two negative magnitudes without
            // canonnum: the sum of adding (neg zint <1 0>) and (neg zint &1)
            // appeared as a raw list within the decimal substrate. A natural
            // <1 0> beside a signed zero lifted only to (pos zint &0) would
            // take the path of natural addition, which keeps leading zeros.
            process_lines(interactive, R"(
(neg zint <1 0>) + (neg zint &1)
<1 0> + (neg zint &0)
(neg zint &1) - (pos zint <0 0>)
)");
            collector.clear();
            process_lines(interactive, R"(
((neg zint <1 0>) + (neg zint &1)) = X
(<1 0> + (neg zint &0)) = X
((neg zint &1) - (pos zint <0 0>)) = X
)");
            CHECK(collect_answers(collector).size() == 3);
            CHECK(answers_contain(collector, "((neg zint <01>) + (neg zint &1)) = (neg zint &2)"));
            CHECK(answers_contain(collector, "(<01> + (neg zint &0)) = &1"));
            CHECK(answers_contain(collector, "((neg zint &1) - (pos zint <00>)) = (neg zint &1)"));
        }
        SUBCASE("a natural compared with a negative leaves no fact about the operand form")
        {
            // Routed through zcmp using the operand form (pos zint &2), the
            // comparison would leave (pos zint &2) > (neg zint &3) beside &2 >
            // (neg zint &3): two nodes for a single value on the predicates
            // that rules over N and Z share.
            interactive.process("&2 cmp (neg zint &3)");
            interactive.process("(neg zint &3) cmp &2");
            collector.clear();
            interactive.process(R"js(%(string "ZFAC-NN-GT-" (zelph/exists (zelph/number "2") ">" (zn "3"))))js");
            interactive.process(R"js(%(string "ZFAC-NN-LT-" (zelph/exists (zn "3") "<" (zelph/number "2"))))js");
            interactive.process(R"js(%(string "ZFAC-NN-PGT-" (zelph/exists (zp "2") ">" (zn "3"))))js");
            interactive.process(R"js(%(string "ZFAC-NN-PLT-" (zelph/exists (zn "3") "<" (zp "2"))))js");
            CHECK(any_output_contains(collector, "ZFAC-NN-GT-true"));
            CHECK(any_output_contains(collector, "ZFAC-NN-LT-true"));
            CHECK(any_output_contains(collector, "ZFAC-NN-PGT-false"));
            CHECK(any_output_contains(collector, "ZFAC-NN-PLT-false"));
        }
        SUBCASE("zelph/int reads every spelling of a negative zero as the natural zero")
        {
            // Only the literal -0 was recognised; -00 constructed
            // (neg zint &0).
            collector.clear();
            interactive.process(R"js(%(string "ZINT-M00-" (= (zelph/int "-00") (zelph/number "0"))))js");
            interactive.process(R"js(%(string "ZINT-M0-" (= (zelph/int "-0") (zelph/number "0"))))js");
            interactive.process(R"js(%(string "ZINT-M05-" (= (zelph/int "-05") (zn "5"))))js");
            CHECK(any_output_contains(collector, "ZINT-M00-true"));
            CHECK(any_output_contains(collector, "ZINT-M0-true"));
            CHECK(any_output_contains(collector, "ZINT-M05-true"));
        }
        SUBCASE("division stays unrouted: a zint / fact derives nothing")
        {
            // The facade lowers a nonnegative outcome to the natural
            // numeral, meaning a routed quotient of two pos operands would
            // answer &3, rather than (pos zint &3), and the probe for the
            // zint form by itself could not detect it. Requesting any
            // answer pins the silence.
            interactive.process("(pos zint &6) / (pos zint &2)");
            interactive.process("(pos zint &7) mod (pos zint &2)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "6") "/" (zp "2"))] (string "ZFAC-DIV-" (zelph/exists t "=" (zp "3")))))js");
            CHECK(any_output_contains(collector, "ZFAC-DIV-false"));

            collector.clear();
            interactive.process("((pos zint &6) / (pos zint &2)) = X");
            interactive.process("((pos zint &7) mod (pos zint &2)) = X");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a list that is not a numeral is not compared with a negative")
        {
            // A natural number surpasses a negative one based solely on sign,
            // and the facade took any cons list as a natural: <x c> compared
            // greater than every negative integer.
            interactive.process("&2 cmp (neg zint &1)");
            interactive.process("<x c> cmp (neg zint &1)");
            interactive.process("(neg zint &1) cmp <x c>");
            collector.clear();
            interactive.process("(&2 cmp (neg zint &1)) = R");
            interactive.process("(<x c> cmp (neg zint &1)) = R");
            interactive.process("((neg zint &1) cmp <x c>) = R");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(&2 cmp (neg zint &1)) = gt"));
        }
        SUBCASE("/ and mod with a negative operand derive nothing, in either position")
        {
            // The floor function, truncation, and Euclidean division yield
            // identical results when applied to nonnegative operands --
            // such operands are natural numerals and are divided by the
            // natural module. Disagreement emerges immediately when either
            // operand becomes negative, and this decision is intentionally
            // left unresolved: until such a choice is established, neither
            // the dividend nor the divisor can be negative.
            process_lines(interactive, R"(
(neg zint &7) / &2
&7 / (neg zint &2)
(neg zint &7) / (neg zint &2)
(neg zint &7) mod &2
&7 mod (neg zint &2)
(neg zint &7) mod (neg zint &2)
)");
            collector.clear();
            process_lines(interactive, R"(
((neg zint &7) / &2) = X
(&7 / (neg zint &2)) = X
((neg zint &7) / (neg zint &2)) = X
((neg zint &7) mod &2) = X
(&7 mod (neg zint &2)) = X
((neg zint &7) mod (neg zint &2)) = X
)");
            CHECK(collect_answers(collector).empty());
        } });
}
