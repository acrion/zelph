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

#include "thread_count.hpp"

#include <cstdlib>

#ifndef __EMSCRIPTEN__
    #include <fstream>
    #include <iterator>
    #include <thread>
#endif

#if defined(__linux__)
    #include <sched.h>
#elif defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <bit>
#endif

namespace
{
#ifndef __EMSCRIPTEN__
    // The CPUs permitted by the process's affinity mask, or 0 when the
    // platform reports no mask: macOS provides none for a process, and Linux
    // refuses a mask wider than the set requested here.
    unsigned affinity_cpus()
    {
    #if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) != 0) return 0;
        return static_cast<unsigned>(CPU_COUNT(&set));
    #elif defined(_WIN32)
        DWORD_PTR of_process = 0;
        DWORD_PTR of_system  = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &of_process, &of_system)) return 0;
        return static_cast<unsigned>(std::popcount(static_cast<std::uint64_t>(of_process)));
    #else
        return 0;
    #endif
    }

    std::optional<std::string> read_file(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (in.bad()) return std::nullopt;
        return content;
    }
#endif
}

namespace zelph::concurrency
{
    const ProcessThreads& process_threads()
    {
        // Read once: the banner of each run names the count, and a session
        // that never launches any parallel activity should not incur file
        // costs for its cgroups more than once.
        static const ProcessThreads of_process = []
        {
            ProcessThreads process;
#ifndef __EMSCRIPTEN__
            if (const char* const named = std::getenv("ZELPH_THREADS"); named != nullptr && *named != '\0' && std::string_view(named) != "auto")
            {
                if (const auto count = thread_count::named_count(named))
                {
                    process.count            = *count;
                    process.from_environment = true;
                    return process;
                }
                process.unusable = named;
            }
            process.count = thread_count::automatic(affinity_cpus(), std::thread::hardware_concurrency(), thread_count::cgroup_cpus(read_file));
#endif
            return process;
        }();
        return of_process;
    }
}
