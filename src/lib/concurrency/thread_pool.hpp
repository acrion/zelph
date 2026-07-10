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
#include <functional>

#ifndef __EMSCRIPTEN__
    #include <condition_variable>
    #include <mutex>
    #include <queue>
    #include <thread>
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
        explicit ThreadPool(size_t /*num_threads*/) {}

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

        size_t count()
        {
            return 1; // one execution lane; callers may partition work by this
        }

        void wait()
        {
            // enqueue() is synchronous, so nothing can ever be pending.
        }

        std::atomic<size_t> pending_tasks{0};
    };
#else
    class ThreadPool
    {
    public:
        explicit ThreadPool(size_t num_threads)
        {
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

        ~ThreadPool()
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
        }

        void enqueue(std::function<void()> task)
        {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                tasks.emplace(std::move(task));
            }
            ++pending_tasks;
            condition.notify_one();
        }

        size_t count()
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
        std::vector<std::thread>          workers;
        std::queue<std::function<void()>> tasks;
        std::mutex                        queue_mutex;
        std::condition_variable           condition;
        bool                              stop{false};
    };
#endif
}
