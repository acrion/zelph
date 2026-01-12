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
#include "network.hpp"

#include <zelph_export.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    struct WrapperNode
    {
        bool     is_placeholder = false;
        uint64_t value          = 0;
        size_t   total_count    = 0; // Renamed for total nodes count (only for placeholders)

        bool operator==(const WrapperNode& other) const
        {
            return is_placeholder == other.is_placeholder && value == other.value && total_count == other.total_count;
        }

        bool operator<(const WrapperNode& other) const
        {
            if (is_placeholder != other.is_placeholder)
            {
                return is_placeholder < other.is_placeholder;
            }
            if (value != other.value)
            {
                return value < other.value;
            }
            return total_count < other.total_count;
        }
    };
}

namespace std
{
    template <>
    struct hash<zelph::network::WrapperNode>
    {
        size_t operator()(const zelph::network::WrapperNode& wn) const
        {
            return hash<bool>()(wn.is_placeholder) ^ hash<uint64_t>()(wn.value) ^ hash<size_t>()(wn.total_count);
        }
    };
}

namespace zelph
{
    namespace network
    {
        using name_of_node_map = ankerl::unordered_dense::map<Node, std::wstring>;
        using node_of_name_map = ankerl::unordered_dense::map<std::wstring, Node>;

        class ZELPH_EXPORT Zelph
        {
        public:
            explicit Zelph(const std::unordered_map<network::Node, std::wstring>& core_node_names, const std::function<void(const std::wstring&, const bool)>& print);
            ~Zelph();

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
            std::string              get_lang() { return _lang; }
            void                     set_print(std::function<void(std::wstring, bool)> print) { _print = print; }
            void                     set_process_node(std::function<void(const Node, const std::string&)> process_node) { _process_node = process_node; }
            std::string              lang() const { return _lang; }
            Node                     node(const std::wstring& name, std::string lang = "");
            bool                     exists(uint64_t nd);
            bool                     has_name(Node node, const std::string& lang) const;
            std::wstring             get_name(const Node node, std::string lang = "", const bool fallback = false, const bool process_node = true) const;
            name_of_node_map         get_nodes_in_language(const std::string& lang) const;
            std::vector<std::string> get_languages() const;
            bool                     has_language(const std::string& language) const;
            Node                     get_node(const std::wstring& name, std::string lang = "") const;
            std::string              get_name_hex(Node node, bool prepend_num, int max_neighbors);
            void                     set_name(Node node, const std::wstring& name, std::string lang = "");
            Node                     set_name(const std::wstring& name_in_current_lang, const std::wstring& name_in_given_lang, std::string lang);
            void                     cleanup_isolated(size_t& removed_count);
            size_t                   cleanup_names();

            adjacency_set        get_sources(Node relationType, Node target, bool exclude_vars = false) const;
            Node                 parse_fact(Node rule, adjacency_set& deductions, Node parent = 0) const;
            Node                 parse_relation(const Node rule);
            std::wstring         get_formatted_name(Node node, const std::string& lang) const;
            void                 format_fact(std::wstring& result, const std::string& lang, Node fact, const int max_objects, const Variables& variables = {}, Node parent = 0, std::shared_ptr<std::unordered_set<Node>> history = nullptr);
            adjacency_set        filter(const adjacency_set& source, Node target) const;
            adjacency_set        filter(Node fact, Node relationType, Node target) const;
            static adjacency_set filter(const adjacency_set& source, const std::function<bool(const Node nd)>& f);
            adjacency_set        get_left(const Node b);
            adjacency_set        get_right(const Node b);
            bool                 has_left_edge(Node b, Node a) const;
            bool                 has_right_edge(Node a, Node b) const;
            Answer               check_fact(Node subject, Node predicate, const adjacency_set& objects);
            Node                 fact(Node subject, Node predicate, const adjacency_set& objects, long double probability = 1);
            Node                 condition(Node op, const adjacency_set& conditions) const;
            void                 gen_mermaid_html(Node start, std::string file_name, int max_depth, int max_neighbors);
            void                 print(const std::wstring&, const bool) const;
            static std::string   get_version();
            void                 save_to_file(const std::string& filename);
            void                 load_from_file(const std::string& filename);

            Node              count() const;
            void              remove_name(Node node, std::string lang = "");
            adjacency_set     get_rules() const;
            void              remove_rules();
            void              remove_node(Node node);
            AllNodeView       get_all_nodes_view() const;
            LangNodeView      get_lang_nodes_view(const std::string& lang) const;
            void              unset_name(Node node, std::string lang = "");
            std::vector<Node> resolve_nodes_by_name(const std::wstring& name) const;

            // member list
            class Impl;
            Impl* const _pImpl; // must stay at top of members list because of initialization order

            const struct PredefinedNode
            {
                Node RelationTypeCategory;
                Node Causes;
                Node And;
                Node IsA;
                Node Unequal;
                Node Contradiction;
            } core;

        protected:
            std::string                                            _lang{"en"};
            const std::unordered_map<network::Node, std::wstring>& _core_node_names;

        private:
            void collect_mermaid_nodes(WrapperNode                                                     current_wrap,
                                       int                                                             max_depth,
                                       std::unordered_set<WrapperNode>&                                visited,
                                       std::unordered_set<Node>&                                       processed_edge_hashes,
                                       const adjacency_set&                                            conditions,
                                       const adjacency_set&                                            deductions,
                                       std::vector<std::tuple<WrapperNode, WrapperNode, std::string>>& raw_edges,
                                       std::unordered_set<WrapperNode>&                                all_nodes,
                                       int                                                             max_neighbors,
                                       size_t&                                                         placeholder_counter);

            std::function<void(std::wstring, bool)> _print;
            std::function<void(Node, std::string)>  _process_node;
        };
    }
}