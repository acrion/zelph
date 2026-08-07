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

#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

using namespace zelph::network;

// A transitive path condition, written `(X P⁺ Y)` or `(X P∗ Y)`.
//
// The condition IS the tag fact (pattern closure mode), where pattern is the
// ordinary one-step fact (X P Y) and mode is "one-or-more" or "zero-or-more".
// That is the shape the neural condition already uses, and it is not a matter
// of taste: a fact's objects are an unordered SET, so the spelling
// `(X closure P Y)` could not tell the predicate to walk from the target to
// reach -- entering `sub pred zeta alpha` and printing it back gives
// `sub pred alpha zeta`.
//
// The walk itself is not implemented here. Zelph::transitive_targets and
// transitive_sources ARE the closure engine that `zelph/closure` exposes and
// that sparql.zph's p+/p* already call -- an indexed closure over the
// predicate, built once and cached next to the .bin, not a walk per query.
void Reasoning::evaluate_closure(const Node condition, const RulePos& rule, ReasoningContext& ctx, const int depth)
{
    // The tag fact: subject is the one-step pattern, the single object says
    // which closure is meant.
    adjacency_set modes;
    const Node    pattern = parse_fact(condition, modes, rule.node);

    if (pattern == 0 || modes.size() != 1)
    {
        if (should_log(depth)) log(depth, "closure", "malformed path condition (expected (pattern closure mode))");
        return;
    }

    const Node mode = *modes.begin();
    if (mode != _closure_one_plus && mode != _closure_zero_plus)
    {
        if (should_log(depth)) log(depth, "closure", "unknown closure mode " + format(mode));
        return;
    }
    const bool reflexive = (mode == _closure_zero_plus);

    // Decompose the one-step pattern. parent=condition keeps the tag fact
    // itself out of the pattern's subject candidates, exactly as in
    // evaluate_neural.
    adjacency_set pattern_objects;
    const Node    start_pattern = parse_fact(pattern, pattern_objects, condition);
    const Node    step          = parse_relation(pattern);

    if (start_pattern == 0 || step == 0 || pattern_objects.size() != 1)
    {
        if (should_log(depth)) log(depth, "closure", "unsupported path shape (need S P O with exactly one object)");
        return;
    }
    const Node target_pattern = *pattern_objects.begin();

    auto resolved = [&](Node n) -> Node
    { return Zelph::Impl::is_var(n) ? string::get(*rule.variables, n, n) : n; };

    const Node from = resolved(start_pattern);
    const Node to   = resolved(target_pattern);
    const Node p    = resolved(step);

    if (Zelph::Impl::is_var(p))
    {
        // A closure is indexed per predicate, so the predicate has to name
        // one. Quantifying over predicates stays the business of an ordinary
        // condition, which can do it.
        if (should_log(depth)) log(depth, "closure", "the predicate of a path condition must be a concrete predicate, not a variable");
        return;
    }

    const bool from_free = Zelph::Impl::is_var(from);
    const bool to_free   = Zelph::Impl::is_var(to);

    if (from_free && to_free)
    {
        // Both ends open would enumerate the whole closure of the graph.
        // SPARQL refuses the same case, and for the same reason.
        if (should_log(depth)) log(depth, "closure", "a path condition needs at least one bound end");
        out("Error: a path condition needs at least one bound end -- \""
                + format(step) + "\" with both ends free would enumerate every path in the graph. "
                                 "Bind one side with a preceding condition.",
            true);
        return;
    }

    if (!from_free && !to_free)
    {
        // --- Reachability test ---
        const adjacency_set reachable = transitive_targets(from, p, reflexive);
        const bool          holds     = reachable.count(to) != 0;

        if (should_log(depth))
            log(depth, "closure", "path test " + format(from) + " " + format(step) + " " + format(to) + (holds ? " HOLDS" : " FAILS"));

        if (!holds) return;

        proceed_after_condition(rule, ctx, depth, rule.variables, rule.unequals, rule.confidence);
        return;
    }

    // --- Generator mode ---
    // One binding per reachable node. The bound end anchors the closure, so
    // the direction decides which of the two indexed walks answers it.
    const Node          open_var = from_free ? start_pattern : target_pattern;
    const adjacency_set found    = from_free
                                     ? transitive_sources(to, p, reflexive)
                                     : transitive_targets(from, p, reflexive);

    if (should_log(depth))
        log(depth, "closure", "path generated " + std::to_string(found.size()) + " binding(s) for " + format(open_var));

    for (const Node reached : found)
    {
        auto bindings         = std::make_shared<Variables>(*rule.variables);
        (*bindings)[open_var] = reached;

        if (contradicts(*bindings, *rule.unequals)) continue;

        proceed_after_condition(rule, ctx, depth, bindings, rule.unequals, rule.confidence);
    }
}
