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
#include "string_utils.hpp"
#include "zelph_impl.hpp"

#include <chrono>
#include <iostream>
#include <unordered_set>
#include <vector>

using namespace zelph::network;

#ifdef _DEBUG
    #include <string>
static void u_log(int depth, const std::string& msg)
{
    if (depth < 10)
    {
        std::string indent(depth * 2, ' ');
        std::clog << indent << "[Unify] " << msg << std::endl;
    }
}
// Helper for Node representation in Debug mode
static std::string u_node_str(Zelph* z, Node n)
{
    if (n == 0) return "0";
    if (Zelph::Impl::is_var(n)) return "VAR(" + std::to_string(n) + ")";
    std::string name = zelph::string::unicode::to_utf8(z->get_name(n, "zelph", true));
    if (name.empty()) name = zelph::string::unicode::to_utf8(z->get_name(n, "en", false));
    if (name.empty()) return std::to_string(n);
    return name + "(" + std::to_string(n) + ")";
}
    #define U_LOG(depth, msg) u_log(depth, msg)
    #define U_NODE(n)         u_node_str(_n, n)
#else
    #define U_LOG(depth, msg) \
        do                    \
        {                     \
        } while (0)
    #define U_NODE(n) ""
#endif

// --- Helper Functions ---

static Node get_value_concept(Zelph* n, Node node)
{
    if (node == 0 || !n->exists(node)) return 0;

    adjacency_set outgoing = n->get_right(node);
    for (Node rel : outgoing)
    {
        if (n->get_right(rel).count(n->core.HasValue) == 1)
        {
            adjacency_set targets = n->get_left(rel);
            for (Node t : targets)
            {
                if (t != node) return t;
            }
        }
    }
    return 0;
}

struct FactStructure
{
    Node                     subject{};
    Node                     predicate{};
    std::unordered_set<Node> objects;
};

// Determines all possible structural interpretations of a node.
static std::vector<FactStructure> get_fact_structures(Zelph* n, Node fact)
{
    std::vector<FactStructure> structures;
    if (fact == 0 || !n->exists(fact)) return structures;

    // Zelph Topology:
    // S <-> F (Subject is bidirectional)
    // F -> P  (Predicate is outgoing)
    // O -> F  (Object is incoming)

    adjacency_set right = n->get_right(fact); // Contains P and S (and Parent-Facts P' where F <-> P')
    adjacency_set left  = n->get_left(fact);  // Contains O and S (and Parent-Facts P')

    adjacency_set predicates;
    for (Node p : right)
    {
        if (n->check_fact(p, n->core.IsA, {n->core.RelationTypeCategory}).is_known())
        {
            predicates.insert(p);
        }
    }

    if (predicates.empty()) return structures;

    for (Node p : predicates)
    {
        for (Node s : right)
        {
            if (s == p) continue;
            if (left.count(s) == 0) continue; // Subject must be bidirectional

            FactStructure fs;
            fs.subject   = s;
            fs.predicate = p;

            // Objects are in 'left', but must NOT be in 'right'.
            // (S is in both, Parent is in both, O is only in left)
            for (Node o : left)
            {
                if (o != s && o != p)
                {
                    if (right.count(o) == 0)
                    {
                        fs.objects.insert(o);
                    }
                }
            }

            if (!fs.objects.empty())
            {
                structures.push_back(fs);
            }
        }
    }

    // Disambiguation: Prefer structures with atomic (Non-Hash) subjects to avoid confusion with parent nodes.
    if (structures.size() > 1)
    {
        std::vector<FactStructure> filtered;
        bool                       has_non_hash = false;

        for (const auto& fs : structures)
        {
            if (!Zelph::Impl::is_hash(fs.subject)) has_non_hash = true;
        }

        if (has_non_hash)
        {
            for (const auto& fs : structures)
            {
                if (!Zelph::Impl::is_hash(fs.subject)) filtered.push_back(fs);
            }
            return filtered;
        }
    }

    return structures;
}

// Recursive Unification Algorithm with Cycle Detection
static bool unify_nodes(Zelph* n, Node rule_node, Node graph_node, Variables& local_bindings, const Variables& global_bindings, std::vector<std::pair<Node, Node>>& history, int depth = 0)
{
    //    if (depth > 50) return false; // Hard limit fallback
    if (rule_node == 0 || graph_node == 0) return false;

#ifdef _DEBUG
    U_LOG(depth, "Comparing " + u_node_str(n, rule_node) + " vs " + u_node_str(n, graph_node));
#endif

    // 0. Cycle Check
    // If we already check this exact pair (rule, graph) in this path, the cycle is closed
    // and we have not found any contradictions so far -> Success.
    for (const auto& pair : history)
    {
        if (pair.first == rule_node && pair.second == graph_node)
        {
            U_LOG(depth, "  -> Cycle detected (already visiting), assuming match.");
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
            if (local_bindings.count(rule_node))
            {
                U_LOG(depth, "  Var local bound -> recursing");
                result = unify_nodes(n, local_bindings[rule_node], graph_node, local_bindings, global_bindings, history, depth);
                break;
            }
            if (global_bindings.count(rule_node))
            {
                Node bound = zelph::string::get(global_bindings, rule_node, 0);
                U_LOG(depth, "  Var global bound -> recursing");
                result = unify_nodes(n, bound, graph_node, local_bindings, global_bindings, history, depth);
                break;
            }

            local_bindings[rule_node] = graph_node;
#ifdef _DEBUG
            U_LOG(depth, "  -> Bound " + u_node_str(n, rule_node) + " to " + u_node_str(n, graph_node));
#endif
            result = true;
            break;
        }

        // 2. Direct identity
        if (rule_node == graph_node)
        {
            U_LOG(depth, "  -> Identical");
            result = true;
            break;
        }

        // 3. Value equivalence
        Node v_rule  = get_value_concept(n, rule_node);
        Node v_graph = get_value_concept(n, graph_node);

        if (v_rule != 0 && v_graph != 0)
        {
            bool match = (v_rule == v_graph);
            U_LOG(depth, match ? "  -> HasValue Match" : "  -> HasValue Mismatch");
            result = match;
            break;
        }
        if (v_rule != 0 || v_graph != 0)
        {
            result = false;
            break;
        }

        // 4. Structural equivalence
        auto rule_structs = get_fact_structures(n, rule_node);
        if (rule_structs.empty())
        {
            U_LOG(depth, "  -> Rule node is atom, but not identical. Fail.");
            result = false;
            break;
        }

        auto graph_structs = get_fact_structures(n, graph_node);
        if (graph_structs.empty())
        {
            U_LOG(depth, "  -> Graph node is atom, but rule expects structure. Fail.");
            result = false;
            break;
        }

        for (const auto& rs : rule_structs)
        {
            for (const auto& gs : graph_structs)
            {
                // A. Predicate
                if (!unify_nodes(n, rs.predicate, gs.predicate, local_bindings, global_bindings, history, depth + 1)) continue;

                // B. Subject
                if (!unify_nodes(n, rs.subject, gs.subject, local_bindings, global_bindings, history, depth + 1)) continue;

                // C. Objekte
                if (rs.objects.empty() != gs.objects.empty()) continue;

                bool      objects_match = true;
                Variables temp_bindings = local_bindings;

                // Greedy Match for Objects
                for (Node r_obj : rs.objects)
                {
                    bool found = false;
                    for (Node g_obj : gs.objects)
                    {
                        if (unify_nodes(n, r_obj, g_obj, temp_bindings, global_bindings, history, depth + 1))
                        {
                            found = true;
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
                    local_bindings = temp_bindings;
                    U_LOG(depth, "    -> Structure Match SUCCESS");
                    result = true;
                    goto end_loop; // Break out of nested loops
                }
            }
        }
    end_loop:;
    } while (false);

    history.pop_back();
    return result;
}

Unification::Unification(Zelph* n, Node condition, Node parent, const std::shared_ptr<Variables>& variables, const std::shared_ptr<Variables>& unequals, ThreadPool* pool)
    : _n(n), _parent(parent), _variables(variables), _unequals(unequals), _pool(pool)
{
    // "condition" means here one of the predicates that form a list of conditions (which are connected by "and")

    adjacency_set relations = _n->filter(condition, _n->core.IsA, _n->core.RelationTypeCategory);

    if (relations.size() == 1) // more than one relation for given condition makes no sense. _relation_list is empty, so Next() won't return anything
    {
        Node relation = *relations.begin(); // there is always only 1 relation
        _subject      = _n->parse_fact(condition, _objects, _parent);

        U_LOG(0, "Init Unification: " + U_NODE(condition));

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

    if (_relation_list.empty()) return;

    // Always initialize sequential fallback
    _relation_index         = _relation_list.begin();
    _fact_index_initialized = false;

// Optimization for Release mode only: Subject/Object Driven Indexing
#ifndef _DEBUG
    // parallel only with fixed relation
    // OPTIMIZATION: Do NOT use parallel processing/snapshotting if subject is bound, as the result set is likely tiny.
    bool subject_is_bound = false;
    if (_subject != 0)
    {
        Node s = _subject;
        if (Zelph::Impl::is_var(s)) s = string::get(*_variables, s, s);
        // Only optimize if s is an ATOM (not a structure), because complex structures
        // cannot be looked up simply via get_right(s) in a deep unification context.
        if (!Zelph::Impl::is_var(s) && get_fact_structures(_n, s).empty()) subject_is_bound = true;
    }

    // Do not use parallel if the object is bound (const or bound var).
    // In this case, an object-driven index lookup (in sequential mode) is much faster than scanning the relation.
    bool object_is_bound = false;
    for (Node o : _objects)
    {
        if (!Zelph::Impl::is_var(o))
        {
            if (get_fact_structures(_n, o).empty())
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

    if (_pool && _relation_variable == 0 && !subject_is_bound && !object_is_bound)
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
            _use_parallel = true;
            _snapshot_vec.assign(snapshot.begin(), snapshot.end());

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
                                   for (size_t i = start; i < end; ++i)
                                   {
                                       Node fact = _snapshot_vec[i];
                                       adjacency_set objects;
                                       Node subject = _n->parse_fact(fact, objects, fixed_rel);
                                       auto result = extract_bindings(subject, objects, fixed_rel);
                                       if (result)
                                       {
                                           std::lock_guard<std::mutex> l(_queue_mtx);
                                           _match_queue.push(std::move(result));
                                           _queue_cv.notify_one();
                                       }
                                   }
                                   {
                                       std::lock_guard<std::mutex> l(_queue_mtx);
                                       --_active_tasks;
                                       _queue_cv.notify_all();
                                   } });
            }
        }
    }
#endif
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

#ifndef _DEBUG
            // Check if Subject is bound
            Node s = _subject;
            if (Zelph::Impl::is_var(s)) s = string::get(*_variables, s, s);

            // Only optimize if subject is atomic (see constructor comments)
            if (s != 0 && !Zelph::Impl::is_var(s) && get_fact_structures(_n, s).empty())
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
            }
            // Check if Object is bound (if Subject wasn't)
            else if (!_objects.empty())
            {
                Node o = *_objects.begin(); // TODO: We assume a single object variable in a rule (see corresponding TODO in method extract_bindings)
                if (Zelph::Impl::is_var(o)) o = string::get(*_variables, o, o);

                if (o != 0 && !Zelph::Impl::is_var(o) && get_fact_structures(_n, o).empty())
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
                }
            }
#endif

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
    if (_relation_list.empty()) return nullptr;

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
            while (increment_fact_index()) // iterate over all matching facts (in snapshot)
            {
                adjacency_set objects;
                Node          subject = _n->parse_fact(*_fact_index, objects, *_relation_index);

                auto result = extract_bindings(subject, objects, *_relation_index);
                if (result)
                {
                    return result;
                }
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
std::shared_ptr<Variables> Unification::extract_bindings(const Node subject, const adjacency_set& objects, const Node relation) const
{
    if (objects.empty() || subject == 0 || Zelph::Impl::is_var(subject)) return nullptr;

    auto result = std::make_shared<Variables>();

    U_LOG(0, "extract_bindings START RuleSubj=" + U_NODE(_subject) + " FactSubj=" + U_NODE(subject));

    std::vector<std::pair<Node, Node>> history; // Cycle detection
    if (!unify_nodes(_n, _subject, subject, *result, *_variables, history))
    {
        U_LOG(0, "  -> Subject Failed");
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

    // Check if the object matches the bound rule variable (if-clause)
    // or one of the objects matches the fixed object from the rule (else-clause).
    bool object_matches   = false;
    Node rule_object_node = *_objects.begin();

    for (auto fact_obj : objects)
    {
        Variables temp_bindings = *result;
        history.clear(); // Reset history for cycle detection
        if (unify_nodes(_n, rule_object_node, fact_obj, temp_bindings, *_variables, history))
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
        return result;
    }
    return nullptr;
}

std::shared_ptr<Variables> Unification::Unequals()
{
    return _unequals;
}
