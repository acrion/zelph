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
// A script named on the command line is a SESSION, not a library load.
//
// `zelph script.zph` used to run the file through the same path as `.import`,
// which suppresses three things a session has: the input echo, the collection
// of focus anchors, and auto-run after each line. Each of those cost the file
// form something its piped twin `zelph < script.zph` had, and none of it was
// visible from the outside:
//
//   - no echo, and the echo is what records the node a bare `.node`,
//     `.mermaid` or `.explain` falls back to. So the file form answered
//     "No argument given and no previous output node available" -- while the
//     shebang `#!/usr/bin/env zelph`, which every zelph script carries,
//     points at exactly that form;
//   - no focus anchors, so the default deduction filter had nothing to keep
//     and removed EVERY deduction. `zelph script.zph > log` wrote an EMPTY
//     file for a script whose piped twin printed its whole derivation, and
//     the notice saying so went to stderr;
//   - auto-run deferred to the end of the file, so a script could not read a
//     result it had just derived.
//
// What a session keeps from a module is everything about finding and
// preparing the file: standard-library resolution, the `.janet` runner,
// script arguments, the import-once module guard. See ScriptRole in
// src/lib/repl_state.hpp.
//
// The second subject here is what a LINE is. zelph decides that from the
// line's first character, and the scan for it used to know only space and
// tab -- so a file saved with a UTF-8 byte order mark lost its first line
// without one word of complaint.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    // Each case writes its own file, so two of them cannot read each other's
    // leftovers, and the stem is what the module guard registers -- a name
    // shared with a standard-library module would make the second import of
    // that module a silent no-op.
    fs::path write_script(const std::string& name, const std::string& body)
    {
        const fs::path path = fs::temp_directory_path() / ("zelph_session_" + name);
        std::ofstream(path, std::ios::binary) << body;
        return path;
    }
}

TEST_CASE("session script: a file named on the command line echoes its statements")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const fs::path script = write_script("echo.zph", "a rel b\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        // The echo is the observable half; the half that matters is that it
        // is also what records the last node (see the next case).
        CHECK(any_output_contains(collector, "a rel b")); });
}

TEST_CASE("session script: a bare .node names the node of the statement before it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The exact shape that failed: `zelph s7_figure1.zph` reported "No
        // argument given and no previous output node available" while
        // `zelph < s7_figure1.zph` drew the figure, and the difference was
        // the suppressed echo. The paper harness compensated by refusing to
        // use the file form at all.
        const fs::path script = write_script("lastnode.zph", "x rel y\n.node\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_output_contains(collector, "Representation: x rel y"));
        CHECK_FALSE(any_event_contains(collector, "no previous output node")); });
}

TEST_CASE("session script: an .import inside it still hides the module's own lines")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The session is only the file's OWN lines. A module it imports is
        // still a library load -- otherwise importing the standard library
        // from a script would print the whole standard library.
        const fs::path module = write_script("quiet_module.zph", "hidden rel thing\n");
        const fs::path script = write_script("importer.zph",
                                             "visible rel thing\n.import " + module.string() + "\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);
        fs::remove(module);

        CHECK(any_output_contains(collector, "visible rel thing"));
        CHECK_FALSE(any_output_contains(collector, "hidden rel thing")); });
}

TEST_CASE("session script: its statements anchor the deduction focus")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // With input capture suppressed the default filter had no anchor at
        // all and dropped every deduction, so the file form wrote nothing to
        // stdout where the piped form printed the derivation. Both forms are
        // compared here against each other rather than against a literal, so
        // the case says what it is actually about: the two are the same run.
        interactive.process(".deductions focus");
        const fs::path script = write_script("focus.zph",
                                             "u p1 v\n(A p1 B) => (A p2 B)\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_deduction_of(collector, "u p2 v")); });
}

TEST_CASE("session script: auto-run fires per line, so a later line sees the consequence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A module runs once when it has been read in full, which is right
        // for a library. In a session the run belongs after each line, and
        // the third line below can only see the consequence if the second
        // one has already fired. Asked through zelph/exists rather than
        // through a query, because asking IS asserting for a query and the
        // question would then answer itself.
        const fs::path script = write_script(
            "autorun.zph",
            "m p1 n\n"
            "(A p1 B) => (A p2 B)\n"
            R"js(%(zelph/out (string "DERIVED-" (zelph/exists "m" "p2" "n"))))js"
            "\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_output_contains(collector, "DERIVED-true")); });
}

TEST_CASE("session script: .quit ends the file where it stands")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // `.quit` was a silent no-op in a file: the command table registers
        // it as an empty handler because the REPL loop intercepts the line
        // itself, and no other loop was looking. A script that ended with
        // `.quit` -- which is how a REPL transcript ends -- therefore kept
        // running past it.
        const fs::path script = write_script("quit.zph", "before rel it\n.quit\nafter rel it\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_output_contains(collector, "before rel it"));
        CHECK_FALSE(any_event_contains(collector, "after rel it")); });
}

TEST_CASE("session script: an unknown extension is run, an imported one is refused")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A `#!/usr/bin/env zelph` script is conventionally called `report`
        // or `check-facts`, not `check-facts.zph`, and every other
        // interpreter runs the file it is pointed at whatever it is named.
        // `.import` keeps the rule, because there the name IS the interface:
        // it is resolved against the standard library, and a ".json" behind
        // it is a mistake worth refusing before the file is fed to the parser
        // line by line.
        const fs::path odd = write_script("named.tool", "tool rel ran\n");
        collector.clear();
        interactive.process_file(odd.string());
        CHECK(any_output_contains(collector, "tool rel ran"));

        collector.clear();
        CHECK_THROWS(interactive.process(".import " + odd.string()));
        fs::remove(odd); });
}

TEST_CASE("session script: a failed import is recorded as a failure of the session")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // The exit status the binary reports is this flag. It used to be
        // absent altogether: whatever happened, zelph left with 0, so a
        // script whose `.import` did not resolve reported success -- and
        // because the statements after it still answered (they never needed
        // the missing module), the log looked entirely plausible while
        // describing a configuration nobody had asked for.
        CHECK_FALSE(interactive.had_failure());

        const fs::path script = write_script("badimport.zph", ".import no-such-module-here\n");
        CHECK_THROWS(interactive.process_file(script.string()));
        fs::remove(script);

        // The library records only what it can see itself; the throw above is
        // what the caller reports and turns into a failure. Both halves are
        // needed, so both are pinned.
        interactive.note_failure();
        CHECK(interactive.had_failure());

        // .new resets the graph, not what the session has already got wrong.
        interactive.process(".new");
        CHECK(interactive.had_failure()); });
}

TEST_CASE("session script: a declined import is a failure without an exception")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // The one path that fails the user's request and throws NOTHING: two
        // scripts claim the same module ID, the second is declined, and the
        // session that follows is not the one that was asked for. An exit
        // status derived from exceptions alone would report this as success.
        const fs::path first  = write_script("provider_a.zph", ".provides sharedid\na rel one\n");
        const fs::path second = write_script("provider_b.zph", ".provides sharedid\nb rel two\n");

        interactive.process(".import " + first.string());
        CHECK_FALSE(interactive.had_failure());

        interactive.process(".import " + second.string());
        CHECK(interactive.had_failure());

        fs::remove(first);
        fs::remove(second); });
}

// --- What a line IS, when something invisible stands in front of it --------

TEST_CASE("invisible prefixes: a byte order mark does not turn a command into a statement")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A .zph file saved with a BOM used to lose its first line in
        // silence. The three bytes made the line miss both the '#' test and
        // the '.' test, so `.import x` was read as the START of a statement,
        // stayed incomplete, and the NEXT line was appended to it: two lines
        // of the script became one fact, with a node named "\xEF\xBB\xBF.import"
        // in the graph and not one error message anywhere.
        const fs::path script = write_script("bom.zph", "\xEF\xBB\xBF" "p rel q\nr rel s\n");
        collector.clear();
        interactive.process_file(script.string());
        fs::remove(script);

        CHECK(any_output_contains(collector, "p rel q"));
        CHECK(any_output_contains(collector, "r rel s"));
        // The tell that the two lines had been glued together.
        CHECK_FALSE(any_output_contains(collector, "p rel q r rel s")); });
}

TEST_CASE("invisible prefixes: a no-break space in front of a command still runs the command")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Same family, and the one a copy out of a PDF or an editor's
        // auto-indent produces. U+00A0 is whitespace to every reader and was
        // whitespace to nothing here.
        collector.clear();
        interactive.process("\xC2\xA0.lang wikidata");
        CHECK_FALSE(any_event_contains(collector, "Could not parse statement"));
        CHECK(interactive.get_lang() == "wikidata");
        interactive.process(".lang zelph"); });
}

TEST_CASE("invisible prefixes: an indented comment is a comment")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The comment test used to demand column 0, so an indented '#' line
        // -- which is how a comment inside a block is written -- was parsed
        // as a fact whose subject is named "#". Nothing complained; the graph
        // simply grew a statement out of a remark.
        collector.clear();
        interactive.process("    # this is a remark, not a fact");
        CHECK_FALSE(any_output_contains(collector, "remark"));
        CHECK_FALSE(any_event_contains(collector, "Could not parse statement")); });
}
