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
// Demanding mathematics, as an end-to-end exercise of the standard library.
//
// These are theorems rather than calculations, and each one asks the engine
// for something the smaller examples do not: full expansion and cancellation
// of a multivariate polynomial to zero, symmetric-function identities in
// three indeterminates, and -- the one that pushes hardest -- a proof that
// runs as a PIPELINE OF RULES across two independent subsystems of the
// standard library, differentiation and symbolic equivalence.
//
// The last one is what this file was written for. It found a real defect: a
// rule whose condition names a request pattern made that request unaskable,
// because the pattern belongs to the rule and a rule's ground patterns are
// not claims. The demand-driven subsystems trigger on the CLAIM, so nothing
// ever computed the derivative and the waiting rule never fired -- and the
// same lines in the other order worked. See *asking a question a rule is
// already waiting for* below.
// ---------------------------------------------------------------------------

namespace
{
    // The math stack is the expensive part of these cases; each SUBCASE that
    // needs it pays for it once.
    void math_with(const zelph::console::Interactive& interactive, const char* indeterminates)
    {
        interactive.process(".import math");
        interactive.process(std::string("<") + indeterminates + "> ~ polyring");
    }

    // `? A ≡ B` answers `(A ≡ B) = proven` when the two terms are equal as
    // polynomials. Absence of that answer is how the stack says "not proven".
    bool proves(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& claim)
    {
        collector.clear();
        interactive.process("? " + claim);
        return any_output_contains(collector, "= proven");
    }
}

TEST_CASE("math: Cayley-Hamilton for a symbolic 2x2 matrix")
{
    // Every square matrix satisfies its own characteristic polynomial. For
    //
    //     A = [[a, b], [c, d]],  tr = a + d,  det = ad - bc
    //
    // the theorem says A^2 - tr*A + det*I = 0 -- FOUR polynomial identities in
    // four indeterminates, each of which only vanishes after the products are
    // expanded and everything cancels. Nothing here is a normal form lookup:
    // the stack has to multiply out and then find zero.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    math_with(interactive, "a b c d");

    // (1,1): a² + bc - (a+d)a + (ad - bc)
    CHECK(proves(collector, interactive, "$( a*a + b*c - (a+d)*a + (a*d - b*c) ) ≡ $( 0 )"));
    // (1,2): ab + bd - (a+d)b
    CHECK(proves(collector, interactive, "$( a*b + b*d - (a+d)*b ) ≡ $( 0 )"));
    // (2,1): ca + dc - (a+d)c
    CHECK(proves(collector, interactive, "$( c*a + d*c - (a+d)*c ) ≡ $( 0 )"));
    // (2,2): cb + d² - (a+d)d + (ad - bc)
    CHECK(proves(collector, interactive, "$( c*b + d*d - (a+d)*d + (a*d - b*c) ) ≡ $( 0 )"));

    // The control: a WRONG entry must not be provable. Without it the four
    // checks above would pass on an engine that answers "proven" to
    // everything.
    CHECK_FALSE(proves(collector, interactive, "$( a*a + b*c - (a+d)*a ) ≡ $( 0 )"));
}

TEST_CASE("math: Newton's identities in three indeterminates")
{
    // Power sums from elementary symmetric polynomials:
    //
    //     p1 = e1
    //     p2 = e1*p1 - 2*e2
    //     p3 = e1*p2 - e2*p1 + 3*e3
    //
    // The third one expands to 27 monomials before cancelling down to
    // a³ + b³ + c³, which is a genuine workout for the normalisation.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    math_with(interactive, "a b c");

    CHECK(proves(collector, interactive, "$( (a+b+c)*(a+b+c) - 2*(a*b+a*c+b*c) ) ≡ $( a^2 + b^2 + c^2 )"));

    CHECK(proves(collector, interactive, "$( (a+b+c)*(a^2+b^2+c^2) - (a*b+a*c+b*c)*(a+b+c) + 3*(a*b*c) ) ≡ $( a^3 + b^3 + c^3 )"));

    // Control: the same identity with one coefficient off is not provable.
    CHECK_FALSE(proves(collector, interactive, "$( (a+b+c)*(a^2+b^2+c^2) - (a*b+a*c+b*c)*(a+b+c) + 2*(a*b*c) ) ≡ $( a^3 + b^3 + c^3 )"));
}

TEST_CASE("math: Jacobi's formula proved by a pipeline of rules")
{
    // d/dx det A(x) = tr(adj(A) A'(x)), for
    //
    //     A(x) = [[x, 1], [x², x³]],  det A = x⁴ - x²
    //
    // and this is the point of the case: the proof is not a query the user
    // types but a PIPELINE OF RULES that spans two subsystems of the standard
    // library. Stage one turns a claim into a differentiation REQUEST, stage
    // two turns the result into an equivalence OBLIGATION, and stage three
    // consumes the equivalence proof. Differentiation is demand-driven and
    // equivalence is proof-driven, so the two only meet if a rule can make
    // the first ask what the second needs.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    math_with(interactive, "x");

    process_lines(interactive, R"(
(D jacobi-claim E) => (D diffby x)
(D jacobi-claim E, (D diffby x) = R) => (R ≡ E)
(D jacobi-claim E, (D diffby x) = R, (R ≡ E) = proven) => (D jacobi-verified E)
)");

    // det A on the left, tr(adj(A) A') = d*a' - b*c' - c*b' + a*d' on the
    // right, written out for the entries above: x³*1 - 1*2x - x²*0 + x*3x².
    interactive.process("$( x^4 - x^2 ) jacobi-claim $( (x^3 - 2*x) + 3*x^3 )");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("S jacobi-verified O");
    CHECK(any_output_contains(collector, "jacobi-verified"));

    // The control that makes the pipeline meaningful: a WRONG right-hand side
    // must not come out verified. It runs the same three stages and stops at
    // the equivalence.
    interactive.process("$( x^4 - x^2 ) jacobi-claim $( 4*x^3 )");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("$( x^4 - x^2 ) jacobi-verified O");
    const auto answers = collect_answers(collector);
    CHECK(answers.size() == 1); // the true claim only
}

TEST_CASE("math: asking a question a rule is already waiting for")
{
    // The defect the case above turned up, reduced to three lines.
    //
    // A rule's ground patterns are not claims -- that is what keeps
    // `(a p b) => (c q d)` from making `a p b` true. The demand-driven parts
    // of the standard library trigger on the CLAIM `T diffby x`, so a rule
    // that merely NAMES that request in a condition used to make it
    // unaskable: .explain answered `[rule pattern; not asserted]`, nothing
    // computed the derivative, and the rule waiting for it never fired.
    //
    // The tell was the order dependence: writing the same two lines the other
    // way round worked. Asking IS asserting the statement one asks about, so
    // the `?` prefix now asserts its request.
    SUBCASE("rule first, then the question")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        math_with(interactive, "x");

        interactive.process("(($( x^3 ) diffby x) = Y) => (marker found Y)");
        interactive.process("? $( x^3 ) diffby x");

        collector.clear();
        interactive.process("S found O");
        CHECK(any_output_contains(collector, "marker found"));
    }

    SUBCASE("the question first, then the rule")
    {
        // The order that always worked, kept as the control.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        math_with(interactive, "x");

        interactive.process("? $( x^3 ) diffby x");
        interactive.process("(($( x^3 ) diffby x) = Y) => (marker found Y)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S found O");
        CHECK(any_output_contains(collector, "marker found"));
    }

    SUBCASE("a request that is a TERM is still not asserted")
    {
        // "? (&17 mod &5)" asks for the value of a term, which the statement
        // grammar rejects by design -- only a request that IS a statement is
        // asserted, or this would break every arithmetic one-liner.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".import decimal-arithmetic");

        collector.clear();
        interactive.process("? (&17 mod &5)");
        CHECK(any_output_contains(collector, "= &2"));
    }
}
