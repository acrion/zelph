/*
Copyright (c) 2025, 2026 acrion innovations GmbH
Authors: Stefan Zipproth, s.zipproth@acrion.ch
...
*/

#pragma once

#include "markdown.hpp"
#include "stopwatch.hpp"
#include "thread_pool.hpp"
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
        Node                               node;
        std::shared_ptr<std::vector<Node>> conditions;
        size_t                             index;
        std::shared_ptr<Variables>         variables{std::make_shared<Variables>()};
        std::shared_ptr<Variables>         unequals{std::make_shared<Variables>()};
    };

    struct ReasoningContext
    {
        Node                 current_condition{0};
        std::vector<RulePos> next;
        adjacency_set        rule_deductions;
    };

    class ZELPH_EXPORT Reasoning : public Zelph
    {
    public:
        explicit Reasoning(const std::unordered_map<network::Node, std::wstring>& core_node_names, const std::function<void(const std::wstring&, const bool)>&);
        void run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition);
        void apply_rule(const network::Node& rule, network::Node condition);
        void set_markdown_subdir(const std::string& subdir);
        void prune_facts(Node pattern, size_t& removed_count);
        void prune_nodes(Node pattern, size_t& removed_facts, size_t& removed_nodes);
        void purge_unused_predicates(size_t& removed_facts, size_t& removed_predicates);

    private:
        void                               evaluate(RulePos rule, ReasoningContext& ctx);
        bool                               contradicts(const Variables& variables, const Variables& unequals) const;
        void                               deduce(const Variables& variables, Node parent, ReasoningContext& ctx);
        std::shared_ptr<std::vector<Node>> optimize_order(const adjacency_set& conditions, const Variables& current_vars);

        std::atomic<bool>                   _done{false};
        std::unique_ptr<wikidata::Markdown> _markdown;
        std::atomic<uint64_t>               _running{0};
        bool                                _print_deductions{true};
        bool                                _generate_markdown{true};
        std::atomic<bool>                   _contradiction{false};
        StopWatch                           _stop_watch;
        std::atomic<size_t>                 _skipped{0};
        std::mutex                          _mtx_output;
        std::mutex                          _mtx_network;
        int                                 _total_matches{0};
        int                                 _total_contradictions{0};
        std::unique_ptr<ThreadPool>         _pool;
        std::string                         _markdown_subdir;
        bool                                _prune_mode{false};
        bool                                _prune_nodes_mode{false};
        std::unordered_set<Node>            _facts_to_prune;
        std::unordered_set<Node>            _nodes_to_prune;
    };
}