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

// Under what circumstances a rule featuring a negated condition may be
// evaluated.
//
// A negation's validity depends entirely on the facts it verifies as absent: it
// can only be evaluated once no further fact can be generated that would
// satisfy its pattern. Positive rules avoid this issue entirely -- whatever
// they produce remains true -- so they run whenever there is pending work, and
// the sole concern becomes the ORDER of rules that perform negation. Up to
// version 1.0.1, all such rules resided within a single deferred stratum and
// ran in a single pass, which was accurate only when no negated rule could
// generate the very facts that another negation depends on. The NAND bootstrap
// and the simplifier's fallback, both evaluated in a single run, were the first
// instances where this approach faltered: the fallback tested (T rw S) before
// the gate rule had produced the digit tables that the rewrite relies upon.
//
// The dependency analysis operates over PREDICATES. A rule depends on another
// if the second can create a fact -- across any depth of its consequence, as
// instantiating (T rw (K * U)) creates the product as well -- whose predicate
// is either read directly by the first or appears within a negation. This
// approach is coarser than unifying patterns, and this breadth is intentional:
// the math stack employs `=` uniformly across every layer, making pattern
// unification lead to identical components, albeit at the price of a unifier
// across templates.
//
// A predicate variable in a position meant for a predicate represents any
// possible predicate. A rule whose consequence is another rule is considered
// to create what that rule will create, as read off that rule's consequence;
// the written rule undergoes analysis once it comes into existence. One
// specific scenario is more limited: a rule that only extends the predicates
// it reads, like transitivity applied to a predicate variable, is excluded
// from cycles, and instead, what it depends on is passed forward (as
// indicated by footprint and negation_levels).
//
// The rules subsequently partition into strongly connected components. A
// component where negations reach back into itself -- specifically, where the
// simplifier's fallback negates rw, and rw is determined by the output of that
// fallback -- admits no stratified interpretation at all; it keeps the
// alternating evaluation it has consistently used. This approach is sound only
// when each negation inside it is settled by facts derived during the same
// positive saturation, which is the argument that symbolic-core's header
// presents concerning the fallback (see reasoning.cpp); a program that is
// locally stratified does not receive this guarantee.
// The analysis ensures the part that stratification makes possible: a negated
// rule executes solely after every rule whose facts it tests under negation --
// either directly or via positive rules in between -- has exhausted its
// derivations. A rule that reads such facts exclusively in a positive manner
// does not operate at a lower level than the rule that derives them,
// potentially sharing the same level, where the two alternate (see final_after
// in negation_levels).

#include "reasoning.hpp"

#include "fact_structure.hpp"
#include "zelph_impl.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace zelph::network;

namespace
{
    // At every level of nesting, each predicate inside a pattern. A
    // variable placed within a predicate position can stand for any
    // existing predicate; it is collected into `vars`, and the caller
    // determines its significance.
    void collect_predicates(const Zelph* z, const Node pattern, std::unordered_set<Node>& preds, std::unordered_set<Node>& vars, std::vector<Node>& history)
    {
        if (pattern == 0 || Zelph::Impl::is_var(pattern) || !Zelph::Impl::is_hash(pattern)) return;
        if (std::find(history.begin(), history.end(), pattern) != history.end()) return;
        history.push_back(pattern);

        const FactStructure fs = get_preferred_structure(z, pattern, 1);
        if (fs.subject != 0)
        {
            if (Zelph::Impl::is_var(fs.predicate))
                vars.insert(fs.predicate);
            else if (fs.predicate != 0)
                preds.insert(fs.predicate);

            collect_predicates(z, fs.subject, preds, vars, history);
            collect_predicates(z, fs.predicate, preds, vars, history);
            for (const Node o : fs.objects)
                collect_predicates(z, o, preds, vars, history);
        }

        history.pop_back();
    }
}

void Reasoning::read_footprint(const Node condition, RuleFootprint& f)
{
    std::vector<Node> history;

    if (is_condition_set(condition))
    {
        for (const Node rel : _pImpl->get_right(condition))
        {
            if (parse_relation(rel) != core.PartOf) continue;
            adjacency_set objs;
            const Node    element = parse_fact(rel, objs);
            if (element && objs.count(condition) == 1) read_footprint(element, f);
        }
        return;
    }

    const adjacency_set rels    = filter(condition, core.IsA, core.RelationTypeCategory);
    const Node          rel     = rels.size() == 1 ? *rels.begin() : Node{0};
    const bool          neural  = _nn_pred != 0 && rel == _nn_pred;
    const bool          closure = _closure_pred != 0 && rel == _closure_pred;
    // Check mode re-tests a negation using explain(), which examines no
    // net whatsoever (refer to footprint for path conditions).
    if (neural) f.checkable = false;

    if (is_negated_condition(condition, 1))
    {
        // Neither the re-test nor explain() walks a negated path: both
        // would match it as a literal fact.
        if (closure) f.checkable = false;
        f.has_negation = true;
        f.negated_conditions.push_back(condition);
        std::unordered_set<Node> vars;
        collect_predicates(this, condition, f.negates, vars, history);
        if (!vars.empty()) f.negates_any = true;
        return;
    }

    if (rel == core.Unequal) return; // a guard compares, it reads no fact
    if (neural)
    {
        // The net processes whatever facts are fed into it, and no
        // indication here says which ones.
        f.reads_any     = true;
        f.reads_unnamed = true;
        return;
    }
    if (closure)
    {
        // The subject of the tag is the path pattern; its predicate is
        // the one the walk follows.
        adjacency_set modes;
        const Node    path = parse_fact(condition, modes, 0);
        collect_predicates(this, path, f.reads, f.reads_vars, history);
        if (!f.reads_vars.empty()) f.reads_any = true;
        f.paths.push_back(path);
        return;
    }

    // The predicate in a plain condition corresponds to the predicate of a
    // fact that is present whenever the condition matches, meaning a
    // variable there is bound to a predicate that already has facts (see
    // footprint).
    if (Zelph::Impl::is_var(rel)) f.binds_vars.insert(rel);
    collect_predicates(this, condition, f.reads, f.reads_vars, history);
    if (!f.reads_vars.empty()) f.reads_any = true;

    std::vector<Node> var_history;
    collect_variables(this, condition, f.joined_vars, 1, var_history);
}

Reasoning::RuleFootprint Reasoning::footprint(const Node rule)
{
    RuleFootprint f;

    adjacency_set consequences;
    const Node    condition = parse_fact(rule, consequences);
    if (condition && condition != core.Causes) read_footprint(condition, f);

    // A consequence that is a rule does not generate anything until the rule
    // itself comes into existence, and only then does it produce whatever
    // its own consequence creates -- which can be determined from the
    // pattern now, at any level of depth and via a rule that the written
    // rule itself generates. Thus, this is what the rule is considered to
    // produce. A variable located within a predicate slot there represents
    // any predicate, just as it does elsewhere: the outer conditions may
    // bind it to anything. What the written rule reads and negates remains
    // unknown until the rule exists; at that moment, it is collected, and
    // the levels are recalculated (Reasoning::run and
    // run_fixpoint_seminaive).
    bool                                 derives_rule = false;
    std::unordered_set<Node>             written_vars;
    const std::function<void(Node, int)> written = [&](const Node inner, const int nesting)
    {
        adjacency_set inner_consequences;
        parse_fact(inner, inner_consequences);
        for (const Node ic : inner_consequences)
        {
            if (ic == core.Contradiction) continue;
            if (deduction_is_rule(ic))
            {
                if (nesting < 16)
                    written(ic, nesting + 1);
                else
                    f.produces_any = true;
                continue;
            }
            std::vector<Node> history;
            collect_predicates(this, ic, f.produces, written_vars, history);
        }
    };
    for (const Node c : consequences)
    {
        if (c == core.Contradiction) continue;
        if (deduction_is_rule(c))
        {
            derives_rule = true;
            written(c, 1);
            continue;
        }
        std::vector<Node> history;
        collect_predicates(this, c, f.produces, f.produces_vars, history);
        std::vector<Node> var_history;
        collect_variables(this, c, f.joined_vars, 1, var_history);
    }
    if (!written_vars.empty()) f.produces_any = true;

    // explain() walks a path only after both its endpoints have been bound
    // by the consequence and the plain conditions (and the predicate,
    // should it be a variable). A path that would have to BIND one of them
    // -- the generator reading -- makes explain() skip the rule, causing a
    // fact derived via it to appear lost to the re-test, even though it
    // remains derived. Such a rule is not re-tested.
    for (const Node path : f.paths)
    {
        std::unordered_set<Node> path_vars;
        std::vector<Node>        var_history;
        collect_variables(this, path, path_vars, 1, var_history);
        for (const Node v : path_vars)
            if (f.joined_vars.count(v) == 0) f.checkable = false;
    }

    // A variable in a predicate position of the consequence may represent any
    // predicate. One case is more restricted: (R is transitive, A R B, B R C)
    // => (A R C) creates an R fact solely for an R it has just read, meaning
    // it only extends predicates that already possess facts. This holds when
    // R serves as the predicate of a plain positive condition and the rule
    // reads no other predicate variable -- introducing a second one, as in
    // (R pairs S, A R B, B S C) => (A R C), would make the R facts rely on
    // the facts of every S. A rule involving negation or a neural condition,
    // or one that derives a rule, is never transparent. negation_levels
    // specifies what a transparent rule depends on instead.
    if (!f.produces_vars.empty())
    {
        f.transparent = !f.has_negation && !derives_rule && !f.reads_unnamed
                     && f.produces_vars.size() == 1 && f.reads_vars == f.produces_vars
                     && f.binds_vars.count(*f.produces_vars.begin()) == 1;
        if (!f.transparent) f.produces_any = true;
    }
    return f;
}

Reasoning::NegationLevels Reasoning::strata()
{
    // The collection of the classic loop within run(). The semi-naive index
    // also skips a rule lacking a condition, which reads nothing and thus
    // shifts no level.
    std::vector<Node> rules;
    for (const Node rule : _pImpl->get_left(core.Causes))
        if (!is_mentioned(rule)) rules.push_back(rule);
    return negation_levels(rules);
}

Reasoning::NegationLevels Reasoning::negation_levels(const std::vector<Node>& rules)
{
    const std::size_t          n = rules.size();
    std::vector<RuleFootprint> footprint(n);

    // The classic path only resolves these once the rule has been applied,
    // which occurs after the schedule has been built.
    _nn_pred      = get_node("nn", "zelph");
    _closure_pred = get_node("closure", "zelph");

    // --- What each rule reads and creates ---------------------------------
    for (std::size_t i = 0; i < n; ++i)
        footprint[i] = this->footprint(rules[i]);

    // --- Who depends on whom ----------------------------------------------
    // edges[r] holds (s, negative): s reads something r can create.
    std::unordered_map<Node, std::vector<std::size_t>> producers;
    std::vector<std::size_t>                           any_producers;
    std::vector<std::size_t>                           all_producers;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (footprint[i].produces_any) any_producers.push_back(i);
        if (footprint[i].produces_any || !footprint[i].produces.empty()) all_producers.push_back(i);
        for (const Node p : footprint[i].produces)
            producers[p].push_back(i);
    }

    std::vector<std::vector<std::pair<std::size_t, bool>>> edges(n);
    const auto                                             link = [&](const std::size_t s, const std::unordered_set<Node>& preds, const bool any, const bool negative)
    {
        if (any)
        {
            for (const std::size_t r : all_producers)
                edges[r].emplace_back(s, negative);
            return;
        }
        for (const Node p : preds)
        {
            if (const auto it = producers.find(p); it != producers.end())
                for (const std::size_t r : it->second)
                    edges[r].emplace_back(s, negative);
        }
        if (!preds.empty())
            for (const std::size_t r : any_producers)
                edges[r].emplace_back(s, negative);
    };
    for (std::size_t s = 0; s < n; ++s)
    {
        link(s, footprint[s].reads, footprint[s].reads_any, false);
        link(s, footprint[s].negates, footprint[s].negates_any, true);
    }

    // A transparent rule (see footprint) does not acquire an edge of its own
    // for the predicates it extends. Any rule that reads or negates such a
    // predicate already depends on every producer of its facts, and the rule
    // executes during every positive saturation, ensuring its additions are
    // finalized before the commencement of the next level. What it modifies
    // is the set of predicates it extends, and this selection hinges on the
    // facts it reads under a fixed predicate -- (R is transitive) in the
    // illustration. Any rule that generates those facts can therefore reach
    // every rule that reads anything, and is placed before each such rule
    // with that rule's own sign. The same holds when the creator is another
    // transparent rule (`is` itself declared transitive): its facts for that
    // predicate come from producers already included in this list. The rule's
    // incoming edges remain unchanged; they only matter for a fixed predicate
    // its consequence creates, and in that context, they are real.
    std::vector<std::size_t> declaring;
    {
        std::vector<bool> seen(n, false);
        for (std::size_t t = 0; t < n; ++t)
        {
            if (!footprint[t].transparent) continue;
            for (const Node p : footprint[t].reads)
                if (const auto it = producers.find(p); it != producers.end())
                    for (const std::size_t r : it->second)
                        if (!seen[r])
                        {
                            seen[r] = true;
                            declaring.push_back(r);
                        }
        }
    }
    for (const std::size_t r : declaring)
        for (std::size_t s = 0; s < n; ++s)
        {
            if (footprint[s].reads_any || !footprint[s].reads.empty()) edges[r].emplace_back(s, false);
            if (footprint[s].negates_any || !footprint[s].negates.empty()) edges[r].emplace_back(s, true);
        }

    // --- Strongly connected components (Tarjan, iterative) -----------------
    constexpr std::size_t    unvisited = static_cast<std::size_t>(-1);
    std::vector<std::size_t> index(n, unvisited), low(n, 0), component(n, unvisited);
    std::vector<bool>        on_stack(n, false);
    std::vector<std::size_t> stack;
    std::size_t              counter    = 0;
    std::size_t              components = 0;

    for (std::size_t root = 0; root < n; ++root)
    {
        if (index[root] != unvisited) continue;

        // (node, next edge to look at)
        std::vector<std::pair<std::size_t, std::size_t>> work{{root, 0}};
        index[root] = low[root] = counter++;
        stack.push_back(root);
        on_stack[root] = true;

        while (!work.empty())
        {
            auto& [v, next] = work.back();
            if (next < edges[v].size())
            {
                const std::size_t w = edges[v][next++].first;
                if (index[w] == unvisited)
                {
                    index[w] = low[w] = counter++;
                    stack.push_back(w);
                    on_stack[w] = true;
                    work.emplace_back(w, 0);
                }
                else if (on_stack[w])
                    low[v] = std::min(low[v], index[w]);
                continue;
            }

            if (low[v] == index[v])
            {
                std::size_t w;
                do
                {
                    w = stack.back();
                    stack.pop_back();
                    on_stack[w]  = false;
                    component[w] = components;
                } while (w != v);
                ++components;
            }

            const std::size_t finished = v;
            work.pop_back();
            if (!work.empty())
            {
                const std::size_t parent = work.back().first;
                low[parent]              = std::min(low[parent], low[finished]);
            }
        }
    }

    // Tarjan completes a component only once all components it can access have
    // been processed, resulting in a numbering that follows a REVERSE
    // topological order: the inputs of a component are assigned higher numbers
    // than the component itself.
    std::vector<bool>                     has_negation(components, false), negation_inside(components, false);
    std::vector<std::vector<std::size_t>> members(components);
    for (std::size_t i = 0; i < n; ++i)
    {
        members[component[i]].push_back(i);
        if (footprint[i].has_negation) has_negation[component[i]] = true;
    }
    // For each component: the inputs received from other components, and
    // whether any of those inputs is delivered under a negation.
    std::vector<std::vector<std::pair<std::size_t, bool>>> inputs(components);
    for (std::size_t r = 0; r < n; ++r)
        for (const auto& [s, negative] : edges[r])
        {
            if (component[r] == component[s])
            {
                if (negative) negation_inside[component[s]] = true;
                continue;
            }
            inputs[component[s]].emplace_back(component[r], negative);
        }

    // final_after[c]: the level beyond which component c yields no further
    // outputs, or -1 if it contains no negation and relies on no other
    // components -- in such cases, it is considered finalized as soon as the
    // positive rules have saturated, even before any level advancement.
    // level[c], for a component that performs negation: the first level at
    // which it can execute. A negated input must be in its FINAL state before
    // the negation reads it, hence the level is set one step beyond that. A
    // positive input merely needs to be capable of re-engaging with the rule
    // each time it grows, which occurs at the same level: the level is re-run
    // after every pass that produced a new output. When a component negates
    // its own products, every input can access the negation via the
    // component, and each such input is treated as if it were negated.
    std::vector<long>        final_after(components, -1);
    std::vector<std::size_t> level(components, 0);
    for (std::size_t c = components; c-- > 0;)
    {
        long first = 0;
        long done  = -1;
        for (const auto& [from, negative] : inputs[c])
        {
            const bool strict = negative || negation_inside[c];
            first             = std::max(first, final_after[from] + (strict ? 1 : 0));
            done              = std::max(done, final_after[from]);
        }
        if (has_negation[c])
        {
            level[c]       = static_cast<std::size_t>(first);
            final_after[c] = first;
        }
        else
            final_after[c] = done;
    }

    // Numbered densely: a level that no one occupies would incur only the
    // cost of an empty pass.
    std::vector<std::size_t> used;
    for (std::size_t c = 0; c < components; ++c)
        if (has_negation[c]) used.push_back(level[c]);
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());

    NegationLevels result;
    result.rules = rules;
    result.level.assign(n, 0);
    result.negates.assign(n, false);
    for (std::size_t i = 0; i < n; ++i)
        if (footprint[i].has_negation)
        {
            result.negates[i] = true;
            result.level[i]   = static_cast<std::size_t>(std::lower_bound(used.begin(), used.end(), level[component[i]]) - used.begin());
        }
    result.component       = std::move(component);
    result.negation_inside = std::move(negation_inside);
    result.levels          = used.size();
    return result;
}
