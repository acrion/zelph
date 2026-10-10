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

#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Symbolic mathematics: symbolic-core.zph (M1) and diff.zph (M2)
//
// Two kinds of assertion. In most cases, STRUCTURAL assertions are
// employed through read-only zelph/exists probes, each tagged with
// unique markers, rather than comparing rendered output: symbolic
// terms combine plain atoms (rendered with surrounding spaces) and
// &-literals (rendered without separation), making expected strings
// fragile. zelph/fact within a probe is idempotent -- each node it
// accesses already exists if the pipeline executed correctly -- and
// zelph/exists never generates any new elements. When the NUMBER of
// answers matters (such as in a second normal form or a second raw
// derivative), a case queries the result and reads the Answer: lines
// instead: collect_answers counts them, and answers_contain compares
// one after normalizing whitespace and the self-fact sugar.
//
// Everything runs across all three arithmetic modules: the symbolic
// layer must be numeral-representation-agnostic, since its &0/&1 leaves
// are the loaded module's cons lists.
//
// These tests intentionally mix the explicit syntax `S P S` and the syntax sugar `:P S`.
// ---------------------------------------------------------------------------

TEST_CASE("symbolic: simplification core (all arithmetic modules)" * doctest::test_suite("slow"))
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
        SUBCASE("generic inverse-pair rule: both declared orientations")
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
        SUBCASE("ln 1 = 0 and exp 0 = 1, also where they meet the inverse pair")
        {
            // Both rules must operate in tandem. The expression `ln 1`
            // gets reduced in a bottom-up fashion before the inverse-pair
            // rule is able to process `exp(ln 1)`, meaning that with just
            // `ln 1 = 0` as the sole basis, the request would halt at
            // `(exp of &0)`, where it previously returned `&1` before
            // either rule was established.
            for (const std::string term : {"(ln of &1)", "(exp of &0)", "(exp of (ln of &1))"})
            {
                CAPTURE(term);
                interactive.process(":simplify " + term);
            }
            collector.clear();
            interactive.process("(:simplify (ln of &1)) = X");
            interactive.process("(:simplify (exp of &0)) = X");
            interactive.process("(:simplify (exp of (ln of &1))) = X");
            CHECK(collect_answers(collector).size() == 3);
            CHECK(answers_contain(collector, "(:simplify (ln of &1)) = &0"));
            CHECK(answers_contain(collector, "(:simplify (exp of &0)) = &1"));
            CHECK(answers_contain(collector, "(:simplify (exp of (ln of &1))) = &1"));
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
        SUBCASE("a canonical numeral leaf is its own normal form: (&5 simplify &5) answers &5")
        {
            interactive.process(":simplify &5");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/number "5")] (string "SIMP-NUM-" (zelph/exists (zelph/fact t "simplify" t) "=" t))))js");
            CHECK(any_output_contains(collector, "SIMP-NUM-true"));
        }
        SUBCASE("a numeral with a leading zero simplifies to its canonical form")
        {
            // A cons list leaf constituted its own normal form, as written. The
            // neutral-element rules match &1 by node, and the SN fold returns
            // canonical numerals, thus (<1 0> * &1) simplified to both &1 and
            // <01>, leading rule SU to report a contradiction. The inner zero
            // in <0 1 0> carries value and remains unchanged.
            //
            // (<1 0> + <0 1 0>), displayed as (<01> + <010>), explains why
            // knowledge folding reads `=` facts concerning the REDUCED form
            // exclusively: the arithmetic computes the sum as written, digit
            // by digit, yielding the result <011> with the leading zero
            // preserved. A bridge that also read the term as written would
            // give it that second, non-canonical normal form.
            collector.clear();
            for (const std::string term : {"<1 0>", "<0 0>", "<0 1 0>", "(<1 0> * &1)", "(<1 0> + x)", "(<1 0> + <0 1 0>)"})
            {
                CAPTURE(term);
                interactive.process(":simplify " + term);
            }
            interactive.run(true, false, false);
            CHECK_FALSE(has_contradiction(collector));
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "1" "cons" (zelph/fact "0" "cons" "nil"))] (string "SIMP-LZ1-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "1")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "0" "cons" (zelph/fact "0" "cons" "nil"))] (string "SIMP-LZ0-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "0" "cons" (zelph/fact "1" "cons" (zelph/fact "0" "cons" "nil")))] (string "SIMP-LZIN-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact "0" "cons" (zelph/fact "1" "cons" "nil"))))))js");
            CHECK(any_output_contains(collector, "SIMP-LZ1-true"));
            CHECK(any_output_contains(collector, "SIMP-LZ0-true"));
            CHECK(any_output_contains(collector, "SIMP-LZIN-true"));

            collector.clear();
            interactive.process("(:simplify (<1 0> * &1)) = X");
            interactive.process("(:simplify (<1 0> + x)) = X");
            interactive.process("(:simplify (<1 0> + <0 1 0>)) = X");
            CHECK(collect_answers(collector).size() == 3);
            CHECK(answers_contain(collector, "(:simplify (<01> * &1)) = &1"));
            CHECK(answers_contain(collector, "(:simplify (<01> + x)) = (&1 + x)"));
            CHECK_FALSE(answers_contain(collector, "(:simplify (<01> + <010>)) = <011>"));
        }
        SUBCASE("a list that is not a numeral stays its own normal form")
        {
            // A list that lacks a rewrite falls to the identity fallback.
            // The canonical form strips trailing zero cells from ANY list,
            // transforming <x 0> into <x>, thus requiring the numeral test
            // to come first.
            for (const std::string term : {"<x c>", "(x * <x c>)", "<x 0>"})
            {
                CAPTURE(term);
                interactive.process(":simplify " + term);
            }
            collector.clear();
            interactive.process("(:simplify <x c>) = X");
            interactive.process("(:simplify (x * <x c>)) = X");
            interactive.process("(:simplify <x 0>) = X");
            CHECK(collect_answers(collector).size() == 3);
            CHECK(answers_contain(collector, "(:simplify <x c>) = <x c>"));
            CHECK(answers_contain(collector, "(:simplify (x * <x c>)) = (x * <x c>)"));
            CHECK(answers_contain(collector, "(:simplify <x 0>) = <x 0>"));

            collector.clear();
            interactive.process(R"js(%(string "SIMP-NOCANON-" (zelph/exists (zelph/fact "x" "cons" (zelph/fact "0" "cons" "nil")) "canon" (zelph/fact "x" "cons" "nil"))))js");
            CHECK(any_output_contains(collector, "SIMP-NOCANON-false"));
        } });
}

TEST_CASE("symbolic: differentiation base cases and constancy (all arithmetic modules)" * doctest::test_suite("slow"))
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
        SUBCASE("constant leaves: dc/dx = &0 and dy/dx = &0")
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
            // A constant composite possesses no rule of its own: the
            // product rule assembles its raw derivative from the &0 of its
            // leaves, and the simplifier reduces this to &0. The raw form
            // must never appear beneath =.
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
            // The TRIGGER hands a dstate fact to the entity that was
            // requested. Up to version 1.0.1, constancy was defined as a
            // negation applied to a containment recursion, and a shape that
            // diff knows nothing about failed that test for the trivial
            // reason that no rule could ever have derived it: it was
            // declared constant, yielding a WRONG answer where the stdlib
            // should remain silent. Currently, only leaves are considered
            // constant, each established by a positive rule, and no rule
            // ever accesses a foreign shape.
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
        }
        SUBCASE("a shape outside the vocabulary stays silent INSIDE a known one")
        {
            // The identical gap one level beneath. (exp of (x / y)) and ((x /
            // y) * y) are forms diff recognizes, yet their derivative depends
            // on one it fails to handle -- and the gate previously examined
            // the whole term exclusively. `contains` cannot traverse /, thus
            // neither term was detected as containing x, both passed the
            // gate, and both emerged as constant: d/dx exp(x/y) = 0.
            interactive.process("(exp of (x / y)) diffby x");
            interactive.process("((x / y) * y) diffby x");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "DIFF-NESTDIV-" (zelph/exists (zelph/fact (zelph/fact "exp" "of" (zelph/fact "x" "/" "y")) "diffby" "x") "=" (zelph/number "0"))))js");
            interactive.process(R"js(%(string "DIFF-NESTPROD-" (zelph/exists (zelph/fact (zelph/fact (zelph/fact "x" "/" "y") "*" "y") "diffby" "x") "=" (zelph/number "0"))))js");
            CHECK(any_output_contains(collector, "DIFF-NESTDIV-false"));
            CHECK(any_output_contains(collector, "DIFF-NESTPROD-false"));

            collector.clear();
            interactive.process("((exp of (x / y)) diffby x) = D");
            interactive.process("(((x / y) * y) diffby x) = D");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a term that contains x in a place the rules do not walk is not constant")
        {
            // The identical class of wrong zero, reached without invoking an
            // unknown operator: `contains` never walked the exponent in a
            // power expression, thus treating c^x as constant, leading to d/dx
            // c^x = 0. Similarly, a term featuring an atom with an undeclared
            // sort is not constant either -- the module guarantees silence
            // regarding undeclared sorts -- yet (z * c) answered 0.
            interactive.process(".import symbolic-pow");
            interactive.process("(c ^ x) diffby x");
            interactive.process("(z * c) diffby x");
            collector.clear();
            interactive.process("((c ^ x) diffby x) = D");
            interactive.process("((z * c) diffby x) = D");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a symbol declared both constant and variable is differentiated by as a variable")
        {
            // Sorts are facts, hence no obstacle exists for a symbol to
            // possess two: math.zph's polyring rule stipulates that every
            // ring indeterminate is a symvar, regardless of any other
            // attribute. Differentiated BY such a symbol, the symbol
            // becomes the variable, and a rule asserting every symconst
            // leaf is constant would add a second, wrong derivative.
            interactive.process("a ~ symconst");
            interactive.process("a ~ symvar");
            interactive.process("(a * x) diffby a");
            collector.clear();
            interactive.process("((a * x) diffby a) = D");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "((a * x) diffby a) = x"));
        }
        SUBCASE("a power with exponent zero is differentiated through its base")
        {
            // The derivative &0 was obtained from (u ^ &0) without examining
            // u, the one composite diff still declared constant by its shape:
            // with an undeclared z, a foreign predicate, or a non-numeral list
            // inside it, it answered &0, where a term containing such a part
            // stays silent.
            interactive.process(".import symbolic-pow");
            interactive.process("(x ^ &0) diffby x");
            interactive.process("(z ^ &0) diffby x");
            interactive.process("((x nosuchop y) ^ &0) diffby x");
            collector.clear();
            interactive.process("((x ^ &0) diffby x) = D");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "((x ^ &0) diffby x) = &0"));
            collector.clear();
            interactive.process("((z ^ &0) diffby x) = D");
            interactive.process("(((x nosuchop y) ^ &0) diffby x) = D");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a cons list is differentiated as a number only if it is one")
        {
            // Each list that arrived at the trigger was formerly treated as
            // a numeral leaf, meaning that <x c>, a list comprising two
            // symbols, got the derivative &0. This violated two promises
            // the documentation makes: a foreign shape stays silent, and
            // cons lists maintain their general-purpose nature rather than
            // being read as numbers. A list qualifies as a numeral only if
            // it is nil-terminated and each element is a digit of the
            // substrate.
            interactive.process("&5 diffby x");
            interactive.process("<x c> diffby x");
            interactive.process("(x * <x c>) diffby x");
            collector.clear();
            interactive.process("(&5 diffby x) = D");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(&5 diffby x) = &0"));

            collector.clear();
            interactive.process("(<x c> diffby x) = D");
            interactive.process("((x * <x c>) diffby x) = D");
            interactive.process("(<x c> wrt x) deriv D");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a coefficient with a leading zero gives one derivative")
        {
            // The raw derivative ((&0 * x) + (<1 0> * &1)) undergoes
            // simplification, and while the leaf <1 0> was its own normal form,
            // the neutral-element rule answered <01> and the SN fold answered
            // &1.
            collector.clear();
            interactive.process("(<1 0> * x) diffby x");
            interactive.process("(x * <1 0>) diffby x");
            CHECK_FALSE(has_contradiction(collector));
            collector.clear();
            interactive.process("((<1 0> * x) diffby x) = D");
            interactive.process("((x * <1 0>) diffby x) = D");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "((<01> * x) diffby x) = &1"));
            CHECK(answers_contain(collector, "((x * <01>) diffby x) = &1"));
        } });
}

TEST_CASE("symbolic: a constant product has one raw derivative, however deep (all arithmetic modules)")
{
    // A constant composite used to receive the constancy &0 and every
    // combination the product rule could assemble from its children's
    // derivatives: N(u * v) = 1 + N(u) N(v), thus yielding 5, 26, and 677 raw
    // derivatives for balanced products with 4, 8, and 16 leaves,
    // respectively, and a 32-leaf tree did not complete within minutes --
    // whereas the identical tree over x required only milliseconds. Each one
    // reduced to &0, so the answer remained accurate and only the
    // computational cost grew, doubly exponentially with depth.
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import diff");
        interactive.process("x ~ symvar");
        interactive.process("c ~ symconst");

        const std::function<std::string(const std::string&, int)> tree = [&](const std::string& leaf, int n) -> std::string
        {
            if (n == 1) return leaf;
            return "(" + tree(leaf, n / 2) + " * " + tree(leaf, n - n / 2) + ")";
        };

        for (const int leaves : {4, 8, 16, 32})
        {
            CAPTURE(leaves);
            const std::string t = tree("c", leaves);
            collector.clear();
            interactive.process(t + " diffby x");
            // `diff.zph` implements a contradiction rule regarding
            // the second raw derivative, hence the return of the
            // blow-up would be announced to every user, not merely
            // detected in this location.
            CHECK_FALSE(has_contradiction(collector));
            collector.clear();
            interactive.process("(" + t + " wrt x) deriv D");
            CHECK(collect_answers(collector).size() == 1);

            collector.clear();
            interactive.process("(" + t + " diffby x) = D");
            const std::vector<std::string> result = collect_answers(collector);
            REQUIRE(result.size() == 1);
            CHECK(result.front().ends_with("= &0"));
        } });
}

TEST_CASE("symbolic: iterated differentiation along a list (all arithmetic modules)" * doctest::test_suite("slow"))
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

TEST_CASE("symbolic: differentiation structural rules (all arithmetic modules)" * doctest::test_suite("slow"))
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
        }
        SUBCASE("a second raw derivative is reported (rule DU)")
        {
            // hasderivative serves as the documented method by which a
            // user can extend diff, meaning that declaring a second
            // derivative for exp constitutes a state a user attains,
            // rather than one imposed on the engine: the shipped
            // `exp hasderivative exp` and the declared version together
            // form two different raw derivatives of (exp of x). All other
            // tests assert the ABSENCE of a contradiction, which holds
            // regardless of whether DU is present; this particular test is
            // the one that fails when DU is absent.
            interactive.process("exp hasderivative foo");
            collector.clear();
            interactive.process("(exp of x) diffby x");
            CHECK(has_contradiction(collector));
            bool reported = false;
            for (const auto& e : collector.events())
            {
                std::istringstream lines(e.text);
                for (std::string line; std::getline(lines, line);)
                {
                    line = normalize(line);
                    if (line.starts_with("!") && line.find("deriv ((exp of x) * &1)") != std::string::npos
                        && line.find("deriv ((foo of x) * &1)") != std::string::npos)
                        reported = true;
                }
            }
            CHECK(reported);
        } });
}

TEST_CASE("symbolic: coexistence with the numeric substrate (all arithmetic modules)" * doctest::test_suite("slow"))
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
        CHECK(answers_contain(collector, "(&12 + &34) = &46"));

        // The opposite direction: numeric triggers fire even on a symbolic
        // fact, and when a numeral divisor is present, the division walks
        // the table of that divisor's whole multiples. All of it is true,
        // yet none of it arrives at `=`: a symbolic operand never yields a
        // numeric result, thus the term remains its own normal form.
        interactive.process(":simplify (x / &123)");
        collector.clear();
        interactive.process("(:simplify (x / &123)) = X");
        CHECK(collect_answers(collector).size() == 1);
        CHECK(answers_contain(collector, "(:simplify (x / &123)) = (x / &123)"));
        collector.clear();
        interactive.process("(x / &123) = X");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("symbolic: knowledge-folding bridge (all arithmetic modules)" * doctest::test_suite("slow"))
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
            // No rewrite applies to the reduced form (x * &5), so the
            // identity fallback answers it: the fallback takes on the
            // REDUCED form, which in this case differs from the term as
            // written.
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
                    // The bridge consumes ANY = fact concerning the reduced form: an
                    // equation declared as ordinary knowledge drives simplification in
                    // the same manner as a computed one, as long as it refers to the
                    // reduced form, as (a + b) is here -- the knowledge-graph use case.
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
        }

        SUBCASE("a declaration arriving after the request is reported, not absorbed")
        {
            // The reply stemmed from the identity fallback, grounded in the
            // ABSENCE of a rewrite. Following this, the declaration provides
            // one, and the engine never retracts the original answer, because
            // rules only add facts: (a + b) answers both itself and c.
            // No factor can halt this process unless facts are removed --
            // however, check mode must not remain silent about it, and up to
            // version 1.0.1 it did, as both evaluation strategies derive the
            // same two answers.
            process_lines(interactive, R"(
a ~ symconst
b ~ symconst
c ~ symconst
:simplify (a + b)
)");
            interactive.run(true, false, false);
            collector.clear();
            CHECK_THROWS_WITH_AS(interactive.process("(a + b) = c"), doctest::Contains("negated premise"), std::runtime_error);
            // In non-check mode, the user receives notification too:
            // symbolic-core implements a contradiction rule for a second
            // `simp` of the identical term.
            CHECK(has_contradiction(collector));
            CHECK(any_output_contains(collector, "((a + b) simp c)"));
        }

        SUBCASE("a declaration that overlaps a rewrite rule is reported")
        {
            // By the neutral-element rule, (x * &1) transforms into x, yet the
            // declaration says it equals z. Since both assertions are valid,
            // the request yields the answers x AND z. No negation is present
            // -- thus the fallback does not activate -- making this not a
            // question of evaluation sequence but rather a commitment defined
            // by symbolic-core: simp is single-valued. symbolic-core presents
            // this as a contradiction rule, so the user sees both answers
            // named, in every evaluation mode.
            process_lines(interactive, R"(
z ~ symconst
(x * &1) = z
)");
            collector.clear();
            interactive.process(":simplify (x * &1)");
            CHECK(has_contradiction(collector));
            CHECK(any_output_contains(collector, "((x * &1) simp x)"));
            CHECK(any_output_contains(collector, "((x * &1) simp z)"));
        }

        SUBCASE("a rule that honours the normal-form contract but overlaps a shipped rewrite is reported")
        {
            // The contract consists of two parts, and this rule meets only
            // the first one: its right-hand side &1 represents a leaf, thus
            // constituting a normal form, yet on input (&0 / &0) it
            // overlaps the shipped rule (&0 / X) -> &0 with a different
            // outcome. Introducing a rewrite is precisely what the contract
            // is written for, so this situation is one a rule author
            // arrives at. The tutorial on terms uses this example.
            interactive.process("(T red (X / X)) => (T rw &1)");
            collector.clear();
            interactive.process(":simplify (&0 / &0)");
            CHECK(has_contradiction(collector));
            CHECK(any_output_contains(collector, "((&0 / &0) simp &0)"));
            CHECK(any_output_contains(collector, "((&0 / &0) simp &1)"));
        }

        SUBCASE("a declaration about a term that is not its own reduced form is not consulted")
        {
            // Knowledge folding reads `=` facts concerning the REDUCED form
            // exclusively. Before the bridge's inspection, ((x + &0) * &2)
            // reduces to (x * &2), meaning the declaration regarding the
            // term as originally written is never read -- silently, without
            // generating a second answer or triggering a contradiction. The
            // identical fact, expressed in terms of the reduced form, namely
            // (x * &3) = d, is consulted. All declarations occur before any
            // requests, hence the sequence between declaration and request
            // plays no part.
            process_lines(interactive, R"(
c ~ symconst
d ~ symconst
((x + &0) * &2) = c
(x * &3) = d
)");
            collector.clear();
            interactive.process(":simplify ((x + &0) * &2)");
            interactive.process(":simplify ((x + &0) * &3)");
            CHECK_FALSE(has_contradiction(collector));
            collector.clear();
            interactive.process("(:simplify ((x + &0) * &2)) = X");
            interactive.process("(:simplify ((x + &0) * &3)) = X");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "(:simplify ((x + &0) * &2)) = (x * &2)"));
            CHECK(answers_contain(collector, "(:simplify ((x + &0) * &3)) = d"));
        } });
}

TEST_CASE("symbolic: EML identities and round trips (all arithmetic modules)" * doctest::test_suite("slow"))
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
        SUBCASE("e = eml(1, 1) in one request, and e is defined as exp(1)")
        {
            // Up to version 1.0.1, eml(1, 1) stopped at (exp of &1),
            // requiring a second request on that outcome to reach e. When
            // simplified according to its definition, eml(1, 1) equals
            // exp(1) - ln 1, and the rewrite that defines e applies
            // within the same request.
            interactive.process(":simplify (&1 eml &1)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact (zelph/number "1") "eml" (zelph/number "1"))] (string "EML-E1-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "e")))))js");
            CHECK(any_output_contains(collector, "EML-E1-true"));

            interactive.process("(exp of &1) simplify (exp of &1)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "exp" "of" (zelph/number "1"))] (string "EML-E2-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "e")))))js");
            CHECK(any_output_contains(collector, "EML-E2-true"));
        } });
}

TEST_CASE("symbolic: eml is simplified by its definition (all arithmetic modules)" * doctest::test_suite("slow"))
{
    // eml(x, y) = exp(x) - ln(y) is DEFINED, not conveyed through an identity
    // table. Every scenario listed here stems from the definition, unforeseen
    // by any eml identity table: the constant e appearing in a single
    // request, the zero value of ln(1) in its pure EML form,
    // subtraction recovered from its own witness, and the compiler's
    // subtraction macro checked by simplifying its output back to the
    // original input. Each request must yield exactly one answer.
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import eml");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
)");

        const auto single = [&](const std::string& term, const std::string& marker, const std::string& probe)
        {
            CAPTURE(term);
            interactive.process(":simplify " + term);
            collector.clear();
            interactive.process("(:simplify " + term + ") = X");
            CHECK(collect_answers(collector).size() == 1);
            collector.clear();
            interactive.process(probe);
            CHECK(any_output_contains(collector, marker + "-true"));
        };

        SUBCASE("e = eml(1, 1) in one request")
        {
            single("(&1 eml &1)", "EMLD-E",
                   R"js(%(let [t (zelph/fact (zelph/number "1") "eml" (zelph/number "1"))] (string "EMLD-E-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/resolve "e")))))js");
        }
        SUBCASE("ln(1) in pure EML form is zero")
        {
            single("(&1 eml ((&1 eml &1) eml &1))", "EMLD-ZERO",
                   R"js(%(let [t (zelph/fact (zelph/number "1") "eml" (zelph/fact (zelph/fact (zelph/number "1") "eml" (zelph/number "1")) "eml" (zelph/number "1")))] (string "EMLD-ZERO-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")))))js");
        }
        SUBCASE("paper Eq. (5) follows from the definition")
        {
            single("(&1 eml ((&1 eml x) eml &1))", "EMLD-EQ5",
                   R"js(%(let [t (zelph/fact (zelph/number "1") "eml" (zelph/fact (zelph/fact (zelph/number "1") "eml" "x") "eml" (zelph/number "1")))] (string "EMLD-EQ5-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact "ln" "of" "x")))))js");
        }
        SUBCASE("ln(e) is one, however e was reached")
        {
            // `e` is DEFINED as `exp(1)`, and congruence applies this
            // definition to the inner expression (exp of &1) before the
            // outer inverse pair is able to process it -- thus,
            // `ln(exp(1))` arrived at (ln of e) and halted at that point.
            single("(ln of (exp of &1))", "EMLD-LNE",
                   R"js(%(let [t (zelph/fact "ln" "of" (zelph/fact "exp" "of" (zelph/number "1")))] (string "EMLD-LNE-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "1")))))js");
        }
        SUBCASE("the subtraction witness eml(ln x, exp y) is x - y")
        {
            single("((ln of x) eml (exp of y))", "EMLD-WIT",
                   R"js(%(let [t (zelph/fact (zelph/fact "ln" "of" "x") "eml" (zelph/fact "exp" "of" "y"))] (string "EMLD-WIT-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact "x" "-" "y")))))js");
        }
        SUBCASE("compiling x - y and simplifying the result gives x - y back")
        {
            interactive.process(":emlcompile (x - y)");
            interactive.run(true, false, false);
            interactive.process(R"js(%(defn lg [t] (zelph/fact (zelph/number "1") "eml" (zelph/fact (zelph/fact (zelph/number "1") "eml" t) "eml" (zelph/number "1")))))js");
            interactive.process(R"js(%(def compiled (zelph/fact (lg "x") "eml" (zelph/fact "y" "eml" (zelph/number "1")))))js");
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "x" "-" "y")] (string "EMLD-COMPILED-" (zelph/exists (zelph/fact t "emlcompile" t) "=" compiled))))js");
            CHECK(any_output_contains(collector, "EMLD-COMPILED-true"));

            interactive.process(R"js(%(zelph/fact compiled "simplify" compiled))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(string "EMLD-BACK-" (zelph/exists (zelph/fact compiled "simplify" compiled) "=" (zelph/fact "x" "-" "y"))))js");
            CHECK(any_output_contains(collector, "EMLD-BACK-true"));
        }
        SUBCASE("e - e is &0")
        {
            // The zero eml(1, eml(eml(1, 1), 1)) above reaches e - e on its
            // way; the constant vanishes just as any other term does.
            interactive.process(":simplify (e - e)");
            collector.clear();
            interactive.process("(:simplify (e - e)) = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify (e - e)) = &0"));
        }
        SUBCASE("compiling + * / neg inv and simplifying the result: what comes back")
        {
            // The compiler adheres to the macro chain defined by the
            // reference compiler, and its negation template is expressed as
            // ln(1) - z. Since ln 1 simplifies to &0, and 0 - y yields no
            // value within the naturals, the result returned is the input,
            // represented using that subtraction: identical in value, but
            // distinct in form. The file mkdocs/docs/math/eml.md details
            // these exact forms.
            const auto after = [](const std::string& answer)
            { return answer.substr(answer.rfind(" = ") + 3); };
            const auto round_trip = [&](const std::string& term) -> std::string
            {
                CAPTURE(term);
                interactive.process(":emlcompile " + term);
                collector.clear();
                interactive.process("(:emlcompile " + term + ") = F");
                const std::vector<std::string> compiled = collect_answers(collector);
                REQUIRE(compiled.size() == 1);
                const std::string form = after(compiled.front());
                interactive.process(":simplify " + form);
                collector.clear();
                interactive.process("(:simplify " + form + ") = X");
                const std::vector<std::string> back = collect_answers(collector);
                REQUIRE(back.size() == 1);
                return after(back.front());
            };
            CHECK(round_trip("(x + y)") == "(x - (&0 - y))");
            CHECK(round_trip("(x * y)") == "(exp of ((ln of x) - (&0 - (ln of y))))");
            CHECK(round_trip("(x / y)") == "(exp of ((ln of x) - (ln of y)))");
            CHECK(round_trip("(neg of x)") == "(&0 - x)");
            CHECK(round_trip("(inv of x)") == "(exp of (&0 - (ln of x)))");
        } });
}

TEST_CASE("symbolic: every small eml tree simplifies to exactly one answer (all arithmetic modules)" * doctest::test_suite("slow"))
{
    // Simplified by its definition, each eml term traverses exp, ln, and -,
    // along with their identities and the definition of e, in a single
    // bottom-up traversal. If two of these rules were to overlap and yield
    // distinct outcomes, that would show here as a second answer and as a
    // contradiction, because symbolic-core states that simp is single-valued.
    // The sweep covers every tree with depth at most two, over the leaves 1
    // and x.
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import eml");
        interactive.process("x ~ symvar");

        const std::vector<std::string> leaves{"&1", "x"};
        std::vector<std::string>       pool = leaves;
        std::vector<std::string>       terms;
        for (const auto& a : leaves)
            for (const auto& b : leaves)
                terms.push_back("(" + a + " eml " + b + ")");
        pool.insert(pool.end(), terms.begin(), terms.end());
        for (std::size_t i = 0; i < pool.size(); ++i)
            for (std::size_t j = 0; j < pool.size(); ++j)
                if (i >= leaves.size() || j >= leaves.size())
                    terms.push_back("(" + pool[i] + " eml " + pool[j] + ")");
        REQUIRE(terms.size() == 36);

        collector.clear();
        for (const auto& t : terms)
            interactive.process(":simplify " + t);
        CHECK_FALSE(has_contradiction(collector));

        for (const auto& t : terms)
        {
            CAPTURE(t);
            collector.clear();
            interactive.process("(:simplify " + t + ") = X");
            CHECK(collect_answers(collector).size() == 1);
        } });
}

TEST_CASE("symbolic: EML compiler macro chain matches eml_compiler_v4 (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import symbolic-core");
        interactive.process(".import eml");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
)");

        // Expected trees are constructed via Janet transcriptions of the
        // emit primitives detailed in the paper's eml_compiler_v4.py:
        // ex = eml_exp, lg = eml_log, sub = eml_sub, zero = eml_zero. The
        // probes consequently validate our rule output by comparing it,
        // node for node, to the structure generated by the emit primitives
        // (hash-consing ensures that structural equality equates to node
        // identity). The reference PROGRAM first rewrites its input using
        // SymPy, causing its output to be more extensive for -, neg, inv,
        // and /; the last subcases pin the points where the two meet and
        // where they part.
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
        SUBCASE("simplifying the compiled subtraction gives x - y back")
        {
            // The x - y tree, mechanically compiled (LeafCount 11), appears
            // in pure EML form as eml(ln x, exp y), serving as the paper's
            // K = 5 discovery witness (Table S2, step 4). Up to version 1.0.1,
            // the identity rules reduced it to that witness and ceased
            // further processing; when simplified via the definition of eml,
            // it reverts to the original input.
            interactive.process(":emlcompile (x - y)");
            interactive.run(true, false, false);
            interactive.process(R"js(%(let [f (sub "x" "y")] (zelph/fact f "simplify" f)))js");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [f (sub "x" "y")] (string "EMLC-WITNESS-" (zelph/exists (zelph/fact f "simplify" f) "=" (zelph/fact "x" "-" "y")))))js");
            CHECK(any_output_contains(collector, "EMLC-WITNESS-true"));
        }
        SUBCASE("the constant e compiles to eml(1, 1), as eml_const_E")
        {
            // eml.zph sets e equal to exp(1), and the reference compiler
            // emits E as eml_exp(1) (eml_const_E). Previously, e compiled
            // into an opaque leaf, similar to a declared constant, meaning
            // an input whose only numeral is 1 could still compile to a
            // tree containing an e leaf -- not pure EML. A constant
            // declared by the user stays a leaf, just as a SymPy Symbol
            // does in the reference compiler.
            interactive.process("c ~ symconst");
            interactive.process(":emlcompile e");
            interactive.process(":emlcompile (exp of c)");
            interactive.process(":emlcompile (x - e)");
            collector.clear();
            interactive.process(R"js(%(string "EMLC-E-" (zelph/exists (zelph/fact "e" "emlcompile" "e") "=" (ex (zelph/number "1")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "exp" "of" "c")] (string "EMLC-CONST-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (ex "c")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "x" "-" "e")] (string "EMLC-SUBE-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (sub "x" (ex (zelph/number "1")))))))js");
            CHECK(any_output_contains(collector, "EMLC-E-true"));
            CHECK(any_output_contains(collector, "EMLC-CONST-true"));
            CHECK(any_output_contains(collector, "EMLC-SUBE-true"));
            collector.clear();
            interactive.process("(:emlcompile e) = F");
            CHECK(collect_answers(collector).size() == 1);
        }
        SUBCASE("negation and reciprocal: the trees of eml_neg and eml_inv")
        {
            interactive.process(":emlcompile (neg of x)");
            interactive.process(":emlcompile (inv of x)");
            collector.clear();
            interactive.process(R"js(%(let [t (zelph/fact "neg" "of" "x")] (string "EMLC-NEG-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (sub zero "x")))))js");
            interactive.process(R"js(%(let [t (zelph/fact "inv" "of" "x")] (string "EMLC-INV-" (zelph/exists (zelph/fact t "emlcompile" t) "=" (ex (sub zero (lg "x")))))))js");
            CHECK(any_output_contains(collector, "EMLC-NEG-true"));
            CHECK(any_output_contains(collector, "EMLC-INV-true"));
        }
        SUBCASE("the reference program's x - y is the tree of x + (-1 * y), not of x - y")
        {
            // SymPy represents x - y as x + (-1)*y, meaning the
            // reference program compiles that sum (LeafCount 83, eml.md
            // "Compile, then simplify"). When expressed as this sum
            // here, it results in an identical tree upon compilation,
            // and this tree differs from the 11-leaf tree that (x - y)
            // compiles to: the compilers diverge based on the operator
            // as written, not on the underlying primitives.
            interactive.process(":emlcompile (x + ((neg of &1) * y))");
            interactive.process(":emlcompile (x - y)");
            collector.clear();
            interactive.process(R"js(%(def sympy-sub (sub "x" (sub zero (ex (sub (lg (sub zero (zelph/number "1"))) (sub zero (lg "y"))))))))js");
            interactive.process(R"js(%(let [t (zelph/fact "x" "+" (zelph/fact (zelph/fact "neg" "of" (zelph/number "1")) "*" "y"))] (string "EMLC-SYMPY-" (zelph/exists (zelph/fact t "emlcompile" t) "=" sympy-sub))))js");
            interactive.process(R"js(%(let [t (zelph/fact "x" "-" "y")] (string "EMLC-SYMPY-SUB-" (zelph/exists (zelph/fact t "emlcompile" t) "=" sympy-sub))))js");
            CHECK(any_output_contains(collector, "EMLC-SYMPY-true"));
            CHECK(any_output_contains(collector, "EMLC-SYMPY-SUB-false"));
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
TEST_CASE("self-fact sugar: rules, conjunctions, consequences, display" * doctest::test_suite("slow"))
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

TEST_CASE("symbolic: constant reassociation folds nested numeral factors" * doctest::test_suite("slow"))
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
        SUBCASE("the folded product is a normal form: simplifying it again changes nothing")
        {
            // The normal-form contract requires each rewrite to produce a
            // normal form, and SX fulfils this by introducing a new node
            // (K * U) that no rule rewrites. This remains valid as long as
            // no declaration pertains to (K * U): such a declaration is
            // consulted for (K * U) itself, yet not for a term that SX
            // rewrote into it (symbolic.md, "Constant Reassociation"),
            // hence this case involves no declaration.
            interactive.process(":simplify (&5 * (&4 * x))");
            interactive.process(":simplify (&20 * x)");
            collector.clear();
            interactive.process("(:simplify (&5 * (&4 * x))) = X");
            interactive.process("(:simplify (&20 * x)) = X");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "(:simplify (&5 * (&4 * x))) = (&20 * x)"));
            CHECK(answers_contain(collector, "(:simplify (&20 * x)) = (&20 * x)"));
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

TEST_CASE("symbolic: constant reassociation over Z (all arithmetic modules)" * doctest::test_suite("slow"))
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
        SUBCASE("a natural outer factor folds into a negative inner one")
        {
            // Pins the productive natural-times-negative rule of ZX. The
            // case above reaches the negative-times-natural rule
            // exclusively (the explicit (pos zint &3) is the natural &3 at
            // the leaf), thus, without this case, the rule might be
            // removed without detection.
            interactive.process(":simplify (&3 * ((neg zint &2) * x))");
            collector.clear();
            interactive.process("(:simplify (&3 * ((neg zint &2) * x))) = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify (&3 * ((neg zint &2) * x))) = ((neg zint &6) * x)"));
        }
        SUBCASE("two negative factors fold into a natural one")
        {
            // Pins the productive negative-times-negative rule of ZX, the
            // one guarded by K != &1; the case involving two (-1) factors
            // below only reaches the adjacent special rule.
            interactive.process(":simplify ((neg zint &2) * ((neg zint &3) * x))");
            collector.clear();
            interactive.process("(:simplify ((neg zint &2) * ((neg zint &3) * x))) = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify ((neg zint &2) * ((neg zint &3) * x))) = (&6 * x)"));
        }
        SUBCASE("a folded signed product is a normal form: simplifying it again changes nothing")
        {
            // The counterpart to the SX case: ZX's new node (K * U)
            // remains in normal form provided that no declaration has
            // been made regarding it.
            interactive.process(":simplify ((neg zint &2) * (&3 * x))");
            interactive.process(":simplify ((neg zint &6) * x)");
            collector.clear();
            interactive.process("(:simplify ((neg zint &2) * (&3 * x))) = X");
            interactive.process("(:simplify ((neg zint &6) * x)) = X");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "(:simplify ((neg zint &2) * (&3 * x))) = ((neg zint &6) * x)"));
            CHECK(answers_contain(collector, "(:simplify ((neg zint &6) * x)) = ((neg zint &6) * x)"));
        }
        SUBCASE("an explicit zint zero keeps the absorbing answer, single-valued")
        {
            // At the leaf, (pos zint &0) represents the natural &0, thus
            // triggering the absorbing rule to provide an answer, and
            // neither (&0 * x) nor the zint zero appears.
            interactive.process(":simplify ((pos zint &0) * ((pos zint &3) * x))");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(R"js(%(let [zp (fn [s] (zelph/fact "pos" "zint" (zelph/number s))) t (zelph/fact (zp "0") "*" (zelph/fact (zp "3") "*" "x"))] (string "ZX-ZERO-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/number "0")) "-ONLY-" (zelph/exists (zelph/fact t "simplify" t) "=" (zelph/fact (zelph/number "0") "*" "x")) "-" (zelph/exists (zelph/fact t "simplify" t) "=" (zp "0")))))js");
            CHECK(any_output_contains(collector, "ZX-ZERO-true-ONLY-false-false"));
        }
        SUBCASE("two signs that cancel leave the other factor, not a unit product")
        {
            // Multiplying (-1) by ((-1) * x) folds the two constants into +1,
            // and the rule used to build ((pos zint &1) * x) from this -- a
            // product involving a neutral factor, which fails to be in normal
            // form: the contract that every rewrite must uphold, and which a
            // later request on the RESULT would have reduced further.
            interactive.process(":simplify ((neg zint &1) * ((neg zint &1) * x))");
            collector.clear();
            interactive.process("(:simplify ((neg zint &1) * ((neg zint &1) * x))) = X");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "(:simplify ((neg zint &1) * ((neg zint &1) * x))) = x"));
        } });
}
