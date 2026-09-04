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

#include <filesystem>

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

TEST_CASE("math: Cayley-Hamilton for a symbolic 2x2 matrix" * doctest::test_suite("slow"))
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

TEST_CASE("math: Jacobi's formula proved by a pipeline of rules" * doctest::test_suite("slow"))
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

TEST_CASE("math: asking a question a rule is already waiting for" * doctest::test_suite("slow"))
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

TEST_CASE("math: Jacobi's formula assembled from the matrix entries" * doctest::test_suite("slow"))
{
    // The full construction, and the most demanding thing in this suite: the
    // rules build BOTH sides out of the matrix entries rather than being
    // handed them.
    //
    //   * the determinant is composed as `a*d - b*c` from four entries;
    //   * five differentiation REQUESTS are derived (the determinant and each
    //     entry);
    //   * the trace of adj(A) A' is composed as `d*a' - b*c' - c*b' + a*d'`
    //     from the entries AND their derivatives;
    //   * the two composed terms are proved equivalent, and only then is the
    //     conclusion drawn.
    //
    // Seventeen rules across three mechanisms that do not otherwise meet:
    // composing a symbolic term inside a rule consequence, demand-driven
    // differentiation, and proof-driven equivalence. Run over two different
    // matrices so the pipeline is not fitted to one example.
    const char* pipeline = R"(
(M ea A, M ed D) => (M ad (A * D))
(M eb B, M ec C) => (M bc (B * C))
(M ad P, M bc Q) => (M det (P - Q))

(M det T) => (T diffby x)
(M ea T) => (T diffby x)
(M eb T) => (T diffby x)
(M ec T) => (T diffby x)
(M ed T) => (T diffby x)

(M ed D, M ea A, (A diffby x) = _DA) => (M t1 (D * _DA))
(M eb B, M ec C, (C diffby x) = _DC) => (M t2 (B * _DC))
(M ec C, M eb B, (B diffby x) = _DB) => (M t3 (C * _DB))
(M ea A, M ed D, (D diffby x) = _DD) => (M t4 (A * _DD))
(M t1 P, M t2 Q) => (M s1 (P - Q))
(M s1 P, M t3 Q) => (M s2 (P - Q))
(M s2 P, M t4 Q) => (M trace (P + Q))

(M det T, (T diffby x) = L, M trace R) => (L ≡ R)
(M det T, (T diffby x) = L, M trace R, (L ≡ R) = proven) => (M jacobi holds)
)";

    SUBCASE("A = [[x, 1], [x^2, x^3]]")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        math_with(interactive, "x");
        process_lines(interactive, pipeline);
        process_lines(interactive, R"(
m ea $( x )
m eb $( 1 )
m ec $( x^2 )
m ed $( x^3 )
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S jacobi O");
        CHECK(answers_contain(collector, "m jacobi holds"));
    }

    SUBCASE("A = [[x^2, x], [1, x^4]]")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        math_with(interactive, "x");
        process_lines(interactive, pipeline);
        process_lines(interactive, R"(
m ea $( x^2 )
m eb $( x )
m ec $( 1 )
m ed $( x^4 )
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S jacobi O");
        CHECK(answers_contain(collector, "m jacobi holds"));
    }

    SUBCASE("the equivalence is what gates the conclusion")
    {
        // The same pipeline with the last stage asked about a term that is
        // NOT the trace: everything up to the equivalence runs, and the
        // conclusion does not follow. Without this the case would pass on an
        // engine that concludes regardless of the proof.
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        math_with(interactive, "x");
        process_lines(interactive, pipeline);
        interactive.process("(M det T, (T diffby x) = L, M ea R, (L ≡ R) = proven) => (M bogus holds)");
        process_lines(interactive, R"(
m ea $( x )
m eb $( 1 )
m ec $( x^2 )
m ed $( x^3 )
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S bogus O");
        CHECK(collect_answers(collector).empty());

        // ... while the real conclusion is there, so the pipeline did run.
        collector.clear();
        interactive.process("S jacobi O");
        CHECK(answers_contain(collector, "m jacobi holds"));
    }
}

TEST_CASE("math: a multi-letter variable needs its underscore" * doctest::test_suite("slow"))
{
    // The trap this file was written around, and it cost an hour: variables
    // are single uppercase letters or identifiers starting with `_`. `DA` is
    // therefore an ordinary NAME, and a rule using it as if it were a
    // variable asks for a specific node instead of binding anything -- so it
    // never fires, silently, and looks exactly like an engine that is not
    // matching.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    math_with(interactive, "x");

    interactive.process("(M ea A) => (A diffby x)");
    interactive.process("(M ea A, (A diffby x) = DA) => (M got DA)");
    interactive.process("m ea $( x^3 )");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("S got O");
    CHECK(collect_answers(collector).empty());

    // `DA` is a name, which is what makes the rule inert.
    collector.clear();
    interactive.process(".node DA");
    CHECK(any_output_contains(collector, "Variable: no"));

    // The same rule with the underscore fires.
    interactive.process("(M ea A, (A diffby x) = _DA) => (M ok _DA)");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("S ok O");
    CHECK_FALSE(collect_answers(collector).empty());
}

// ---------------------------------------------------------------------------
// Cross-subsystem combinations. Each of these puts two or three mechanisms
// together that no other test exercises in the same network: negation-as-
// failure over a demand-driven layer, a rule GENERATOR driving that layer,
// a cluster rolling back what both of them produced, mathematics reporting a
// contradiction, and a computation suspended by .save and resumed by .load.
//
// They pass. They are here because the ways they could fail are silent ones:
// a stratification that evaluates the negation too early would refute a true
// identity, and a rollback that misses derived structure would leave a graph
// nobody can account for.
// ---------------------------------------------------------------------------

TEST_CASE("math: negation-as-failure over the equivalence machinery" * doctest::test_suite("slow"))
{
    // The negation has to be evaluated AFTER the equivalence machinery has
    // had its chance -- it is a multi-stage, demand-driven proof, not a
    // lookup. Evaluated too early, every identity would come out refuted.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    math_with(interactive, "a b");

    process_lines(interactive, R"(
(P lhs L, P rhs R) => (L ≡ R)
(P lhs L, P rhs R, (L ≡ R) = proven) => (P status verified)
(P lhs L, P rhs R, ¬((L ≡ R) = proven)) => (P status refuted)
t1 lhs $( (a+b)*(a+b) )
t1 rhs $( a^2 + 2*a*b + b^2 )
t2 lhs $( (a+b)*(a+b) )
t2 rhs $( a^2 + b^2 )
)");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("S status O");
    CHECK(answers_contain(collector, "t1 status verified"));
    CHECK(answers_contain(collector, "t2 status refuted"));
    CHECK(collect_answers(collector).size() == 2);
}

TEST_CASE("math: a rule generator drives the differentiation layer")
{
    // Three layers that never meet elsewhere: a generator writes one rule per
    // indeterminate, each generated rule derives a differentiation REQUEST,
    // and the demand-driven layer answers it.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    math_with(interactive, "x y");

    process_lines(interactive, R"(
(V ~ indet) => ((T interesting yes) => (T diffby V))
x ~ indet
y ~ indet
$( x*y ) interesting yes
)");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("(A diffby B) = R");
    CHECK(answers_contain(collector, "((x * y) diffby x) = y"));
    CHECK(answers_contain(collector, "((x * y) diffby y) = x"));
}

TEST_CASE("math: a cluster rolls back generated rules and derived mathematics")
{
    // .cluster-drop promises to remove exactly what was created while the
    // cluster was active. Here that is a generated RULE plus everything the
    // demand-driven layer computed because of it -- and the math stack's own
    // 338 rules must survive untouched. The node count is the whole
    // assertion: it is the only thing that can say "exactly".
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    math_with(interactive, "x y");

    const auto nodes = [&]
    {
        collector.clear();
        interactive.process(".stat");
        for (const auto& event : collector.events())
        {
            const std::string text = normalize(event.text);
            const auto        pos  = text.find("Nodes: ");
            if (pos != std::string::npos) return std::stoul(text.substr(pos + 7));
        }
        return 0UL;
    };

    const std::size_t before = nodes();

    interactive.process(".cluster exp");
    process_lines(interactive, R"(
(V ~ indet) => ((T interesting yes) => (T diffby V))
x ~ indet
$( x*y ) interesting yes
)");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("(A diffby B) = R");
    REQUIRE(answers_contain(collector, "((x * y) diffby x) = y"));
    CHECK(nodes() > before);

    interactive.process(".cluster-drop exp");
    CHECK(nodes() == before);

    // The derived mathematics is gone with it, and the stack still works.
    collector.clear();
    interactive.process("(A diffby B) = R");
    CHECK(collect_answers(collector).empty());

    collector.clear();
    interactive.process(".list-rules");
    CHECK_FALSE(any_output_contains(collector, "No rules found"));
}

TEST_CASE("math: a refuted identity reported as a contradiction" * doctest::test_suite("slow"))
{
    // Negation-as-failure, the equivalence machinery and the contradiction
    // marker in one rule set: a claimed identity that cannot be proved makes
    // the knowledge base contradictory. A true one must not.
    SUBCASE("a true identity is no contradiction")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        math_with(interactive, "a b");
        process_lines(interactive, R"(
(P lhs L, P rhs R) => (L ≡ R)
(P lhs L, P rhs R, ¬((L ≡ R) = proven)) => !
t1 lhs $( (a+b)*(a+b) )
t1 rhs $( a^2 + 2*a*b + b^2 )
)");
        collector.clear();
        interactive.run(true, false, false);
        CHECK_FALSE(has_contradiction(collector));
    }

    SUBCASE("a false one is")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        math_with(interactive, "a b");
        process_lines(interactive, R"(
(P lhs L, P rhs R) => (L ≡ R)
(P lhs L, P rhs R, ¬((L ≡ R) = proven)) => !
t2 lhs $( (a+b)*(a+b) )
t2 rhs $( a^2 + b^2 )
)");
        // Reported when it is FOUND. It used to be asserted of a later run
        // instead, which worked only because the report repeated on every one
        // of them -- the behaviour the graph-side record replaces.
        CHECK(has_contradiction(collector));

        // And a later run does not say it again: the graph holds the record.
        collector.clear();
        interactive.run(true, false, false);
        CHECK_FALSE(has_contradiction(collector));
    }
}

TEST_CASE("math: a demand-driven computation survives being saved half-way")
{
    // The request is made, the network is written to disk BEFORE anything
    // computes it, and the answer is produced after the reload. What has to
    // survive is the request itself -- a fact like any other -- and the
    // stack's ability to pick it up on the next run.
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "zelph_half_derivative.bin";
    std::filesystem::remove(file);

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".auto-run"); // off: nothing may compute yet
        math_with(interactive, "x");
        interactive.process("$( x^3 ) diffby x");

        collector.clear();
        interactive.process("(A diffby x) = R");
        REQUIRE(collect_answers(collector).empty()); // genuinely half-way

        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    std::filesystem::remove(file);
    interactive.process(".import math"); // rules are not in the .bin by design
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("(A diffby x) = R");
    CHECK(answers_contain(collector, "((x ^ &3) diffby x) = $( &3 * x ^ &2 )"));
}
