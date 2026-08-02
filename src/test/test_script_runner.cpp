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
