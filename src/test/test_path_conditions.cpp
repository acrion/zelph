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
// Transitive path conditions: (X P⁺ Y) is one or more P steps, (X P∗ Y) zero
// or more. The condition is the tag fact (pattern closure mode), so nothing
// became a core node, and the walk is the same indexed closure engine that
// zelph/closure and sparql.zph's p+/p* already use.
//
// The chain in these tests is a -> b -> c -> d over P279, plus one anchor
// (x rel a) so that a rule has something to bind an end with.
// ---------------------------------------------------------------------------

namespace
{
    constexpr const char* kChain =
        "a P279 b\nb P279 c\nc P279 d\nx rel a\n";

    void feed(auto& interactive, const char* text)
    {
        std::string line;
        for (const char* p = text; *p; ++p)
        {
            if (*p == '\n')
            {
                interactive.process(line);
                line.clear();
            }
            else
                line.push_back(*p);
        }
        if (!line.empty()) interactive.process(line);
    }
} // namespace

TEST_CASE("path condition: a bound start generates every node it reaches")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        collector.clear();

        interactive.process("(X rel S, S P279⁺ T) => (X above T)");

        // One or more steps from a: b, c and d -- and NOT a itself.
        CHECK(any_output_contains(collector, "(x above b)"));
        CHECK(any_output_contains(collector, "(x above c)"));
        CHECK(any_output_contains(collector, "(x above d)"));
        CHECK_FALSE(any_output_contains(collector, "(x above a)")); });
}

TEST_CASE("path condition: the reflexive variant includes the start")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        collector.clear();

        interactive.process("(X rel S, S P279∗ T) => (X above T)");

        CHECK(any_output_contains(collector, "(x above a)"));
        CHECK(any_output_contains(collector, "(x above d)")); });
}

TEST_CASE("path condition: a bound target generates every node that reaches it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("y rel d");
        collector.clear();

        // T is bound by the anchor, S is the open end: walk backwards.
        interactive.process("(X rel T, S P279⁺ T) => (X below S)");

        CHECK(any_output_contains(collector, "(y below a)"));
        CHECK(any_output_contains(collector, "(y below b)"));
        CHECK(any_output_contains(collector, "(y below c)"));
        CHECK_FALSE(any_output_contains(collector, "(y below d)")); });
}

TEST_CASE("path condition: both ends bound is a reachability test")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("p pair a");
        interactive.process("p pair d");
        interactive.process("q pair d");
        interactive.process("q pair a");
        collector.clear();

        // Both ends come bound from the two preceding conditions; the path
        // condition only filters. d is reachable from a, a is not from d.
        interactive.process("(P pair S, P pair T, S P279⁺ T, S != T) => (S under T)");

        CHECK(any_output_contains(collector, "(a under d)"));
        CHECK_FALSE(any_output_contains(collector, "(d under a)")); });
}

TEST_CASE("path condition: both ends free is refused, and says why")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        collector.clear();

        interactive.process("(S P279⁺ T) => (S above T)");

        CHECK(any_output_contains(collector, "needs at least one bound end"));
        CHECK_FALSE(any_output_contains(collector, "(a above d)")); });
}

TEST_CASE("path condition: the printed rule re-enters as the same rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("(X rel S, S P279⁺ T) => (X above T)");
        collector.clear();

        // The verbose form is what .list-rules prints; entering it must give
        // the same rule and the same deductions, or the notation would not
        // determine the term.
        interactive.process(".new");
        feed(interactive, kChain);
        interactive.process("(((S P279 T) closure one-or-more), (X rel S)) => (X above T)");

        CHECK(any_output_contains(collector, "(x above b)"));
        CHECK(any_output_contains(collector, "(x above d)")); });
}

// The marker earns its place by NOT being reserved: it is read only as a
// trailing marker in predicate position, and only when a name is left over.
TEST_CASE("path condition: the marker stays an ordinary character elsewhere")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();

        // A name ENDING in the marker, in subject and object position.
        interactive.process("Na⁺ charge plus");
        interactive.process("cation includes Na⁺");
        CHECK(any_output_contains(collector, "Na⁺ charge plus"));
        CHECK(any_output_contains(collector, "cation includes Na⁺"));

        // A name CONTAINING it, used as a predicate: not a path condition,
        // because the marker is not the last character.
        interactive.process("u a⁺b v");
        CHECK(any_output_contains(collector, "u a⁺b v"));

        // The marker alone as a predicate is that predicate, not an operator
        // on an empty name.
        interactive.process("m ⁺ n");
        CHECK(any_output_contains(collector, "m ⁺ n")); });
}
