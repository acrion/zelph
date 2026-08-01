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
// A rule's patterns are not data.
//
// Writing a rule materializes its conditions and its consequences as real
// fact nodes, because the engine has nothing else to match against. A pattern
// carrying a variable gives itself away as a template and is rejected as data
// everywhere. A GROUND one does not, and the consequences were severe and
// silent:
//
//     zelph> (a p b) => (c q d)
//     zelph> C q D
//     Answer: c q d          <- nobody said this
//
// It was not confined to queries -- the leaked pattern satisfied other rules'
// conditions -- and the shape that bites hardest is a ground CONSEQUENCE
// under a variable condition: "(A is bad) => (alarm is on)" answered "alarm
// is on" with nothing bad anywhere.
//
// The node itself cannot say which happened; asserting a statement and
// building it as a pattern produce the same node with the same edges. What
// can say it is the MOMENT of construction, and that is what the marking uses
// (see Zelph::mark_rule_patterns). Asserting or deriving the statement later
// revokes the mark, so the tests below come in pairs: the pattern is inert,
// and the moment it is claimed it behaves like any other fact.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;
}

TEST_CASE("rule patterns: a ground pattern answers no query")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d"));

        collector.clear();
        interactive.process("A p B");
        CHECK_FALSE(answers_contain(collector, "a p b")); });
}

TEST_CASE("rule patterns: a ground pattern satisfies no other rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("(X q Y) => (X r Y)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X r Y");
        CHECK_FALSE(answers_contain(collector, "c r d")); });
}

TEST_CASE("rule patterns: the shape that bites -- a ground consequence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A is bad) => (alarm is on)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X is on");
        REQUIRE_FALSE(answers_contain(collector, "alarm is on"));

        // ... and the moment something IS bad, it is an ordinary fact.
        interactive.process("rust is bad");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X is on");
        CHECK(answers_contain(collector, "alarm is on")); });
}

TEST_CASE("rule patterns: being derived announces the statement")
{
    // Revoking the mark is the moment the statement BECOMES data, and
    // everything that reacts to a new fact has to hear about it. Without the
    // announcement, semi-naive seeding never offers it to the rules whose
    // conditions it now satisfies -- the node itself is old, so nothing else
    // would mention it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(A is bad) => (alarm is on)");
        interactive.process("(X is on) => (X needs attention)");
        collector.clear();
        interactive.process("rust is bad");
        CHECK(any_deduction_of(collector, "alarm is on"));
        CHECK(any_deduction_of(collector, "alarm needs attention"));

        collector.clear();
        interactive.process("S needs O");
        CHECK(answers_contain(collector, "alarm needs attention")); });
}

TEST_CASE("rule patterns: asserting the statement first keeps it data")
{
    // Order matters and must not: a statement that was CLAIMED before any
    // rule mentioned it is a claim, and the rule reusing its node changes
    // nothing about that. The construction only ever marks what it created.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("(a p b) => (c q d)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        // The rule fires on it, so the consequence is a claim too now.
        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: asserting it afterwards revokes the mark")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: a propositional rule reaches its conclusion")
{
    // Both halves are needed for this one to be observable at all: the
    // ground condition has to be allowed to match (see "rules: a ground
    // condition is not a non-match" in test_reasoning.cpp), and the ground
    // consequence has to be a pattern rather than a fact, or it would have
    // been there before the rule ever fired.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(a p b) => (c q d)");
        collector.clear();
        interactive.process("a p b");
        CHECK(any_deduction_of(collector, "c q d"));

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d")); });
}

TEST_CASE("rule patterns: a pattern with variables is untouched")
{
    // The control. Nothing about the marking may reach the ordinary case,
    // where the template check has always done the work.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A p B) => (A q B)");
        interactive.process("x p y");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "x q y"));

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "x p y")); });
}

TEST_CASE("rule patterns: .explain does not call a pattern an axiom")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "rule pattern; not asserted"));
        CHECK_FALSE(any_output_contains(collector, "[axiom]"));

        // Once it is derived, it has a proof like anything else.
        interactive.process("a p b");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "a p b"));
        CHECK_FALSE(any_output_contains(collector, "rule pattern")); });
}

TEST_CASE("rule patterns: the mark survives .save and .load")
{
    // The record is a fact in the graph, not something the session
    // remembers -- so a round trip has to keep it, and the in-memory index
    // has to be read back off the graph afterwards.
    const auto file = fs::temp_directory_path() / "zelph_rule_pattern_test.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process("(a p b) => (c q d)");
        interactive.process(".save \"" + file.string() + "\"");
    }

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        interactive.process(".load \"" + file.string() + "\"");

        collector.clear();
        interactive.process("C q D");
        CHECK_FALSE(answers_contain(collector, "c q d"));

        // .load disables auto-run.
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d"));
    }

    fs::remove(file);
}
