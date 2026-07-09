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

#include <doctest/doctest.h> // provides main()

#include "test_helpers.hpp"

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Node display / reconstruction
//
// Most output checking in this suite is implicit: tests assert the presence
// of expected substrings as a side effect of testing reasoning semantics.
// That style has two systematic blind spots, both of which have produced
// real bugs:
//   1. Structures that only REASONING creates (never parsed input) take
//      reconstruction paths no test script exercises.
//   2. Presence checks don't catch silently DROPPED components -- a '?' or
//      a missing tail passes any contains() assertion aimed elsewhere.
// This file collects cases where the rendered output IS the tested
// semantics: round-trips and reconstruction of node structures. It is not
// the start of a systematic display suite; it grows when a reconstruction
// path breaks.
// ---------------------------------------------------------------------------

TEST_CASE("display: improper cons chains render their tail (rule patterns)")
{
    // A cons chain not ending at nil is not a proper list. The list
    // formatter used to collect the cars and silently drop the tail,
    // rendering the rule pattern (A cons R) as <A>. Improper chains must
    // render in explicit cons input syntax instead.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("((A cons R) probe M) => (R probe M)");
        // The echo must contain the full pattern including the tail
        // variable R, in round-trippable input syntax.
        CHECK(any_output_contains(collector, "(A cons R)"));
        CHECK_FALSE(any_output_contains(collector, "<A>")); });
}

TEST_CASE("display: improper cons chain with atomic tail")
{
    // Data-level improper list: (a cons b) where b is a plain atom, not
    // nil. Historically rendered as <a>, hiding both the tail and the
    // fact that the chain is unterminated.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("(a cons b) tagged t1");
        CHECK(any_output_contains(collector, "a cons b"));
        CHECK_FALSE(any_output_contains(collector, "<a>")); });
}

TEST_CASE("display: proper lists keep their compact rendering")
{
    // Guard against over-correction: nil-terminated chains must continue
    // to render as lists (<123>) and, with a registered digit alphabet,
    // as &-literals -- the existing display paths are untouched.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("<123> tagged t2");
        CHECK(any_output_contains(collector, "<123>"));

        interactive.process(".import arithmetic");
        collector.clear();
        interactive.process("&42 tagged t3");
        CHECK(any_output_starts_with(collector, "&42 tagged t3")); });
}

TEST_CASE("display: self-referential fact as subject of further facts")
{
    // End-to-end companion to the parse_fact pinning test in
    // test_reasoning.cpp: the constellation that division X/X produces
    // systematically. Kept here as the display-level regression anchor.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(x foo x) bar a
(x foo x) baz b
)");
        collector.clear();
        interactive.process("(x foo x) qux c");
        CHECK(any_output_contains(collector, "x foo x"));
        CHECK_FALSE(any_output_contains(collector, "foo ?")); });
}
