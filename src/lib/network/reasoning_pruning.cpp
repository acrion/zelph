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

// A prune of a real dump runs for hours and used to say nothing at all until
// it was finished, so there was no way to tell a slow run from a stuck one
// from outside the process. Below this many victims nothing is reported: a
// small prune is over before a line could help, and the transcripts of the
// documentation would change for nothing.
namespace
{
    constexpr size_t prune_progress_step = 100000;
}

// The engine's own vocabulary is not data -- and neither is the one fact that
// makes it usable. "A ~ ->" binds A to every declared relation type, the core
// predicates among them, and prune_nodes has always kept those NODES for that
// reason. Their relation-type DECLARATIONS were pruned all the same, which
// left a graph whose core predicates are no longer read as predicates: no fact
// of `~`, `=>` or `in` is reconstructible any more. That stayed invisible only
// because the memoized relation-type set was not refreshed on removal; with it
// refreshed, `.prune-nodes A ~ ->` empties the network of readable facts.
bool Reasoning::is_core_declaration(const Node fact) const
{
    if (!is_relation_type_declaration(fact)) return false;

    adjacency_set objects;
    return !get_core_name(parse_fact(fact, objects, 0)).empty();
}

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

    // The matching is the long half on a real dump and says nothing while it
    // runs, so the count is worth a line the moment it is known -- the same
    // reason prune_nodes reports the size of its job.
    if (removed_count >= prune_progress_step)
        out_stream() << "Pruning " << removed_count << " matched fact(s)..." << std::endl;

    if (removed_count > 0)
    {
        std::lock_guard<std::mutex> lock(_mtx_network);
        bool                        declaration_removed = false;
        size_t                      kept_core           = 0;

        for (Node fact : _facts_to_prune)
        {
            if (is_core_declaration(fact))
            {
                ++kept_core;
                continue;
            }

            // Same question remove_node asks: a predicate that has lost its
            // relation-type declaration must stop being read as one. This path
            // removes facts directly, so it has to ask for itself.
            if (!declaration_removed && is_relation_type_declaration(fact)) declaration_removed = true;
            _pImpl->remove(fact);
        }

        removed_count -= kept_core;
        if (kept_core > 0)
            out_stream() << "Kept " << kept_core
                         << " relation-type declaration(s) of core predicates; they are part of the engine, not data."
                         << std::endl;

        if (declaration_removed) invalidate_relation_type_set();
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

    bool   declaration_removed = false;
    size_t kept_core_facts     = 0;
    for (Node fact : _facts_to_prune)
    {
        if (is_core_declaration(fact))
        {
            ++kept_core_facts;
            continue;
        }
        if (!declaration_removed && is_relation_type_declaration(fact)) declaration_removed = true;
        _pImpl->remove(fact);
    }
    removed_facts -= kept_core_facts;
    if (declaration_removed) invalidate_relation_type_set();

    size_t kept_core_nodes = 0;

    // The size of the job is known here, and it is what makes the progress
    // lines below mean anything.
    const bool report = _nodes_to_prune.size() >= prune_progress_step;

    if (report)
    {
        out_stream() << "Pruning " << _nodes_to_prune.size() << " node(s) and "
                     << removed_facts << " matched fact(s)..." << std::endl;
    }

    // Erasing a node's names costs a SCAN of the reverse name map, so a bulk
    // removal collects its victims and pays for one scan at the end -- see
    // Zelph::remove_node. Doing it per node is what made this run at one node
    // per second on the full Wikidata network.
    adjacency_set named_dead;

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
        removed_facts += remove_node(node, &named_dead) - 1;
        ++removed_nodes;

        if (report && removed_nodes % prune_progress_step == 0)
        {
            out_stream() << "  " << removed_nodes << " of " << _nodes_to_prune.size()
                         << " node(s) removed" << std::endl;
        }
    }

    if (report) out_stream() << "  removing the names of " << named_dead.size() << " node(s)..." << std::endl;

    remove_names_of(named_dead);

    if (kept_core_nodes > 0 || kept_core_facts > 0)
    {
        out_stream() << "Kept " << kept_core_nodes
                     << " core node(s) and " << kept_core_facts
                     << " of their relation-type declaration(s) that the pattern matched; "
                        "they are part of the engine, not data."
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

    // A removed predicate is no longer a relation type, whether or not its
    // declaration fact was among the zombies: the memoized set holds the
    // predicate NODE, and that node is gone.
    if (removed_predicates > 0) invalidate_relation_type_set();
}
