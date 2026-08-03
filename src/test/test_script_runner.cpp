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
// zelph/run-script: running a Janet source file the way the janet CLI would.
//
// This is what the binary itself uses for `zelph <file.janet>`, and it is the
// counterpart of zelph/import, which deliberately rejects .janet files. The
// two properties below are the ones a caller cannot work out from the name and
// would otherwise have to discover by experiment:
//
//   - a relative (use ./x) inside the script resolves against the SCRIPT's
//     directory, not the process's working directory, so a program can be
//     started from anywhere;
//   - each run starts from an empty module cache, so a second run of the same
//     script sees an edited dependency. Janet's require caches modules
//     process-wide, which would otherwise freeze the first version loaded for
//     the lifetime of the session.
//
// Output is reported through zelph/out rather than Janet's print, because only
// the former reaches the collector.
// ---------------------------------------------------------------------------

namespace
{
    // A directory of its own per test, so the two cases cannot see each
    // other's dependency file.
    std::filesystem::path make_script_dir(const std::string& name)
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("zelph-run-script-" + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    void write_file(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream out(path);
        out << content;
    }

    std::string call(const std::filesystem::path& script)
    {
        return R"js(%(zelph/run-script ")js" + script.string() + R"js("))js";
    }
} // namespace

TEST_CASE("zelph/run-script: relative imports resolve against the script's directory")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto dir = make_script_dir("imports");
        write_file(dir / "lib.janet", R"js((defn tag [] "from-lib"))js");
        write_file(dir / "prog.janet",
                   R"js((use ./lib)
                        (zelph/out (string "toplevel=" (tag)))
                        (defn main [& args] (zelph/out (string "argc=" (length args)))))js");

        collector.clear();
        interactive.process(call(dir / "prog.janet"));

        // The top level ran and found the sibling module ...
        CHECK(any_output_contains(collector, "toplevel=from-lib"));
        // ... and main was called with the script path as its first argument,
        // exactly as the janet CLI does it.
        CHECK(any_output_contains(collector, "argc=1")); });
}

TEST_CASE("zelph/run-script: a second run sees an edited dependency")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto dir = make_script_dir("cache");
        write_file(dir / "lib.janet", R"js((defn tag [] "v1"))js");
        write_file(dir / "prog.janet",
                   R"js((use ./lib)
                        (zelph/out (string "tag=" (tag))))js");

        collector.clear();
        interactive.process(call(dir / "prog.janet"));
        CHECK(any_output_contains(collector, "tag=v1"));

        write_file(dir / "lib.janet", R"js((defn tag [] "v2"))js");

        collector.clear();
        interactive.process(call(dir / "prog.janet"));
        CHECK(any_output_contains(collector, "tag=v2"));
        CHECK_FALSE(any_output_contains(collector, "tag=v1")); });
}

// ---------------------------------------------------------------------------
// zelph/import from Janet: statement processing re-entered from inside the VM.
//
// This is the path that makes every janet_pcall in script_engine.cpp a NESTED
// call -- the VM is already running when the imported script's statements are
// parsed, its inline-keyword handlers are invoked and its rules are built. A
// fiber created for such a call is reachable from no GC root unless it is
// rooted by hand (see pcall_rooted), and a collection inside the call then
// frees the fiber that is running it.
//
// The symptom is not a failed assertion but a crash somewhere else entirely,
// so what these cases pin is that the path runs at all, and that both halves
// of it -- plain facts and RULE construction, which goes through
// zelph/dedup-rule and its scratch cluster -- survive it.
// ---------------------------------------------------------------------------

TEST_CASE("zelph/import: a .zph script imported from Janet builds facts and rules")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto dir = make_script_dir("import");
        write_file(dir / "rules.zph",
                   "(A ancestorof B) => (B descendantof A)\n"
                   "abraham ancestorof isaac\n");

        interactive.process(R"js(%(zelph/import ")js" + (dir / "rules.zph").string() + R"js("))js");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "IMP-FACT-" (zelph/exists "abraham" "ancestorof" "isaac")))js");
        interactive.process(R"js(%(string "IMP-DERIVED-" (zelph/exists "isaac" "descendantof" "abraham")))js");
        CHECK(any_output_contains(collector, "IMP-FACT-true"));
        CHECK(any_output_contains(collector, "IMP-DERIVED-true")); });
}

TEST_CASE("zelph/import: the same rule imported twice from Janet stays one rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto dir = make_script_dir("import-twin");
        // Two files, because the module guard would refuse the same one
        // twice. The second import therefore reaches the duplicate branch of
        // zelph/dedup-rule, which drops its scratch cluster again.
        write_file(dir / "first.zph", "(A ancestorof B) => (B descendantof A)\n");
        write_file(dir / "second.zph", "(X ancestorof Y) => (Y descendantof X)\n");

        interactive.process(R"js(%(zelph/import ")js" + (dir / "first.zph").string() + R"js("))js");
        interactive.process(R"js(%(zelph/import ")js" + (dir / "second.zph").string() + R"js("))js");
        interactive.process("abraham ancestorof isaac");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "TWIN-DERIVED-" (zelph/exists "isaac" "descendantof" "abraham")))js");
        CHECK(any_output_contains(collector, "TWIN-DERIVED-true")); });
}
