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
// .export-wikidata <dump> <id...> pulls the exact JSON lines of the given
// entities out of a dump. Two things it has to get right and did not say
// anything about: that "Q4" does not also match "Q42" or "Q420", and that an
// ID which is simply not in the dump is reported. The latter left no trace at
// all -- one file fewer at the end of a scan that takes hours on a real dump.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    // The command writes into the CURRENT directory, so the test has to move
    // there and back, whatever happens.
    struct ScopedCwd
    {
        fs::path previous;
        explicit ScopedCwd(const fs::path& to)
            : previous(fs::current_path()) { fs::current_path(to); }
        ~ScopedCwd() { fs::current_path(previous); }
    };

    const char* kDump = R"json([
{"type":"item","id":"Q4","labels":{"en":{"language":"en","value":"death"}},"claims":{},"sitelinks":{}},
{"type":"item","id":"Q42","labels":{"en":{"language":"en","value":"Douglas Adams"}},"claims":{},"sitelinks":{}},
{"type":"item","id":"Q420","labels":{"en":{"language":"en","value":"medicine"}},"claims":{},"sitelinks":{}}
]
)json";

    std::string read_file(const fs::path& path)
    {
        std::ifstream     in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
}

TEST_CASE("wikidata export: exact ids, and a report for the ones that are missing")
{
    const auto dump = fs::temp_directory_path() / "zelph_export_test.json";
    {
        std::ofstream(dump, std::ios::binary) << kDump;
    }

    const auto out_dir = fs::temp_directory_path() / "zelph_export_out";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    {
        ScopedCwd cwd(out_dir);

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        interactive.process(".export-wikidata \"" + dump.string() + "\" Q4 Q9999 Q42");

        // Q4 is a prefix of both other ids in the dump and must not drag them in.
        REQUIRE(fs::exists(out_dir / "Q4.json"));
        CHECK(read_file(out_dir / "Q4.json").find("\"value\":\"death\"") != std::string::npos);
        CHECK(read_file(out_dir / "Q4.json").find("Douglas") == std::string::npos);

        REQUIRE(fs::exists(out_dir / "Q42.json"));
        CHECK(read_file(out_dir / "Q42.json").find("Douglas Adams") != std::string::npos);

        // Not requested, not written.
        CHECK_FALSE(fs::exists(out_dir / "Q420.json"));

        // An id that is not in the dump is named, not silently skipped.
        CHECK_FALSE(fs::exists(out_dir / "Q9999.json"));
        CHECK(any_event_contains(collector, "Not found"));
        CHECK(any_event_contains(collector, "Q9999"));
    }

    fs::remove_all(out_dir);
    fs::remove(dump);
}

TEST_CASE("wikidata export: a dump with CRLF line endings")
{
    // Four of the dump fixtures under dev_scripts are CRLF-terminated, so the
    // case is real. The carriage return must not survive into the extracted
    // line, which is supposed to be exactly what the dump held.
    std::string crlf(kDump);
    for (size_t pos = 0; (pos = crlf.find('\n', pos)) != std::string::npos; pos += 2)
    {
        crlf.replace(pos, 1, "\r\n");
    }

    const auto dump = fs::temp_directory_path() / "zelph_export_crlf_test.json";
    {
        std::ofstream(dump, std::ios::binary) << crlf;
    }

    const auto out_dir = fs::temp_directory_path() / "zelph_export_crlf_out";
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);

    {
        ScopedCwd cwd(out_dir);

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        interactive.process(".export-wikidata \"" + dump.string() + "\" Q42");

        REQUIRE(fs::exists(out_dir / "Q42.json"));
        const std::string text = read_file(out_dir / "Q42.json");
        CHECK(text.find("Douglas Adams") != std::string::npos);
        CHECK(text.find('\r') == std::string::npos);
    }

    fs::remove_all(out_dir);
    fs::remove(dump);
}
