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

// Counter updates go through PROF so that a missing profiler is a no-op
// (reasoning_explain.cpp drives Unification standalone and passes nullptr).
// Like U_LOG, the macro expects a variable named _prof in scope -- member or
// parameter. do/while wrapper: unlike U_LOG this macro is used inside plain
// if-blocks, where a bare if would be dangling-else bait.
#define PROF(stmt)       \
    do                   \
    {                    \
        if (_prof)       \
        {                \
            _prof->stmt; \
        }                \
    } while (false)

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
    ReasoningProfiler* const            _prof)
{
    if (_prof && _n->logging_active())
    {
        _prof->unify_calls.fetch_add(1, std::memory_order_relaxed);
        ReasoningProfiler::atomic_max(_prof->max_unify_depth, (uint64_t)depth);
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
                PROF(unify_cycle_hits.fetch_add(1, std::memory_order_relaxed));

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
                PROF(unify_var_seen.fetch_add(1, std::memory_order_relaxed));

            if (local_bindings.count(rule_node))
            {
                if (_n->logging_active())
                {
                    PROF(unify_var_local_recurse.fetch_add(1, std::memory_order_relaxed));
                    U_LOG(depth, "  Var local bound -> recursing");
                }
                result = unify_nodes(_n, local_bindings[rule_node], graph_node, local_bindings, global_bindings, history, depth + 1, _prof);
                break;
            }
            if (global_bindings.count(rule_node))
            {
                if (_n->logging_active())
                    PROF(unify_var_global_recurse.fetch_add(1, std::memory_order_relaxed));

                Node bound = zelph::string::get(global_bindings, rule_node, Node{0});
                if (bound == graph_node)
                {
                    U_LOG(depth, "  -> Var global bound, identical to graph node");
                    result = true;
                    break;
                }
                U_LOG(depth, "  Var global bound to " + U_NODE(bound) + " (id=" + std::to_string(bound) + ") -> recursing with graph_node " + U_NODE(graph_node) + " (id=" + std::to_string(graph_node) + ")");
                result = unify_nodes(_n, bound, graph_node, local_bindings, global_bindings, history, depth + 1, _prof);
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
                PROF(unify_var_bound_new.fetch_add(1, std::memory_order_relaxed));
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
                PROF(unify_identity_hits.fetch_add(1, std::memory_order_relaxed));
                U_LOG(depth, "  -> Identical");
            }
            result = true;
            break;
        }

        // 3. Structural equivalence
        auto rule_structs = get_fact_structures(_n, rule_node, depth);
        if (rule_structs->empty())
        {
            U_LOG(depth, "  -> Rule node is atom, but not identical. Fail.");
            result = false;
            break;
        }

        auto graph_structs = get_fact_structures(_n, graph_node, depth);
        if (graph_structs->empty())
        {
            U_LOG(depth, "  -> Graph node is atom, but rule expects structure. Fail.");
            result = false;
            break;
        }

        if (_n->logging_active())
            PROF(unify_struct_pair_attempts.fetch_add(1, std::memory_order_relaxed));

        for (const auto& rs : *rule_structs)
        {
            for (const auto& gs : *graph_structs)
            {
                Variables attempt = local_bindings;

                // A. Predicate
                if (!unify_nodes(_n, rs.predicate, gs.predicate, attempt, global_bindings, history, depth + 1, _prof)) continue;

                // B. Subject
                if (!unify_nodes(_n, rs.subject, gs.subject, attempt, global_bindings, history, depth + 1, _prof)) continue;

                // C. Objects
                if (rs.objects.empty() != gs.objects.empty()) continue;

                bool      objects_match = true;
                Variables obj_bindings  = attempt;

                if (_n->logging_active())
                    PROF(unify_object_try.fetch_add(1, std::memory_order_relaxed));

                // Greedy Match for Objects
                for (Node r_obj : rs.objects)
                {
                    bool found = false;
                    for (Node g_obj : gs.objects)
                    {
                        Variables try_obj = obj_bindings; // allow backtracking per object choice
                        if (unify_nodes(_n, r_obj, g_obj, try_obj, global_bindings, history, depth + 1, _prof))
                        {
                            obj_bindings = std::move(try_obj);
                            found        = true;

                            if (_n->logging_active())
                            {
                                PROF(unify_object_success.fetch_add(1, std::memory_order_relaxed));
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
        if (n->var_in_closure(pattern)) return GroundResult::Unbound;
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

// --- Partial-pattern anchoring ----------------------------------------------
//
// Bound-pattern grounding (above) handles the fully-bound case: the pattern
// denotes exactly one node. This section handles the PARTIALLY bound case:
// grounding failed (some variable is unbound), but the pattern still
// contains a concrete node in a subject or object position at some
// structural depth -- a bound variable's value, a concrete atom, or a fully
// concrete subterm. Every graph fact matching the condition must contain
// that exact node at the corresponding depth: concrete nodes unify only via
// identity, because hash-consing makes equal fully-concrete structures the
// SAME node. zelph's topology then makes each structural parent reachable
// from its child via get_right (subjects are bidirectional with their fact
// node, objects point to it), so climbing `depth` adjacency levels from the
// anchor yields a complete candidate SUPERSET -- typically a handful of
// nodes instead of the full relation extent. This is exactly what the
// SC-congruence conditions ((U + V) needssimp (U + V)) with bound V used to
// scan: 94M candidate facts in the diffby phase, ~78M of them needssimp.
// The same mechanism, with a 1-level climb, covers conditions like
// (T rw S) whose subject VARIABLE is bound to a fact node -- accepted by
// the sequential anchor path but rejected by the constructor's stricter
// atom-only subject_is_bound check, which sent them into parallel
// full-relation scans (red/rw/simp/= in the profiles).
//
// Predicate positions never qualify as anchors: a fact points TO its
// predicate, so the predicate's incoming side is the full extent
// (snapshot_left_of) -- exactly the scan this avoids.
//
// Soundness is unconditional: candidates still pass extract_bindings'
// structural unification. Completeness is budget-independent: an aborted
// climb falls back to the full scan, never to a truncated candidate set.

namespace
{
    struct AnchorCandidate
    {
        Node              node{0}; // concrete node (after variable substitution)
        std::vector<Node> preds;   // climb filter per level, immediate parent's
                                   // predicate first; 0 = no filter (variable
                                   // predicate). The final climb to the fact
                                   // candidates uses the condition's relation.
    };
}

// Collect all concrete anchor candidates inside `pattern` (a subject or
// object of the condition). `chain` holds the predicate filters from
// pattern's level up to (excluding) the condition's relation.
static void collect_partial_anchors(
    Zelph*                        n,
    Node                          pattern,
    const Node                    parent_rule,
    const Variables&              vars,
    std::vector<Node>&            chain,
    std::unordered_set<Node>&     visited,
    const int                     depth_left,
    const int                     log_depth,
    std::vector<AnchorCandidate>& out)
{
    if (pattern == 0) return;

    if (Zelph::Impl::is_var(pattern))
    {
        const Node bound = zelph::string::get(vars, pattern, Node{0});
        if (bound == 0 || Zelph::Impl::is_var(bound) || bound == parent_rule || !n->exists(bound)) return;
        if (Zelph::Impl::is_hash(bound))
        {
            // Mirror is_concrete_lookup_node: a bound value containing
            // variables at any depth (template leak) is not a data node;
            // anchoring on it would break the identity argument.
            if (n->var_in_closure(bound)) return;
        }
        out.push_back({bound, chain});
        return;
    }

    if (!Zelph::Impl::is_hash(pattern))
    {
        if (pattern != parent_rule)
            out.push_back({pattern, chain}); // concrete atom in the pattern
        return;
    }

    if (!n->var_in_closure(pattern))
    {
        if (pattern != parent_rule)
            out.push_back({pattern, chain}); // fully concrete subterm
        return;
    }

    if (depth_left <= 0) return;
    if (!visited.insert(pattern).second) return; // cyclic pattern: conservative

    // Descend. The predicate chain becomes a climb FILTER, so a wrong
    // structural reading could filter out genuine candidates: descend only
    // when the reading is unambiguous.
    auto structs = get_fact_structures(n, pattern, log_depth);
    if (structs->size() == 1)
    {
        const FactStructure& fs = (*structs)[0];
        const Node           p  = Zelph::Impl::is_var(fs.predicate) ? Node{0} : fs.predicate;

        chain.insert(chain.begin(), p);
        collect_partial_anchors(n, fs.subject, parent_rule, vars, chain, visited, depth_left - 1, log_depth, out);
        for (Node o : fs.objects)
            collect_partial_anchors(n, o, parent_rule, vars, chain, visited, depth_left - 1, log_depth, out);
        chain.erase(chain.begin());
    }

    visited.erase(pattern);
}

// Climb from the anchor to the condition's candidate facts. Returns false
// when a budget is exceeded; the caller then keeps the full-scan behaviour.
static bool climb_partial_anchor(
    Zelph*                 n,
    const AnchorCandidate& anchor,
    const Node             current_rel,
    const size_t           frontier_budget,
    const size_t           work_budget,
    adjacency_set&         out)
{
    // ONE lock scope for the whole climb: formerly one full adjacency copy
    // per frontier node plus two locked edge probes per parent.
    const Network::ReadScope scope = n->read_scope();

    adjacency_set frontier;
    frontier.insert(anchor.node);
    size_t work = 0;

    for (size_t level = 0; level <= anchor.preds.size() && !frontier.empty(); ++level)
    {
        const Node filter = level < anchor.preds.size() ? anchor.preds[level] : current_rel;

        adjacency_set next;
        for (const Node c : frontier)
        {
            const adjacency_set& parents = scope.right(c);
            work += parents.size();
            if (work > work_budget) return false;

            for (const Node f : parents)
            {
                if (filter != 0)
                {
                    if (scope.right(f).count(filter) == 0) continue; // wrong predicate
                    // Subject, not predicate -- unless the two are the same
                    // node, which collapses f's outgoing adjacency to one
                    // entry. Same test as Zelph::collect_anchored_facts.
                    if (scope.left(f).count(filter) != 0 && scope.right(f).size() > 1) continue;
                }
                next.insert(f);
            }
        }
        if (next.size() > frontier_budget) return false;
        frontier = std::move(next);
    }

    out = std::move(frontier);
    return true;
}

zelph::network::PatternInfo zelph::network::build_pattern_info(const Zelph* n, const Node condition, const int log_depth)
{
    PatternInfo pi;
    pi.condition = condition;

    // Decompose the RULE PATTERN via the shared structure list: the former
    // get_preferred_structure calls copied the full FactStructure
    // (including its object adjacency_set) once for the condition and once
    // more for the subject hint -- two heap allocations per call.
    const auto condition_structs = get_fact_structures(n, condition, log_depth);
    if (condition_structs->empty() || condition_structs->front().predicate == 0)
        return pi; // relation stays 0: decomposition failed

    const FactStructure& fs = condition_structs->front();
    pi.relation             = fs.predicate;
    pi.subject              = fs.subject;
    pi.objects              = fs.objects;

    {
        const auto subject_structs = get_fact_structures(n, pi.subject, log_depth);
        pi.subject_pred_hint       = subject_structs->empty() ? Node{0} : subject_structs->front().predicate;
    }

    return pi;
}

Unification::Unification(
    Zelph*                            n,
    Node                              condition,
    Node                              parent,
    const std::shared_ptr<Variables>& variables,
    const std::shared_ptr<Variables>& unequals,
    concurrency::ThreadPool*          pool,
    int                               log_depth,
    ReasoningProfiler*                profiler,
    Node                              seed_fact,
    Node                              seed_predicate)
    : Unification(n, build_pattern_info(n, condition, log_depth), parent, variables, unequals, pool, log_depth, profiler, seed_fact, seed_predicate)
{
}

Unification::Unification(
    Zelph*                            n,
    PatternInfo                       pattern,
    Node                              parent,
    const std::shared_ptr<Variables>& variables,
    const std::shared_ptr<Variables>& unequals,
    concurrency::ThreadPool*          pool,
    int                               log_depth,
    ReasoningProfiler*                profiler,
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
        PROF(unification_instances.fetch_add(1, std::memory_order_relaxed));

    if (pattern.relation != 0)
    {
        Node relation      = pattern.relation;
        _subject           = pattern.subject;
        _objects           = std::move(pattern.objects); // sink: the by-value parameter is ours
        _subject_pred_hint = pattern.subject_pred_hint;

        if (_n->logging_active() && !Zelph::Impl::is_var(relation))
            _current_rel_ctx = relation;

        U_LOG(_log_depth, "Init Unification: " + U_NODE(pattern.condition));

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
        else if (_n->var_in_closure(relation))
        {
            // A COMPOSITE predicate carrying a variable -- `(X (Y r s) Z)` --
            // is a pattern, exactly as a structured subject is, and the two
            // other positions have unified structurally all along. Only the
            // predicate was compared by identity, so the rule matched nothing
            // whatsoever: the graph holds `(b r s)`, never `(Y r s)`.
            //
            // The candidate set is the one a relation VARIABLE gets; what
            // separates the two is that extract_bindings unifies the pattern
            // against each candidate instead of binding one variable to it.
            if (_seed_fact != 0)
            {
                _relation_list.insert(_seed_predicate);
            }
            else
            {
                // Narrowed by the pattern's OWN predicate wherever it has a
                // ground one: a candidate has to unify with `(Y r s)`, and
                // unify_nodes matches predicates before anything else, so no
                // fact whose predicate is not `r` can survive. Without this
                // the condition scans every declared relation type, which is
                // right but is the cost a predicate VARIABLE pays -- and this
                // pattern says far more than a variable does.
                const FactStructure pfs = get_preferred_structure(_n, relation, _log_depth);

                if (pfs.predicate != 0
                    && !Zelph::Impl::is_var(pfs.predicate)
                    && !_n->var_in_closure(pfs.predicate))
                {
                    _relation_list = _n->get_facts_of_predicate(pfs.predicate);
                }
                else
                {
                    _relation_list = _n->get_sources(_n->core.IsA, _n->core.RelationTypeCategory, true);
                }
            }

            _relation_pattern = relation;
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
            _n->log(1, "Unify", "get_preferred_structure failed for rule condition: " + _n->format(pattern.condition));
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
        if (_n->var_in_closure(_subject))
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
        u_log(_n, _log_depth, "Unification: condition=" + _n->format(pattern.condition) + "subject=" + U_NODE(_subject) + " relations: [" + rels_str + "] objects=" + std::to_string(_objects.size()) + " parent=" + _n->format(parent));
    }

    if (_relation_list.empty()) return;

    // Always initialize sequential fallback
    _relation_index         = _relation_list.begin();
    _fact_index_initialized = false;

    // Anchor construction and the parallel scan launch share the boundness
    // analysis below but are gated INDEPENDENTLY: anchoring (use_anchors)
    // is a semantically neutral index shortcut that must engage in
    // single-core / classic evaluation too, while the snapshot launch
    // additionally requires the thread pool (use_parallel). The former
    // coupling to use_parallel() alone left classic mode scanning full
    // relation extents (>20 min for the det workload vs ~1 min anchored).
    // Seed mode bypasses both: its candidate set is the single seeded fact.
    if (_seed_fact == 0 && (_n->use_anchors() || _n->use_parallel()))
    {
        // Subject/Object boundness analysis.
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
            if (!Zelph::Impl::is_var(s) && get_fact_structures(_n, s, log_depth)->empty()) subject_is_bound = true;
        }

        // Do not use parallel if the object is bound (const or bound var).
        // In this case, an object-driven index lookup (in sequential mode) is much faster than scanning the relation.
        bool object_is_bound = false;
        for (Node o : _objects)
        {
            if (!Zelph::Impl::is_var(o))
            {
                if (get_fact_structures(_n, o, log_depth)->empty())
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
                if (!Zelph::Impl::is_var(s) && get_fact_structures(_n, s, log_depth)->empty()) s_bound = true;
            }
            u_log(_n, _log_depth, "DIAG: subject_is_bound=" + std::to_string(s_bound) + " object_is_bound=" + std::to_string(object_is_bound) + " relation_list_size=" + std::to_string(_relation_list.size()) + " subject=" + U_NODE(_subject) + " objects_size=" + std::to_string(_objects.size()));
        }

        // --- Partial-pattern anchoring ---
        // Neither side is fully bound and grounding failed; before resorting
        // to a full-relation scan (sequential or parallel), try to anchor on
        // a concrete inner node of the pattern (see collect_partial_anchors
        // above). Only for a fixed relation: the climb's top filter is the
        // relation itself.
        // Gated on use_anchors() alone, NOT use_parallel(): the climbed
        // candidate set is consumed by the sequential iterator and is
        // exactly as valid in single-core mode.
        // _relation_pattern excluded for the same reason as _relation_variable:
        // both leave _relation_list holding EVERY relation type, and the two
        // paths below take *begin() as though it were the only one.
        if (_n->use_anchors()
            && !subject_is_bound && !object_is_bound && _relation_variable == 0 && _relation_pattern == 0
            && !_relation_list.empty())
        {
            const Node fixed_rel = *_relation_list.begin();

            std::vector<AnchorCandidate> anchors;
            {
                std::vector<Node>        chain;
                std::unordered_set<Node> visited;
                constexpr int            max_anchor_depth = 4;
                collect_partial_anchors(_n, _subject, _parent, *_variables, chain, visited, max_anchor_depth, _log_depth, anchors);
                for (Node o : _objects)
                    collect_partial_anchors(_n, o, _parent, *_variables, chain, visited, max_anchor_depth, _log_depth, anchors);
            }

            if (!anchors.empty())
            {
                // Prefer the lowest-degree anchor: the climb's cost is the
                // frontier's adjacency, so a specific subterm beats a hub
                // (nil, a digit, a popular numeral).
                const AnchorCandidate* best     = nullptr;
                size_t                 best_deg = 0;
                for (const auto& a : anchors)
                {
                    const size_t deg = _n->_pImpl->right_count_of(a.node);
                    if (!best || deg < best_deg)
                    {
                        best     = &a;
                        best_deg = deg;
                    }
                }

                // Budgets relative to the full-scan alternative: a climb
                // visits edges with O(1) hash checks, while a relation scan
                // runs get_fact_structures + unification per fact -- an edge
                // is far cheaper than a scanned fact, hence the 16x factor.
                // Exceeding a budget keeps today's behaviour (hub anchors
                // degrade to the full scan, never to a slow climb);
                // completeness is unaffected either way.
                const size_t extent          = _n->_pImpl->left_count_of(fixed_rel);
                const size_t frontier_budget = std::max<size_t>(128, extent);
                const size_t work_budget     = std::max<size_t>(1024, 16 * extent);

                adjacency_set candidates;
                if (climb_partial_anchor(_n, *best, fixed_rel, frontier_budget, work_budget, candidates))
                {
                    _partial_snapshot       = std::move(candidates);
                    _partial_snapshot_valid = true;

                    if (_n->should_log(1) && _n->should_log(_log_depth - 1))
                    {
                        u_log(_n, _log_depth, "partial-anchor: " + std::to_string(best->preds.size() + 1) + "-level climb from " + U_NODE(best->node) + " -> " + std::to_string(_partial_snapshot.size()) + " candidate(s) for relation " + U_NODE(fixed_rel));
                    }
                }
            }
        }

        if (_pool && _n->use_parallel() && _relation_variable == 0 && _relation_pattern == 0
            && !subject_is_bound && !object_is_bound
            && !_partial_snapshot_valid && !concurrency::tl_is_pool_worker)
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
                    PROF(unification_parallel_instances.fetch_add(1, std::memory_order_relaxed));
                    PROF(relation_snapshots.fetch_add(1, std::memory_order_relaxed));
                    PROF(snapshot_full_relation.fetch_add(1, std::memory_order_relaxed));
                    PROF(snapshot_facts_total.fetch_add(_snapshot_vec.size(), std::memory_order_relaxed));
                    if (_current_rel_ctx) PROF(note_relation_scan(_current_rel_ctx, _snapshot_vec.size()));
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
                                       if (_n->is_rule_pattern(fact)) continue; // not data, see Next()
                                       auto structs = get_fact_structures(_n, fact, _log_depth);
                                       ++local_scanned;

                                       for (const auto& fs : *structs)
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
                                       PROF(facts_scanned_parallel.fetch_add(local_scanned, std::memory_order_relaxed));
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
            bool partial_used       = false;
            Node current_rel        = *_relation_index;
            _snapshot_prefiltered   = false;

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
            else if (_partial_snapshot_valid)
            {
                // Partial-pattern anchor (see the constructor): the complete
                // candidate superset was precomputed by climbing from a
                // concrete inner node. Built only for the single fixed
                // relation, so it is consumed exactly once.
                _facts_snapshot         = std::move(_partial_snapshot);
                _partial_snapshot_valid = false;
                partial_used            = true;
                optimized_snapshot      = true;
                _snapshot_prefiltered   = true; // climb's top-level filter is
                                                // always the fixed relation and
                                                // applies exactly the predicate-
                                                // vs-subject exclusion below
            }
            else if (_n->use_anchors())
            {
                // Subject/object-driven anchoring is gated on use_anchors(),
                // not use_parallel(): these lookups are index shortcuts
                // orthogonal to threading. ".anchors off" restores the
                // full-relation reference scan of the fallback branch below.
                //
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

                    if (nd == _parent || _n->var_in_closure(nd))
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
                    // One lock scope filters the anchor's adjacency straight
                    // into the snapshot (Zelph::collect_anchored_facts).
                    _n->collect_anchored_facts(s, current_rel, _facts_snapshot);
                    optimized_snapshot    = true;
                    _snapshot_prefiltered = true;
                    optimized_snapshot    = true;
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
                        // One lock scope filters the anchor's adjacency straight
                        // into the snapshot (Zelph::collect_anchored_facts).
                        _n->collect_anchored_facts(o, current_rel, _facts_snapshot);
                        optimized_snapshot    = true;
                        _snapshot_prefiltered = true;
                        optimized_snapshot    = true;
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

                PROF(relation_snapshots.fetch_add(1, std::memory_order_relaxed));
                PROF(snapshot_facts_total.fetch_add(_facts_snapshot.size(), std::memory_order_relaxed));
                if (partial_used)
                {
                    PROF(snapshot_partial_anchor.fetch_add(1, std::memory_order_relaxed));
                }
                else if (optimized_snapshot)
                {
                    PROF(snapshot_subject_driven.fetch_add(1, std::memory_order_relaxed));
                }
                else
                {
                    PROF(snapshot_full_relation.fetch_add(1, std::memory_order_relaxed));
                }
                if (current_rel) PROF(note_relation_scan(current_rel, _facts_snapshot.size()));
            }

            _fact_index             = _facts_snapshot.begin(); // used to iterate over all facts that have relation type *_relation_index
            _fact_index_initialized = true;
        }
        else if (++_fact_index == _facts_snapshot.end()) // increment and return false if we reached the end, so _relation_index will be incremented
        {
            return false;
        }
        // Skip nodes that are not relations of type *_relation_index but
        // relations having *_relation_index as their subject (recognised by
        // the bidirectional connection to the subject). The second probe is
        // the subject == predicate exemption: there both roles share one
        // outgoing edge, so the relation IS the predicate. Order matters --
        // has_left_edge answers no for every ordinary candidate, so the
        // outgoing-degree lookup is paid only for the rare fact that really
        // does have the relation as its subject.
    } while (!_snapshot_prefiltered
             && _n->has_left_edge(*_fact_index, *_relation_index)
             && _n->_pImpl->right_count_of(*_fact_index) > 1);

    return true;
}

std::shared_ptr<Variables> Unification::Next()
{
    if (_n->logging_active())
        PROF(unification_next_calls.fetch_add(1, std::memory_order_relaxed));

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

                // A ground rule pattern is not data: it exists because a rule
                // was written, and nobody claimed it. See
                // Zelph::is_rule_pattern -- one hash probe, and a single
                // empty() test wherever no rule has a ground pattern.
                if (_n->is_rule_pattern(fact)) continue;

                if (_n->logging_active())
                    PROF(facts_scanned_sequential.fetch_add(1, std::memory_order_relaxed));

                // Get all valid structural interpretations of the fact node.
                // This allows matching facts that serve as subjects for other facts (nested structures).
                auto structs = get_fact_structures(_n, fact, _log_depth);

                if (_n->logging_active())
                {
                    PROF(get_fact_structures_calls.fetch_add(1, std::memory_order_relaxed));
                    PROF(structures_total.fetch_add(structs->size(), std::memory_order_relaxed));
                }

                std::shared_ptr<Variables> first = nullptr;

                for (const auto& fs : *structs)
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
        PROF(extract_calls.fetch_add(1, std::memory_order_relaxed));

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
        if (_n->logging_active()) PROF(extract_fail_subject.fetch_add(1, std::memory_order_relaxed));
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
            PROF(extract_fail_subject.fetch_add(1, std::memory_order_relaxed));
        }
        return results;
    }

    // --- Predicate unification ---
    // Only for a composite predicate carrying a variable; a fixed one is
    // already the relation this candidate was reached through, and a
    // predicate VARIABLE is bound at the end of the enumeration below.
    if (_relation_pattern != 0)
    {
        if (Zelph::Impl::is_var(relation) || _n->var_in_closure(relation))
            return results; // the candidate's predicate is itself a template

        history.clear();
        if (!unify_nodes(_n, _relation_pattern, relation, base_result, *_variables, history, _log_depth, _prof))
        {
            if (_n->logging_active())
            {
                U_LOG(depth, "  -> Predicate pattern failed");
                PROF(extract_fail_subject.fetch_add(1, std::memory_order_relaxed));
            }
            return results;
        }
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
        if (_n->var_in_closure(subject))
        {
            if (_n->logging_active())
            {
                PROF(template_rejects.fetch_add(1, std::memory_order_relaxed));
                U_LOG(depth, "extract_bindings REJECT: subject " + U_NODE(subject) + " contains variable (rule template)");
            }
            return results;
        }
    }
    for (Node o : objects)
    {
        if (_n->var_in_closure(o))
        {
            if (_n->logging_active())
            {
                PROF(template_rejects.fetch_add(1, std::memory_order_relaxed));
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
                PROF(extract_success.fetch_add(1, std::memory_order_relaxed));
                if (_current_rel_ctx) PROF(note_relation_match(_current_rel_ctx));

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
        PROF(extract_fail_object.fetch_add(1, std::memory_order_relaxed));
        U_LOG(depth, "extract_bindings FAIL: no object permutation matched");
    }

    return results;
}

std::shared_ptr<Variables> Unification::Unequals()
{
    return _unequals;
}
