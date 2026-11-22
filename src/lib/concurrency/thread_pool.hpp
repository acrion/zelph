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

#include <atomic>
#include <cstddef>
#include <functional>

#ifndef __EMSCRIPTEN__
    #include <condition_variable>
    #include <mutex>
    #include <queue>
    #include <thread>
    #include <vector>
#endif

namespace zelph::concurrency
{
    inline thread_local bool tl_is_pool_worker = false;

#ifdef __EMSCRIPTEN__
    // Single-threaded WebAssembly build (no pthreads, no SharedArrayBuffer):
    // enqueue() executes the task synchronously. The interface is identical
    // to the native pool below, so no call site needs to change.
    // tl_is_pool_worker is set during task execution to mirror the native
    // pool's behavior for call sites that branch on it.
    class ThreadPool
    {
    public:
        void start(size_t /*num_threads*/) {}

        void enqueue(std::function<void()> task)
        {
            struct FlagGuard
            {
                bool previous{tl_is_pool_worker};
                FlagGuard() { tl_is_pool_worker = true; }
                ~FlagGuard() { tl_is_pool_worker = previous; }
            } guard;

            task();
        }

        size_t count() const
        {
            return 0; // no worker ever runs here
        }

        void wait()
        {
            // enqueue() is synchronous, so nothing can ever be pending.
        }

        std::atomic<size_t> pending_tasks{0};
    };
#else
    // A pool possesses no thread until start() is called for its first launch:
    // an engine that never spreads work over threads then costs none, and that
    // is every engine within a small session, a test case, and a `.new`.
    class ThreadPool
    {
    public:
        ThreadPool()                             = default;
        ThreadPool(const ThreadPool&)            = delete;
        ThreadPool& operator=(const ThreadPool&) = delete;

        ~ThreadPool()
        {
            stop_workers();
        }

        // Executed before handing off the tasks of a launch: initiates
        // `num_threads` workers, or replaces those currently running if their
        // count differs, once the tasks still pending have ended. A scan's
        // last task signals the end of the scan before its worker counts the
        // task as completed, so a launch that immediately follows the scan may
        // discover that task still pending; kept instead, the old workers ran
        // that launch.
        void start(const size_t num_threads)
        {
            if (workers.size() == num_threads) return;
            wait();
            stop_workers();
            for (size_t i = 0; i < num_threads; ++i)
            {
                workers.emplace_back(
                    [this]
                    {
                        tl_is_pool_worker = true;
                        while (true)
                        {
                            std::function<void()> task;
                            {
                                std::unique_lock<std::mutex> lock(queue_mutex);
                                condition.wait(lock, [this]
                                               { return stop || !tasks.empty(); });
                                if (stop && tasks.empty()) return;
                                task = std::move(tasks.front());
                                tasks.pop();
                            }
                            task();
                            --pending_tasks;
                        }
                    });
            }
        }

        void enqueue(std::function<void()> task)
        {
            // Without a worker, nobody would ever take the task: it
            // runs here, just as it does within the WebAssembly build.
            if (workers.empty())
            {
                struct FlagGuard
                {
                    bool previous{tl_is_pool_worker};
                    FlagGuard() { tl_is_pool_worker = true; }
                    ~FlagGuard() { tl_is_pool_worker = previous; }
                } guard;

                task();
                return;
            }
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                tasks.emplace(std::move(task));
            }
            ++pending_tasks;
            condition.notify_one();
        }

        // The workers that run: none before the first start().
        size_t count() const
        {
            return workers.size();
        }

        void wait()
        {
            while (pending_tasks > 0)
            {
                std::this_thread::yield();
            }
        }

        std::atomic<size_t> pending_tasks{0};

    private:
        void stop_workers()
        {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                stop = true;
            }
            condition.notify_all();
            for (auto& worker : workers)
            {
                if (worker.joinable()) worker.join();
            }
            workers.clear();
            stop = false;
        }

        std::vector<std::thread>          workers;
        std::queue<std::function<void()>> tasks;
        std::mutex                        queue_mutex;
        std::condition_variable           condition;
        bool                              stop{false};
    };
#endif
}
