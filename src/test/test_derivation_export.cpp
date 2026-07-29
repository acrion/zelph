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
