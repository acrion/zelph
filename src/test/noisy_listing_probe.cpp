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

// Stands in for zelph_tests where what matters is not what it tests but what it
// writes. A Windows runner decoded the real output with the locale codec and
// choked on a byte the tests emit, which left the check with no output to read
// at all. Reaching that from a recorded file would prove nothing, because the
// decoding happens between two processes.

#include <cstdio>
#include <string_view>

namespace
{
    constexpr int case_count = 40;

    void emit(const char* text)
    {
        std::fputs(text, stdout);
    }
}

int main(int argc, char** argv)
{
    bool listing = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view(argv[i]) == "--list-test-cases")
        {
            listing = true;
        }
    }

    if (listing)
    {
        emit("[doctest] listing all test case names\n");
        emit("===============================================================================\n");
        for (int i = 0; i < case_count; ++i)
        {
            std::printf("case %d\n", i);
        }
        emit("===============================================================================\n");
        std::printf("[doctest] unskipped test cases passing the current filters: %d\n", case_count);
        return 0;
    }

    for (int i = 0; i < case_count; ++i)
    {
        std::printf("%.6f s: case %d\n", 0.05, i);
    }

    // The byte itself. It is invalid in UTF-8 and undefined in cp1252, so it
    // breaks a strict decoder on every platform rather than only on the one
    // where it was found.
    std::printf("%.6f s: a name carrying the byte \x81 that broke this\n", 0.05);
    return 0;
}
