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

// A path condition under `¬` asks the same question the other way round: does
// this node NOT reach that one. The reading follows from the one reading `¬`
// has -- the condition succeeds when nothing matches, and it never binds.
TEST_CASE("path condition: under ¬ it succeeds exactly when there is no path")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("y rel d");
        collector.clear();

        // a reaches d, d reaches nothing. So the negated condition must fail
        // for the anchor at a and succeed for the one at d.
        //
        // Until this was fixed the guard dispatch in Reasoning::evaluate ran
        // before the negation tag was read, so `¬(S P279⁺ d)` was evaluated as
        // `(S P279⁺ d)` -- it derived the answer for a, the one case where
        // there IS a path, and `.explain` printed the walked premise as
        // `[absent]` beside it.
        interactive.process("(X rel S, ¬(S P279⁺ d)) => (X clear-of-d S)");

        CHECK(any_output_contains(collector, "(y clear-of-d d)"));
        CHECK_FALSE(any_output_contains(collector, "(x clear-of-d a)")); });
}

// The reflexive variant differs from the transitive one exactly on the start
// node, and that difference has to survive the negation rather than being
// swallowed by it.
TEST_CASE("path condition: ¬ over the reflexive variant excludes the start too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("y rel d");
        collector.clear();

        // (d P279∗ d) holds by zero steps, so the negation fails for d as
        // well -- where `¬(d P279⁺ d)` succeeded in the test above.
        interactive.process("(X rel S, ¬(S P279∗ d)) => (X strictly-clear-of-d S)");

        CHECK_FALSE(any_output_contains(collector, "(y strictly-clear-of-d d)"));
        CHECK_FALSE(any_output_contains(collector, "(x strictly-clear-of-d a)")); });
}

TEST_CASE("path condition: a negated path with an open end is refused")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        collector.clear();

        // One bound end is enough for the positive condition, which then
        // GENERATES a binding per node reached. A negation binds nothing, so
        // there is nothing for the open end to become and no second reading to
        // fall back on -- "reaches nothing at all" is asked by binding it.
        interactive.process("(X rel S, ¬(S P279⁺ T)) => (X nowhere T)");

        CHECK(any_output_contains(collector, "needs BOTH ends bound"));
        CHECK_FALSE(any_output_contains(collector, "(x nowhere")); });
}

// The refusal is decided by the shape of the condition, so it is the same for
// every candidate binding. Reporting it per binding turns one diagnostic into
// one per fact of the anchoring relation -- millions on a Wikidata network.
TEST_CASE("path condition: a refusal is reported once, not once per binding")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("x rel b");
        interactive.process("x rel c");
        interactive.process("x rel d");
        collector.clear();

        interactive.process("(X rel S, ¬(S P279⁺ T)) => (X nowhere T)");

        size_t refusals = 0;
        for (const auto& event : collector.events())
            if (event.text.find("needs BOTH ends bound") != std::string::npos) ++refusals;
        CHECK(refusals == 1); });
}

// `⁺` and `∗` say what the engine WALKS. A rule condition may therefore use
// them, and a question may ask with them -- but a ground statement outside a
// rule would be asserting a reachability, which is not a thing anyone can
// assert. It did worse than mean nothing: the sugar builds its operand with
// zelph/fact, so `a p⁺ b` entered `a p b` as a claim and hung a closure tag
// off it that nothing ever reads, since only a rule condition is walked.
TEST_CASE("path condition: a ground path outside a rule is refused")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);
    collector.clear();

    CHECK_THROWS(interactive.process("a P279⁺ d"));

    // And nothing of it reached the graph -- neither the one step it would
    // have asserted nor a closure tag.
    collector.clear();
    interactive.process("S P279 d");
    CHECK_FALSE(any_output_contains(collector, "a P279 d"));
    collector.clear();
    interactive.process("X closure Y");
    CHECK(collect_answers(collector).empty());
}

// A `¬` in front is not a way past that refusal, and it used to be one. A
// negated line is routed through zelph/refute, while the path sugar is keyed on
// zelph/fact -- so the marker was never split off the predicate token and
// "¬(a P279⁺ d)" built a fact whose predicate is a node NAMED "P279⁺". An
// invented predicate, marked refuted, out of a line about reachability, with no
// message of any kind; the same line without the "¬" was refused throughout.
TEST_CASE("path condition: a ground path under a negation is refused too")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);
    collector.clear();

    CHECK_THROWS(interactive.process("¬(a P279⁺ d)"));
    CHECK_THROWS(interactive.process("¬(a P279∗ d)"));

    // What the throw is worth is this: the marker is still a marker and not
    // part of a name. A node called "P279⁺" is what the old path left behind.
    collector.clear();
    CHECK_THROWS(interactive.process(".node \"P279⁺\""));

    collector.clear();
    interactive.process("X closure Y");
    CHECK(collect_answers(collector).empty());
    collector.clear();
    interactive.process("X ~ refuted");
    CHECK(collect_answers(collector).empty());
}

// A path marker one argument DOWN is refused whether or not its ends are
// bound, and that is the difference from the shape above. Alone on a line the
// ends decide -- ground asserts and is refused, free is a question and answers
// one -- but inside a statement there is no question to ask: the marker hung a
// closure tag off a fact where nothing ever walks one, and the ground form
// reached the runtime guard only by luck of both ends being concrete. The
// three lines below are, in order: both ends bound, one end free, and one
// level further down.
TEST_CASE("path condition: a path marker inside a plain statement is refused")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);
    collector.clear();

    CHECK_THROWS(interactive.process("x q (a P279⁺ d)"));
    CHECK_THROWS(interactive.process("x q (S P279⁺ d)"));
    CHECK_THROWS(interactive.process("x q (y r (a P279⁺ d))"));

    collector.clear();
    interactive.process("X closure Y");
    CHECK(collect_answers(collector).empty());
    collector.clear();
    interactive.process("S q O");
    CHECK(collect_answers(collector).empty());

    // And the question the walk must NOT reach: its positions are the
    // statement's own, not a nested argument.
    collector.clear();
    interactive.process("S P279⁺ d");
    CHECK_FALSE(collect_answers(collector).empty());
}

// The same shape with a variable in it is a QUESTION, and answering it is the
// feature. The refusal above is decided when the ends are resolved, not by the
// syntax, which is why it cannot live in the parser.
TEST_CASE("path condition: a path with a free end is a question and answers one")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        collector.clear();

        interactive.process("S P279⁺ d");

        CHECK(any_output_contains(collector, "(a P279 d) closure one-or-more"));
        CHECK(any_output_contains(collector, "(c P279 d) closure one-or-more")); });
}

TEST_CASE("path condition: a path as a rule consequence is refused")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);
    collector.clear();

    // It used to be accepted, listed by .list-rules and derive nothing at all.
    CHECK_THROWS(interactive.process("(X rel S) => (S P279⁺ d)"));

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "No rules found"));
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

// .prune-nodes acts on what ONE variable binds. A conjunction has one per
// condition, so a BARE conjunction says nothing about which is meant -- and
// it used to answer "a pattern without variables binds nothing to delete"
// about a pattern full of them, then prune nothing. Naming the variable is
// what the leading token does, so the refusal names that form.
TEST_CASE("prune: a conjunction without a named variable says how to name one")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        collector.clear();

        std::string message;
        try
        {
            interactive.process(".prune-nodes (S rel T, T P279⁺ U)");
        }
        catch (const std::exception& ex)
        {
            message = ex.what();
        }

        CHECK(message.find(".prune-nodes <variable> (<conditions>)") != std::string::npos);
        CHECK(message.find("without variables") == std::string::npos);

        // Nothing was deleted on the way to the message.
        collector.clear();
        interactive.process(".node a");
        CHECK(any_output_contains(collector, "Name in language")); });
}

// .explain used to answer "asserted; no derivation found" for every fact
// derived through a path condition -- although the deduction line printed at
// derivation time named its premises correctly. The reconstruction looked for
// the tag fact among the graph's facts, and a path condition has none: it
// holds by a WALK. A reconstructible justification per result is what .explain
// is for, so a whole class of results had none.
TEST_CASE("path condition: .explain reconstructs a derivation that walked")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("(X rel S, S P279⁺ T) => (X above T)");

        collector.clear();
        interactive.process(".explain (x above d)");

        // The derivation is found at all ...
        CHECK_FALSE(any_output_contains(collector, "no derivation found"));
        // ... its ordinary premise is an axiom ...
        CHECK(any_output_contains(collector, "x rel a  [axiom]"));
        // ... and the walked one says so, in the verbose form that re-enters
        // as the same rule. NOT [axiom]: nobody asserted the path.
        CHECK(any_output_contains(collector, "(a P279 d) closure one-or-more  [closure]")); });
}

TEST_CASE("path condition: .explain marks a zero-step path as walked too")
{
    // The reflexive variant reaches its start, and the premise then renders
    // through the self-fact sugar -- `(:P279 a)` is `a P279 a`, which is what
    // a zero-step path from a to a denotes.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("(X rel S, S P279∗ T) => (X reaches T)");

        collector.clear();
        interactive.process(".explain (x reaches a)");
        CHECK_FALSE(any_output_contains(collector, "no derivation found"));
        CHECK(any_output_contains(collector, "closure zero-or-more  [closure]"));

        collector.clear();
        interactive.process(".explain (x reaches d)");
        CHECK(any_output_contains(collector, "(a P279 d) closure zero-or-more  [closure]")); });
}
