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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// .run-export writes what a run DERIVED, in a form that says nothing about
// who is going to read it. The engine used to write MkDocs markdown with
// Wikidata URLs directly, which put a specific consumer -- two, in fact --
// inside an engine whose whole claim is that it knows no application domain.
//
// These tests pin the properties a converter depends on: one JSON object per
// derivation, premises kept apart rather than glued into a printed set,
// identifiers as node references with every name the node has, and zelph's
// own vocabulary marked as such so nobody looks "!" up in Wikidata.
// ---------------------------------------------------------------------------

namespace
{
    std::vector<std::string> lines_of(const std::filesystem::path& p)
    {
        std::vector<std::string> out;
        std::ifstream            in(p);
        std::string              line;
        while (std::getline(in, line))
            if (!line.empty()) out.push_back(line);
        return out;
    }

    bool any_contains(const std::vector<std::string>& lines, const std::string& needle)
    {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& l)
                           { return l.find(needle) != std::string::npos; });
    }
}

TEST_CASE("run-export: a deduction becomes one record with separated premises")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_export_deduction.jsonl";
        std::filesystem::remove(out);

        // Auto-run would derive the consequence before .run-export exists.
        interactive.process(".auto-run");
        interactive.process("(*{(A rel B) (B rel C)} ~ conjunction) => (A rel C)");
        interactive.process("a rel b");
        interactive.process("b rel c");
        interactive.process(".run-export " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const auto lines = lines_of(out);
        std::filesystem::remove(out);

        REQUIRE(lines.size() == 1);
        CHECK(lines[0].find("\"kind\":\"deduction\"") != std::string::npos);

        // The printed line shows the condition SET; the export shows its
        // elements, so a reader never has to take braces apart.
        CHECK(lines[0].find("\"premises\":[[") != std::string::npos);
        CHECK(lines[0].find("\"names\":{\"zelph\":\"rel\"}") != std::string::npos);
        CHECK(lines[0].find("\"names\":{\"zelph\":\"a\"}") != std::string::npos);
        CHECK(lines[0].find("\"names\":{\"zelph\":\"c\"}") != std::string::npos); });
}

TEST_CASE("run-export: the conclusions of a run are stable, and with .parallel off the whole file repeats")
{
    // Across the chain a..g, the fact (a before g) can be derived from
    // (a before c) and (c before g), or from (a before e) and (e before g),
    // and similarly for other intermediate steps, all in one run. Only the
    // first derivation of a given fact is written, and when .parallel is
    // active, the thread that reaches the conclusion first varies between
    // runs: the premises associated with a record differ, yet the set of
    // conclusions remains unchanged. The file rules.md asserts this
    // behaviour for every program without rules that are not stratifiable
    // and without a fresh variable, including this one, and promises that
    // with .parallel off, two runs write the same file -- which is what
    // this case pins.
    //
    // Only the conclusions are subjected to comparison under .parallel.
    // The derivation that prevails there depends on timing, so asserting
    // the premises DIFFER would effectively test the scheduler, and
    // would fail whenever it happens to repeat itself.
    auto export_chain = [](bool parallel, const std::string& file_name)
    {
        const std::filesystem::path out = std::filesystem::temp_directory_path() / file_name;
        std::filesystem::remove(out);
        {
            // Only one session at a time: launching a second live
            // Interactive within the same process results in it sharing
            // the script engine of the first.
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            if (!parallel) interactive.process(".parallel");
            interactive.process(".semi-naive check");
            interactive.process(".auto-run");
            interactive.process("(X before Y, Y before Z) => (X before Z)");
            process_lines(interactive, R"(
a before b
b before c
c before d
d before e
e before f
f before g
)");
            interactive.process(".run-export " + out.string());
        }
        REQUIRE(std::filesystem::exists(out));
        std::ifstream      in(out, std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        in.close();
        std::filesystem::remove(out);
        return content.str();
    };

    auto sorted_conclusions = [](const std::string& file)
    {
        std::vector<std::string> conclusions;
        std::istringstream       in(file);
        std::string              line;
        while (std::getline(in, line))
        {
            if (line.empty()) continue;
            const auto from = line.find("\"conclusion\":");
            const auto to   = line.find(",\"premises\":");
            REQUIRE(from != std::string::npos);
            REQUIRE(to != std::string::npos);
            conclusions.push_back(line.substr(from, to - from));
        }
        std::sort(conclusions.begin(), conclusions.end());
        return conclusions;
    };

    SUBCASE("parallel: the same conclusions, whatever premises they name")
    {
        const auto first  = sorted_conclusions(export_chain(true, "zelph_test_export_stable_par1.jsonl"));
        const auto second = sorted_conclusions(export_chain(true, "zelph_test_export_stable_par2.jsonl"));
        // A chain composed of seven elements contains 21 ordered pairs, six
        // of which are specified.
        CHECK(first.size() == 15);
        CHECK(first == second);
    }
    SUBCASE(".parallel off: byte-identical files")
    {
        const auto first  = export_chain(false, "zelph_test_export_stable_seq1.jsonl");
        const auto second = export_chain(false, "zelph_test_export_stable_seq2.jsonl");
        CHECK(sorted_conclusions(first).size() == 15);
        CHECK(first == second);
    }
}

TEST_CASE("run-export: an exported deduction is not one that was withheld")
{
    // .run-export runs with printing off, and everything not printed used to
    // be counted as withheld -- so a run that wrote every one of its
    // derivations to the file also reported "(skipped 1 deductions)" once per
    // derivation. Nothing had been skipped: the caller asked for a file, not
    // for lines, and the notice contradicted the file sitting next to it.
    //
    // What the count means is "derived, and you did not get to see it", and a
    // record in the export is seeing it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_export_not_withheld.jsonl";
        std::filesystem::remove(out);

        interactive.process(".auto-run");
        interactive.process("(*{(A rel B) (B rel C)} ~ conjunction) => (A rel C)");
        interactive.process("a rel b");
        interactive.process("b rel c");
        collector.clear();
        interactive.process(".run-export " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const auto lines = lines_of(out);
        std::filesystem::remove(out);

        REQUIRE(lines.size() == 1);
        CHECK_FALSE(any_event_contains(collector, "not shown"));
        CHECK_FALSE(any_event_contains(collector, "skipped")); });
}

TEST_CASE("run-export: a contradiction names zelph's own vocabulary as core")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_export_contradiction.jsonl";
        std::filesystem::remove(out);

        interactive.process(".auto-run");
        interactive.process("(X opp Y, A ~ X, A ~ Y, X != Y) => !");
        interactive.process("bright opp dark");
        interactive.process("yellow ~ bright");
        interactive.process("yellow ~ dark");
        interactive.process(".run-export " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const auto lines = lines_of(out);
        std::filesystem::remove(out);

        REQUIRE_FALSE(lines.empty());
        CHECK(any_contains(lines, "\"kind\":\"contradiction\""));
        // "!" is a node of the engine, not an entity of the data. A
        // converter that mistook it for one would link it somewhere.
        CHECK(any_contains(lines, "\"conclusion\":[{\"core\":\"!\"}]"));
        CHECK(any_contains(lines, "\"names\":{\"zelph\":\"yellow\"}")); });
}

TEST_CASE("run-export: a node reports every name it has")
{
    // What a report should DISPLAY and what it should LINK to are decisions
    // about a target format. The export therefore hands over all the names
    // and takes none of those decisions -- which is the whole reason the
    // Wikidata/MkDocs knowledge could leave the engine.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_export_names.jsonl";
        std::filesystem::remove(out);

        interactive.process(".lang wikidata");
        interactive.process(".auto-run");
        interactive.process("Q1 P279 Q2");
        interactive.process("Q2 P279 Q3");
        interactive.process(".lang en");
        interactive.process(".name Q1 wikidata Q1");
        interactive.process(".name Q1 en universe");
        interactive.process(".lang wikidata");
        interactive.process("(*{(A P279 B) (B P279 C)} ~ conjunction) => (A P279 C)");
        interactive.process(".run-export " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const auto lines = lines_of(out);
        std::filesystem::remove(out);

        REQUIRE_FALSE(lines.empty());
        CHECK(any_contains(lines, "\"wikidata\":\"Q1\""));
        CHECK(any_contains(lines, "\"en\":\"universe\"")); });
}

TEST_CASE("run-export: a name that would break the format is escaped, not lost")
{
    // The identifiers reach the file as JSON strings, so a name carrying
    // quotes, backslashes or the guillemets the renderer marks names with
    // cannot corrupt the record -- which an in-band text format could not
    // promise. French and German Wikidata labels really do contain « ».
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_export_escaping.jsonl";
        std::filesystem::remove(out);

        interactive.process(".auto-run");
        interactive.process("(X rel Y) => (X rel2 Y)");
        interactive.process("src rel dst");
        interactive.process(".name dst \"«Le Monde»\"");
        interactive.process(".run-export " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const auto lines = lines_of(out);
        std::filesystem::remove(out);

        REQUIRE_FALSE(lines.empty());
        CHECK(any_contains(lines, "«Le Monde»")); });
}

TEST_CASE("run-export: the predicate of a self-fact stays a node reference")
{
    // A self-fact renders as ":pred subject". The colon is sugar, the
    // predicate is a NAME, and the export needs the second as a node the
    // converter can look up. The renderer used to mark the two as one leaf,
    // which this file then had to take apart again by hand -- a heuristic
    // that could not tell that pair from a node genuinely named ":pred".
    // The renderer now emits the colon beside the marked name. This case
    // passed under the heuristic too -- it guards the property now that the
    // heuristic is gone, rather than proving the change.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_export_selffact.jsonl";
        std::filesystem::remove(out);

        interactive.process(".auto-run");
        interactive.process("(A parent_of B) => (B child_of A)");
        interactive.process("narc parent_of narc");
        interactive.process(".run-export " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const auto lines = lines_of(out);
        std::filesystem::remove(out);

        REQUIRE_FALSE(lines.empty());
        CHECK(any_contains(lines, "\"names\":{\"zelph\":\"child_of\"}"));
        CHECK(any_contains(lines, "\"names\":{\"zelph\":\"parent_of\"}"));
        // Not glued into the literal text around it.
        CHECK_FALSE(any_contains(lines, "\":child_of\"")); });
}

TEST_CASE("run-export: a bracket-shaped name stays a node reference")
{
    // The export splits the rendering at the markers the renderer puts
    // around leaf names; whatever is not marked can only become literal
    // text. A name that merely LOOKS structural -- ">" as a predicate, or a
    // label ending in ")" -- used to go unmarked and reached the file as
    // text, so a converter could neither link it nor show a different
    // language for it. Which name to show is the converter's decision, and
    // it needs the node to make it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_export_bracket_name.jsonl";
        std::filesystem::remove(out);

        interactive.process(".auto-run");
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        interactive.process("> is transitive");
        interactive.process("6 > 5");
        interactive.process("5 > 4");
        interactive.process(".name 4 \"Mercury (planet)\"");
        interactive.process(".run-export " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const auto lines = lines_of(out);
        std::filesystem::remove(out);

        REQUIRE_FALSE(lines.empty());
        CHECK(any_contains(lines, "\"names\":{\"zelph\":\">\"}"));
        CHECK(any_contains(lines, "\"names\":{\"zelph\":\"Mercury (planet)\"}"));
        // The forms the bug produced: the name glued into the surrounding
        // literal text instead of standing on its own.
        CHECK_FALSE(any_contains(lines, "\"(> "));
        CHECK_FALSE(any_contains(lines, " > ")); });
}

TEST_CASE("run-export: a refused deduction is marked as refused, a real contradiction is not")
{
    // deduce turns every refusal from fact() into a contradiction_error, so a
    // shape the engine cannot represent stops a rule exactly as a
    // contradiction in the data does -- and the console says which of the two
    // it was since the reason rides along. The export did not: a consumer
    // counting contradictions OF THE DATA, which is what the training-data
    // pipeline does, could not tell them apart.
    //
    // The field is optional, so a reader that does not know it is unaffected;
    // that is why the record keeps its kind.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path refused_out =
            std::filesystem::temp_directory_path() / "zelph_test_export_refused.jsonl";
        std::filesystem::remove(refused_out);

        interactive.process(".auto-run");
        interactive.process("z rel {a b}");
        interactive.process("q p r");
        interactive.process("(X p Y) => (X in {a b})");
        interactive.process(".run-export " + refused_out.string());

        REQUIRE(std::filesystem::exists(refused_out));
        const auto refused_lines = lines_of(refused_out);
        std::filesystem::remove(refused_out);

        REQUIRE_FALSE(refused_lines.empty());
        CHECK(any_contains(refused_lines, "\"kind\":\"contradiction\""));
        CHECK(any_contains(refused_lines, "\"refused\":\"a set constant cannot be extended"));
        CHECK(any_contains(refused_lines, "@{...}")); });

    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path genuine_out =
            std::filesystem::temp_directory_path() / "zelph_test_export_genuine.jsonl";
        std::filesystem::remove(genuine_out);

        interactive.process(".auto-run");
        interactive.process("(X opp Y, A ~ X, A ~ Y, X != Y) => !");
        interactive.process("bright opp dark");
        interactive.process("yellow ~ bright");
        interactive.process("yellow ~ dark");
        interactive.process(".run-export " + genuine_out.string());

        REQUIRE(std::filesystem::exists(genuine_out));
        const auto genuine_lines = lines_of(genuine_out);
        std::filesystem::remove(genuine_out);

        // A contradiction of the DATA carries no reason at all.
        CHECK(any_contains(genuine_lines, "\"kind\":\"contradiction\""));
        CHECK_FALSE(any_contains(genuine_lines, "\"refused\"")); });
}
