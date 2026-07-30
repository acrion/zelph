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

#include "zelph.hpp"

#include "zelph_impl.hpp"

#include <algorithm>

using namespace zelph::network;

void Zelph::cleanup_isolated(size_t& removed_count) const
{
    removed_count = 0;

    invalidate_fact_structures_cache();

    _pImpl->remove_isolated_nodes(removed_count);
}

size_t Zelph::cleanup_names() const
{
    return _pImpl->cleanup_dangling_names();
}

void Zelph::remove_node(Node node) const
{
    if (!_pImpl->exists(node))
    {
        throw std::runtime_error("Cannot remove non-existent node " + std::to_string(node));
    }

    invalidate_fact_structures_cache();

    _pImpl->remove(node);            // Disconnects edges and removes from adjacency maps
    _pImpl->remove_node_names(node); // Separate method for name cleanup
}

// Returns all nodes that are subjects of a core.Causes relation
adjacency_set Zelph::get_rules() const
{
    const adjacency_set& rule_candidates = _pImpl->get_left(core.Causes);

    adjacency_set rules;

    for (Node rule_candidate : rule_candidates)
    {
        // We filter the rule candidates in the same way as Reasoning::apply_rule() does it.
        // Note that a rule candidate with empty deductions is interpreted as a question, see Reasoning::evaluate()
        if (rule_candidate)
        {
            adjacency_set deductions;
            Node          condition = parse_fact(rule_candidate, deductions);
            if (condition && condition != core.Causes && !deductions.empty())
            {
                rules.insert(rule_candidate);
            }
        }
    }

    return rules;
}

void Zelph::remove_rules() const
{
    adjacency_set rules = get_rules();
    for (Node rule : rules)
    {
        invalidate_fact_structures_cache();

        _pImpl->remove(rule);
        // Clean up names
        for (auto& lang_map : _pImpl->_name_of_node)
        {
            lang_map.second.erase(rule);
        }
        for (auto& lang_map : _pImpl->_node_of_name)
        {
            for (auto it = lang_map.second.begin(); it != lang_map.second.end();)
            {
                if (it->second == rule)
                {
                    it = lang_map.second.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
}

size_t Zelph::rule_count() const
{
    return get_rules().size();
}

#ifndef __EMSCRIPTEN__
void Zelph::save_to_file(const std::string& filename) const
{
    _pImpl->saveToFile(filename);
}

// A slice is a network in its own right that contains the facts of the given
// predicates, the nodes those facts connect, every name of those nodes, and
// nothing else. It exists because the interesting questions about a large
// graph rarely need all of it: the class hierarchy of Wikidata is 5.2 million
// P279 facts inside 88 GB, and once it stands alone it fits in a fraction of
// the memory while answering the same queries with the same engine.
//
// Two things have to travel with the facts or the result is a network that
// looks complete and answers nothing:
//
//   * the relation-type declaration of each predicate -- fact structures are
//     only reconstructed for declared predicates, so without it every query
//     over the slice returns empty. It is addressed by its content hash
//     rather than searched: core.IsA's adjacency holds every instance-of fact
//     of a mapped Wikidata network, and scanning that to find one edge would
//     cost a full pass over the graph.
//   * the structural closure of everything retained -- a fact whose subject
//     is itself a fact (a rule, a qualified statement) drags in the nodes it
//     is built from, or the loaded slice has a node whose structure cannot be
//     read back.
size_t Zelph::save_predicate_slice(const std::string& filename, const std::vector<Node>& predicates) const
{
    ankerl::unordered_dense::set<Node> keep{
        core.RelationTypeCategory, core.Causes, core.IsA, core.Unequal, core.Contradiction,
        core.Cons, core.Nil, core.PartOf, core.Conjunction, core.Negation};

    size_t facts = 0;

    {
        // Same lock order as the writers: left before right.
        std::shared_lock<std::shared_mutex> lock_left(_pImpl->_smtx_left);
        std::shared_lock<std::shared_mutex> lock_right(_pImpl->_smtx_right);

        const auto exists_unlocked = [this](const Node nd)
        { return _pImpl->_left.find(nd) != _pImpl->_left.end(); };

        std::vector<Node> pending;

        const auto retain = [&keep, &pending](const Node nd)
        {
            if (keep.insert(nd).second) pending.push_back(nd);
        };

        for (const Node p : predicates) retain(p);

        // The declarations that make the predicates usable, including the
        // core ones (a freshly constructed network re-creates those, but a
        // load replaces the whole state, so they have to be in the file).
        std::vector<Node> declared(predicates);
        declared.insert(declared.end(), {core.IsA, core.Unequal, core.Causes, core.Cons, core.PartOf});
        for (const Node nd : declared)
        {
            const Node declaration = create_hash(core.IsA, nd, {core.RelationTypeCategory});
            if (exists_unlocked(declaration)) retain(declaration);
        }

        for (const Node p : predicates)
        {
            const auto rels_it = _pImpl->_right.find(p);
            if (rels_it == _pImpl->_right.end()) continue;

            for (const Node rel : rels_it->second)
            {
                const auto rel_left  = _pImpl->_left.find(rel);
                const auto rel_right = _pImpl->_right.find(rel);
                if (rel_left == _pImpl->_left.end() || rel_right == _pImpl->_right.end()) continue;

                if (rel_left->second.count(p) == 0) continue;

                // rel can also be a fact ABOUT p -- the declaration above, or
                // "P279 is a transitive relation". Those carry p as their
                // SUBJECT, which puts it on both sides of rel just like the
                // predicate position does, so the two are told apart the same
                // way the index builder does it: a fact OF p has a subject
                // that is not p and stands on both sides of rel.
                const bool is_fact_of_p =
                    std::any_of(rel_left->second.begin(), rel_left->second.end(),
                                [&](const Node candidate)
                                { return candidate != p
                                      && !Impl::is_var(candidate)
                                      && rel_right->second.count(candidate) == 1; });
                if (!is_fact_of_p) continue;

                retain(rel);
                ++facts;
                for (const Node nd : rel_left->second) retain(nd);
                for (const Node nd : rel_right->second) retain(nd);
            }
        }

        // Structural closure: expand retained fact nodes until nothing new
        // appears. Plain nodes are not expanded -- their adjacency is the
        // rest of the graph, which is exactly what the slice leaves behind.
        while (!pending.empty())
        {
            const Node nd = pending.back();
            pending.pop_back();
            if (!Impl::is_hash(nd)) continue;

            const auto left_it = _pImpl->_left.find(nd);
            if (left_it != _pImpl->_left.end())
            {
                for (const Node participant : left_it->second) retain(participant);
            }

            const auto right_it = _pImpl->_right.find(nd);
            if (right_it != _pImpl->_right.end())
            {
                for (const Node participant : right_it->second) retain(participant);
            }
        }
    }

    _pImpl->saveToFile(filename, &keep);

    return facts;
}

void Zelph::load_from_file(const std::string& filename) const
{
    invalidate_fact_structures_cache();

    _pImpl->loadFromFile(filename);
}

void Zelph::load_from_file(const std::string& filename, const BinChunkSelection& selection, const bool skip_payload) const
{
    invalidate_fact_structures_cache();

    _pImpl->loadFromFile(filename, selection, skip_payload);
}

void Zelph::load_from_manifest(const std::string&       manifest_path,
                               const BinChunkSelection& selection,
                               const std::string&       shard_root,
                               const std::string&       bin_path_override,
                               const bool               skip_payload) const
{
    invalidate_fact_structures_cache();

    _pImpl->loadFromManifest(manifest_path, selection, shard_root, bin_path_override, skip_payload);
}
#endif

void        Zelph::set_active_cluster(const std::string& name) const { _pImpl->set_active_cluster(name); }
void        Zelph::deactivate_cluster() const { _pImpl->deactivate_cluster(); }
std::string Zelph::active_cluster_name() const { return _pImpl->active_cluster_name(); }

std::vector<std::pair<std::string, size_t>> Zelph::list_clusters() const { return _pImpl->list_clusters(); }

bool Zelph::merge_cluster(const std::string& from, const std::string& to) const
{
    return _pImpl->merge_cluster(from, to);
}

// Destructive: removes every node recorded in the cluster, including all
// of their edges and names. Nodes that no longer exist (e.g. merged away
// by set_name) are skipped silently.
size_t Zelph::drop_cluster(const std::string& name) const
{
    const std::vector<Node> nodes = _pImpl->take_cluster(name);
    if (nodes.empty()) return 0;

    invalidate_fact_structures_cache();

    size_t removed = 0;
    for (const Node n : nodes)
    {
        if (_pImpl->exists(n))
        {
            remove_node(n);
            ++removed;
        }
    }
    return removed;
}