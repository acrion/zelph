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

#include "concurrency/thread_pool.hpp"
#include "reasoning_profiler.hpp"
#include "zelph.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

namespace zelph::network
{
    // Rule-static decomposition of a leaf condition's pattern: everything
    // the Unification constructor derives from the condition node ALONE,
    // independent of current bindings -- the relation/subject/objects
    // reading plus the subject predicate hint. Built once per (rule, leaf)
    // by the semi-naive index and reused for every seed instance;
    // recomputing it per instance made the constructor the largest
    // self-time item of the Jacobian profiles (8.6%, ~412k constructions
    // per phase, ~72% of them semi-naive seeds).
    //
    // Binding-DEPENDENT work stays per-instance in the constructor:
    // relation-variable resolution, bound-pattern grounding, boundness
    // analysis, partial-pattern anchoring, snapshot launches, and the
    // Unequal side effects.
    //
    // relation == 0 means the decomposition failed; the constructor then
    // logs the fallback and leaves the relation list empty, as before.
    struct PatternInfo
    {
        Node          condition{0};
        Node          relation{0}; // raw pattern predicate; may be a variable or a composite pattern
        Node          subject{0};
        adjacency_set objects;
        Node          subject_pred_hint{0};
    };

    ZELPH_EXPORT PatternInfo build_pattern_info(const Zelph* n, Node condition, int log_depth);

    class Unification
    {
    public:
        Unification(
            Zelph*                            n,
            Node                              condition,
            Node                              parent,
            const std::shared_ptr<Variables>& variables,
            const std::shared_ptr<Variables>& unequals,
            concurrency::ThreadPool*          pool,
            int                               log_depth,
            ReasoningProfiler*                profiler,
            Node                              seed_fact      = 0,
            Node                              seed_predicate = 0);

        // Hoisted-pattern overload (semi-naive seeding): consumes a
        // precomputed rule-static decomposition instead of re-deriving it
        // from the condition node. The condition-taking constructor above
        // DELEGATES here, so both entry points share one body -- path
        // divergence is structurally impossible. By-value sink parameter:
        // callers with a cached PatternInfo pay exactly the one objects
        // copy the old constructor paid; the delegating path moves.
        Unification(
            Zelph*                            n,
            PatternInfo                       pattern,
            Node                              parent,
            const std::shared_ptr<Variables>& variables,
            const std::shared_ptr<Variables>& unequals,
            concurrency::ThreadPool*          pool,
            int                               log_depth,
            ReasoningProfiler*                profiler,
            Node                              seed_fact      = 0,
            Node                              seed_predicate = 0);

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
        bool                                    increment_fact_index();
        std::vector<std::shared_ptr<Variables>> extract_bindings(const Node subject, const adjacency_set& objects, const Node relation, const int depth) const;

        Zelph* const               _n;
        Node                       _parent;
        std::shared_ptr<Variables> _variables;
        std::shared_ptr<Variables> _unequals;
        adjacency_set              _relation_list;
        Node                       _relation_variable{};
        Node                       _relation_pattern{};
        Node                       _subject{};
        adjacency_set              _objects;
        Node                       _subject_pred_hint{};
        Node                       _subject_grounded{}; // concrete fact node the subject pattern
                                                        // resolves to under current bindings
                                                        // (bound-pattern grounding); 0 = not groundable

        // // Partial-pattern anchoring (see unification.cpp): candidate facts
        // precomputed by climbing from a concrete inner node of a partially
        // bound pattern. Valid only for the single fixed relation; consumed
        // once by increment_fact_index.
        adjacency_set _partial_snapshot;
        bool          _partial_snapshot_valid{false};

        Node                     _seed_fact{};      // semi-naive seed: the single candidate fact (0 = normal scan mode)
        Node                     _seed_predicate{}; // its relation type, known at creation time
        int                      _log_depth{};
        ReasoningProfiler* const _prof; // nullptr = profiling disabled
        Node                     _current_rel_ctx{};

        // Parallel mode
        concurrency::ThreadPool*               _pool{nullptr};
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
        bool                    _snapshot_prefiltered{false}; // snapshot provably contains no
                                                              // facts that use the relation as
                                                              // their SUBJECT (anchored/partial
                                                              // paths); the per-fact
                                                              // has_left_edge skip in the scan
                                                              // loop is redundant then
    };
}
