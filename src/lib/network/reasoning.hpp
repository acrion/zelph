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

    // --- Proof reconstruction (reasoning_explain.cpp) --------------------------
    // Rebuild a justification for an asserted fact from the saturated graph.
    // Nothing is tracked during inference: after quiescence, every derived
    // fact has at least one rule instantiation whose consequence unifies
    // with it and whose conditions are all present -- this backward search
    // finds one, using the same unification machinery that runs forward.
    // Read-only: no graph structure is created. Cost is irrelevant here by
    // design; it buys a zero-overhead forward pass.
    struct ProofNode
    {
        enum class Status
        {
            Derived,   // justified by `rule` via `premises` and `absent`
            Axiom,     // asserted, and no rule consequence unifies: an input fact
            Unfounded, // asserted, rule consequences unify, but no acyclic
                       // instantiation verifies against the CURRENT graph
                       // (e.g. a NAF premise that has since become true)
            Truncated  // not expanded: depth limit reached
        };

        Node                                    fact   = 0;
        Node                                    rule   = 0; // the => fact, Derived only
        Status                                  status = Status::Axiom;
        std::vector<std::shared_ptr<ProofNode>> premises; // positive conditions
        std::vector<Node>                       absent;   // NAF conditions, verified absent NOW
    };

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

        // On-demand profiler summary (command .prof): the counter block
        // plus top-N sections (relations by scan/match, rules by
        // application/created facts). reset_after additionally zeroes the
        // counters, starting a fresh measurement window (.prof reset).
        void profiler_dump(bool reset_after = false);

        // --- Deduction focus (implemented in reasoning.cpp) ---

        // Capture nodes materialized by user input (fact nodes plus their
        // subjects and objects) into the input-focus set, via the fact
        // creation observer. Idempotent; run() ends the capture itself, so
        // the observer never overlaps with the semi-naive delta observer.
        void begin_input_capture();
        // When on, deduction printing is restricted to deductions whose
        // subject or rule is in the input-focus set ("focus mode").
        void set_deduction_filter(bool on);
        // Temporarily suppress input capture (imports): begin_input_capture
        // becomes a no-op while suppressed, and an active capture is closed
        // WITHOUT contributing to the focus set -- imported statements are
        // not "entered by the user".
        void suppress_input_capture(bool on);
        // Reset the accumulated focus anchors (mode switch; .reset gets a fresh Reasoning instance anyway).
        void clear_input_focus();

        // --- Implemented in reasoning_pruning.cpp ---

        void prune_facts(Node pattern, size_t& removed_count);
        void prune_nodes(Node pattern, size_t& removed_facts, size_t& removed_nodes);
        void purge_unused_predicates(size_t& removed_facts, size_t& removed_predicates);

        // --- Implemented in reasoning_seminaive.cpp ---

        void set_seminaive(bool on);
        bool seminaive() const;
        void set_seminaive_check(bool on);
        bool seminaive_check() const;

        // max_depth 0 = unlimited (terminates via path exclusion and memoization;
        // hash-consing makes shared subterms shared subproofs, so the result is
        // a DAG: identical facts share one ProofNode instance).
        std::shared_ptr<ProofNode> explain(Node fact, std::size_t max_depth) const;

    private:
        // --- Implemented in reasoning.cpp (orchestration) ---

        std::shared_ptr<std::vector<Node>> optimize_order(const adjacency_set& conditions, const Variables& current_vars, int depth);
        static bool                        contradicts(const Variables& variables, const Variables& unequals);
        void                               end_input_capture();

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
        std::unordered_set<Node>                 _input_captured; // raw fact nodes materialized while parsing input
        std::unordered_set<Node>                 _input_focus;    // reduced focus set: top-level inputs + their components
        bool                                     _deduction_filter{false};
        bool                                     _capturing{false};
        int                                      _capture_suppress_depth{0}; // > 0: input capture suppressed (nested imports)

        // --- Neural (≈) support ---
        Node                                       _nn_pred{0};        // node named "nn" in lang "zelph", 0 = feature inactive
        Node                                       _nn_layers_pred{0}; // node named "nn-layers" in lang "zelph"
        std::map<Node, std::unique_ptr<NeuralNet>> _nn_cache;          // compiled nets, cleared per epoch

        bool _seminaive{true};
        bool _seminaive_check{false};
    };
}