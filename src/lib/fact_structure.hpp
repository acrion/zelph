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

#include "zelph_impl.hpp"

#include <vector>

namespace zelph::network
{
    struct FactStructure
    {
        Node                     subject{};
        Node                     predicate{};
        std::unordered_set<Node> objects;
    };

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
    // When `prefer_single` is true (used by reasoning/instantiation), only
    // the single best structure is returned.  When false (used by unification),
    // all valid interpretations after disambiguation are returned.
    inline std::vector<FactStructure> get_fact_structures(
        const Zelph* n,
        Node         fact,
        bool         prefer_single,
        int          depth)
    {
        std::vector<FactStructure> structures;

        if (n->should_log(depth))
        {
            n->log(depth, "get_fact_structures", "Starting for fact: " + n->format(fact) + ", prefer_single: " + std::to_string(prefer_single));
        }

        if (fact == 0 || !n->exists(fact)) return structures;

        // Zelph Topology:
        // S <-> F (Subject is bidirectional)
        // F -> P  (Predicate is outgoing)
        // O -> F  (Object is incoming)

        adjacency_set right = n->get_right(fact); // Contains P and S (and Parent-Facts P' where F <-> P')
        adjacency_set left  = n->get_left(fact);  // Contains O and S (and Parent-Facts P')

        if (n->should_log(depth))
        {
            n->log(depth, "get_fact_structures", "Right neighbors: " + std::to_string(right.size()) + ", Left neighbors: " + std::to_string(left.size()));
        }

        adjacency_set predicates;
        for (Node p : right)
        {
            if (n->check_fact(p, n->core.IsA, {n->core.RelationTypeCategory}).is_known())
            {
                predicates.insert(p);
            }
        }

        if (n->should_log(depth))
        {
            n->log(depth, "get_fact_structures", "Found predicates: " + std::to_string(predicates.size()));
        }

        if (predicates.empty()) return structures;

        for (Node p : predicates)
        {
            if (n->should_log(depth + 1))
            {
                n->log(depth + 1, "get_fact_structures", "Processing predicate: " + n->format(p));
            }

            for (Node s : right)
            {
                if (n->should_log(depth + 2))
                {
                    n->log(depth + 2, "get_fact_structures", "Checking potential subject s: " + n->format(s));
                }

                if (s == p)
                {
                    if (n->should_log(depth + 2))
                    {
                        n->log(depth + 2, "get_fact_structures", "Skipping s == p");
                    }
                    continue;
                }
                if (left.count(s) == 0)
                {
                    if (n->should_log(depth + 2))
                    {
                        n->log(depth + 2, "get_fact_structures", "Skipping: s not bidirectional");
                    }
                    continue; // Subject must be bidirectional
                }

                // Filter out "child fact" nodes: nodes that use `fact` as THEIR
                // subject.  These appear bidirectionally connected because
                // fact(fact, child_pred, {child_obj}) creates the bidirectional
                // link fact <-> child_relation_node.
                if (Zelph::Impl::is_hash(s)) // s is fact or variable - TODO: exclude variables?
                {
                    if (n->should_log(depth + 2))
                    {
                        n->log(depth + 2, "get_fact_structures", "s is hash: Checking for child-fact");
                    }

                    Node s_pred = n->parse_relation(s);
                    if (s_pred != 0 && s_pred != p)
                    {
                        adjacency_set s_right = n->get_right(s);
                        adjacency_set s_left  = n->get_left(s);

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
                                if (n->should_log(depth + 3))
                                {
                                    n->log(depth + 3, "get_fact_structures", "Checking bidirectional x: " + n->format(x));
                                }

                                if (x == fact || x == s_pred)
                                {
                                    if (n->should_log(depth + 3))
                                    {
                                        n->log(depth + 3, "get_fact_structures", "Skipping x == fact or s_pred");
                                    }
                                    continue;
                                }
                                if (s_left.count(x) > 0)
                                {
                                    // x is bidirectional with s.
                                    // If x is itself a hash node with a DIFFERENT
                                    // predicate than s, it is a grandchild (child of s),
                                    // not an alternative subject.
                                    if (Zelph::Impl::is_hash(x))
                                    {
                                        Node x_pred = n->parse_relation(x);
                                        if (x_pred != 0 && x_pred != s_pred)
                                        {
                                            // x has a different predicate than s — could be:
                                            // (a) a child-fact of s (grandchild of fact), OR
                                            // (b) the genuine subject of s (a complex fact
                                            //     node that happens to have a different predicate).
                                            //
                                            // Distinguish: if `fact` is bidirectional with x,
                                            // then x is part of fact's sub-tree (case a).
                                            // Otherwise x is an independent node — the genuine
                                            // subject of s (case b).
                                            if (n->get_right(x).count(fact) > 0
                                                && n->get_left(x).count(fact) > 0)
                                            {
                                                // x is connected to fact → child-fact
                                                if (n->should_log(depth + 3))
                                                {
                                                    n->log(depth + 3, "get_fact_structures", "x is child-fact (different pred, connected to fact), continue");
                                                }
                                                continue;
                                            }
                                            // x is NOT connected to fact → genuine alternative subject
                                            fact_is_subject_of_s = false;
                                            if (n->should_log(depth + 3))
                                            {
                                                n->log(depth + 3, "get_fact_structures", "x has different pred but NOT connected to fact -> genuine alt subject x=" + n->format(x));
                                            }
                                            break;
                                        }
                                    }
                                    // x is a genuine alternative subject of s
                                    fact_is_subject_of_s = false;
                                    if (n->should_log(depth + 3))
                                    {
                                        n->log(depth + 3, "get_fact_structures", "Found genuine alternative subject x=" + n->format(x) + " -> fact_is_subject_of_s=false");
                                    }
                                    break;
                                }
                            }
                            if (fact_is_subject_of_s)
                            {
                                if (n->should_log(depth + 2))
                                {
                                    n->log(depth + 2, "get_fact_structures", "Skipping: s is child-fact (fact is subject of s)");
                                }
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

                if (n->should_log(depth + 1))
                {
                    n->log(depth + 1, "get_fact_structures", "Added structure: subject=" + n->format(fs.subject) + ", predicate=" + n->format(fs.predicate) + ", objects_count=" + std::to_string(fs.objects.size()));
                }
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
                if (!Zelph::Impl::is_hash(fs.subject))
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
                    if (!Zelph::Impl::is_hash(fs.subject)) filtered.push_back(fs);
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

        if (prefer_single && structures.size() > 1)
        {
            structures.resize(1);

            if (n->should_log(depth))
            {
                n->log(depth, "get_fact_structures", "prefer_single: Reduced to 1 structure");
            }
        }

        if (n->should_log(depth))
        {
            n->log(depth, "get_fact_structures", "Completed: Returning " + std::to_string(structures.size()) + " structures");
        }

        return structures;
    }

    // Convenience: return a single preferred structure (for reasoning/instantiation).
    inline FactStructure get_preferred_structure(const Zelph* n, Node fact, const int depth)
    {
        auto results = get_fact_structures(n, fact, true, depth);
        if (results.empty()) return FactStructure{};
        return results[0];
    }
}
