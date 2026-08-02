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

// Set constants and collections.
//
// zelph used to have one brace literal doing both jobs, and the two are
// incompatible. A mathematical SET is determined by its members -- the axiom
// of extensionality -- so `{a b}` written twice denotes one and the same set,
// and there is no such operation as adding an element to it; you form a new
// set. A CONTAINER has an identity of its own, and putting something into it
// is exactly what one does with it.
//
// Conflating them cost both: `{a b}` built a fresh container every time, so a
// set literal in a rule condition could never match data written with the same
// literal, and asserting `x in {a b}` silently extended the very set it named,
// leaving a node whose identity said {a b} while it rendered {a b x}.
//
// The two are now separate literals. `{...}` is the set constant, keeping the
// mathematical convention; `@{...}` is the collection, following Janet -- the
// language zelph embeds -- where `{...}` is the immutable struct and `@{...}`
// the mutable table. The marker costs no reserved character: `@` stays an
// ordinary name character and only `@{` is special.

#include "test_helpers.hpp"

#include <filesystem>

using namespace zelph::test;

TEST_CASE("sets: a set constant is its members, so the same literal is the same node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Two occurrences of the literal denote ONE node, which is what a
        // rule needs: the set in its condition has to be the set in the data.
        process_lines(interactive, R"(
p q {a b}
r s {a b}
)");
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "p q {a b}"));

        collector.clear();
        interactive.process("S s O");
        CHECK(answers_contain(collector, "r s {a b}"));

        // Order does not matter -- a set is not a list.
        interactive.process("t u {b a}");
        collector.clear();
        interactive.process("t u O");
        CHECK(answers_contain(collector, "t u {a b}")); });
}

TEST_CASE("sets: a collection has its own identity, so two literals are two containers")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x in @{a b}
y in @{a b}
)");
        // Each literal built its own container, and each took its own member.
        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "x in @{a b x}"));
        CHECK(answers_contain(collector, "y in @{a b y}")); });
}

TEST_CASE("sets: a set constant cannot be extended, and the message says what to write")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process("x in {a b}"),
                             doctest::Contains("set constant cannot be extended"),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process("x in {a b}"),
                             doctest::Contains("@{...}"),
                             std::runtime_error);

        // Stating what already holds is not an extension: `a in {a b}` is
        // true by construction, so it is a no-op rather than an error.
        interactive.process("a in {a b}"); });
}

TEST_CASE("sets: a rule quantifies over the members of a set constant")
{
    // The payoff. This shape derived nothing at all while every literal built
    // its own container -- the rule's set was one nothing else referred to.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in {a b}) => (X flagged yes)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "a flagged yes"));
        CHECK(answers_contain(collector, "b flagged yes"));
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("sets: a rule fills a collection while it runs")
{
    // The counterpart: a container is what a rule can put things INTO. The
    // derived facts name the rule's own container, and it grows.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c p d
(X p Y) => (X in @{Y})
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        // Both subjects landed in one container -- and NOT the rule's pattern
        // variables, which used to be printed as members: `a in {a c Y X}`.
        CHECK(answers_contain(collector, "a in @{a c}"));
        CHECK(answers_contain(collector, "c in @{a c}")); });
}

TEST_CASE("sets: a literal carrying a variable is a container, not a constant")
{
    // Extensionality needs KNOWN members. `{Y}` denotes a different set for
    // every binding of Y, so it cannot be hash-consed and is the container a
    // pattern can be -- which is also what keeps the engine's own conjunction
    // sugar `*{(A rel B) (B rel C)} ~ conjunction` working.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(X p Y) => (X in {Y})
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "a in @{a}")); });
}

TEST_CASE("sets: an empty literal of either kind is nil")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
p q {}
p r @{}
)");
        collector.clear();
        interactive.process("p q O");
        CHECK(answers_contain(collector, "p q nil"));

        collector.clear();
        interactive.process("p r O");
        CHECK(answers_contain(collector, "p r nil")); });
}

TEST_CASE("sets: both kinds survive .save and .load and still print apart")
{
    const std::string path =
        (std::filesystem::temp_directory_path() / "zelph_test_sets.bin").string();

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
p q {a b}
x in @{c d}
)");
        interactive.process(".save " + path);
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + path);
    interactive.process(".auto-run");

    collector.clear();
    interactive.process("S q O");
    CHECK(answers_contain(collector, "p q {a b}"));

    collector.clear();
    interactive.process("S in O");
    CHECK(answers_contain(collector, "x in @{c d x}"));

    // The identity survives too: the literal still lands on the loaded node
    // rather than building a second set.
    CHECK_THROWS_AS(interactive.process("z in {a b}"), std::runtime_error);

    std::filesystem::remove(path);
}

TEST_CASE("sets: what is printed reads back as what was printed")
{
    // The round trip, for both kinds and nested inside a list.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
p q <@{a b} c>
r s {a b}
)");
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "p q <@{a b} c>"));

        collector.clear();
        interactive.process("S s O");
        CHECK(answers_contain(collector, "r s {a b}")); });
}

TEST_CASE("sets: a membership fact keeps its own member in the printed container")
{
    // The same node printed two ways depending on which command asked for it.
    // A container left the membership fact it was rendered FROM out of its own
    // element list, so `a in {a b}` came back as `a in {a}` -- and a set
    // constant is its members, so `{a}` is a different node. The query path
    // never had it, because there the container hangs off a pattern rather
    // than off the fact itself.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("x rel {a b}");

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "a in {a b}"));
        CHECK(answers_contain(collector, "b in {a b}"));

        collector.clear();
        interactive.process(".node a in {a b}");
        CHECK(any_output_contains(collector, "Representation: a in {a b}"));
        CHECK_FALSE(any_output_contains(collector, "Representation: a in {a}")); });
}

