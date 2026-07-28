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
// Symbolic mathematics: symbolic-core.zph (M1) and diff.zph (M2)
//
// Assertions are STRUCTURAL, via read-only zelph/exists probes tagged
// with unique markers, instead of matching rendered output: symbolic
// terms mix plain atoms (rendered with surrounding spaces) and
// &-literals (rendered attached), so expected strings would be brittle.
// zelph/fact inside a probe is idempotent -- every node it touches
// already exists if the pipeline worked -- and zelph/exists never
// creates anything. The only string checks are &-literal-only formats
// already pinned by test_numbers.cpp.
//
// Everything runs across all three arithmetic modules: the symbolic
// layer must be numeral-representation-agnostic, since its &0/&1 leaves
// are the loaded module's cons lists.
//
// These tests intentionally mix the explicit syntax `S P S` and the syntax sugar `:P S`.
// ---------------------------------------------------------------------------

TEST_CASE("symbolic: simplification core (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
)");

        SUBCASE("neutral elements: (x + &0) and (&1 * x) simplify to x")
        {
            interactive.process(":simplify (x + &0)");
            interactive.process("(&1 * x) simplify (&1 * x)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "+" (zelph/number "0"))] (string "SIMP-PLUS0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "1") "*" "x")] (string "SIMP-MUL1-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            CHECK(any_output_contains(collector, "SIMP-PLUS0-true"));
            CHECK(any_output_contains(collector, "SIMP-MUL1-true"));
        }
        SUBCASE("absorbing element: (x * &0) simplifies to &0")
        {
            interactive.process(":simplify (x * &0)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "*" (zelph/number "0"))] (string "SIMP-MUL0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "SIMP-MUL0-true"));
        }
        SUBCASE("generic inverse meta-rule: both declared orientations")
        {
            interactive.process(":simplify (exp of (ln of x))");
            interactive.process("(ln of (exp of x)) simplify (ln of (exp of x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "exp" "of" (zelph/fact "ln" "of" "x"))] (string "SIMP-INV1-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "ln" "of" (zelph/fact "exp" "of" "x"))] (string "SIMP-INV2-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            CHECK(any_output_contains(collector, "SIMP-INV1-true"));
            CHECK(any_output_contains(collector, "SIMP-INV2-true"));
        }
        SUBCASE("stratum alternation: (exp of (ln of (x + &0))) simplifies to x")
        {
            // The key scheduling pin: the inner (ln of x) needs the
            // deferred identity fallback, whose consequence must re-open
            // the positive stratum so the outer inverse rewrite can fire.
            interactive.process(":simplify (exp of (ln of (x + &0)))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "exp" "of" (zelph/fact "ln" "of" (zelph/fact "x" "+" (zelph/number "0"))))] (string "SIMP-CHAIN-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            CHECK(any_output_contains(collector, "SIMP-CHAIN-true"));
        }
        SUBCASE("identity fallback (NAF): (x + y) is its own normal form")
        {
            interactive.process("(x + y) simplify (x + y)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "+" "y")] (string "SIMP-ID-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            interactive.process(R"js(%(let [t (zelph/fact "x" "+" "y")] (string "SIMP-ID-NOT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "x")))))js");
            CHECK(any_output_contains(collector, "SIMP-ID-true"));
            CHECK(any_output_contains(collector, "SIMP-ID-NOT-false"));
        }
        SUBCASE("numeral leaves are opaque: (&5 simplify &5) answers &5")
        {
            interactive.process(":simplify &5");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/number "5")] (string "SIMP-NUM-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            CHECK(any_output_contains(collector, "SIMP-NUM-true"));
        } });
}

TEST_CASE("symbolic: differentiation base cases and constancy (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import diff");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
c ~ symconst
)");

        SUBCASE("dx/dx = &1 (and not &0)")
        {
            interactive.process(":diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-VAR-" (zelph/exists (zelph/fact "x" "diffby" "x") "=" (zelph/number "1"))))js");
            interactive.process(R"js(%(string "DIFF-VAR-NOT-" (zelph/exists (zelph/fact "x" "diffby" "x") "=" (zelph/number "0"))))js");
            CHECK(any_output_contains(collector, "DIFF-VAR-true"));
            CHECK(any_output_contains(collector, "DIFF-VAR-NOT-false"));
        }
        SUBCASE("constants via NAF: dc/dx = &0 and dy/dx = &0")
        {
            interactive.process("c diffby x");
            interactive.process("y diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-CONST-" (zelph/exists (zelph/fact "c" "diffby" "x") "=" (zelph/number "0"))))js");
            interactive.process(R"js(%(string "DIFF-OTHERVAR-" (zelph/exists (zelph/fact "y" "diffby" "x") "=" (zelph/number "0"))))js");
            CHECK(any_output_contains(collector, "DIFF-CONST-true"));
            CHECK(any_output_contains(collector, "DIFF-OTHERVAR-true"));
        }
        SUBCASE("constant composite: d(y*y)/dx = &0, single-valued")
        {
            // Both derivation paths (NAF short-circuit at the composite
            // AND the structural product rule over &0-children) must
            // collapse to the same &0 through the simplifier; the raw
            // composite derivative must never surface under =.
            interactive.process("(y * y) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-CCOMP-" (zelph/exists (zelph/fact (zelph/fact "y" "*" "y") "diffby" "x") "=" (zelph/number "0"))))js");
            interactive.process(R"js(%(string "DIFF-CCOMP-NOT-" (zelph/exists (zelph/fact (zelph/fact "y" "*" "y") "diffby" "x") "=" (zelph/fact (zelph/fact (zelph/number "0") "*" "y") "+" (zelph/fact "y" "*" (zelph/number "0"))))))js");
            CHECK(any_output_contains(collector, "DIFF-CCOMP-true"));
            CHECK(any_output_contains(collector, "DIFF-CCOMP-NOT-false"));
        }
        SUBCASE("shapes outside the vocabulary stay SILENT, not constant")
        {
            // The constancy rule is a negation over `contains`, and
            // `contains` only walks the forms the DD rules decompose --
            // while the TRIGGER hands a dstate fact to whatever was
            // requested. Ungated, a shape diff knows nothing about failed
            // the containment test for the trivial reason that no rule
            // could ever have derived it, and was declared constant: a
            // WRONG answer, where the stdlib owes silence. Rules DS gate
            // the fallback on a positive shape marker instead.
            //
            // (x / y) is the case that matters most: / is ordinary
            // symbolic-core vocabulary AND is produced by diff itself (the
            // ln rule), so a wrong zero here would poison every second
            // derivative of a logarithm.
            interactive.process("(x / y) diffby x");
            interactive.process("(x nosuchop y) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-DIV-" (zelph/exists (zelph/fact (zelph/fact "x" "/" "y") "diffby" "x") "=" (zelph/number "0"))))js");
            interactive.process(R"js(%(string "DIFF-FOREIGN-" (zelph/exists (zelph/fact (zelph/fact "x" "nosuchop" "y") "diffby" "x") "=" (zelph/number "0"))))js");
            CHECK(any_output_contains(collector, "DIFF-DIV-false"));
            CHECK(any_output_contains(collector, "DIFF-FOREIGN-false"));
        } });
}

TEST_CASE("symbolic: iterated differentiation along a list (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import symbolic-pow");
        interactive.process(".import diff");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
)");

        SUBCASE("the empty list differentiates nothing")
        {
            interactive.process("(x * x) diffalong nil");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "*" "x")] (string "DL-NIL-" (zelph/exists (zelph/fact t "diffalong" (zelph/resolve "nil")) "=" t))))js");
            CHECK(any_output_contains(collector, "DL-NIL-true"));
        }
        SUBCASE("a one-element list is an ordinary derivative")
        {
            interactive.process("(x * x) diffalong <x>");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "*" "x")] (string "DL-ONE-" (zelph/exists (zelph/fact t "diffalong" (zelph/list "x")) "=" (zelph/fact "x" "+" "x")))))js");
            CHECK(any_output_contains(collector, "DL-ONE-true"));
        }
        SUBCASE("mixed partials agree in both orders (Schwarz/Clairaut)")
        {
            // The two requests are DIFFERENT nodes -- <x y> and <y x> are
            // distinct cons lists -- so agreement is a derived result, not
            // an artefact of the encoding. Facts carry a SET of objects,
            // which is exactly why the order is expressed as a list.
            interactive.process("((x * x) * y) diffalong <x y>");
            interactive.process("((x * x) * y) diffalong <y x>");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact "x" "*" "x") "*" "y") e (zelph/fact "x" "+" "x")] (string "DL-MIX-" (and (zelph/exists (zelph/fact t "diffalong" (zelph/list "x" "y")) "=" e) (zelph/exists (zelph/fact t "diffalong" (zelph/list "y" "x")) "=" e)))))js");
            interactive.process(R"js(%(string "DL-DISTINCT-" (= (zelph/list "x" "y") (zelph/list "y" "x"))))js");
            CHECK(any_output_contains(collector, "DL-MIX-true"));
            CHECK(any_output_contains(collector, "DL-DISTINCT-false"));
        }
        SUBCASE("partiality composes: a silent step silences the chain")
        {
            // The first step is outside diff's shape domain, so it derives
            // nothing -- and the list recursion must not invent a result.
            interactive.process("(x / y) diffalong <x x>");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "/" "y")] (string "DL-PART-" (zelph/exists (zelph/fact t "diffalong" (zelph/list "x" "x")) "=" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "DL-PART-false"));
        } });
}

TEST_CASE("symbolic: differentiation structural rules (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import diff");
        process_lines(interactive, R"(
x ~ symvar
c ~ symconst
)");

        SUBCASE("sum rule with cleanup: d(x + c)/dx = &1")
        {
            // Raw derivative (&1 + &0); the plus-zero rewrite earns its
            // keep immediately.
            interactive.process("(x + c) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-SUMC-" (zelph/exists (zelph/fact (zelph/fact "x" "+" "c") "diffby" "x") "=" (zelph/number "1"))))js");
            CHECK(any_output_contains(collector, "DIFF-SUMC-true"));
        }
        SUBCASE("product rule: d(x * x)/dx = (x + x)")
        {
            interactive.process("(x * x) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-PROD-" (zelph/exists (zelph/fact (zelph/fact "x" "*" "x") "diffby" "x") "=" (zelph/fact "x" "+" "x"))))js");
            CHECK(any_output_contains(collector, "DIFF-PROD-true"));
        }
        SUBCASE("chain rule via hasderivative: d(exp of x)/dx = (exp of x)")
        {
            interactive.process("(exp of x) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-EXP-" (zelph/exists (zelph/fact (zelph/fact "exp" "of" "x") "diffby" "x") "=" (zelph/fact "exp" "of" "x"))))js");
            CHECK(any_output_contains(collector, "DIFF-EXP-true"));
        }
        SUBCASE("dedicated ln rule: d(ln of x)/dx = (&1 / x)")
        {
            interactive.process("(ln of x) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-LN-" (zelph/exists (zelph/fact (zelph/fact "ln" "of" "x") "diffby" "x") "=" (zelph/fact (zelph/number "1") "/" "x"))))js");
            CHECK(any_output_contains(collector, "DIFF-LN-true"));
        } });
}

TEST_CASE("symbolic: coexistence with the numeric substrate (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import diff");
        interactive.process("x ~ symvar");

        // d(x + x)/dx: the raw derivative (&1 + &1) is materialized as an
        // ORDINARY + fact, the numeric module's trigger fires on it, and
        // the M3 bridge folds the result: the exposed derivative is &2,
        // and the raw form never surfaces under =.
        interactive.process("(x + x) diffby x");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(R"js(%(string "DIFF-SUMFOLD-" (zelph/exists (zelph/fact (zelph/fact "x" "+" "x") "diffby" "x") "=" (zelph/number "2"))))js");
        interactive.process(R"js(%(string "DIFF-SUMRAW-" (zelph/exists (zelph/fact (zelph/fact "x" "+" "x") "diffby" "x") "=" (zelph/fact (zelph/number "1") "+" (zelph/number "1")))))js");
        CHECK(any_output_contains(collector, "DIFF-SUMFOLD-true"));
        CHECK(any_output_contains(collector, "DIFF-SUMRAW-false"));

        // The numeric fact the bridge consumed is ordinary graph
        // knowledge, queryable like any computed result.
        collector.clear();
        interactive.process("(&1 + &1) = X");
        CHECK(answers_contain(collector, "(&1 + &1) = &2"));

        // Numeric arithmetic is unaffected by the symbolic modules.
        collector.clear();
        interactive.process("(&12 + &34) = X");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("(&12 + &34) = X");
        CHECK(answers_contain(collector, "(&12 + &34) = &46")); });
}

TEST_CASE("symbolic: knowledge-folding bridge (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process("x ~ symvar");

        SUBCASE("direct folds: (&2 + &3) -> &5, (&17 / &5) -> &3")
        {
            interactive.process(":simplify (&2 + &3)");
            interactive.process("(&17 / &5) simplify (&17 / &5)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "2") "+" (zelph/number "3"))] (string "FOLD-ADD-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "5")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "17") "/" (zelph/number "5"))] (string "FOLD-DIV-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "3")))))js");
            CHECK(any_output_contains(collector, "FOLD-ADD-true"));
            CHECK(any_output_contains(collector, "FOLD-DIV-true"));
        }
        SUBCASE("cascaded fold: ((&2 + &3) * (&4 + &6)) -> &50")
        {
            // The inner sums fold to &5 and &10; congruence then
            // materializes the FRESH numeric fact (&5 * &10) mid-
            // simplification, whose cascade the bridge consumes -- the
            // instantiation-side-effect seeding pinned in test_seminaive.
            interactive.process(":simplify ((&2 + &3) * (&4 + &6))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact (zelph/number "2") "+" (zelph/number "3")) "*" (zelph/fact (zelph/number "4") "+" (zelph/number "6")))] (string "FOLD-CASC-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "50")))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact (zelph/number "2") "+" (zelph/number "3")) "*" (zelph/fact (zelph/number "4") "+" (zelph/number "6")))] (string "FOLD-CASC-NOT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact (zelph/number "5") "*" (zelph/number "10"))))))js");
            CHECK(any_output_contains(collector, "FOLD-CASC-true"));
            CHECK(any_output_contains(collector, "FOLD-CASC-NOT-false"));
        }
        SUBCASE("mixed term: ((x + &0) * (&2 + &3)) -> (x * &5)")
        {
            interactive.process("((x + &0) * (&2 + &3)) simplify ((x + &0) * (&2 + &3))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/fact "x" "+" (zelph/number "0")) "*" (zelph/fact (zelph/number "2") "+" (zelph/number "3")))] (string "FOLD-MIX-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact "x" "*" (zelph/number "5"))))))js");
            CHECK(any_output_contains(collector, "FOLD-MIX-true"));
        }
        SUBCASE("partiality composes: (&5 / &0) is its own normal form")
        {
            // Division by zero derives no = fact, no SR rule matches, the
            // deferred fallback keeps the term -- undefinedness stays
            // visible instead of folding to a wrong value.
            interactive.process("(&5 / &0) simplify (&5 / &0)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "5") "/" (zelph/number "0"))] (string "FOLD-PART-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "5") "/" (zelph/number "0"))] (string "FOLD-PART-NOT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
            CHECK(any_output_contains(collector, "FOLD-PART-true"));
            CHECK(any_output_contains(collector, "FOLD-PART-NOT-false"));
        } 

        SUBCASE("declared equational knowledge folds like computed knowledge")
                {
                    // The bridge consumes ANY = fact about the reduced form: an
                    // equation declared as ordinary knowledge drives simplification
                    // exactly like a computed one -- the knowledge-graph use case.
                    process_lines(interactive, R"(
a ~ symconst
b ~ symconst
c ~ symconst
(a + b) = c
:simplify (a + b)
)");
                    interactive.run(true, false, false);
                    collector.clear();
                    interactive.process(R"js(%(let [t (zelph/fact "a" "+" "b")] (string "FOLD-DECL-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "c")))))js");
                    CHECK(any_output_contains(collector, "FOLD-DECL-true"));
        } });
}

TEST_CASE("symbolic: EML identities and round trips (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import eml");
        interactive.process("x ~ symvar");

        SUBCASE("paper Eq. (5): eml(1, eml(eml(1, x), 1)) simplifies to (ln of x)")
        {
            interactive.process(":simplify (&1 eml ((&1 eml x) eml &1))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "1") "eml" (zelph/fact (zelph/fact (zelph/number "1") "eml" "x") "eml" (zelph/number "1")))] (string "EML-EQ5-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact "ln" "of" "x")))))js");
            CHECK(any_output_contains(collector, "EML-EQ5-true"));
        }
        SUBCASE("exp round trip: compile (exp of x), simplify back")
        {
            interactive.process(":emlcompile (exp of x)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "exp" "of" "x")] (string "EML-CEXP-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (zelph/fact "x" "eml" (zelph/number "1"))))))js");
            CHECK(any_output_contains(collector, "EML-CEXP-true"));

            interactive.process(":simplify (x eml &1)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "eml" (zelph/number "1"))] (string "EML-REXP-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact "exp" "of" "x")))))js");
            CHECK(any_output_contains(collector, "EML-REXP-true"));
        }
        SUBCASE("ln round trip: compile (ln of x) to the Eq.-5 tree, simplify back")
        {
            interactive.process(":emlcompile (ln of x)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "ln" "of" "x") f (zelph/fact (zelph/number "1") "eml" (zelph/fact (zelph/fact (zelph/number "1") "eml" "x") "eml" (zelph/number "1")))] (string "EML-CLN-" (zelph/exists (zelph/fact t "emlcompile" t) "=" f))))js");
            CHECK(any_output_contains(collector, "EML-CLN-true"));

            interactive.process(":simplify (&1 eml ((&1 eml x) eml &1))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [f (zelph/fact (zelph/number "1") "eml" (zelph/fact (zelph/fact (zelph/number "1") "eml" "x") "eml" (zelph/number "1")))] (string "EML-RTLN-" (zelph/exists (zelph/fact f "simplify" f) "=" (zelph/fact "ln" "of" "x")))))js");
            CHECK(any_output_contains(collector, "EML-RTLN-true"));
        }
        SUBCASE("e = eml(1, 1), reduced across two requests (single-pass semantics)")
        {
            interactive.process(":simplify (&1 eml &1)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "1") "eml" (zelph/number "1"))] (string "EML-E1-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact "exp" "of" (zelph/number "1"))))))js");
            CHECK(any_output_contains(collector, "EML-E1-true"));

            interactive.process("(exp of &1) simplify (exp of &1)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "exp" "of" (zelph/number "1"))] (string "EML-E2-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "e")))))js");
            CHECK(any_output_contains(collector, "EML-E2-true"));
        } });
}

TEST_CASE("symbolic: EML compiler macro chain matches eml_compiler_v4 (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import eml");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
)");

        // Expected trees are built by Janet transcriptions of the emit
        // primitives in the paper's eml_compiler_v4.py: ex = eml_exp,
        // lg = eml_log, sub = eml_sub, zero = eml_zero. The probes thus
        // check our rule output against the reference compiler's
        // structure, node for node (hash-consing makes structural
        // equality node identity).
        static const char* helpers =
            R"js(%(defn ex [t] (zelph/fact t "eml" (zelph/number "1"))))js";
        static const char* helpers2 =
            R"js(%(defn lg [t] (zelph/fact (zelph/number "1") "eml" (ex (zelph/fact (zelph/number "1") "eml" t)))))js";
        static const char* helpers3 =
            R"js(%(defn sub [a b] (zelph/fact (lg a) "eml" (ex b))))js";
        static const char* helpers4 =
            R"js(%(def zero (lg (zelph/number "1"))))js";
        interactive.process(helpers);
        interactive.process(helpers2);
        interactive.process(helpers3);
        interactive.process(helpers4);

        SUBCASE("subtraction: eml(ln x, exp y)")
        {
            interactive.process(":emlcompile (x - y)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "-" "y")] (string "EMLC-SUB-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (sub "x" "y")))))js");
            CHECK(any_output_contains(collector, "EMLC-SUB-true"));
        }
        SUBCASE("addition: x - (-y), negation via ln(1) - y")
        {
            interactive.process("(x + y) emlcompile (x + y)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "+" "y")] (string "EMLC-ADD-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (sub "x" (sub zero "y"))))))js");
            CHECK(any_output_contains(collector, "EMLC-ADD-true"));
        }
        SUBCASE("multiplication: exp(ln x + ln y)")
        {
            interactive.process(":emlcompile (x * y)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "*" "y")] (string "EMLC-MUL-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (ex (sub (lg "x") (sub zero (lg "y"))))))))js");
            CHECK(any_output_contains(collector, "EMLC-MUL-true"));
        }
        SUBCASE("division: x * inv(y), inv via exp(-ln y)")
        {
            interactive.process("(x / y) emlcompile (x / y)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "/" "y") iv (ex (sub zero (lg "y")))] (string "EMLC-DIV-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (ex (sub (lg "x") (sub zero (lg iv))))))))js");
            CHECK(any_output_contains(collector, "EMLC-DIV-true"));
        }
        SUBCASE("simplifying the compiled subtraction recovers the Table-S2 witness")
        {
            // The mechanically compiled x - y tree (LeafCount 11) reduces
            // through the exp and ln identity rules to eml(ln x, exp y) --
            // the paper's K = 5 discovery witness (Table S2, step 4),
            // recovered by derivation.
            interactive.process(":emlcompile (x - y)");
            interactive.run(true, false, false);
            interactive.process(R"js(%(let [f (sub "x" "y")] (zelph/fact f "simplify" f)))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [f (sub "x" "y")] (string "EMLC-WITNESS-" (zelph/exists (zelph/fact f "simplify" f) "=" (zelph/fact (zelph/fact "ln" "of" "x") "eml" (zelph/fact "exp" "of" "y"))))))js");
            CHECK(any_output_contains(collector, "EMLC-WITNESS-true"));
        } });
}

// ---------------------------------------------------------------------------
// Self-fact sugar ":pred X" (parser-level desugaring to (X pred X)).
// The probes never mention the sugar: they check ordinary facts that only
// exist if the desugared self-facts were created AND matched correctly --
// at top level, in rule conditions (single and conjunction), and in
// consequences. The display checks pin the inverse direction: self-facts
// render back in the sugar form. The feature is arithmetic-agnostic;
// run_arithmetic_modules is used as the standard harness only.
// ---------------------------------------------------------------------------
TEST_CASE("self-fact sugar: rules, conjunctions, consequences, display")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        // Consequence sugar creates the self-fact (X done X); the second
        // rule's condition sugar must match it -- both directions desugar
        // to the same structure a verbose (X done X) pattern would have.
        process_lines(interactive, R"(
(:seed X) => (:done X)
(:done X) => (X chain ok)
(:seed X, :done X) => (X both ok)
)");
        interactive.process(":seed foo");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(R"js(%(string "SF-CHAIN-" (zelph/exists "foo" "chain" "ok")))js");
        interactive.process(R"js(%(string "SF-BOTH-" (zelph/exists "foo" "both" "ok")))js");
        CHECK(any_output_contains(collector, "SF-CHAIN-true"));
        CHECK(any_output_contains(collector, "SF-BOTH-true"));

        // Manual expansion: the verbose form parses to the SAME
        // (hash-consed) fact -- and its echo is rendered back in sugar
        // form, pinning the display inverse.
        collector.clear();
        interactive.process("foo seed foo");
        CHECK(any_output_contains(collector, ":seed"));

        // Multi-line input: ":pred" alone is incomplete; the operand may
        // arrive on the next line.
        collector.clear();
        interactive.process(":seed");
        interactive.process("bar");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(string "SF-ML-" (zelph/exists "bar" "chain" "ok")))js");
        CHECK(any_output_contains(collector, "SF-ML-true")); });
}

TEST_CASE("symbolic: numeral-times-zero folding is single-valued (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        // Regression pin: before multiplication results were routed
        // through canonnum, a multi-digit numeral times &0 folded (via
        // SN) to the raw zero-extended product node -- e.g. binary <00>,
        // rendered indistinguishably as &0 -- while the SR identity rule
        // rewrote to the canonical &0: two rw facts, two simp results.
        // Both rewrite paths must land on the SAME canonical node.
        interactive.process(".import symbolic-core");
        interactive.process(":simplify (&3 * &0)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(let [t (zelph/fact (zelph/number "3") "*" (zelph/number "0"))] (string "SMUL0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
        interactive.process(R"js(%(let [t (zelph/fact (zelph/number "3") "*" (zelph/number "0")) raw (zelph/fact "0" "cons" (zelph/number "0"))] (string "SMUL0-NOT-" (zelph/exists (zelph/fact t "simplify" t) "=" raw))))js");
        CHECK(any_output_contains(collector, "SMUL0-true"));
        CHECK(any_output_contains(collector, "SMUL0-NOT-false")); });
}

TEST_CASE("symbolic: constant reassociation folds nested numeral factors")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import symbolic-pow");
        interactive.process(".import diff");
        interactive.process("x ~ symvar");

        SUBCASE("(m * (n * u)) folds m and n")
        {
            interactive.process(":simplify (&5 * (&4 * x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "5") "*" (zelph/fact (zelph/number "4") "*" "x"))] (string "SX-FOLD-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact (zelph/number "20") "*" "x")))))js");
            CHECK(any_output_contains(collector, "SX-FOLD-true"));
        }
        SUBCASE("it nests: three factors collapse in one bottom-up pass")
        {
            interactive.process(":simplify (&5 * (&4 * (&3 * x)))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "5") "*" (zelph/fact (zelph/number "4") "*" (zelph/fact (zelph/number "3") "*" "x")))] (string "SX-NEST-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact (zelph/number "60") "*" "x")))))js");
            CHECK(any_output_contains(collector, "SX-NEST-true"));
        }
        SUBCASE("a zero factor keeps the absorbing answer, single-valued")
        {
            // Both the absorbing rule and reassociation match this reduced
            // form. Reassociation would rewrite to (&0 * x), which is not a
            // normal form -- so its guard sits on the product and lets the
            // absorbing rule answer alone. Two rw values would make simp
            // multi-valued, which the second probe pins.
            interactive.process(":simplify (&0 * (&4 * x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "0") "*" (zelph/fact (zelph/number "4") "*" "x"))] (string "SX-ZERO-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")) "-ONLY-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact (zelph/number "0") "*" "x")))))js");
            CHECK(any_output_contains(collector, "SX-ZERO-true-ONLY-false"));
        }
        SUBCASE("a unit factor agrees with the neutral-element rule")
        {
            // K = &1 * n = n, so reassociation and the neutral rule reach
            // the SAME node -- an overlap that is harmless by construction.
            interactive.process(":simplify (&1 * (&4 * x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "1") "*" (zelph/fact (zelph/number "4") "*" "x"))] (string "SX-UNIT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact (zelph/number "4") "*" "x")))))js");
            CHECK(any_output_contains(collector, "SX-UNIT-true"));
        }
        SUBCASE("a symbolic outer factor is left alone")
        {
            interactive.process("y ~ symvar");
            interactive.process(":simplify (y * (&4 * x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [inner (zelph/fact (zelph/number "4") "*" "x") t (zelph/fact "y" "*" inner)] (string "SX-SYM-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            CHECK(any_output_contains(collector, "SX-SYM-true"));
        }
        SUBCASE("iterated differentiation reads as a derivative again")
        {
            interactive.process("(x ^ &5) diffalong <x x x>");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "^" (zelph/number "5"))] (string "SX-D3-" (zelph/exists (zelph/fact t "diffalong" (zelph/list "x" "x" "x")) "=" (zelph/fact (zelph/number "60") "*" (zelph/fact "x" "^" (zelph/number "2")))))))js");
            CHECK(any_output_contains(collector, "SX-D3-true"));
        } });
}

TEST_CASE("symbolic: constant reassociation over Z (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import integer-arithmetic");
        interactive.process(".import symbolic-core");
        interactive.process(".import symbolic-integers");
        interactive.process("x ~ symvar");

        SUBCASE("signed constants fold, sign included")
        {
            interactive.process(":simplify ((neg zint &2) * ((pos zint &3) * x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [zn (fn [s] (zelph/fact "neg" "zint" (zelph/number s))) zp (fn [s] (zelph/fact "pos" "zint" (zelph/number s))) t (zelph/fact (zn "2") "*" (zelph/fact (zp "3") "*" "x"))] (string "ZX-FOLD-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact (zn "6") "*" "x")))))js");
            CHECK(any_output_contains(collector, "ZX-FOLD-true"));
        }
        SUBCASE("the zint zero keeps the absorbing answer, single-valued")
        {
            interactive.process(":simplify ((pos zint &0) * ((pos zint &3) * x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [zp (fn [s] (zelph/fact "pos" "zint" (zelph/number s))) t (zelph/fact (zp "0") "*" (zelph/fact (zp "3") "*" "x"))] (string "ZX-ZERO-" (zelph/exists (zelph/fact t "simplify" t) "=" (zp "0")) "-ONLY-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact (zp "0") "*" "x")))))js");
            CHECK(any_output_contains(collector, "ZX-ZERO-true-ONLY-false"));
        } });
}
