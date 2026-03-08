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
#include "network.hpp"
#include "output.hpp"

#include <zelph_export.h>

#include <boost/bimap.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    using name_of_node_map = ankerl::unordered_dense::map<Node, std::wstring>;
    using node_of_name_map = ankerl::unordered_dense::map<std::wstring, Node>;

    // The core semantic network engine. It manages the in-memory graph structure (nodes, edges),
    // provides low-level API for graph manipulation, and handles raw binary serialization (I/O)
    // of the network state via load_from_file/save_to_file. It is agnostic to the semantic meaning
    // or source format of the data.
    class ZELPH_EXPORT Zelph
    {
    public:
        explicit Zelph(const OutputHandler& output = default_output_handler);
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

        Node var() const;

        void                     set_lang(const std::string& lang);
        std::string              get_lang() const { return _lang; }
        void                     set_print(std::function<void(std::wstring, bool)> print) const;
        std::string              lang() const { return _lang; }
        Node                     node(const std::wstring& name, std::string lang = "");
        void                     register_core_node(Node n, const std::wstring& name);
        Node                     get_core_node(const std::wstring& name) const;
        std::wstring             get_core_name(Node n) const;
        bool                     exists(uint64_t nd) const;
        bool                     has_name(Node node, const std::string& lang) const;
        std::wstring             get_name(const Node node, std::string lang = "", const bool fallback = false) const;
        name_of_node_map         get_nodes_in_language(const std::string& lang) const;
        std::vector<std::string> get_languages() const;
        bool                     has_language(const std::string& language) const;
        Node                     get_node(const std::wstring& name, std::string lang = "") const;
        std::string              get_name_hex(Node node, bool prepend_num, int max_neighbors) const;
        void                     set_name(Node node, const std::wstring& name, std::string lang, bool merge_on_conflict);
        Node                     set_name(const std::wstring& name_in_current_lang, const std::wstring& name_in_given_lang, std::string lang);
        void                     cleanup_isolated(size_t& removed_count) const;
        size_t                   cleanup_names() const;
        size_t                   get_name_of_node_size(const std::string& lang) const;
        size_t                   get_node_of_name_size(const std::string& lang) const;
        size_t                   language_count() const;
        size_t                   rule_count() const;
        void                     set_logging(int max_depth) const;
        void                     log(int depth, const std::string& category, const std::string& message) const;
        bool                     should_log(int depth) const;
        bool                     logging_active() const;
        bool                     use_parallel() const { return _use_parallel; }
        void                     toggle_parallel() { _use_parallel = !_use_parallel; }

        adjacency_set        get_sources(Node relationType, Node target, bool exclude_vars = false) const;
        Node                 parse_fact(Node rule, adjacency_set& deductions, Node parent = 0) const;
        Node                 parse_relation(const Node rule) const;
        std::wstring         get_formatted_name(Node node, const std::string& lang) const;
        std::string          format(Node node) const;
        void                 format_fact(std::wstring& result, const std::string& lang, Node fact, const int max_objects = default_display_max_neighbors, const Variables& variables = {}, Node parent = 0, std::shared_ptr<std::unordered_set<Node>> history = nullptr) const;
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
        Node                 list(const std::vector<Node>& elements);
        Node                 list(const std::vector<std::wstring>& elements);
        Node                 set(const std::unordered_set<Node>& elements);
        FactComponents       extract_fact_components(Node relation) const;
        void                 set_output_handler(OutputHandler output) const;
        void                 emit(OutputChannel channel, const std::wstring& text, bool newline = true) const;

        void               out(const std::wstring&, bool newline = true) const;
        void               error(const std::wstring&, bool newline = true) const;
        void               diagnostic(const std::wstring&, bool newline = true) const;
        void               prompt(const std::wstring&, bool newline = false) const;
        static std::string get_version();
        void               save_to_file(const std::string& filename) const;
        void               load_from_file(const std::string& filename) const;

        Node              count() const;
        void              remove_name(Node node, std::string lang = "");
        adjacency_set     get_rules() const;
        void              remove_rules() const;
        void              remove_node(Node node) const;
        AllNodeView       get_all_nodes_view() const;
        LangNodeView      get_lang_nodes_view(const std::string& lang) const;
        void              unset_name(Node node, std::string lang = "");
        std::vector<Node> resolve_nodes_by_name(const std::wstring& name) const;
        bool              try_get_fact_structures_cached(Node fact, std::vector<FactStructure>& out) const;
        void              store_fact_structures_cached(Node fact, const std::vector<FactStructure>& value) const;
        void              invalidate_fact_structures_cache() const noexcept;
        OutputStream      out_stream() const;
        OutputStream      diagnostic_stream() const;
        OutputStream      error_stream() const;
        OutputStream      prompt_stream() const;

        // member list
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

        static constexpr int default_display_max_neighbors{5};

    protected:
        std::string                               _lang{"en"};
        boost::bimap<network::Node, std::wstring> _core_names;
        bool                                      _use_parallel{true};
    };
}
