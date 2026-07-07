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

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace zelph::platform
{
    size_t   get_process_memory_usage();
    uint64_t get_process_cpu_time_ns();

    // Absolute, symlink-resolved path of the running executable.
    // Returns an empty path if it cannot be determined.
    std::filesystem::path get_executable_path();

    // Candidate directories for the zelph standard library, in search order:
    //   1. $ZELPH_STDLIB (explicit override, e.g. for development)
    //   2. <exe dir>/stdlib          (release archives, Homebrew libexec,
    //                                 Chocolatey tools dir, local build tree)
    //   3. <exe dir>/../share/zelph  (FHS installs: /usr/bin + /usr/share/zelph)
    //   4. /usr/local/share/zelph and /usr/share/zelph (non-Windows fallbacks)
    // Existence is NOT checked here; callers probe the entries in order.
    std::vector<std::filesystem::path> get_standard_library_paths();
}
