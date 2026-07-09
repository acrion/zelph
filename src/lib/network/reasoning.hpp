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

#include "chrono/stopwatch.hpp"
#include "concurrency/thread_pool.hpp"
#include "io/markdown.hpp"
#include "io/output.hpp"
#include "network_types.hpp"
#include "neural.hpp"
#include "reasoning_profiler.hpp"
#include "zelph.hpp"

#include <zelph_export.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    struct RulePos
    {
        Node                                      node;
        std::shared_ptr<std::vector<Node>>        conditions;
        size_t                                    index;
        std::shared_ptr<Variables>                variables{std::make_shared<Variables>()};
        std::shared_ptr<Variables>                unequals{std::make_shared<Variables>()};
        std::shared_ptr<std::unordered_set<Node>> excluded{std::make_shared<std::unordered_set<Node>>()};

        // Accumulated confidence of ≈ conditions along this binding path;
        // stays 1.0 when no neural condition fired. Propagated into deduce()
        // and stored as the deduced fact's probability.
        double confidence{1.0};
    };

    struct ReasoningContext
    {
        Node                 current_condition{0};
        std::vector<RulePos> next;
        adjacency_set        rule_deductions;
    };

    // --- Free helper functions (implemented in reasoning.cpp) ---

    // Recursively substitute variables in a fact pattern to produce a concrete fact.
    // Used by evaluate and deduce to instantiate rule patterns with current bindings.
    Node instantiate_fact(Zelph* z, Node pattern, const Variables& variables, int depth, std::vector<Node>& history);

    // Recursively collect all variable nodes from a fact pattern.
    // Used to detect "fresh variables" that appear only in rule consequences.
    void collect_variables(Zelph* z, Node pattern, std::unordered_set<Node>& vars, int depth, std::vector<Node>& history);

    class ZELPH_EXPORT Reasoning : public Zelph
    {
    public:
        // --- Implemented in reasoning.cpp (orchestration) ---

        explicit Reasoning(const io::OutputHandler& output = io::default_output_handler);
        void set_markdown_subdir(const std::string& subdir);
        void set_query_collector(std::vector<std::shared_ptr<Variables>>* collector);
        void run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition, const bool silent = false);
        void apply_rule(const network::Node& rule, network::Node condition);
        void profiler_reset_epoch()
        {
            _prof.reset_epoch();
            _nn_cache.clear();
        }

        // --- Implemented in reasoning_pruning.cpp ---

        void prune_facts(Node pattern, size_t& removed_count);
        void prune_nodes(Node pattern, size_t& removed_facts, size_t& removed_nodes);
        void purge_unused_predicates(size_t& removed_facts, size_t& removed_predicates);

        // --- Implemented in reasoning_seminaive.cpp ---

        void set_seminaive(bool on);
        bool seminaive() const;
        void set_seminaive_check(bool on);
        bool seminaive_check() const;

    private:
        // --- Implemented in reasoning.cpp (orchestration) ---

        std::shared_ptr<std::vector<Node>> optimize_order(const adjacency_set& conditions, const Variables& current_vars, int depth);
        static bool                        contradicts(const Variables& variables, const Variables& unequals);

        // --- Implemented in reasoning_evaluate.cpp ---

        void evaluate(RulePos rule, ReasoningContext& ctx, int depth);
        bool is_negated_condition(Node condition, int depth);
        bool condition_contains_negation(Node condition, int depth);

        // --- Implemented in reasoning_deduce.cpp ---

        void deduce(const Variables& variables, Node parent, const int depth, ReasoningContext& ctx, double confidence);
        bool consequences_already_exist(const Variables&     condition_bindings,
                                        const adjacency_set& deductions,
                                        Node                 parent,
                                        const int            depth);

        // --- Implemented in reasoning_neural.cpp ---
        const NeuralNet* compiled_net(Node net_node, int depth);
        void             evaluate_neural(Node condition, const RulePos& rule, ReasoningContext& ctx, int depth);
        void             proceed_after_condition(const RulePos& rule, ReasoningContext& ctx, int depth, std::shared_ptr<Variables> vars, std::shared_ptr<Variables> uneqs, double confidence);

        // --- Implemented in reasoning_seminaive.cpp ---

        // Delta-driven fixpoint loop (semi-naive evaluation). Returns the
        // number of safety-net violations found (always 0 unless
        // _seminaive_check is active and delta seeding missed a derivation).
        uint64_t run_fixpoint_seminaive(bool silent);

        // --- Members ---

        std::atomic<bool>                        _done{false};
        std::unique_ptr<io::Markdown>            _markdown;
        std::atomic<uint64_t>                    _running{0};
        bool                                     _print_deductions{true};
        bool                                     _generate_markdown{true};
        std::atomic<bool>                        _contradiction{false};
        chrono::StopWatch                        _stop_watch;
        std::atomic<size_t>                      _skipped{0};
        std::mutex                               _mtx_output;
        std::mutex                               _mtx_network;
        std::atomic<int>                         _total_matches{0};
        std::atomic<int>                         _total_contradictions{0};
        std::unique_ptr<concurrency::ThreadPool> _pool;
        std::string                              _markdown_subdir;
        bool                                     _prune_mode{false};
        bool                                     _prune_nodes_mode{false};
        std::unordered_set<Node>                 _facts_to_prune;
        std::unordered_set<Node>                 _nodes_to_prune;
        std::vector<std::shared_ptr<Variables>>* _query_results{nullptr};
        ReasoningProfiler                        _prof;

        // --- Neural (≈) support ---
        Node                                       _nn_pred{0};        // node named "nn" in lang "zelph", 0 = feature inactive
        Node                                       _nn_layers_pred{0}; // node named "nn-layers" in lang "zelph"
        std::map<Node, std::unique_ptr<NeuralNet>> _nn_cache;          // compiled nets, cleared per epoch

        bool _seminaive{true};
        bool _seminaive_check{false};
    };
}