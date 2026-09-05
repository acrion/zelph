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
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// The scripts under dev_scripts/paper still run.
//
// This exists for one reason, and it is not that those scripts deserve tests.
// `dev_scripts/paper` is the only place in this repository where a `.zph` file
// names the standard library by name and NOTHING executes it. Everywhere else
// that reference is already load-bearing: nineteen of the twenty stdlib
// modules are imported by name from files in this directory, and the stdlib
// mirror beside the test binary makes those imports real, so a module that
// moves reds the suite within seconds.
//
// That asymmetry is the whole explanation for what happened in a562e51, which
// renamed `arithmetic.zph` to `decimal-arithmetic.zph`, swept every consumer it
// could see, and missed the one directory that had no consumer that runs. The
// scripts kept their `.import arithmetic` for six weeks, and the runs they
// produced in the meantime looked plausible: a failed import was not fatal,
// so the remaining statements executed and answered correctly -- those terms
// never needed the arithmetic module. The logs described a configuration
// nobody had asked for.
//
// So what is asserted is the invariant and nothing else. NOT the output: it
// carries timings, node identifiers and formatting, none of which says
// anything test_symbolic.cpp does not already pin against all three
// substrates, and asserting on it would break for reasons that are not
// defects. Interactive::process rethrows every failure it meets, so a
// try/catch around the lines IS the assertion, and it is stricter than the
// greps the scripts' own harness uses.
//
// Two deliberate choices:
//
//   - the directory is read at RUNTIME with a glob, so adding, renaming or
//     reordering a script requires no change here. The rot this test exists to
//     prevent would otherwise come back one level up, in a hand-kept list. The
//     pattern is the selection dev_scripts/paper/run_all.sh already makes, so
//     a scratch script named anything else stays out of the suite;
//
//   - every case here is `slow`, including the cheap one. Not as a timing
//     concession: these scripts are not installed, so a user verifying a
//     package has no business running them, and the packages run the suite
//     with --test-suite-exclude=slow. This check belongs to ctest and to a
//     developer's full run.
//
// It does mean the suite can be reddened by a session that is not working on
// the suite. That is the trade being bought, and the alternative has been
// measured: it cost a paper six weeks of evidence.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    // Every s<digit>...zph in the directory, in name order so a failure report
    // reads in the order the scripts are numbered.
    std::vector<fs::path> paper_scripts()
    {
        std::vector<fs::path> found;
        std::error_code       ec;
        for (const auto& entry : fs::directory_iterator(ZELPH_PAPER_SCRIPT_DIR, ec))
        {
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = entry.path().filename().string();
            if (name.size() > 2 && name[0] == 's' && std::isdigit(static_cast<unsigned char>(name[1]))
                && entry.path().extension() == ".zph")
                found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        return found;
    }
}

TEST_CASE("paper scripts: the directory the check reads is where it is expected" * doctest::test_suite("slow"))
{
    // A glob that silently matches nothing reads exactly like a glob over
    // scripts that all pass, which is the failure mode this whole file exists
    // to remove. So the count is asserted before anything is run.
    REQUIRE(fs::is_directory(ZELPH_PAPER_SCRIPT_DIR));
    CHECK(paper_scripts().size() >= 8);
}

TEST_CASE("paper scripts: every one of them runs without an error" * doctest::test_suite("slow"))
{
    for (const auto& script : paper_scripts())
    {
        CAPTURE(script.string());

        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());

        std::ifstream stream(script);
        REQUIRE(stream.good());

        std::string line;
        std::string failure;
        while (std::getline(stream, line))
        {
            try
            {
                interactive.process(line);
            }
            catch (const std::exception& ex)
            {
                failure = ex.what();
                break;
            }
        }

        if (failure.empty())
        {
            // A script may end inside an unfinished statement, a keyword block
            // or a Janet block, and that is reported here rather than by the
            // loop -- the same point at which the binary reports it.
            try
            {
                interactive.finish_input();
            }
            catch (const std::exception& ex)
            {
                failure = ex.what();
            }
        }

        CAPTURE(failure);
        CHECK(failure.empty());
    }
}
