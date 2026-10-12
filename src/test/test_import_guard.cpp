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
#include "wikidata_dump.hpp"

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
Q10 P279 Q40
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
    // Q30 and Q40 are unrelated and Q10 sits below both, so this is a real
    // disjoint pair. It used to pass the same ID twice, which the class tools
    // now refuse -- a class is not disjoint from itself.
    interactive.process("%(culprits \"Q30\" \"Q40\")");
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

// A load that replaces the graph discards what a module wrote into it -- its
// rules and facts -- while the registry continued to identify the module as
// already imported, causing subsequent .import operations on it to be
// bypassed and the session to proceed without access to what the module
// defines; only .new restored access. The load clears the registry. Each
// subcase arrives at the load through a distinct path a session may follow.
TEST_CASE("import: a load that replaces the graph lets a module be imported again")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto network = write_network(interactive, "replaced");
    interactive.process(".new");

    const auto writer     = write_script("replaced_writer", R"(.provides import-guard-replaced-writer
alpha ~ beta
)");
    const auto janet_only = write_script("replaced_janet", R"(.provides import-guard-replaced-janet
%(def import-guard-replaced-janet true)
)");

    const auto imported_again = [&](const fs::path& script)
    {
        collector.clear();
        interactive.process(".import \"" + script.string() + "\"");
        return !any_event_contains(collector, "Skipping already imported");
    };

    SUBCASE("a network file")
    {
        interactive.process(".import \"" + writer.string() + "\"");
        interactive.process(".load \"" + network.string() + "\"");
        CHECK(imported_again(writer));

        collector.clear();
        interactive.process("X ~ beta");
        CHECK(answers_contain(collector, "alpha ~ beta"));
    }
    SUBCASE("zelph/load")
    {
        interactive.process(".import \"" + writer.string() + "\"");
        interactive.process("%(zelph/load \"" + network.generic_string() + "\")");
        CHECK(imported_again(writer));
    }
    SUBCASE("a partial load")
    {
        interactive.process(".import \"" + writer.string() + "\"");
        interactive.process(".load-partial \"" + network.string() + "\"");
        CHECK(imported_again(writer));
    }
    SUBCASE("a manifest")
    {
        // Only a header, thus the view holds no node whatsoever, and the
        // module defines nothing other than Janet code. The binPath is
        // written in the generic form: a native Windows path within a JSON
        // string does not qualify as one.
        const auto manifest = fs::temp_directory_path() / "zelph_import_guard_replaced.json";
        std::ofstream(manifest) << "{\"source\": {\"binPath\": \"" << network.generic_string() << "\"}, \"sections\": {"
                                << "\"left\": {\"chunks\": []}, \"right\": {\"chunks\": []}, "
                                << "\"nameOfNode\": {\"chunks\": []}, \"nodeOfName\": {\"chunks\": []}}}\n";

        interactive.process(".import \"" + janet_only.string() + "\"");
        interactive.process(".load-partial \"" + manifest.string() + "\" meta-only");
        CHECK(imported_again(janet_only));

        fs::remove(manifest);
    }
    SUBCASE("a dump's cache")
    {
        // A cache is read exclusively into an empty graph, which a module
        // that specifies nothing beyond Janet code leaves empty.
        using namespace wikidata_dump;
        const auto dump = write_lines("zelph_import_guard_dump", {entity("Q1", text("en", "one"))});
        interactive.process(".load \"" + dump.string() + "\"");
        interactive.process(".new");

        interactive.process(".import \"" + janet_only.string() + "\"");
        collector.clear();
        interactive.process(".load \"" + dump.string() + "\"");
        REQUIRE(any_event_contains(collector, "Cache loaded successfully"));
        CHECK(imported_again(janet_only));

        remove_dump(dump);
    }
    SUBCASE("a claim made with .provides")
    {
        // Typed at the prompt, .provides claims an identifier for no
        // module whatsoever, and a load takes the claim along with the
        // rest.
        interactive.process(".provides import-guard-replaced-writer");
        interactive.process(".load \"" + network.string() + "\"");
        collector.clear();
        interactive.process(".import \"" + writer.string() + "\"");
        CHECK_FALSE(any_event_contains(collector, "already provided"));
        CHECK_FALSE(interactive.had_failure());
    }
    SUBCASE("a module that loads a network itself stays imported")
    {
        // After the module's load, its lines are integrated into the loaded
        // graph, meaning everything the module performs is present; its
        // claim also terminates an import cycle that runs through it. Only
        // the modules that were imported earlier and have finished are
        // forgotten.
        const auto loader = write_script("replaced_loader", ".provides import-guard-replaced-loader\n.import \"" + writer.string() + "\"\n.load \"" + network.string() + "\"\nomega ~ beta\n");
        interactive.process(".import \"" + loader.string() + "\"");

        collector.clear();
        interactive.process(".import \"" + loader.string() + "\"");
        CHECK(any_event_contains(collector, "Skipping already imported"));
        CHECK(imported_again(writer));

        fs::remove(loader);
    }

    fs::remove(network);
    fs::remove(writer);
    fs::remove(janet_only);
}

// When a module is loaded, it registers additional elements beyond its
// rules -- a block keyword, an inline keyword, a display scheme along with
// its operators, a digit alphabet, and verbose self-fact predicates --
// which are re-registered upon a subsequent import after a load. No
// component of these may be denied during the second registration: display
// registrations, which name nodes by their id, are discarded with the
// load, and any keyword re-registered supersedes its prior handler.
TEST_CASE("import: a module imported again after a load registers what it registered before")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    const auto network = write_network(interactive, "registers");
    interactive.process(".new");

    SUBCASE("an arithmetic module: rules, a digit alphabet, verbose self-facts")
    {
        interactive.process(".import decimal-arithmetic");
        interactive.process(".load \"" + network.string() + "\"");
        CHECK_NOTHROW(interactive.process(".import decimal-arithmetic"));

        collector.clear();
        interactive.process("(&17 / &5) = X");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "((&17 / &5) = &3)"));
    }
    SUBCASE("math-syntax: a display scheme, its operators, an inline keyword")
    {
        interactive.process(".import math-syntax");
        interactive.process(".load \"" + network.string() + "\"");
        CHECK_NOTHROW(interactive.process(".import math-syntax"));

        collector.clear();
        interactive.process("$( 1 - x*x ) marker r1");
        CHECK(any_output_contains(collector, "$( &1 - x * x ) marker r1"));
    }
    SUBCASE("sparql: a block keyword")
    {
        interactive.process(".import sparql");
        interactive.process(".load \"" + network.string() + "\"");
        collector.clear();
        CHECK_NOTHROW(interactive.process(".import sparql"));
        CHECK(any_event_contains(collector, "SPARQL subset loaded"));

        collector.clear();
        interactive.process("sparql");
        interactive.process("SELECT ?x WHERE { wd:Q10 wdt:P279 ?x . }");
        interactive.process("");
        CHECK(any_output_contains(collector, "-- 2 result(s) --"));
    }

    fs::remove(network);
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
