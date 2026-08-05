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
#include <string>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Predicate slices (.save-predicates) and the class tools built on them.
//
// A slice is what makes a graph that needs hundreds of gigabytes answerable
// on an ordinary machine: it keeps the facts of a few predicates and drops
// everything else. The tests below pin the three properties that decide
// whether the result is usable rather than merely small:
//
//   1. it contains the named facts and NOT the others, including facts of
//      another predicate between two retained nodes;
//   2. it still answers queries -- which depends on the relation-type
//      declaration travelling with it, an invisible prerequisite whose
//      absence turns every query over the slice into "no results";
//   3. the closure engine agrees with the source network, since transitive
//      questions over the class hierarchy are the reason slices exist.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    fs::path slice_path(const std::string& name)
    {
        return fs::temp_directory_path() / ("zelph_slice_test_" + name + ".bin");
    }

    // Q10 -> Q20 -> Q30 is the class chain; Q40 hangs off Q20 and Q50.
    // P31 and P106 facts share their nodes with the P279 facts, so a slice
    // that leaked them would be visible immediately.
    void build_source(const zelph::console::Interactive& interactive)
    {
        process_lines(interactive, R"(
.lang wikidata
Q10 P279 Q20
Q20 P279 Q30
Q40 P279 Q20
Q40 P279 Q50
Q10 P279 Q50
Q10 P31 Q20
Q11 P106 Q10
)");
        interactive.process(".name Q10 en \"little thing\"");
    }
}

TEST_CASE("slice: keeps the named predicate and drops the rest")
{
    const auto file = slice_path("basic");

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        build_source(interactive);
        interactive.process(".save-predicates \"" + file.string() + "\" P279");
        CHECK(any_event_contains(collector, "Saved 5 fact(s)"));
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    interactive.process(".lang wikidata");

    // Q10 Q20 Q30 Q40 Q50 P279 -- Q11 and P106 were only mentioned by facts
    // of another predicate and are gone with them. Asked BEFORE the queries
    // below: entering a pattern names its variables, which would count.
    collector.clear();
    interactive.process(".stat");
    CHECK(any_output_contains(collector, "wikidata: 6"));

    collector.clear();
    interactive.process("A P279 Q20");
    CHECK(answers_contain(collector, "Q10 P279 Q20"));
    CHECK(answers_contain(collector, "Q40 P279 Q20"));

    // The P31 fact connects two nodes that both survived, and must still be
    // gone -- the slice is defined by its predicates, not by its nodes.
    collector.clear();
    interactive.process("A P31 B");
    CHECK(collect_answers(collector).empty());

    fs::remove(file);
}

TEST_CASE("slice: names of retained nodes survive in every language")
{
    const auto file = slice_path("names");

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        build_source(interactive);
        interactive.process(".save-predicates \"" + file.string() + "\" P279");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");

    collector.clear();
    interactive.process(".lang en");
    interactive.process(".node \"little thing\"");
    CHECK(any_event_contains(collector, "Q10"));

    // Q11 took part in no P279 fact, so it and its name went with it.
    collector.clear();
    interactive.process(".lang wikidata");
    CHECK_THROWS(interactive.process(".node Q11"));

    fs::remove(file);
}

TEST_CASE("slice: the closure over the slice equals the closure over the source")
{
    const auto file = slice_path("closure");

    std::string from_source;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        build_source(interactive);
        interactive.process(".import wikidata-classes");
        collector.clear();
        interactive.process("%(class-report \"Q10\")");
        from_source = normalize(last_out_text(collector));
        interactive.process(".save-predicates \"" + file.string() + "\" P279");
    }

    REQUIRE(from_source.find("Subclasses") != std::string::npos);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    interactive.process(".import wikidata-classes");
    collector.clear();
    interactive.process("%(class-report \"Q10\")");

    CHECK(normalize(last_out_text(collector)) == from_source);

    fs::remove(file);
}

TEST_CASE("slice: a rule that travels with the slice still fires and still explains")
{
    const auto file = slice_path("rule");

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
.lang wikidata
(R P31 P1, A R B, B R C) => (A R C)
P279 P31 P1
Q10 P279 Q20
Q20 P279 Q30
)");
        // The rule fires on the source: that is the baseline the slice has
        // to reproduce.
        collector.clear();
        interactive.process("Q10 P279 X");
        REQUIRE(answers_contain(collector, "Q10 P279 Q30"));

        interactive.process(".save-predicates \"" + file.string() + "\" P279 P31");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    interactive.process(".auto-run");
    interactive.process(".lang wikidata"); // .load leaves the language at zelph

    // A rule's condition set is a fresh node, not a content-addressed one, so
    // the structural closure did not expand it and "<set> ~ conjunction" was
    // left behind. The slice then reported the rule, printed it in full and
    // counted it -- and read the whole condition set as ONE condition, so
    // nothing ever matched. Adding a fact that the rule must extend is the
    // only way to tell the two apart from outside.
    collector.clear();
    interactive.process("Q30 P279 Q40");
    interactive.process("Q10 P279 X");
    CHECK(answers_contain(collector, "Q10 P279 Q40"));

    // ...and the derivation is reconstructible, rather than the fact being
    // called an axiom because no rule consequence could unify with it.
    collector.clear();
    interactive.process(".explain (Q10 P279 Q40)");
    CHECK_FALSE(any_output_contains(collector, "[axiom]"));
    CHECK(any_output_contains(collector, "Q20 P279 Q40"));

    fs::remove(file);
}

TEST_CASE("slice: every rule travels, including the ones that report contradictions")
{
    const auto file = slice_path("rulecount");

    std::string report;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
.lang wikidata
(A P279 X, A P279 Y, X != Y) => (A P9 yes)
(A P279 Q10, A P279 Q20) => !
Q30 P279 Q10
)");
        collector.clear();
        interactive.process(".save-predicates \"" + file.string() + "\" P279 P9");
        // The summary goes to the Diagnostic channel, like every other
        // progress line of a save.
        for (const auto& event : collector.events())
        {
            if (event.text.find("Saved ") != std::string::npos) report = event.text;
        }
    }

    CHECK(report.find("2 rule(s)") != std::string::npos);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    interactive.process(".auto-run");
    interactive.process(".lang wikidata");

    collector.clear();
    interactive.process(".stat");
    CHECK(any_output_contains(collector, "Rules: 2"));

    // The contradiction rule is the one that used to stay behind: its
    // consequence is the core "!" node, a fact of no predicate, so nothing
    // the closure retained ever reached it. A slice then stopped reporting a
    // contradiction its source reports -- silently, which is the part that
    // made it worth changing.
    collector.clear();
    interactive.process("Q30 P279 Q20");
    CHECK(has_contradiction(collector));

    fs::remove(file);
}

TEST_CASE("slice: a rule naming a predicate outside the slice stays readable")
{
    // Every rule travels (see the case above), and its patterns bring their
    // nodes along -- but the relation-type DECLARATION of a predicate that is
    // not one of the sliced ones stayed behind, and a fact structure is only
    // reconstructed for a declared predicate. So the rule arrived
    // structurally complete and unreadable:
    //
    //     .list-rules  ->  (a p b) => ??
    //
    // Typing "q ~ ->" into the loaded slice brought the line back in full,
    // which is what identified the missing fact. The promise attached to
    // carrying every rule -- "complete and simply never matches" -- needs it.
    const auto file = slice_path("foreign_predicate");

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
(a p b) => (c q d)
z p w
)");
        interactive.process(".save-predicates \"" + file.string() + "\" p");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");

    collector.clear();
    interactive.process(".list-rules");
    CHECK(any_output_contains(collector, "(a p b) => (c q d)"));
    CHECK_FALSE(any_output_contains(collector, "??"));

    // The consequence is still a rule PATTERN in the slice, not data: the
    // marking travels too, and the index is rebuilt on load.
    collector.clear();
    interactive.process("S q O");
    CHECK(collect_answers(collector).empty());

    // And the rule is not merely printable -- it fires.
    interactive.process(".auto-run");
    interactive.process("a p b");
    collector.clear();
    interactive.process("S q O");
    CHECK(answers_contain(collector, "c q d"));

    fs::remove(file);
}

TEST_CASE("slice: rejects a predicate the network does not know")
{
    const auto file = slice_path("unknown");

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    build_source(interactive);

    CHECK_THROWS(interactive.process(".save-predicates \"" + file.string() + "\" P9999"));
    CHECK_THROWS(interactive.process(".save-predicates \"" + file.string() + "\""));
    CHECK_THROWS(interactive.process(".save-predicates \"" + file.string() + ".txt\" P279"));

    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("wikidata-classes: culprits are ranked by how many they carry")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // Q30 and Q50 are the disjoint pair. Q10 is below both directly; Q60 and
    // Q70 are below Q10 and therefore culprits only through it -- exactly the
    // situation the ranking exists for: one edit at Q10 removes three
    // entries, one edit at Q40 removes one.
    process_lines(interactive, R"(
.lang wikidata
Q10 P279 Q20
Q20 P279 Q30
Q10 P279 Q50
Q60 P279 Q10
Q70 P279 Q60
Q40 P279 Q20
Q40 P279 Q50
)");
    interactive.process(".import wikidata-classes");
    collector.clear();
    interactive.process("%(culprits \"Q30\" \"Q50\")");

    CHECK(any_output_contains(collector, "3 Q10"));
    CHECK(any_output_contains(collector, "1 Q40"));
    CHECK(any_output_contains(collector, "-- 2 topmost culprit(s) of 4 affected class(es)"));

    // The ranking must put the consequential edit first.
    const std::string table = last_out_text(collector);
    const size_t      q10   = table.find("Q10");
    const size_t      q40   = table.find("Q40");
    REQUIRE(q10 != std::string::npos);
    REQUIRE(q40 != std::string::npos);
    CHECK(q10 < q40);
}

TEST_CASE("wikidata-classes: a pair that is not a disjoint pair is refused or flagged")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // Q30 and Q80 are unrelated to each other and Q10 sits below both, so
    // (Q30, Q80) is a real disjoint pair; Q30 and Q20 are nested.
    process_lines(interactive, R"(
.lang wikidata
Q10 P279 Q20
Q20 P279 Q30
Q10 P279 Q80
Q60 P279 Q10
)");
    interactive.process(".import wikidata-classes");

    // A class is not disjoint from itself, and the answer for (A, A) is not a
    // culprit report at all: every subclass of A is trivially below both. On
    // real data a mistyped second ID produced a plausible-looking work list of
    // tens of thousands of classes.
    CHECK_THROWS(interactive.process("%(culprits \"Q30\" \"Q30\")"));

    // Same shape, legal input: one class below the other. The numbers are
    // then a subclass count wearing the words of a culprit report, so the
    // report says so -- in either argument order.
    collector.clear();
    interactive.process("%(culprits \"Q30\" \"Q20\")");
    CHECK(any_output_contains(collector, "nested rather than disjoint"));

    collector.clear();
    interactive.process("%(culprits \"Q20\" \"Q30\")");
    CHECK(any_output_contains(collector, "nested rather than disjoint"));

    // A genuinely disjoint pair carries no such note.
    collector.clear();
    interactive.process("%(culprits \"Q30\" \"Q80\")");
    CHECK(any_output_contains(collector, "topmost culprit(s)"));
    CHECK_FALSE(any_output_contains(collector, "nested rather than disjoint"));

    CHECK_THROWS(interactive.process("%(culprits \"Q30\" \"Q80\" -1)"));
}

TEST_CASE("wikidata-classes: the path names the statements to look at")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    process_lines(interactive, R"(
.lang wikidata
Q10 P279 Q20
Q20 P279 Q30
Q10 P279 Q99
Q99 P279 Q98
Q98 P279 Q30
)");
    interactive.process(".import wikidata-classes");
    collector.clear();

    // Two chains lead from Q10 to Q30; the shorter one is reported, because
    // a two-step chain is what an editor can check by hand.
    interactive.process("%(culprit-path \"Q10\" \"Q30\")");
    CHECK(any_output_contains(collector, "P279 chain, 3 class(es)"));
    CHECK(any_output_contains(collector, "Q20"));
    CHECK_FALSE(any_output_contains(collector, "Q98"));

    collector.clear();
    interactive.process("%(culprit-path \"Q30\" \"Q10\")");
    CHECK(any_output_contains(collector, "No P279 chain"));
}

TEST_CASE("wikidata-classes: an ID that is in no P279 fact is reported as such")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    process_lines(interactive, R"(
.lang wikidata
Q10 P279 Q20
)");
    interactive.process(".import wikidata-classes");
    collector.clear();

    // A mistyped ID would otherwise resolve to a fresh node and answer
    // "no culprits", which reads like a clean result.
    CHECK_THROWS(interactive.process("%(culprits \"Q10\" \"Q4711\")"));
}
