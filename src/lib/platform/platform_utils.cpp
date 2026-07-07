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

#include "platform_utils.hpp"

#include <cstdlib>
#include <cstring>

#ifdef __linux__
    #include <fstream>
    #include <sstream>
    #include <string>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <string>
#elif defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

size_t zelph::platform::get_process_memory_usage()
{
#ifdef __linux__
    std::ifstream status("/proc/self/status");
    if (!status.is_open())
    {
        return 0; // Fallback if file not accessible
    }
    std::string line;
    size_t      rss_kb  = 0;
    size_t      swap_kb = 0;
    while (std::getline(status, line))
    {
        if (line.compare(0, 6, "VmRSS:") == 0)
        {
            std::istringstream iss(line.substr(6));
            iss >> rss_kb;
        }
        else if (line.compare(0, 7, "VmSwap:") == 0)
        {
            std::istringstream iss(line.substr(7));
            iss >> swap_kb;
        }
    }
    return (rss_kb + swap_kb) * 1024; // Convert total kB to Bytes
#endif
    return 0; // Non-Linux
}

uint64_t zelph::platform::get_process_cpu_time_ns()
{
#ifdef __linux__
    timespec ts{};
    if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
    {
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull
             + static_cast<uint64_t>(ts.tv_nsec);
    }
#endif
    return 0;
}

std::filesystem::path zelph::platform::get_executable_path()
{
#if defined(__linux__)
    std::error_code             ec;
    const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : p;
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // query required buffer size
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::strlen(buffer.c_str()));

    // Resolve symlinks (e.g. Homebrew's bin/zelph -> libexec/zelph) so the
    // standard library is searched next to the real binary.
    std::error_code             ec;
    const std::filesystem::path canonical = std::filesystem::canonical(buffer, ec);
    return ec ? std::filesystem::path(buffer) : canonical;
#elif defined(_WIN32)
    wchar_t     buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buffer);
#else
    return {};
#endif
}

std::vector<std::filesystem::path> zelph::platform::get_standard_library_paths()
{
    std::vector<std::filesystem::path> paths;

    if (const char* env = std::getenv("ZELPH_STDLIB"); env && *env)
    {
        paths.emplace_back(env);
    }

    const std::filesystem::path exe = get_executable_path();
    if (!exe.empty())
    {
        const std::filesystem::path exe_dir = exe.parent_path();
        paths.push_back(exe_dir / "stdlib");
        paths.push_back(exe_dir.parent_path() / "share" / "zelph");
    }

#ifndef _WIN32
    paths.emplace_back("/usr/local/share/zelph");
    paths.emplace_back("/usr/share/zelph");
#endif

    return paths;
}
