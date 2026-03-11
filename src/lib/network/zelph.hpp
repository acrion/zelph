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

#include "answer.hpp"
#include "fact_structure_types.hpp"
#include "io/output.hpp"
#include "network.hpp"

#include <zelph_export.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    using name_of_node_map = ankerl::unordered_dense::map<Node, std::string_view>;
    using node_of_name_map = ankerl::unordered_dense::map<std::string_view, Node>;

    // The core semantic network engine. It manages the in-memory graph structure (nodes, edges),
    // provides low-level API for graph manipulation, and handles raw binary serialization (I/O)
    // of the network state via load_from_file/save_to_file. It is agnostic to the semantic meaning
    // or source format of the data.
    class ZELPH_EXPORT Zelph
    {
    public:
        explicit Zelph(const io::OutputHandler& output = io::default_output_handler);
        ~Zelph();

        struct FactComponents
        {
            Node          subject   = 0;
            Node          predicate = 0;
            adjacency_set objects;
        };

        class AllNodeView
        {
        private:
            const adjacency_map& _left_ref;

        public:
            explicit AllNodeView(const adjacency_map& left) : _left_ref(left) {}
            auto begin() const { return _left_ref.begin(); }
            auto end() const { return _left_ref.end(); }
            // Usage: for (auto it = view.begin(); it != view.end(); ++it) { Node nd = it->first; }
        };

        class LangNodeView
        {
        private:
            const node_of_name_map& _rev_map;

        public:
            explicit LangNodeView(const node_of_name_map& rev) : _rev_map(rev) {}
            auto begin() const { return _rev_map.begin(); }
            auto end() const { return _rev_map.end(); }
            // Usage: for (auto it = view.begin(); it != view.end(); ++it) { Node nd = it->second; }
        };

        // --- Implemented in zelph.cpp (core graph operations) ---

        static std::string   get_version();
        Node                 var() const;
        void                 set_lang(const std::string& lang);
        std::string          get_lang() const { return _lang; }
        std::string          lang() const { return _lang; }
        Node                 node(const std::string& name, std::string lang = "");
        bool                 exists(uint64_t nd) const;
        adjacency_set        get_sources(Node relationType, Node target, bool exclude_vars = false) const;
        adjacency_set        filter(const adjacency_set& source, Node target) const;
        adjacency_set        filter(Node fact, Node relationType, Node target) const;
        static adjacency_set filter(const adjacency_set& source, const std::function<bool(const Node nd)>& f);
        adjacency_set        get_left(const Node b) const;
        adjacency_set        get_right(const Node b) const;
        bool                 has_left_edge(Node b, Node a) const;
        bool                 has_right_edge(Node a, Node b) const;
        static Node          create_hash(const adjacency_set& vec);
        static bool          is_hash(Node a);
        static bool          is_var(Node a);
        Answer               check_fact(Node subject, Node predicate, const adjacency_set& objects) const;
        Node                 fact(Node subject, Node predicate, const adjacency_set& objects, long double probability = 1);
        Node                 fact_import_trusted_single_object(Node subject, Node predicate, Node object) const;
        Node                 list(const std::vector<Node>& elements);
        Node                 list(const std::vector<std::string>& elements);
        Node                 set(const std::unordered_set<Node>& elements);
        Node                 parse_fact(Node rule, adjacency_set& deductions, Node parent = 0) const;
        Node                 parse_relation(const Node rule) const;
        Node                 count() const;
        AllNodeView          get_all_nodes_view() const;
        LangNodeView         get_lang_nodes_view(const std::string& lang) const;
        bool                 try_get_fact_structures_cached(Node fact, std::vector<FactStructure>& out) const;
        void                 store_fact_structures_cached(Node fact, const std::vector<FactStructure>& value) const;
        void                 invalidate_fact_structures_cache() const noexcept;
        FactComponents       extract_fact_components(Node relation) const;
        void                 set_output_handler(io::OutputHandler output) const;
        io::OutputHandler    get_output_handler() const;
        void                 emit(io::OutputChannel channel, const std::string& text, bool newline = true) const;
        void                 out(const std::string&, bool newline = true) const;
        void                 error(const std::string&, bool newline = true) const;
        void                 diagnostic(const std::string&, bool newline = true) const;
        void                 prompt(const std::string&, bool newline = false) const;
        io::OutputStream     out_stream() const;
        io::OutputStream     diagnostic_stream() const;
        io::OutputStream     error_stream() const;
        io::OutputStream     prompt_stream() const;
        void                 set_logging(int max_depth) const;
        bool                 should_log(int depth) const;
        bool                 logging_active() const;
        void                 log(int depth, const std::string& category, const std::string& message) const;
        bool                 use_parallel() const { return _use_parallel; }
        void                 toggle_parallel() { _use_parallel = !_use_parallel; }

        // --- Implemented in zelph_names.cpp (name management) ---

        void                     set_name(Node node, const std::string& name, std::string lang, bool merge_on_conflict);
        Node                     set_name(const std::string& name_in_current_lang, const std::string& name_in_given_lang, std::string lang);
        std::string              get_name(const Node node, std::string lang = "", const bool fallback = false) const;
        std::string              get_formatted_name(Node node, const std::string& lang) const;
        bool                     has_name(Node node, const std::string& lang) const;
        void                     remove_name(Node node, std::string lang = "");
        void                     unset_name(Node node, std::string lang = "");
        Node                     get_node(const std::string& name, std::string lang = "") const;
        void                     register_core_node(Node n, const std::string& name);
        Node                     get_core_node(const std::string& name) const;
        std::string              get_core_name(Node n) const;
        std::string              get_name_hex(Node node, bool prepend_num, int max_neighbors) const;
        std::string              format(Node node) const;
        std::vector<std::string> get_languages() const;
        bool                     has_language(const std::string& language) const;
        name_of_node_map         get_nodes_in_language(const std::string& lang) const;
        std::vector<Node>        resolve_nodes_by_name(const std::string& name) const;
        size_t                   get_name_of_node_size(const std::string& lang) const;
        size_t                   get_node_of_name_size(const std::string& lang) const;
        size_t                   language_count() const;

        // --- Implemented in zelph_maintenance.cpp (cleanup, rules, persistence) ---

        void          cleanup_isolated(size_t& removed_count) const;
        size_t        cleanup_names() const;
        void          remove_node(Node node) const;
        adjacency_set get_rules() const;
        void          remove_rules() const;
        size_t        rule_count() const;
        void          save_to_file(const std::string& filename) const;
        void          load_from_file(const std::string& filename) const;

        // --- Members ---

        class Impl;
        Impl* const _pImpl; // must stay at top of members list because of initialization order

        const struct PredefinedNode
        {
            const Node RelationTypeCategory;
            const Node Causes;
            const Node IsA;
            const Node Unequal;
            const Node Contradiction;
            const Node Cons;
            const Node Nil;
            const Node PartOf;
            const Node Conjunction;
            const Node Negation;
        } core;

    protected:
        std::string                                    _lang{"en"};
        std::unordered_map<network::Node, std::string> _core_names_by_node;
        std::unordered_map<std::string, network::Node> _core_names_by_name;
        bool                                           _use_parallel{true};
    };
}