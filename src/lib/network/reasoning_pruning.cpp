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

#include "fact_structure.hpp"
#include "zelph_impl.hpp"

// Prune mode: what does the matched condition denote?
//
// resolve_pattern rather than a lookup in the bindings, because a pattern is
// not always a bare VARIABLE. `((Y r s) p Z)`, `(X (Y r s) Z)` and
// `(X p (Y r s))` all match -- unification resolves them structurally, the
// predicate included since it learned to -- but the reconstruction here
// substituted only where the pattern WAS a variable and then asked check_fact
// about the pattern node itself. It found nothing, and `.prune-facts`
// reported "Pruned 0 matching facts" for facts it had just matched.
//
// resolve_pattern is the read-only sibling of instantiate_fact: pure hash
// lookups, nothing created, which is what a prune pass may do.
void zelph::network::Reasoning::collect_prune_targets(const Node condition, const Variables& bindings, const Node parent)
{
    // `.prune-nodes A (...)`: the command named the variable whose bindings
    // die, so the matched condition says nothing about what to delete -- it
    // is one of several, and which one arrives here depends on the order
    // optimize_order chose. The other conditions are the FILTER that selected
    // the victim; their facts are not the user's to lose, and removing the
    // node takes the facts it is part of with it anyway. Hence: no
    // _facts_to_prune, and no reading of the condition at all.
    if (_prune_target_var != 0)
    {
        const auto it = bindings.find(_prune_target_var);
        if (it != bindings.end() && it->second != 0 && !Zelph::Impl::is_var(it->second))
        {
            _nodes_to_prune.insert(it->second);
        }
        return;
    }

    adjacency_set objects;
    const Node    subject_pattern  = parse_fact(condition, objects, parent);
    const Node    relation_pattern = parse_relation(condition);

    std::vector<Node> history;
    Node              subject  = 0;
    Node              relation = 0;

    if (resolve_pattern(this, subject_pattern, bindings, subject, history) != Resolve::Ok) return;

    history.clear();
    if (resolve_pattern(this, relation_pattern, bindings, relation, history) != Resolve::Ok) return;

    adjacency_set targets;
    for (const Node obj : objects)
    {
        history.clear();
        Node iobj = 0;
        if (resolve_pattern(this, obj, bindings, iobj, history) == Resolve::Ok && iobj != 0
            && !Zelph::Impl::is_var(iobj))
        {
            targets.insert(iobj);
        }
    }

    if (subject == 0 || relation == 0 || targets.empty()) return;
    if (Zelph::Impl::is_var(subject) || Zelph::Impl::is_var(relation)) return;

    const Answer ans = check_fact(subject, relation, targets);
    if (!ans.is_known() || ans.relation() == 0) return;

    _facts_to_prune.insert(ans.relation());

    if (_prune_nodes_mode)
    {
        // Unchanged from the three copies this replaces, parent-less call
        // included: which side is the variable decides what gets deleted.
        if (Zelph::Impl::is_var(parse_fact(condition, objects)))
            _nodes_to_prune.insert(subject);
        else if (objects.size() == 1)
            _nodes_to_prune.insert(*targets.begin());
    }
}

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

void Reasoning::prune_nodes(Node pattern, const Node target_var, size_t& removed_facts, size_t& removed_nodes)
{
    invalidate_fact_structures_cache();

    _prune_mode       = true;
    _prune_nodes_mode = true;
    _prune_target_var = target_var;
    _facts_to_prune.clear();
    _nodes_to_prune.clear();

    apply_rule(0, pattern);

    _pool->wait();

    removed_facts = _facts_to_prune.size();
    removed_nodes = 0;

    std::lock_guard<std::mutex> lock(_mtx_network);

    for (Node fact : _facts_to_prune)
    {
        _pImpl->remove(fact);
    }

    size_t kept_core_nodes = 0;

    for (Node node : _nodes_to_prune)
    {
        // The engine's own vocabulary is not data. A pattern as ordinary as
        // "A ~ ->" binds A to every declared relation type, the core
        // predicates among them, and deleting those leaves a network whose
        // next negation or list fails deep inside the engine. Skipped rather
        // than refused: a prune is a bulk operation, and aborting it halfway
        // leaves a graph nobody asked for.
        if (!get_core_name(node).empty())
        {
            ++kept_core_nodes;
            continue;
        }

        // A bound node can be gone before its turn: removing an earlier one
        // takes the facts it is part of with it, and a bound node may BE
        // such a fact -- "X rel o" binds X to `a` and to `(a p b)` alike.
        // Which of the two comes first is not fixed (the set is unordered),
        // so the case cannot be provoked reliably; remove_node REFUSES a
        // node that does not exist, so asking is not decoration.
        if (!_pImpl->exists(node)) continue;

        // remove_node, not Impl::remove: the names have to go with the
        // node, exactly as for the .remove command. Without that the name
        // still resolved to the deleted node, so ".node <name>" answered
        // with an empty node that is no longer in the graph.
        //
        // Its return value is what ACTUALLY went, which is more than one
        // whenever the node took part in a fact -- and those are facts, not
        // nodes the pattern named. Counting them as nodes made the two forms
        // of this command report different numbers for the same destruction:
        // the single-fact form removes the matched facts first, so its
        // remove_node returns 1 apiece, while a target variable leaves them
        // to their node and its remove_node returned 2. Same two victims,
        // "2 facts and 2 nodes" against "0 facts and 4 nodes".
        removed_facts += remove_node(node) - 1;
        ++removed_nodes;
    }

    if (kept_core_nodes > 0)
    {
        out_stream() << "Kept " << kept_core_nodes
                     << " core node(s) that the pattern matched; they are part of the engine, not data."
                     << std::endl;
    }

    _prune_mode       = false;
    _prune_nodes_mode = false;
    _prune_target_var = 0;
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

        // The facts that USE pred as their relation type -- not everything
        // pointing at it. For an atomic predicate the two coincide; a
        // COMPOSITE one is a fact as well, so its own subject and objects
        // point at it too, and the scan below then read those atoms as
        // malformed facts and PURGED them: `.cleanup` on a graph holding
        // `a p b` and `z (a p b) w` deleted `a` and `b`, taking both facts
        // with them. get_facts_of_predicate applies the exact role test.
        adjacency_set incoming_to_pred = get_facts_of_predicate(pred);

        if (incoming_to_pred.size() > 200000)
        {
            std::string name = get_name(pred, "wikidata", true);
            diagnostic_stream() << "[" << (i + 1) << "/" << all_predicates.size() << "] Checking "
                                << name << " (" << pred << ") with "
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
