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
#include <fstream>
#include <iterator>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// A contradiction is written INTO the graph.
//
// Why it had to be: a derived FACT is quiet on the second run because the
// graph holds it. `!` held nothing -- the consequence is the single core
// Contradiction node, the same node for every instantiation -- so a
// contradiction came back on every later input line. A per-run hash set kept
// one run tidy and could not do more; a cross-run one would have been
// knowledge kept BESIDE the network, which is the one thing zelph does not do.
//
// The record is the refuted SET of the facts that matched: "these statements
// do not hold together". Nothing is retracted, and nothing is created but the
// set node. A set constant is content-addressed and order-independent, so the
// same contradiction yields the same node however it was reached -- that is
// where the quiet second run comes from.
// ---------------------------------------------------------------------------

namespace
{
    std::size_t contradiction_lines(const zelph::io::OutputCollector& collector)
    {
        std::size_t n = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).rfind("! ⇐", 0) == 0) ++n;
        return n;
    }

    // The graph's stored records, sorted, each printed as their respective set.
    // A record answers no query, thus they are extracted from the extent of the
    // marking's predicate, just as zelph/sources reads it.
    std::vector<std::string> recorded_contradictions(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process(R"(%(each s (zelph/sources "~" "refuted") (zelph/out (string/format "REC %q" s))))");
        const std::string        prefix = "REC <zelph/node {";
        std::vector<std::string> records;
        for (const auto& e : collector.events())
        {
            const std::string line = normalize(e.text);
            if (line.rfind(prefix, 0) == 0 && line.back() == '>') records.push_back(line.substr(prefix.size() - 1, line.size() - prefix.size()));
        }
        std::sort(records.begin(), records.end());
        return records;
    }
}

TEST_CASE("contradiction record: reported once, and the next run is quiet")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
        CHECK(contradiction_lines(collector) == 1);

        // The line that used to bring it back. Every later input starts a run
        // that re-derives everything, and the contradiction has no result node
        // that hash-consing could collapse.
        collector.clear();
        interactive.process("unrelated rel thing");
        CHECK(contradiction_lines(collector) == 0);

        collector.clear();
        interactive.run(true, false, false);
        CHECK(contradiction_lines(collector) == 0); });
}

// The record is keyed on the FACTS, not on (rule, bindings) -- which a cache
// could not have been. Two rules that contradict on the same statements make
// the same claim, so they report once between them. This test is red under any
// design that keys the record on the rule.
TEST_CASE("contradiction record: two rules on the same facts report once between them")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
(S q O, S p O) => !
)");
        CHECK(contradiction_lines(collector) == 1); });
}

TEST_CASE("contradiction record: nothing is retracted, and every member still answers")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
        // The two facts stay asserted. The record says they do not hold
        // TOGETHER; each of them individually is what the user entered.
        collector.clear();
        interactive.process("A p y");
        CHECK(answers_contain(collector, "x p y"));

        collector.clear();
        interactive.process("A q y");
        CHECK(answers_contain(collector, "x q y"));

        // And so does the conjunctive question, which is the shape that could
        // have collided: its answer is rendered as the very set the record
        // refutes. It is the substituted PATTERN, not a lookup of that node,
        // and the two members match individually.
        collector.clear();
        interactive.process("A p y, A q y");
        const std::vector<std::string> both = collect_answers(collector);
        REQUIRE_FALSE(both.empty());
        CHECK(both.front().find("x p y") != std::string::npos);
        CHECK(both.front().find("x q y") != std::string::npos); });
}

// The record represents the engine's internal account of a contradiction,
// not a declaration made by anyone: its members answer, as demonstrated in
// the prior case, while the record itself does not. `S ~ refuted` was
// answered by `{(x p y) (x q y)} ~ refuted`, `S in O` by the two memberships
// within its set, and a rule over memberships derived `(x p y) flagged yes`
// from the record alone. A load reads the record back as a refuted set
// constant, ensuring the round trip yields identical results.
TEST_CASE("contradiction record: the record itself answers no query and matches no condition")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_record_unanswered.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
        REQUIRE(contradiction_lines(collector) == 1);
        REQUIRE(recorded_contradictions(collector, interactive) == std::vector<std::string>{"{(x p y) (x q y)}"});

        SUBCASE("in the session") {}
        SUBCASE("after .save and .load")
        {
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
            interactive.process(".auto-run"); // a load disables it
        }

        CHECK(answers("S ~ refuted").empty());
        CHECK(answers("S in O").empty());
        interactive.process("(M in C) => (M flagged yes)");
        CHECK(answers("S flagged O").empty());
        CHECK(answers("A p y") == std::vector<std::string>{"x p y"});
        CHECK(recorded_contradictions(collector, interactive) == std::vector<std::string>{"{(x p y) (x q y)}"}); });
    std::filesystem::remove(file);
}

// A set constant consists solely of its content, meaning that a record of a
// contradiction and a set a statement names based on identical facts
// constitute a single node. The record hid the memberships of that set as
// its own: following a contradiction, `{(x p y) (x q y)} label foo` no
// longer answered `S in O`, and `(R go k, M in R) => (M member yes)`
// derived nothing. A set named by a statement is that statement's set, with
// its memberships serving as data; only the record's marking remains the
// record's own and stays hidden.
TEST_CASE("contradiction record: a set a statement names keeps its memberships, and only the marking is the record's")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_record_named_set.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::sort(result.begin(), result.end());
            return result;
        };
        const auto stated = [&](const std::string& lines)
        {
            process_lines(interactive, lines);
            SUBCASE("in the session") {}
            SUBCASE("after .save and .load")
            {
                interactive.process(".save \"" + file.string() + "\"");
                interactive.process(".load \"" + file.string() + "\"");
                interactive.process(".auto-run"); // a load disables it
            }
        };

        std::string holder;
        SUBCASE("a statement about the set, written before the contradiction")
        {
            holder = "{(x p y) (x q y)} label foo";
            stated("x p y\nx q y\n" + holder + "\n(A p B, A q B) => !\n");
        }
        SUBCASE("a statement about the set, written after the contradiction")
        {
            holder = "{(x p y) (x q y)} label foo";
            stated("x p y\nx q y\n(A p B, A q B) => !\n" + holder + "\n");
        }
        SUBCASE("a statement whose object is the set")
        {
            holder = "k has {(x p y) (x q y)}";
            stated("x p y\nx q y\n(A p B, A q B) => !\n" + holder + "\n");
        }
        CAPTURE(holder);

        REQUIRE(recorded_contradictions(collector, interactive) == std::vector<std::string>{"{(x p y) (x q y)}"});
        CHECK(answers("S in O") == std::vector<std::string>{"(x p y) in {(x p y) (x q y)}", "(x q y) in {(x p y) (x q y)}"});
        CHECK(answers("S ~ refuted").empty());

        process_lines(interactive, "(M in C) => (M member yes)\n(R ~ refuted) => (R flagged yes)\n");
        CHECK(answers("S member O") == std::vector<std::string>{"(x p y) member yes", "(x q y) member yes"});
        CHECK(answers("S flagged O").empty());

        // The record continues to keep a later run from signalling the
        // contradiction once more.
        collector.clear();
        interactive.process(".run");
        CHECK(contradiction_lines(collector) == 0); });
    std::filesystem::remove(file);
}

// A membership that names the set beside a second container is a
// statement someone wrote, not among the memberships constituting the set:
// a record writes each of its members into the set alone. The record took
// the statement as one of its own, thus after the contradiction `(x p y) in
// bag {(x p y) (x q y)}` answered neither `S in bag` nor `S in O`, and
// `(M in bag) => (M bagged yes)` derived nothing. As with any statement
// that names the set, it makes the set its own, and the memberships answer.
TEST_CASE("contradiction record: a membership naming the set beside another container is a statement about the set")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_record_two_containers.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::sort(result.begin(), result.end());
            return result;
        };
        const auto stated = [&](const std::string& lines)
        {
            process_lines(interactive, lines);
            SUBCASE("in the session") {}
            SUBCASE("after .save and .load")
            {
                interactive.process(".save \"" + file.string() + "\"");
                interactive.process(".load \"" + file.string() + "\"");
                interactive.process(".auto-run"); // a load disables it
            }
        };

        const std::string holder = "(x p y) in bag {(x p y) (x q y)}";
        SUBCASE("written before the contradiction") { stated("x p y\nx q y\n" + holder + "\n(A p B, A q B) => !\n"); }
        SUBCASE("written after the contradiction") { stated("x p y\nx q y\n(A p B, A q B) => !\n" + holder + "\n"); }

        REQUIRE(recorded_contradictions(collector, interactive) == std::vector<std::string>{"{(x p y) (x q y)}"});
        CHECK(answers("S in bag") == std::vector<std::string>{"(x p y) in bag"});
        CHECK(answers("(x q y) in O") == std::vector<std::string>{"(x q y) in {(x p y) (x q y)}"});
        CHECK(answers("S ~ refuted").empty());

        process_lines(interactive, "(M in bag) => (M bagged yes)\n");
        CHECK(answers("S bagged O") == std::vector<std::string>{"(x p y) bagged yes"}); });
    std::filesystem::remove(file);
}

// The memberships of a record turn into data the moment a statement first
// names its set, and these memberships predate that statement, meaning no
// seed offered them to a rule concerning memberships: `.run-delta`
// following `{(x p y) (x q y)} label foo`, typed between two runs or
// derived by the seeded run, missed `(x p y) member yes`, and check mode
// reported a completeness violation. `.semi-naive on` performs no
// verification pass and simply lacked those two facts.
TEST_CASE("contradiction record: a statement that names the set offers its memberships to a seeded run")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::sort(result.begin(), result.end());
            return result;
        };

        const auto seeded = [&]
        {
            interactive.process(".auto-run");
            process_lines(interactive, "x p y\nx q y\n(A p B, A q B) => !\n.run\n(M in C) => (M member yes)\n");
            SUBCASE("a statement entered between two runs")
            {
                process_lines(interactive, ".run\n{(x p y) (x q y)} label foo\n");
            }
            SUBCASE("a statement the seeded run derives")
            {
                process_lines(interactive, "(K go Z) => ({(x p y) (x q y)} label Z)\n.run\nk go z\n");
            }
            REQUIRE(answers("S member O").empty());

            collector.clear();
            interactive.process(".run-delta");
            CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));
            CHECK_FALSE(any_event_contains(collector, "missed by delta seeding"));
            CHECK(answers("S member O") == std::vector<std::string>{"(x p y) member yes", "(x q y) member yes"});
        };

        SUBCASE("check mode") { seeded(); }
        SUBCASE(".semi-naive on")
        {
            interactive.process(".semi-naive on");
            seeded();
        } });
}

// On the opposite side of the line: a statement that holds a variable names no
// set, meaning a query over the record's set leaves the record the engine's
// own. The pattern of a Janet query remains in the graph, in contrast to the
// pattern of a typed question, and if it had counted, posing a question about
// the record would have turned its memberships into data.
TEST_CASE("contradiction record: a query that names the record's set leaves it hidden")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
%(def rec (first (zelph/sources "~" "refuted")))
%(zelph/out (string "IN-" (length (zelph/query (zelph/fact 'S "in" rec)))))
%(zelph/out (string "ABOUT-" (length (zelph/query (zelph/fact rec 'P 'O)))))
)");
        CHECK(any_output_contains(collector, "IN-0"));
        CHECK(any_output_contains(collector, "ABOUT-0"));

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty());
        interactive.process("(M in C) => (M flagged yes)");
        collector.clear();
        interactive.process("S flagged O");
        CHECK(collect_answers(collector).empty()); });
}

// A record constitutes a `~` fact, meaning a rule that matched `~` facts as
// its condition and refuted a claimed fact met the record of every
// contradiction it caused as a new candidate, made a contradiction of it and
// a record of that, and the run never ended: with the `refuted` the
// consequence names, with one that a binding takes from a refutation the user
// wrote, and with a rule that refutes the members of a set. Each pass below
// must leave the graph exactly as the first one left it, so an engine that
// feeds the records back fails at the REQUIRE instead of stalling during the
// subsequent run. That run, in check mode, and the one following a load
// conclude and write nothing.
TEST_CASE("contradiction record: a rule that meets the records of its own contradictions ends its run")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_record_feedback.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        std::string              lines;
        std::vector<std::string> markings;
        SUBCASE("the refuted the consequence names") { lines = "t p b\n(S ~ Y) => ((t p b) ~ refuted)\n"; }
        SUBCASE("the refuted a binding takes from a refutation")
        {
            lines    = "¬(a p b)\nt p b\n(S ~ Y) => ((t p b) ~ Y)\n";
            markings = {"(a p b) ~ refuted"};
        }
        SUBCASE("a rule that refutes the members of a set") { lines = "t p b\nt q b\n(A p B, A q B) => !\n(M in C) => (M ~ refuted)\n"; }

        const auto counts = counts_over_passes(collector, interactive, lines);
        REQUIRE(counts == std::vector<std::size_t>(4, counts.front()));

        interactive.process(".semi-naive check");
        interactive.run(true, false, false);
        CHECK(node_count(collector, interactive) == counts.front());

        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        const std::size_t loaded = node_count(collector, interactive);
        interactive.run(true, false, false);
        CHECK(node_count(collector, interactive) == loaded);

        CHECK(answers("S p O") == std::vector<std::string>{"t p b"});
        CHECK(answers("S ~ refuted") == markings); });
    std::filesystem::remove(file);
}

// Instantiating a `!=` condition would ASSERT it: instantiate_fact ends in
// Zelph::fact, so the record would enter `(bright != dark)` as a claim of the
// core `!=` predicate that nobody made -- the family of d749fbf, where asking
// created the answer. A member that matched no fact contributes nothing.
TEST_CASE("contradiction record: a guard condition is not turned into a fact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
bright "is opposite of" dark
yellow ~ bright
(X "is opposite of" Y, A ~ X, A ~ Y, X != Y) => !
yellow ~ dark
)");
        CHECK(contradiction_lines(collector) == 1);

        // The premise line still NAMES the guard -- that is the condition
        // rendered with its bindings, and it is right. What must not exist is
        // a fact.
        collector.clear();
        interactive.process("S != O");
        CHECK(collect_answers(collector).empty());

        // Still reported once, guard or no guard.
        collector.clear();
        interactive.process("harmless rel statement");
        CHECK(contradiction_lines(collector) == 0); });
}

TEST_CASE("contradiction record: a negated condition contributes nothing and breaks nothing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x p y
(A p B, ¬(A safe B)) => !
)");
        CHECK(contradiction_lines(collector) == 1);

        collector.clear();
        interactive.process("harmless rel statement");
        CHECK(contradiction_lines(collector) == 0);

        // The negation matched by ABSENCE, so nothing about `safe` was
        // created: the record is the one positive member.
        collector.clear();
        interactive.process("S safe O");
        CHECK(collect_answers(collector).empty()); });
}

// A record names the facts its rule's conditions matched, as they stand. A
// condition regarding the marking of a refuted fact was built through fact(),
// which refused the fact that the marking names as known to be wrong:
// following "¬(t p b)" and "t go k", the rule
// "((X p b) ~ refuted, X go Y) => (X p b)" ended the run due to that error,
// and the input after it was not read, whereas the identical rule with solely
// the marking as its condition is reported as the contradiction it
// constitutes. A rule that either concludes or denies a fact previously
// refuted is a line any user may enter.
TEST_CASE("contradiction record: a condition that holds a refuted fact is recorded as it stands")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "¬(t p b)\nt go k\n");
        std::string rule;
        std::string member = "((t p b) ~ refuted)";
        SUBCASE("a consequence refuted before") { rule = "((X p b) ~ refuted, X go Y) => (X p b)"; }
        SUBCASE("a denial") { rule = "((X p b) ~ refuted, X go Y) => !"; }
        // The argument supplied to an opaque predicate holds the refuted
        // fact as a mention, and the record built it through fact() just the
        // same.
        SUBCASE("an opaque statement that holds the refuted fact")
        {
            process_lines(interactive, "believes ~ opaque\nk believes (t p b)\n");
            rule   = "((K believes (X p b)), X go Y) => (X p b)";
            member = "(k believes (t p b))";
        }
        collector.clear();
        CHECK_NOTHROW(interactive.process(rule));
        CHECK(contradiction_lines(collector) == 1);

        collector.clear();
        CHECK_NOTHROW(interactive.process("harmless rel statement"));
        CHECK(contradiction_lines(collector) == 0);

        const std::vector<std::string> records = recorded_contradictions(collector, interactive);
        REQUIRE(records.size() == 1);
        CHECK(records.front().find(member) != std::string::npos);
        CHECK(records.front().find("(t go k)") != std::string::npos);
        CHECK_THROWS_WITH(interactive.process("t p b"), doctest::Contains("this fact is known to be wrong")); });
}

// Recording a contradiction asserts nothing. A condition that matched a fact
// through more than it names was built for the record, and the record named a
// fact that nobody had asserted, which answered from then on.
TEST_CASE("contradiction record: a condition is recorded with the fact it matched, which nobody built")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string query;
        std::string matched;
        std::vector<std::string> answers;
        // `X p Y` matches both objects in `a p b c`. The record built
        // `a p b`, and `S p O` answered it a second time.
        SUBCASE("a fact with more objects than the condition")
        {
            process_lines(interactive, "a p b c\n(X p Y, X q Y) => !\n");
            query   = "S p O";
            matched = "(a p b c)";
            answers = {"a p b", "a p c"};
            collector.clear();
            interactive.process("a q b");
        }
        // The set in the condition matches `{a b}` by its members. The record
        // built the condition using its set as written, and `S has O`
        // answered `k has @{b}`.
        SUBCASE("a set that the condition matched by its members")
        {
            process_lines(interactive, "k has {a b}\n(K has {X b}, X q z) => !\n");
            query   = "S has O";
            matched = "(k has {a b})";
            answers = {"k has {a b}"};
            collector.clear();
            interactive.process("a q z");
        }
        // The condition names `a p b`, which is refuted, and matched
        // `a p b c`, because Unification passes over a refuted fact. The
        // look-up found `a p b`, and the record named a fact that the rule
        // had not matched; built through fact(), it ended the run as known
        // to be wrong.
        SUBCASE("a fact with more objects beside a refuted fact the condition names")
        {
            process_lines(interactive, "¬(a p b)\na p b c\n(X p Y, X q Y) => !\n");
            query   = "S p O";
            matched = "(a p b c)";
            answers = {"a p b", "a p c"};
            collector.clear();
            interactive.process("a q b");
        }
        // The same applies to a rule's pattern, which no condition matches
        // either: the record named `a p b`, the pattern of
        // `(a p b) => (c q d)`, in place of the fact `a p b c` that the
        // condition had matched.
        SUBCASE("a fact with more objects beside a rule's pattern the condition names")
        {
            process_lines(interactive, "(a p b) => (c q d)\na p b c\n(X p Y, X q Y) => !\n");
            query   = "S p O";
            matched = "(a p b c)";
            answers = {"a p b", "a p c"};
            collector.clear();
            interactive.process("a q b");
        }
        // The identical reading within a record: the condition
        // `(X p Y) bad Z`, re-matched under the firing's bindings, was
        // grounded to `(a p b) bad k`, which constitutes no fact, and found
        // nothing, so the record stayed unnamed and the contradiction was
        // re-reported at every line.
        SUBCASE("a nested statement with more objects beside the narrower fact")
        {
            process_lines(interactive, "a p b c\n(a p b c) bad k\na p b\n(((X p Y) bad Z), X q Y) => !\n");
            query   = "S bad O";
            matched = "((a p b c) bad k)";
            answers = {"(a p b c) bad k"};
            collector.clear();
            interactive.process("a q b");
        }
        // A stored rule is matched by a held rule whose conditions form a
        // comma list, by the members of its conditions. The look-up found
        // the held rule itself, which no statement names, causing the record
        // to stay unnamed and the contradiction to return at every line;
        // built through fact(), the record said that x proposed the held
        // rule, and `S proposed O` answered twice.
        SUBCASE("a stored rule that a held rule matched by the members of its conditions")
        {
            process_lines(interactive, "x proposed ((c q d, a p b) => (alarm is on))\n(P proposed ((a p b, c q d) => (alarm is on)), P q z) => !\n");
            query   = "S proposed O";
            matched = "(x proposed";
            answers = {"x proposed (((a p b), (c q d)) => (alarm is on))"};
            collector.clear();
            interactive.process("x q z");
        }
        CHECK(contradiction_lines(collector) == 1);

        collector.clear();
        interactive.process(query);
        std::vector<std::string> got = collect_answers(collector);
        std::sort(got.begin(), got.end());
        CHECK(got == answers);
        CHECK(contradiction_lines(collector) == 0);

        const std::vector<std::string> records = recorded_contradictions(collector, interactive);
        REQUIRE(records.size() == 1);
        CHECK(records.front().find(matched) != std::string::npos); });
}

// The record is wired to its members, so it dies with them -- which is what a
// cache could never do. Once the data that caused the contradiction is gone,
// the same contradiction is a NEW finding again if it ever returns.
TEST_CASE("contradiction record: it dies with the facts that caused it")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
    REQUIRE(contradiction_lines(collector) == 1);

    collector.clear();
    interactive.process(".prune-facts (x p y)");

    // Entered again, it is found again -- and reported again, because the
    // record went with the fact it was about.
    collector.clear();
    interactive.process("x p y");
    CHECK(contradiction_lines(collector) == 1);
}

TEST_CASE("contradiction record: it survives a save and a load")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_roundtrip.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
        REQUIRE(contradiction_lines(collector) == 1);
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    interactive.process(".auto-run"); // a load disables it
    collector.clear();

    // The marking fact travelled, and rebuild_refuted_index read it back --
    // the probability alone could not have carried this, since the weight
    // store is keyed by a hash of the edge.
    interactive.run(true, false, false);
    CHECK(contradiction_lines(collector) == 0);
}

// The quiet second run is the design, and the test above pins it. What the
// design did NOT account for is the same silence after a .load: the record is a
// fact, so a network saved after a run carries every contradiction it found,
// and the next run over that file meets all of them as already known. No line,
// no count, and a summary reading "0 contradictions found" over data that
// contradicts itself in a hundred places. That is not "quiet", it is wrong --
// and it lands exactly on the reproducibility recipe, where someone downloads a
// published network and runs the rules over it.
//
// The lines stay away, because a contradiction the graph holds is not a new
// finding. The COUNT is what has to speak.
// The counterpart of the case below: the run that FINDS a contradiction says
// nothing about already-known ones, so an ordinary first run is unchanged.
TEST_CASE("contradiction record: the run that finds one adds no note")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".auto-run"); // off, so the explicit run is the finder
    process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
    REQUIRE(contradiction_lines(collector) == 0);

    collector.clear();
    interactive.run(true, false, false);
    CHECK(contradiction_lines(collector) == 1);
    CHECK(any_event_contains(collector, "1 contradictions found."));
    CHECK_FALSE(any_event_contains(collector, "already recorded"));
}

TEST_CASE("contradiction record: a run says what it met but did not announce")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_known_count.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
        REQUIRE(contradiction_lines(collector) == 1);

        // Auto-run already found it while the lines went in, so this run meets
        // the same contradiction as an already-known one: no line, and the
        // count says why it is silent instead of reading as a clean graph.
        collector.clear();
        interactive.run(false, false, false);
        CHECK(contradiction_lines(collector) == 0);
        CHECK(any_event_contains(collector, "0 contradictions found (1 already recorded in this network)."));

        interactive.process(".save \"" + file.string() + "\"");
    }

    // The case this exists for. A published network saved after a run carries
    // its records, so every contradiction in it is already known on the very
    // first run over the file -- and the summary used to read "0 contradictions
    // found." over data that contradicts itself in as many places as it does.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    interactive.process(".auto-run"); // a load disables it
    collector.clear();

    interactive.run(false, false, false);

    CHECK(contradiction_lines(collector) == 0); // the lines stay away, by design
    CHECK(any_event_contains(collector, "0 contradictions found (1 already recorded in this network)."));
}

// The number says how many contradictions the NETWORK holds, so it has to be
// read off the graph rather than counted as the run meets them. A symmetric
// rule is the case that separates the two: (A p B, B p A) => ! matches the
// pair in both directions, so a run MEETS the contradiction twice -- but a
// record is content-addressed and both instantiations reach the same set node,
// so the graph holds one. Counting encounters reported two, which is the one
// thing this note must not do: it is the number a Wikidata report would carry,
// and an inflated violation count reads as an unusable tool.
//
// The same property is what the export documentation tells its readers -- count
// violations by deduplicating on the premise set, not by counting lines.
TEST_CASE("contradiction record: a contradiction met twice is one record")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
x p y
y p x
(A p B, B p A) => !
)");
    REQUIRE(contradiction_lines(collector) == 1); // announced once, as ever

    collector.clear();
    interactive.run(false, false, false);
    CHECK(any_event_contains(collector, "0 contradictions found (1 already recorded in this network)."));
    CHECK_FALSE(any_event_contains(collector, "2 already recorded"));
}

// A contradiction record is a refuted SET constant; `¬(a p b)` refutes the
// relation node of an ordinary fact. Both live in the same refuted index, so
// the count has to tell them apart -- otherwise every hand-written refutation
// in a published network would be reported as a contradiction it never was.
TEST_CASE("contradiction record: a refutation written by hand is not one")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
¬(a p b)
¬(c p d)
x p y
y p x
(A p B, B p A) => !
)");
    REQUIRE(contradiction_lines(collector) == 1);

    collector.clear();
    interactive.run(false, false, false);
    CHECK(any_event_contains(collector, "0 contradictions found (1 already recorded in this network)."));
    CHECK_FALSE(any_event_contains(collector, "3 already recorded"));
}

// Read at the START of the run, not at its end: a contradiction this run
// announces is a finding of THIS run and must not also appear as one that was
// already there. The two numbers then add up to what the graph holds
// afterwards, which is what makes the sentence readable at all.
TEST_CASE("contradiction record: what this run found is not also 'already recorded'")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
x p y
y p x
(A p B, B p A) => !
)");
    REQUIRE(contradiction_lines(collector) == 1);

    interactive.process(".auto-run"); // off, so the run below is the finder
    process_lines(interactive, R"(
u p v
v p u
)");

    collector.clear();
    interactive.run(true, false, false);
    CHECK(contradiction_lines(collector) == 1); // the new pair, announced
    CHECK(any_event_contains(collector, "1 contradictions found (1 already recorded in this network)."));
}

// A rule with a SINGLE condition has no condition set to point at, so
// record_contradiction has nothing to record and returns "new" every time.
// That is why it is announced on every run -- and why it must not appear in
// the count either: the note is about what the graph HOLDS, and it holds
// nothing here. Pinning it keeps the sentence exact rather than approximately
// true.
TEST_CASE("contradiction record: a single-condition rule leaves nothing to count")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    // The quantity that is counted is the announcements of a single
    // evaluation. In check mode, a second classic run follows the initial
    // one, and a contradiction lacking a record is declared once more during
    // this process.
    interactive.process(".semi-naive on");
    process_lines(interactive, R"(
x p y
(A p B) => !
)");
    REQUIRE(contradiction_lines(collector) == 1);

    collector.clear();
    interactive.run(true, false, false);
    CHECK(contradiction_lines(collector) == 1); // announced again, no record
    CHECK(any_event_contains(collector, "1 contradictions found."));
    CHECK_FALSE(any_event_contains(collector, "already recorded"));
}

// The way out of the case above, and a promise a program can build on: a
// rule possessing a single condition within a condition set of its own --
// the collection of that condition, labelled `~ conjunction`, the `=>` fact
// built over it -- has a member to point at, thus its contradiction is
// recorded just like any other, and the next run is quiet. A program
// requiring a record for a rule with a solitary condition constructs the
// condition in this manner, since the plain spelling provides none;
// constructed within zelph/dedup-rule, as a typed rule is.
TEST_CASE("contradiction record: a one-condition rule built as a one-element condition set is recorded once")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(R"js(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def c (zelph/collection (zelph/fact x "~" "man"))) (zelph/fact c "~" "conjunction") (zelph/fact c "=>" (zelph/resolve "!")))))js");
    interactive.process("john ~ man");
    REQUIRE(contradiction_lines(collector) == 1);

    collector.clear();
    interactive.run(true, false, false);
    CHECK(contradiction_lines(collector) == 0);
    CHECK(any_event_contains(collector, "0 contradictions found (1 already recorded in this network)."));
}

// The rule above printed as `((X ~ man)) => !`, a comma list containing a
// single item, and the parser reads `((X ~ man))` as a grouping: when typed
// back into a network that did not hold the rule, the line was the plain rule,
// whose contradictions are never recorded. It prints in the explicit form,
// which builds the one-element condition set once more.
TEST_CASE("contradiction record: the printed one-condition set re-enters as the same rule")
{
    std::string printed;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(R"js(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def c (zelph/collection (zelph/fact x "~" "man"))) (zelph/fact c "~" "conjunction") (zelph/fact c "=>" (zelph/resolve "!")))))js");
        collector.clear();
        interactive.process(".list-rules");
        for (const auto& e : collector.events())
            if (e.text.find("=>") != std::string::npos) printed = normalize(e.text);
        CHECK(printed == "(*{(X ~ man)} ~ conjunction) => !");

        // Within the same network, the line serves as the rule it lists.
        interactive.process(printed);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(count_outputs_containing(collector, "=>") == 1);
    }

    // In a network that does not hold it, the line builds it: listed as it
    // was printed, and recorded.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(printed);
    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, printed));

    interactive.process("john ~ man");
    REQUIRE(contradiction_lines(collector) == 1);
    collector.clear();
    interactive.run(true, false, false);
    CHECK(contradiction_lines(collector) == 0);
    CHECK(any_event_contains(collector, "1 already recorded in this network"));
}

// With the records off there is nothing in the graph to read, so the note has
// to stay away entirely -- and the contradiction comes back on every run, which
// is what the switch is for. A note appearing here would describe a network
// that was asked not to hold any.
TEST_CASE("contradiction record: with records off the note stays away")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".contradiction-records off");
    process_lines(interactive, R"(
x p y
y p x
(A p B, B p A) => !
)");
    REQUIRE(contradiction_lines(collector) >= 1);

    collector.clear();
    interactive.run(true, false, false);
    CHECK(contradiction_lines(collector) >= 1);
    CHECK_FALSE(any_event_contains(collector, "already recorded"));
}

// The test this pair replaces, "a hand-written one means the same before and
// after a round trip", pinned that the marking `*(a p b) ~ refuted` manually
// written reached the refuted index exactly as written, thereby aligning
// the live index with the one reconstructed upon load (7329c20). This
// alignment was achieved by honouring the marking at creation, and that
// un-claimed `a p b`, which had previously been asserted. A marking line now
// means what `¬(a p b)` means: refused for a fact the graph claims, written
// as the refutation for one it does not. The round trip remains consistent,
// and that half is pinned below.
TEST_CASE("refuted marking: a hand-written one of a claimed fact is refused")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, "a p b\nc p d\n");

    CHECK_THROWS_WITH(interactive.process("*(a p b) ~ refuted"), doctest::Contains("this fact is known to be true"));
    CHECK_THROWS_WITH(interactive.process("(a p b) ~ refuted"), doctest::Contains("this fact is known to be true"));

    collector.clear();
    interactive.process("S p O");
    CHECK(collect_answers(collector).size() == 2);
    CHECK(answers_contain(collector, "a p b"));
    CHECK(answers_contain(collector, "c p d"));
}

// A group of a single value equates to the value it contains, yet the line
// marking a fact within a standalone group, or its focus within such a
// group, was not read as a refutation: it was constructed using zelph/fact
// and refuted the fact the graph claimed, just as the identical marking
// held by a statement did.
TEST_CASE("refuted marking: one of a claimed fact in a group of its own is refused too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\nc p d");
        std::string line;
        SUBCASE("the fact in a group") { line = "((a p b)) ~ refuted"; }
        SUBCASE("the focus in a group") { line = "(*(a p b)) ~ refuted"; }
        SUBCASE("the focus on a group") { line = "*((a p b)) ~ refuted"; }
        SUBCASE("held by a statement") { line = "x says (((a p b)) ~ refuted)"; }
        // A focus on an entire value hands on that value, just as a group
        // of a single value hands on the value it holds.
        SUBCASE("the focus on a focus") { line = "*(*(a p b)) ~ refuted"; }
        SUBCASE("the focus on a focus in a group") { line = "(*(*(a p b))) ~ refuted"; }

        CHECK_THROWS_WITH(interactive.process(line), doctest::Contains("this fact is known to be true"));

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).size() == 2);
        CHECK(answers_contain(collector, "a p b"));
        CHECK(answers_contain(collector, "c p d")); });
}

// A statement may carry a fact by reference -- a Janet variable `,f`, or the
// node produced by an inline keyword -- and the parser is unable to determine
// which specific fact is meant, hence the marking written on that line was
// constructed using zelph/fact rather than zelph/refute, which refuses the
// refutation of a fact that has been claimed. Following `a p b` and
// `%(def f (zelph/fact "a" "p" "b"))`, each line below echoed
// `(a p b) ~ refuted` and took the claimed `a p b` from every query. The fact
// is claimed in the manner a user claims one, and handing a node on by
// reference is precisely the purpose of a Janet variable and an inline
// keyword.
TEST_CASE("refuted marking: one of a claimed fact a statement holds by reference is refused")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // `¬(m n o)` creates the node `refuted`, which is part of the
        // engine's vocabulary and remains preserved once a line names it
        // (Zelph::node), ensuring the count evaluates exclusively what the
        // refused line authored.
        process_lines(interactive, "¬(m n o)\na p b\n" R"js(%(def f (zelph/fact "a" "p" "b")))js");
        std::string line;
        SUBCASE("the reference marked") { line = ",f ~ refuted"; }
        SUBCASE("the reference in a group") { line = "(,f) ~ refuted"; }
        SUBCASE("a focus on the reference") { line = "*,f ~ refuted"; }
        SUBCASE("the focus in a group") { line = "(*,f) ~ refuted"; }
        SUBCASE("the reference a focus hands on") { line = "(x says *,f) ~ refuted"; }
        SUBCASE("held by a statement") { line = "x says (,f ~ refuted)"; }
        SUBCASE("in a comma list without variables") { line = "c q d, ,f ~ refuted"; }
        SUBCASE("the node an inline keyword returns")
        {
            interactive.process(R"js(%(zelph/register-keyword "F<" ">" (fn [text] f)))js");
            line = "F<> ~ refuted";
        }
        // The handler for an inline keyword writes the marking directly: the
        // term belongs to the statement, not forming a separate Janet block.
        SUBCASE("a marking an inline keyword writes")
        {
            interactive.process(R"js(%(zelph/register-keyword "M<" ">" (fn [text] (zelph/fact f "~" "refuted"))))js");
            line = "x says M<>";
        }
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process(line), doctest::Contains("this fact is known to be true"));
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(m n o) ~ refuted"});
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

// A derived fact holds just as a claimed one does: the typed line
// `(t q k) ~ refuted` is refused for it, and the identical marking of the
// fact that a Janet variable holds refuted it, following which each run
// reported the rule's firing as a contradiction.
TEST_CASE("refuted marking: one of a derived fact a statement holds by reference is refused as its typed line is")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(X go Y) => (X q Y)\nt go k\n" R"js(%(def g (zelph/mention "t" "q" "k")))js");
        CHECK_THROWS_WITH(interactive.process("(t q k) ~ refuted"), doctest::Contains("this fact is known to be true"));
        CHECK_THROWS_WITH(interactive.process(",g ~ refuted"), doctest::Contains("this fact is known to be true"));

        collector.clear();
        interactive.run(true, false, false);
        CHECK(contradiction_lines(collector) == 0);
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"t q k"});
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty()); });
}

// The refusal concerns a fact that the graph claims. A rule's pattern, or a
// fact that a mention wrote and no one claimed, is refuted via a reference,
// as its typed line refutes it. The marking of a fact that ¬ has previously
// refuted is accepted again, without effect.
TEST_CASE("refuted marking: one of a fact nobody claimed that a statement holds by reference refutes it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a rule's pattern") { interactive.process("(a p b) => (c d e)"); }
        SUBCASE("a mention") {}
        SUBCASE("a refuted fact") { interactive.process("¬(a p b)"); }
        interactive.process(R"js(%(def g (zelph/mention "a" "p" "b")))js");

        CHECK_NOTHROW(interactive.process(",g ~ refuted"));

        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a p b) ~ refuted"});
        CHECK_THROWS_WITH(interactive.process("a p b"), doctest::Contains("this fact is known to be wrong"));
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S d O");
        CHECK(collect_answers(collector).empty()); });
}

// Within an argument of an opaque predicate, the marking constitutes a
// mention and does not refute anything, regardless of how the statement holds
// its fact.
TEST_CASE("refuted marking: one of a claimed fact by reference in an opaque argument is a mention")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\nbelieves ~ opaque\n" R"js(%(def f (zelph/fact "a" "p" "b")))js");

        CHECK_NOTHROW(interactive.process("john believes (,f ~ refuted)"));

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S believes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john believes ((a p b) ~ refuted)"}); });
}

// The second half of the test mentioned earlier: when a fact is not claimed
// by anyone, the marking serves as a refutation, and the network answers the
// same both before and following a save-and-load cycle -- the one thing a
// round trip must never break.
TEST_CASE("refuted marking: a hand-written one of an unclaimed fact means the same before and after a round trip")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_refuted_by_hand.bin";

    const auto answers_for_p = [](zelph::io::OutputCollector&  collector,
                                  zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process("S p O");
        return collect_answers(collector);
    };

    std::vector<std::string> before;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
c p d
*(a p b) ~ refuted
)");
        before = answers_for_p(collector, interactive);
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    const std::vector<std::string> after = answers_for_p(collector, interactive);

    CHECK(before == after);
    CHECK(after == std::vector<std::string>{"c p d"});

    // And it is the refutation `¬(a p b)` writes: asserting the fact is
    // refused as it is after `¬`.
    CHECK_THROWS_WITH(interactive.process("a p b"), doctest::Contains("this fact is known to be wrong"));

    std::filesystem::remove(file);
}

// A marking that a claimed statement writes is itself claimed alongside it,
// and thus it is the refutation that "(F) ~ refuted" on its own line is.
// When nested within another statement, or serving as a member of a
// comma-separated list lacking variables, it was built as data: following
// "a p b", the line "x says ((a p b) ~ refuted)" left `a p b` un-claimed
// without a word, just as the comma list, the focus spelling, a self-fact
// that held the marking, and a Janet block that conveyed a rule's marking
// pattern by reference did. Each is refused before any writing: the Janet
// block's statement used to be written before the claim reached the marking,
// and it stayed (Zelph::refuse_claim_of_marked_parts). A marking within a
// refuted fact is mentioned, not claimed: see
// "a marking within a refuted fact is mentioned and not claimed" below.
TEST_CASE("refuted marking: one a claimed statement writes is refused for a claimed fact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        std::string statement;
        SUBCASE("nested in a statement") { statement = "x says ((a p b) ~ refuted)"; }
        SUBCASE("nested with the focus") { statement = "x says (*(a p b) ~ refuted)"; }
        SUBCASE("in a comma list without variables") { statement = "a p b, (a p b) ~ refuted"; }
        SUBCASE("nested in a self-fact") { statement = ":says ((a p b) ~ refuted)"; }
        // The elements of a list and the members of a set are also nested in
        // the statement, and the claim extends to a marking among them: before
        // this, `x says <((a p b) ~ refuted)>` un-claimed `a p b` exactly as
        // the statement form did.
        SUBCASE("an element of a list") { statement = "x says <((a p b) ~ refuted)>"; }
        SUBCASE("a member of a set") { statement = "x says {((a p b) ~ refuted)}"; }
        SUBCASE("nested where a rule wrote the marking as its pattern")
        {
            interactive.process("((a p b) ~ refuted) => !");
            statement = "x says ((a p b) ~ refuted)";
        }
        SUBCASE("a rule's marking pattern handed in from Janet")
        {
            interactive.process("((a p b) ~ refuted) => !");
            statement = R"js(%(zelph/fact "x" "says" (first (zelph/sources "=>" "!"))))js";
        }
        // A typed statement also accepts a node by reference, either via a
        // Janet variable or an inline keyword. The claim reaches the marking
        // at the end of the line, once the statement has been written, and the
        // refused `x says ((a p b) ~ refuted)` remained within the graph.
        SUBCASE("a rule's marking pattern a typed statement names by a Janet variable")
        {
            interactive.process("((a p b) ~ refuted) => !");
            interactive.process(R"js(%(def marking (first (zelph/sources "=>" "!"))))js");
            statement = "x says ,marking";
        }
        SUBCASE("a rule's marking pattern an inline keyword hands in")
        {
            interactive.process("((a p b) ~ refuted) => !");
            interactive.process(R"js(%(zelph/register-keyword "M<" ">" (fn [text] (first (zelph/sources "=>" "!")))))js");
            statement = "x says M<>";
        }
        // The statement itself a rule's pattern: the claim took its mark
        // before attaining the marking, and the statement subsequently
        // answered as asserted, without a node being written.
        SUBCASE("a rule's marking pattern in a statement a rule wrote")
        {
            process_lines(interactive, "((a p b) ~ refuted) => !\n(x says ((a p b) ~ refuted)) => (k l m)\n");
            interactive.process(R"js(%(def marking (first (zelph/sources "=>" "!"))))js");
            statement = "x says ,marking";
        }
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("this fact is known to be true"));
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S l O");
        CHECK(collect_answers(collector).empty()); });
}

// A statement that claims a refuted fact is refused, as asserting it is: the
// fact is known to be wrong. The typed "x says (a p b)" following "¬(a p b)"
// is refused, yet a statement that held the fact through a node was accepted:
// its claim took the mark of the rule's pattern, thereby causing the fact to
// be both claimed and refuted at once, and once the refutation was pruned, it
// answered and fired the rule. The refuted fact is either a rule's pattern
// that ¬ refuted, or the mention that the refutation of a new fact writes, as
// a user reaches either.
TEST_CASE("refuted marking: a statement that claims a refuted fact by node is refused")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(a p b) => (c q d)\n¬(a p b)\n");
        std::string statement;
        std::string query = "S p O";
        SUBCASE("a mention a Janet block builds") { statement = R"js(%(zelph/fact "x" "says" (zelph/mention "a" "p" "b")))js"; }
        SUBCASE("the fact the focus of a refutation stands for") { statement = "x says (*(a p b) ~ refuted)"; }
        SUBCASE("a node a typed statement names by a Janet variable")
        {
            interactive.process(R"js(%(def held (zelph/mention "a" "p" "b")))js");
            statement = "x says ,held";
        }
        SUBCASE("what an inline keyword hands in")
        {
            interactive.process(R"js(%(zelph/register-keyword "R<" ">" (fn [text] (zelph/refute "a" "p" "b"))))js");
            statement = "x says R<>";
        }
        SUBCASE("the mention the refutation of a new fact writes")
        {
            interactive.process("¬(e r f)");
            statement = "x says (*(e r f) ~ refuted)";
            query     = "S r O";
        }
        // The claim did not enter a comma list that the statement wrote;
        // thus, the line was accepted, holding the refuted fact, whereas the
        // typed "x says ((a p b), j r s)" is refused.
        SUBCASE("a node in a comma list the statement writes")
        {
            interactive.process(R"js(%(def held (zelph/mention "a" "p" "b")))js");
            statement = "x says (,held, j r s)";
        }
        SUBCASE("a node a ground comma list holds as a member")
        {
            interactive.process(R"js(%(def held (zelph/mention "a" "p" "b")))js");
            statement = ",held, x q y";
        }
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("this fact is known to be wrong"));
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());

        // Withdrawn, the refutation preserves the original fact: the rule's
        // pattern, or a mention.
        interactive.process(".prune-facts X ~ refuted");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(query);
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

// The boundary of the refusal above: an argument of an opaque predicate
// constitutes a mention that claims nothing, thus enabling it to hold a
// refuted fact by node, just as the typed "k believes (a p b)" can. The
// refusal is common to every claim route, and the claim walk, along with the
// Janet and C pre-walk, stops at an opaque predicate.
TEST_CASE("refuted marking: an opaque argument holding a refuted fact by node is a mention")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "believes ~ opaque\n(a p b) => (c q d)\n¬(a p b)\n");
        interactive.process(R"js(%(def held (zelph/mention "a" "p" "b")))js");
        std::string statement;
        std::string query = "S believes O";
        SUBCASE("a Janet block") { statement = R"js(%(zelph/fact "k" "believes" held))js"; }
        SUBCASE("a typed statement naming it by a Janet variable") { statement = "k believes ,held"; }
        SUBCASE("an opaque fact nested in a claimed statement")
        {
            statement = "x says (k believes ,held)";
            query     = "S says O";
        }
        CHECK_NOTHROW(interactive.process(statement));
        collector.clear();
        interactive.process(query);
        CHECK(collect_answers(collector).size() == 1);
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a p b) ~ refuted"});

        // Withdrawn, the refutation leaves the rule's pattern: the mention
        // claimed nothing.
        interactive.process(".prune-facts X ~ refuted");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

// A statement that is refused leaves the graph as it was. The refutation
// inside a claimed statement is refused for a fact that the graph claims, and
// the statement can claim that fact itself, either alongside or before the
// marking: "c q d, (c q d) ~ refuted" was refused after it had asserted a new
// `c q d`, which remained, as did any content a statement wrote before the
// refusal -- the subject `y q z` in "(y q z) says ((a p b) ~ refuted)", the
// member `x q y` in a comma list. The statement runs where its refusal takes
// back what it had written (zelph/claim-text).
TEST_CASE("refuted marking: a statement whose refutation is refused keeps nothing it wrote")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        std::string statement;
        std::string refusal = "this fact is known to be true";
        SUBCASE("a comma list that asserts the fact it refutes") { statement = "c q d, (c q d) ~ refuted"; }
        SUBCASE("a comma list that refutes the fact it asserts")
        {
            statement = "(c q d) ~ refuted, c q d";
            refusal   = "this fact is known to be wrong";
        }
        SUBCASE("a part written before the marking") { statement = "(y q z) says ((a p b) ~ refuted)"; }
        SUBCASE("a member written beside the marking") { statement = "x q y, (a p b) ~ refuted"; }
        // A rule's pattern that the comma list asserts yet refutes. The list
        // did not claim its members, thus the pattern stayed a pattern
        // beside its refutation, and the line was accepted, discarding the
        // assertion without a word.
        SUBCASE("a comma list that asserts a rule's pattern it refutes")
        {
            interactive.process("(c q d) => (e r f)");
            statement = "c q d, (c q d) ~ refuted";
            refusal   = "this fact is known to be wrong";
        }
        // The identical comma list serving as the object within a
        // statement. The claim did not enter a comma list that the
        // statement wrote, hence the line was accepted as well.
        SUBCASE("a comma list in a statement that asserts a rule's pattern it refutes")
        {
            interactive.process("(c q d) => (e r f)");
            statement = "x says (c q d, (c q d) ~ refuted)";
            refusal   = "this fact is known to be wrong";
        }
        // The engine creates its own vocabulary at first use -- `refuted` and
        // the predicate of the pattern marks -- and retains it regardless of
        // whether something is refused or dropped ("clusters: a vocabulary
        // node the engine created inside a dropped cluster stays"). Both are
        // established before the count, which subsequently observes only what
        // the statement wrote.
        interactive.process(R"(%(zelph/resolve "refuted" "zelph"))");
        interactive.process("(u v w) => (u v x)");
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains(refusal));
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        CHECK_NOTHROW(interactive.process("c q d")); });
}

// When a statement is accepted, what it writes belongs to the currently active
// cluster, just as everything a statement writes does: the drop of that
// cluster takes it back, the refutation included.
TEST_CASE("refuted marking: a statement that carries a refutation writes into the active cluster")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(c q d) => (e r f)");
        // The engine creates its own vocabulary at first use -- `refuted` and
        // the predicate of the pattern marks -- and retains it regardless of
        // whether something is refused or dropped ("clusters: a vocabulary
        // node the engine created inside a dropped cluster stays"). Both are
        // established before the count, which subsequently observes only what
        // the statement wrote.
        interactive.process(R"(%(zelph/resolve "refuted" "zelph"))");
        const std::size_t before = node_count(collector, interactive);
        interactive.process(".cluster t");
        interactive.process("x says ((c q d) ~ refuted), y q z");
        interactive.process(".cluster default");
        interactive.process(".cluster-drop t");

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        CHECK_NOTHROW(interactive.process("c q d"));
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"e r f"}); });
}

// A statement that is refused discards solely what it produced. One that is
// accepted merges its output into the cluster that was active before it, and
// when no cluster was active, it ensures that nothing remains for a subsequent
// statement to discard: the accepted refutation and its associated statement
// remain once the next statement is refused.
TEST_CASE("refuted marking: a refused statement takes back nothing an earlier one wrote")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(c q d) => (e r f)");
        interactive.process("x says ((c q d) ~ refuted)");
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process("g h i, (g h i) ~ refuted"), doctest::Contains("this fact is known to be true"));
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(c q d) ~ refuted"});
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x says ((c q d) ~ refuted)"}); });
}

// Following a refused statement, the cluster that was active before it
// resumes its active state: whatever the next statement writes is assigned to
// it and goes with its drop.
TEST_CASE("refuted marking: a refused statement leaves the active cluster active")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);
        interactive.process(".cluster t");
        CHECK_THROWS_WITH(interactive.process("g h i, (g h i) ~ refuted"), doctest::Contains("this fact is known to be true"));
        interactive.process("y q z");
        interactive.process(".cluster default");
        interactive.process(".cluster-drop t");

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}

// A statement that was refused also retracts a refutation it issued against a
// fact that predated it: `x p y`, a rule's pattern, is refuted by the first
// member of the list below, and the refusal of the last member drops the
// marking alongside the 0 that the first member inscribed on its edge
// (Zelph::restore_refutations), so `x p y` can be asserted and fires its rule.
TEST_CASE("refuted marking: a refused statement takes back its refutation of a rule's pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(x p y) => (e r f)");
        CHECK_THROWS_WITH(interactive.process("(x p y) ~ refuted, c q d, (c q d) ~ refuted"), doctest::Contains("this fact is known to be true"));

        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        CHECK_NOTHROW(interactive.process("x p y"));
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"e r f"}); });
}

// Replaces the SUBCASEs "nested in a refutation" and "nested in the fact of a
// marking line" from the prior test, which rejected
// `¬(x says ((a p b) ~ refuted))` after `a p b` because the refutation claimed
// what its fact holds, the marking included. A refutation claims its marking
// and nothing concerning its fact (Zelph::refute): the line says that x does
// not say that `a p b` is refuted, and its nested marking is a mention that
// refutes nothing.
TEST_CASE("refuted marking: a marking within a refuted fact is mentioned and not claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        std::string statement;
        SUBCASE("the ¬ line") { statement = "¬(x says ((a p b) ~ refuted))"; }
        SUBCASE("the marking line") { statement = "(x says ((a p b) ~ refuted)) ~ refuted"; }

        CHECK_NOTHROW(interactive.process(statement));
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(x says ((a p b) ~ refuted)) ~ refuted"}); });
}

// On the opposite side of that refusal: for a fact that nobody claimed, the
// nested marking functions as the refutation the line is. A fact previously
// refuted was refused as known to be wrong, since the operand was built
// again as a claim; the nested marking now re-enters just as the printed
// line does.
TEST_CASE("refuted marking: one a claimed statement writes refutes a fact nobody claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a new fact") {}
        SUBCASE("a fact refuted before") { interactive.process("¬(a p b)"); }
        SUBCASE("a rule's pattern") { interactive.process("(a p b) => (c q d)"); }

        CHECK_NOTHROW(interactive.process("x says ((a p b) ~ refuted)"));
        CHECK_THROWS_WITH(interactive.process("a p b"), doctest::Contains("this fact is known to be wrong"));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a p b) ~ refuted"});
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x says ((a p b) ~ refuted)"}); });
}

// A refutation of a refutation marking says that the marking does not hold, and
// it wrote the marking regardless, at a probability of 0, while Zelph::fact
// refutes the fact of any marking it writes: `¬((a p b) ~ refuted)` took a
// claimed `a p b` from every query, and for a fact nobody claimed it said the
// opposite of its intended meaning, because `a p b` was subsequently refuted.
// The marking line of a marking, a marking applied to a marking nested within a
// statement, and zelph/refute of a marking in Janet concluded identically. Only
// a claimed marking refutes, and a refutation is withdrawn by removing its
// marking, so each of these is refused.
TEST_CASE("refuted marking: a refutation of a refutation marking is refused")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto refused = [&](const std::string& statement, const bool janet)
        {
            SUBCASE("of a fact the graph claims")
            {
                interactive.process("a p b");
                CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("cannot itself be refuted"));
                collector.clear();
                interactive.process("S p O");
                CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});
            }
            // A typed line is refused before its code runs, thus writing
            // nothing; by the time zelph/refute receives it, the Janet block
            // has already built and claimed `a p b`.
            SUBCASE("of a fact nobody claimed")
            {
                const std::size_t before = node_count(collector, interactive);
                CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("cannot itself be refuted"));
                if (!janet) CHECK(node_count(collector, interactive) == before);
                CHECK_NOTHROW(interactive.process("a p b"));
            }

            collector.clear();
            interactive.process("X ~ refuted");
            CHECK(collect_answers(collector).empty());
        };

        SUBCASE("the ¬ line") { refused("¬((a p b) ~ refuted)", false); }
        SUBCASE("the marking line") { refused("((a p b) ~ refuted) ~ refuted", false); }
        SUBCASE("nested in a statement") { refused("x says (((a p b) ~ refuted) ~ refuted)", false); }
        SUBCASE("zelph/refute in a Janet block")
        {
            refused(R"js(%(zelph/refute (zelph/fact "a" "p" "b") "~" (zelph/resolve "refuted" "zelph")))js", true);
        } });
}

// A derived marking of a fact that nobody claimed is a refutation, and it
// must reject a subsequent assertion in the manner that `¬(F)` does. It
// wrote only the index half, so "mary sleeps nil" typed afterwards was
// permitted, displayed as "¬(mary sleeps nil)" and swallowed without
// comment.
TEST_CASE("refuted marking: a derived one refuses a later assertion as ¬ does")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(X ~ person) => ((X sleeps nil) ~ refuted)\nmary ~ person\n");
        CHECK_THROWS_WITH(interactive.process("mary sleeps nil"), doctest::Contains("this fact is known to be wrong"));

        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector).empty()); });
}

// The fact that a derived refutation creates is a mention, just as the fact of
// a typed refutation is (Zelph::refute). The firing wrote it as data, and
// after the marking had been pruned, it answered `S sleeps O`; the 0
// positioned on its edge kept the rule from deriving the refutation once more,
// and upon removal of that 0, the next run would meet a claimed fact with the
// refutation it derives. The rule still holds following the prune, thus the
// run derives the refutation again.
TEST_CASE("refuted marking: a pruned derived one is derived again and its fact does not answer")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        process_lines(interactive, "(X ~ person) => ((X sleeps nil) ~ refuted)\nmary ~ person\n");
        REQUIRE(answers("X ~ refuted") == std::vector<std::string>{"(mary sleeps nil) ~ refuted"});

        collector.clear();
        interactive.process(".prune-facts X ~ refuted");
        REQUIRE(any_output_contains(collector, "Pruned 1"));
        CHECK(answers("S sleeps O").empty());

        collector.clear();
        interactive.run(true, false, false);
        CHECK_FALSE(has_contradiction(collector));
        CHECK(answers("X ~ refuted") == std::vector<std::string>{"(mary sleeps nil) ~ refuted"});
        CHECK(answers("S sleeps O").empty()); });
}

// A derived refutation of a fact that another rule wrote as its pattern leaves
// the fact that rule's pattern, just as a typed refutation does: the derived
// marking claims nothing of its fact. The firing took `a p b` from the
// patterns of `(a p b) => (c q d)`, and after the refutation was withdrawn
// together with its source, `a p b` answered and fired the rule, while
// asserting it was refused as known to be wrong.
TEST_CASE("refuted marking: a derived one of a rule's pattern leaves the pattern as it was once withdrawn")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        process_lines(interactive, "(a p b) => (c q d)\n(X ~ person) => ((X p b) ~ refuted)\na ~ person\n");
        interactive.run(true, false, false);
        REQUIRE(answers("X ~ refuted") == std::vector<std::string>{"(a p b) ~ refuted"});

        collector.clear();
        interactive.process(".prune-facts X ~ person");
        REQUIRE(any_output_contains(collector, "Pruned 1"));
        collector.clear();
        interactive.process(".prune-facts X ~ refuted");
        REQUIRE(any_output_contains(collector, "Pruned 1"));
        interactive.run(true, false, false);
        CHECK(answers("X ~ refuted").empty());
        CHECK(answers("S p O").empty());
        CHECK(answers("S q O").empty());

        CHECK_NOTHROW(interactive.process("a p b"));
        interactive.run(true, false, false);
        CHECK(answers("S q O") == std::vector<std::string>{"c q d"}); });
}

// The same applies to a marking nested within the consequence: the fact it
// refutes, as the firing creates it, functions as a mention, and answers
// nothing once the marking is removed. The marking goes with `.remove`, which
// takes along the statement that holds it; a prune leaves that statement
// behind as a self-fact, a defect of its own.
TEST_CASE("refuted marking: the fact a derived statement's nested marking creates is a mention")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(X go Y) => (Y says ((X p b) ~ refuted))\nt go k\n");
        collector.clear();
        interactive.process(".node (t p b) ~ refuted");
        std::string marking;
        for (const auto& event : collector.events())
            if (const std::size_t at = event.text.find("Node ID: "); at != std::string::npos) marking = event.text.substr(at + 9);
        REQUIRE_FALSE(marking.empty());

        interactive.process(".remove " + marking);
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty()); });
}

// Only a claimed marking refutes. The patterns of a rule are written
// through the same fact() calls as a claim, and the marking a rule merely
// names as its condition -- specifically, the structure "if F is refuted
// then ..." -- hid F as though it had been refuted: a fact asserted before
// the rule or following it answered nothing, and after a save and a load,
// the rule itself was listed as refuted, since the index was read back from
// every left neighbour of a marking, with the rule whose subject the marking
// is included among them.
TEST_CASE("refuted marking: one that a rule only names as a condition refutes nothing")
{
    const auto john_waits = [](zelph::io::OutputCollector& collector, zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process("S waits O");
        return collect_answers(collector) == std::vector<std::string>{"john waits nil"};
    };

    SUBCASE("a fact claimed before the rule stays claimed")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "john waits nil\n((john waits nil) ~ refuted) => (mary sleeps nil)\n");
        CHECK(john_waits(collector, interactive));
    }

    SUBCASE("a fact asserted after the rule is claimed")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((john waits nil) ~ refuted) => (mary sleeps nil)\njohn waits nil\n");
        CHECK(john_waits(collector, interactive));
    }

    SUBCASE("the same after a save and a load")
    {
        const auto file = std::filesystem::temp_directory_path() / "zelph_refuted_rule_pattern.bin";
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, "((john waits nil) ~ refuted) => (mary sleeps nil)\n((a p b) ~ refuted) noted-by sue\n");
            interactive.process(".save \"" + file.string() + "\"");
        }

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + file.string() + "\"");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "((john waits nil) ~ refuted) => (mary sleeps nil)"));
        CHECK_FALSE(any_output_contains(collector, "¬"));

        // A statement whose subject was a marking stood beside the marking in
        // the identical manner, and stopped answering following the load.
        collector.clear();
        interactive.process("S noted-by O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"((a p b) ~ refuted) noted-by sue"});

        interactive.process("john waits nil");
        CHECK(john_waits(collector, interactive));
        std::filesystem::remove(file);
    }

    SUBCASE("the rule fires once the refutation is claimed")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((john waits nil) ~ refuted) => (mary sleeps nil)\n¬(john waits nil)\n");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"mary sleeps nil"});
    }

    // The identical rule constructed via Janet, on every path that builds rule
    // text: within the thunk of zelph/dedup-rule (the route a front end takes,
    // with zelph/fact building the conditions) and of zelph/build-rule, and
    // within the argument forms of zelph/rule, the ground marking represents
    // the rule's condition, neither refused nor applied, much like a typed
    // rule. zelph/rule* accepts nodes created before its invocation, and
    // zelph/fact claims, which refuses a ground marking, hence its condition
    // is written with zelph/mention. The rule is in force before the
    // refutation, without firing, and fires once the refutation is claimed.
    SUBCASE("built from Janet the rule fires once the refutation is claimed")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        const std::string           condition = R"js((zelph/fact (zelph/fact "john" "waits" "nil") "~" "refuted"))js";
        const std::string           sleeps    = R"js((zelph/fact "mary" "sleeps" "nil"))js";
        SUBCASE("zelph/dedup-rule") { interactive.process("%(zelph/dedup-rule (fn [] (zelph/fact " + condition + " \"=>\" " + sleeps + ")))"); }
        SUBCASE("zelph/build-rule") { interactive.process("%(zelph/build-rule (fn [] (zelph/fact " + condition + " \"=>\" " + sleeps + ")))"); }
        SUBCASE("the zelph/rule macro") { interactive.process("%(zelph/rule [" + condition + "] " + sleeps + ")"); }
        SUBCASE("zelph/rule* over mentions")
        {
            interactive.process(R"js(%(zelph/rule* [(zelph/mention (fn [] (zelph/fact (zelph/fact "john" "waits" "nil") "~" "refuted")))] (zelph/mention "mary" "sleeps" "nil")))js");
        }
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(john waits nil) ~ refuted"));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector).empty());

        interactive.process("¬(john waits nil)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"mary sleeps nil"});
    }

    // The identical rules retrieved from a file: the marking stays the rule's
    // condition, just as it does in a typed rule ("the same after a save and
    // a load"), and is not read as a refutation that fires the rule during
    // the next run.
    SUBCASE("built from Janet the rule stays a condition after a save and a load")
    {
        const auto                  file = std::filesystem::temp_directory_path() / "zelph_refuted_janet_condition.bin";
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        SUBCASE("zelph/dedup-rule")
        {
            interactive.process(R"js(%(zelph/dedup-rule (fn [] (zelph/fact (zelph/fact (zelph/fact "john" "waits" "nil") "~" "refuted") "=>" (zelph/fact "mary" "sleeps" "nil")))))js");
        }
        SUBCASE("zelph/rule* over mentions")
        {
            interactive.process(R"js(%(zelph/rule* [(zelph/mention (fn [] (zelph/fact (zelph/fact "john" "waits" "nil") "~" "refuted")))] (zelph/mention "mary" "sleeps" "nil")))js");
        }
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector).empty());

        interactive.process("¬(john waits nil)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"mary sleeps nil"});
        std::filesystem::remove(file);
    }

    // A rule that a statement only mentions claims none of its parts, the
    // marking included.
    SUBCASE("a rule a statement mentions")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "john waits nil\nx about (((john waits nil) ~ refuted) => (mary sleeps nil))\n");
        CHECK(john_waits(collector, interactive));
    }

    // The identical mention written from Janet: zelph/rule-text writes the
    // rule's text, and its ground marking constitutes a condition of the rule
    // mentioned.
    SUBCASE("a rule zelph/rule-text writes from Janet")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("john waits nil");
        interactive.process(R"js(%(zelph/fact "x" "about" (zelph/rule-text (fn [] (zelph/fact (zelph/fact (zelph/fact "john" "waits" "nil") "~" "refuted") "=>" (zelph/fact "mary" "sleeps" "nil"))))))js");
        CHECK(john_waits(collector, interactive));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
    }

    // A rule that a query mentions forms part of what the query asserts (the
    // query asserts what it creates under a mentioned `=>`,
    // Zelph::mark_query_operands), and the marking of the claimed fact was
    // never applied: the claimed fact kept answering within the session, and
    // following a .save and a .load, the marking was read back as a refutation
    // and `john waits nil` produced no answer.
    SUBCASE("a rule a query mentions before and after a save and a load")
    {
        const auto                  file = std::filesystem::temp_directory_path() / "zelph_refuted_query_mention.bin";
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "john waits nil\nS noted (((john waits nil) ~ refuted) => (mary sleeps nil))\n");
        CHECK(john_waits(collector, interactive));
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(john_waits(collector, interactive));
        std::filesystem::remove(file);
    }

    // The same mention written by a firing: the marking is constructed as the
    // consequence is instantiated, before the mentioned rule's parts are
    // marked as its patterns.
    SUBCASE("a rule a firing mentions")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "john waits nil\njohn p k\n(X p Y) => (Y says (((X waits nil) ~ refuted) => (X ~ calm)))\n");
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).size() == 1);
        CHECK(john_waits(collector, interactive));
    }

    // And read back: the marking a firing writes into a mentioned rule is
    // that rule's pattern, and the load reads it as such, not as a refutation
    // of the claimed fact -- the session and the reload remain consistent,
    // just as they do for the rule a query mentions.
    SUBCASE("a rule a firing mentions before and after a save and a load")
    {
        const auto                  file = std::filesystem::temp_directory_path() / "zelph_refuted_firing_mention.bin";
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "john waits nil\njohn p k\n(X p Y) => (Y says (((X waits nil) ~ refuted) => (X ~ calm)))\n");
        CHECK(john_waits(collector, interactive));
        interactive.process(".save \"" + file.string() + "\"");
        interactive.process(".load \"" + file.string() + "\"");
        CHECK(john_waits(collector, interactive));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).empty());
        std::filesystem::remove(file);
    }

    // This time, the marking is claimed through a derivation: the
    // node that the first rule wrote as its pattern is the one
    // the second rule derives.
    SUBCASE("a refutation derived later claims it")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "((john waits nil) ~ refuted) => (mary sleeps nil)\n(X ~ man) => ((X waits nil) ~ refuted)\njohn ~ man\n");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S sleeps O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"mary sleeps nil"});
        CHECK_THROWS_WITH(interactive.process("john waits nil"), doctest::Contains("this fact is known to be wrong"));
    }
}

// What a command resolves is a pattern, just as a rule's is. The commands
// `.explain` and `.prune-facts` construct a pattern that cannot be located
// within a scratch cluster, and a marking built there refuted its fact for the
// rest of the session, even though the marking itself went with the cluster:
// `john waits nil` stopped answering after a command had merely inquired into
// its refutation.
TEST_CASE("refuted marking: a command that names a marking refutes nothing")
{
    for (const char* command : {".explain ((john waits nil) ~ refuted)", ".prune-facts ((john waits nil) ~ refuted)", ".node (john waits nil) ~ refuted"})
    {
        CAPTURE(command);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("john waits nil");
        try
        {
            interactive.process(command);
        }
        catch (const std::exception&)
        {
            // .node refuses a pattern that denotes no node; what matters is
            // what the attempt left behind.
        }
        collector.clear();
        interactive.process("S waits O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john waits nil"});
    }
}

// Before evaluation, a query is constructed, and within its own components, a
// ground marking refuted its fact during the construction phase. A query
// that subsequently failed discarded everything it had assembled, including
// the marking, and left the refutation behind: the rule's pattern `john waits
// nil`, asserted following the unsuccessful inquiry, yielded no response, and
// its rule did not fire.
TEST_CASE("refuted marking: a query that fails refutes nothing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "t go k\n(john waits nil) => (x y z)\n");
        CHECK_THROWS_WITH(interactive.process("X go Y, (john waits nil) ~ refuted, ¬{(e f g)}"), doctest::Contains("is not one"));

        interactive.process("john waits nil");
        collector.clear();
        interactive.process("S waits O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john waits nil"});
        collector.clear();
        interactive.process("S y O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"x y z"}); });
}

// Instantiation builds a nested fact at probability 1, and Zelph::fact refuses
// that for a fact the graph holds as known to be wrong -- outside the guard
// that turns a refused consequence into a reported `!`. A rule concluding that
// F is refuted, when F was already refuted by `¬`, halted the entire run with
// the message "this fact is known to be wrong": the other firings of the run
// were lost, and each later line in an auto-running session failed
// identically. Reiterating that F is refuted has no effect.
TEST_CASE("refuted marking: a rule concluding what ¬ already refuted completes its run")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
¬(mary sleeps nil)
(X ~ person) => ((X sleeps nil) ~ refuted)
(X ~ person) => (X ~ mortal)
mary ~ person
bob ~ person
)");
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        interactive.process("X ~ mortal");
        CHECK(collect_answers(collector).size() == 2);

        collector.clear();
        interactive.process("X ~ refuted");
        const std::vector<std::string> refuted = collect_answers(collector);
        CHECK(refuted.size() == 2);
        CHECK(std::find(refuted.begin(), refuted.end(), "(bob sleeps nil) ~ refuted") != refuted.end()); });
}

// The same abort, arrived at through a consequence that holds the refuted
// fact as a part. A statement claims what it nests, thereby causing that
// consequence to contradict the refutation and be reported accordingly, and
// the run continues.
TEST_CASE("refuted marking: a consequence nesting a refuted fact is a contradiction, not an abort")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "¬(mary sleeps nil)\n(X ~ person) => ((X sleeps nil) doubted-by bob)\n(X ~ person) => (X ~ mortal)\n");
        collector.clear();
        CHECK_NOTHROW(interactive.process("mary ~ person"));
        CHECK(any_event_contains(collector, "! ⇐ (mary ~ person)"));

        collector.clear();
        interactive.process("S doubted-by O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("X ~ mortal");
        CHECK(collect_answers(collector) == std::vector<std::string>{"mary ~ mortal"}); });
}

// The identical contradiction, reached via a container that the consequence
// writes: the statement claims its members, just as it claims what it nests.
// The prediction did not enter a container, so after "¬(t p b)", the rule
// "(X go Y) => (k says {(X p b)})" ended its run with "known to be wrong", as
// did each subsequent line, and a comma list without variables was derived,
// holding the refuted fact without a word. The typed statements are refused
// as known to be wrong.
TEST_CASE("refuted marking: a container a consequence writes holding a refuted fact is a contradiction, not an abort")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "¬(t p b)\nt go k\n");
        std::string rule;
        SUBCASE("a set") { rule = "(X go Y) => (k says {(X p b)})"; }
        SUBCASE("a collection") { rule = "(X go Y) => (k says @{(X p b)})"; }
        SUBCASE("a comma list") { rule = "(X go Y) => (k says ((t p b), j r s))"; }
        SUBCASE("a set nested in a statement") { rule = "(X go Y) => (k says (j r {(X p b)}))"; }
        // A member serving as a container is traversed according to its
        // own plan, as the instantiation rebuilds it.
        SUBCASE("a set nested in a set") { rule = "(X go Y) => (k says {{(X p b)}})"; }
        collector.clear();
        CHECK_NOTHROW(interactive.process(rule));
        CHECK(has_contradiction(collector));

        CHECK_NOTHROW(interactive.run(true, false, false));
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());
        CHECK_THROWS_WITH(interactive.process("t p b"), doctest::Contains("known to be wrong")); });
}

// The prediction reads a comma list as the claim enters it. A rule's condition
// set that a consequence names by node is not entered, and the prediction
// read its members as claimed: following "¬(a p b)", the rule
// "(X go Y) => (k says ,conditions)" was reported as a contradiction at each
// line, whereas the typed "x says ,conditions" is accepted.
TEST_CASE("refuted marking: a rule's condition set that a consequence names by node holding a refuted fact is no contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "¬(a p b)\n(a p b, c d e) => (f g h)\n");
        interactive.process(R"js(%(def conditions (get (zelph/parse (get (first (zelph/rules)) :rule)) 0)))js");
        interactive.process("t go z");
        collector.clear();
        CHECK_NOTHROW(interactive.process("(X go Y) => (k says ,conditions)"));
        CHECK_NOTHROW(interactive.run(true, false, false));
        CHECK_FALSE(has_contradiction(collector));

        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).size() == 1);
        CHECK_THROWS_WITH(interactive.process("a p b"), doctest::Contains("known to be wrong"));
        CHECK_NOTHROW(interactive.process("x says ,conditions")); });
}

// A composite predicate is a statement that the consequence claims, as it
// claims its subject and its objects, and the prediction read only those:
// after "¬(t p b)", the rule "(X go Y) => (k (X p b) c)" ended its run with
// "known to be wrong", and after "t p b", the rule
// "(X go Y) => (k ((X p b) ~ refuted) c)" refuted the claimed fact without a
// word. The typed "k (t p b) c" is refused as known to be wrong.
TEST_CASE("refuted marking: a composite predicate of a consequence meets the refutations its subject and objects meet")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        bool        claimed = false;
        std::string rule;
        SUBCASE("a refuted fact as the predicate")
        {
            interactive.process("¬(t p b)");
            rule = "(X go Y) => (k (X p b) c)";
        }
        SUBCASE("a refuted fact nested in the predicate")
        {
            interactive.process("¬(t p b)");
            rule = "(X go Y) => (k ((X p b) r s) c)";
        }
        // A predicate which functions as a container is traversed as a
        // container in any other position is.
        SUBCASE("a refuted fact in a set as the predicate")
        {
            interactive.process("¬(t p b)");
            rule = "(X go Y) => (k {(X p b)} c)";
        }
        SUBCASE("a marking of a claimed fact as the predicate")
        {
            interactive.process("t p b");
            rule    = "(X go Y) => (k ((X p b) ~ refuted) c)";
            claimed = true;
        }
        interactive.process("t go k");
        collector.clear();
        CHECK_NOTHROW(interactive.process(rule));
        CHECK(has_contradiction(collector));

        CHECK_NOTHROW(interactive.run(true, false, false));
        collector.clear();
        interactive.process("k P O");
        CHECK(collect_answers(collector).empty());
        if (claimed)
        {
            collector.clear();
            interactive.process("S p O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"t p b"});
        }
        else
        {
            CHECK_THROWS_WITH(interactive.process("t p b"), doctest::Contains("known to be wrong"));
        } });
}

// A rule that the consequence mentions is written as a mention, which claims
// none of its components, so a refuted fact within it contradicts nothing.
TEST_CASE("refuted marking: a refuted fact inside a rule a consequence mentions is no contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string said;
        SUBCASE("a ground fact")
        {
            process_lines(interactive, "¬(mary sleeps nil)\n(X ~ person) => (X says ((mary sleeps nil) => (bob ~ awake)))\n");
            said = "bob says ((mary sleeps nil) => (bob ~ awake))";
        }
        // The instantiation built the fact that the binding completes at
        // probability 1, causing the refusal "this fact is known to be wrong"
        // to terminate the run, and each subsequent line ran the rule once
        // more and failed in the same way.
        SUBCASE("a fact the binding completes")
        {
            process_lines(interactive, "¬(bob sleeps nil)\n(X ~ person) => (X says ((X sleeps nil) => (bob ~ awake)))\n");
            said = "bob says ((bob sleeps nil) => (bob ~ awake))";
        }
        collector.clear();
        CHECK_NOTHROW(interactive.process("bob ~ person"));
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector) == std::vector<std::string>{said});
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector).size() == 1); });
}

// After a derived marking writes the probability half of its refutation, the
// next firing of that same rule meets an F refuted by itself, causing a rule
// of this kind to fire on every run. Absent the step that reads such a
// consequence as already known, the second run aborted.
TEST_CASE("refuted marking: a marking rule runs twice without aborting")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(X ~ person) => ((X sleeps nil) ~ refuted)\nmary ~ person\n");
        CHECK_NOTHROW(interactive.run(true, false, false));
        CHECK_NOTHROW(interactive.run(true, false, false));
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(mary sleeps nil) ~ refuted"}); });
}

// The two spellings of a refutation, `¬(F)` and `(F) ~ refuted`, must
// acknowledge one another. The marking line built F as a nested statement at
// probability 1, so following `¬(F)` it was refused as known to be wrong -- the
// answer "(a p b) ~ refuted" that "S ~ refuted" prints could not be entered
// again -- and conversely `¬(F)` met an F the marking had left at probability 1
// and was refused as known to be true.
TEST_CASE("refuted marking: a marking of a refuted fact is accepted and changes nothing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("¬ first, then the marking")
        {
            interactive.process("¬(a p b)");
            CHECK_NOTHROW(interactive.process("(a p b) ~ refuted"));
            CHECK_NOTHROW(interactive.process("*(a p b) ~ refuted"));
        }
        SUBCASE("the printed marking re-enters")
        {
            interactive.process("¬(a p b)");
            collector.clear();
            interactive.process("S ~ refuted");
            const std::vector<std::string> printed = collect_answers(collector);
            REQUIRE(printed == std::vector<std::string>{"(a p b) ~ refuted"});
            CHECK_NOTHROW(interactive.process(printed.front()));
        }
        SUBCASE("the marking first, then ¬")
        {
            interactive.process("(a p b) ~ refuted");
            CHECK_NOTHROW(interactive.process("¬(a p b)"));
        }

        const std::size_t before = node_count(collector, interactive);
        interactive.process("(a p b) ~ refuted");
        interactive.process("¬(a p b)");
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("S ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a p b) ~ refuted"}); });
}

// A marking of an already refuted fact reiterates what is established, so a
// marking obtained through derivation is accepted and introduces no
// alteration, as the typed marking line does (above). Read as a statement that
// nests a refuted fact, the firing of
// "(X go Y) => (k says ((X p b) ~ refuted))" following "¬(t p b)" was reported
// as a contradiction in every run, and when a binding supplied `refuted`, the
// derived marking by itself also constituted one. The setup refutes every fact
// in the manner a user would: via `¬`, or by the firing that initially derived
// the marking.
TEST_CASE("refuted marking: a derived one of a refuted fact is accepted and changes nothing")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::vector<std::string> said{"k says ((t p b) ~ refuted)"};
        SUBCASE("nested in a statement")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (k says ((t p b) ~ refuted))\n");
        }
        SUBCASE("nested with the fact the binding completes")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (k says ((X p b) ~ refuted))\n");
        }
        SUBCASE("nested with the refuted a binding supplies")
        {
            process_lines(interactive, "¬(t p b)\nt go refuted\n(X go Y) => (k says ((t p b) ~ Y))\n");
        }
        SUBCASE("as the consequence with the refuted a binding supplies")
        {
            process_lines(interactive, "¬(t p b)\nt go refuted\n(X go Y) => ((t p b) ~ Y)\n");
            said.clear();
        }
        // The prediction did not enter a set or a composite predicate, and
        // the instantiation built the refuted fact at probability 1, which
        // ended the run with "known to be wrong".
        SUBCASE("in a set the consequence builds")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (k says {((X p b) ~ refuted)})\n");
            said = {"k says {((t p b) ~ refuted)}"};
        }
        SUBCASE("as the composite predicate of the consequence")
        {
            process_lines(interactive, "¬(t p b)\nt go k\n(X go Y) => (k ((X p b) ~ refuted) c)\n");
            said.clear();
        }
        // The first firing generates `t p b` and refutes it; each later run
        // met it as a refuted fact.
        SUBCASE("nested where the firing refuted the fact itself")
        {
            process_lines(interactive, "t go k\n(X go Y) => (k says ((X p b) ~ refuted))\n");
        }
        // Following a load, the walk reads fact structures reconstructed from
        // adjacency instead of retrieving them from the store, and the
        // refuted fact it looks up is the node that the load restored.
        SUBCASE("nested where the firing refuted the fact itself before a save and a load")
        {
            const auto file = std::filesystem::temp_directory_path() / "zelph_derived_marking_of_refuted.bin";
            process_lines(interactive, "t go k\n(X go Y) => (k says ((X p b) ~ refuted))\n");
            interactive.process(".save \"" + file.string() + "\"");
            interactive.process(".load \"" + file.string() + "\"");
            std::filesystem::remove(file);
        }
        // A derived marking associated with a fact that was refuted
        // before the cluster was opened refutes nothing, thus the drop takes
        // the derived statement and leaves the refutation in place.
        SUBCASE("nested in a cluster dropped afterwards")
        {
            process_lines(interactive, "¬(t p b)\n.cluster derived-marking\nt go k\n(X go Y) => (k says ((X p b) ~ refuted))\n.cluster-drop derived-marking\n");
            said.clear();
        }
        CHECK_FALSE(has_contradiction(collector));

        const std::size_t before = node_count(collector, interactive);
        collector.clear();
        CHECK_NOTHROW(interactive.run(true, false, false));
        CHECK_FALSE(has_contradiction(collector));
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector) == said);
        collector.clear();
        interactive.process("S ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(t p b) ~ refuted"});
        CHECK_THROWS_WITH(interactive.process("t p b"), doctest::Contains("known to be wrong")); });
}

// The prediction lists the refuted facts it meets where the statement claims
// nothing, and the instantiation takes one exactly as it stands only at the
// location where the prediction met it. Taken at any place, the look-up
// reached places -- a member of a set constructed by the consequence, a
// composite predicate of the consequence -- where the statement claims the
// fact: following "¬(t p b)", the rule
// "(X go Y) => (k says ((X p b) ~ refuted) {(X p b)})" wrote the set holding
// the refuted fact without a word, while the typed statement is refused as
// known to be wrong. The walk now proceeds into both places, and the line
// represents the contradiction that the identical consequence without the
// marking is (the tests on a container and on a composite predicate above);
// this test pins that the line does not pass silently and writes nothing.
TEST_CASE("refuted marking: a refuted fact a derived marking names is taken as it stands only where the prediction met it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "¬(t p b)\nt go k\n");
        std::string rule;
        SUBCASE("in a set the consequence builds") { rule = "(X go Y) => (k says ((X p b) ~ refuted) {(X p b)})"; }
        SUBCASE("as the composite predicate of the consequence") { rule = "(X go Y) => (k (X p b) ((X p b) ~ refuted))"; }
        // `sue says (X p b)` appears within an argument of an opaque
        // predicate, functioning as a mention, and also resides in the set,
        // where the statement claims it: in both places, the refuted fact
        // shares the same statement directly above it.
        SUBCASE("in a set the consequence builds and in a mention of the same statement")
        {
            interactive.process("believes ~ opaque");
            rule = "(X go Y) => (k says (j believes (sue says (X p b))) {(sue says (X p b))})";
        }
        collector.clear();

        bool loud = false;
        try
        {
            interactive.process(rule);
            loud = has_contradiction(collector);
        }
        catch (const std::exception&)
        {
            loud = true;
        }
        CHECK(loud);

        // Read without a run: a line that ended the run ends every later run
        // in the identical manner.
        interactive.process(".auto-run");
        collector.clear();
        interactive.process("k P O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(t p b) ~ refuted"}); });
}

// A derived refutation that meets a fact someone claimed constitutes a
// contradiction between the two, with neither prevailing without notice.
// The marking used to take the claimed fact out of every query, wordlessly.
// Now it is reported once, nothing is written, the fact keeps answering,
// and the record holds the fact beside the instance of the rule's single
// plain condition: the record that the denial "(X ~ person, X sleeps nil)
// => !" would leave.
//
// This test was named "refuted marking: a derived one meeting a claimed fact
// is a contradiction", and it read the record via the query `S ~ refuted`. A
// record delivers no response to any query because "contradiction record:
// the record itself answers no query and matches no condition", hence the
// record is extracted from the graph, and the query answers nothing.
TEST_CASE("refuted marking: a derived one meeting a claimed fact is a contradiction, recorded with both facts")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, "(X ~ person) => ((X sleeps nil) ~ refuted)\nmary sleeps nil\n");
    collector.clear();
    interactive.process("mary ~ person");
    CHECK(contradiction_lines(collector) == 1);

    collector.clear();
    interactive.run(true, false, false);
    CHECK(contradiction_lines(collector) == 0);
    CHECK(any_event_contains(collector, "already recorded"));

    collector.clear();
    interactive.process("S sleeps O");
    CHECK(collect_answers(collector) == std::vector<std::string>{"mary sleeps nil"});

    collector.clear();
    interactive.process("S ~ refuted");
    CHECK(collect_answers(collector).empty());
    const std::vector<std::string> records = recorded_contradictions(collector, interactive);
    REQUIRE(records.size() == 1);
    CHECK(records.front().find("(mary sleeps nil)") != std::string::npos);
    CHECK(records.front().find("(mary ~ person)") != std::string::npos);
}

// The same contradiction arises regardless of which of the two derivations
// comes first: either the fact is derived before its refutation, or the
// refutation is derived before the fact -- in which case it confronts an F
// that is known to be wrong.
TEST_CASE("refuted marking: a derived one meeting a claimed fact is a contradiction whatever the order")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(X ~ tired) => (X sleeps nil)\n(X ~ person) => ((X sleeps nil) ~ refuted)\n");
        collector.clear();

        SUBCASE("the fact first") { process_lines(interactive, "mary ~ tired\nmary ~ person\n"); }
        SUBCASE("the refutation first") { process_lines(interactive, "mary ~ person\nmary ~ tired\n"); }

        CHECK(has_contradiction(collector)); });
}

// A firing claims what its consequence writes, and a marking within a
// statement among it refuted its fact, which the marking alone no longer
// does: following "t p b", the rule "(X go Y) => (k says ((t p b) ~
// refuted))" un-claimed `t p b` at its first firing, and each subsequent run
// reported a consequence that nests a refuted fact. It is the identical
// contradiction to that of the derived marking, sharing the same record, and
// nothing is written.
//
// This test was named "refuted marking: a derived statement nesting one of a
// claimed fact is a contradiction"; it read the record via `S ~ refuted`,
// which answers no record any more (see "refuted marking: a derived one
// meeting a claimed fact is a contradiction, recorded with both facts").
TEST_CASE("refuted marking: a derived statement nesting one of a claimed fact is a contradiction, recorded with both facts")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "t p b\nt go k\n");
        collector.clear();

        SUBCASE("a ground marking") { interactive.process("(X go Y) => (k says ((t p b) ~ refuted))"); }
        SUBCASE("a marking the binding completes") { interactive.process("(X go Y) => (k says ((X p b) ~ refuted))"); }

        CHECK(has_contradiction(collector));

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"t p b"});
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S ~ refuted");
        CHECK(collect_answers(collector).empty());
        const std::vector<std::string> records = recorded_contradictions(collector, interactive);
        REQUIRE(records.size() == 1);
        CHECK(records.front().find("(t p b)") != std::string::npos);
        CHECK(records.front().find("(t go k)") != std::string::npos); });
}

// The members of a container that the consequence writes are nested in the
// statement too: a set, a collection, a comma list. The prediction did not
// enter a container, thus after "t p b", the rule
// "(X go Y) => (k says {((X p b) ~ refuted)})" refuted the claimed `t p b`
// without a word, also via the `refuted` a binding supplies, and the next
// line stopped with "known to be wrong"; with a comma list lacking
// variables, the statement was derived and its marking refuted nothing. The
// typed statement is refused in every shape.
TEST_CASE("refuted marking: a derived statement holding one of a claimed fact in a container is a contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t p b");
        std::string binding = "t go k";
        std::string rule;
        SUBCASE("in a set") { rule = "(X go Y) => (k says {((X p b) ~ refuted)})"; }
        SUBCASE("in a set with the refuted a binding supplies")
        {
            binding = "t go refuted";
            rule    = "(X go Y) => (k says {((t p b) ~ Y)})";
        }
        SUBCASE("in a collection") { rule = "(X go Y) => (k says @{((X p b) ~ refuted)})"; }
        SUBCASE("in a comma list") { rule = "(X go Y) => (k says ((t p b) ~ refuted, j r s))"; }
        SUBCASE("in a set nested in a statement") { rule = "(X go Y) => (k says (j r {((X p b) ~ refuted)}))"; }
        interactive.process(binding);
        collector.clear();
        CHECK_NOTHROW(interactive.process(rule));
        CHECK(has_contradiction(collector));

        collector.clear();
        CHECK_NOTHROW(interactive.process("S p O"));
        CHECK(collect_answers(collector) == std::vector<std::string>{"t p b"});
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());

        const std::vector<std::string> records = recorded_contradictions(collector, interactive);
        REQUIRE(records.size() == 1);
        CHECK(records.front().find("(t p b)") != std::string::npos);
        CHECK(records.front().find("(" + binding + ")") != std::string::npos); });
}

// A firing claims the members of a comma list its consequence writes, and a
// list that asserts a rule's pattern while refuting it constitutes the
// contradiction the typed "x says (c q d, (c q d) ~ refuted)" is refused as.
// The claim did not enter the list, thus the rule was derived without a word,
// and the refutation was never applied. Which of the two claims the walk meets
// first determines what the refusal names, so the test asks for either.
TEST_CASE("refuted marking: a derived comma list that asserts a rule's pattern it refutes is a contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(c q d) => (e r f)\nt go k\n");
        collector.clear();
        CHECK_NOTHROW(interactive.process("(X go Y) => (k says (c q d, (c q d) ~ refuted))"));
        CHECK(has_contradiction(collector));
        CHECK(any_event_contains(collector, "refused: this fact is known to be"));
        CHECK_NOTHROW(interactive.run(true, false, false));
        CHECK_NOTHROW(interactive.process("harmless rel statement")); });
}

// The claim walk for this kind of firing is refused midway, once the
// derived statement has come into existence and after the claim it met
// first: the statement stayed, either with the refutation of `a p b` or with
// its claim, which turned the rule's pattern into data and derived `c q d`.
// The typed statements are refused and retain nothing, as the firing does
// now. The configurations match those of the case above and of its set and
// two-object siblings.
TEST_CASE("refuted marking: a derived statement whose claim is refused midway keeps nothing of it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string rule;
        SUBCASE("in a set") { rule = "(X go Y) => (k says {(a p b) ((a p b) ~ refuted)})"; }
        SUBCASE("as two objects") { rule = "(X go Y) => (k says (a p b) ((a p b) ~ refuted))"; }
        SUBCASE("as subject and object") { rule = "(X go Y) => ((a p b) says ((a p b) ~ refuted))"; }
        SUBCASE("in a comma list") { rule = "(X go Y) => (k says (a p b, (a p b) ~ refuted))"; }
        process_lines(interactive, "(a p b) => (c q d)\nt go k\n");
        collector.clear();
        CHECK_NOTHROW(interactive.process(rule));
        CHECK(any_event_contains(collector, "refused: this fact is known to be"));
        for (const char* query : {"X ~ refuted", "S p O", "S says O", "C q D"})
        {
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        } });
}

// The firing's twin of "one a claimed statement writes refutes a fact nobody
// claimed": a marking that a derived comma list holds refutes its underlying
// fact, just as the typed `x says ((a p b) ~ refuted, j r s)` does. The claim
// did not enter the comma list, thus the marking stayed a pattern of the
// rule, `X ~ refuted` answered nothing, and a later `a p b` met the marking
// as a contradiction rather than being refused.
TEST_CASE("refuted marking: one in a comma list a derived statement writes refutes a fact nobody claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a new fact") {}
        SUBCASE("a rule's pattern") { interactive.process("(a p b) => (c q d)"); }

        process_lines(interactive, "t go k\n");
        collector.clear();
        CHECK_NOTHROW(interactive.process("(X go Y) => (k says ((a p b) ~ refuted, j r s))"));
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(a p b) ~ refuted"});
        collector.clear();
        interactive.process("C q D");
        CHECK(collect_answers(collector).empty());
        CHECK_THROWS_WITH(interactive.process("a p b"), doctest::Contains("this fact is known to be wrong")); });
}

// A contradiction arises from any spelling that constructs a derived marking
// of a claimed fact. When `refuted` or `~` is provided via a binding, the
// prediction failed to detect the marking: after "t p b" and "t go refuted",
// the rule "(X go Y) => ((t p b) ~ Y)" -- or a version that nests this marking
// within a statement -- removed `t p b` from every query without a word, and
// each subsequent run reported a contradiction. It is now the contradiction
// produced by a written `refuted`, maintaining the identical record, and
// nothing is written.
//
// This test was named "refuted marking: a derived one whose refuted a binding
// supplies meets a claimed fact as a contradiction"; it read the record via
// `S ~ refuted`, which answers no record anymore (see "refuted marking: a
// derived one meeting a claimed fact is a contradiction, recorded with both
// facts").
TEST_CASE("refuted marking: a derived one whose refuted a binding supplies meets a claimed fact as a contradiction, recorded with both facts")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t p b");
        std::string binding = "(t go ";
        SUBCASE("as the consequence")
        {
            process_lines(interactive, "t go refuted\n(X go Y) => ((t p b) ~ Y)\n");
        }
        SUBCASE("nested in a statement")
        {
            process_lines(interactive, "t go refuted\n(X go Y) => (k says ((t p b) ~ Y))\n");
        }
        SUBCASE("nested two levels deep")
        {
            process_lines(interactive, "t go refuted\n(X go Y) => (k says (j says ((t p b) ~ Y)))\n");
        }
        SUBCASE("nested with the ~ a binding supplies")
        {
            process_lines(interactive, "t go ~\n(X go Y) => (k says ((t p b) Y refuted))\n");
        }
        // The fact the marking names serves as the value of a binding: the
        // walk does not enter the value a variable supplies, yet the marking
        // that the firing builds around it refutes it all the same.
        SUBCASE("nested with the fact and the refuted a binding supplies")
        {
            process_lines(interactive, "(t p b) go refuted\n(X go Y) => (k says (X ~ Y))\n");
            binding = "((t p b) go refuted)";
        }
        CHECK(has_contradiction(collector));

        collector.clear();
        CHECK_NOTHROW(interactive.run(true, false, false));
        CHECK(contradiction_lines(collector) == 0);

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"t p b"});
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S ~ refuted");
        CHECK(collect_answers(collector).empty());
        const std::vector<std::string> records = recorded_contradictions(collector, interactive);
        REQUIRE(records.size() == 1);
        CHECK(records.front().find("(t p b)") != std::string::npos);
        CHECK(records.front().find(binding) != std::string::npos); });
}

// This test was named
// "a derived claim of a marking pattern the prediction misses is a contradiction, not an abort":
// the prediction read a marking from the consequence as it was written, thus
// `(a p b) ~ Y` with Y bound to `refuted` did not constitute a marking for it,
// and the firing claimed the existing marking, a rule's pattern. That claim
// was refused for a claimed fact (unmark_rule_pattern), and the refusal
// escaped the run and made every later line fail; the catch in deduce
// transformed it into the predicted contradiction. The prediction now reads
// the binding, so the contradiction is reported before any construction, and
// the name says what the test establishes rather than the method. Nested
// within a statement, the claim met the marking only after the statement was
// written: the run reported "refused: this fact is known to be true" with no
// record and left `k says ((a p b) ~ refuted)` in the graph.
//
// Then named "refuted marking: a derived claim of a rule's marking pattern of
// a claimed fact is a contradiction, not an abort", it read the record via
// `S ~ refuted`, which answers no record anymore (see "refuted marking: a
// derived one meeting a claimed fact is a contradiction, recorded with both
// facts").
TEST_CASE("refuted marking: a derived claim of a rule's marking pattern of a claimed fact is a contradiction recorded with both facts, not an abort")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "a p b\n((a p b) ~ refuted) => (k r s)\nt go refuted\n");
        collector.clear();

        SUBCASE("as the consequence") { CHECK_NOTHROW(interactive.process("(X go Y) => ((a p b) ~ Y)")); }
        SUBCASE("nested in a statement") { CHECK_NOTHROW(interactive.process("(X go Y) => (k says ((a p b) ~ Y))")); }
        CHECK(has_contradiction(collector));
        CHECK_FALSE(any_event_contains(collector, "refused"));

        collector.clear();
        CHECK_NOTHROW(interactive.run(true, false, false));
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});
        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S ~ refuted");
        CHECK(collect_answers(collector).empty());
        const std::vector<std::string> records = recorded_contradictions(collector, interactive);
        REQUIRE(records.size() == 1);
        CHECK(records.front().find("(a p b)") != std::string::npos);
        CHECK(records.front().find("(t go refuted)") != std::string::npos); });
}

// A statement claims what it nests, meaning that a fact which only a belief
// or another statement holds is claimed, and a refutation obtained through
// derivation is the same contradiction as that of a fact explicitly typed on
// a separate line.
TEST_CASE("refuted marking: a fact claimed inside another statement is claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "mary believes (john waits nil)\n(X ~ man) => ((X waits nil) ~ refuted)\n");
        collector.clear();
        interactive.process("john ~ man");
        CHECK(has_contradiction(collector));

        collector.clear();
        interactive.process("S waits O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john waits nil"}); });
}

// What the silent un-claim accomplished beyond hiding the fact: a negation
// read the user's own assertion as absent and a rule derived from that,
// while a denial over the negation found a contradiction that no one had
// stated. A rejected marking writes nothing, so neither outcome occurs.
TEST_CASE("refuted marking: a rejected marking derives nothing under ¬ and causes no other contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((X ~ person), ¬(X sleeps nil)) => (X ~ awake)
(X ~ tired, ¬(X sleeps nil)) => !
(X ~ person) => ((X sleeps nil) ~ refuted)
mary sleeps nil
mary ~ tired
)");
        collector.clear();
        interactive.process("mary ~ person");
        CHECK(any_event_contains(collector, "! ⇐ (mary ~ person)"));
        CHECK_FALSE(any_event_contains(collector, "mary ~ tired"));

        collector.clear();
        interactive.process("X ~ awake");
        CHECK(collect_answers(collector).empty()); });
}

// When nothing claimed the fact, a refutation obtained through derivation
// is merely that, and "X ~ refuted" presents it in a list.
TEST_CASE("refuted marking: X ~ refuted answers a derived refutation of an unclaimed fact")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(X ~ man) => ((X waits nil) ~ refuted)\njohn ~ man\n");
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(john waits nil) ~ refuted"}); });
}

// A program requests refutations using a marking pattern where the fact holds
// a variable, constructed within a scratch cluster and then queried. This
// remains permissible alongside the refusal below: the pattern does not
// constitute a refutation, the query yields responses for the refuted fact
// exclusively, and the claimed fact beside it keeps answering.
TEST_CASE("refuted marking: a pattern with a variable can still be queried from Janet")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, "¬(bill waits nil)\njohn waits nil\n");
    const std::size_t before = node_count(collector, interactive);

    collector.clear();
    interactive.process(R"js(%(do (def prev (zelph/cluster)) (zelph/cluster "probe") (def x (zelph/var "X")) (def rows (zelph/query (zelph/fact (zelph/fact x "waits" "nil") "~" "refuted"))) (zelph/cluster prev) (zelph/cluster-drop "probe") (zelph/out (string "rows=" (length rows) " " (string/join (map |(string ($ 'X)) rows) ",")))))js");
    CHECK(any_output_contains(collector, "rows=1 bill"));
    CHECK(node_count(collector, interactive) == before);

    collector.clear();
    interactive.process("S waits O");
    CHECK(collect_answers(collector) == std::vector<std::string>{"john waits nil"});
}

// A ground marking composed using zelph/fact serves as a refutation, and a
// refutation has its own call, which refuses one of a fact the graph claims.
// Constructed via zelph/fact, the marking silently un-claimed the fact; the
// refusal names the call to use.
TEST_CASE("refuted marking: a ground one from Janet is refused and names zelph/refute")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("john waits nil");
    CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/fact (zelph/fact "john" "waits" "nil") "~" "refuted"))js"), doctest::Contains("zelph/refute"));

    collector.clear();
    interactive.process("S waits O");
    CHECK(collect_answers(collector) == std::vector<std::string>{"john waits nil"});
}

// A query asserts its ground parts, and a ground marking within them
// constitutes the refutation it spells: refused for a fact the graph claims
// -- the question "X sleeps nil, (john waits nil) ~ refuted" un-claimed
// `john waits nil` before it was answered -- and for one it does not,
// written once the query has fully formed, so the query answers with it.
TEST_CASE("refuted marking: a ground one a query asserts is read as a refutation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "john waits nil\nbob sleeps nil\n");
        CHECK_THROWS_WITH(interactive.process("X sleeps nil, (john waits nil) ~ refuted"), doctest::Contains("this fact is known to be true"));
        collector.clear();
        interactive.process("S waits O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"john waits nil"});

        collector.clear();
        interactive.process("X sleeps nil, (mary waits nil) ~ refuted");
        CHECK(collect_answers(collector).size() == 1);
        CHECK_THROWS_WITH(interactive.process("mary waits nil"), doctest::Contains("this fact is known to be wrong")); });
}

// `¬` is a statement spelling, so it reads as one only where the node IS the
// statement. The marking fact printed itself as "(¬(a p b)) ~ refuted", and
// there the prefix is the OTHER operator -- negation as failure -- which a
// plain statement has no reading for: pasting that answer back added a negation
// tag nobody had written, and `.node` reported "Negated by a rule: yes" for it.
// The predicate of the marking fact already says what the prefix was saying
// twice, so inside the term it goes.
TEST_CASE("refuted marking: the printed marking is the marking that was written")
{
    const auto refuted_answers = [](const char* line)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(line);
        collector.clear();
        interactive.process("S ~ refuted");
        return collect_answers(collector);
    };

    const std::vector<std::string> printed = refuted_answers("(a p b) ~ refuted");
    REQUIRE(printed.size() == 1);
    CHECK(printed.front().find("(a p b) ~ refuted") != std::string::npos);

    // The round trip is the assertion: what came back has to name the same
    // marking, and only that one -- a second answer would be the negation tag
    // the old spelling brought with it.
    CHECK(refuted_answers(printed.front().c_str()) == printed);

    // Standing alone the node still prints as the claim it is. That is the
    // direction the prefix was introduced for, and dropping it there would
    // re-enter as the opposite of what the graph holds.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("¬(a p b)");
    collector.clear();
    interactive.process(".explain (a p b)");
    CHECK(any_output_contains(collector, "¬(a p b)"));

    collector.clear();
    interactive.process(".node a p b");
    CHECK(any_output_contains(collector, "Refuted (claimed not to hold): yes"));
    CHECK_FALSE(any_output_contains(collector, "Negated by a rule"));
}

// A slice promises the facts of the named predicates and nothing else. The
// record is the one thing that can break that promise from the inside: it is
// the set of the facts that matched, reached by expanding any ONE of them, and
// expanding it in turn reaches the others -- whatever predicate they belong to.
TEST_CASE("contradiction record: it does not travel with a predicate slice")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_slice.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
        REQUIRE(contradiction_lines(collector) == 1);
        interactive.process(".save-predicates \"" + file.string() + "\" p");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    collector.clear();

    // The q fact is not in the slice, and neither is the record about it. The
    // marking fact has to stay out with it: the saver writes the EDGES of what
    // it keeps, so a kept marking fact rebuilds the set -- and the set rebuilds
    // its members.
    interactive.process("S q O");
    CHECK(collect_answers(collector).empty());

    collector.clear();
    interactive.process("S ~ refuted");
    CHECK(collect_answers(collector).empty());

    // What the slice DOES keep is the fact of the named predicate and the rule.
    collector.clear();
    interactive.process("S p O");
    CHECK(answers_contain(collector, "x p y"));

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "=> !"));

    std::filesystem::remove(file);
}

// The record stays behind whole. Its memberships were accessed via the
// member that the slice keeps, and they moved without their set: once a
// slice of `p` beside a contradiction was loaded, `S in O` answered
// `:in @{?}`, a membership whose container remained behind, and likewise
// `S P O`. Slices of `in` and of `~` take nothing from the record either.
TEST_CASE("contradiction record: a predicate slice leaves its memberships behind with it")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_slice_memberships.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        std::string predicates;
        SUBCASE("a slice of a member's predicate") { predicates = "p"; }
        SUBCASE("a slice of `in`") { predicates = "p in"; }
        SUBCASE("a slice of `~`") { predicates = "p ~"; }
        CAPTURE(predicates);

        process_lines(interactive, "x p y\nx q y\n(A p B, A q B) => !\n");
        REQUIRE(contradiction_lines(collector) == 1);
        interactive.process(".save-predicates \"" + file.string() + "\" " + predicates);
        interactive.process(".load \"" + file.string() + "\"");

        CHECK(answers("S in O").empty());
        CHECK(answers("S q O").empty());
        CHECK(answers("S p O") == std::vector<std::string>{"x p y"});
        CHECK(recorded_contradictions(collector, interactive).empty());
        for (const std::string& answer : answers("S P O"))
            CHECK_MESSAGE(answer.find("@{") == std::string::npos, answer); });
    std::filesystem::remove(file);
}

// A set that a statement names constitutes that statement's set,
// not merely the record (see "contradiction record: a set a statement names
// keeps its memberships, and only the marking is the record's"), and it
// moves just like any other set, carrying its memberships and everything
// those reach; solely the marking stays behind. The slice treated the set as
// the record: a statement naming it as its object moved onward without it,
// `S has O` answering `:has k`, while one naming it as its subject stayed
// behind even though its predicate was sliced and counted. The rules move,
// thus the first run over the slice records the contradiction again.
TEST_CASE("contradiction record: a set a statement names travels with a slice, and the record stays behind")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_slice_named_set.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::sort(result.begin(), result.end());
            return result;
        };

        std::string holder;
        std::string predicate;
        std::string query;
        SUBCASE("the set as the object")
        {
            holder    = "k has {(x p y) (x q y)}";
            predicate = "has";
            query     = "S has O";
        }
        SUBCASE("the set as the subject")
        {
            holder    = "{(x p y) (x q y)} label foo";
            predicate = "label";
            query     = "S label O";
        }
        CAPTURE(holder);

        process_lines(interactive, "x p y\nx q y\n" + holder + "\n(A p B, A q B) => !\n");
        REQUIRE(contradiction_lines(collector) == 1);
        interactive.process(".save-predicates \"" + file.string() + "\" " + predicate);
        interactive.process(".load \"" + file.string() + "\"");

        CHECK(answers(query) == std::vector<std::string>{holder});
        CHECK(answers("S in O") == std::vector<std::string>{"(x p y) in {(x p y) (x q y)}", "(x q y) in {(x p y) (x q y)}"});
        CHECK(recorded_contradictions(collector, interactive).empty());

        collector.clear();
        interactive.process(".run");
        CHECK(contradiction_lines(collector) == 1);
        CHECK(recorded_contradictions(collector, interactive) == std::vector<std::string>{"{(x p y) (x q y)}"}); });
    std::filesystem::remove(file);
}

// The same statement in a slice of `in`: the slice took the set for the
// record's and left it behind, while the statement travelled without its
// second container, thus after the load `S in bag O` found nothing and
// `S in O` read `(x p y) in bag`. The statement travels whole, accompanied
// by the set and its memberships.
TEST_CASE("contradiction record: a membership naming the set beside another container travels whole with a slice")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_slice_two_containers.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::sort(result.begin(), result.end());
            return result;
        };

        process_lines(interactive, "x p y\nx q y\n(x p y) in bag {(x p y) (x q y)}\n(A p B, A q B) => !\n");
        REQUIRE(contradiction_lines(collector) == 1);
        interactive.process(".save-predicates \"" + file.string() + "\" in");
        interactive.process(".load \"" + file.string() + "\"");

        CHECK(answers("S in bag O") == std::vector<std::string>{"(x p y) in bag {(x p y) (x q y)}"});
        CHECK(answers("(x q y) in O") == std::vector<std::string>{"(x q y) in {(x p y) (x q y)}"});
        CHECK(recorded_contradictions(collector, interactive).empty()); });
    std::filesystem::remove(file);
}

// A record remains behind within a slice, yet something that travels may
// hold it, and leaving the record behind then broke the holder: in a `says`
// slice, `x says ({(x p y) (x q y)} ~ refuted)` lost its object and answered
// `:says x`, while a rule whose condition names a record's set beside a
// variable, which travels with each slice, arrived as
// `(:label X) => (X ok yes)`. This kind of condition does not count as a
// statement that names the set, thus the record keeps hiding its memberships
// in the source. A holder of the marking or of the set now takes the record in
// full, and the slice reads it as the source does: the record exists, its
// memberships fail to answer any query, and a run over the slice finds the
// contradiction already recorded. Both setups are user-defined: one statement
// concerning a refutation, and one rule regarding a set.
TEST_CASE("contradiction record: a statement or a rule that holds a record takes it whole into a slice")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_slice_held_record.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::sort(result.begin(), result.end());
            return result;
        };
        const auto arrived_whole = [&]()
        {
            CHECK(recorded_contradictions(collector, interactive) == std::vector<std::string>{"{(x p y) (x q y)}"});
            CHECK(answers("S in O").empty());
            collector.clear();
            interactive.process(".run");
            CHECK(contradiction_lines(collector) == 0);
        };

        SUBCASE("a statement nesting the record's marking")
        {
            const std::string holder = "x says ({(x p y) (x q y)} ~ refuted)";
            process_lines(interactive, "x p y\nx q y\n(A p B, A q B) => !\n" + holder + "\n");
            REQUIRE(contradiction_lines(collector) == 1);
            interactive.process(".save-predicates \"" + file.string() + "\" says");
            interactive.process(".load \"" + file.string() + "\"");

            CHECK(answers("S says O") == std::vector<std::string>{holder});
            arrived_whole();
        }
        SUBCASE("a rule naming the record's set beside a variable")
        {
            process_lines(interactive, "x p y\nx q y\n(A p B, A q B) => !\n(X label {(x p y) (x q y)}) => (X ok yes)\n");
            REQUIRE(contradiction_lines(collector) == 1);
            interactive.process(".save-predicates \"" + file.string() + "\" p");
            interactive.process(".load \"" + file.string() + "\"");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "(X label {(x p y) (x q y)}) => (X ok yes)"));
            arrived_whole();

            process_lines(interactive, "k label {(x p y) (x q y)}\n.run\n");
            CHECK(answers("S ok O") == std::vector<std::string>{"k ok yes"});
        } });
    std::filesystem::remove(file);
}

// A statement may additionally nest the marking of a set that a different
// statement names. That set travels just like any other set (refer to "a set
// a statement names travels with a slice, and the record stays behind"), and
// only the marking stayed behind, so within a slice of `says` the holder lost
// its object and answered `:says x`. When reached from the holder, the
// marking travels as well: the slice holds the record, the memberships answer
// as they do in the source, and a run across the slice finds the
// contradiction that was already recorded.
TEST_CASE("contradiction record: a statement nesting the marking of a set a statement names takes the marking into a slice")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_slice_held_named_marking.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            auto result = collect_answers(collector);
            std::sort(result.begin(), result.end());
            return result;
        };

        const std::string holder = "x says ({(x p y) (x q y)} ~ refuted)";
        process_lines(interactive, "x p y\nx q y\n(A p B, A q B) => !\n{(x p y) (x q y)} label foo\n" + holder + "\n");
        REQUIRE(contradiction_lines(collector) == 1);
        interactive.process(".save-predicates \"" + file.string() + "\" says");
        interactive.process(".load \"" + file.string() + "\"");

        CHECK(answers("S says O") == std::vector<std::string>{holder});
        CHECK(answers("S in O") == std::vector<std::string>{"(x p y) in {(x p y) (x q y)}", "(x q y) in {(x p y) (x q y)}"});
        CHECK(recorded_contradictions(collector, interactive) == std::vector<std::string>{"{(x p y) (x q y)}"});
        collector.clear();
        interactive.process(".run");
        CHECK(contradiction_lines(collector) == 0); });
    std::filesystem::remove(file);
}

// Whether a record stays behind depends on what the closure has reached so
// far: if a `says` statement nests the record's marking, the record is taken
// along. A slice therefore cannot tell a record's statement based on whether
// it stays behind, because doing so would count the marking as a fact of `~`
// when `says` is named before `~`, and not as a fact when `says` is named
// afterwards, similarly applying to memberships as facts of `in`. The count
// remains unchanged regardless of order, and the record's statements are not
// included in it.
TEST_CASE("contradiction record: a slice counts none of a record's statements, whichever predicate is named first")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_slice_record_count.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        const auto saved = [&](const std::string& predicates)
        {
            collector.clear();
            interactive.process(".save-predicates \"" + file.string() + "\" " + predicates);
            std::string report;
            for (const auto& event : collector.events())
                if (event.text.find("Saved ") != std::string::npos) report = event.text;
            return report;
        };

        process_lines(interactive, "x p y\nx q y\n(A p B, A q B) => !\nx says ({(x p y) (x q y)} ~ refuted)\n");
        REQUIRE(contradiction_lines(collector) == 1);

        SUBCASE("the marking, a fact of ~")
        {
            const std::string first = saved("says ~");
            CHECK_FALSE(first.empty());
            CHECK(first == saved("~ says"));
        }
        SUBCASE("the memberships, facts of in")
        {
            const std::string first = saved("says in");
            CHECK(first.find("Saved 1 fact(s) of 2 predicate(s)") != std::string::npos);
            CHECK(first == saved("in says"));
        } });
    std::filesystem::remove(file);
}

// A refutation is not a record: a slice that keeps a fact keeps its
// refutation, the marking, and the weight, ensuring the loaded slice aligns
// with the session that wrote it. The refuted fact was left behind alongside
// the records, and its marking with it: after "¬(mary waits nil)", a slice of
// `waits` and `~` loaded without the refutation and accepted "mary waits nil".
// A refuted fact belonging to a predicate that the slice names travels even
// though nobody claims it, just as the predicate's declaration does.
TEST_CASE("refuted marking: a refutation travels with a predicate slice")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_refutation_slice.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        std::string predicates;
        bool        waits = true;
        SUBCASE("a slice of the fact's predicate and of ~") { predicates = "waits ~"; }
        SUBCASE("a slice of the fact's predicate") { predicates = "waits"; }
        SUBCASE("a slice of ~")
        {
            predicates = "~";
            waits      = false;
        }
        process_lines(interactive, "¬(mary waits nil)\nbob waits nil\n");
        interactive.process(".save-predicates \"" + file.string() + "\" " + predicates);
        interactive.process(".load \"" + file.string() + "\"");
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(mary waits nil) ~ refuted"});
        collector.clear();
        interactive.process("S waits O");
        CHECK(collect_answers(collector) == (waits ? std::vector<std::string>{"bob waits nil"} : std::vector<std::string>{}));
        CHECK_THROWS_WITH(interactive.process("mary waits nil"), doctest::Contains("known to be wrong")); });

    std::filesystem::remove(file);
}

// A fact that has been refuted, which serves as a rule's pattern. Each rule
// moves in tandem with a slice, and its pattern accompanies it; the
// refutation of the pattern was left behind and took the pattern with it,
// thus the rule reached its destination lacking its condition: the loaded
// slice listed no rule, answered no refutation, and accepted
// "mary waits nil".
TEST_CASE("refuted marking: a refuted pattern of a rule travels with a predicate slice")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_refuted_pattern_slice.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "(mary waits nil) => (x y z)\n¬(mary waits nil)\nbob waits nil\n");
        interactive.process(".save-predicates \"" + file.string() + "\" waits");
        interactive.process(".load \"" + file.string() + "\"");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(mary waits nil) => (x y z)"));
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(mary waits nil) ~ refuted"});

        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S y O");
        CHECK(collect_answers(collector).empty());
        CHECK_THROWS_WITH(interactive.process("mary waits nil"), doctest::Contains("known to be wrong")); });

    std::filesystem::remove(file);
}

// A partial view reads a slice via its own loader, and the refutation
// contained within the slice holds there as well.
TEST_CASE("refuted marking: a partial view of a predicate slice keeps its refutation")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_refutation_partial.bin";
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "¬(mary waits nil)\nbob waits nil\n");
        interactive.process(".save-predicates \"" + file.string() + "\" waits");
        interactive.process(".load-partial \"" + file.string() + "\"");
        collector.clear();
        interactive.process("X ~ refuted");
        CHECK(collect_answers(collector) == std::vector<std::string>{"(mary waits nil) ~ refuted"});
        CHECK_THROWS_WITH(interactive.process("mary waits nil"), doctest::Contains("known to be wrong")); });

    std::filesystem::remove(file);
}

TEST_CASE("contradiction record: the switch is honoured in both directions")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    // Counted per evaluation, as in the case above: with records off,
    // check mode's second, classic evaluation would report each
    // contradiction twice.
    interactive.process(".semi-naive on");

    collector.clear();
    interactive.process(".contradiction-records");
    CHECK(any_output_contains(collector, "Contradiction records: on"));

    interactive.process(".contradiction-records off");
    process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
    collector.clear();
    interactive.process("harmless rel statement");

    // Off: nothing was written down, so the next run finds it again. This is
    // the behaviour every version before the record had.
    CHECK(contradiction_lines(collector) == 1);

    // On again -- re-armable, unlike .fact-stores, because the absence of a
    // record means nothing.
    interactive.process(".contradiction-records on");
    interactive.process("another rel statement");
    collector.clear();
    interactive.process("a third rel statement");
    CHECK(contradiction_lines(collector) == 0);
}

// A run that stays quiet is still a run in which the contradiction was
// encountered, and the export is a record of the run. Without this a second
// `.run-export` handed back a file with nothing in it.
TEST_CASE("contradiction record: the export is filled even when the line is not printed")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_contradiction_export.json";

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    process_lines(interactive, R"(
x p y
x q y
(A p B, A q B) => !
)");
    REQUIRE(contradiction_lines(collector) == 1);

    collector.clear();
    interactive.process(".run-export \"" + file.string() + "\"");
    CHECK(contradiction_lines(collector) == 0); // quiet, as it should be

    REQUIRE(std::filesystem::exists(file));
    std::string exported;
    {
        // Scoped, because Windows will not remove a file that is still
        // open: on Linux the same code deletes it and the reader goes on
        // reading, so nothing here said that the handle was still held.
        std::ifstream in(file);
        exported.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(exported.find("contradiction") != std::string::npos);

    std::filesystem::remove(file);
}
