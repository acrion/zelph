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

#include "reasoning.hpp"

#include "zelph_impl.hpp"

using namespace zelph::network;

void Reasoning::prune_facts(Node pattern, size_t& removed_count)
{
    invalidate_fact_structures_cache();

    _prune_mode       = true;
    _prune_nodes_mode = false;
    _facts_to_prune.clear();
    _nodes_to_prune.clear();

    apply_rule(0, pattern);

    _pool->wait();

    removed_count = _facts_to_prune.size();

    if (removed_count > 0)
    {
        std::lock_guard<std::mutex> lock(_mtx_network);
        for (Node fact : _facts_to_prune)
        {
            _pImpl->remove(fact);
        }
    }

    _prune_mode = false;
}

void Reasoning::prune_nodes(Node pattern, size_t& removed_facts, size_t& removed_nodes)
{
    invalidate_fact_structures_cache();

    _prune_mode       = true;
    _prune_nodes_mode = true;
    _facts_to_prune.clear();
    _nodes_to_prune.clear();

    apply_rule(0, pattern);

    _pool->wait();

    removed_facts = _facts_to_prune.size();
    removed_nodes = _nodes_to_prune.size();

    std::lock_guard<std::mutex> lock(_mtx_network);

    for (Node fact : _facts_to_prune)
    {
        _pImpl->remove(fact);
    }

    for (Node node : _nodes_to_prune)
    {
        _pImpl->remove(node);
    }

    _prune_mode       = false;
    _prune_nodes_mode = false;
}

void Reasoning::purge_unused_predicates(size_t& removed_facts, size_t& removed_predicates)
{
    invalidate_fact_structures_cache();

    removed_facts      = 0;
    removed_predicates = 0;

    std::vector<Node> all_predicates;

    {
        std::lock_guard<std::mutex> lock(_mtx_network);

        adjacency_set def_facts = _pImpl->get_right(core.RelationTypeCategory);

        for (Node def_fact : def_facts)
        {
            if (_pImpl->get_right(def_fact).count(core.IsA) == 1)
            {
                for (Node cand : _pImpl->get_left(def_fact))
                {
                    if (cand != core.RelationTypeCategory && cand != core.IsA)
                    {
                        if (_pImpl->get_right(cand).count(def_fact) == 1)
                        {
                            all_predicates.push_back(cand);
                        }
                    }
                }
            }
        }
    }

    auto is_protected = [&](Node n)
    {
        return n == core.IsA || n == core.Causes || n == core.RelationTypeCategory || n == core.Unequal || n == core.Contradiction || n == core.Cons || n == core.Nil || n == core.PartOf || n == core.Conjunction;
    };

    diagnostic_stream() << "Found " << all_predicates.size() << " predicates. Starting deep scan..." << std::endl;

    std::lock_guard<std::mutex> lock(_mtx_network);

    for (size_t i = 0; i < all_predicates.size(); ++i)
    {
        Node pred = all_predicates[i];

        if (is_protected(pred)) continue;

        adjacency_set incoming_to_pred = _pImpl->get_left(pred);

        if (incoming_to_pred.size() > 200000)
        {
            std::wstring name = get_name(pred, "wikidata", true);
            diagnostic_stream() << "[" << (i + 1) << "/" << all_predicates.size() << "] Checking "
                                << string::unicode::to_utf8(name) << " (" << pred << ") with "
                                << incoming_to_pred.size() << " entries..." << std::endl;
        }

        size_t valid_usage_count = 0;
        size_t local_removed     = 0;

        for (Node fact : incoming_to_pred)
        {
            if (_pImpl->get_right(fact).count(core.IsA) == 1)
            {
                continue;
            }

            bool is_zombie = false;

            adjacency_set incoming_to_fact = _pImpl->get_left(fact);
            if (incoming_to_fact.empty())
            {
                is_zombie = true;
            }
            else
            {
                adjacency_set outgoing_from_fact = _pImpl->get_right(fact);

                bool has_subject = false;
                bool has_object  = false;

                for (Node out_node : outgoing_from_fact)
                {
                    if (out_node == pred) continue;

                    if (incoming_to_fact.count(out_node) == 1)
                    {
                        has_subject = true;
                        break;
                    }
                }

                if (has_subject)
                {
                    for (Node in_node : incoming_to_fact)
                    {
                        if (outgoing_from_fact.count(in_node) == 0)
                        {
                            has_object = true;
                            break;
                        }
                    }
                    if (!has_object)
                    {
                        // Self-referential: all incoming nodes are also outgoing (bidirectional subject only).
                        has_object = std::all_of(incoming_to_fact.begin(), incoming_to_fact.end(), [&](Node n)
                                                 { return outgoing_from_fact.count(n) != 0; });
                    }
                }

                if (!has_subject || !has_object)
                {
                    is_zombie = true;
                }
            }

            if (is_zombie)
            {
                _pImpl->remove(fact);
                local_removed++;
            }
            else
            {
                valid_usage_count++;
            }
        }

        removed_facts += local_removed;

        if (local_removed > 0 && incoming_to_pred.size() > 200000)
        {
            out_stream() << "   -> Purged " << local_removed << " broken facts." << std::endl;
        }

        if (valid_usage_count == 0)
        {
            _pImpl->remove(pred);
            removed_predicates++;
        }
    }
}
