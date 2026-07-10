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