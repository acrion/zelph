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

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// The number of threads an engine's parallel work may utilize: the count a
// process initializes with, and the texts from which that count is read.
// Separated from the code that reads them from the platform
// (thread_count.cpp), just as node_map_sizing.hpp is separated from
// network.hpp, enabling the policy to be tested in isolation.
namespace zelph::concurrency::thread_count
{
    // The maximum value that `.threads`, ZELPH_THREADS, and the bindings
    // take. A count exceeding the number of CPUs on the machine is
    // permitted, as a test running the parallel path on a machine with two
    // cores needs one.
    inline constexpr unsigned max_threads = 1024;

    // A relation containing fewer facts than this undergoes scanning on the
    // evaluating thread regardless of the count. The value originates from a
    // series spanning relations with 512 to 262,144 facts, using 2 to 16
    // threads. A scan where all candidates are present in the cache of fact
    // structures and all match, so that one thread hands on as many matches as
    // the relation holds facts, takes 1.3 to 1.8 times as long across threads
    // as on a single thread at 1,024 facts, as long at 4,096, and 0.8 to 0.9
    // times as long at 65,536. A scan in which no candidate matches gains at
    // every size, up to a factor of 2.4. The first scan of a relation, where
    // candidates are absent from the cache, is slower across threads at every
    // size, since each miss takes the cache's lock exclusively, and no number
    // of facts tells such a scan from others.
    inline constexpr std::size_t parallel_scan_min_facts = 4096;

    // The number that `text` represents when it contains only decimal
    // digits and nothing else.
    inline std::optional<std::uint64_t> whole_number(const std::string_view text)
    {
        std::uint64_t value     = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
        return value;
    }

    inline std::string_view first_line(const std::string_view text)
    {
        return text.substr(0, text.find('\n'));
    }

    // The CPUs granted by the content of a `cpu.max` file in cgroup v2:
    // "<quota> <period>", both measured in microseconds, grants quota / period
    // of them, rounded down, and "max <period>" sets no restriction, which
    // equates to no value in this context. A second thread operating on half a
    // CPU would face throttling, and a throttled thread that holds a lock
    // stalls every other one.
    inline std::optional<std::uint64_t> cpus_of_cpu_max(const std::string_view content)
    {
        const std::string_view line  = first_line(content);
        const std::size_t      space = line.find(' ');
        if (space == std::string_view::npos) return std::nullopt;
        const auto quota  = whole_number(line.substr(0, space));
        const auto period = whole_number(line.substr(space + 1));
        if (!quota || !period || *period == 0) return std::nullopt;
        return *quota / *period;
    }

    // The same applies to the two files of cgroup v1, `cpu.cfs_quota_us` and
    // `cpu.cfs_period_us`: setting a quota to -1 imposes no limit.
    inline std::optional<std::uint64_t> cpus_of_cfs(const std::string_view quota_content, const std::string_view period_content)
    {
        const auto quota  = whole_number(first_line(quota_content));
        const auto period = whole_number(first_line(period_content));
        if (!quota || !period || *period == 0) return std::nullopt;
        return *quota / *period;
    }

    // The fields of a line, separated by `separator`: the next one is
    // removed from the front of `rest`.
    inline std::string_view next_field(std::string_view& rest, const char separator)
    {
        const std::size_t      at    = rest.find(separator);
        const std::string_view field = rest.substr(0, at);
        rest                         = at == std::string_view::npos ? std::string_view{} : rest.substr(at + 1);
        return field;
    }

    inline bool lists(std::string_view options, const std::string_view option)
    {
        while (!options.empty())
            if (next_field(options, ',') == option) return true;
        return false;
    }

    // The directory within the file system representing a cgroup hierarchy,
    // and the group that directory represents, as specified in the text of
    // /proc/self/mountinfo: for the unified hierarchy of cgroup v2 where
    // `controller` is empty, and for the hierarchy of cgroup v1 that holds
    // `controller` otherwise.
    struct Hierarchy
    {
        std::string directory;
        std::string root;
    };

    // A path as written by /proc/self/mountinfo, with kernel-escaped
    // characters restored: a space, a tab, a newline, and a backslash stand
    // in that file as a backslash followed by three octal digits (`\040`,
    // `\011`, `\012`, `\134`), whereas /proc/self/cgroup writes the same
    // group exactly as it is. A group whose name holds a backslash, such as
    // the unit names that systemd escapes (`machine-my\x2dbox.scope`), is
    // otherwise not identified as the root of its own mount.
    inline std::string unescaped(const std::string_view field)
    {
        const auto octal = [&field](const std::size_t at, const char last)
        {
            return at < field.size() && field[at] >= '0' && field[at] <= last;
        };
        std::string path;
        path.reserve(field.size());
        for (std::size_t i = 0; i < field.size(); ++i)
        {
            if (field[i] == '\\' && octal(i + 1, '3') && octal(i + 2, '7') && octal(i + 3, '7'))
            {
                path.push_back(static_cast<char>((field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0')));
                i += 3;
            }
            else
                path.push_back(field[i]);
        }
        return path;
    }

    // Whether the mount of the group `root` shows `group`: the group itself,
    // or a group located beneath it, whose path extends past `root` via a
    // `/`.
    inline bool shows(const std::string_view root, const std::string_view group)
    {
        if (root == "/") return true;
        return group.substr(0, root.size()) == root && (group.size() == root.size() || group[root.size()] == '/');
    }

    // The first mount of the hierarchy that shows `group`. A hierarchy may be
    // mounted multiple times, and a mount of another group, such as a bind
    // mount listed before the one at /sys/fs/cgroup, holds the limits of that
    // group: read as the process's, an unrelated quota lowered the count.
    inline std::optional<Hierarchy> hierarchy_of(const std::string_view mountinfo, const std::string_view controller, const std::string_view group)
    {
        std::string_view rest = mountinfo;
        while (!rest.empty())
        {
            std::string_view line = next_field(rest, '\n');
            // "<id> <parent> <device> <root> <mount point> <options> ... - <type> <source> <super options>"
            const std::size_t dash = line.find(" - ");
            if (dash == std::string_view::npos) continue;
            std::string_view after = line.substr(dash + 3);
            std::string_view type  = next_field(after, ' ');
            next_field(after, ' ');
            const std::string_view super_options = after;
            if (controller.empty() ? type != "cgroup2" : (type != "cgroup" || !lists(super_options, controller))) continue;

            next_field(line, ' ');
            next_field(line, ' ');
            next_field(line, ' ');
            const std::string root      = unescaped(next_field(line, ' '));
            const std::string directory = unescaped(next_field(line, ' '));
            if (root.empty() || directory.empty() || !shows(root, group)) continue;
            return Hierarchy{directory, root};
        }
        return std::nullopt;
    }

    // The CPUs allocated to the process via its cgroups: the minimal quota
    // found within its group and all ancestor groups, in the unified hierarchy
    // of cgroup v2 and in the `cpu` hierarchy of cgroup v1, and no value when
    // none of them sets a limit or the platform lacks cgroups. `read` returns
    // the content of a file, or no value for a file that cannot be read:
    // std::optional<std::string>(const std::string& path).
    template <typename Read>
    std::optional<std::uint64_t> cgroup_cpus(const Read& read)
    {
        const std::optional<std::string> membership = read("/proc/self/cgroup");
        const std::optional<std::string> mountinfo  = read("/proc/self/mountinfo");
        if (!membership || !mountinfo) return std::nullopt;

        std::optional<std::uint64_t> cpus;
        const auto                   limit = [&](const std::optional<std::uint64_t> granted)
        {
            if (granted && (!cpus || *granted < *cpus)) cpus = granted;
        };

        std::string_view rest = *membership;
        while (!rest.empty())
        {
            // "<hierarchy>:<controllers>:<group>"
            std::string_view line = next_field(rest, '\n');
            next_field(line, ':');
            const std::string_view controllers = next_field(line, ':');
            const std::string_view group       = line;
            const bool             unified     = controllers.empty();
            if (group.empty() || group.front() != '/' || (!unified && !lists(controllers, "cpu"))) continue;

            // When no mount shows the group, nothing is read for it.
            const std::optional<Hierarchy> hierarchy = hierarchy_of(*mountinfo, unified ? std::string_view{} : std::string_view{"cpu"}, group);
            if (!hierarchy) continue;

            // The mounted directory represents the group `root`, hence the
            // process's group resides below it along the rest of its path.
            std::string_view below = hierarchy->root == "/" ? group : group.substr(hierarchy->root.size());
            while (!below.empty() && below.back() == '/')
                below.remove_suffix(1);

            for (;;)
            {
                const std::string directory = hierarchy->directory + std::string(below);
                if (unified)
                {
                    if (const auto content = read(directory + "/cpu.max")) limit(cpus_of_cpu_max(*content));
                }
                else
                {
                    const auto quota  = read(directory + "/cpu.cfs_quota_us");
                    const auto period = read(directory + "/cpu.cfs_period_us");
                    if (quota && period) limit(cpus_of_cfs(*quota, *period));
                }
                if (below.empty()) break;
                const std::size_t slash = below.rfind('/');
                below                   = slash == std::string_view::npos ? std::string_view{} : below.substr(0, slash);
            }
        }
        return cpus;
    }

    // The count a process starts with when nothing sets one: the CPUs
    // permitted by its affinity mask (`affinity`; 0 if the platform reports no
    // mask, and then the total CPUs it reports, `hardware`, which may also be
    // 0), constrained by the CPUs allocated to its cgroups, and no less
    // than 1.
    inline unsigned automatic(const unsigned affinity, const unsigned hardware, const std::optional<std::uint64_t> cgroup_cpus)
    {
        std::uint64_t count = affinity != 0 ? affinity : hardware;
        if (cgroup_cpus && *cgroup_cpus < count) count = *cgroup_cpus;
        return static_cast<unsigned>(std::clamp<std::uint64_t>(count, 1, max_threads));
    }

    // The count that a setting names: a whole number ranging from 1 to
    // max_threads.
    inline std::optional<unsigned> named_count(const std::string_view text)
    {
        const auto value = whole_number(text);
        if (!value || *value < 1 || *value > max_threads) return std::nullopt;
        return static_cast<unsigned>(*value);
    }
}

namespace zelph::concurrency
{
    // The count of the process, which an engine employs unless provided with
    // its own: the count designated by the environment variable ZELPH_THREADS,
    // and otherwise thread_count::automatic derived from what the platform
    // reports. A value of the variable that refers to no count (and is neither
    // empty nor `auto`) is kept in `unusable` and not utilized. The
    // WebAssembly build runs on a single thread and has the count 1.
    struct ProcessThreads
    {
        unsigned    count{1};
        bool        from_environment{false};
        std::string unusable;
    };

    const ProcessThreads& process_threads();
}
