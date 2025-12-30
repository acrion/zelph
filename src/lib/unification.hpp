/*
Copyright (c) 2025 acrion innovations GmbH
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

#include "string_utils.hpp"
#include "thread_pool.hpp"
#include "zelph.hpp"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_set>

namespace zelph
{
    namespace network
    {
        class Unification
        {
        public:
            Unification(Zelph* n, Node condition, Node parent, const std::shared_ptr<Variables>& variables, const std::shared_ptr<Variables>& unequals, ThreadPool* pool = nullptr);
            std::shared_ptr<Variables> Next();
            std::shared_ptr<Variables> Unequals();
            bool                       uses_parallel() const { return _use_parallel; }

            void wait_for_completion()
            {
                if (!_use_parallel) return;

                std::unique_lock<std::mutex> lock(_queue_mtx);
                _queue_cv.wait(lock, [this]
                               { return _active_tasks.load() == 0; });
            }

        private:
            bool                       increment_fact_index();
            std::shared_ptr<Variables> extract_bindings(const Node subject, const adjacency_set& objects, const Node relation) const;

            Zelph* const               _n;
            Node                       _parent;
            std::shared_ptr<Variables> _variables;
            std::shared_ptr<Variables> _unequals;
            adjacency_set              _relation_list;
            Node                       _relation_variable{0};
            Node                       _subject{0};
            adjacency_set              _objects;

            // Parallel mode
            ThreadPool*                            _pool{nullptr};
            bool                                   _use_parallel{false};
            std::queue<std::shared_ptr<Variables>> _match_queue;
            std::mutex                             _queue_mtx;
            std::condition_variable                _queue_cv;
            std::atomic<size_t>                    _active_tasks{0};
            std::vector<Node>                      _snapshot_vec;

            // Sequential fallback
            adjacency_set::iterator _relation_index;
            adjacency_set::iterator _fact_index;
            adjacency_set           _facts_snapshot;
            bool                    _fact_index_initialized{false};
        };
    }
}