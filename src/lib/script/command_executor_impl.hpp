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

// The implementation of the dot-commands, declared so that it can be written
// in parts. One file per group of commands, and the groups are the ones the
// user already has: the sections of ".help".
//
// Internal. The public header keeps its two forward declarations and pulls in
// nothing else, which is what keeps Cap'n Proto, the Wikidata importer and the
// Janet headers out of everything that merely wants to run a command.

#include "network/network_types.hpp"
#include "script/command_executor.hpp"
#include "string/node_to_string.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace zelph
{
    namespace io
    {
        class DataManager;
    }
    namespace network
    {
        class Reasoning;
        struct ProofNode;
    }
}

namespace zelph::console
{
    class CommandExecutor::Impl
    {
    public:
        // --- Implemented in command_executor.cpp ---

        Impl(network::Reasoning* n, ScriptEngine* se, std::shared_ptr<ReplState> rs, CommandExecutor::LineProcessor lp);
        void finish_input() const;
        void execute(const std::vector<std::string>& cmd, const std::vector<std::string>& sources);
        void register_commands();
        void require_full_graph_mode(const char* command_name) const;

        // --- Implemented in commands_session.cpp ---

#ifndef __EMSCRIPTEN__
        static std::vector<uint32_t> parse_chunk_index_list(const std::string& value, const std::string& label);
#endif
#ifndef __EMSCRIPTEN__
        static std::vector<uint64_t> parse_node_id_list(const std::string& value, const std::string& label);
#endif
        void import_file(const std::string& file, const std::vector<std::string>& args = {}) const;
#ifndef __EMSCRIPTEN__
        void cmd_load(const std::vector<std::string>& cmd);
#endif
#ifndef __EMSCRIPTEN__
        void cmd_load_partial(const std::vector<std::string>& cmd);
#endif
#ifndef __EMSCRIPTEN__
        void cmd_stat_file(const std::vector<std::string>& cmd);
#endif
#ifndef __EMSCRIPTEN__
        void cmd_index_file(const std::vector<std::string>& cmd);
#endif
        void cmd_licenses(const std::vector<std::string>& cmd);
#ifndef __EMSCRIPTEN__
        void cmd_save(const std::vector<std::string>& cmd);
#endif
#ifndef __EMSCRIPTEN__
        void cmd_save_predicates(const std::vector<std::string>& cmd);
#endif
        void cmd_import(const std::vector<std::string>& cmd) const;
        void cmd_provides(const std::vector<std::string>& cmd);

        // --- Implemented in commands_explore.cpp ---

        void display_node_details(network::Node nd, bool resolved_from_name, int depth = 1, int max_neighbors = string::default_display_max_neighbors) const;
        void generate_and_print_mermaid_link(network::Node nd, int depth, int max_neighbors, const std::unordered_set<network::Node>& exclude_nodes, bool dark_theme = true, bool horizontal_layout = true, bool use_subgraphs = true) const;
        void list_predicate_usage(size_t limit);
        void list_predicate_value_usage(const network::Node pred, size_t limit /*= 0*/);
        void cmd_node(const std::vector<std::string>& cmd);
        void cmd_list(const std::vector<std::string>& cmd);
        void cmd_clist(const std::vector<std::string>& cmd);
        void cmd_connections(const std::vector<std::string>& cmd, bool outgoing);
        void cmd_mermaid(const std::vector<std::string>& cmd);
        void cmd_list_predicate_usage(const std::vector<std::string>& cmd);
        void cmd_list_predicate_value_usage(const std::vector<std::string>& cmd);
        void cmd_stat(const std::vector<std::string>& cmd);

        // --- Implemented in command_patterns.cpp ---

        network::Node resolve_node(const std::string& arg, const std::string& lang) const;
        network::Node resolve_single_node(const std::string& arg, bool prioritize_id) const;
        std::string   try_parse_pattern(const std::string& pattern, std::string* why = nullptr) const;
        network::Node evaluate_pattern_read_only(const std::string& code);
        std::string   pattern_code(const std::vector<std::string>& parts, const std::size_t first, bool* has_collection = nullptr, std::string* why = nullptr) const;
        network::Node resolve_explain_pattern(const std::vector<std::string>& parts, const std::size_t first = 1);
        network::Node resolve_node_or_fact(const std::vector<std::string>& parts, size_t* count = nullptr);

        // --- Implemented in commands_help.cpp ---

        void cmd_help(const std::vector<std::string>& cmd);

        // --- Implemented in commands_names.cpp ---

        void cmd_lang(const std::vector<std::string>& cmd);
        void cmd_name(const std::vector<std::string>& cmd);
        void cmd_delname(const std::vector<std::string>& cmd);

        // --- Implemented in commands_edit.cpp ---

        void cmd_remove(const std::vector<std::string>& cmd);
        void cmd_prune(const std::vector<std::string>& cmd, bool facts_mode);
        void cmd_cleanup(const std::vector<std::string>& cmd);
        void cmd_new(const std::vector<std::string>& cmd);

        // --- Implemented in commands_inference.cpp ---

        void cmd_run(const std::vector<std::string>&);
        void cmd_run_once(const std::vector<std::string>&);
        void cmd_run_delta(const std::vector<std::string>&);
#ifndef __EMSCRIPTEN__
        void cmd_run_export(const std::vector<std::string>& cmd);
#endif
        void cmd_list_rules(const std::vector<std::string>& cmd);
        void cmd_remove_rules(const std::vector<std::string>& cmd);
        void cmd_auto_run(const std::vector<std::string>& cmd);
        void cmd_deductions(const std::vector<std::string>& cmd);

        // --- Implemented in commands_wikidata.cpp ---

#ifndef __EMSCRIPTEN__
        void cmd_wikidata_constraints(const std::vector<std::string>& cmd);
#endif
#ifndef __EMSCRIPTEN__
        void cmd_wikidata_qualifiers(const std::vector<std::string>& cmd);
#endif
#ifndef __EMSCRIPTEN__
        void cmd_export_wikidata(const std::vector<std::string>& cmd);
#endif

        // --- Implemented in commands_engine.cpp ---

        void cmd_log(const std::vector<std::string>& cmd);
        void cmd_log_janet(const std::vector<std::string>& cmd);
        void cmd_prof(const std::vector<std::string>& cmd);
        void cmd_parallel(const std::vector<std::string>& cmd);
        void cmd_anchors(const std::vector<std::string>& cmd);
        void cmd_semi_naive(const std::vector<std::string>& cmd);
        void cmd_fact_stores(const std::vector<std::string>& cmd);
        void cmd_contradiction_records(const std::vector<std::string>& cmd);

        // --- Implemented in commands_cluster.cpp ---

        void cmd_cluster(const std::vector<std::string>& cmd);
        void cmd_cluster_drop(const std::vector<std::string>& cmd);
        void cmd_cluster_merge(const std::vector<std::string>& cmd);
        void restore_cluster(const std::string& name) const;

        // --- Implemented in commands_explain.cpp ---

        void explain_collection_literal(const std::vector<std::string>& parts, const std::size_t first = 1) const;
        void cmd_explain(const std::vector<std::string>& cmd);
        void render_proof(const std::shared_ptr<network::ProofNode>& p, const std::string& indent, const bool last, std::set<network::Node>& printed, std::string& out) const;

    private:
        // --- Context References ---
        network::Reasoning*              _n;
        ScriptEngine*                    _script_engine;
        std::shared_ptr<io::DataManager> _data_manager;
        std::shared_ptr<ReplState>       _repl_state;
        CommandExecutor::LineProcessor   _process_line_callback;

        // --- Dispatch Map ---
        using Handler = std::function<void(const std::vector<std::string>&)>;
        std::map<std::string, Handler> _command_map;

        // The tokens of the command currently running, in parser form. Empty
        // when the caller did not record them (the Janet command handler passes
        // a ready-made vector), and the pattern builder then falls back to the
        // only other evidence there is, whitespace.
        std::vector<std::string> _sources;

        // A listing over a real dump is an operation of hours, and one that says
        // nothing until it is finished cannot be told apart from a stuck one --
        // which is exactly what happened on 14 August 2026, when
        // .list-predicate-value-usage P31 spent three and a half hours in silence
        // on the full Wikidata network. Below these sizes nothing is reported: a
        // listing on an ordinary network is over before a line could help, and the
        // transcripts of the documentation must not change for nothing.
        static constexpr size_t listing_report_threshold = 1000000;
        static constexpr size_t listing_report_step      = 1000000;
        static constexpr size_t predicate_report_step    = 1000;
    };
}
