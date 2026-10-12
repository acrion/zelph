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

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// The persisted predicate index (<file>.bin.pidx.<predicate>).
//
// A transitive closure switches from the direct traversal to a per-predicate
// index once its scan budget is exhausted, and the index is written next to
// the .bin so later sessions skip the extraction. That sidecar is the only
// piece of engine state that OUTLIVES the process, so a change in how the
// extraction reads a relation makes every file written by an older binary
// wrong -- and wrong silently, because the closure then answers from the
// cache. The header therefore carries a format version.
//
// The fixture needs a graph large enough to blow the closure's scan budget
// (2^16 relation entries on the start node), so it is one hub with 66000
// facts pointing at it -- the smallest shape that reaches the index at all.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    constexpr size_t kFacts = 66000; // > 2^16 entries on the hub node

    struct Fixture
    {
        fs::path root;
        fs::path script;
        fs::path bin;
    };

    // The setup contained within this file, along with `extra` positioned
    // after the hub facts, resides in its own directory to ensure that no
    // two fixtures ever utilize the same sidecar.
    Fixture build_fixture(const char* directory = "zelph_pidx_test", const char* extra = "")
    {
        Fixture f;
        f.root   = fs::temp_directory_path() / directory;
        f.script = f.root / "hub.zph";
        f.bin    = f.root / "hub.bin";

        fs::remove_all(f.root);
        fs::create_directories(f.root);

        {
            std::ofstream out(f.script);
            out << ".deductions off\n.auto-run\n"; // .auto-run is a toggle: off
            for (size_t i = 0; i < kFacts; ++i)
            {
                out << "a" << i << " p hub\n";
            }
            out << extra;
        }

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".import \"" + f.script.string() + "\"");
        interactive.process(".save \"" + f.bin.string() + "\"");

        REQUIRE(fs::exists(f.bin));
        return f;
    }

    // The sidecar of predicate `p`; its node id is not known to the test, so
    // the one file matching the prefix is taken.
    fs::path sidecar_of(const Fixture& f)
    {
        for (const auto& entry : fs::directory_iterator(f.root))
        {
            const std::string name = entry.path().filename().string();
            if (name.rfind("hub.bin.pidx.", 0) == 0) return entry.path();
        }
        return {};
    }

    // Load the fixture and run the closure that reaches the index.
    void load_and_close(const Fixture& f, zelph::console::Interactive& interactive)
    {
        interactive.process(".load \"" + f.bin.string() + "\"");
        interactive.process(R"(%(string "CLOSURE-" (length (zelph/closure-sources "hub" "p"))))");
    }

    void patch_version(const fs::path& sidecar, const uint32_t version)
    {
        std::fstream file(sidecar, std::ios::in | std::ios::out | std::ios::binary);
        REQUIRE(file.good());
        file.seekp(4, std::ios::beg); // right after the "ZPIX" magic
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    }
}

TEST_CASE("predicate index: a sidecar written by an older format is rebuilt, not trusted")
{
    const Fixture f = build_fixture();

    // First closure: builds the index and writes the sidecar.
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        load_and_close(f, interactive);
        CHECK(any_event_contains(collector, "Building adjacency index"));
        CHECK(any_event_contains(collector, "Saved adjacency index"));
        CHECK(any_output_contains(collector, "CLOSURE-" + std::to_string(kFacts)));
    }

    const fs::path sidecar = sidecar_of(f);
    REQUIRE(!sidecar.empty());

    // Second closure: the sidecar answers, no extraction happens.
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        load_and_close(f, interactive);
        CHECK(any_event_contains(collector, "Loaded adjacency index"));
        CHECK_FALSE(any_event_contains(collector, "Building adjacency index"));
        CHECK(any_output_contains(collector, "CLOSURE-" + std::to_string(kFacts)));
    }

    // A sidecar of the PREVIOUS format holds edges read off the adjacency:
    // one per fact that has an indexed fact as its subject, and none for a
    // self-fact. It must not be believed, however well its node counts match.
    patch_version(sidecar, 1);
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        load_and_close(f, interactive);
        CHECK_FALSE(any_event_contains(collector, "Loaded adjacency index"));
        CHECK(any_event_contains(collector, "Building adjacency index"));
        CHECK(any_output_contains(collector, "CLOSURE-" + std::to_string(kFacts)));
    }

    fs::remove_all(f.root);
}

// A self-fact represents a single-step loop, and the one-or-more closure of
// its node contains the node itself. A closure is answered in one of two ways
// -- via a direct walk beneath the scan budget, or through an index above it
// -- with each way reading a relation independently. The index read the loop,
// whereas the walk did not, meaning that within the same graph and over the
// same predicate, `hub p hub` made hub part of its own closure, while `k p k`
// left k out of its. Asked of both ways, during the session that builds the
// sidecar and during the one that loads it.
TEST_CASE("predicate index: a loop of one step is a path, through the index as through the direct walk")
{
    const Fixture f = build_fixture("zelph_pidx_loop_test", "hub p hub\nk p k\n");

    for (const char* sidecar : {"Building adjacency index", "Loaded adjacency index"})
    {
        CAPTURE(sidecar);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load \"" + f.bin.string() + "\"");

        // hub carries additional facts beyond what the scan budget allows:
        // the index.
        interactive.process(R"(%(string "HUB-" (length (zelph/closure "hub" "p"))))");
        interactive.process(R"(%(string "HUB-SOURCES-" (length (zelph/closure-sources "hub" "p"))))");
        CHECK(any_event_contains(collector, sidecar));
        CHECK(any_output_contains(collector, "HUB-1"));
        CHECK(any_output_contains(collector, "HUB-SOURCES-" + std::to_string(kFacts + 1)));

        // k holds a single fact: the direct walk.
        interactive.process(R"(%(string "K-" (length (zelph/closure "k" "p"))))");
        interactive.process(R"(%(string "K-SOURCES-" (length (zelph/closure-sources "k" "p"))))");
        CHECK(any_output_contains(collector, "K-1"));
        CHECK(any_output_contains(collector, "K-SOURCES-1"));
    }

    fs::remove_all(f.root);
}
