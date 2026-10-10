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
#include <unordered_map>

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

    // A node within a pattern that represents what a firing builds in its
    // place (Reasoning::firing_stand_ins): the `term` contained in a bucket,
    // which constitutes a single node, or -- `term` 0 -- any term a firing
    // builds for a collection of the rule's text, a set constant included when
    // the collection is unable to determine its kind, and the node itself
    // where a firing might keep it.
    struct StandIn
    {
        bool set_constant{false};
        Node term{0};
    };
    using StandIns = std::unordered_map<Node, StandIn>;

    class Unification
    {
    public:
        // `candidates` refers to the method by which a candidate fact's
        // variables are read (Zelph::var_in_closure): either as rule text,
        // meaning a fact whose rule text holds a variable acts as a pattern
        // and matches nothing, or as a firing writes it -- what the check for
        // a prior witness of a fresh variable must locate
        // (Reasoning::consequences_already_exist).
        //
        // `stand_ins` names pattern nodes that represent what a firing builds
        // in their place (StandIn): the pattern functions as a consequence,
        // and the fact resulting from a firing holds a term where the rule
        // holds its own collection. A stand-in carrying a term is matched and
        // anchored on as that term; one lacking a term matches any term
        // produced by a firing, acts as no anchor, and leaves the caller to
        // decide by the prediction which match corresponds to the fact derived
        // by the firing. When no map is present, each node in the pattern
        // refers to itself. The map must persist beyond the lifetime of the
        // Unification.
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
            Node                              seed_predicate = 0,
            Zelph::VariableReading            candidates     = Zelph::VariableReading::Text,
            const StandIns*                   stand_ins      = nullptr);

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
            Node                              seed_predicate = 0,
            Zelph::VariableReading            candidates     = Zelph::VariableReading::Text,
            const StandIns*                   stand_ins      = nullptr);

        std::shared_ptr<Variables> Next();
        std::shared_ptr<Variables> Unequals();
        bool                       uses_parallel() const { return _use_parallel; }

        // Takes the relations and their associated candidate facts, arranged
        // according to the sequence of their identifiers; invoke this before
        // the first Next() call, for a sequential enumeration. If not invoked
        // beforehand, the order corresponds to the adjacency from which the
        // data is retrieved, which varies across parallel runs due to
        // differing construction orders -- the forward pass remains
        // unaffected, but a proof search that shows the first encountered
        // instantiation (Reasoning::explain) is sensitive to this. Incurs a
        // sorting cost for each candidate set before its first fact, unless
        // the order is already correct. The candidates are then maintained
        // within a list rather than a set: a set exceeding 128 elements
        // constructs a hash index, and sorting it built that index a second
        // time.
        void enumerate_by_id();

        // Takes, when the candidates are read at a bound node of the pattern
        // that is an atom, only the facts where that node plays the identical
        // role: the facts in which a bound subject functions as the subject,
        // and those in which a bound object functions as an object
        // (Zelph::collect_anchored_facts). All other facts cannot match:
        // taken, each is tried and dismissed, and retained while the
        // enumeration remains active (Reasoning::explain). A bound statement
        // keeps every candidate: it matches a wider statement in the other
        // role as well (see increment_fact_index).
        void skip_candidates_in_other_roles();

        // Next() returns nothing further: no match waits, the relation is
        // the last, and every remaining candidate it holds yields nothing
        // (yields_nothing). It inquires solely about the candidates already
        // contained in the enumeration and reads no adjacency. False when
        // operating in parallel mode.
        bool exhausted();

        // At most `at_most` candidate facts remain to be processed: the
        // relation is the final one, and so many follow the current entry.
        // False when operating in parallel mode, and before the first
        // Next().
        bool few_left(std::size_t at_most);

        // A match of the fact Next() returned last waits (Next() will
        // deliver it next).
        bool has_queued();

        // The relation Next() reads its candidates from now, 0 when there is
        // none.
        Node current_relation() const;

        // Resumes from where a prior enumeration of the identical pattern
        // under the same bindings left off: following `fact`, the candidate
        // from which it last yielded a match, within `relation`. Call it
        // after enumerate_by_id and before the first Next(); the order of
        // identifiers is what makes the position one to return to. The
        // candidates are re-read.
        void start_after(Node relation, Node fact);

        // The adjacency entries that have been read so far to find the
        // candidate facts: the anchor's complete adjacency, the relation's
        // associated facts, and the partial anchor's climb. A caller that
        // bounds its work charges these elements, and the candidates that
        // were tried without success (Reasoning::explain).
        std::size_t scanned() const { return _scanned; }

        // The candidate facts tried so far that failed to match: their
        // subject, their objects, or their number did not align with the
        // pattern. A rule's ground pattern and a refuted fact are skipped
        // without being tested.
        std::size_t fruitless() const { return _fruitless; }

        // The fact the match Next() returned last was read from, or 0 when
        // operating in parallel mode, during which multiple facts contribute
        // matches simultaneously. The bindings do not indicate the
        // originating fact: `(X p Y)` binds Y to b both from `a p b` and from
        // `a p b c`.
        Node matched_fact() const { return !_use_parallel && _fact_index_initialized ? *_fact_index : 0; }

        void wait_for_completion()
        {
            if (!_use_parallel) return;

            std::unique_lock<std::mutex> lock(_queue_mtx);
            _queue_cv.wait(lock, [this]
                           { return _active_tasks.load() == 0; });
        }

    private:
        bool increment_fact_index();
        // A node within the pattern that represents any term (see
        // the constructors).
        bool matches_any(Node pattern) const;

        // Next() does not accept a match from `fact` within the current
        // relation: it skips a rule's ground pattern and a refuted fact
        // untried, it moves past a `=>` reading where the fact holds a
        // variable in its rule text (as verified by is_rule_text in
        // unification.cpp), and extract_bindings rejects a reading if its
        // subject or object includes a variable. A rule's own condition
        // qualifies as such a fact, and when it names the node at which the
        // candidates are read, it constitutes one of them.
        bool                                    yields_nothing(Node fact) const;
        std::size_t                             snapshot_size() const { return _listed ? _listed_facts.size() : _facts_snapshot.size(); }
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

        // How a candidate's variables are read (see the constructors).
        const Zelph::VariableReading _candidates;

        // See the constructors; null: each node in the pattern stands for
        // itself.
        const StandIns* const _stand_ins;

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
        adjacency_set::iterator _facts_end;
        adjacency_set           _facts_snapshot;
        std::vector<Node>       _listed_facts; // the snapshot, where it is taken by id (see enumerate_by_id)
        bool                    _listed{false};
        bool                    _fact_index_initialized{false};
        bool                    _by_id{false};       // see enumerate_by_id
        bool                    _role_filter{false}; // see skip_candidates_in_other_roles
        std::size_t             _scanned{0};         // see scanned
        std::size_t             _fruitless{0};       // see fruitless
        Node                    _resume_relation{0}; // see start_after
        Node                    _resume_fact{0};
        bool                    _snapshot_prefiltered{false}; // snapshot provably contains no
                                                              // facts that use the relation as
                                                              // their SUBJECT (anchored/partial
                                                              // paths); the per-fact
                                                              // has_left_edge skip in the scan
                                                              // loop is redundant then
    };
}
