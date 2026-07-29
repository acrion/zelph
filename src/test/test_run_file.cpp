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
#include <sstream>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// .run-file writes the deductions in a REVERSED, bracket-free form, and
// under .lang wikidata it compresses every Q/P identifier to a single CJK
// character -- the format's whole purpose (compact, LLM-tokenizer-friendly
// training data).
//
// Both properties depend on the console format the interception parses, and
// that format moved: a deduction is wrapped in "(...)" and its reason set in
// "{...}". The stripping only peeled ONE leading/trailing paren off the
// reasons, so brackets survived on both sides. For the wikidata case that
// is not cosmetic: "{Q2" and "Q3)" are not identifiers, so the compressor
// left them alone and the line came out HALF encoded -- unusable as
// training data and undetectable without looking at the file.
// ---------------------------------------------------------------------------

namespace
{
    std::string slurp(const std::filesystem::path& p)
    {
        std::ifstream     in(p);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

TEST_CASE("run-file: the written line carries no brackets and no markup")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_run_file_plain.txt";
        std::filesystem::remove(out);

        // Auto-run would derive the consequence before .run-file installs
        // its interception, leaving nothing for the file.
        interactive.process(".auto-run");
        interactive.process("(*{(A rel B) (B rel C)} ~ conjunction) => (A rel C)");
        interactive.process("a rel b");
        interactive.process("b rel c");
        interactive.process(".run-file " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const std::string text = slurp(out);
        std::filesystem::remove(out);

        // Reversed order: premises first, then the conclusion. The premises
        // are a SET, so their relative order is not pinned.
        REQUIRE(text.find(" ⇒ ") != std::string::npos);
        const std::string premises = text.substr(0, text.find(" ⇒ "));
        CHECK(premises.find("a rel b") != std::string::npos);
        CHECK(premises.find("b rel c") != std::string::npos);
        CHECK(text.substr(text.find(" ⇒ ")).find("a rel c") != std::string::npos);

        for (const char* delim : {"(", ")", "{", "}", "«", "»"})
            CHECK(text.find(delim) == std::string::npos); });
}

TEST_CASE("run-file: under .lang wikidata every identifier is encoded")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_run_file_wikidata.txt";
        std::filesystem::remove(out);

        interactive.process(".lang wikidata");
        interactive.process(".auto-run");
        interactive.process("Q1 P279 Q2");
        interactive.process("Q2 P279 Q3");
        interactive.process("(*{(A P279 B) (B P279 C)} ~ conjunction) => (A P279 C)");
        interactive.process(".run-file " + out.string());

        REQUIRE(std::filesystem::exists(out));
        const std::string text = slurp(out);
        std::filesystem::remove(out);

        REQUIRE_FALSE(text.empty());
        // The exact CJK character per identifier depends on first-encounter
        // order, so the test pins the invariant instead: NO Q/P identifier
        // survives in plain form.
        CHECK(text.find("Q1") == std::string::npos);
        CHECK(text.find("Q2") == std::string::npos);
        CHECK(text.find("Q3") == std::string::npos);
        CHECK(text.find("P279") == std::string::npos);
        CHECK(text.find("⇒") != std::string::npos); });
}
