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
// Assertions are STRUCTURAL, via read-only zelph/exists probes tagged
// with unique markers (the test_symbolic.cpp pattern): zint terms mix
// plain atoms (rendered with surrounding spaces) and &-literals
// (rendered attached), so expected output strings would be brittle.
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

TEST_CASE("integers: signed addition (all arithmetic modules)")
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

TEST_CASE("integers: signed subtraction via delegation (all arithmetic modules)")
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

TEST_CASE("integers: signed multiplication (all arithmetic modules)")
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

TEST_CASE("integers: composability and cross-module cascades (all arithmetic modules)")
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

TEST_CASE("integers: uniform operator facade on shared predicates (all arithmetic modules)")
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
        SUBCASE("- routes to z-: ((-2) - (-5)) = +3")
        {
            interactive.process("(neg zint &2) - (neg zint &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zn "2") "-" (zn "5"))] (string "ZFAC-SUB-" (zelph/exists t "=" (zp "3")))))js");
            CHECK(any_output_contains(collector, "ZFAC-SUB-true"));
        }
        SUBCASE("* routes to zx: ((-3) * (-4)) = +12")
        {
            interactive.process("(neg zint &3) * (neg zint &4)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zn "3") "*" (zn "4"))] (string "ZFAC-MUL-" (zelph/exists t "=" (zp "12")))))js");
            CHECK(any_output_contains(collector, "ZFAC-MUL-true"));
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
        SUBCASE("division stays unrouted: a zint / fact derives nothing")
        {
            interactive.process("(pos zint &6) / (pos zint &2)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zp "6") "/" (zp "2"))] (string "ZFAC-DIV-" (zelph/exists t "=" (zp "3")))))js");
            CHECK(any_output_contains(collector, "ZFAC-DIV-false"));
        } });
}
