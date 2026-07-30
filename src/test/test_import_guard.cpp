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
// What .import may do, and what it says about it.
//
// .import used to be refused outright on a partial view, which made the
// layered query languages unreachable there: `.import sparql` after
// `.load-partial` failed, although the SPARQL layer only defines functions and
// registers a keyword. The refusal also protected nothing, because every
// operation that would be wrong on an incomplete graph is refused on its own
// and a typed statement was always allowed. It is permitted now, and a script
// that ADDS to the partial view says so afterwards -- the discriminator being
// the node count, i.e. what actually reached the graph.
//
// The second subject here is the module-ID claim. It is taken before the
// script runs (so that an import cycle terminates) and was never released,
// so a script that threw halfway could not be imported again at all.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    fs::path write_script(const std::string& name, const std::string& body)
    {
        const auto path = fs::temp_directory_path() / ("zelph_import_guard_" + name + ".zph");
        std::ofstream(path) << body;
        return path;
    }

    // A .bin to open as a partial view. Content is irrelevant beyond being
    // loadable; the partial mode is what the tests are after.
    fs::path write_network(const zelph::console::Interactive& interactive, const std::string& name)
    {
        const auto path = fs::temp_directory_path() / ("zelph_import_guard_" + name + ".bin");
        process_lines(interactive, R"(
.lang wikidata
Q10 P279 Q20
Q20 P279 Q30
)");
        interactive.process(".save \"" + path.string() + "\"");
        return path;
    }
}

TEST_CASE("import: a definition-only script imports into a partial view silently")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto network = write_network(interactive, "silent");
    interactive.process(".new");
    interactive.process(".load-partial \"" + network.string() + "\"");

    collector.clear();
    interactive.process(".import wikidata-classes");

    CHECK(any_event_contains(collector, "Wikidata class tools loaded"));
    CHECK_FALSE(any_event_contains(collector, "added"));

    // And it is usable, which is the reason the import has to be possible at
    // all: the class tools answer over the partially loaded graph.
    collector.clear();
    interactive.process("%(culprits \"Q30\" \"Q30\")");
    CHECK(any_output_contains(collector, "topmost culprit(s)"));

    fs::remove(network);
}

TEST_CASE("import: a script that adds to a partial view is reported")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto network = write_network(interactive, "writes");
    const auto script  = write_script("writes", R"(.provides import-guard-writes
.lang wikidata
Q77 P279 Q88
)");

    interactive.process(".new");
    interactive.process(".load-partial \"" + network.string() + "\"");

    collector.clear();
    interactive.process(".import \"" + script.string() + "\"");

    CHECK(any_event_contains(collector, "node(s) to a partial view"));
    CHECK(any_event_contains(collector, "Inference over them is blocked"));

    fs::remove(network);
    fs::remove(script);
}

TEST_CASE("import: adding to a complete network says nothing")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto script = write_script("normal", R"(.provides import-guard-normal
.lang wikidata
Q77 P279 Q88
)");

    collector.clear();
    interactive.process(".import \"" + script.string() + "\"");

    // The warning is about the partial view, not about writing.
    CHECK_FALSE(any_event_contains(collector, "partial view"));

    fs::remove(script);
}

TEST_CASE("import: a failed import can be repeated after the script is fixed")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    // .run-export into a directory that does not exist fails in the middle of
    // the script, after its first statement was applied.
    const auto script = write_script("broken", R"(.provides import-guard-broken
alpha ~ beta
.run-export /nonexistent-directory-for-zelph-test/x.txt
gamma ~ beta
)");

    CHECK_THROWS(interactive.process(".import \"" + script.string() + "\""));

    collector.clear();
    CHECK_THROWS(interactive.process(".import \"" + script.string() + "\""));
    // Reaching the same error again means the script ran again; the claim on
    // its module ID was released.
    CHECK_FALSE(any_event_contains(collector, "Skipping already imported"));

    // With the fault removed the import completes -- the state the claim used
    // to make unreachable without .new.
    std::ofstream(script) << R"(.provides import-guard-broken
alpha ~ beta
gamma ~ beta
)";
    collector.clear();
    interactive.process(".import \"" + script.string() + "\"");
    CHECK_FALSE(any_event_contains(collector, "Skipping already imported"));

    collector.clear();
    interactive.process("X ~ beta");
    CHECK(answers_contain(collector, "gamma ~ beta"));

    fs::remove(script);
}

TEST_CASE("import: a successful import is still imported only once")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto script = write_script("once", R"(.provides import-guard-once
alpha ~ beta
)");

    interactive.process(".import \"" + script.string() + "\"");
    collector.clear();
    interactive.process(".import \"" + script.string() + "\"");
    CHECK(any_event_contains(collector, "Skipping already imported"));

    fs::remove(script);
}
