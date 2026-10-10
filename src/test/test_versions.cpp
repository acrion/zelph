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
#include "versions.hpp"

#include <string>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// The third-party list that `zelph -v` and `.licenses` print.
//
// The mimalloc line was previously built using whichever <mimalloc.h> the
// compiler found. The library is never given the pinned copy -- only the app
// connects to the allocator -- so on a machine with a system mimalloc, the
// SYSTEM header was the one that spoke, and it did so with incorrect divisors:
// MI_MALLOC_VERSION 30405 corresponds to 3.4.5, yet it was displayed as
// 30.4.5. The artefact of a paper recorded that line as the allocator used
// during the run it documents.
//
// This executable has no connection to mimalloc whatsoever, thus the line
// must not be present here. The fact that the app names the version it is
// actually linked to is a property of the app, and the script
// tools/check_version_banner.py asks the app for this information.
// ---------------------------------------------------------------------------

TEST_CASE("versions: an allocator this process does not run on is not named")
{
    const std::string description = zelph::get_version_description();
    CHECK(description.find("Janet") != std::string::npos);
    CHECK(description.find("mimalloc") == std::string::npos);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".licenses");
    CHECK(any_output_contains(collector, "Janet"));
    CHECK_FALSE(any_output_contains(collector, "mimalloc"));
}
