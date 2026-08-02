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
#include <string>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// .stat-file and .index-file off the happy path.
//
// Both are the entry point of the sharding pipeline, so what they say about a
// file is believed by everything downstream. They used to answer a file that
// is not a .bin, or is empty, with the raw kj exception -- a C++ source
// location and a hex stack trace -- and .stat-file accepted any header,
// including one declaring more chunks than the file has bytes.
//
// The division of labour between the two is deliberate and now documented:
// .stat-file reads the header only, which is what makes it instant on an
// 88 GB file and also means it cannot see a file truncated after the header.
// .index-file reads every chunk and is the one that notices.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    struct Fixtures
    {
        fs::path root;
        fs::path good;      // a saved two-fact network
        fs::path empty;     // a saved network with nothing but the core nodes
        fs::path truncated; // good, cut after the header
        fs::path tiny;      // good, cut inside the header's own claims
        fs::path garbage;   // not a .bin at all
        fs::path nothing;   // zero bytes
    };

    void copy_prefix(const fs::path& from, const fs::path& to, std::streamsize bytes)
    {
        std::ifstream     in(from, std::ios::binary);
        std::vector<char> buffer(static_cast<size_t>(bytes));
        in.read(buffer.data(), bytes);
        std::ofstream(to, std::ios::binary).write(buffer.data(), in.gcount());
    }

    Fixtures build()
    {
        Fixtures f;
        f.root = fs::temp_directory_path() / "zelph_bin_inspection";
        fs::remove_all(f.root);
        fs::create_directories(f.root);

        f.good      = f.root / "good.bin";
        f.empty     = f.root / "empty.bin";
        f.truncated = f.root / "truncated.bin";
        f.tiny      = f.root / "tiny.bin";
        f.garbage   = f.root / "garbage.bin";
        f.nothing   = f.root / "nothing.bin";

        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
a p b
c p d
)");
            interactive.process(".save \"" + f.good.string() + "\"");
            interactive.process(".new");
            interactive.process(".save \"" + f.empty.string() + "\"");
        }

        copy_prefix(f.good, f.truncated, 200);
        copy_prefix(f.good, f.tiny, 40);
        std::ofstream(f.garbage, std::ios::binary) << "hello, this is not a serialized network";
        std::ofstream nothing(f.nothing, std::ios::binary); // created empty on purpose
        nothing.close();

        return f;
    }
}

TEST_CASE("bin inspection: a file that is not a network is refused in words")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    for (const auto& bad : {f.garbage, f.nothing})
    {
        CHECK_THROWS_AS(interactive.process(".stat-file \"" + bad.string() + "\""), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".index-file \"" + bad.string() + "\" \""
                                            + (f.root / "out.json").string() + "\""),
                        std::runtime_error);

        // The message has to name the file and say what is wrong with it. The
        // kj detail may follow, the stack trace may not.
        try
        {
            interactive.process(".stat-file \"" + bad.string() + "\"");
            FAIL("expected .stat-file to refuse " << bad.string());
        }
        catch (const std::runtime_error& e)
        {
            const std::string message(e.what());
            CHECK(message.find(bad.filename().string()) != std::string::npos);
            CHECK(message.find("zelph .bin") != std::string::npos);
            CHECK(message.find("stack:") == std::string::npos);
        }
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a header claiming more than the file holds is refused")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // 40 bytes hold the header, whose four chunk counts cannot possibly be
    // backed by what is left. That is decidable without reading a chunk.
    try
    {
        interactive.process(".stat-file \"" + f.tiny.string() + "\"");
        FAIL("expected .stat-file to refuse a header that does not fit");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message(e.what());
        CHECK(message.find("chunks") != std::string::npos);
        CHECK(message.find("truncated") != std::string::npos);
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: the header report and the chunk walk divide the work")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // Happy path: four chunks, and the report says where the numbers come from.
    collector.clear();
    interactive.process(".stat-file \"" + f.good.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));
    CHECK(any_output_contains(collector, "declared by the header"));

    // A network with only the core nodes has no names to write, so two of the
    // four sections are empty -- a shape the pipeline has to accept.
    collector.clear();
    interactive.process(".stat-file \"" + f.empty.string() + "\"");
    CHECK(any_output_contains(collector, "Name-of-Node Chunks: 0"));
    CHECK(any_output_contains(collector, "Node-of-Name Chunks: 0"));

    // Truncated after the header: .stat-file reports what the header says,
    // because it never looks further. This is the documented division of
    // labour, not an oversight -- pinned so that a future change to either
    // command has to be deliberate.
    collector.clear();
    interactive.process(".stat-file \"" + f.truncated.string() + "\"");
    CHECK(any_output_contains(collector, "Total Chunks: 4"));

    // ...and .index-file, which reads every chunk, is the one that notices.
    CHECK_THROWS_AS(interactive.process(".index-file \"" + f.truncated.string() + "\" \""
                                        + (f.root / "out.json").string() + "\""),
                    std::runtime_error);

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a failed load says what happened to the network")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // A load merges into whatever is there and discards the previous state on
    // the way, so a file that stops in the middle leaves a graph that is
    // neither. The message used to be the kj backtrace and said nothing about
    // that; the way back is .new, and it now says so.
    try
    {
        interactive.process(".load \"" + f.truncated.string() + "\"");
        FAIL("expected .load to refuse a truncated file");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message(e.what());
        CHECK(message.find("zelph .bin") != std::string::npos);
        CHECK(message.find(".new") != std::string::npos);
        CHECK(message.find("stack:") == std::string::npos);
    }

    fs::remove_all(f.root);
}

TEST_CASE("bin inspection: a mistyped chunk selector leaves the session alone")
{
    const Fixtures f = build();

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // The selectors are checked before the graph is touched. They used to be
    // checked after it had been discarded, which left a network without even
    // its core nodes: every following statement failed with "requested left
    // node 1 does not exist", and nothing said that .new was the way out.
    CHECK_THROWS_AS(interactive.process(".load-partial \"" + f.good.string() + "\" left=99"),
                    std::runtime_error);

    collector.clear();
    interactive.process("a p b");
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    fs::remove_all(f.root);
}
