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

#pragma once

#include "fact_structure_types.hpp"
#include "zelph.hpp"
#include <vector>

namespace zelph::network
{
    // Determines all possible structural interpretations of a fact node.
    //
    // A fact node F encodes the triple (Subject, Predicate, Objects) via:
    //   Subject <-> F  (bidirectional)
    //   F -> Predicate  (outgoing)
    //   Object -> F     (incoming only)
    //
    // This function reconstructs those triples, filtering out "child facts":
    // nodes that are bidirectionally connected to F because F is THEIR subject
    // (not the other way around).
    //
    // Returns the full disambiguated list as an immutable shared pointer;
    // callers that only need the single best structure use
    // get_preferred_structure (equivalent to the former prefer_single=true).
    inline FactStructurePtr get_fact_structures(
        const Zelph* n,
        Node         fact,
        int          depth)
    {
        // One shared instance backs ALL empty results (atoms, nonexistent
        // nodes, predicate-free hash nodes): negative entries are by far
        // the most frequent lookups on the unify recursion path, and this
        // way they cost neither an allocation nor a per-entry list.
        static const FactStructurePtr shared_empty = std::make_shared<FactStructureList>();

        if (n->should_log(depth))
        {
            n->log(depth, "get_fact_structures", "Starting for fact: " + n->format(fact));
        }

        if (fact == 0) return shared_empty;

        // ---- Structureless node classes: answer without any lock ----
        // Atoms (sequential IDs) and variables can never decompose: they
        // were never created by fact(), so there is no triple to find --
        // the node ID of a fact IS the hash of its triple, and an atom's
        // ID is a counter. These probes were the atom share of ~17M
        // fs_cache hits per Jacobian phase, each paying a rwlock pair plus
        // a map find; the classification is two bit tests. Holds
        // independently of the authoritative bits, i.e. also after binary
        // loads and trusted imports.
        if (!Zelph::is_hash(fact) || Zelph::is_var(fact)) return shared_empty;

        // ---- Cache lookup FIRST (ignores depth; depth is only for logging) ----
        // A hit answers without touching the adjacency locks: the former
        // exists() probe before the lookup cost one _smtx_left rwlock pair
        // per hit -- ~60M pairs per Jacobian phase. Safe without it: every
        // removal/merge/load path clears the whole cache, so a present
        // entry implies a live node.
        FactStructurePtr cached;
        if (n->try_get_fact_structures_cached(fact, cached))
        {
            if (n->should_log(depth))
            {
                n->log(depth, "get_fact_structures", "Cache HIT for fact: " + n->format(fact) + " (structures=" + std::to_string(cached->size()) + ")");
            }
            return cached;
        }

        // ---- Genuine-structure fast path ----
        // Every triple-created node answers with its exact stored triple:
        // no adjacency locks, no O(deg^2) walk over hub neighborhoods.
        // The entry is promoted into the fs_cache so subsequent probes pay
        // a single lock pair; per-node invalidation may erase that copy at
        // any time -- harmless, the next probe re-promotes the same
        // immutable list.
        FactStructurePtr genuine;
        if (n->try_get_genuine_structure(fact, genuine))
        {
            n->store_fact_structures_cached(fact, genuine);
            if (n->should_log(depth))
            {
                n->log(depth, "get_fact_structures", "Genuine-store HIT for fact: " + n->format(fact));
            }
            return genuine;
        }

        n->count_genuine_walk(); // everything below is the historical reconstruction

        // Predicate-detection memo for the whole reconstruction (see
        // Zelph::relation_type_set). MUST be fetched before the ReadScope
        // below opens: the memo's lazy build takes the very locks the
        // scope will hold.
        const auto rel_types = n->relation_type_set();

        FactStructureList structures;
        bool              no_predicates = false;

        {
            // ---- Locked-scope reconstruction ----
            // ONE shared lock pair covers the entire miss path. Previously
            // every neighborhood probe -- dozens per reconstructed node,
            // up to three hops deep in the child-fact heuristic -- paid
            // its own rwlock pair plus a full adjacency_set copy. All
            // reads below go through scope REFERENCES (stable while the
            // scope is alive). Per the ReadScope contract, nothing in this
            // block may write, take another network lock, or log (the
            // output handler is user code, and format/log lock).
            const Network::ReadScope scope = n->read_scope();

            if (!scope.exists(fact)) return shared_empty;

            // Zelph Topology:
            // S <-> F (Subject is bidirectional)
            // F -> P  (Predicate is outgoing)
            // O -> F  (Object is incoming)
            const adjacency_set& right = scope.right(fact); // Contains P and S (and Parent-Facts P' where F <-> P')
            const adjacency_set& left  = scope.left(fact);  // Contains O and S (and Parent-Facts P')

            adjacency_set predicates;
            for (Node p : right)
            {
                if (rel_types->count(p) != 0)
                {
                    predicates.insert(p);
                }
            }

            // See the object loop below: only a node that serves as a predicate
            // somewhere can have users mixed into its object candidates.
            const bool fact_is_predicate = rel_types->count(fact) != 0;

            if (predicates.empty())
            {
                no_predicates = true; // cache store happens OUTSIDE the scope
            }
            else
            {
                for (Node p : predicates)
                {
                    for (Node s : right)
                    {
                        // s == p is the fact whose SUBJECT IS its predicate.
                        // insert_fact writes _left[fact] = {subject,
                        // predicate}, so the two roles share a single entry
                        // and skipping the candidate left such a fact with no
                        // reading at all. Since unification reads every
                        // candidate through this decomposition, those facts
                        // matched nothing -- `~ ~ ->` included, which zelph
                        // creates in every network and which is what licenses
                        // reading `~` as a predicate in the first place.
                        //
                        // The reading is offered but must hash back to the
                        // node (checked at the push below). That is exact
                        // rather than heuristic -- a fact node's ID IS
                        // create_hash(predicate, subject, objects) -- and it
                        // is what separates this case from a parent fact that
                        // happens to be a relation type as well.
                        const bool subject_is_predicate = (s == p);
                        if (left.count(s) == 0) continue; // Subject must be bidirectional

                        // Filter out "child fact" nodes: nodes that use `fact` as THEIR
                        // subject.  These appear bidirectionally connected because
                        // fact(fact, child_pred, {child_obj}) creates the bidirectional
                        // link fact <-> child_relation_node.

                        // Exclude variables from this check. Variables in rule patterns
                        // are hash nodes but act as primitive subjects. They
                        // must not be filtered out as child-facts.
                        if (Zelph::is_hash(s) && !Zelph::is_var(s))
                        {
                            Node s_pred = n->parse_relation_scoped(scope, *rel_types, s);
                            if (s_pred != 0 && s_pred != p)
                            {
                                const adjacency_set& s_right = scope.right(s);
                                const adjacency_set& s_left  = scope.left(s);

                                if (s_right.count(fact) > 0 && s_left.count(fact) > 0)
                                {
                                    // Heuristic: `fact` is the subject of `s` (i.e. s is a
                                    // child-fact) UNLESS `s` has another bidirectional node
                                    // that is itself a plausible subject — meaning it is NOT
                                    // a recognized relation type and NOT itself a child of `s`.
                                    //
                                    // A node x that is bidirectional with s could be:
                                    //   (a) s's actual subject  → fact is NOT the subject
                                    //   (b) a child-fact of s   → doesn't change that fact IS the subject
                                    //
                                    // To distinguish: if x is a hash node whose own predicate
                                    // differs from s's predicate, x is likely a child-fact of s
                                    // (case b).  Only non-hash nodes or hash nodes sharing s's
                                    // predicate qualify as alternative subjects (case a).
                                    bool fact_is_subject_of_s = true;
                                    for (Node x : s_right)
                                    {
                                        if (x == fact || x == s_pred) continue;
                                        if (s_left.count(x) > 0)
                                        {
                                            // x is bidirectional with s.
                                            // If x is itself a hash node with a DIFFERENT
                                            // predicate than s, it is a grandchild (child of s),
                                            // not an alternative subject.
                                            if (Zelph::is_hash(x))
                                            {
                                                Node x_pred = n->parse_relation_scoped(scope, *rel_types, x);
                                                if (x_pred != 0 && x_pred != s_pred)
                                                {
                                                    // x has a different predicate than s — could be:
                                                    // (a) a child-fact of s (grandchild of fact), OR
                                                    // (b) the genuine subject of s.
                                                    //
                                                    // Distinguish: if `fact` is bidirectional with x,
                                                    // then x is part of fact's sub-tree (case a).
                                                    // Otherwise x is an independent node — the genuine
                                                    // subject of s (case b).
                                                    if (scope.right(x).count(fact) > 0
                                                        && scope.left(x).count(fact) > 0)
                                                    {
                                                        // x is connected to fact → child-fact (direct)
                                                        continue;
                                                    }

                                                    // x is NOT directly connected to fact.
                                                    // It might still be a grandchild: a child-fact
                                                    // of s that sits deeper in the tree with no
                                                    // direct edge to fact.  Detect this by checking
                                                    // whether s is x's subject — i.e. s is the only
                                                    // bidirectional non-predicate neighbor of x.
                                                    {
                                                        bool                 x_is_child_of_s = true;
                                                        const adjacency_set& x_right2        = scope.right(x);
                                                        const adjacency_set& x_left2         = scope.left(x);
                                                        for (Node y : x_right2)
                                                        {
                                                            if (y == s || y == x_pred) continue;
                                                            if (x_left2.count(y) > 0)
                                                            {
                                                                // y is bidirectional with x and is
                                                                // neither s nor x's predicate.
                                                                // Before concluding that x has an
                                                                // alternative subject, check whether
                                                                // y is itself just a child-fact of x.
                                                                if (Zelph::is_hash(y))
                                                                {
                                                                    Node y_pred = n->parse_relation_scoped(scope, *rel_types, y);
                                                                    if (y_pred != 0 && y_pred != x_pred)
                                                                    {
                                                                        const adjacency_set& y_right3        = scope.right(y);
                                                                        const adjacency_set& y_left3         = scope.left(y);
                                                                        bool                 y_is_child_of_x = true;
                                                                        for (Node z_node : y_right3)
                                                                        {
                                                                            if (z_node == x || z_node == y_pred) continue;
                                                                            if (y_left3.count(z_node) > 0)
                                                                            {
                                                                                y_is_child_of_x = false;
                                                                                break;
                                                                            }
                                                                        }
                                                                        if (y_is_child_of_x)
                                                                        {
                                                                            continue; // y is child of x, not an alt subject
                                                                        }
                                                                    }
                                                                }
                                                                x_is_child_of_s = false;
                                                                break;
                                                            }
                                                        }
                                                        if (x_is_child_of_s)
                                                        {
                                                            continue; // x is grandchild (child of s)
                                                        }
                                                    }

                                                    // x is genuinely an alternative subject of s
                                                    fact_is_subject_of_s = false;
                                                    break;
                                                }
                                            }
                                            // x is a genuine alternative subject of s
                                            fact_is_subject_of_s = false;
                                            break;
                                        }
                                    }
                                    if (fact_is_subject_of_s)
                                    {
                                        continue; // skip: s is a child-fact
                                    }
                                }
                            }
                        }

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
                                    // A node that is itself used as a predicate collects
                                    // its USERS exactly where its objects sit: fact() draws
                                    // "F -> P" for the predicate and "O -> F" for an object,
                                    // so both land in the predicate's left set and the two
                                    // edges are indistinguishable from here. Reading a user
                                    // as another object made (a p b) come back as
                                    // "a p b (x ? y)" once the genuine store no longer
                                    // answered -- the typed and the reloaded network then
                                    // disagreed. What separates them is the candidate's OWN
                                    // reading: a user is a fact whose predicate is this very
                                    // node. Objects that are not fact nodes are never asked,
                                    // which matters: an atom object points at `fact` and
                                    // nothing else, so parse_relation would answer `fact`
                                    // for it too. Paid only when `fact` is a relation type
                                    // at all, which no ordinary data node is.
                                    if (fact_is_predicate
                                        && Zelph::is_hash(o) && !Zelph::is_var(o)
                                        && n->parse_relation_scoped(scope, *rel_types, o) == fact)
                                    {
                                        continue;
                                    }
                                    fs.objects.insert(o);
                                }
                            }
                        }

                        if (fs.objects.empty())
                        {
                            fs.objects.insert(s);
                        }
                        if (subject_is_predicate
                            && Zelph::create_hash(fs.predicate, fs.subject, fs.objects) != fact)
                        {
                            continue; // see the s == p note at the top of the loop
                        }
                        structures.push_back(fs);
                    }
                }
            }
        } // ReadScope released -- locking API, logging and cache stores are legal again

        if (no_predicates)
        {
            if (n->should_log(depth))
            {
                n->log(depth, "get_fact_structures", "Found predicates: 0");
            }
            n->store_fact_structures_cached(fact, shared_empty);
            return shared_empty;
        }

        // --- Hash verification ---
        // A fact node's ID IS create_hash(predicate, subject, objects), so a
        // candidate reading that does not hash back to the node cannot be the
        // one fact() was called with. Only used to prune an ambiguous candidate
        // set: a single candidate is kept even if it fails, because a partial
        // reading is still more useful than none (a network that lost edges to
        // a removal has no verifying reading at all).
        if (structures.size() > 1)
        {
            std::vector<FactStructure> verified;
            for (const auto& fs : structures)
            {
                if (Zelph::create_hash(fs.predicate, fs.subject, fs.objects) == fact)
                    verified.push_back(fs);
            }
            if (!verified.empty())
            {
                if (n->should_log(depth))
                {
                    n->log(depth, "get_fact_structures", "Hash verification: " + std::to_string(structures.size()) + " candidate(s) -> " + std::to_string(verified.size()) + " verified");
                }
                structures = std::move(verified);
            }
        }

        // --- Disambiguation ---
        if (structures.size() > 1)
        {
            if (n->should_log(depth))
            {
                n->log(depth, "get_fact_structures", "Disambiguation needed: " + std::to_string(structures.size()) + " structures found");
            }

            // Prefer structures with atomic (non-hash) subjects
            bool has_non_hash = false;
            for (const auto& fs : structures)
            {
                if (!Zelph::is_hash(fs.subject))
                {
                    has_non_hash = true;
                    break;
                }
            }

            if (has_non_hash)
            {
                std::vector<FactStructure> filtered;
                for (const auto& fs : structures)
                {
                    if (!Zelph::is_hash(fs.subject)) filtered.push_back(fs);
                }
                structures = std::move(filtered);

                if (n->should_log(depth))
                {
                    n->log(depth, "get_fact_structures", "After preferring non-hash subjects: " + std::to_string(structures.size()) + " left");
                }
            }
            else
            {
                // Among all-hash subjects, prefer Cons cells: they are semantic values,
                // not relation nodes that accidentally appear via bidirectional subject edges.
                bool has_cons = false;
                for (const auto& fs : structures)
                {
                    if (n->parse_relation(fs.subject) == n->core.Cons)
                    {
                        has_cons = true;
                        break;
                    }
                }
                if (has_cons)
                {
                    std::vector<FactStructure> filtered;
                    for (const auto& fs : structures)
                    {
                        if (n->parse_relation(fs.subject) == n->core.Cons)
                            filtered.push_back(fs);
                    }
                    structures = std::move(filtered);

                    if (n->should_log(depth))
                    {
                        n->log(depth, "get_fact_structures", "After preferring Cons subjects: " + std::to_string(structures.size()) + " left");
                    }
                }
            }
        }

        auto result = std::make_shared<FactStructureList>(std::move(structures));
        n->store_fact_structures_cached(fact, result);

        if (n->should_log(depth))
        {
            n->log(depth, "get_fact_structures", "Completed: Returning " + std::to_string(result->size()) + " structures");
        }

        return result;
    }

    // Convenience: return a single preferred structure (for reasoning/instantiation).
    inline FactStructure get_preferred_structure(const Zelph* n, Node fact, const int depth)
    {
        const auto results = get_fact_structures(n, fact, depth);
        if (results->empty()) return FactStructure{};
        return results->front(); // by-value copy: callers hold it independently of the cache
    }

    inline bool try_get_preferred_structure(const Zelph* n, Node fact, FactStructure& out, int depth)
    {
        out = get_preferred_structure(n, fact, depth);
        return out.predicate != 0;
    }

    // --- Pattern resolution (bindings -> denoted node) --------------------
    //
    // Resolve a pattern under variable bindings to the concrete node it
    // denotes, by pure hash lookups -- nothing is created. The read-only
    // sibling of reasoning.cpp's instantiate_fact (which materializes) and
    // of unification.cpp's ground_pattern (which stops at "denotable"):
    //
    //   Ok       the pattern denotes an EXISTING node
    //   Unbound  an unbound variable, or a variable-carrying structure that
    //            cannot be decomposed / is cyclic
    //   Missing  every variable is bound, but the denoted fact does not exist
    //
    // Exact object-set semantics, as in ground_pattern: a graph fact
    // carrying objects beyond the pattern's is a different node and is not
    // found. `containing` opts out of that -- see the parameter.
    enum class Resolve
    {
        Ok,
        Unbound,
        Missing
    };

    // The fact of this subject and predicate whose objects CONTAIN the given
    // ones, or 0. Unification matches a one-object condition against a fact
    // that carries more -- `(X p Y)` binds Y to b and to c of `a p b c`, and
    // the rule fires twice -- so a caller that reconstructs what the engine
    // did needs the node the engine matched, which the exact hash never
    // finds. Deliberately NOT the default: a prune asked for `a p b` must
    // not take `a p b c` with it, and a guard operand that denotes no fact
    // must stay absent.
    inline Node containing_fact(const Zelph* n, const Node subject, const Node predicate, const adjacency_set& objects)
    {
        for (const Node candidate : n->get_right(subject))
        {
            if (!n->has_right_edge(candidate, predicate)) continue;

            // The pointer is held: see the note at Zelph::get_fact_subjects.
            const auto structures = get_fact_structures(n, candidate, 1);
            for (const auto& fs : *structures)
            {
                if (fs.predicate != predicate || fs.subject != subject) continue;
                if (fs.objects.size() <= objects.size()) continue; // equal is the exact case

                bool all = true;
                for (const Node o : objects)
                {
                    if (fs.objects.count(o) == 0)
                    {
                        all = false;
                        break;
                    }
                }
                if (all) return candidate;
            }
        }

        return 0;
    }

    // `containing` true additionally accepts a fact that carries objects
    // BEYOND the pattern's, which is what unification matched (see
    // containing_fact). Only the proof reconstruction passes it.
    inline Resolve resolve_pattern(const Zelph* n, const Node pattern, const Variables& vars, Node& out, std::vector<Node>& history, const bool containing = false)
    {
        if (pattern == 0) return Resolve::Unbound;

        if (Zelph::is_var(pattern))
        {
            const auto it = vars.find(pattern);
            if (it == vars.end() || it->second == 0 || Zelph::is_var(it->second)) return Resolve::Unbound;
            out = it->second;
            return Resolve::Ok;
        }

        if (!Zelph::is_hash(pattern))
        {
            out = pattern; // plain atom
            return Resolve::Ok;
        }

        // Variable-free structure: hash-consing makes the pattern IDENTICAL
        // to the node it denotes -- no decomposition, no lookup. O(1) via
        // the closure flag, and it covers the overwhelming majority of
        // subterms in a rendered answer.
        if (!n->var_in_closure(pattern))
        {
            out = pattern;
            return n->exists(pattern) ? Resolve::Ok : Resolve::Missing;
        }

        for (const Node visited : history)
            if (visited == pattern) return Resolve::Unbound; // cyclic: be conservative
        history.push_back(pattern);

        const FactStructure fs = get_preferred_structure(n, pattern, 1);
        if (fs.predicate == 0)
        {
            history.pop_back();
            return Resolve::Unbound; // carries variables, but is undecomposable
        }

        Node    gs = 0;
        Resolve r  = resolve_pattern(n, fs.subject, vars, gs, history, containing);
        if (r != Resolve::Ok)
        {
            history.pop_back();
            return r;
        }

        Node gp = 0;
        r       = resolve_pattern(n, fs.predicate, vars, gp, history, containing);
        if (r != Resolve::Ok)
        {
            history.pop_back();
            return r;
        }

        adjacency_set gobjs;
        for (const Node o : fs.objects)
        {
            Node go = 0;
            r       = resolve_pattern(n, o, vars, go, history, containing);
            if (r != Resolve::Ok)
            {
                history.pop_back();
                return r;
            }
            gobjs.insert(go);
        }
        history.pop_back();

        const Answer ans = n->check_fact(gs, gp, gobjs);
        out              = ans.relation(); // the hash is deterministic even for absent facts
        if (ans.is_known()) return Resolve::Ok;

        if (containing)
        {
            if (const Node wider = containing_fact(n, gs, gp, gobjs); wider != 0)
            {
                out = wider;
                return Resolve::Ok;
            }
        }

        return Resolve::Missing;
    }

    // Convenience for display code: the denoted node, or the pattern itself
    // when the bindings do not resolve it. Free of charge without bindings.
    inline Node resolve_pattern_node(const Zelph* n, const Node pattern, const Variables& vars)
    {
        if (n == nullptr || vars.empty()) return pattern;

        Node              out = 0;
        std::vector<Node> history;
        return resolve_pattern(n, pattern, vars, out, history) == Resolve::Ok ? out : pattern;
    }
}
