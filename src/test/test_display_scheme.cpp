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
// Display schemes: the mechanism by which a SCRIPT tells node_to_string how
// its own notation is written. C++ knows precedence, associativity,
// delimiters, numeral prefix and leaf grammar -- never a concrete operator.
// These tests therefore use a deliberately NON-mathematical vocabulary; the
// math-syntax integration is tested separately.
// ---------------------------------------------------------------------------

namespace
{
    // A policy notation: "andthen" binds tighter than "unless", both left-
    // associative, enclosed in [[ ]] wherever the rendering deviates.
    const char* const policy_scheme =
        "%(do (zelph/register-display-scheme \"policy\" \"[[\" \"]]\" "
        "{:name-first \"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_\" "
        ":name-chars \"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_\"}) "
        "(zelph/set-infix-display \"policy\" [[\"unless\" 10 :left] [\"andthen\" 20 :left]]))";

    // Same, but numbers are written without the '&' sigil.
    const char* const bare_numeral_scheme =
        "%(do (zelph/register-display-scheme \"policy\" \"[[\" \"]]\" "
        "{:numeral-prefix \"\" "
        ":name-first \"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_\" "
        ":name-chars \"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_\"}) "
        "(zelph/set-infix-display \"policy\" [[\"andthen\" 20 :left]]))";
}

TEST_CASE("display scheme: nothing changes without a registration")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("(a andthen b) unless c");
        CHECK(any_output_contains(collector, "(a andthen b) unless c"));
        CHECK_FALSE(any_event_contains(collector, "[[")); });
}

TEST_CASE("display scheme: precedence and associativity govern the parentheses")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(policy_scheme);

        SUBCASE("a tighter-binding subject loses its parentheses")
        {
            collector.clear();
            interactive.process("(a andthen b) unless c");
            CHECK(any_output_contains(collector, "[[a andthen b unless c]]"));
        }
        SUBCASE("left associativity drops the LEFT operand's parentheses")
        {
            collector.clear();
            interactive.process("(a unless b) unless c");
            CHECK(any_output_contains(collector, "[[a unless b unless c]]"));
        }
        SUBCASE("left associativity KEEPS the right operand's parentheses")
        {
            collector.clear();
            interactive.process("a unless (b unless c)");
            CHECK(any_output_contains(collector, "a unless (b unless c)"));
            CHECK_FALSE(any_event_contains(collector, "[["));
        }
        SUBCASE("no deviation, no wrapper")
        {
            collector.clear();
            interactive.process("a andthen b");
            CHECK(any_output_contains(collector, "a andthen b"));
            CHECK_FALSE(any_event_contains(collector, "[["));
        } });
}

TEST_CASE("display scheme: a leaf outside the declared grammar disables the scheme")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(policy_scheme);

        // ':' is a perfectly ordinary symbol character in zelph (wd:Q5), but
        // it is not in this scheme's identifier grammar -- so the whole term
        // falls back to the default rendering rather than producing output
        // the scheme's parser could not read.
        collector.clear();
        interactive.process("(a andthen wd:Q5) unless c");
        CHECK(any_output_contains(collector, "(a andthen wd:Q5) unless c"));
        CHECK_FALSE(any_event_contains(collector, "[[")); });
}

TEST_CASE("display scheme: the numeral prefix replaces '&' inside the scheme")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import binary-arithmetic");
        interactive.process(bare_numeral_scheme);

        collector.clear();
        interactive.process("&1 andthen b");
        CHECK(any_output_contains(collector, "[[1 andthen b]]"));

        // Outside the scheme the '&' is untouched.
        collector.clear();
        interactive.process("&1 mentions b");
        CHECK(any_output_contains(collector, "&1 mentions b")); });
}

TEST_CASE("display scheme: a predicate belongs to at most one scheme")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(policy_scheme);
        interactive.process("%(zelph/register-display-scheme \"other\" \"<<\" \">>\")");
        CHECK_THROWS_AS(interactive.process("%(zelph/set-infix-display \"other\" [[\"andthen\" 5 :left]])"),
                        std::runtime_error); });
}

TEST_CASE("display scheme: registered operators never use the self-fact sugar")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(policy_scheme);

        collector.clear();
        interactive.process(":andthen x");
        CHECK(any_output_contains(collector, "x andthen x"));
        CHECK_FALSE(any_event_contains(collector, ":andthen")); });
}

TEST_CASE("display scheme: an unknown scheme name is rejected")
{
    run_both_modes([](auto& collector, auto& interactive)
                   { CHECK_THROWS_AS(interactive.process("%(zelph/set-infix-display \"nope\" [[\"x\" 1 :left]])"),
                                     std::runtime_error); });
}
