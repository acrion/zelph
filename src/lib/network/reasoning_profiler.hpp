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

#include "string/string_utils.hpp"
#include "zelph.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace zelph::network
{
    struct ReasoningProfiler
    {
        explicit ReasoningProfiler(const Zelph* const zelph) : _zelph{zelph} {}

        const Zelph* const _zelph;

        std::atomic<uint64_t>                 epoch_id{0};
        std::chrono::steady_clock::time_point epoch_start{std::chrono::steady_clock::now()};

        // --- Reasoning-level counters ---
        std::atomic<uint64_t> apply_rule_calls{0};
        std::atomic<uint64_t> evaluate_calls{0};
        std::atomic<uint64_t> conjunction_sets{0};
        std::atomic<uint64_t> leaf_conditions{0};
        std::atomic<uint64_t> optimize_order_calls{0};

        std::atomic<uint64_t> negated_conditions{0};
        std::atomic<uint64_t> negation_success{0};
        std::atomic<uint64_t> negation_fail{0};
        std::atomic<uint64_t> neg_complement_subjects_tested{0};

        std::atomic<uint64_t> deduce_calls{0};
        std::atomic<uint64_t> termination_guard_checks{0};
        std::atomic<uint64_t> termination_guard_skips{0};

        std::atomic<uint64_t> fresh_vars_total{0};
        std::atomic<uint64_t> fresh_nodes_created{0};

        std::atomic<uint64_t> check_fact_known{0};
        std::atomic<uint64_t> check_fact_new{0};
        std::atomic<uint64_t> check_fact_wrong{0};

        std::atomic<uint64_t> facts_created{0};

        // --- Unification-level counters ---
        std::atomic<uint64_t> unification_instances{0};
        std::atomic<uint64_t> unification_parallel_instances{0};
        std::atomic<uint64_t> unification_next_calls{0};

        std::atomic<uint64_t> relation_snapshots{0}; // any snapshot (subject-driven/object-driven/full)
        std::atomic<uint64_t> snapshot_subject_driven{0};
        std::atomic<uint64_t> snapshot_object_driven{0};
        std::atomic<uint64_t> snapshot_full_relation{0};
        std::atomic<uint64_t> snapshot_facts_total{0}; // sum of snapshot sizes

        std::atomic<uint64_t> facts_scanned_sequential{0}; // how many fact nodes were iterated
        std::atomic<uint64_t> facts_scanned_parallel{0};

        std::atomic<uint64_t> get_fact_structures_calls{0};
        std::atomic<uint64_t> structures_total{0};

        std::atomic<uint64_t> extract_calls{0};
        std::atomic<uint64_t> extract_success{0};
        std::atomic<uint64_t> extract_fail_subject{0};
        std::atomic<uint64_t> extract_fail_object{0};
        std::atomic<uint64_t> template_rejects{0};

        // unify_nodes() inner recursion
        std::atomic<uint64_t> unify_calls{0};
        std::atomic<uint64_t> unify_cycle_hits{0};
        std::atomic<uint64_t> unify_identity_hits{0};

        std::atomic<uint64_t> unify_var_seen{0};
        std::atomic<uint64_t> unify_var_bound_new{0};
        std::atomic<uint64_t> unify_var_local_recurse{0};
        std::atomic<uint64_t> unify_var_global_recurse{0};

        std::atomic<uint64_t> unify_rule_atom_fail{0};
        std::atomic<uint64_t> unify_graph_atom_fail{0};

        std::atomic<uint64_t> unify_struct_pair_attempts{0};
        std::atomic<uint64_t> unify_struct_success{0};

        std::atomic<uint64_t> unify_object_try{0};
        std::atomic<uint64_t> unify_object_success{0};

        std::atomic<uint64_t> max_reasoning_depth{0};
        std::atomic<uint64_t> max_unify_depth{0};

        // Context aggregation (coarse-grained; guarded by mutex)
        std::mutex                         _mtx;
        std::unordered_map<Node, uint64_t> rel_scanned_facts;  // per relation: candidate facts scanned (snapshot size or actual scan)
        std::unordered_map<Node, uint64_t> rel_matches;        // per relation: extract_success
        std::unordered_map<Node, uint64_t> rule_applied;       // apply_rule per rule node
        std::unordered_map<Node, uint64_t> rule_facts_created; // created facts per rule node

        static inline void atomic_max(std::atomic<uint64_t>& target, uint64_t v)
        {
            uint64_t cur = target.load(std::memory_order_relaxed);
            while (cur < v && !target.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
        }

        void reset_epoch()
        {
            // Reset only if logging enabled (otherwise keep overhead minimal).
            if (_zelph->logging_active()) return;

            epoch_id.fetch_add(1, std::memory_order_relaxed);
            epoch_start = std::chrono::steady_clock::now();

            // atomics
#define RZ(x) x.store(0, std::memory_order_relaxed)
            RZ(apply_rule_calls);
            RZ(evaluate_calls);
            RZ(conjunction_sets);
            RZ(leaf_conditions);
            RZ(optimize_order_calls);
            RZ(negated_conditions);
            RZ(negation_success);
            RZ(negation_fail);
            RZ(neg_complement_subjects_tested);
            RZ(deduce_calls);
            RZ(termination_guard_checks);
            RZ(termination_guard_skips);
            RZ(fresh_vars_total);
            RZ(fresh_nodes_created);
            RZ(check_fact_known);
            RZ(check_fact_new);
            RZ(check_fact_wrong);
            RZ(facts_created);

            RZ(unification_instances);
            RZ(unification_parallel_instances);
            RZ(unification_next_calls);
            RZ(relation_snapshots);
            RZ(snapshot_subject_driven);
            RZ(snapshot_object_driven);
            RZ(snapshot_full_relation);
            RZ(snapshot_facts_total);
            RZ(facts_scanned_sequential);
            RZ(facts_scanned_parallel);
            RZ(get_fact_structures_calls);
            RZ(structures_total);
            RZ(extract_calls);
            RZ(extract_success);
            RZ(extract_fail_subject);
            RZ(extract_fail_object);
            RZ(template_rejects);
            RZ(unify_calls);
            RZ(unify_cycle_hits);
            RZ(unify_identity_hits);
            RZ(unify_var_seen);
            RZ(unify_var_bound_new);
            RZ(unify_var_local_recurse);
            RZ(unify_var_global_recurse);
            RZ(unify_rule_atom_fail);
            RZ(unify_graph_atom_fail);
            RZ(unify_struct_pair_attempts);
            RZ(unify_struct_success);
            RZ(unify_object_try);
            RZ(unify_object_success);
            RZ(max_reasoning_depth);
            RZ(max_unify_depth);
#undef RZ

            // maps
            {
                std::lock_guard<std::mutex> lk(_mtx);
                rel_scanned_facts.clear();
                rel_matches.clear();
                rule_applied.clear();
                rule_facts_created.clear();
            }
        }

        void note_rule_applied(Node rule)
        {
            if (!_zelph->logging_active()) return;
            apply_rule_calls.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(_mtx);
            rule_applied[rule] += 1;
        }

        void note_relation_scan(Node rel, uint64_t facts)
        {
            if (!_zelph->logging_active()) return;
            std::lock_guard<std::mutex> lk(_mtx);
            rel_scanned_facts[rel] += facts;
        }

        void note_relation_match(Node rel, uint64_t n = 1)
        {
            if (!_zelph->logging_active()) return;
            std::lock_guard<std::mutex> lk(_mtx);
            rel_matches[rel] += n;
        }

        void note_rule_created_fact(Node rule)
        {
            if (!_zelph->logging_active()) return;
            std::lock_guard<std::mutex> lk(_mtx);
            rule_facts_created[rule] += 1;
        }

        void log_after_deduction(Node rule_node, Node deduced_fact, int depth)
        {
            if (!_zelph->logging_active()) return;

            using namespace std::chrono;
            auto   now = steady_clock::now();
            double ms  = duration<double, std::milli>(now - epoch_start).count();

            auto load = [](const std::atomic<uint64_t>& a)
            { return a.load(std::memory_order_relaxed); };

            // Top relations by scanned facts
            std::vector<std::pair<Node, uint64_t>> top_rel;
            {
                std::lock_guard<std::mutex> lk(_mtx);
                top_rel.assign(rel_scanned_facts.begin(), rel_scanned_facts.end());
            }
            std::sort(top_rel.begin(), top_rel.end(), [](auto& a, auto& b)
                      { return a.second > b.second; });
            if (top_rel.size() > 5) top_rel.resize(5);

            auto rel_name = [&](Node r) -> std::string
            {
                if (!r) return "0";
                std::wstring wn = _zelph->get_name(r, _zelph->lang(), true);
                if (!wn.empty()) return zelph::string::unicode::to_utf8(wn);
                return _zelph->format(r);
            };

            auto fact_str = [&](Node f) -> std::string
            {
                if (!f) return "0";
                return _zelph->format(f);
            };

            std::ostringstream oss;
            oss << "\n[prof] epoch=" << load(epoch_id)
                << " +" << (ms / 1000.0) << "s"
                << " after " << fact_str(deduced_fact)
                << " (rule=" << fact_str(rule_node) << ", depth=" << depth << ")\n";

            oss << "  rules_applied=" << load(apply_rule_calls)
                << " deduce_calls=" << load(deduce_calls)
                << " facts_created=" << load(facts_created) << "\n";

            oss << "  unification: instances=" << load(unification_instances)
                << " parallel=" << load(unification_parallel_instances)
                << " next()=" << load(unification_next_calls)
                << " snapshots=" << load(relation_snapshots)
                << " snapshot_facts=" << load(snapshot_facts_total)
                << " scanned(seq)=" << load(facts_scanned_sequential)
                << " scanned(par)=" << load(facts_scanned_parallel) << "\n";

            oss << "  unify(): calls=" << load(unify_calls)
                << " cycle=" << load(unify_cycle_hits)
                << " id=" << load(unify_identity_hits)
                << " var_seen=" << load(unify_var_seen)
                << " var_new=" << load(unify_var_bound_new)
                << " struct_pairs=" << load(unify_struct_pair_attempts)
                << " struct_ok=" << load(unify_struct_success)
                << " obj_try=" << load(unify_object_try)
                << " obj_ok=" << load(unify_object_success) << "\n";

            oss << "  get_fact_structures: calls=" << load(get_fact_structures_calls)
                << " total_structs=" << load(structures_total)
                << " extract: calls=" << load(extract_calls)
                << " ok=" << load(extract_success)
                << " failS=" << load(extract_fail_subject)
                << " failO=" << load(extract_fail_object)
                << " template_rejects=" << load(template_rejects) << "\n";

            oss << "  negation: cond=" << load(negated_conditions)
                << " ok=" << load(negation_success)
                << " fail=" << load(negation_fail)
                << " complement_subjects=" << load(neg_complement_subjects_tested) << "\n";

            oss << "  termination_guard: checks=" << load(termination_guard_checks)
                << " skips=" << load(termination_guard_skips)
                << " fresh_vars=" << load(fresh_vars_total)
                << " fresh_nodes=" << load(fresh_nodes_created) << "\n";

            oss << "  max_depth: reasoning=" << load(max_reasoning_depth)
                << " unify=" << load(max_unify_depth) << "\n";

            if (!top_rel.empty())
            {
                oss << "  top_relations_by_scan: ";
                for (size_t i = 0; i < top_rel.size(); ++i)
                {
                    if (i) oss << " | ";
                    oss << rel_name(top_rel[i].first) << "=" << top_rel[i].second;
                }
                oss << "\n";
            }

            _zelph->diagnostic_stream() << oss.str() << std::flush;
        }
    };
}
