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
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

#include <vector>

using namespace zelph::network;

#include <string>
static void u_log(const Zelph* zelph, int depth, const std::string& msg)
{
    std::string indent(depth * 2, ' ');
    zelph->diagnostic_stream() << indent << "[depth " << depth << ", Unify] " << msg << std::endl;
}

static std::string u_node_str(const Zelph* z, Node n)
{
    if (n == 0) return "0";
    if (Zelph::Impl::is_var(n)) return "VAR(" + z->format(n) + ")";
    std::string name = z->get_name(n, "zelph", true);
    if (name.empty()) name = z->format(n);
    if (name.empty())
        return std::to_string(n);
    else
        return name;
}
#define U_LOG(depth, msg) \
    if (_n->should_log(depth)) { u_log(_n, depth, msg); }
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
                    u_log(_n, depth, "  DIAGNOSTIC DUMP: rule_node=" + std::to_string(rule_node) + " global_bindings has " + std::to_string(global_bindings.size()) + " entries:");
                    for (const auto& [k, v] : global_bindings)
                        u_log(_n, depth, "    key=" + std::to_string(k) + " (" + U_NODE(k) + ") -> val=" + std::to_string(v) + " (" + U_NODE(v) + ")");
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

// Returns true if nd is a VAR, or if its structural closure (subject,
// predicate, objects at any depth) contains a VAR node.
//
// Rationale: the subject/object-driven snapshot anchors on a node and scans
// only its adjacency. That is correct for concrete nodes (atoms or fully
// concrete structures), because data facts referencing them are directly
// adjacent. A pattern node with variables at ANY depth, however, must match
// OTHER graph nodes via structural unification -- its own adjacency contains
// only rule topology, so an anchor lookup would silently miss all data.
// The shallow check is not sufficient here: e.g. the rule subject
// ((A cons R) add (B cons S)) has no variables in its immediate structure;
// the variables sit one level deeper inside the cons cells. This is exactly
// the case the former BFS-based template check was introduced for
// ("extend parallel reasoning to cover cons cells").
static bool contains_variable_deep(Zelph* n, Node nd, const int depth, std::unordered_set<Node>& visited)
{
    if (nd == 0) return false;
    if (Zelph::Impl::is_var(nd)) return true;
    if (!Zelph::Impl::is_hash(nd)) return false;  // plain atom -> no internal structure
    if (!visited.insert(nd).second) return false; // cycle protection

    auto structs = get_fact_structures(n, nd, false, depth);
    for (const auto& fs : structs)
    {
        if (contains_variable_deep(n, fs.subject, depth, visited)) return true;
        if (contains_variable_deep(n, fs.predicate, depth, visited)) return true;
        for (Node o : fs.objects)
            if (contains_variable_deep(n, o, depth, visited)) return true;
    }
    return false;
}

// --- Bound-pattern grounding -------------------------------------------------
//
// A structured rule pattern whose variables are all bound by earlier
// conditions denotes exactly one concrete node: the fact node obtained by
// substituting the bindings bottom-up. ground_pattern resolves that node via
// pure hash lookups (check_fact) WITHOUT creating anything.
//
// Result semantics:
//   Grounded  out = the existing concrete node
//   Unbound   at least one variable is unbound (or the structure is cyclic /
//             undecomposable) -> caller falls back to scanning
//   Missing   all variables are bound, but the denoted fact does not exist
//             -> the condition cannot match at all (fail fast)
//
// NOTE on multi-object facts: lookup uses check_fact and therefore EXACT
// object-set semantics -- the same interpretation that instantiate_fact()
// and the termination guard (consequences_already_exist) apply to fully
// bound patterns. extract_bindings' greedy subset matching of objects is
// deliberately NOT replicated here: a graph fact carrying additional objects
// beyond the pattern's is a different node and is not found. If that corner
// case ever becomes relevant, returning Unbound instead of Missing below
// restores the scan-based (subset-matching) behaviour.

enum class GroundResult
{
    Grounded,
    Unbound,
    Missing
};

static GroundResult ground_pattern(Zelph* n, Node pattern, const Variables& vars, const int depth, Node& out, std::vector<Node>& history)
{
    if (pattern == 0) return GroundResult::Unbound;

    if (Zelph::Impl::is_var(pattern))
    {
        const Node bound = zelph::string::get(vars, pattern, Node{0});
        if (bound == 0 || Zelph::Impl::is_var(bound)) return GroundResult::Unbound;
        out = bound;
        return GroundResult::Grounded;
    }

    if (!Zelph::Impl::is_hash(pattern))
    {
        out = pattern; // plain atom
        return GroundResult::Grounded;
    }

    for (Node visited : history)
        if (visited == pattern) return GroundResult::Unbound; // cyclic structure: be conservative
    history.push_back(pattern);

    FactStructure fs = get_preferred_structure(n, pattern, depth);
    if (fs.predicate == 0)
    {
        history.pop_back();
        // Hash node without decomposable structure (e.g. a set node):
        // concrete iff it contains no variables.
        std::unordered_set<Node> visited;
        if (contains_variable_deep(n, pattern, depth, visited)) return GroundResult::Unbound;
        out = pattern;
        return GroundResult::Grounded;
    }

    Node         gs = 0;
    GroundResult r  = ground_pattern(n, fs.subject, vars, depth, gs, history);
    if (r != GroundResult::Grounded)
    {
        history.pop_back();
        return r;
    }

    Node gp = 0;
    r       = ground_pattern(n, fs.predicate, vars, depth, gp, history);
    if (r != GroundResult::Grounded)
    {
        history.pop_back();
        return r;
    }

    adjacency_set gobjs;
    bool          changed = (gs != fs.subject) || (gp != fs.predicate);
    for (Node o : fs.objects)
    {
        Node go = 0;
        r       = ground_pattern(n, o, vars, depth, go, history);
        if (r != GroundResult::Grounded)
        {
            history.pop_back();
            return r;
        }
        gobjs.insert(go);
        if (go != o) changed = true;
    }
    history.pop_back();

    if (!changed)
    {
        out = pattern; // pattern was fully concrete to begin with
        return GroundResult::Grounded;
    }

    const Answer ans = n->check_fact(gs, gp, gobjs);
    if (!ans.is_known()) return GroundResult::Missing;

    out = ans.relation();
    return GroundResult::Grounded;
}

Unification::Unification(
    Zelph*                            n,
    Node                              condition,
    Node                              parent,
    const std::shared_ptr<Variables>& variables,
    const std::shared_ptr<Variables>& unequals,
    concurrency::ThreadPool*          pool,
    int                               log_depth,
    ReasoningProfiler&                profiler,
    Node                              seed_fact,
    Node                              seed_predicate)
    : _n(n)
    , _parent(parent)
    , _variables(variables)
    , _unequals(unequals)
    , _seed_fact(seed_fact)
    , _seed_predicate(seed_predicate)
    , _log_depth(log_depth)
    , _prof(profiler)
    , _pool(pool)
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

        _subject_pred_hint = get_preferred_structure(_n, _subject, _log_depth).predicate;

        if (_n->logging_active() && !Zelph::Impl::is_var(relation))
            _current_rel_ctx = relation;

        U_LOG(_log_depth, "Init Unification: " + U_NODE(condition));

        if (Zelph::Impl::is_var(relation))
        {
            // the relation is a variable, so fill _relation_list with all possible relations (excluding variables)
            if (_seed_fact != 0)
            {
                // Seed mode: the candidate's relation type is known upfront.
                // extract_bindings binds the relation variable to it exactly
                // like the full scan would; the existing bound-variable check
                // below then works unchanged against this one-element list.
                _relation_list.insert(_seed_predicate);
            }
            else
            {
                _relation_list = _n->get_sources(_n->core.IsA, _n->core.RelationTypeCategory, true);
            }
            _relation_variable = relation;
        }
        else
        {
            _relation_list.insert(relation); // leaving _relation_variable==0, ...

            if (_seed_fact != 0 && relation != _seed_predicate)
            {
                // Seed mode with a fixed, different relation: the seeded fact
                // can never satisfy this condition. The semi-naive index
                // already filters by predicate; this is defense in depth.
                _relation_list.clear();
            }

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

    // --- Bound-pattern grounding ---
    // A structured subject pattern whose variables are all bound denotes
    // exactly one concrete fact node. Resolving it via hash lookup turns a
    // full-relation (or hub-anchored) scan into a direct subject-driven
    // anchor; if the denoted node does not exist, the condition can never
    // match and unification terminates immediately.
    if (!_relation_list.empty() && _subject != 0
        && Zelph::Impl::is_hash(_subject) && !Zelph::Impl::is_var(_subject))
    {
        std::unordered_set<Node> visited;
        if (contains_variable_deep(_n, _subject, _log_depth, visited))
        {
            Node              grounded = 0;
            std::vector<Node> ground_history;
            switch (ground_pattern(_n, _subject, *_variables, _log_depth, grounded, ground_history))
            {
            case GroundResult::Grounded:
                _subject_grounded = grounded;
                U_LOG(_log_depth, "subject pattern grounded to " + U_NODE(grounded));
                break;
            case GroundResult::Missing:
                U_LOG(_log_depth, "subject pattern grounding: denoted fact missing -> condition cannot match");
                _relation_list.clear();
                return;
            case GroundResult::Unbound:
                break; // unbound variables remain -> scan as before
            }
        }
    }

    if (_n->should_log(1) && _n->should_log(_log_depth - (_relation_list.empty() ? 0 : 1)))
    {
        std::string rels_str;
        for (Node r : _relation_list)
            rels_str += " " + U_NODE(r);
        u_log(_n, _log_depth, "Unification: condition=" + _n->format(condition) + "subject=" + U_NODE(_subject) + " relations: [" + rels_str + "] objects=" + std::to_string(_objects.size()) + " parent=" + _n->format(parent));
    }

    if (_relation_list.empty()) return;

    // Always initialize sequential fallback
    _relation_index         = _relation_list.begin();
    _fact_index_initialized = false;

    if (_seed_fact == 0 && _n->use_parallel())
    {
        // Subject/Object Driven Indexing
        //  parallel only with fixed relation
        //  OPTIMIZATION: Do NOT use parallel processing/snapshotting if subject is bound, as the result set is likely tiny.
        bool subject_is_bound = false;
        if (_subject_grounded != 0)
        {
            // A grounded subject pattern is as good as a bound atom: the
            // grounded node is a direct get_right() anchor.
            subject_is_bound = true;
        }
        else if (_subject != 0)
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

        if (_n->should_log(1))
        {
            bool s_bound = false;
            if (_subject != 0)
            {
                Node s = _subject;
                if (Zelph::Impl::is_var(s)) s = string::get(*_variables, s, s);
                if (!Zelph::Impl::is_var(s) && get_fact_structures(_n, s, false, log_depth).empty()) s_bound = true;
            }
            u_log(_n, _log_depth, "DIAG: subject_is_bound=" + std::to_string(s_bound) + " object_is_bound=" + std::to_string(object_is_bound) + " relation_list_size=" + std::to_string(_relation_list.size()) + " subject=" + U_NODE(_subject) + " objects_size=" + std::to_string(_objects.size()));
        }

        if (_pool && _relation_variable == 0 && !subject_is_bound && !object_is_bound && !concurrency::tl_is_pool_worker)
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
                _n->diagnostic_stream() << "[Timer] Unification snapshot " << fixed_rel
                                        << " size=" << snapshot.size()
                                        << " took=" << snap_ms << "ms" << std::endl;
            }

            if (snapshot.size() > 0)
            {
                _use_parallel = true;
                _snapshot_vec.assign(snapshot.begin(), snapshot.end());

                if (_n->logging_active())
                {
                    // NOTE: must run AFTER _snapshot_vec is assigned; the
                    // previous ordering attributed size 0 to parallel scans,
                    // hiding them from top_relations_by_scan entirely.
                    _prof.unification_parallel_instances.fetch_add(1, std::memory_order_relaxed);
                    _prof.relation_snapshots.fetch_add(1, std::memory_order_relaxed);
                    _prof.snapshot_full_relation.fetch_add(1, std::memory_order_relaxed);
                    _prof.snapshot_facts_total.fetch_add(_snapshot_vec.size(), std::memory_order_relaxed);
                    if (_current_rel_ctx) _prof.note_relation_scan(_current_rel_ctx, _snapshot_vec.size());
                }

                if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                {
                    u_log(_n, _log_depth, "parallel snapshot: " + std::to_string(_snapshot_vec.size()) + " candidate facts for relation " + U_NODE(fixed_rel));
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

                                           for (auto& r : extract_bindings(fs.subject, fs.objects, fixed_rel, _log_depth))
                                           {
                                               std::lock_guard<std::mutex> l(_queue_mtx);
                                               _match_queue.push(std::move(r));
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

            if (_seed_fact != 0)
            {
                // Semi-naive seed mode: the candidate set is exactly the
                // seeded fact -- no snapshot, no anchor lookup. The generic
                // bookkeeping below stays active, so note_relation_scan
                // honestly records a scan of size 1 for this relation and
                // scans/fact metrics remain comparable across modes.
                _facts_snapshot.clear();
                _facts_snapshot.insert(_seed_fact);
                optimized_snapshot = true;
            }
            else if (_n->use_parallel())
            {
                // Rule-template nodes exist in the graph. The subject/object-
                // driven shortcut must not anchor on nodes that are themselves
                // rule topology: the conjunction set node, or pattern fact
                // nodes containing variables.
                //
                // Concrete atoms that merely OCCUR inside the template (e.g.
                // the constant Q6256 in a condition (A P31 Q6256)) are valid
                // anchors: their adjacency is exactly the data we want to
                // scan. The previous BFS-based check rejected them, forcing a
                // full-relation snapshot -- catastrophic for high-cardinality
                // relations like P31 (~15M facts).
                //
                // Template fact nodes that still end up in the candidate
                // snapshot are rejected later by extract_bindings via
                // contains_variable_shallow, exactly as in the full-scan path.
                auto is_concrete_lookup_node = [&](Node nd) -> bool
                {
                    if (nd == 0) return false;

                    if (Zelph::Impl::is_var(nd))
                        nd = string::get(*_variables, nd, nd);

                    if (nd == 0 || Zelph::Impl::is_var(nd) || !_n->exists(nd))
                        return false;

                    std::unordered_set<Node> visited;
                    if (nd == _parent || contains_variable_deep(_n, nd, _log_depth, visited))
                    {
                        if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                            u_log(_n, _log_depth, "is_concrete_lookup_node: REJECT (template) " + U_NODE(nd));
                        return false;
                    }

                    return true;
                };

                // Check if Subject is bound
                Node s = _subject;
                if (Zelph::Impl::is_var(s)) s = string::get(*_variables, s, s);
                if (_subject_grounded != 0) s = _subject_grounded; // anchor on the grounded pattern node
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
                        if (_n->has_left_edge(fact, current_rel)) continue;

                        // Zelph::fact -> connect(Fact, RelationType). Fact points to RelationType.
                        // So get_right(Fact) contains RelationType.
                        if (_n->has_right_edge(fact, current_rel))
                        {
                            _facts_snapshot.insert(fact);
                        }
                    }
                    optimized_snapshot = true;
                    if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                    {
                        u_log(_n, _log_depth, std::string("optimized_snapshot=") + (optimized_snapshot ? "YES" : "NO") + " rel=" + U_NODE(current_rel) + " subj=" + U_NODE(s) + (optimized_snapshot ? " size=" + std::to_string(_facts_snapshot.size()) : ""));
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
                            if (_n->has_left_edge(fact, current_rel)) continue;

                            if (_n->has_right_edge(fact, current_rel))
                            {
                                _facts_snapshot.insert(fact);
                            }
                        }
                        optimized_snapshot = true;
                        if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                        {
                            u_log(_n, _log_depth, std::string("optimized_snapshot=") + (optimized_snapshot ? "YES" : "NO") + " rel=" + U_NODE(current_rel) + " obj=" + U_NODE(o) + (optimized_snapshot ? " size=" + std::to_string(_facts_snapshot.size()) : ""));
                        }
                    }
                }
            }

            if (optimized_snapshot)
            {
                if (_n->should_log(1))
                    u_log(_n, _log_depth, "DIAG increment_fact_index: optimized_snapshot=YES, _facts_snapshot.size()=" + std::to_string(_facts_snapshot.size()));
            }
            else
            {
                if (_n->should_log(1))
                    u_log(_n, _log_depth, "DIAG increment_fact_index: optimized_snapshot=NO, _facts_snapshot.size()=" + std::to_string(_facts_snapshot.size()));

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
                    u_log(_n, _log_depth, "increment_fact_index: " + std::to_string(_facts_snapshot.size()) + " candidate facts for relation " + U_NODE(*_relation_index));
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
    } while (_n->has_left_edge(*_fact_index, *_relation_index)); // skip nodes that represent not relations of type *_relation_index, but relations having *_relation_index as subject (using bidirectional connection to the subject)

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

                    for (auto& r : extract_bindings(fs.subject, fs.objects, *_relation_index, _log_depth))
                    {
                        if (!first)
                            first = std::move(r);
                        else
                        {
                            std::lock_guard<std::mutex> l(_queue_mtx);
                            _match_queue.push(std::move(r));
                        }
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
// The function returns all valid injective matchings instead of
// stopping at the first one. This is necessary because subsequent
// conditions may only be satisfiable for one particular permutation;
// the engine has no cross-fact backtracking, so every permutation
// must be offered as a distinct candidate upfront.
std::vector<std::shared_ptr<Variables>> Unification::extract_bindings(
    const Node subject, const adjacency_set& objects, const Node relation, const int depth) const
{
    std::vector<std::shared_ptr<Variables>> results;

    if (_n->logging_active())
        _prof.extract_calls.fetch_add(1, std::memory_order_relaxed);

    if (Zelph::Impl::is_var(subject))
    {
        if (objects.empty() || subject == 0)
        {
            U_LOG(depth, "extract_bindings FAIL: objects=" + std::to_string(objects.empty()) + " subject=" + (subject == 0 ? "null" : (Zelph::Impl::is_var(subject) ? "var" : U_NODE(subject))));
            return results;
        }
    }
    else if (_subject_pred_hint && !_n->has_right_edge(subject, _subject_pred_hint))
    {
        if (_n->logging_active()) _prof.extract_fail_subject.fetch_add(1, std::memory_order_relaxed);
        return results;
    }

    U_LOG(depth, "extract_bindings START RuleSubj=" + U_NODE(_subject) + " FactSubj=" + U_NODE(subject));

    // --- Subject unification ---
    Variables                          base_result;
    std::vector<std::pair<Node, Node>> history;
    if (!unify_nodes(_n, _subject, subject, base_result, *_variables, history, _log_depth, _prof))
    {
        if (_n->logging_active())
        {
            U_LOG(depth, "  -> Subject Failed");
            _prof.extract_fail_subject.fetch_add(1, std::memory_order_relaxed);
        }
        return results;
    }

    // --- Reject rule-template fact nodes ---
    for (auto o : objects)
    {
        if (Zelph::Impl::is_var(o))
            return results; // fact contains a variable => it is a rule template, not data
    }

    // Reject rule-template fact nodes -- at ANY structural depth.
    //
    // Rule consequences are stored as real graph nodes (e.g. the consequence
    // of rule As2), and matching them as data produces variable-to-variable
    // bindings that instantiate_fact() would then materialize as partially
    // instantiated junk nodes, cascading into wrong deductions.
    //
    // The check must be DEEP, not shallow: a template like
    //   (((A dmul nil) mci 0) pprod nil)
    // exposes no variable at depth 1 -- its subject decomposes to a hash
    // node and the constant 0 -- while the variable A sits at depth 2.
    // This exact shape slipped through the former shallow check and caused
    // the multiplication junk-fact regression.
    {
        std::unordered_set<Node> visited;
        if (contains_variable_deep(_n, subject, depth, visited))
        {
            if (_n->logging_active())
            {
                _prof.template_rejects.fetch_add(1, std::memory_order_relaxed);
                U_LOG(depth, "extract_bindings REJECT: subject " + U_NODE(subject) + " contains variable (rule template)");
            }
            return results;
        }
    }
    for (Node o : objects)
    {
        std::unordered_set<Node> visited;
        if (contains_variable_deep(_n, o, depth, visited))
        {
            if (_n->logging_active())
            {
                _prof.template_rejects.fetch_add(1, std::memory_order_relaxed);
                U_LOG(depth, "extract_bindings REJECT: object " + U_NODE(o) + " contains variable (rule template)");
            }
            return results;
        }
    }

    // --- Enumerate all valid injective matchings of rule objects to fact objects ---
    //
    // "Injective" means each fact object is used at most once per result, so that
    // two distinct rule-object variables (e.g. A and B in "F maps A B") always bind
    // to two DIFFERENT fact objects.
    //
    // ALL valid permutations are returned, not just the first one.  The caller
    // (Next()) queues all of them so that the evaluation engine can explore every
    // branch.  Without this, a permutation that fails at a later condition would
    // cause the engine to miss the correct solution entirely.
    //
    // For single-object rules this degenerates to the original behaviour: the loop
    // runs once and produces at most one result.

    const std::vector<Node> rule_obj_vec(_objects.begin(), _objects.end());
    const std::vector<Node> fact_obj_vec(objects.begin(), objects.end());
    std::vector<bool>       used(fact_obj_vec.size(), false);

    std::function<void(size_t, Variables)> enumerate = [&](size_t idx, Variables bindings)
    {
        if (idx == rule_obj_vec.size())
        {
            auto result = std::make_shared<Variables>(std::move(bindings));

            if (_relation_variable != 0
                && _variables->count(_relation_variable) == 0
                && result->count(_relation_variable) == 0)
            {
                (*result)[_relation_variable] = relation;
            }

            if (_n->logging_active())
            {
                _prof.extract_success.fetch_add(1, std::memory_order_relaxed);
                if (_current_rel_ctx) _prof.note_relation_match(_current_rel_ctx);

                U_LOG(depth, "extract_bindings SUCCESS (permutation " + std::to_string(results.size()) + ")");
                if (_n->should_log(depth))
                {
                    for (const auto& [k, v] : *result)
                        u_log(_n, depth, "  binding: " + U_NODE(k) + " = " + U_NODE(v));
                }
            }

            results.push_back(std::move(result));
            return;
        }

        for (size_t fi = 0; fi < fact_obj_vec.size(); ++fi)
        {
            if (used[fi]) continue;

            Variables try_b = bindings;
            history.clear();
            if (unify_nodes(_n, rule_obj_vec[idx], fact_obj_vec[fi], try_b, *_variables, history, _log_depth, _prof))
            {
                used[fi] = true;
                enumerate(idx + 1, std::move(try_b));
                used[fi] = false;
            }
        }
    };

    enumerate(0, base_result);

    if (results.empty() && _n->logging_active())
    {
        _prof.extract_fail_object.fetch_add(1, std::memory_order_relaxed);
        U_LOG(depth, "extract_bindings FAIL: no object permutation matched");
    }

    return results;
}

std::shared_ptr<Variables> Unification::Unequals()
{
    return _unequals;
}
