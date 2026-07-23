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
                        if (s == p) continue;
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
                                    fs.objects.insert(o);
                                }
                            }
                        }

                        if (fs.objects.empty())
                        {
                            fs.objects.insert(s);
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
        // [Kommentarblock unverändert aus der aktuellen Datei übernehmen]
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
}
