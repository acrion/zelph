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
// This is present primarily for a single purpose, and it is not that those
// scripts merit testing.
// `dev_scripts/paper` stands as the sole location in this repository where a
// `.zph` file explicitly names the standard library, and NOTHING runs it. In
// every other instance, that reference is already load-bearing: nineteen out
// of the twenty standard library modules are imported by name from files
// within this directory, and the stdlib mirror adjacent to the test binary
// makes those imports real, so a module that moves reds the suite within
// seconds.
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
// What is asserted is the invariant, and among the output, only two phrases
// that remain unchanged regardless of timing, node identifier, or layout. The
// rest of the output reflects those, conveys no new information beyond what
// test_symbolic.cpp already pins across all three substrates, and asserting
// on it would result in failure due to causes that are not defects.
// Interactive::process rethrows every error it encounters, hence wrapping the
// lines in a try/catch IS the assertion of the invariant. The two phrases
// pertain to what the paper prints from these scripts: "no derivation found"
// must not occur within a proof tree, and "matches processed" must not appear
// in an evidence log. Each is clarified at the point where it is checked.
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

    // Each s<digit>...zph within the directory, arranged by name (thus s10
    // comes between s0 and s1). The directory_iterator visits entries in an
    // unspecified order; applying sorting makes the run order, and
    // consequently the order of a failure report, consistent across all
    // machines.
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
    // s0 through s8, along with the differentiation and term-island
    // instances from the paper's differentiation section, which lacked
    // individual scripts.
    CHECK(paper_scripts().size() >= 10);
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

        // The paper prints the proof trees generated by these scripts. A
        // fact marked "asserted; no derivation found" within one of them
        // constitutes a mistaken assertion in print, and s1's proof of Eq.
        // (5) contained such a wrong claim.
        CHECK_FALSE(any_output_contains(collector, "no derivation found"));

        // No evidence log carries a match count. A script runs as a session,
        // meaning inference has fully saturated the graph following each
        // line, and an explicit .run merely performs one additional pass over
        // the already saturated graph. Its count appears directly beneath the
        // derivation, where a reader might mistakenly interpret it as the
        // derivation's cost, which it is not; the counts the paper prints
        // originate from s9_timings.py. The report is directed to the
        // diagnostic channel, hence any_event_contains.
        CHECK_FALSE(any_event_contains(collector, "matches processed"));
    }
}
