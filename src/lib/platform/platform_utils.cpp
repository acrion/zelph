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
    #include <pthread.h>
    #include <sstream>
    #include <string>
    #if !defined(__GLIBC__)
        #include <sys/syscall.h>
        #include <unistd.h>
    #endif
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <pthread.h>
    #include <string>
    #include <sys/resource.h>
#elif defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

#if defined(__EMSCRIPTEN__)
    #include <emscripten/stack.h>
#elif defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace
{
    // The calling thread's stack as reported by the platform: the minimum
    // address to which it may expand downward, and the address from which it
    // expands downward. Both values are 0 when the platform reports no
    // information.
    struct StackBounds
    {
        std::uintptr_t low  = 0;
        std::uintptr_t high = 0;
    };

    StackBounds read_thread_stack()
    {
#if defined(__EMSCRIPTEN__)
        return {emscripten_stack_get_end(), emscripten_stack_get_base()};
#elif defined(__linux__)
    #if !defined(__GLIBC__)
        // musl's pthread_getattr_np reports the main thread's stack up to its
        // current mapped extent, rather than to its maximum possible expansion;
        // outside glibc, the main thread therefore reports nothing in this
        // context.
        if (getpid() == static_cast<pid_t>(syscall(SYS_gettid))) return {};
    #endif
        pthread_attr_t attributes;
        if (pthread_getattr_np(pthread_self(), &attributes) != 0) return {};
        void*       address = nullptr;
        std::size_t size    = 0;
        const int   result  = pthread_attr_getstack(&attributes, &address, &size);
        pthread_attr_destroy(&attributes);
        if (result != 0 || address == nullptr || size == 0) return {};
        const auto low = reinterpret_cast<std::uintptr_t>(address);
        return {low, low + size};
#elif defined(__APPLE__)
        const pthread_t      self = pthread_self();
        const std::uintptr_t high = reinterpret_cast<std::uintptr_t>(pthread_get_stackaddr_np(self));
        std::size_t          size = pthread_get_stacksize_np(self);
        // The kernel sizes the main thread's stack by RLIMIT_STACK; when
        // pthread_get_stacksize_np indicates a larger value, that limit
        // represents the size to trust.
        if (pthread_main_np() != 0)
        {
            rlimit limit{};
            if (getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY && static_cast<std::size_t>(limit.rlim_cur) < size)
                size = static_cast<std::size_t>(limit.rlim_cur);
        }
        if (high == 0 || size == 0 || size >= high) return {};
        return {high - size, high};
#elif defined(_WIN32)
    #if _WIN32_WINNT >= 0x0602
        ULONG_PTR low  = 0;
        ULONG_PTR high = 0;
        GetCurrentThreadStackLimits(&low, &high);
        return {static_cast<std::uintptr_t>(low), static_cast<std::uintptr_t>(high)};
    #else
        // GetCurrentThreadStackLimits needs the headers from Windows 8 or a
        // newer version (_WIN32_WINNT 0x0602); a build targeting an older
        // version, or an older MinGW-w64, yields no results.
        return {};
    #endif
#else
        return {};
#endif
    }
}

std::uintptr_t zelph::platform::stack_position()
{
#if defined(__EMSCRIPTEN__)
    return emscripten_stack_get_current();
#elif defined(_MSC_VER)
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
#else
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
}

std::uintptr_t zelph::platform::stack_floor(const std::size_t reserve, const std::size_t fallback)
{
    thread_local const StackBounds bounds = read_thread_stack();

    const std::uintptr_t here = stack_position();
    if (bounds.low != 0 && bounds.low < here && here <= bounds.high) return bounds.low + reserve;
    return here > fallback ? here - fallback : 0;
}

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

std::filesystem::path zelph::platform::path_from_utf8(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string zelph::platform::to_utf8(const std::u8string& text)
{
    return std::string(text.begin(), text.end());
}
