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
        CHECK(any_output_contains(collector, "<3 2 1>"));

        interactive.process(".import decimal-arithmetic");
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

TEST_CASE("node display: non-canonical digit lists render raw, not as &-literals (binary mul)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Binary &3 * &0 accumulates <0> + <00>: the prod fact carries the
        // zero-extended raw list <00> (only the user-facing = result is
        // canonicalized via canonnum, see common-arithmetic MC0). The raw
        // node must render structurally, visibly distinct from &0.
        interactive.process(".import binary-arithmetic");
        collector.clear();
        interactive.process("&3 * &0");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "((&3 mul &0) prod <00>)"));
        CHECK_FALSE(any_output_contains(collector, "((&3 mul &0) prod &0)"));
        // The canonicalized user-facing result stays a &-literal.
        CHECK(any_output_contains(collector, "((&3 * &0) = &0)")); });
}

TEST_CASE("node display: non-canonical digit lists render raw, not as &-literals (decimal sub)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Decimal &105 - &98 internally yields the raw diff <007> (the
        // SC0 comment's own example); the canonnum bridge line now shows
        // the connection between both renderings explicitly.
        interactive.process(".import decimal-arithmetic");
        collector.clear();
        interactive.process("&105 - &98");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "diff <007>"));
        CHECK(any_output_contains(collector, "(<007> canonnum &7)"));
        CHECK(any_output_contains(collector, "((&105 - &98) = &7)"));
        CHECK_FALSE(any_output_contains(collector, "diff &7")); });
}

TEST_CASE("display: tagging a structured term does not hide it behind the concept")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("(x + y) ~ t");
        // "is an instance of" is not "is": the term keeps its own structure,
        // so the echo stays re-enterable input.
        CHECK(any_output_contains(collector, "(x + y) ~ t"));
        CHECK_FALSE(any_output_contains(collector, "t ~ t")); });
}

TEST_CASE("display: a compiled term renders in full as a nested subterm")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A term that has been through topoly acquires predicates of its
        // own (aspoly, needstopoly, mul). z->parse_fact's candidate filter
        // discards such a subject, and the fallback walk used to take the
        // first bidirectional neighbour of the fact node -- which includes
        // every fact the node is the SUBJECT of, among them the parent
        // currently being rendered. The history check then printed '?'.
        // Only the nested position was affected: at top level parent == 0,
        // so there was no ancestor to pick wrongly.
        interactive.process(".import topoly");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
x pouter y
:topoly (((x * x) * y) + x)
)");
        collector.clear();
        interactive.process("((x * x) * y) foo probe");
        CHECK(any_output_contains(collector, "((x * x) * y) foo probe"));
        // Both failure shapes seen in practice: the ancestor collapsing to
        // '?', and a fallback that finds no candidate at all ('??').
        CHECK_FALSE(any_output_contains(collector, "(? * y)"));
        CHECK_FALSE(any_output_contains(collector, "(?? * y)")); });
}

TEST_CASE("display: a compiled term keeps its structure across several parents")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The wrong subject was picked from an unordered adjacency set, so
        // which foreign fact won depended on how many facts the term was
        // part of. Rendering the same term under several different parents
        // pins that the recorded structure wins every time.
        interactive.process(".import topoly");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
x pouter y
:topoly (((x * x) * y) + x)
((x * x) * y) foo probe
)");
        collector.clear();
        interactive.process("((x * x) * y) bar probe2");
        interactive.process("(x * x) qux probe3");
        CHECK(any_output_contains(collector, "((x * x) * y) bar probe2"));
        CHECK(any_output_contains(collector, "(x * x) qux probe3"));
        CHECK_FALSE(any_output_contains(collector, "?")); });
}

TEST_CASE("display: rendering under active logging does not recurse")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Rendering consults get_fact_structures, whose log messages are
        // built with format() -- which renders again. With logging on,
        // that pair recursed without bound and overflowed the stack, so
        // should_log() suppresses log output while a rendering is in
        // progress. Reaching the CHECKs at all is half the assertion here.
        interactive.process(".import topoly");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
x pouter y
:topoly (((x * x) * y) + x)
)");
        interactive.process(".log 3");
        collector.clear();
        interactive.process("((x * x) * y) foo probe");
        interactive.process(".log 0");
        CHECK(any_output_contains(collector, "((x * x) * y) foo probe")); });
}
