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

// A comma-separated sequence lacking a variable asserts every one of its
// members, and a path on a member's predicate was refused solely upon the
// runtime guard being triggered: "e q f, a P279⁺ d" kept `e q f`, whereas the
// reverse sequence left nothing -- one statement, two distinct results. The
// traversal across the list reads arguments, and a member's predicate is not
// one of them. Rejected before any construction.
TEST_CASE("path condition: a ground path in a comma list without a variable is refused before anything is built")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);

    for (const char* statement : {"e q f, a P279⁺ d", "a P279⁺ d, e q f", "e q f, :P279⁺ a"})
    {
        CAPTURE(statement);
        CHECK_THROWS(interactive.process(statement));
    }

    collector.clear();
    interactive.process("S q O");
    CHECK(collect_answers(collector).empty());
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
// feature. Whether a line poses a question is derived from its syntax --
// specifically, a variable located outside the rules it mentions -- and a
// question functions as a condition, which is where a path belongs. The guard
// for the ground case remains for the ends the parser does not see
// (janet_cfun_zelph_path_guard).
TEST_CASE("path condition: a path with a free end is a question and answers one")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        collector.clear();

        interactive.process("S P279⁺ d");

        CHECK(answers_contain(collector, "a P279⁺ d"));
        CHECK(answers_contain(collector, "c P279⁺ d")); });
}

// In a query, a ground path functions identically to a ground path within a
// rule condition: it serves as a reachability TEST. It was refused by the
// guard meant for a ground path outside a rule, which examines only the two
// endpoints, causing "X go Y, a P279⁺ d" to be interpreted as asserting
// reachability -- regardless of the sequence of its conditions. The single
// step beneath the marker constitutes a pattern, just as it does in a rule:
// `S P279 O` does not answer it.
TEST_CASE("path condition: a ground path in a query is a reachability test, in either order")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("t go k");

        for (const char* query : {"X go Y, a P279⁺ d", "a P279⁺ d, X go Y"})
        {
            CAPTURE(query);
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).size() == 1);
        }
        collector.clear();
        interactive.process("X go Y, d P279⁺ a");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S P279 O");
        CHECK_FALSE(answers_contain(collector, "a P279 d"));
        CHECK_FALSE(answers_contain(collector, "d P279 a")); });
}

// The same applies under "¬": the query asks that no path exists, and
// answers exactly when there is none.
TEST_CASE("path condition: a negated ground path in a query is a test too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        interactive.process("t go k");

        collector.clear();
        interactive.process("X go Y, ¬(d P279⁺ a)");
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process("X go Y, ¬(a P279⁺ d)");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S P279 O");
        CHECK_FALSE(answers_contain(collector, "d P279 a")); });
}

// The path condition expressed as `((S p O) closure one-or-more)`, the fact
// that the sugar constructs, is the same node, and refused where the sugar
// is: when typed as a statement, it asserted the single step beneath it.
// Within a query, it serves as the condition.
TEST_CASE("path condition: the path written out is refused in a statement that asserts")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kChain);
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS(interactive.process("(a P279 d) closure one-or-more"));
        CHECK_THROWS(interactive.process("x says ((a P279 d) closure zero-or-more)"));
        CHECK(node_count(collector, interactive) == before);

        interactive.process("t go k");
        collector.clear();
        interactive.process("X go Y, ((a P279 d) closure one-or-more)");
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process("S P279 O");
        CHECK_FALSE(answers_contain(collector, "a P279 d")); });
}

// A condition expressed in full is recognised by the node to which its tokens
// refer, rather than by the way they are read. Within ".lang en", the token
// "closure" names a node of that language, and a fact concerning it is a
// standard fact; the evaluation would not read it as a path either.
TEST_CASE("path condition: a path written out is recognised by its node, not by its name")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    interactive.process(".lang en");
    CHECK_NOTHROW(interactive.process("(a p b) closure one-or-more"));
    collector.clear();
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    interactive.process(".lang zelph");
    CHECK_THROWS(interactive.process("(c p d) closure one-or-more"));
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

// The self-path yields the identical consequence when expressed using
// self-fact sugar, and the check verified a marker solely on a predicate
// written between two values: "(X rel S) => (:P279⁺ X)" was accepted, and it
// derived `x P279 x`, the one step beneath the marker, as a claim.
TEST_CASE("path condition: a self-path as a rule consequence is refused too")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);

    CHECK_THROWS(interactive.process("(X rel S) => (:P279⁺ X)"));

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "No rules found"));
    collector.clear();
    interactive.process("S P279 O");
    CHECK(collect_answers(collector).size() == 3);
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

namespace
{
    // a and b form a two-cycle, c is not in one. All three are classes, so a
    // rule can bind an end without saying which node it is interested in.
    constexpr const char* kCycle =
        "a ~ cls\nb ~ cls\nc ~ cls\na P279 b\nb P279 a\nc P279 d\n";

    // k carries a single-step loop, m and n form a two-cycle, and each
    // possesses an anchor (x rel k, y rel m) enabling a rule to bind an end.
    constexpr const char* kLoop =
        "k P279 k\nx rel k\nm P279 n\nn P279 m\ny rel m\n";
} // namespace

// ":pred X" is the self-fact "(X pred X)", and a marker on the predicate token
// is a marker there like anywhere else -- so ":P⁺ X" asks whether X reaches
// ITSELF, which is a cycle test. The sugar used to take its token literally,
// which is the one position where a marker in predicate position was not read
// as one: ":P279⁺ x" built a predicate NAMED "P279⁺" and derived nothing.
//
// Pinned as an EQUIVALENCE rather than by its answers alone. The two spellings
// are one statement or they are two, and the second is how they drift apart
// again -- a later change to either has to move both.
TEST_CASE("path condition: :P⁺ X is the self-path (X P⁺ X)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto in_cycle = [&](const char* rule)
        {
            interactive.process(".new");
            feed(interactive, kCycle);
            interactive.process(rule);
            collector.clear();
            interactive.process("S in-cycle O");
            std::vector<std::string> answers = collect_answers(collector);
            std::sort(answers.begin(), answers.end());
            return answers;
        };

        const auto sugared = in_cycle("(X ~ cls, :P279⁺ X) => (X in-cycle X)");
        const auto spelled = in_cycle("(X ~ cls, X P279⁺ X) => (X in-cycle X)");

        CHECK(sugared == spelled);

        // And it is the cycle that decides, not the class membership: a and b
        // reach themselves through each other, c does not reach itself at all.
        // Asserted as the whole answer set, because a substring test for "c"
        // matches the predicate name itself.
        CHECK(sugared == std::vector<std::string>{":in-cycle a", ":in-cycle b"}); });
}

TEST_CASE("path condition: the reflexive self-path holds for every node")
{
    // Zero steps is a path, so ∗ says yes even for c, which has no P279 edge
    // back to itself. That difference from ⁺ is the whole of what the two
    // markers are, and it has to survive the sugar.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kCycle);
        interactive.process("(X ~ cls, :P279∗ X) => (X reaches-self X)");
        collector.clear();

        interactive.process("S reaches-self O");
        CHECK(collect_answers(collector).size() == 3); });
}

// A self-referential fact, where the subject equals its object, constitutes a
// one-step loop, and a one-step loop is a path of length one. The walk that
// answers a closure on a small graph read such a fact as lacking an object --
// a self-fact keeps its object within the subject's own entry -- thus,
// following `k P279 k`, `k P279⁺ T` reached nothing, whereas `k P279∗ T`
// answered k by its zero steps. The adjacent two-cycle demonstrates that a
// longer loop back to the start was always read.
TEST_CASE("path condition: a loop of one step is a path of length one, walked forward and backward")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kLoop);
        collector.clear();

        interactive.process("(X rel S, S P279⁺ T) => (X above T)");
        CHECK(any_output_contains(collector, "(x above k)"));
        CHECK(any_output_contains(collector, "(y above m)"));
        CHECK(any_output_contains(collector, "(y above n)"));

        interactive.process("(X rel T, S P279⁺ T) => (S below X)");
        CHECK(any_output_contains(collector, "(k below x)"));
        CHECK(any_output_contains(collector, "(m below y)"));
        CHECK(any_output_contains(collector, "(n below y)")); });
}

// When both ends are bound, the condition is the reachability test, and the
// loop has to pass it: the cycle test `S P279⁺ S` failed to include k, and its
// negation took k for a node that does not reach itself. w possesses no edge
// at all, thus the negation holds for it.
TEST_CASE("path condition: a loop of one step passes the reachability test, and fails its negation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kLoop);
        interactive.process("z rel w");
        collector.clear();

        interactive.process("(X rel S, S P279⁺ S) => (X loops S)");
        CHECK(any_output_contains(collector, "(x loops k)"));
        CHECK(any_output_contains(collector, "(y loops m)"));
        CHECK_FALSE(any_output_contains(collector, "(z loops w)"));

        interactive.process("(X rel S, ¬(S P279⁺ S)) => (X acyclic S)");
        CHECK_FALSE(any_output_contains(collector, "(x acyclic k)"));
        CHECK_FALSE(any_output_contains(collector, "(y acyclic m)"));
        CHECK(any_output_contains(collector, "(z acyclic w)")); });
}

// A query evaluates the identical condition list that a rule evaluates, thus
// reaches the loop the same way. Asserted by the number of answers, which
// remains unaffected by the manner in which an answer is printed. With ∗ the
// loop adds nothing: zero steps and one step reach the same node.
TEST_CASE("path condition: a query reaches a loop of one step, in both directions")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kLoop);

        for (const char* query : {"k P279⁺ X", "X P279⁺ k", "k P279∗ X"})
        {
            CAPTURE(query);
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).size() == 1);
        }

        collector.clear();
        interactive.process("X rel Y, k P279⁺ k");
        CHECK(collect_answers(collector).size() == 2);

        collector.clear();
        interactive.process("X rel Y, ¬(k P279⁺ k)");
        CHECK(collect_answers(collector).empty()); });
}

// A walk that passes through its own predicate node met the predicate's
// declaration `q ~ ->` on the way. This fact has the predicate as its
// subject, hence the direct walk read it as an additional step: `q q⁺ X`
// answered `->` beside z, and a rule over the path derived
// `(x above ->)`. The index build already distinguished the declaration by
// its hash.
TEST_CASE("path condition: a walk through its own predicate node does not step along the predicate's declaration")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("q q z");
        interactive.process("x rel q");

        collector.clear();
        interactive.process("q q⁺ X");
        const std::vector<std::string> answers = collect_answers(collector);
        CHECK(answers.size() == 1);
        for (const std::string& answer : answers)
            CHECK(answer.find("->") == std::string::npos);

        collector.clear();
        interactive.process("(R rel S, S q⁺ T) => (R above T)");
        CHECK(any_output_contains(collector, "(x above z)"));
        CHECK_FALSE(any_output_contains(collector, "(x above ->)")); });
}

TEST_CASE("path condition: a ground self-path outside a rule is refused")
{
    // The same ruling as for "a P279⁺ d": with the end concrete there is
    // nothing to ask, and the guard has to fire before the one-step fact is
    // built. What the refusal is worth is that no node named "P279⁺" is left
    // behind -- that node is what the literal reading used to create.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kCycle);
    collector.clear();

    CHECK_THROWS(interactive.process(":P279⁺ a"));

    collector.clear();
    CHECK_THROWS(interactive.process(".node \"P279⁺\""));
    collector.clear();
    interactive.process("X closure Y");
    CHECK(collect_answers(collector).empty());
}

// In a query, the identical ground self-path functions as a condition and a
// reachability test, just like "X ~ cls, a P279⁺ a" does. The walks that find
// a path marker read it exclusively when a predicate is written between two
// values, so ":P279⁺ a" was not considered an operator by them: the query
// built `a P279 a` as data, and "S P279 O" answered it from then on.
TEST_CASE("path condition: a ground self-path in a query is a test, and its one step a pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kCycle);

        collector.clear();
        interactive.process("X ~ cls, :P279⁺ a");
        CHECK(collect_answers(collector).size() == 3);
        collector.clear();
        interactive.process("X ~ cls, :P279⁺ c");
        CHECK(collect_answers(collector).empty());

        // The three edges comprising kCycle, and no step of either self-path.
        collector.clear();
        interactive.process("S P279 O");
        CHECK(collect_answers(collector).size() == 3); });
}

// A condition operator has slots in a RULE, and the checks stood at the top of
// them and nowhere else. One argument in, a marker was built and never read:
// the rule came back printed WITHOUT it -- "(x q (¬(a p b))) => (c r d)" listed
// itself as "(x q (a p b)) => (c r d)" -- and a path marker there tagged a fact
// no closure is ever walked for. The consequence slot had the checks but only
// the shallow ones, so the same shape one argument in derived the pattern the
// operator denied.
TEST_CASE("path condition: an operator inside a rule's condition is refused")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);
    collector.clear();

    // Condition, conjunction member, and consequence, for each of the three
    // operators that have a slot to be misplaced in.
    CHECK_THROWS(interactive.process("(x q (a P279⁺ d)) => (c r d)"));
    CHECK_THROWS(interactive.process("(x q (¬(a P279 d))) => (c r d)"));
    CHECK_THROWS(interactive.process("(X q (¬(a P279 d)), X rel Y) => (Y ok Y)"));
    CHECK_THROWS(interactive.process("(A rel B) => (x q (¬(a P279 d)))"));
    CHECK_THROWS(interactive.process("(A rel B) => (x q (a P279⁺ d))"));

    // The assertion that matters, as ever: nothing of it was built. A rule the
    // parser accepted and then meant differently is what this replaces.
    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "No rules found"));
    collector.clear();
    interactive.process("X closure Y");
    CHECK(collect_answers(collector).empty());
}

// A condition written as a self-fact is a statement where the operand serves
// as an argument, and the walk for an incorrectly positioned operator failed
// to examine it: "(:p ¬(X q b)) => (X r b)" built a rule whose condition
// holds the negation fact as a term, and listed it without the "¬". A query
// goes through the identical traversal.
TEST_CASE("path condition: an operator inside a self-fact condition is refused")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);

    CHECK_THROWS(interactive.process("(:p ¬(X q b)) => (X r b)"));
    CHECK_THROWS(interactive.process("(:p (a P279⁺ d)) => (c r d)"));
    CHECK_THROWS(interactive.process(":p ¬(X q b)"));

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "No rules found"));
    collector.clear();
    interactive.process("X closure Y");
    CHECK(collect_answers(collector).empty());
}

// A self-path one argument down functions as a marker positioned beneath a
// condition, exemplified by "x q (a P279⁺ d)", and it was not recognized as
// such: within a rule it was accepted and tagged a fact for which no closure
// is walked, and within a query "X says (:p⁺ a)" built `a p a` as data.
TEST_CASE("path condition: a self-path inside a condition is refused like any path there")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);
    interactive.process("t says k");

    CHECK_THROWS(interactive.process("(X rel Y, X says (:p⁺ a)) => (X ok Y)"));
    CHECK_THROWS(interactive.process("X says (:p⁺ a)"));
    CHECK_THROWS(interactive.process("x says (:p⁺ a)"));

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "No rules found"));
    collector.clear();
    interactive.process("S p O");
    CHECK(collect_answers(collector).empty());
}

// A set constitutes data, and a self-path among its members is not considered
// a condition either (refer to "a condition operator inside a set literal is
// refused" in test_stratified.cpp). The validation recognized the marker
// solely when a predicate was expressed between two values, hence "{(a p⁺
// c)}" was refused and "{(:p⁺ a)}" was built.
TEST_CASE("path condition: a self-path inside a set literal is refused, in a rule as in a query")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);

    CHECK_THROWS_WITH(interactive.process("(X rel Y, Y in {(:p⁺ a)}) => (X ok Y)"), doctest::Contains("a set is DATA"));
    CHECK_THROWS_WITH(interactive.process("X rel Y, Y in {(:p⁺ X)}"), doctest::Contains("a set is DATA"));

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "No rules found"));
    collector.clear();
    interactive.process("X closure Y");
    CHECK(collect_answers(collector).empty());
}

// Where they DO belong, and every layer they are allowed to wear. A condition
// may be a negation, a neural condition, a path, or a negation of either of
// those; and an inner rule is a rule, so a generator's condition keeps its own
// right to a "¬".
TEST_CASE("path condition: the slots an operator does belong in still take it")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    feed(interactive, kChain);
    interactive.process("y rel d");
    collector.clear();

    interactive.process("(S P279⁺ T) => (S above T)");
    interactive.process("(X rel S, ¬(S P279⁺ d)) => (X clear-of S)");
    interactive.process("(X rel S, ¬(S P279∗ d)) => (X strictly-clear-of S)");
    interactive.process("(P lifts Q) => ((X P Y, ¬(X Q Y)) => (X flagged Y))");

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "=> (S above T)"));
    CHECK(any_output_contains(collector, "=> (X clear-of S)"));
    CHECK(any_output_contains(collector, "=> (X strictly-clear-of S)"));
    CHECK(any_output_contains(collector, "=> (X flagged Y)"));
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

// `.explain` reconstructs the walk a path condition followed. Around a loop of
// one step, that walk amounts to a single fact, where both the subject and the
// object correspond to the starting point of the walk.
TEST_CASE("path condition: .explain reconstructs a walk around a loop of one step")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        feed(interactive, kLoop);
        interactive.process("(X rel S, S P279⁺ T) => (X above T)");

        collector.clear();
        interactive.process(".explain (x above k)");
        CHECK_FALSE(any_output_contains(collector, "no derivation found"));
        CHECK(any_output_contains(collector, "x rel k  [axiom]"));
        CHECK(any_output_contains(collector, "[closure]")); });
}
