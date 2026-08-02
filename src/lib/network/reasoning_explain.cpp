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

// Proof reconstruction: one backward step per level over the rule
// structures stored in the graph. For a target fact F:
//   1. enumerate rules (facts whose predicate is core.Causes),
//   2. unify each rule consequence template with F (seed-mode Unification:
//      the candidate set is exactly {F}),
//   3. join the remaining positive conditions against asserted facts
//      (backtracking; conditions that become fully ground under the current
//      bindings are verified by pure hash lookup, everything else through
//      the same anchored Unification scan the forward pass uses),
//   4. check != guards and verify NAF conditions as absent under the
//      complete bindings (stratification makes them ground after the join),
//   5. recurse on the positive premises, excluding facts on the current
//      path (no fact may support itself).
// Every derivation happened in finitely many forward steps, so a
// well-founded justification exists; the path exclusion only cuts the
// infinite regress, never the proof.
//
// Read-only by construction: resolve_pattern and check_fact are pure hash
// lookups, and Unification is used without a thread pool, so it only reads
// the graph. Nothing is created.

#include "reasoning.hpp"

#include "fact_structure.hpp"
#include "unification.hpp"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    namespace
    {
        struct ExplainContext
        {
            Zelph*      z{nullptr}; // mutable view for the Unification API; never used to write
            Node        nn_pred{0}; // node named "nn" in lang "zelph" (approx conditions); 0 = inactive
            std::size_t max_depth{0};

            std::unordered_map<Node, std::shared_ptr<ProofNode>> memo;
            std::unordered_set<Node>                             path;
        };

        // Split a rule's condition part into positive conditions, NAF
        // conditions, != guards, and detect neural conditions. Same detection
        // scheme as Reasoning::evaluate: conjunction sets via
        // (IsA Conjunction) plus PartOf membership, negation via the
        // (IsA Negation) tag on the pattern itself, guards/neural via the
        // condition's declared relation type.
        void collect_conditions(Zelph* z, const Node nn_pred, const Node part, std::vector<Node>& positive, std::vector<Node>& negative, std::vector<Node>& guards, bool& neural)
        {
            if (z->check_fact(part, z->core.IsA, {z->core.Conjunction}).is_known())
            {
                for (const Node rel : z->get_right(part))
                {
                    if (z->parse_relation(rel) != z->core.PartOf) continue;
                    adjacency_set objs;
                    const Node    element = z->parse_fact(rel, objs);
                    if (element && objs.count(part) == 1)
                        collect_conditions(z, nn_pred, element, positive, negative, guards, neural);
                }
                return;
            }

            if (z->check_fact(part, z->core.IsA, {z->core.Negation}).is_known())
            {
                negative.push_back(part);
                return;
            }

            const adjacency_set rels = z->filter(part, z->core.IsA, z->core.RelationTypeCategory);
            if (rels.size() == 1)
            {
                if (*rels.begin() == z->core.Unequal)
                {
                    guards.push_back(part);
                    return;
                }
                if (nn_pred != 0 && *rels.begin() == nn_pred)
                {
                    neural = true;
                    return;
                }
            }

            positive.push_back(part);
        }

        // != guard under complete bindings: violated only when both sides
        // resolve to the SAME concrete node (mirrors the forward pass).
        bool guard_holds(Zelph* z, const Node guard, const Node rule, const Variables& vars)
        {
            adjacency_set objs;
            const Node    subj = z->parse_fact(guard, objs, rule);
            const Node    obj  = objs.empty() ? 0 : *objs.begin();
            if (subj == 0 || obj == 0) return true; // malformed guard: the forward pass skips it too

            Node lhs = subj;
            Node rhs = obj;
            if (Zelph::is_var(lhs))
                if (const auto it = vars.find(lhs); it != vars.end()) lhs = it->second;
            if (Zelph::is_var(rhs))
                if (const auto it = vars.find(rhs); it != vars.end()) rhs = it->second;

            if (!Zelph::is_var(lhs) && !Zelph::is_var(rhs)) return lhs != rhs;
            return true; // an unbound side cannot witness a violation
        }

        // Backtracking join: bind all variables of `conditions` such that
        // every instantiated condition is an asserted fact NOT on the current
        // path. Conditions that are fully ground under the current bindings
        // are verified by hash lookup; the first non-ground condition is
        // enumerated through the same (anchored, sequential) Unification the
        // forward pass uses -- seeded with the current bindings, so matches
        // only extend them consistently. On success, `bindings` holds the
        // complete solution and `premises` the instantiated condition facts.
        bool join_conditions(ExplainContext& ctx, const Node rule, std::vector<Node> conditions, Variables& bindings, std::vector<Node>& premises)
        {
            if (conditions.empty()) return true;

            // 1. Prefer a fully ground condition: O(1) verification, and it
            //    binds nothing, so failure of the remainder is final.
            for (std::size_t i = 0; i < conditions.size(); ++i)
            {
                Node              premise = 0;
                std::vector<Node> history;
                const Resolve     r = resolve_pattern(ctx.z, conditions[i], bindings, premise, history);
                if (r == Resolve::Unbound) continue;
                if (r == Resolve::Missing) return false;   // cannot hold under these bindings
                if (ctx.path.count(premise)) return false; // would support itself

                std::vector<Node> rest = conditions;
                rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(i));
                premises.push_back(premise);
                if (join_conditions(ctx, rule, std::move(rest), bindings, premises)) return true;
                premises.pop_back();
                return false;
            }

            // 2. Enumerate the first non-ground condition.
            const Node        condition = conditions.front();
            std::vector<Node> rest(conditions.begin() + 1, conditions.end());

            const auto  vars = std::make_shared<Variables>(bindings);
            const auto  uneq = std::make_shared<Variables>();
            Unification u(ctx.z, condition, rule, vars, uneq, /*pool*/ nullptr, /*log_depth*/ 1, /*profiler*/ nullptr);

            while (const std::shared_ptr<Variables> match = u.Next())
            {
                Variables joined = bindings;
                for (const auto& [k, v] : *match)
                    joined[k] = v;

                Node              premise = 0;
                std::vector<Node> history;
                if (resolve_pattern(ctx.z, condition, joined, premise, history) != Resolve::Ok)
                    continue; // exact-object-set caveat (see resolve_pattern)
                if (ctx.path.count(premise)) continue;

                premises.push_back(premise);
                std::vector<Node> rest_copy = rest;
                if (join_conditions(ctx, rule, std::move(rest_copy), joined, premises))
                {
                    bindings = std::move(joined);
                    return true;
                }
                premises.pop_back();
            }
            return false;
        }

        std::shared_ptr<ProofNode> reconstruct(ExplainContext& ctx, const Node fact, const std::size_t depth)
        {
            if (const auto it = ctx.memo.find(fact); it != ctx.memo.end())
                return it->second; // shared subproof (DAG)

            auto proof  = std::make_shared<ProofNode>();
            proof->fact = fact;

            if (ctx.max_depth != 0 && depth >= ctx.max_depth)
            {
                proof->status = ProofNode::Status::Truncated;
                return proof; // deliberately NOT memoized: a later, shallower
                              // occurrence may still be expandable
            }

            Zelph* const z         = ctx.z;
            const Node   fact_pred = z->predicate_of(fact);

            ctx.path.insert(fact);
            bool any_rule_unified = false;

            if (fact_pred != 0)
            {
                // Rules are the facts whose predicate is core.Causes; their
                // subject is the condition part, their objects the consequence
                // templates. Candidate enumeration exactly as in the forward
                // scan: left adjacency of the relation node, validated via
                // parse_relation.
                for (const Node rule : z->get_left(z->core.Causes))
                {
                    if (z->parse_relation(rule) != z->core.Causes) continue;

                    adjacency_set consequences;
                    const Node    condition_part = z->parse_fact(rule, consequences);
                    if (condition_part == 0 || consequences.empty()) continue;

                    std::vector<Node> positive, negative, guards;
                    bool              neural = false;
                    collect_conditions(z, ctx.nn_pred, condition_part, positive, negative, guards, neural);

                    for (const Node cons : consequences)
                    {
                        if (cons == z->core.Contradiction) continue;

                        // Seed-mode Unification: the candidate set is exactly
                        // {fact}, so Next() enumerates precisely the ways the
                        // consequence template unifies with the target.
                        const auto  vars = std::make_shared<Variables>();
                        const auto  uneq = std::make_shared<Variables>();
                        Unification u(z, cons, rule, vars, uneq, nullptr, 1, /*profiler*/ nullptr, fact, fact_pred);

                        while (const std::shared_ptr<Variables> match = u.Next())
                        {
                            any_rule_unified = true;

                            // Neural premises cannot be verified structurally;
                            // do not reconstruct via this rule.
                            if (neural) break;

                            // The match may bind the template to a fact that
                            // merely SUBSUMES the consequence (extra objects);
                            // accept only instantiations denoting exactly F.
                            {
                                Node              hit = 0;
                                std::vector<Node> history;
                                if (resolve_pattern(z, cons, *match, hit, history) == Resolve::Ok && hit != fact)
                                    continue;
                            }

                            Variables         bindings = *match;
                            std::vector<Node> premises;
                            if (!join_conditions(ctx, rule, positive, bindings, premises)) continue;

                            bool ok = true;
                            for (const Node g : guards)
                            {
                                if (!guard_holds(z, g, rule, bindings))
                                {
                                    ok = false;
                                    break;
                                }
                            }
                            if (!ok) continue;

                            // NAF: ground after the join (stratification); a
                            // condition that has since become true in the
                            // CURRENT graph disqualifies this justification.
                            std::vector<Node> absent;
                            for (const Node m : negative)
                            {
                                Node              ground = 0;
                                std::vector<Node> history;
                                const Resolve     r = resolve_pattern(z, m, bindings, ground, history);
                                if (r == Resolve::Ok)
                                {
                                    ok = false; // the negated fact is present NOW
                                    break;
                                }
                                // Keep the rule's PATTERN, not the ground node:
                                // the pattern carries the (IsA Negation) tag, so
                                // it renders as ¬(...) on its own, and the
                                // caller supplies `bindings` to substitute the
                                // variables the join fixed. A ground node would
                                // render without the negation and force the
                                // caller to re-add it -- which is how the
                                // display grew a second ¬ around a pattern that
                                // already had one.
                                absent.push_back(m);
                            }
                            if (!ok) continue;

                            proof->status   = ProofNode::Status::Derived;
                            proof->rule     = rule;
                            proof->absent   = std::move(absent);
                            proof->bindings = bindings;
                            for (const Node premise : premises)
                                proof->premises.push_back(reconstruct(ctx, premise, depth + 1));
                            break; // one justification per fact
                        }

                        if (proof->status == ProofNode::Status::Derived || neural) break;
                    }

                    if (proof->status == ProofNode::Status::Derived) break;
                }
            }

            ctx.path.erase(fact);

            if (proof->status != ProofNode::Status::Derived)
                proof->status = ctx.z->is_rule_pattern(fact) ? ProofNode::Status::RulePattern
                              : any_rule_unified      ? ProofNode::Status::Unfounded
                                                      : ProofNode::Status::Axiom;

            // Memoize only path-independent results: Derived (a found proof
            // stays a proof) and Axiom (no rule consequence unifies -- a
            // property of the fact alone). Unfounded may be an artifact of
            // the current path's exclusions and is recomputed; Truncated is
            // depth-dependent (see above).
            if (proof->status == ProofNode::Status::Derived
                || proof->status == ProofNode::Status::Axiom
                || proof->status == ProofNode::Status::RulePattern)
                ctx.memo.emplace(fact, proof);

            return proof;
        }
    } // namespace

    std::shared_ptr<ProofNode> Reasoning::explain(const Node fact, const std::size_t max_depth) const
    {
        // The unification machinery takes a mutable engine pointer, but this
        // reconstruction never creates nodes or edges: resolve_pattern and
        // check_fact are pure hash lookups, and Unification is constructed
        // without a thread pool, so it only reads the graph.
        Reasoning* const self = const_cast<Reasoning*>(this);

        ExplainContext ctx;
        ctx.z         = self;
        ctx.nn_pred   = get_node("nn", "zelph");
        ctx.max_depth = max_depth;

        return reconstruct(ctx, fact, 0);
    }
} // namespace zelph::network
