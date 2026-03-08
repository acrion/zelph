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

#include "mermaid.hpp"

#include "adjacency_set.hpp"
#include "node_to_string.hpp"
#include "zelph.hpp"

#include <boost/algorithm/string.hpp>

#include <fstream>
#include <set>

// #define DEBUG_MERMAID

namespace zelph::console
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
    struct hash<zelph::console::WrapperNode>
    {
        size_t operator()(const zelph::console::WrapperNode& wn) const
        {
            return hash<bool>()(wn.is_placeholder) ^ hash<uint64_t>()(wn.value) ^ hash<size_t>()(wn.total_count);
        }
    };
}

using namespace zelph;
using namespace zelph::console;

// Recursively collects all nodes visually contained in a subgraph, using criteria 1–4 to detect nested subgraphs.
bool collect_subgraph_contents(const network::Zelph* const z, network::Node n, std::unordered_set<network::Node>& contents, std::unordered_set<network::Node>& visited)
{
    if (!visited.insert(n).second)
        return false; // cycle protection

    if (!network::Zelph::is_hash(n))
        return false;

    const auto right = z->get_right(n);
    const auto left  = z->get_left(n);

    // Criterion 2: Find predicate among outgoing connections
    network::Node pred_candidate = 0;
    for (network::Node p : right)
    {
        if ((!network::Zelph::is_hash(p) || network::Zelph::is_var(p))
            && z->check_fact(p, z->core.IsA, {z->core.RelationTypeCategory}).is_known())
        {
            if (pred_candidate != 0) return false;
            pred_candidate = p;
        }
    }
    if (pred_candidate == 0) return false;

    // Criterion 3: Find subject among bidirectional connections
    std::vector<network::Node> bidi_nodes;
    for (network::Node s : right)
    {
        if (s != pred_candidate && left.count(s) > 0)
            bidi_nodes.push_back(s);
    }
    if (bidi_nodes.empty()) return false;

    network::Node subj = 0;
    if (bidi_nodes.size() == 1)
    {
        subj = bidi_nodes[0];
    }
    else
    {
        for (network::Node s : bidi_nodes)
        {
            if (!network::Zelph::is_hash(s))
            {
                if (subj != 0) return false;
                subj = s;
            }
        }
        if (subj == 0) return false;
    }

    // Criterion 4: Find object among incoming-only connections
    std::vector<network::Node> incoming_only;
    for (network::Node o : left)
    {
        if (right.count(o) == 0)
            incoming_only.push_back(o);
    }
    if (incoming_only.empty()) return false;

    network::Node obj = 0;
    if (incoming_only.size() == 1)
    {
        obj = incoming_only[0];
    }
    else
    {
        for (network::Node o : incoming_only)
        {
            if (!network::Zelph::is_hash(o))
            {
                if (obj != 0) return false;
                obj = o;
            }
        }
        if (obj == 0) return false;
    }

    contents.insert(subj);
    contents.insert(obj);
    collect_subgraph_contents(z, subj, contents, visited);
    collect_subgraph_contents(z, obj, contents, visited);
    return true;
}

// Determine whether node n should be rendered as a Mermaid subgraph and identify its subject, predicate, and object.
bool identify_subgraph_components(const network::Zelph* const z, network::Node n, network::Node& subject, network::Node& predicate, network::Node& object, std::unordered_set<network::Node>* containment_conflicts)
{
    subject   = 0;
    predicate = 0;
    object    = 0;

    // Criterion 1: Must be a hash node
    if (!network::Zelph::is_hash(n))
        return false;

#ifdef DEBUG_MERMAID
    diagnostic_stream() << "[DEBUG_MERMAID] identify_subgraph node=" << n
                        << " (name: " << string::unicode::to_utf8(get_name(n, _lang, true))
                        << ", format: " << format(n) << ")" << std::endl;
#endif

    const auto right = z->get_right(n);
    const auto left  = z->get_left(n);

    // Criterion 2: Exactly one outgoing neighbor p where
    //   (!network::Zelph::is_hash(p) || network::Zelph::is_var(p)) && check_fact(p, z->core.IsA, {z->core.RelationTypeCategory}).is_known()
    network::Node pred_candidate = 0;
    for (network::Node p : right)
    {
        if (left.count(p) == 0 // exclude bidirectional
            && (!network::Zelph::is_hash(p) || network::Zelph::is_var(p))
            && z->check_fact(p, z->core.IsA, {z->core.RelationTypeCategory}).is_known())
        {
            if (pred_candidate != 0)
            {
#ifdef DEBUG_MERMAID
                diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 2 FAILED: multiple predicate candidates: "
                                    << pred_candidate << " (" << string::unicode::to_utf8(get_name(pred_candidate, _lang, true)) << ") and "
                                    << p << " (" << string::unicode::to_utf8(get_name(p, _lang, true)) << ")" << std::endl;
#endif
                return false; // more than one predicate candidate
            }
            pred_candidate = p;
        }
    }
    if (pred_candidate == 0)
    {
#ifdef DEBUG_MERMAID
        diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 2 FAILED: no predicate candidate found among " << right.size() << " outgoing neighbors:" << std::endl;
        for (network::Node p : right)
        {
            diagnostic_stream() << "[DEBUG_MERMAID]     outgoing " << p
                                << " (name: " << string::unicode::to_utf8(get_name(p, _lang, true))
                                << ", is_hash=" << network::Zelph::is_hash(p)
                                << ", is_var=" << network::Zelph::is_var(p)
                                << ", is_RTC=" << check_fact(p, z->core.IsA, {z->core.RelationTypeCategory}).is_known()
                                << ")" << std::endl;
        }
#endif
        return false;
    }
    predicate = pred_candidate;

    // Criterion 3: Subject identification via bidirectional connections
    std::vector<network::Node> bidi_nodes;
    for (network::Node s : right)
    {
        if (s != predicate && left.count(s) > 0)
            bidi_nodes.push_back(s);
    }
    if (bidi_nodes.empty())
    {
#ifdef DEBUG_MERMAID
        diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 3 FAILED: no bidirectional neighbor found (predicate="
                            << predicate << " (" << string::unicode::to_utf8(get_name(predicate, _lang, true)) << "))" << std::endl;
        diagnostic_stream() << "[DEBUG_MERMAID]     right nodes:";
        for (network::Node s : right)
            diagnostic_stream() << " " << s;
        diagnostic_stream() << std::endl;
        diagnostic_stream() << "[DEBUG_MERMAID]     left nodes:";
        for (network::Node s : left)
            diagnostic_stream() << " " << s;
        diagnostic_stream() << std::endl;
#endif
        return false;
    }

    if (bidi_nodes.size() == 1)
    {
        subject = bidi_nodes[0];
    }
    else
    {
        // Multiple bidirectional: exactly one must be non-hash or var
        network::Node non_hash = 0;
        for (network::Node s : bidi_nodes)
        {
            if (!network::Zelph::is_hash(s) || network::Zelph::is_var(s))
            {
                if (non_hash != 0)
                {
#ifdef DEBUG_MERMAID
                    diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 3 FAILED: multiple non-hash bidirectional neighbors: "
                                        << non_hash << " (" << string::unicode::to_utf8(get_name(non_hash, _lang, true)) << ") and "
                                        << s << " (" << string::unicode::to_utf8(get_name(s, _lang, true)) << ")" << std::endl;
#endif
                    return false; // ambiguous
                }
                non_hash = s;
            }
        }
        if (non_hash == 0)
        {
#ifdef DEBUG_MERMAID
            diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 3 FAILED: " << bidi_nodes.size()
                                << " bidirectional neighbors, ALL are hash nodes:" << std::endl;
            for (network::Node s : bidi_nodes)
            {
                diagnostic_stream() << "[DEBUG_MERMAID]     bidi " << s
                                    << " (name: " << string::unicode::to_utf8(get_name(s, _lang, true))
                                    << ", format: " << format(s)
                                    << ", is_hash=" << network::Zelph::is_hash(s)
                                    << ", is_var=" << network::Zelph::is_var(s) << ")" << std::endl;
            }
#endif
            return false;
        }
        subject = non_hash;
    }

    // Criterion 4: Object identification via incoming-only connections
    std::vector<network::Node> incoming_only;
    for (network::Node o : left)
    {
        if (right.count(o) == 0)
            incoming_only.push_back(o);
    }
    if (incoming_only.empty())
    {
#ifdef DEBUG_MERMAID
        diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 4 FAILED: no incoming-only neighbor found for node=" << n << std::endl;
        diagnostic_stream() << "[DEBUG_MERMAID]     left nodes:";
        for (network::Node o : left)
            diagnostic_stream() << " " << o << "(in_right=" << (right.count(o) > 0) << ")";
        diagnostic_stream() << std::endl;
#endif
        return false;
    }

    if (incoming_only.size() == 1)
    {
        object = incoming_only[0];
    }
    else
    {
        // Multiple incoming: exactly one must be non-hash
        network::Node non_hash = 0;
        for (network::Node o : incoming_only)
        {
            if (!network::Zelph::is_hash(o))
            {
                if (non_hash != 0)
                {
#ifdef DEBUG_MERMAID
                    diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 4 FAILED: multiple non-hash incoming-only neighbors: "
                                        << non_hash << " (" << string::unicode::to_utf8(get_name(non_hash, _lang, true)) << ") and "
                                        << o << " (" << string::unicode::to_utf8(get_name(o, _lang, true)) << ")" << std::endl;
#endif
                    return false; // multiple objects — not handled
                }
                non_hash = o;
            }
        }
        if (non_hash == 0)
        {
#ifdef DEBUG_MERMAID
            diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 4 FAILED: " << incoming_only.size()
                                << " incoming-only neighbors, ALL are hash nodes:" << std::endl;
            for (network::Node o : incoming_only)
            {
                diagnostic_stream() << "[DEBUG_MERMAID]     incoming-only " << o
                                    << " (name: " << string::unicode::to_utf8(get_name(o, _lang, true))
                                    << ", format: " << format(o)
                                    << ", is_hash=" << network::Zelph::is_hash(o) << ")" << std::endl;
            }
#endif
            return false;
        }
        object = non_hash;
    }

    // Criterion 5: If subject is a subgraph, it must not contain the object
    {
        std::unordered_set<network::Node> subject_contents;
        std::unordered_set<network::Node> visited;
        if (collect_subgraph_contents(z, subject, subject_contents, visited))
        {
            if (subject_contents.count(object))
            {
                if (containment_conflicts)
                {
                    // Record conflict but allow subgraph creation
                    containment_conflicts->insert(object);
#ifdef DEBUG_MERMAID
                    diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 5 CONFLICT (allowed via cloning): subject " << subject
                                        << " contains object " << object << " (" << string::unicode::to_utf8(get_name(object, _lang, true))
                                        << ")" << std::endl;
#endif
                }
                else
                {
#ifdef DEBUG_MERMAID
                    diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 5 FAILED: subject " << subject
                                        << " contains object " << object << " in its subgraph contents" << std::endl;
#endif
                    return false;
                }
            }
        }
    }

    // Criterion 6: If object is a subgraph, it must not contain the subject
    {
        std::unordered_set<network::Node> object_contents;
        std::unordered_set<network::Node> visited;
        if (collect_subgraph_contents(z, object, object_contents, visited))
        {
            if (object_contents.count(subject))
            {
                if (containment_conflicts)
                {
                    containment_conflicts->insert(subject);
#ifdef DEBUG_MERMAID
                    diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 6 CONFLICT (allowed via cloning): object " << object
                                        << " (" << string::unicode::to_utf8(get_name(object, _lang, true))
                                        << ") contains subject " << subject
                                        << " (" << string::unicode::to_utf8(get_name(subject, _lang, true)) << ")" << std::endl;
#endif
                }
                else
                {
#ifdef DEBUG_MERMAID
                    diagnostic_stream() << "[DEBUG_MERMAID]   Criterion 6 FAILED: object " << object
                                        << " contains subject " << subject << " in its subgraph contents" << std::endl;
#endif
                    return false;
                }
            }
        }
    }

#ifdef DEBUG_MERMAID
    diagnostic_stream() << "[DEBUG_MERMAID]   SUCCESS: subgraph node=" << n
                        << " subject=" << subject << " (" << string::unicode::to_utf8(get_name(subject, _lang, true)) << ")"
                        << " predicate=" << predicate << " (" << string::unicode::to_utf8(get_name(predicate, _lang, true)) << ")"
                        << " object=" << object << " (" << string::unicode::to_utf8(get_name(object, _lang, true)) << ")" << std::endl;
#endif

    return true;
}

void collect_mermaid_nodes(const network::Zelph* const                                     z,
                           WrapperNode                                                     current_wrap,
                           int                                                             max_depth,
                           std::unordered_set<WrapperNode>&                                visited,
                           std::unordered_set<network::Node>&                              processed_edge_hashes,
                           const network::adjacency_set&                                   conditions,
                           const network::adjacency_set&                                   deductions,
                           std::vector<std::tuple<WrapperNode, WrapperNode, std::string>>& raw_edges,
                           std::unordered_set<WrapperNode>&                                all_nodes,
                           int                                                             max_neighbors,
                           size_t&                                                         placeholder_counter,
                           const std::unordered_set<network::Node>&                        exclude_nodes)
{
    if (--max_depth <= 0 || visited.count(current_wrap))
        return;

    visited.insert(current_wrap);
    all_nodes.insert(current_wrap);

    if (current_wrap.is_placeholder) return; // No recursion for placeholders

    network::Node current = current_wrap.value;

    // Left neighbors (incoming)
    const auto& lefts      = z->get_left(current);
    size_t      num_left   = lefts.size();
    size_t      limit_left = (max_neighbors > 0) ? std::min(static_cast<size_t>(max_neighbors), num_left) : num_left;
    auto        left_it    = lefts.begin();
    for (size_t i = 0; i < limit_left; ++i, ++left_it)
    {
        network::Node left = *left_it;

        if (exclude_nodes.count(left)) continue;

        network::Node hash = network::Zelph::create_hash({current, left});

        if (processed_edge_hashes.insert(hash).second)
        {
            bool        is_bi = z->has_left_edge(left, current);
            std::string arrow = is_bi ? "<-->" : "-->";
            raw_edges.emplace_back(WrapperNode{false, left}, WrapperNode{false, current}, arrow);
            all_nodes.insert(WrapperNode{false, left});
        }

        collect_mermaid_nodes(z, WrapperNode{false, left}, max_depth, visited, processed_edge_hashes, conditions, deductions, raw_edges, all_nodes, max_neighbors, placeholder_counter, exclude_nodes);
    }
    if (max_neighbors > 0 && num_left > static_cast<size_t>(max_neighbors))
    {
        // Add unique placeholder for lefts
        ++placeholder_counter;
        WrapperNode placeholder_wrap{true, placeholder_counter, num_left};
        raw_edges.emplace_back(placeholder_wrap, WrapperNode{false, current}, std::string("-->"));
        all_nodes.insert(placeholder_wrap);
    }

    // Right neighbors (outgoing)
    const auto& rights      = z->get_right(current);
    size_t      num_right   = rights.size();
    size_t      limit_right = (max_neighbors > 0) ? std::min(static_cast<size_t>(max_neighbors), num_right) : num_right;
    auto        right_it    = rights.begin();
    for (size_t i = 0; i < limit_right; ++i, ++right_it)
    {
        network::Node right = *right_it;

        if (exclude_nodes.count(right)) continue;

        network::Node hash = network::Zelph::create_hash({current, right});

        if (processed_edge_hashes.insert(hash).second)
        {
            bool        is_bi = z->has_right_edge(right, current);
            std::string arrow = is_bi ? "<-->" : "-->";
            raw_edges.emplace_back(WrapperNode{false, current}, WrapperNode{false, right}, arrow);
            all_nodes.insert(WrapperNode{false, right});
        }

        collect_mermaid_nodes(z, WrapperNode{false, right}, max_depth, visited, processed_edge_hashes, conditions, deductions, raw_edges, all_nodes, max_neighbors, placeholder_counter, exclude_nodes);
    }
    if (max_neighbors > 0 && num_right > static_cast<size_t>(max_neighbors))
    {
        // Add unique placeholder for rights
        ++placeholder_counter;
        WrapperNode placeholder_wrap{true, placeholder_counter, num_right};
        raw_edges.emplace_back(WrapperNode{false, current}, placeholder_wrap, std::string("-->"));
        all_nodes.insert(placeholder_wrap);
    }
}

void console::gen_mermaid_html(const network::Zelph* const              z,
                               network::Node                            start,
                               std::string                              file_name,
                               int                                      max_depth,
                               int                                      max_neighbors,
                               const std::unordered_set<network::Node>& exclude_nodes,
                               bool                                     dark_theme,
                               bool                                     horizontal_layout,
                               bool                                     use_subgraphs,
                               size_t                                   min_mermaid_nodes)
{
    // Helper: check if a node is a predefined z->core node
    auto is_predefined_node = [&](network::Node nd) -> bool
    {
        return nd == z->core.RelationTypeCategory || nd == z->core.Causes || nd == z->core.IsA
            || nd == z->core.Unequal || nd == z->core.Contradiction || nd == z->core.Cons
            || nd == z->core.Nil || nd == z->core.PartOf || nd == z->core.Conjunction || nd == z->core.Negation;
    };

    network::adjacency_set conditions, deductions;

    // Extract rules and parse conditions/deductions
    for (network::Node rule : z->get_left(z->core.Causes))
    {
        network::adjacency_set current_deductions;
        network::Node          condition = z->parse_fact(rule, current_deductions);

        if (condition && condition != z->core.Causes)
        {
            conditions.insert(condition);
            for (network::Node deduction : current_deductions)
            {
                deductions.insert(deduction);
            }
        }
    }

    struct SubgraphInfo
    {
        network::Node subject;
        network::Node predicate;
        network::Node object;
    };
    std::map<network::Node, SubgraphInfo>                          subgraphs;
    int                                                            effective_depth = (max_depth == 1 && min_mermaid_nodes > 0) ? 2 : max_depth;
    bool                                                           dynamic_depth   = (max_depth == 1 && min_mermaid_nodes > 0);
    std::unordered_set<WrapperNode>                                visited;
    std::unordered_set<network::Node>                              processed_edge_hashes;
    std::vector<std::tuple<WrapperNode, WrapperNode, std::string>> raw_edges;
    std::unordered_set<WrapperNode>                                all_nodes;
    size_t                                                         placeholder_counter = 0;
    size_t                                                         clone_counter       = 0;

    // clone_in_sg[(original_node, subgraph)] = clone_mermaid_id
    // Tracks which leaf nodes are replaced by clones in which subgraphs.
    std::map<std::pair<network::Node, network::Node>, std::string> clone_in_sg;

    // (original_mermaid_id, clone_mermaid_id) pairs for identity edges
    std::vector<std::pair<std::string, std::string>> clone_identity_edges;

    // --- BEGIN dynamic depth loop ---
    for (;;)
    {
        visited.clear();
        processed_edge_hashes.clear();
        raw_edges.clear();
        all_nodes.clear();
        placeholder_counter = 0;
        collect_mermaid_nodes(z, WrapperNode{false, start}, effective_depth, visited, processed_edge_hashes, conditions, deductions, raw_edges, all_nodes, max_neighbors, placeholder_counter, exclude_nodes);

        // Remove excluded nodes from all_nodes and raw_edges
        for (auto it = all_nodes.begin(); it != all_nodes.end();)
        {
            if (!it->is_placeholder && exclude_nodes.count(it->value))
                it = all_nodes.erase(it);
            else
                ++it;
        }
        // Remove edges that involve excluded nodes
        raw_edges.erase(
            std::remove_if(raw_edges.begin(), raw_edges.end(), [&](const auto& edge)
                           {
                           const auto& [from, to, arrow] = edge;
                           return (!from.is_placeholder && exclude_nodes.count(from.value))
                               || (!to.is_placeholder && exclude_nodes.count(to.value)); }),
            raw_edges.end());

        // === SUBGRAPH IDENTIFICATION WITH CLONE-BASED CONFLICT RESOLUTION ===
        subgraphs.clear();
        clone_in_sg.clear();
        clone_identity_edges.clear();
        clone_counter = 0;

        if (use_subgraphs)
        {
            // Phase 1: Identify subgraph candidates, allowing containment conflicts
#ifdef DEBUG_MERMAID
            diagnostic_stream() << "[DEBUG_MERMAID] === SUBGRAPH IDENTIFICATION PHASE ===" << std::endl;
            diagnostic_stream() << "[DEBUG_MERMAID] Total nodes to check: " << all_nodes.size() << std::endl;
#endif
            for (const WrapperNode& wn : all_nodes)
            {
                if (wn.is_placeholder) continue;
                if (exclude_nodes.count(wn.value)) continue;

                network::Node                     s, p, o;
                std::unordered_set<network::Node> containment_conflicts;
                if (identify_subgraph_components(z, wn.value, s, p, o, &containment_conflicts))
                {
                    bool s_present = all_nodes.count(WrapperNode{false, s, 0}) && !exclude_nodes.count(s);
                    bool o_present = all_nodes.count(WrapperNode{false, o, 0}) && !exclude_nodes.count(o);
                    if (s_present && o_present)
                    {
                        subgraphs[wn.value] = {s, p, o};
#ifdef DEBUG_MERMAID
                        diagnostic_stream() << "[DEBUG_MERMAID] Registered subgraph: node=" << wn.value
                                            << " S=" << s << " P=" << p << " O=" << o;
                        if (!containment_conflicts.empty())
                        {
                            diagnostic_stream() << " (containment conflicts:";
                            for (network::Node c : containment_conflicts)
                                diagnostic_stream() << " " << c;
                            diagnostic_stream() << ")";
                        }
                        diagnostic_stream() << std::endl;
#endif
                    }
                }
            }

            // Phase 2: Determine nesting hierarchy
            std::map<network::Node, network::Node> sg_parent;
            for (auto& [r, info] : subgraphs)
            {
                sg_parent[r] = 0;
                for (auto& [r2, info2] : subgraphs)
                {
                    if (r2 == r) continue;
                    if (info2.subject == r || info2.object == r)
                    {
                        sg_parent[r] = r2;
                        break;
                    }
                }
#ifdef DEBUG_MERMAID
                diagnostic_stream() << "[DEBUG_MERMAID] Nesting: subgraph " << r
                                    << (sg_parent[r] == 0 ? " is top-level" : " is child of subgraph " + std::to_string(sg_parent[r]))
                                    << std::endl;
#endif
            }

            // Phase 3: Clone-based conflict resolution
            // For each non-subgraph (leaf) node, find all subgraphs where it appears as a direct member
            // (subject or object). If it appears in more than one subgraph, the outermost keeps the
            // original and all inner/independent subgraphs get clones.

            auto is_ancestor_of = [&](network::Node potential_ancestor, network::Node sg) -> bool
            {
                network::Node current = sg;
                while (sg_parent.count(current) && sg_parent[current] != 0)
                {
                    current = sg_parent[current];
                    if (current == potential_ancestor) return true;
                }
                return false;
            };

            std::map<network::Node, std::vector<network::Node>> node_to_direct_sgs;
            for (auto& [r, info] : subgraphs)
            {
                // Only track leaf nodes (non-subgraph nodes)
                if (!subgraphs.count(info.subject))
                    node_to_direct_sgs[info.subject].push_back(r);
                if (!subgraphs.count(info.object))
                    node_to_direct_sgs[info.object].push_back(r);
            }

            for (auto& [nd, sgs] : node_to_direct_sgs)
            {
                if (sgs.size() <= 1) continue;

                // Pick the "owner" subgraph that keeps the original node.
                // Priority: outermost ancestor > referenced subgraph > larger ID
                network::Node owner = sgs[0];
                for (size_t i = 1; i < sgs.size(); ++i)
                {
                    if (is_ancestor_of(sgs[i], owner))
                    {
                        owner = sgs[i]; // sgs[i] is an ancestor of owner, so it's more outer
                    }
                    else if (!is_ancestor_of(owner, sgs[i]))
                    {
                        // Independent subgraphs: prefer the one that is referenced by another subgraph
                        bool owner_ref = false, sgi_ref = false;
                        for (auto& [r2, info2] : subgraphs)
                        {
                            if (info2.subject == owner || info2.object == owner) owner_ref = true;
                            if (info2.subject == sgs[i] || info2.object == sgs[i]) sgi_ref = true;
                        }
                        if ((sgi_ref && !owner_ref)
                            || (sgs[i] > owner && !(owner_ref && !sgi_ref)))
                            owner = sgs[i];
                    }
                }

                // Create clones for all non-owner subgraphs
                std::string orig_id = "n_" + std::to_string(static_cast<unsigned long long>(nd));
                for (network::Node sg : sgs)
                {
                    if (sg == owner) continue;
                    std::string cid = "clone_" + std::to_string(static_cast<unsigned long long>(nd))
                                    + "_" + std::to_string(++clone_counter);
                    clone_in_sg[{nd, sg}] = cid;
                    clone_identity_edges.emplace_back(orig_id, cid);

#ifdef DEBUG_MERMAID
                    diagnostic_stream() << "[DEBUG_MERMAID] Clone: node " << nd
                                        << " (" << string::unicode::to_utf8(get_name(nd, _lang, true))
                                        << ") cloned as " << cid << " in subgraph " << sg
                                        << " (owner subgraph: " << owner << ")" << std::endl;
#endif
                }
            }

            // Phase 4: Handle subgraph-level conflicts (a subgraph node itself appearing in multiple
            // independent subgraphs). This is the fallback: if the shared entity is itself a subgraph
            // (not a leaf), we cannot clone it easily, so we fall back to removing one subgraph.
            {
                bool changed = true;
                while (changed)
                {
                    changed = false;

                    // Collect all nodes contained in each subgraph (recursively through nesting)
                    std::map<network::Node, std::unordered_set<network::Node>> sg_contained;
                    for (auto& [r, info] : subgraphs)
                    {
                        std::function<void(network::Node, std::unordered_set<network::Node>&)> collect;
                        collect = [&](network::Node sg_node, std::unordered_set<network::Node>& out)
                        {
                            auto it = subgraphs.find(sg_node);
                            if (it == subgraphs.end()) return;
                            out.insert(it->second.subject);
                            out.insert(it->second.object);
                            collect(it->second.subject, out);
                            collect(it->second.object, out);
                        };
                        collect(r, sg_contained[r]);
                    }

                    // Build reverse map: node -> set of subgraphs containing it (transitively)
                    // But only consider subgraph-nodes (nodes that are themselves subgraphs)
                    std::map<network::Node, std::set<network::Node>> subgraph_node_to_sgs;
                    for (auto& [r, contained] : sg_contained)
                    {
                        for (network::Node nd : contained)
                        {
                            if (subgraphs.count(nd)) // only subgraph-nodes
                                subgraph_node_to_sgs[nd].insert(r);
                        }
                    }

                    // Helper: check if sg1 is nested inside sg2
                    auto is_nested_in = [&](network::Node sg1, network::Node sg2) -> bool
                    {
                        return sg_contained.count(sg2) && sg_contained[sg2].count(sg1);
                    };

                    // Check if a subgraph is "referenced" (appears as subject or object of another subgraph)
                    auto is_referenced = [&](network::Node sg) -> bool
                    {
                        for (auto& [r, info] : subgraphs)
                        {
                            if (r == sg) continue;
                            if (info.subject == sg || info.object == sg) return true;
                        }
                        return false;
                    };

                    // Find a conflicting pair: two independent subgraphs sharing a node
                    network::Node to_remove = 0;
                    for (auto& [nd, sgs] : subgraph_node_to_sgs)
                    {
                        if (sgs.size() <= 1) continue;
                        std::vector<network::Node> sg_vec(sgs.begin(), sgs.end());
                        for (size_t i = 0; i < sg_vec.size() && to_remove == 0; ++i)
                        {
                            for (size_t j = i + 1; j < sg_vec.size() && to_remove == 0; ++j)
                            {
                                network::Node a = sg_vec[i], b = sg_vec[j];
                                if (is_nested_in(a, b) || is_nested_in(b, a)) continue;

                                bool a_ref = is_referenced(a), b_ref = is_referenced(b);
                                if (a_ref && !b_ref)
                                    to_remove = b;
                                else if (!a_ref && b_ref)
                                    to_remove = a;
                                else
                                    to_remove = std::min(a, b);

#ifdef DEBUG_MERMAID
                                diagnostic_stream() << "[DEBUG_MERMAID] Subgraph-level conflict: subgraph-node " << nd
                                                    << " in independent subgraphs " << a << " and " << b
                                                    << " -> removing subgraph " << to_remove << std::endl;
#endif
                            }
                        }
                        if (to_remove != 0) break;
                    }

                    if (to_remove != 0)
                    {
                        // Also remove clones associated with the removed subgraph
                        for (auto it = clone_in_sg.begin(); it != clone_in_sg.end();)
                        {
                            if (it->first.second == to_remove)
                                it = clone_in_sg.erase(it);
                            else
                                ++it;
                        }
                        subgraphs.erase(to_remove);
                        changed = true;
                    }
                }
            }

            // Rebuild sg_parent after potential removals
            sg_parent.clear();
            for (auto& [r, info] : subgraphs)
            {
                sg_parent[r] = 0;
                for (auto& [r2, info2] : subgraphs)
                {
                    if (r2 == r) continue;
                    if (info2.subject == r || info2.object == r)
                    {
                        sg_parent[r] = r2;
                        break;
                    }
                }
            }
        }

        if (!dynamic_depth)
            break;

        // Count effective visible nodes: each subgraph counts as 1,
        // nodes inside subgraphs are not counted.
        size_t effective_count = subgraphs.size();
        for (const WrapperNode& wn : all_nodes)
        {
            if (wn.is_placeholder) continue;
            if (subgraphs.count(wn.value)) continue;
            bool inside_sg = false;
            for (auto& [r, info] : subgraphs)
            {
                if (info.subject == wn.value || info.object == wn.value)
                {
                    inside_sg = true;
                    break;
                }
            }
            if (!inside_sg)
                ++effective_count;
        }

        if (effective_count >= min_mermaid_nodes || effective_depth >= 100)
            break;

        ++effective_depth;
    }
    // --- END dynamic depth loop ---

    // === DETERMINE NODE PLACEMENT ===

    // For each subgraph, find its direct parent subgraph (if any)
    std::map<network::Node, network::Node> sg_parent_final; // subgraph → parent subgraph (0 = top-level)
    for (auto& [r, info] : subgraphs)
    {
        sg_parent_final[r] = 0;
        for (auto& [r2, info2] : subgraphs)
        {
            if (r2 == r) continue;
            if (info2.subject == r || info2.object == r)
            {
                sg_parent_final[r] = r2;
                break;
            }
        }
    }

    // For each non-subgraph node, determine its innermost containing subgraph
    std::map<network::Node, network::Node> node_container; // node → containing subgraph (0 = top-level)
    for (const WrapperNode& wn : all_nodes)
    {
        if (wn.is_placeholder) continue;
        network::Node nd = wn.value;
        if (subgraphs.count(nd)) continue; // subgraphs are handled separately

        node_container[nd] = 0;
        for (auto& [r, info] : subgraphs)
        {
            if (info.subject == nd || info.object == nd)
            {
                // Check if this is the innermost container (no child subgraph also contains nd)
                bool is_innermost = true;
                for (auto& [r2, info2] : subgraphs)
                {
                    if (r2 == r) continue;
                    if (sg_parent_final.count(r2) && sg_parent_final[r2] == r) // r2 is a child of r
                    {
                        if (info2.subject == nd || info2.object == nd)
                        {
                            is_innermost = false;
                            break;
                        }
                    }
                }
                if (is_innermost)
                {
                    node_container[nd] = r;
                    break;
                }
            }
        }
    } // if (use_subgraphs)

    // Cloned nodes are also "inside" subgraphs, so mark them as contained
    for (auto& [key, cid] : clone_in_sg)
    {
        network::Node nd = key.first;
        network::Node sg = key.second;
        // The clone replaces the original in this subgraph,
        // so the original should NOT be considered contained here
        if (node_container.count(nd) && node_container[nd] == sg)
        {
            // The original was assigned to this subgraph as innermost,
            // but a clone takes its place here. Find the correct container for the original.
            node_container[nd] = 0; // will be re-assigned below or left at top-level
            for (auto& [r, info] : subgraphs)
            {
                if (r == sg) continue;
                if (info.subject == nd || info.object == nd)
                {
                    // Check this isn't also a clone-target subgraph
                    if (clone_in_sg.count({nd, r}) == 0)
                    {
                        node_container[nd] = r;
                        break;
                    }
                }
            }
        }
    }

    // === BUILD INTERNAL EDGE SET ===
    // Edges that are part of a subgraph's S-P-O structure (to be removed from raw edges)
    std::set<std::pair<uint64_t, uint64_t>> internal_edge_pairs;
    for (auto& [r, info] : subgraphs)
    {
        // Bidirectional: R ↔ subject
        internal_edge_pairs.insert({r, info.subject});
        internal_edge_pairs.insert({info.subject, r});
        // Outgoing: R → predicate
        internal_edge_pairs.insert({r, info.predicate});
        internal_edge_pairs.insert({info.predicate, r});
        // Incoming: object → R
        internal_edge_pairs.insert({info.object, r});
        internal_edge_pairs.insert({r, info.object});
    }

    // Originals that still have at least one active clone should be highlighted as well.
    std::unordered_set<network::Node> cloned_original_nodes;
    for (const auto& [key, cid] : clone_in_sg)
        cloned_original_nodes.insert(key.first);

    // --- COLOR PALETTES ---
    std::string col_start  = dark_theme ? "#8a5c00" : "#FFBB00";
    std::string col_var    = dark_theme ? "#4e483d" : "#eee8dc";
    std::string col_cond   = dark_theme ? "#2a5275" : "#87cefa";
    std::string col_ded    = dark_theme ? "#3b6327" : "#bcee68";
    std::string col_ph     = dark_theme ? "#404040" : "#d3d3d3";
    std::string col_def    = dark_theme ? "#2d2d38" : "#f0f2f5";
    std::string col_sg     = dark_theme ? "#1a1a2e" : "#fafbfd";
    std::string col_predef = dark_theme ? "#5c3566" : "#d4a5e5";
    std::string col_clone  = dark_theme ? "#3d3022" : "#fff3cd";

    std::string text_col = dark_theme ? "#e0e0e0" : "#111111";
    std::string line_col = dark_theme ? "#666666" : "#999999";

    std::string predef_stroke = dark_theme ? "#9b59b6" : "#8e44ad";
    std::string clone_stroke  = dark_theme ? "#d4a017" : "#b8860b";

    // === BUILD NODE IDS AND DEFINITIONS ===

    std::map<WrapperNode, std::string> node_ids;
    std::map<WrapperNode, std::string> node_defs;
    std::vector<std::string>           style_defs;

    // Clone node definitions: clone_id -> definition string
    std::map<std::string, std::string> clone_node_defs;

    for (const WrapperNode& wn : all_nodes)
    {
        std::string id;
        std::string raw_label;

        if (wn.is_placeholder)
        {
            id        = "ph_" + std::to_string(wn.value);
            raw_label = "[... " + std::to_string(wn.total_count) + " nodes ...]";
        }
        else
        {
            id        = "n_" + std::to_string(static_cast<unsigned long long>(wn.value));
            raw_label = z->get_name_hex(wn.value, true, max_neighbors);
        }
        node_ids[wn] = id;

        std::string label = raw_label;
        boost::replace_all(label, "\"", "#quot;");

        node_defs[wn] = id + "(\"" + label + "\")";

        // Determine fill color
        std::string fill_color;
        if (!wn.is_placeholder)
        {
            network::Node node = wn.value;
            if (node == start)
                fill_color = col_start;
            else if (is_predefined_node(node))
                fill_color = col_predef;
            else if (network::Zelph::is_var(node))
                fill_color = col_var;
            else if (conditions.count(node))
                fill_color = col_cond;
            else if (deductions.count(node))
                fill_color = col_ded;
            else
                fill_color = col_def;
        }
        else
        {
            fill_color = col_ph;
        }

        if (!subgraphs.count(wn.value))
        {
            std::string stroke       = line_col;
            std::string stroke_width = "1.5px";
            std::string stroke_dasharray;

            if (!wn.is_placeholder)
            {
                if (cloned_original_nodes.count(wn.value))
                {
                    // Highlight the original as part of the "=" clone family as well.
                    stroke           = clone_stroke;
                    stroke_width     = "2px";
                    stroke_dasharray = ",stroke-dasharray:5 3";
                }
                else if (is_predefined_node(wn.value))
                {
                    stroke = predef_stroke;
                }
            }

            style_defs.push_back(
                "    style " + id
                + " fill:" + fill_color
                + ",stroke:" + stroke
                + ",stroke-width:" + stroke_width
                + stroke_dasharray
                + ",color:" + text_col);
        }
    }

    // Build clone node definitions and styles
    for (auto& [key, cid] : clone_in_sg)
    {
        network::Node nd    = key.first;
        std::string   label = z->get_name_hex(nd, true, max_neighbors);
        boost::replace_all(label, "\"", "#quot;");
        clone_node_defs[cid] = cid + "(\"" + label + "\")";

        // Clone nodes get a distinctive style (same base color but clone stroke)
        style_defs.push_back("    style " + cid + " fill:" + col_clone + ",stroke:" + clone_stroke
                             + ",stroke-width:2px,stroke-dasharray:5 3,color:" + text_col);
    }

    // Subgraph styles
    for (auto& [r, info] : subgraphs)
    {
        std::string        sg_id  = "sg_" + std::to_string(static_cast<unsigned long long>(r));
        const std::string& fill   = col_sg;
        std::string        stroke = line_col;
        if (r == start)
            stroke = dark_theme ? "#8a5c00" : "#FFBB00";
        style_defs.push_back("    style " + sg_id + " fill:" + fill + ",stroke:" + stroke + ",stroke-width:2px,color:" + text_col);
    }

    // === GENERATE MERMAID ===

    std::stringstream mermaid;
    mermaid << (horizontal_layout ? "graph LR" : "graph TD") << std::endl;

    // Helper: resolve the mermaid ID for a node in a given subgraph context.
    // Returns clone ID if the node is cloned in that subgraph, otherwise the original ID.
    auto resolve_id_in_sg = [&](network::Node nd, network::Node sg) -> std::string
    {
        auto clone_it = clone_in_sg.find({nd, sg});
        if (clone_it != clone_in_sg.end())
            return clone_it->second;
        if (subgraphs.count(nd))
            return "sg_" + std::to_string(static_cast<unsigned long long>(nd));
        return node_ids[WrapperNode{false, nd, 0}];
    };

    // Recursive subgraph emitter
    std::function<void(network::Node, int)> emit_subgraph;
    emit_subgraph = [&](network::Node sg, int indent_level)
    {
        auto&       info = subgraphs[sg];
        std::string indent(indent_level * 4, ' ');
        std::string sg_id = "sg_" + std::to_string(static_cast<unsigned long long>(sg));

        // Generate subgraph label from z->node_to_wstring
        std::wstring label_w;
        console::node_to_wstring(z, label_w, z->get_lang(), sg, max_neighbors);
        std::string label = string::unicode::to_utf8(label_w);
        boost::replace_all(label, "\"", "#quot;");

        mermaid << indent << "subgraph " << sg_id << "[\"" << label << "\"]" << std::endl;

        // Emit subject
        auto subj_clone_it = clone_in_sg.find({info.subject, sg});
        if (subj_clone_it != clone_in_sg.end())
        {
            // Subject is cloned in this subgraph
            mermaid << indent << "    " << clone_node_defs[subj_clone_it->second] << std::endl;
        }
        else if (subgraphs.count(info.subject))
        {
            emit_subgraph(info.subject, indent_level + 1);
        }
        else
        {
            WrapperNode swn{false, info.subject, 0};
            mermaid << indent << "    " << node_defs[swn] << std::endl;
        }

        // Emit object
        auto obj_clone_it = clone_in_sg.find({info.object, sg});
        if (obj_clone_it != clone_in_sg.end())
        {
            mermaid << indent << "    " << clone_node_defs[obj_clone_it->second] << std::endl;
        }
        else if (subgraphs.count(info.object))
        {
            emit_subgraph(info.object, indent_level + 1);
        }
        else
        {
            WrapperNode own{false, info.object, 0};
            mermaid << indent << "    " << node_defs[own] << std::endl;
        }

        mermaid << indent << "end" << std::endl;
    };

    // Emit top-level nodes
    for (const WrapperNode& wn : all_nodes)
    {
        if (wn.is_placeholder)
        {
            mermaid << "    " << node_defs[wn] << std::endl;
            continue;
        }
        if (subgraphs.count(wn.value)) continue;                                       // will be emitted as subgraph
        if (node_container.count(wn.value) && node_container[wn.value] != 0) continue; // inside a subgraph
        mermaid << "    " << node_defs[wn] << std::endl;
    }

    // Emit top-level subgraphs
    for (auto& [r, info] : subgraphs)
    {
        if (sg_parent_final[r] == 0)
            emit_subgraph(r, 1);
    }

    // Emit styles
    for (const auto& s : style_defs)
        mermaid << s << std::endl;

    // === EMIT EDGES ===
    size_t              edge_index = 0;
    std::vector<size_t> predefined_edge_indices;
    std::vector<size_t> clone_edge_indices;

    // 1. Labeled edges for subgraphs (subject -->|predicate| object)
    for (auto& [r, info] : subgraphs)
    {
        std::string subj_id = resolve_id_in_sg(info.subject, r);
        std::string obj_id  = resolve_id_in_sg(info.object, r);

        std::string pred_label = z->get_name_hex(info.predicate, false, max_neighbors);
        boost::replace_all(pred_label, "\"", "#quot;");

        mermaid << "    " << subj_id << " -->|\"" << pred_label << "\"| " << obj_id << std::endl;

        if (is_predefined_node(info.predicate))
            predefined_edge_indices.push_back(edge_index);

        ++edge_index;
    }

    // 2. Remaining raw edges (excluding internal subgraph edges)
    for (const auto& [from, to, arrow] : raw_edges)
    {
        // Skip edges that are internal to a subgraph
        if (!from.is_placeholder && !to.is_placeholder)
        {
            auto pair_fwd = std::make_pair(static_cast<uint64_t>(from.value), static_cast<uint64_t>(to.value));
            auto pair_rev = std::make_pair(static_cast<uint64_t>(to.value), static_cast<uint64_t>(from.value));
            if (internal_edge_pairs.count(pair_fwd) || internal_edge_pairs.count(pair_rev))
                continue;
        }

        // Redirect edges: if an endpoint is a subgraph, point to the subgraph ID instead
        std::string from_id;
        if (!from.is_placeholder && subgraphs.count(from.value))
            from_id = "sg_" + std::to_string(static_cast<unsigned long long>(from.value));
        else
            from_id = node_ids[from];

        std::string to_id;
        if (!to.is_placeholder && subgraphs.count(to.value))
            to_id = "sg_" + std::to_string(static_cast<unsigned long long>(to.value));
        else
            to_id = node_ids[to];

        mermaid << "    " << from_id << " " << arrow << " " << to_id << std::endl;
        ++edge_index;
    }

    // 3. Clone identity edges (dotted lines connecting original to clone)
    for (auto& [orig_id, cid] : clone_identity_edges)
    {
        mermaid << "    " << orig_id << " -.-|\"=\"| " << cid << std::endl;
        clone_edge_indices.push_back(edge_index);
        ++edge_index;
    }

    // Emit linkStyle for predefined-predicate edges
    for (size_t idx : predefined_edge_indices)
        mermaid << "    linkStyle " << idx << " stroke:" << predef_stroke << ",stroke-width:2.5px" << std::endl;

    // Emit linkStyle for clone identity edges
    for (size_t idx : clone_edge_indices)
        mermaid << "    linkStyle " << idx << " stroke:" << clone_stroke << ",stroke-width:1.5px,stroke-dasharray:5 3" << std::endl;

    // --- HTML TEMPLATE GENERATION ---
    std::string mermaid_theme = dark_theme ? "dark" : "default";
    std::string body_bg       = dark_theme ? "#18181b" : "#ffffff";
    std::string text_color    = dark_theme ? "#cccccc" : "#333333";

    std::string shadow_css = dark_theme ? "drop-shadow(2px 4px 6px rgba(0,0,0,0.5))"
                                        : "drop-shadow(2px 4px 6px rgba(0,0,0,0.1))";

    std::string html_header = R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Zelph Graph Explorer</title>
    <script src="https://cdn.jsdelivr.net/npm/mermaid/dist/mermaid.min.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/svg-pan-zoom/dist/svg-pan-zoom.min.js"></script>

    <script>
        function fixSubgraphTitleOverlap(svg) {
            var clusters = Array.from(svg.querySelectorAll('.cluster'));
            if (clusters.length === 0) return;
    
            // Get element bounding box in SVG root coordinate space
            function svgBBox(el) {
                var bb = el.getBBox();
                var ctm = el.getCTM();
                var p1 = svg.createSVGPoint();
                var p2 = svg.createSVGPoint();
                p1.x = bb.x; p1.y = bb.y;
                p2.x = bb.x + bb.width; p2.y = bb.y + bb.height;
                var t1 = p1.matrixTransform(ctm);
                var t2 = p2.matrixTransform(ctm);
                return { x: t1.x, y: t1.y, r: t2.x, b: t2.y };
            }
    
            // Gather cluster data
            var data = [];
            clusters.forEach(function(cluster) {
                var rect = null, label = null;
                for (var i = 0; i < cluster.children.length; i++) {
                    var c = cluster.children[i];
                    if (!rect && c.tagName.toLowerCase() === 'rect') rect = c;
                    if (!label && c.classList && c.classList.contains('cluster-label')) label = c;
                }
                if (!rect || !label) return;
                data.push({
                    cluster: cluster,
                    rect: rect,
                    label: label,
                    rb: svgBBox(rect),
                    lb: svgBBox(label)
                });
            });
    
            // Sort by area ascending: fix innermost clusters first,
            // so their (possibly grown) rects are correct when outer clusters check.
            data.sort(function(a, b) {
                var aA = (a.rb.r - a.rb.x) * (a.rb.b - a.rb.y);
                var aB = (b.rb.r - b.rb.x) * (b.rb.b - b.rb.y);
                return aA - aB;
            });
    
            var minGap = 10;
    
            for (var i = 0; i < data.length; i++) {
                var parent = data[i];
                var labelBottom = parent.lb.b;
    
                for (var j = 0; j < data.length; j++) {
                    if (i === j) continue;
                    var child = data[j];
    
                    // Is child geometrically inside parent? (with tolerance)
                    var tol = 2;
                    if (child.rb.x >= parent.rb.x - tol &&
                        child.rb.y >= parent.rb.y - tol &&
                        child.rb.r <= parent.rb.r + tol &&
                        child.rb.b <= parent.rb.b + tol)
                    {
                        var overlap = labelBottom + minGap - child.rb.y;
                        if (overlap > 0) {
                            // Grow parent rect upward
                            var ry = parseFloat(parent.rect.getAttribute('y'));
                            var rh = parseFloat(parent.rect.getAttribute('height'));
                            parent.rect.setAttribute('y', String(ry - overlap));
                            parent.rect.setAttribute('height', String(rh + overlap));
    
                            // Move label up into the newly created space
                            var existing = parent.label.getAttribute('transform') || '';
                            parent.label.setAttribute('transform',
                                (existing ? existing + ' ' : '')
                                + 'translate(0,' + (-overlap) + ')');
    
                            // Update cached bounds for cascading fixes
                            parent.rb.y -= overlap;
                            parent.lb.y -= overlap;
                            parent.lb.b -= overlap;
                            labelBottom = parent.lb.b;
                        }
                    }
                }
            }
        }
    
        function normalizeSubgraphTitles(svg) {
            svg.querySelectorAll('.cluster-label').forEach(function(labelGroup) {
                var text = (labelGroup.textContent || '').replace(/\s+/g, ' ').trim();
                if (!text) return;

                // If Mermaid rendered the label as HTML inside foreignObject, force single-line layout.
                labelGroup.querySelectorAll('foreignObject *').forEach(function(el) {
                    el.textContent = text;
                    el.style.whiteSpace = 'nowrap';
                    el.style.wordBreak = 'normal';
                    el.style.overflowWrap = 'normal';
                    el.style.display = 'inline-block';
                });

                // If Mermaid rendered the label as SVG text/tspans, collapse it to one text node.
                var textEl = labelGroup.querySelector('text');
                if (textEl) {
                    while (textEl.firstChild) {
                        textEl.removeChild(textEl.firstChild);
                    }
                    textEl.textContent = text;
                }
            });
        }

        mermaid.initialize({
            startOnLoad: true,
            theme: ')" + mermaid_theme
                            + R"(',
                                flowchart: {
                                                useMaxWidth: false,
                subGraphTitleMargin: { top: 8, bottom: 8 }
                                            }
        });

        window.addEventListener('load', function () {
            var checkExist = setInterval(function() {
                var svg = document.querySelector('.mermaid svg');
                if (svg) {
                    clearInterval(checkExist);

                    fixSubgraphTitleOverlap(svg);
                    normalizeSubgraphTitles(svg);

                    svg.style.width = '100%';
                    svg.style.height = '100%';
                    svg.style.maxWidth = 'none';

                    window.panZoom = svgPanZoom(svg, {
                        zoomEnabled: true,
                        controlIconsEnabled: true,
                        fit: true,
                        center: true,
                        minZoom: 0.1,
                        maxZoom: 20
                    });
                }
            }, 100);
        });
    </script>

    <style>
        body {
            margin: 0;
            padding: 0;
            width: 100vw;
            height: 100vh;
            background: )" + body_bg
                            + R"(;
            color: )" + text_color
                            + R"(;
            font-family: sans-serif;
            overflow: hidden;
        }

        .mermaid {
            width: 100%;
            height: 100%;
            display: flex;
            justify-content: center;
            align-items: center;
        }

        .node rect, .node circle, .node ellipse, .node polygon, .node path {
            filter: )" + shadow_css
                            + R"(;
            transition: all 0.2s ease-in-out;
        }

        .node:hover rect, .node:hover circle, .node:hover ellipse, .node:hover polygon, .node:hover path {
            filter: drop-shadow(0px 0px 8px rgba(135, 206, 250, 0.6));
            stroke-width: 2.5px !important;
            cursor: grab;
        }

        .mermaid svg {
            cursor: grab;
        }
        .mermaid svg:active {
            cursor: grabbing;
        }
        
        .cluster-label foreignObject * {
            white-space: nowrap !important;
            word-break: normal !important;
            overflow-wrap: normal !important;
        }
    </style>
</head>
<body>
    <div class="mermaid">
)";

    const std::string html_footer = R"(
    </div>
</body>
</html>
)";

    std::ofstream out_file(file_name);
    if (out_file.is_open())
    {
        out_file << html_header << mermaid.str() << html_footer;
        out_file.close();
    }
}
