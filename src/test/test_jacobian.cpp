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
// End-to-end case study: the July-2026 Jacobian-conjecture counterexample
// (Alpoege/Fable), verified purely over Z.
//
// The original map F : C^3 -> C^3,
//   F1 = (1+xy)^3 z + y^2 (1+xy)(4+3xy)
//   F2 = y + 3x(1+xy)^2 z + 3x y^2 (4+3xy)
//   F3 = 2x - 3x^2 y - x^3 z
// has constant Jacobian determinant -2 yet maps three distinct points
// to one image, refuting the conjecture for n >= 3. Fractions are
// avoided entirely: substituting x=a, y=b/2, z=c/4 and clearing
// denominators, G = (32*F1, 16*F2, 4*F3) has INTEGER coefficients:
//   G1 = (2+ab)^3 c + 2 b^2 (2+ab)(8+3ab)
//   G2 = 8b + 3a(2+ab)^2 c + 6a b^2 (8+3ab)
//   G3 = 8a - (6 a^2 b + a^3 c)
// The collision points become P1=(0,0,-1), P2=(1,-3,26), P3=(-1,3,26),
// all mapping to (-8,0,0); by the chain rule, det J_G =
// det diag(32,16,4) * det J_F * det diag(1,1/2,1/4) = 2048*(-2)/8 =
// -512 -- and conversely, det J_G == -512 as a polynomial identity
// implies det J_F == -2.
//
// This file proves both halves inside zelph:
//  (a) the collision: each G component, EVALUATED at each point,
//      compiles to the same canonical zint node. Evaluation needs no
//      dedicated machinery: the components are built by one Janet
//      constructor parameterized over its leaves -- atoms a,b,c give
//      the symbolic map, zint numerals give the ground terms, and
//      topoly compiles either. Point distinctness is node
//      distinctness of the coordinates (canonical zints of different
//      values are different nodes, pinned by the integer suite).
//  (b) det J_G == -512 as a POLYNOMIAL IDENTITY: the nine partial
//      derivatives come from diff + symbolic-minus, the first-row
//      cofactor expansion is assembled as a symbolic term over them,
//      and topoly compiles it to (neg zint &512) -- node identity as
//      proof, no evaluation involved.
//
// Deliberately DECIMAL-substrate only (run_both_modes): every layer's
// substrate-agnosticism is pinned by its own suite; this is the
// integration case study, and base 10 keeps the large-coefficient
// polynomial arithmetic cheapest. This file is also the blueprint for
// the demos.js playground group.
// ---------------------------------------------------------------------------

namespace
{
    template <typename Interactive>
    void import_jacobian_stack(Interactive& interactive, const bool with_diff)
    {
        interactive.process(".import binary-arithmetic");
        interactive.process(".import integer-arithmetic");
        interactive.process(".import polynomial");
        interactive.process(".import topoly");
        if (with_diff)
        {
            interactive.process(".import symbolic-core");
            interactive.process(".import diff");
            interactive.process(".import symbolic-minus");
        }
        interactive.process(R"js(%(defn t* [u v] (zelph/fact u "*" v)))js");
        interactive.process(R"js(%(defn t+ [u v] (zelph/fact u "+" v)))js");
        interactive.process(R"js(%(defn t- [u v] (zelph/fact u "-" v)))js");
        interactive.process(R"js(%(defn n [s] (zelph/number s)))js");
        interactive.process(R"js(%(defn tp [t] (zelph/fact t "topoly" t)))js");
        // One constructor for the whole map; shared subterms (ab, s, t,
        // squares) are built once and hash-cons across components.
        interactive.process(R"js(%(defn g-terms [a b c] (def ab (t* a b)) (def s (t+ (n "2") ab)) (def t (t+ (n "8") (t* (n "3") ab))) (def s2 (t* s s)) (def b2 (t* b b)) (def a2 (t* a a)) [(t+ (t* (t* s2 s) c) (t* (t* (n "2") b2) (t* s t))) (t+ (t* (n "8") b) (t+ (t* (t* (n "3") a) (t* s2 c)) (t* (t* (n "6") a) (t* b2 t)))) (t- (t* (n "8") a) (t+ (t* (t* (n "6") a2) b) (t* (t* a2 a) c)))]))js");
    }
} // namespace

TEST_CASE("jacobian: three distinct points collide on (-8, 0, 0) (binary substrate)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        import_jacobian_stack(interactive, false);
        interactive.process(R"js(%(def gp1 (g-terms (zelph/int "0") (zelph/int "0") (zelph/int "-1"))))js");
        interactive.process(R"js(%(def gp2 (g-terms (zelph/int "1") (zelph/int "-3") (zelph/int "26"))))js");
        interactive.process(R"js(%(def gp3 (g-terms (zelph/int "-1") (zelph/int "3") (zelph/int "26"))))js");
        interactive.process(R"js(%(each g gp1 (tp g)))js");
        interactive.process(R"js(%(each g gp2 (tp g)))js");
        interactive.process(R"js(%(each g gp3 (tp g)))js");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(string "JB-P1G1-" (zelph/exists (tp (get gp1 0)) "=" (zelph/int "-8"))))js");
        interactive.process(R"js(%(string "JB-P1G2-" (zelph/exists (tp (get gp1 1)) "=" (zelph/int "0"))))js");
        interactive.process(R"js(%(string "JB-P1G3-" (zelph/exists (tp (get gp1 2)) "=" (zelph/int "0"))))js");
        interactive.process(R"js(%(string "JB-P2G1-" (zelph/exists (tp (get gp2 0)) "=" (zelph/int "-8"))))js");
        interactive.process(R"js(%(string "JB-P2G2-" (zelph/exists (tp (get gp2 1)) "=" (zelph/int "0"))))js");
        interactive.process(R"js(%(string "JB-P2G3-" (zelph/exists (tp (get gp2 2)) "=" (zelph/int "0"))))js");
        interactive.process(R"js(%(string "JB-P3G1-" (zelph/exists (tp (get gp3 0)) "=" (zelph/int "-8"))))js");
        interactive.process(R"js(%(string "JB-P3G2-" (zelph/exists (tp (get gp3 1)) "=" (zelph/int "0"))))js");
        interactive.process(R"js(%(string "JB-P3G3-" (zelph/exists (tp (get gp3 2)) "=" (zelph/int "0"))))js");
        interactive.process(R"js(%(string "JB-NOTPOS-" (zelph/exists (tp (get gp1 0)) "=" (zelph/int "8"))))js");
        CHECK(any_output_contains(collector, "JB-P1G1-true"));
        CHECK(any_output_contains(collector, "JB-P1G2-true"));
        CHECK(any_output_contains(collector, "JB-P1G3-true"));
        CHECK(any_output_contains(collector, "JB-P2G1-true"));
        CHECK(any_output_contains(collector, "JB-P2G2-true"));
        CHECK(any_output_contains(collector, "JB-P2G3-true"));
        CHECK(any_output_contains(collector, "JB-P3G1-true"));
        CHECK(any_output_contains(collector, "JB-P3G2-true"));
        CHECK(any_output_contains(collector, "JB-P3G3-true"));
        CHECK(any_output_contains(collector, "JB-NOTPOS-false")); });
}

TEST_CASE("jacobian: det J_G is the constant -512 as a polynomial identity (binary substrate)" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions off");
        import_jacobian_stack(interactive, true);
        process_lines(interactive, R"(
a ~ symvar
b ~ symvar
c ~ symvar
a pouter b
b pouter c
)");
        interactive.process(R"js(%(def gs (g-terms "a" "b" "c")))js");
        interactive.process(R"js(%(each g gs (each v ["a" "b" "c"] (zelph/fact g "diffby" v))))js");
        interactive.run(false, false, false);
        // Read the nine simplified derivatives (diffby results, row i =
        // component G_{i+1}, column j = variable a/b/c), assemble the
        // first-row cofactor expansion
        //   det = D11*M11 - D12*M12 + D13*M13,
        //   M11 = D22*D33 - D23*D32, M12 = D21*D33 - D23*D31,
        //   M13 = D21*D32 - D22*D31,
        // and request its compilation.
        interactive.process(R"js(%(defn dof [g v] (get (get (zelph/query (zelph/fact (zelph/fact g "diffby" v) "=" '_D)) 0) '_D)))js");
        interactive.process(R"js(%(def ds (map (fn [g] (map (fn [v] (dof g v)) ["a" "b" "c"])) gs)))js");
        interactive.process(R"js(%(defn cof [p q r s] (t- (t* p s) (t* q r))))js");
        interactive.process(R"js(%(def m11 (cof (get (get ds 1) 1) (get (get ds 1) 2) (get (get ds 2) 1) (get (get ds 2) 2))))js");
        interactive.process(R"js(%(def m12 (cof (get (get ds 1) 0) (get (get ds 1) 2) (get (get ds 2) 0) (get (get ds 2) 2))))js");
        interactive.process(R"js(%(def m13 (cof (get (get ds 1) 0) (get (get ds 1) 1) (get (get ds 2) 0) (get (get ds 2) 1))))js");
        interactive.process(R"js(%(def det (t+ (t- (t* (get (get ds 0) 0) m11) (t* (get (get ds 0) 1) m12)) (t* (get (get ds 0) 2) m13))))js");
        interactive.process(R"js(%(tp det))js");
        interactive.run(false, false, false);
        collector.clear();
        interactive.process(R"js(%(string "JB-DET-" (zelph/exists (tp det) "=" (zelph/int "-512"))))js");
        interactive.process(R"js(%(string "JB-DET-NOT-" (zelph/exists (tp det) "=" (zelph/int "512"))))js");
        CHECK(any_output_contains(collector, "JB-DET-true"));
        CHECK(any_output_contains(collector, "JB-DET-NOT-false")); });
}

// ---------------------------------------------------------------------------
// The shipped example script. The two cases above build the map through a
// Janet constructor; stdlib/examples/math/jacobian.zph states the same
// determinant half in plain zelph, and the tutorial quotes it verbatim. It
// is a documented, user-facing artifact, so it needs a test of its own --
// otherwise it can rot without anything failing.
//
// Single mode on purpose: the semantics are already pinned by the cases
// above under both schedules; what this adds is that the FILE parses,
// imports and still answers.
// ---------------------------------------------------------------------------
TEST_CASE("jacobian: the shipped example script reproduces det J_G = -512" * doctest::test_suite("slow"))
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".import examples/math/jacobian");
    collector.clear();

    interactive.process(R"js(%(string "JEX-NEG-" (> (length (zelph/query (zelph/fact '_M "jdet" (zelph/fact "neg" "zint" (zelph/number "512"))))) 0)))js");
    interactive.process(R"js(%(string "JEX-POS-" (> (length (zelph/query (zelph/fact '_M "jdet" (zelph/fact "pos" "zint" (zelph/number "512"))))) 0)))js");
    CHECK(any_output_contains(collector, "JEX-NEG-true"));
    CHECK(any_output_contains(collector, "JEX-POS-false"));
}
