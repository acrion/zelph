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

// The number of threads that the engine's parallel work employs.
//
// The pool used to allocate std::thread::hardware_concurrency() threads for
// each engine upon creation: 24 on a machine equipped with 24 CPUs, even when
// the process was restricted to 8, and 0 when the platform reports none, which
// divided by zero in the scan and let a prune wait indefinitely. The scan of a
// relation woke the pool for two facts, its matches came in the order in which
// the threads happened to end, the build of a predicate index took its own
// specified number of threads and ignored `.parallel`, and a prune with one
// worker handed its work to a pool thread and then awaited its completion.

#include "test_helpers.hpp"

#include "concurrency/thread_count.hpp"
#include "concurrency/thread_pool.hpp"
#include "network/reasoning.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace zelph::test;
namespace thread_count = zelph::concurrency::thread_count;

namespace
{
    // The files associated with a platform, according to path: what
    // thread_count::cgroup_cpus is given to read from.
    struct Files
    {
        std::map<std::string, std::string> content;

        std::optional<std::string> operator()(const std::string& path) const
        {
            const auto it = content.find(path);
            if (it == content.end()) return std::nullopt;
            return it->second;
        }
    };

    const char* const unified_mount = "30 23 0:26 / /sys/fs/cgroup rw,nosuid,nodev,noexec,relatime shared:4 - cgroup2 cgroup2 rw,nsdelegate,memory_recursiveprot\n";

    // `count` facts `a<i> p b<i>`, expressed via Janet in a single line.
    void write_facts(const zelph::console::Interactive& interactive, const std::size_t count)
    {
        interactive.process("%(for i 0 " + std::to_string(count) + R"( (zelph/fact (string "a" i) "p" (string "b" i))))");
    }

    std::string shown(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& line)
    {
        collector.clear();
        interactive.process(line);
        for (const auto& event : collector.events())
            if (event.channel == zelph::io::OutputChannel::Out) return event.text;
        return {};
    }
} // namespace

// ---------------------------------------------------------------------------
// The policy: from where the count of a process is read.
// ---------------------------------------------------------------------------

TEST_CASE("threads: a cpu.max line grants the whole CPUs of its quota")
{
    // "max" sets no limit, which is equivalent to no value and not a count of
    // zero.
    CHECK_FALSE(thread_count::cpus_of_cpu_max("max 100000\n").has_value());
    CHECK(thread_count::cpus_of_cpu_max("200000 100000\n") == 2);
    CHECK(thread_count::cpus_of_cpu_max("800000 100000") == 8);

    // A quota that is not a whole number of CPUs is reduced to the nearest
    // lower integer: the second thread of one and a half CPUs would be
    // throttled. Half a CPU constitutes a limit of zero whole CPUs, which the
    // count turns into one thread.
    CHECK(thread_count::cpus_of_cpu_max("150000 100000\n") == 1);
    CHECK(thread_count::cpus_of_cpu_max("50000 100000\n") == 0);

    for (const char* text : {"", "\n", "max", "100000", "abc 100000", "100000 abc", "100000 0", "-1 100000", "100000 100000 7", " 100000 100000"})
    {
        CAPTURE(text);
        CHECK_FALSE(thread_count::cpus_of_cpu_max(text).has_value());
    }
}

TEST_CASE("threads: the two files of cgroup v1 grant the whole CPUs of their quota")
{
    CHECK_FALSE(thread_count::cpus_of_cfs("-1\n", "100000\n").has_value()); // no limit
    CHECK(thread_count::cpus_of_cfs("300000\n", "100000\n") == 3);
    CHECK(thread_count::cpus_of_cfs("250000", "100000") == 2);
    CHECK_FALSE(thread_count::cpus_of_cfs("300000\n", "0\n").has_value());
    CHECK_FALSE(thread_count::cpus_of_cfs("", "100000\n").has_value());
}

TEST_CASE("threads: the quota of a process is the smallest among its group and the groups above it")
{
    SUBCASE("cgroup v2: no group sets a limit")
    {
        Files files;
        files.content["/proc/self/cgroup"]                                 = "0::/user.slice/user-1001.slice/session-3.scope\n";
        files.content["/proc/self/mountinfo"]                              = unified_mount;
        files.content["/sys/fs/cgroup/user.slice/cpu.max"]                 = "max 100000\n";
        files.content["/sys/fs/cgroup/user.slice/user-1001.slice/cpu.max"] = "max 100000\n";
        CHECK_FALSE(thread_count::cgroup_cpus(files).has_value());
    }

    SUBCASE("cgroup v2: the group's own limit")
    {
        Files files;
        files.content["/proc/self/cgroup"]               = "0::/work/job\n";
        files.content["/proc/self/mountinfo"]            = unified_mount;
        files.content["/sys/fs/cgroup/work/job/cpu.max"] = "400000 100000\n";
        files.content["/sys/fs/cgroup/work/cpu.max"]     = "max 100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 4);
    }

    SUBCASE("cgroup v2: a group above it limits it further")
    {
        // A systemd slice configured with a CPUQuota exceeding the
        // unit's own: the unit gets no more than the slice's limit.
        Files files;
        files.content["/proc/self/cgroup"]               = "0::/work/job\n";
        files.content["/proc/self/mountinfo"]            = unified_mount;
        files.content["/sys/fs/cgroup/work/job/cpu.max"] = "400000 100000\n";
        files.content["/sys/fs/cgroup/work/cpu.max"]     = "200000 100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 2);
    }

    SUBCASE("cgroup v2: a container sees its group as the root of the mount")
    {
        // Within a cgroup namespace, the process resides in `/`, and the
        // container's limit is defined within the mounted directory
        // directly.
        Files files;
        files.content["/proc/self/cgroup"]      = "0::/\n";
        files.content["/proc/self/mountinfo"]   = unified_mount;
        files.content["/sys/fs/cgroup/cpu.max"] = "300000 100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 3);
    }

    SUBCASE("a mount that shows a group other than the root")
    {
        // Without a namespace, a container's mount shows the container's
        // group as its directory, even though the process continues to
        // reference the whole path: the group beneath the mount's own
        // level is what resides beneath the directory. Read as a path
        // below the directory as it stands, the group's own file would not
        // be found, and its limit of three CPUs would be overlooked in
        // favour of the five belonging to the group above.
        Files files;
        files.content["/proc/self/cgroup"]            = "0::/docker/abc/inner\n";
        files.content["/proc/self/mountinfo"]         = "30 23 0:26 /docker/abc /sys/fs/cgroup ro,nosuid - cgroup2 cgroup rw\n";
        files.content["/sys/fs/cgroup/inner/cpu.max"] = "300000 100000\n";
        files.content["/sys/fs/cgroup/cpu.max"]       = "500000 100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 3);

        // A group whose name only starts with the same sequence as the mount's
        // is not beneath it, hence the mount does not show it, and nothing is
        // read for it: neither the quota of the mounted directory, which
        // belongs to a different group, nor a path formed from the rest of its
        // name.
        files.content["/proc/self/cgroup"]             = "0::/docker/abcd/inner\n";
        files.content["/sys/fs/cgroupd/inner/cpu.max"] = "100000 100000\n";
        CHECK_FALSE(thread_count::cgroup_cpus(files).has_value());
    }

    SUBCASE("a mount of another group listed before the mount that shows the group")
    {
        // A hierarchy may be mounted multiple times. The initial mount was
        // taken, and a group it did not show was read as its directory: an
        // unrelated bind mount listed before /sys/fs/cgroup gave its own
        // quota of one CPU rather than the two belonging to the process's
        // group.
        Files files;
        files.content["/proc/self/cgroup"]               = "0::/work/job\n";
        files.content["/proc/self/mountinfo"]            = std::string("29 23 0:26 /other /mnt/x rw - cgroup2 cgroup2 rw\n") + unified_mount;
        files.content["/mnt/x/cpu.max"]                  = "100000 100000\n";
        files.content["/sys/fs/cgroup/work/job/cpu.max"] = "200000 100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 2);
    }

    SUBCASE("a mount whose paths the kernel wrote with escapes")
    {
        // /proc/self/mountinfo writes a backslash in a path as `\134`, whereas
        // /proc/self/cgroup writes the group exactly as it is. When compared in
        // their written form, the mount of a container lacking a cgroup
        // namespace does not show the process's group if that group's name
        // contains a backslash, as the unit names that systemd escapes do, and
        // no quota is read.
        Files files;
        files.content["/proc/self/cgroup"]              = "0::/machine.slice/machine-my\\x2dbox.scope/payload\n";
        files.content["/proc/self/mountinfo"]           = "31 23 0:27 /machine.slice/machine-my\\134x2dbox.scope /sys/fs/cgroup rw,nosuid - cgroup2 cgroup2 rw\n";
        files.content["/sys/fs/cgroup/cpu.max"]         = "200000 100000\n";
        files.content["/sys/fs/cgroup/payload/cpu.max"] = "max 100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 2);

        // The mount point is expressed identically: a space within it
        // is represented as `\040`.
        files.content["/proc/self/mountinfo"]            = "31 23 0:27 /machine.slice/machine-my\\134x2dbox.scope /mnt/cgroup\\040two rw,nosuid - cgroup2 cgroup2 rw\n";
        files.content["/mnt/cgroup two/cpu.max"]         = "300000 100000\n";
        files.content["/mnt/cgroup two/payload/cpu.max"] = "max 100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 3);
    }

    SUBCASE("cgroup v1: the hierarchy of the cpu controller")
    {
        Files files;
        files.content["/proc/self/cgroup"]                                     = "12:memory:/job\n7:cpu,cpuacct:/job/part\n3:cpuset:/\n";
        files.content["/proc/self/mountinfo"]                                  = "40 30 0:35 / /sys/fs/cgroup/memory rw - cgroup cgroup rw,memory\n"
                                                                                 "41 30 0:36 / /sys/fs/cgroup/cpu,cpuacct rw,nosuid - cgroup cgroup rw,cpu,cpuacct\n"
                                                                                 "42 30 0:37 / /sys/fs/cgroup/cpuset rw - cgroup cgroup rw,cpuset\n";
        files.content["/sys/fs/cgroup/cpu,cpuacct/job/part/cpu.cfs_quota_us"]  = "-1\n";
        files.content["/sys/fs/cgroup/cpu,cpuacct/job/part/cpu.cfs_period_us"] = "100000\n";
        files.content["/sys/fs/cgroup/cpu,cpuacct/job/cpu.cfs_quota_us"]       = "600000\n";
        files.content["/sys/fs/cgroup/cpu,cpuacct/job/cpu.cfs_period_us"]      = "100000\n";
        // The file bearing the same name within a different controller's
        // hierarchy is not read: `cpuset` only resembles `cpu` in its
        // initial portion.
        files.content["/sys/fs/cgroup/cpuset/cpu.cfs_quota_us"]  = "100000\n";
        files.content["/sys/fs/cgroup/cpuset/cpu.cfs_period_us"] = "100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 6);
    }

    SUBCASE("both hierarchies: the smaller limit")
    {
        Files files;
        files.content["/proc/self/cgroup"]                        = "7:cpu,cpuacct:/job\n0::/job\n";
        files.content["/proc/self/mountinfo"]                     = std::string(unified_mount) + "41 30 0:36 / /sys/fs/cgroup/cpu rw - cgroup cgroup rw,cpu,cpuacct\n";
        files.content["/sys/fs/cgroup/job/cpu.max"]               = "800000 100000\n";
        files.content["/sys/fs/cgroup/cpu/job/cpu.cfs_quota_us"]  = "200000\n";
        files.content["/sys/fs/cgroup/cpu/job/cpu.cfs_period_us"] = "100000\n";
        CHECK(thread_count::cgroup_cpus(files) == 2);
    }

    SUBCASE("a platform without these files has no quota")
    {
        CHECK_FALSE(thread_count::cgroup_cpus(Files{}).has_value());

        // A hierarchy that remains unmounted is likewise not accessed for
        // reading.
        Files files;
        files.content["/proc/self/cgroup"]          = "0::/job\n";
        files.content["/proc/self/mountinfo"]       = "22 1 0:21 / /proc rw - proc proc rw\n";
        files.content["/sys/fs/cgroup/job/cpu.max"] = "100000 100000\n";
        CHECK_FALSE(thread_count::cgroup_cpus(files).has_value());
    }
}

TEST_CASE("threads: the automatic count is the smaller of the affinity mask and the quota, and at least one")
{
    // A process granted access to 8 out of 24 CPUs: the mask decides, not the
    // machine.
    CHECK(thread_count::automatic(8, 24, std::nullopt) == 8);
    CHECK(thread_count::automatic(8, 24, 2) == 2);
    CHECK(thread_count::automatic(8, 24, 16) == 8);

    // A quota under a single CPU still leaves one thread.
    CHECK(thread_count::automatic(8, 24, 0) == 1);

    // A platform that reports no mask: the CPUs it reports altogether.
    CHECK(thread_count::automatic(0, 24, std::nullopt) == 24);
    CHECK(thread_count::automatic(0, 24, 4) == 4);

    // std::thread::hardware_concurrency() might be 0, "not computable". A pool
    // of such a size had no worker to take a scan's tasks.
    CHECK(thread_count::automatic(0, 0, std::nullopt) == 1);
    CHECK(thread_count::automatic(0, 0, 4) == 1);

    CHECK(thread_count::automatic(5000, 5000, std::nullopt) == thread_count::max_threads);
}

TEST_CASE("threads: a count is a whole number from 1 to 1024")
{
    CHECK(thread_count::named_count("1") == 1U);
    CHECK(thread_count::named_count("24") == 24U);
    CHECK(thread_count::named_count("1024") == 1024U);
    for (const char* text : {"", "0", "1025", "-1", "+4", "4.0", "4 ", " 4", "four", "0x10", "99999999999999999999"})
    {
        CAPTURE(text);
        CHECK_FALSE(thread_count::named_count(text).has_value());
    }
}

// ---------------------------------------------------------------------------
// The setting of the engine.
// ---------------------------------------------------------------------------

// The count of the process is the value with which the test binary was
// launched (the environment variable ZELPH_THREADS, the affinity mask, the
// quota), thus these cases read it from a fresh engine and compare with it.
TEST_CASE("threads: .threads shows the count of the engine and sets it")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const unsigned              of_process = interactive.graph()->threads();
    REQUIRE(of_process >= 1);
    CHECK(interactive.graph()->threads_setting() == 0);
    CHECK(shown(collector, interactive, ".threads").starts_with("Threads: " + std::to_string(of_process) + " ("));

    CHECK(shown(collector, interactive, ".threads 3") == "Threads: 3");
    CHECK(interactive.graph()->threads() == 3);
    CHECK(interactive.graph()->threads_setting() == 3);
    CHECK(shown(collector, interactive, ".threads") == "Threads: 3");

    // A number exceeding the machine's CPUs is taken: one is required for a
    // test assessing the parallel path on a small machine.
    CHECK(shown(collector, interactive, ".threads 1024") == "Threads: 1024");

    // `auto` returns to the count associated with the process.
    CHECK(shown(collector, interactive, ".threads auto").starts_with("Threads: " + std::to_string(of_process) + " ("));
    CHECK(interactive.graph()->threads_setting() == 0);
    CHECK(interactive.graph()->threads() == of_process);

    // `.parallel` functions as an independent switch: the count remains
    // unchanged, and no parallel work relies on it.
    interactive.process(".threads 3");
    interactive.process(".parallel");
    CHECK(shown(collector, interactive, ".threads") == "Threads: 3, unused while parallel processing is disabled (.parallel)");
    CHECK(interactive.graph()->threads() == 3);
    CHECK(interactive.graph()->parallel_threads() == 1);
    interactive.process(".parallel");
    CHECK(interactive.graph()->parallel_threads() == 3);
}

TEST_CASE("threads: .threads refuses what is no count")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".threads 3");

    for (const char* line : {".threads 0", ".threads -1", ".threads 1025", ".threads four", ".threads 2.5", ".threads 2 3", ".threads on", ".threads default"})
    {
        CAPTURE(line);
        CHECK_THROWS_WITH(interactive.process(line), doctest::Contains("Usage: .threads [<count>|auto] -- <count> is a number from 1 to 1024"));
        CHECK(interactive.graph()->threads() == 3);
    }
    CHECK_THROWS_AS(interactive.graph()->set_threads(1025), std::invalid_argument);
    CHECK(interactive.graph()->threads() == 3);
}

// The count is a setting of the engine: `.new` returns it to the count of the
// process, and a `--default` line reinstates the count for every engine.
TEST_CASE("threads: .new returns to the count of the process, and a default line sets the count for every engine")
{
    SUBCASE("without a default")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        const unsigned              of_process = interactive.graph()->threads();
        interactive.process(".threads 5");
        interactive.process(".new");
        CHECK(interactive.graph()->threads_setting() == 0);
        CHECK(interactive.graph()->threads() == of_process);
    }

    SUBCASE("with a default")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.add_default(".threads 5");
        CHECK(interactive.graph()->threads() == 5);
        interactive.process(".threads auto");
        CHECK(interactive.graph()->threads_setting() == 0);
        interactive.process(".new");
        CHECK(interactive.graph()->threads() == 5);
    }
}

TEST_CASE("threads: zelph/threads reads the count and sets it")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const unsigned              of_process = interactive.graph()->threads();

    CHECK(shown(collector, interactive, "%(zelph/threads)") == std::to_string(of_process));
    CHECK(shown(collector, interactive, "%(zelph/threads 4)") == "4");
    CHECK(interactive.graph()->threads_setting() == 4);
    CHECK(shown(collector, interactive, "%(zelph/threads)") == "4");
    CHECK(shown(collector, interactive, "%(zelph/threads :auto)") == std::to_string(of_process));
    CHECK(interactive.graph()->threads_setting() == 0);

    interactive.process("%(zelph/threads 4)");
    for (const char* call : {R"(%(zelph/threads "2"))", "%(zelph/threads 0)", "%(zelph/threads -2)", "%(zelph/threads 2.5)", "%(zelph/threads 1025)", "%(zelph/threads :default)", "%(zelph/threads nil)", "%(zelph/threads 2 3)"})
    {
        CAPTURE(call);
        CHECK_THROWS(interactive.process(call));
        CHECK(interactive.graph()->threads() == 4);
    }
}

// ---------------------------------------------------------------------------
// The pool and the scan.
// ---------------------------------------------------------------------------

// Each engine, upon creation, initiated its own pool, and `.new` joined the
// threads and restarted them: roughly a millisecond of CPU per engine for
// threads that a small session never hands a task to. The pool starts with
// the first scan that is spread over it.
TEST_CASE("threads: an engine starts no worker thread until a scan is spread over them")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".threads 4");
    CHECK(interactive.graph()->worker_threads_running() == 0);

    // A rule applied to a small relation, executed until reaching its fixed
    // point: no spreading occurs, and the run names the count all the same.
    process_lines(interactive, R"(
a q b
c q d
(X q Y) => (Y r X)
)");
    collector.clear();
    interactive.run(true, false, false);
    CHECK(any_event_contains(collector, "Starting reasoning with 4 worker threads."));
    CHECK(any_event_contains(collector, "Parallel unifications activated for 0 distinct fixed relations."));
    CHECK(interactive.graph()->worker_threads_running() == 0);

    // A relation of the size from which a scan is spread: the pool starts
    // with the engine's count.
    write_facts(interactive, thread_count::parallel_scan_min_facts);
    collector.clear();
    interactive.process("X p X");
    CHECK(collect_answers(collector).empty());
    CHECK(interactive.graph()->worker_threads_running() == 4);

    // A different count becomes active with the next scan that is spread.
    interactive.process(".threads 2");
    CHECK(interactive.graph()->worker_threads_running() == 4);
    interactive.process("X p X");
    CHECK(interactive.graph()->worker_threads_running() == 2);
}

// A pool that was started with a different count while a task remained pending
// kept its workers, and a scan's last task signals the end of the scan before
// its worker counts the task as completed: a launch immediately following a
// scan might execute using the old number of workers (a start right after a
// task had signalled its completion maintained the old count approximately once
// in a hundred tries). The pool waits for the pending tasks and subsequently
// replaces its workers. A task that awaits a flag serves as a placeholder for
// the one whose worker has not yet accounted for it: the pool is started while
// that task is pending, unless the thread that starts it is delayed for longer
// than the 50 ms after which the flag is set, and in such a scenario the case
// holds nothing.
TEST_CASE("threads: a pool started with another count while a task is pending replaces its workers once the task has ended")
{
    zelph::concurrency::ThreadPool pool;
    pool.start(4);
    std::atomic<bool> release{false};
    pool.enqueue([&release]
                 {
        while (!release.load())
            std::this_thread::yield(); });
    std::thread releaser([&release]
                         {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release.store(true); });
    pool.start(2);
    CHECK(pool.count() == 2);
    releaser.join();
}

// Below the threshold, a scan remains tied to the thread that initiated it:
// for a relation comprising a few hundred facts, waking the pool incurs
// greater cost than conducting the scan. The figures behind the number stand
// at its definition; this case holds the border itself.
TEST_CASE("threads: a relation below the launch threshold is scanned on the asking thread")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".threads 3");
    interactive.process(".auto-run");

    // What is counted is what the scan would retrieve: each node that
    // references the relation, which is its facts, its own declaration
    // `p ~ ->`, and the pattern of the rule below. These together amount to
    // one fewer than the threshold here.
    write_facts(interactive, thread_count::parallel_scan_min_facts - 3);
    interactive.process("(X p X) => (X selfp yes)");
    collector.clear();
    interactive.run(true, false, false);
    CHECK(any_event_contains(collector, "Parallel unifications activated for 0 distinct fixed relations."));
    CHECK(interactive.graph()->worker_threads_running() == 0);

    interactive.process("extra p one");
    collector.clear();
    interactive.run(true, false, false);
    CHECK(any_event_contains(collector, "Parallel unifications activated for 1 distinct fixed relations."));
    CHECK(interactive.graph()->worker_threads_running() == 3);
}

TEST_CASE("threads: with one thread, and with .parallel off, no scan is spread whatever the relation's size")
{
    for (const char* setting : {".threads 1", ".parallel"})
    {
        CAPTURE(setting);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".threads 4");
        interactive.process(setting);
        interactive.process(".auto-run");
        write_facts(interactive, 2 * thread_count::parallel_scan_min_facts);
        interactive.process("(X p X) => (X selfp yes)");
        collector.clear();
        interactive.run(true, false, false);
        CHECK(any_event_contains(collector, "Starting reasoning with 1 worker thread."));
        CHECK(any_event_contains(collector, "Parallel unifications activated for 0 distinct fixed relations."));
        CHECK(interactive.graph()->worker_threads_running() == 0);
    }
}

// The tasks of a scan pushed their matches into a single queue as they found
// them, so the matches arrived in the sequence dictated by the order in which
// the threads happened to run: two runs of one program printed their
// deductions in differing sequences, and one run in six hundred printed a
// deduction with another premise. Each task now keeps its matches, and they
// are handed on in the order of the ranges the tasks scan, which corresponds
// to the order one thread reads the relation in.
TEST_CASE("threads: a scan that is spread over threads hands its matches on in the order of one thread" * doctest::test_suite("slow"))
{
    const std::size_t facts = 3 * thread_count::parallel_scan_min_facts + 17;

    const auto answers_and_deductions = [&](const char* setting)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(setting);
        interactive.process(".auto-run");
        write_facts(interactive, facts);

        collector.clear();
        interactive.process("X p Y");
        std::vector<std::string> lines = collect_answers(collector);
        REQUIRE(lines.size() == facts);

        // The deductions generated by a rule over the relation, in the
        // sequence they appear when printed, and those produced by a
        // rule over a second relation that arrives at each of its seven
        // outcomes from roughly two thousand facts: only the first
        // derivation is printed, with its premise positioned following
        // the arrow.
        interactive.process("%(for i 0 " + std::to_string(facts) + R"( (zelph/fact (string "a" i) "g" (string "c" (% i 7)))))");
        interactive.process("(X p Y) => (Y q X)");
        interactive.process("(X g Y) => (Y marked yes)");
        collector.clear();
        interactive.run(true, false, false);
        std::size_t deductions = 0;
        for (const auto& event : collector.events())
            if (event.channel == zelph::io::OutputChannel::Out && event.text.find("⇐") != std::string::npos)
            {
                lines.push_back(event.text);
                ++deductions;
            }
        REQUIRE(deductions == facts + 7);
        return lines;
    };

    const std::vector<std::string> on_one_thread = answers_and_deductions(".threads 1");
    CHECK(answers_and_deductions(".parallel") == on_one_thread);

    // More threads than the machine may have CPUs, and counts that do not
    // divide the relation: the order remains that of one thread at all times.
    for (const char* setting : {".threads 2", ".threads 3", ".threads 8", ".threads 8", ".threads 31"})
    {
        CAPTURE(setting);
        CHECK(answers_and_deductions(setting) == on_one_thread);
    }
}

// The removal of nodes united the sets of nodes its workers had collected in
// the order of the workers and erased them in that order. The erasures left
// the adjacency of a surviving node in an order contingent on their
// sequence, and every subsequent scan adheres to that order: after the same
// prune, a query answered in a different order and a rule printed distinct
// deductions, some with different premises, for each number of threads. The
// union is erased in ascending order of the ids.
TEST_CASE("threads: after a prune, the answers and deductions come in the order of one thread")
{
    const auto answers_and_deductions = [](const char* setting)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(setting);
        interactive.process(".auto-run");
        interactive.process(R"(%(for i 0 3000 (zelph/fact (string "a" i) "p" (string "b" (% i 61)))))");
        interactive.process(R"(%(for i 0 50 (zelph/fact (string "b" i) "bad" "yes")))");
        collector.clear();
        interactive.process(".prune-nodes X bad yes");
        REQUIRE(any_output_contains(collector, "50 nodes"));

        collector.clear();
        interactive.process("X p Y");
        std::vector<std::string> lines = collect_answers(collector);
        REQUIRE(lines.size() == 539);

        // Each of the 11 surviving objects originates from 49 facts, and only
        // the first derivation is output, accompanied by its premise.
        interactive.process("(X p Y) => (Y seen yes)");
        collector.clear();
        interactive.run(true, false, false);
        std::size_t deductions = 0;
        for (const auto& event : collector.events())
            if (event.channel == zelph::io::OutputChannel::Out && event.text.find("⇐") != std::string::npos)
            {
                lines.push_back(event.text);
                ++deductions;
            }
        REQUIRE(deductions == 11);
        return lines;
    };

    const std::vector<std::string> on_one_thread = answers_and_deductions(".threads 1");
    CHECK(answers_and_deductions(".parallel") == on_one_thread);
    for (const char* setting : {".threads 2", ".threads 3", ".threads 8"})
    {
        CAPTURE(setting);
        CHECK(answers_and_deductions(setting) == on_one_thread);
    }
}

// A victim of a prune may reside within the closure of another victim: `X rel
// o` binds X to `a` and to `(a p b)` alike. Such a victim was counted as a node
// only where another worker had collected the closure it belongs to; a worker
// holding both victims counted it among the facts that came along. For the same
// graph and the same removal, the command reported "4 matching facts and 2
// nodes" with one or two threads and "3 matching facts and 3 nodes" with three
// or more. Every victim counts as a node.
TEST_CASE("threads: a prune counts a victim inside another victim's closure as a node, whatever the number of threads")
{
    for (const char* setting : {".parallel", ".threads 1", ".threads 2", ".threads 3", ".threads 8"})
    {
        CAPTURE(setting);
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(setting);
        interactive.process("a p b");
        interactive.process("a rel o");
        interactive.process("(a p b) rel o");
        interactive.process("c rel o");
        collector.clear();
        interactive.process(".prune-nodes X rel o");
        CHECK(any_output_contains(collector, "Pruned 3 matching facts and 3 nodes."));
    }
}

// The tasks of a scan spread over the pool apply their own copy of the filters
// that a scan on a single thread applies to a candidate and its readings: a
// rule's ground pattern, a refuted fact, a rule's text, and what the engine
// writes regarding rules and contradictions. A relation below
// thread_count::parallel_scan_min_facts is scanned on the asking thread,
// meaning only a relation of that size arrives at the copy: before these
// cases, each of its filters could have been omitted with every case passing.
// Each case below runs one session with four threads and with one, over
// relations of that size, and performs comparison.
namespace
{
    struct Scanned
    {
        std::vector<std::string> answers; // sorted
        std::size_t              workers; // Following the query, the sole line capable of initiating them: no rule runs
    };

    Scanned scanned(const char* setting, const std::string& session, const char* query)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(setting);
        process_lines(interactive, session);
        collector.clear();
        interactive.process(query);
        Scanned result{collect_answers(collector), interactive.graph()->worker_threads_running()};
        std::ranges::sort(result.answers);
        return result;
    }

    // A relation `p` sufficiently large to be spread, written after the
    // lines `before` and after `.auto-run` has switched off the runs that
    // each line would start.
    std::string with_relation_p(const std::string& before)
    {
        return before + "\n.auto-run\n%(for i 0 " + std::to_string(thread_count::parallel_scan_min_facts + 4)
             + R"( (zelph/fact (string "a" i) "p" (string "b" i))))";
    }

    bool any_answer(const std::vector<std::string>& answers, const std::string& answer)
    {
        return std::ranges::find(answers, answer) != answers.end();
    }
} // namespace

// `¬(zz p yy)` keeps the statement as a fact with probability 0, thereby
// refuting it rather than leaving it unknown; no condition can match it. `¬`
// also marks a statement it writes as mentioned and refuses one that is
// claimed, so each refuted fact is simultaneously a mention or a rule's
// pattern as well, which the task's test of rule patterns excludes as well:
// the case holds the two tests together.
TEST_CASE("threads: a scan spread over the pool does not answer a refuted fact")
{
    const std::string session = with_relation_p("¬(zz p yy)");
    const Scanned     on_four = scanned(".threads 4", session, "X p Y");
    const Scanned     on_one  = scanned(".threads 1", session, "X p Y");
    CHECK(on_four.workers == 4);
    CHECK(on_one.workers == 0);
    CHECK(on_four.answers == on_one.answers);
    CHECK(on_four.answers.size() == thread_count::parallel_scan_min_facts + 4);
    CHECK_FALSE(any_answer(on_four.answers, "zz p yy"));
}

// The condition `zz p yy` of a ground rule is a statement nobody asserted: it
// exists solely because the rule was written.
TEST_CASE("threads: a scan spread over the pool does not answer the ground pattern of a rule")
{
    const std::string session = with_relation_p("(zz p yy) => (zz seen yy)");
    const Scanned     on_four = scanned(".threads 4", session, "X p Y");
    const Scanned     on_one  = scanned(".threads 1", session, "X p Y");
    CHECK(on_four.workers == 4);
    CHECK(on_four.answers == on_one.answers);
    CHECK(on_four.answers.size() == thread_count::parallel_scan_min_facts + 4);
    CHECK_FALSE(any_answer(on_four.answers, "zz p yy"));
}

// A rule whose conditions are contained within a set, and whose consequence
// is ground, has its variables exclusively among the members of that set,
// beyond the closures of the subject and the consequence of its `=>` fact
// that the extraction of bindings examines: only the rule's own flag tells it
// from data. The ground rules around it are data, and a query answers them.
TEST_CASE("threads: a scan spread over the pool does not answer the text of a rule" * doctest::test_suite("slow"))
{
    const std::string session = std::string("(X p Y, Y p Z) => (k ok y)\n.auto-run\n%(for i 0 ") + std::to_string(thread_count::parallel_scan_min_facts + 4)
                              + R"( (zelph/fact (zelph/fact (string "a" i) "p" "b") "=>" (zelph/fact (string "c" i) "q" "d"))))";
    const Scanned     on_four = scanned(".threads 4", session, "A => B");
    const Scanned     on_one  = scanned(".threads 1", session, "A => B");
    CHECK(on_four.workers == 4);
    CHECK(on_four.answers == on_one.answers);
    CHECK(on_four.answers.size() == thread_count::parallel_scan_min_facts + 4);
    CHECK_FALSE(std::ranges::any_of(on_four.answers, [](const std::string& answer)
                                    { return answer.find("k ok y") != std::string::npos; }));
}

// What the engine writes regarding a rule or a contradiction is not data: the
// memberships of a contradiction record and its marking, along with the mark
// of a rule's ground pattern. A rule over rules reads the tag `~ conjunction`
// from a rule's condition set, which a query answers in either setting, while
// the memberships of that set hold variables and yield no answer. Scanned
// across thousands of `in` and `~` data facts, the two settings produce
// identical results.
TEST_CASE("threads: a scan spread over the pool does not answer what the engine writes about rules and contradictions" * doctest::test_suite("slow"))
{
    // Facts `x<i> <relation> s<i % 7>`, sufficient to enable a scan to be
    // spread.
    const auto facts_of = [](const std::string& relation)
    {
        return "%(for i 0 " + std::to_string(thread_count::parallel_scan_min_facts + 4) + R"( (zelph/fact (string "x" i) ")" + relation
             + R"(" (string "s" (% i 7)))))";
    };
    const std::string session = "x g y\nx h y\n(A g B, A h B) => !\n(X p Y, Y p Z) => (k ok y)\n(a p b) => (c q d)\n.auto-run\n"
                              + facts_of("in") + "\n" + facts_of("~");
    for (const char* query : {"A in B", "A ~ B"})
    {
        CAPTURE(query);
        const Scanned on_four = scanned(".threads 4", session, query);
        const Scanned on_one  = scanned(".threads 1", session, query);
        CHECK(on_four.workers == 4);
        CHECK(on_four.answers == on_one.answers);
        CHECK(on_four.answers.size() >= thread_count::parallel_scan_min_facts + 4);
    }
}

// The build of a predicate index asked std::thread::hardware_concurrency()
// directly and started that many threads as well, with `.parallel` off, whose
// help says that `off` means one thread everywhere.
TEST_CASE("threads: the build of a predicate index keeps to .parallel and to the count" * doctest::test_suite("slow"))
{
    // More than 2^16 facts on one node, so that a closure shifts from direct
    // traversal to the index, and more than 2^15 relation nodes, from which
    // the build is spread at all.
    const std::size_t facts = 66000;

    const auto build_line = [&](const std::vector<const char*>& settings)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        for (const char* setting : settings)
            interactive.process(setting);
        interactive.process(".auto-run");
        interactive.process("%(for i 0 " + std::to_string(facts) + R"( (zelph/fact (string "a" i) "p" "hub")))");
        collector.clear();
        interactive.process(R"(%(string "CLOSURE-" (length (zelph/closure-sources "hub" "p")) "."))");
        CHECK(any_output_contains(collector, "CLOSURE-" + std::to_string(facts) + "."));
        for (const auto& event : collector.events())
            if (event.text.find("Building adjacency index") != std::string::npos) return event.text;
        return std::string{};
    };

    CHECK(build_line({".threads 3"}).ends_with("relation nodes (3 thread(s))..."));
    CHECK(build_line({".threads 1"}).ends_with("relation nodes (1 thread(s))..."));
    CHECK(build_line({".threads 3", ".parallel"}).ends_with("relation nodes (1 thread(s))..."));
}

// With `.parallel` off, and with a single worker, the removal of nodes handed
// the collecting of what its victims take with them to a single pool thread
// and spun until that thread was done. It collects on the calling thread then,
// and the pool does not start for it.
TEST_CASE("threads: a prune with one worker collects on the calling thread")
{
    const auto pool_after_prune = [](const std::vector<const char*>& settings)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        for (const char* setting : settings)
            interactive.process(setting);
        process_lines(interactive, R"(
a p hub
c p hub
e q hub
(a p hub) noted yes
)");
        collector.clear();
        interactive.process(".prune-nodes X p hub");
        CHECK(any_output_contains(collector, "2 nodes"));
        collector.clear();
        interactive.process("X p hub");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("X q hub");
        CHECK(collect_answers(collector) == std::vector<std::string>{"e q hub"});
        collector.clear();
        interactive.process("X noted yes");
        CHECK(collect_answers(collector).empty());
        return interactive.graph()->worker_threads_running();
    };

    CHECK(pool_after_prune({".threads 1"}) == 0);
    CHECK(pool_after_prune({".threads 3", ".parallel"}) == 0);
    CHECK(pool_after_prune({".threads 3"}) == 3);
}
