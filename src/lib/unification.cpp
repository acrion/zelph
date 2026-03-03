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

#include "unification.hpp"
#include "fact_structure.hpp"
#include "string_utils.hpp"
#include "zelph_impl.hpp"

#include <iostream>
#include <vector>

using namespace zelph::network;

#include <string>
static void u_log(int depth, const std::string& msg)
{
    std::string indent(depth * 2, ' ');
    std::clog << indent << "[depth " << depth << ", Unify] " << msg << std::endl;
}

static std::string u_node_str(const Zelph* z, Node n)
{
    if (n == 0) return "0";
    if (Zelph::Impl::is_var(n)) return "VAR(" + z->format(n) + ")";
    std::string name = zelph::string::unicode::to_utf8(z->get_name(n, "zelph", true));
    if (name.empty()) name = z->format(n);
    if (name.empty())
        return std::to_string(n);
    else
        return name;
}
#define U_LOG(depth, msg) \
    if (_n->should_log(depth)) { u_log(depth, msg); }
#define U_NODE(n) u_node_str(_n, n)

// --- Helper Functions ---

// Recursive Unification Algorithm with Cycle Detection
static bool unify_nodes(
    const Zelph* const                  _n,
    Node                                rule_node,
    Node                                graph_node,
    Variables&                          local_bindings,
    const Variables&                    global_bindings,
    std::vector<std::pair<Node, Node>>& history,
    int                                 depth,
    ReasoningProfiler&                  prof)
{
    if (_n->logging_active())
    {
        prof.unify_calls.fetch_add(1, std::memory_order_relaxed);
        ReasoningProfiler::atomic_max(prof.max_unify_depth, (uint64_t)depth);
    }

    if (rule_node == 0 || graph_node == 0) return false;

    U_LOG(depth, "Comparing " + U_NODE(rule_node) + " vs " + U_NODE(graph_node));

    // 0. Cycle Check
    // If we already check this exact pair (rule, graph) in this path, the cycle is closed
    // and we have not found any contradictions so far -> Success.
    for (const auto& pair : history)
    {
        if (pair.first == rule_node && pair.second == graph_node)
        {
            if (_n->logging_active())
            {
                prof.unify_cycle_hits.fetch_add(1, std::memory_order_relaxed);

                U_LOG(depth, "  -> Cycle detected (already visiting), assuming match.");
            }
            return true;
        }
    }
    history.emplace_back(rule_node, graph_node);

    bool result = false; // Default result

    // Scope for RAII-like pop (manually at the end)
    do
    {
        // 1. Variable Binding
        if (Zelph::Impl::is_var(rule_node))
        {
            if (_n->logging_active())
                prof.unify_var_seen.fetch_add(1, std::memory_order_relaxed);

            if (local_bindings.count(rule_node))
            {
                if (_n->logging_active())
                {
                    prof.unify_var_local_recurse.fetch_add(1, std::memory_order_relaxed);
                    U_LOG(depth, "  Var local bound -> recursing");
                }
                result = unify_nodes(_n, local_bindings[rule_node], graph_node, local_bindings, global_bindings, history, depth + 1, prof);
                break;
            }
            if (global_bindings.count(rule_node))
            {
                if (_n->logging_active())
                    prof.unify_var_global_recurse.fetch_add(1, std::memory_order_relaxed);

                Node bound = zelph::string::get(global_bindings, rule_node, Node{0});
                if (bound == graph_node)
                {
                    U_LOG(depth, "  -> Var global bound, identical to graph node");
                    result = true;
                    break;
                }
                U_LOG(depth, "  Var global bound to " + U_NODE(bound) + " (id=" + std::to_string(bound) + ") -> recursing with graph_node " + U_NODE(graph_node) + " (id=" + std::to_string(graph_node) + ")");
                result = unify_nodes(_n, bound, graph_node, local_bindings, global_bindings, history, depth + 1, prof);
                if (_n->should_log(depth) && !result)
                {
                    u_log(depth, "  DIAGNOSTIC DUMP: rule_node=" + std::to_string(rule_node) + " global_bindings has " + std::to_string(global_bindings.size()) + " entries:");
                    for (const auto& [k, v] : global_bindings)
                        u_log(depth, "    key=" + std::to_string(k) + " (" + U_NODE(k) + ") -> val=" + std::to_string(v) + " (" + U_NODE(v) + ")");
                }
                break;
            }

            local_bindings[rule_node] = graph_node;

            if (_n->logging_active())
            {
                prof.unify_var_bound_new.fetch_add(1, std::memory_order_relaxed);
                U_LOG(depth, "  -> Bound " + U_NODE(rule_node) + " to " + U_NODE(graph_node));
            }

            result = true;
            break;
        }

        // 2. Direct identity
        if (rule_node == graph_node)
        {
            if (_n->logging_active())
            {
                prof.unify_identity_hits.fetch_add(1, std::memory_order_relaxed);
                U_LOG(depth, "  -> Identical");
            }
            result = true;
            break;
        }

        // 3. Structural equivalence
        auto rule_structs = get_fact_structures(_n, rule_node, false, depth);
        if (rule_structs.empty())
        {
            U_LOG(depth, "  -> Rule node is atom, but not identical. Fail.");
            result = false;
            break;
        }

        auto graph_structs = get_fact_structures(_n, graph_node, false, depth);
        if (graph_structs.empty())
        {
            U_LOG(depth, "  -> Graph node is atom, but rule expects structure. Fail.");
            result = false;
            break;
        }

        if (_n->logging_active())
            prof.unify_struct_pair_attempts.fetch_add(1, std::memory_order_relaxed);

        for (const auto& rs : rule_structs)
        {
            for (const auto& gs : graph_structs)
            {
                Variables attempt = local_bindings;

                // A. Predicate
                if (!unify_nodes(_n, rs.predicate, gs.predicate, attempt, global_bindings, history, depth + 1, prof)) continue;

                // B. Subject
                if (!unify_nodes(_n, rs.subject, gs.subject, attempt, global_bindings, history, depth + 1, prof)) continue;

                // C. Objects
                if (rs.objects.empty() != gs.objects.empty()) continue;

                bool      objects_match = true;
                Variables obj_bindings  = attempt;

                if (_n->logging_active())
                    prof.unify_object_try.fetch_add(1, std::memory_order_relaxed);

                // Greedy Match for Objects
                for (Node r_obj : rs.objects)
                {
                    bool found = false;
                    for (Node g_obj : gs.objects)
                    {
                        Variables try_obj = obj_bindings; // allow backtracking per object choice
                        if (unify_nodes(_n, r_obj, g_obj, try_obj, global_bindings, history, depth + 1, prof))
                        {
                            obj_bindings = std::move(try_obj);
                            found        = true;

                            if (_n->logging_active())
                            {
                                prof.unify_object_success.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                    }
                    if (!found)
                    {
                        objects_match = false;
                        break;
                    }
                }

                if (objects_match)
                {
                    local_bindings = std::move(obj_bindings);
                    U_LOG(depth, "    -> Structure Match SUCCESS");
                    result = true;
                    goto end_loop;
                }
            }
        }
    end_loop:;
    } while (false);

    history.pop_back();
    return result;
}

// Returns true if nd is itself a VAR, or if nd is a hash node whose
// immediate structural subject or objects contain a VAR node.
// Used to reject rule-template fact nodes from the unification candidate set.
static bool contains_variable_shallow(Zelph* n, Node nd, const int depth)
{
    if (nd == 0) return false;
    if (Zelph::Impl::is_var(nd)) return true;
    if (!Zelph::Impl::is_hash(nd)) return false; // plain atom → no internal structure

    auto structs = get_fact_structures(n, nd, false, depth);
    for (const auto& fs : structs)
    {
        if (Zelph::Impl::is_var(fs.subject)) return true;
        for (Node o : fs.objects)
            if (Zelph::Impl::is_var(o)) return true;
    }
    return false;
}

Unification::Unification(
    Zelph*                            n,
    Node                              condition,
    Node                              parent,
    const std::shared_ptr<Variables>& variables,
    const std::shared_ptr<Variables>& unequals,
    ThreadPool*                       pool,
    int                               log_depth,
    ReasoningProfiler&                profiler)
    : _n(n), _parent(parent), _variables(variables), _unequals(unequals), _pool(pool), _log_depth(log_depth), _prof(profiler)
{
    if (_n->logging_active())
        _prof.unification_instances.fetch_add(1, std::memory_order_relaxed);

    // Use get_preferred_structure to robustly decompose the RULE PATTERN (condition).
    // This handles cases where the pattern itself is a complex structure (like (A cons R)).
    FactStructure fs = get_preferred_structure(_n, condition, _log_depth);

    if (fs.predicate != 0)
    {
        Node relation = fs.predicate;
        _subject      = fs.subject;
        _objects      = fs.objects;

        if (_n->logging_active() && !Zelph::Impl::is_var(relation))
            _current_rel_ctx = relation;

        U_LOG(_log_depth, "Init Unification: " + U_NODE(condition));

        if (Zelph::Impl::is_var(relation))
        {
            // the relation is a variable, so fill _relation_list with all possible relations (excluding variables)
            _relation_list     = _n->get_sources(_n->core.IsA, _n->core.RelationTypeCategory, true);
            _relation_variable = relation;
        }
        else
        {
            _relation_list.insert(relation); // leaving _relation_variable==0, which denotes that this condition specifies the relation (instead of using a variable for it)

            if (relation == _n->core.Unequal)
            {
                for (Node object : _objects)
                    (*_unequals)[_subject] = object;
            }
        }
    }
    else
    {
        // Fallback or error logging if structure cannot be determined
        if (_n->should_log(1))
            _n->log(1, "Unify", "get_preferred_structure failed for rule condition: " + _n->format(condition));
    }

    if (_relation_variable != 0)
    {
        auto it = _variables->find(_relation_variable);
        if (it != _variables->end())
        {
            Node bound = it->second;
            if (_relation_list.count(bound))
            {
                _relation_list     = {bound};
                _relation_variable = 0;
            }
            else
            {
                _relation_list.clear();
            }
        }
    }

    if (_n->should_log(1) && _n->should_log(_log_depth - (_relation_list.empty() ? 0 : 1)))
    {
        std::string rels_str;
        for (Node r : _relation_list)
            rels_str += " " + U_NODE(r);
        u_log(_log_depth, "Unification: condition=" + _n->format(condition) + "subject=" + U_NODE(_subject) + " relations: [" + rels_str + "] objects=" + std::to_string(_objects.size()) + " parent=" + _n->format(parent));
    }

    if (_relation_list.empty()) return;

    // Always initialize sequential fallback
    _relation_index         = _relation_list.begin();
    _fact_index_initialized = false;

    if (_n->use_parallel())
    {
        // Subject/Object Driven Indexing
        //  parallel only with fixed relation
        //  OPTIMIZATION: Do NOT use parallel processing/snapshotting if subject is bound, as the result set is likely tiny.
        bool subject_is_bound = false;
        if (_subject != 0)
        {
            Node s = _subject;
            if (Zelph::Impl::is_var(s)) s = string::get(*_variables, s, s);
            // Only optimize if s is an ATOM (not a structure), because complex structures
            // cannot be looked up simply via get_right(s) in a deep unification context.
            if (!Zelph::Impl::is_var(s) && get_fact_structures(_n, s, false, log_depth).empty()) subject_is_bound = true;
        }

        // Do not use parallel if the object is bound (const or bound var).
        // In this case, an object-driven index lookup (in sequential mode) is much faster than scanning the relation.
        bool object_is_bound = false;
        for (Node o : _objects)
        {
            if (!Zelph::Impl::is_var(o))
            {
                if (get_fact_structures(_n, o, false, log_depth).empty())
                {
                    object_is_bound = true;
                    break;
                }
            }
            else if (_variables->find(o) != _variables->end())
            {
                object_is_bound = true;
                break;
            }
        }

        if (_pool && _relation_variable == 0 && !subject_is_bound && !object_is_bound && !tl_is_pool_worker)
        {
            Node fixed_rel = *_relation_list.begin();

            auto snap_start = std::chrono::steady_clock::now();

            adjacency_set snapshot;
            if (!_n->_pImpl->snapshot_left_of(fixed_rel, snapshot))
            {
                return;
            }

            auto   snap_end = std::chrono::steady_clock::now();
            double snap_ms  = std::chrono::duration<double, std::milli>(snap_end - snap_start).count();

            if (snap_ms > 100) // Only log significant snapshots
            {
                std::clog << "[Timer] Unification snapshot " << fixed_rel
                          << " size=" << snapshot.size()
                          << " took=" << snap_ms << "ms" << std::endl;
            }

            if (snapshot.size() > 0)
            {
                if (_n->logging_active())
                {
                    _prof.unification_parallel_instances.fetch_add(1, std::memory_order_relaxed);
                    _prof.relation_snapshots.fetch_add(1, std::memory_order_relaxed);
                    _prof.snapshot_full_relation.fetch_add(1, std::memory_order_relaxed);
                    _prof.snapshot_facts_total.fetch_add(_snapshot_vec.size(), std::memory_order_relaxed);
                    if (_current_rel_ctx) _prof.note_relation_scan(_current_rel_ctx, _snapshot_vec.size());
                }

                _use_parallel = true;
                _snapshot_vec.assign(snapshot.begin(), snapshot.end());

                if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                {
                    u_log(_log_depth, "parallel snapshot: " + std::to_string(_snapshot_vec.size()) + " candidate facts for relation " + U_NODE(fixed_rel));
                }

                size_t threads    = std::thread::hardware_concurrency();
                size_t chunks     = std::min(threads * 4, snapshot.size());
                size_t chunk_size = snapshot.size() / chunks;
                _active_tasks     = chunks;

                for (size_t c = 0; c < chunks; ++c)
                {
                    size_t start = c * chunk_size;
                    size_t end   = (c + 1 == chunks) ? _snapshot_vec.size() : (c + 1) * chunk_size;

                    _pool->enqueue([this, fixed_rel, start, end]()
                                   {
                                   uint64_t local_scanned = 0;
                                   for (size_t i = start; i < end; ++i)
                                   {
                                       Node fact = _snapshot_vec[i];
                                       auto structs = get_fact_structures(_n, fact, /*prefer_single=*/false, _log_depth);
                                       ++local_scanned;

                                       for (const auto& fs : structs)
                                       {
                                           if (fs.predicate != fixed_rel) continue;

                                           auto result = extract_bindings(fs.subject, fs.objects, fixed_rel, _log_depth);
                                           if (result)
                                           {
                                               std::lock_guard<std::mutex> l(_queue_mtx);
                                               _match_queue.push(std::move(result));
                                               _queue_cv.notify_one();
                                           }
                                       }
                                   }
                                   if (_n->logging_active())
                                   {
                                       _prof.facts_scanned_parallel.fetch_add(local_scanned, std::memory_order_relaxed);
                                   }
                                   {
                                       std::lock_guard<std::mutex> l(_queue_mtx);
                                       --_active_tasks;
                                       _queue_cv.notify_all();
                                   } });
                }
            }
        }
    }
}

bool Unification::increment_fact_index()
{
    if (_relation_index == _relation_list.end())
    {
        return false;
    }

    do
    {
        if (!_fact_index_initialized)
        {
            // Check if the Subject or Object is already bound. If so, iterate only their connections.
            bool optimized_snapshot = false;
            Node current_rel        = *_relation_index;

            if (_n->use_parallel())
            {
                auto is_relation_type = [&](Node x) -> bool
                {
                    return _n->check_fact(x, _n->core.IsA, {_n->core.RelationTypeCategory}).is_correct();
                };

                auto is_in_current_rule_template = [&](Node needle) -> bool
                {
                    if (_parent == 0 || needle == 0) return false;

                    if (Zelph::Impl::is_var(needle))
                        needle = string::get(*_variables, needle, needle);

                    if (needle == 0) return false;

                    if (is_relation_type(needle)) return false;

                    std::deque<Node>         q;
                    std::unordered_set<Node> seen;
                    q.push_back(_parent);

                    Node membership_pred = 0;
                    {
                        std::unordered_set<Node> preds;
                        for (Node rel : _n->get_right(_parent)) // Object -> Relation (incoming only => object has outgoing to rel)
                        {
                            zelph::network::FactStructure fs;
                            if (!zelph::network::try_get_preferred_structure(_n, rel, fs, _log_depth)) continue;

                            // _parent must be an object (incoming only => in objects, but not bidirectional)
                            if (fs.objects.count(_parent) == 0) continue;

                            // Predicate must be a RelationType
                            if (!is_relation_type(fs.predicate)) continue;

                            preds.insert(fs.predicate);
                        }
                        if (preds.size() == 1) membership_pred = *preds.begin();
                    }

                    while (!q.empty())
                    {
                        Node x = q.front();
                        q.pop_front();

                        if (x == 0) continue;
                        if (!seen.insert(x).second) continue;

                        if (x == needle) return true;

                        // 1) If x is the conjunction set (_parent): expand over its membership facts
                        if (x == _parent)
                        {
                            for (Node rel : _n->get_right(_parent))
                            {
                                zelph::network::FactStructure fs;
                                if (!zelph::network::try_get_preferred_structure(_n, rel, fs, _log_depth)) continue;
                                if (fs.objects.count(_parent) == 0) continue;

                                if (membership_pred != 0 && fs.predicate != membership_pred) continue;

                                q.push_back(rel);
                                q.push_back(fs.subject);
                                // TODO: Add more objects (besides _parent)?
                            }
                            continue;
                        }

                        // 2) If x is a fact node: expand syntactically into subject/objects.
                        //    Important: We do not run outward from atoms.
                        {
                            zelph::network::FactStructure fx;
                            if (zelph::network::try_get_preferred_structure(_n, x, fx, _log_depth))
                            {
                                q.push_back(fx.subject);
                                for (Node o : fx.objects)
                                    q.push_back(o);
                            }
                        }
                    }

                    return false;
                };

                // Rule template nodes exist in the graph.
                // The subject/object-driven shortcut is only correct if the lookup node
                // is NOT part of the current rule template.
                auto is_concrete_lookup_node = [&](Node nd) -> bool
                {
                    if (nd == 0) return false;

                    if (Zelph::Impl::is_var(nd))
                        nd = string::get(*_variables, nd, nd);

                    if (nd == 0 || Zelph::Impl::is_var(nd) || !_n->exists(nd))
                        return false;

                    if (is_in_current_rule_template(nd))
                    {
                        if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                            u_log(_log_depth, "is_concrete_lookup_node: REJECT (template) " + U_NODE(nd));
                        return false;
                    }

                    return true;
                };

                // Check if Subject is bound
                Node s = _subject;
                if (Zelph::Impl::is_var(s)) s = string::get(*_variables, s, s);
                if (is_concrete_lookup_node(s))
                {
                    // Strategy: Subject Driven
                    // Zelph::fact -> connect(Subject, Fact). Subject points to Fact (Source->Fact).
                    // So get_right(Subject) contains the Facts involving this subject.
                    adjacency_set candidates = _n->get_right(s);
                    _facts_snapshot.clear();
                    // Filter candidates: we only want facts that are of type 'current_rel'
                    for (Node fact : candidates)
                    {
                        if (_n->_pImpl->get_left(fact).count(current_rel) == 1) continue;

                        // Zelph::fact -> connect(Fact, RelationType). Fact points to RelationType.
                        // So get_right(Fact) contains RelationType.
                        if (_n->get_right(fact).count(current_rel) == 1)
                        {
                            _facts_snapshot.insert(fact);
                        }
                    }
                    optimized_snapshot = true;
                    if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                    {
                        u_log(_log_depth,
                              std::string("optimized_snapshot=") + (optimized_snapshot ? "YES" : "NO") + " rel=" + U_NODE(current_rel) + " subj=" + U_NODE(s) + (optimized_snapshot ? " size=" + std::to_string(_facts_snapshot.size()) : ""));
                    }
                }
                // Check if Object is bound (if Subject wasn't)
                else if (!_objects.empty())
                {
                    Node o = *_objects.begin();
                    if (Zelph::Impl::is_var(o)) o = string::get(*_variables, o, o);

                    if (is_concrete_lookup_node(o))
                    {
                        // Strategy: Object Driven
                        // Zelph::fact -> connect(Object, Fact). Object points to Fact.
                        adjacency_set candidates = _n->get_right(o);
                        _facts_snapshot.clear();
                        for (Node fact : candidates)
                        {
                            if (_n->_pImpl->get_left(fact).count(current_rel) == 1) continue;

                            if (_n->get_right(fact).count(current_rel) == 1)
                            {
                                _facts_snapshot.insert(fact);
                            }
                        }
                        optimized_snapshot = true;
                        if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                        {
                            u_log(_log_depth,
                                  std::string("optimized_snapshot=") + (optimized_snapshot ? "YES" : "NO") + " rel=" + U_NODE(current_rel) + " obj=" + U_NODE(o) + (optimized_snapshot ? " size=" + std::to_string(_facts_snapshot.size()) : ""));
                        }
                    }
                }
            }

            if (!optimized_snapshot)
            {
                // Fallback: Snapshot entire relation extent (slow for huge relations)
                if (!_n || !_n->_pImpl || !_n->_pImpl->snapshot_left_of(current_rel, _facts_snapshot))
                {
                    return false; // there is a relation without any facts that use it (might happen if it has been explicitly defined via fact(r, core.IsA, core.RelationType))
                }
            }

            // If the snapshot is empty,
            // we must not initialize the iterator to begin() and then check *_fact_index,
            // because begin() == end(), and dereferencing end() crashes.
            if (_facts_snapshot.empty())
            {
                return false;
            }

            if (_n->logging_active())
            {
                if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                {
                    u_log(_log_depth, "increment_fact_index: " + std::to_string(_facts_snapshot.size()) + " candidate facts for relation " + U_NODE(*_relation_index));
                }

                _prof.relation_snapshots.fetch_add(1, std::memory_order_relaxed);
                _prof.snapshot_facts_total.fetch_add(_facts_snapshot.size(), std::memory_order_relaxed);
                if (optimized_snapshot)
                {
                    // Heuristik: subject-driven wenn subject bound branch genutzt wurde, sonst object-driven
                    // (hier nicht perfekt unterscheidbar ohne extra flag; wenn du willst, kann ich das sauber flaggen)
                    _prof.snapshot_subject_driven.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    _prof.snapshot_full_relation.fetch_add(1, std::memory_order_relaxed);
                }
                if (current_rel) _prof.note_relation_scan(current_rel, _facts_snapshot.size());
            }

            _fact_index             = _facts_snapshot.begin(); // used to iterate over all facts that have relation type *_relation_index
            _fact_index_initialized = true;
        }
        else if (++_fact_index == _facts_snapshot.end()) // increment and return false if we reached the end, so _relation_index will be incremented
        {
            return false;
        }
    } while (_n->_pImpl->get_left(*_fact_index).count(*_relation_index) == 1); // skip nodes that represent not relations of type *_relation_index, but relations having *_relation_index as subject (using bidirectional connection to the subject)

    return true;
}

std::shared_ptr<Variables> Unification::Next()
{
    if (_n->logging_active())
        _prof.unification_next_calls.fetch_add(1, std::memory_order_relaxed);

    if (_relation_list.empty()) return nullptr;

    // 1. Check queue for buffered matches (from parallel execution or multiple structures)
    {
        std::lock_guard<std::mutex> l(_queue_mtx);
        if (!_match_queue.empty())
        {
            auto match = std::move(_match_queue.front());
            _match_queue.pop();
            return match;
        }
    }

    if (_use_parallel)
    {
        std::unique_lock<std::mutex> lock(_queue_mtx);
        _queue_cv.wait(lock, [this]
                       { return !_match_queue.empty() || _active_tasks == 0; });
        if (_match_queue.empty()) return nullptr;
        auto match = std::move(_match_queue.front());
        _match_queue.pop();
        return match;
    }
    else
    {
        if (_relation_variable == 0 || string::get(*_variables, _relation_variable, *_relation_index) == *_relation_index)
        {
            while (increment_fact_index()) // iterate over all matching facts
            {
                Node fact = *_fact_index;

                if (_n->logging_active())
                    _prof.facts_scanned_sequential.fetch_add(1, std::memory_order_relaxed);

                // Get all valid structural interpretations of the fact node.
                // This allows matching facts that serve as subjects for other facts (nested structures).
                auto structs = get_fact_structures(_n, fact, /*prefer_single=*/false, _log_depth);

                if (_n->logging_active())
                {
                    _prof.get_fact_structures_calls.fetch_add(1, std::memory_order_relaxed);
                    _prof.structures_total.fetch_add(structs.size(), std::memory_order_relaxed);
                }

                std::shared_ptr<Variables> first = nullptr;

                for (const auto& fs : structs)
                {
                    // Filter: Ensure the interpretation matches the relation currently being scanned
                    if (fs.predicate != *_relation_index) continue;

                    auto result = extract_bindings(fs.subject, fs.objects, *_relation_index, _log_depth);
                    if (!result) continue;

                    if (!first)
                    {
                        first = std::move(result);
                    }
                    else
                    {
                        // Buffer additional valid interpretations
                        std::lock_guard<std::mutex> l(_queue_mtx);
                        _match_queue.push(std::move(result));
                    }
                }

                if (first) return first;
            }
        }

        if (++_relation_index == _relation_list.end()) return nullptr;
        _fact_index_initialized = false;
        return Next();
    }
}

// extract_bindings is passed a fact (statement) and tries to match it to the rule (which is defined in the Unification constructor)
// The rule is contained in the member variables _subject and _object.
// The relation (predicate) of the rule is either a variable _relation_variable, or it matches the given fact’s relation.
// Both a rule and a fact can have only a single subject, but multiple objects.
// In a rule, these objects are interpreted as alternatives.
// In a fact, these objects are interpreted as if stating the fact n times, each with one of the listed objects.
std::shared_ptr<Variables> Unification::extract_bindings(const Node subject, const adjacency_set& objects, const Node relation, const int depth) const
{
    if (_n->logging_active())
        _prof.extract_calls.fetch_add(1, std::memory_order_relaxed);

    if (objects.empty() || subject == 0 || Zelph::Impl::is_var(subject))
    {
        U_LOG(depth, "extract_bindings FAIL: objects=" + std::to_string(objects.empty()) + " subject=" + (subject == 0 ? "null" : (Zelph::Impl::is_var(subject) ? "var" : U_NODE(subject))));
        return nullptr;
    }

    auto result = std::make_shared<Variables>();

    U_LOG(depth, "extract_bindings START RuleSubj=" + U_NODE(_subject) + " FactSubj=" + U_NODE(subject));

    std::vector<std::pair<Node, Node>> history; // Cycle detection
    if (!unify_nodes(_n, _subject, subject, *result, *_variables, history, _log_depth, _prof))
    {
        if (_n->logging_active())
        {
            U_LOG(depth, "  -> Subject Failed");
            _prof.extract_fail_subject.fetch_add(1, std::memory_order_relaxed);
        }
        return nullptr;
    }

    for (auto o : objects)
    {
        if (Zelph::Impl::is_var(o))
        {
            // the given "fact" is not a fact, but a rule, because it contains a variable in at least one of its objects
            return nullptr;
        }
    }

    // Reject rule-template fact nodes. Rule consequences are stored as real
    // graph nodes (e.g. the consequence (D cons T) of rule As2), but their
    // immediate substructure contains VAR nodes. Without this check those
    // templates are matched by unification, producing incorrect bindings and
    // causing extreme performance degradation because every sum/ci/co query
    // iterates over them endlessly.
    if (contains_variable_shallow(_n, subject, depth))
    {
        if (_n->logging_active())
        {
            _prof.template_rejects.fetch_add(1, std::memory_order_relaxed);
            U_LOG(depth, "extract_bindings REJECT: subject " + U_NODE(subject) + " contains variable (rule template)");
        }
        return nullptr;
    }
    for (Node o : objects)
    {
        if (contains_variable_shallow(_n, o, depth))
        {
            if (_n->logging_active())
            {
                _prof.template_rejects.fetch_add(1, std::memory_order_relaxed);
                U_LOG(depth, "extract_bindings REJECT: object " + U_NODE(o) + " contains variable (rule template)");
            }
            return nullptr;
        }
    }

    // Check if the object matches the bound rule variable (if-clause)
    // or one of the objects matches the fixed object from the rule (else-clause).
    bool object_matches   = false;
    Node rule_object_node = *_objects.begin();

    for (auto fact_obj : objects)
    {
        Variables temp_bindings = *result;
        history.clear(); // Reset history for cycle detection
        if (unify_nodes(_n, rule_object_node, fact_obj, temp_bindings, *_variables, history, _log_depth, _prof))
        {
            *result        = temp_bindings;
            object_matches = true;
            break;
        }
    }

    if (object_matches)
    {
        if (_relation_variable != 0 && _variables->count(_relation_variable) == 0 && result->count(_relation_variable) == 0)
            (*result)[_relation_variable] = relation;

        if (_n->logging_active())
        {
            _prof.extract_success.fetch_add(1, std::memory_order_relaxed);
            if (_current_rel_ctx) _prof.note_relation_match(_current_rel_ctx);

            U_LOG(depth, "extract_bindings SUCCESS");

            if (_n->should_log(depth))
            {
                for (const auto& [k, v] : *result)
                    u_log(depth, "  binding: " + U_NODE(k) + " = " + U_NODE(v));
            }
        }

        return result;
    }

    if (_n->logging_active())
    {
        _prof.extract_fail_object.fetch_add(1, std::memory_order_relaxed);
        U_LOG(depth, "extract_bindings FAIL: no object matched");
    }
    return nullptr;
}

std::shared_ptr<Variables> Unification::Unequals()
{
    return _unequals;
}
