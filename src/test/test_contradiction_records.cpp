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

// A slice promises the facts of the named predicates and nothing else. The
// record is the one thing that can break that promise from the inside: it is
// the set of the facts that matched, reached by expanding any ONE of them, and
// expanding it in turn reaches the others -- whatever predicate they belong to.
// The index is a CACHE of the marking facts, and a cache has to agree with
// what it caches whichever way round the graph was built. rebuild_refuted_index
// reads the markings back on load, so one written BY HAND has to reach the
// index when it is written, or the same network answers differently before and
// after a save and a load -- which is the one thing a round trip may never do.
//
// Whether writing an engine marking by hand SHOULD mean anything is a separate,
// open question (`(myrel ~ ->) is odd` is the same shape). This test does not
// answer it. It pins that the answer is the same on both sides of the file.
TEST_CASE("refuted marking: a hand-written one means the same before and after a round trip")
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
a p b
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

    // And the marking is honoured, not ignored: `c p d` answers, `a p b` does
    // not.
    CHECK(after.size() == 1);
    CHECK(after.front().find("c p d") != std::string::npos);

    std::filesystem::remove(file);
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

TEST_CASE("contradiction record: the switch is honoured in both directions")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

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
    std::ifstream     in(file);
    const std::string exported((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(exported.find("contradiction") != std::string::npos);

    std::filesystem::remove(file);
}
