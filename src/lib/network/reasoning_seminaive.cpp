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

#include "contradiction_error.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "unification.hpp"
#include "zelph_impl.hpp"

#include <mutex>
#include <utility>
#include <vector>

using namespace zelph::network;

namespace
{
    // Static evaluation-plan data for one rule, built once per run().
    struct IndexedRule
    {
        Node                                      rule{0};
        Node                                      top_condition{0}; // leaf or conjunction set node
        adjacency_set                             elements;         // conjunction elements (or the single condition)
        std::vector<Node>                         leaves;           // positive, seedable leaf conditions
        std::vector<Node>                         leaf_preds;       // parallel to leaves; 0 = variable predicate
        std::shared_ptr<std::unordered_set<Node>> excluded;         // rule topology nodes (conjunction set + elements)
        adjacency_set                             deductions;
        bool                                      delta_unsafe{false}; // must be applied classically every iteration
    };
}

void Reasoning::set_seminaive(bool on)
{
    _seminaive = on;
}

bool Reasoning::seminaive() const
{
    return _seminaive;
}

void Reasoning::set_seminaive_check(bool on)
{
    _seminaive_check = on;
}

bool Reasoning::seminaive_check() const
{
    return _seminaive_check;
}

// Semi-naive (delta-driven) fixpoint evaluation.
//
// Iteration 1 is a classic pass over the whole graph: it covers user input,
// parser side effects, and any pre-existing facts. From iteration 2 on, the
// direction is inverted: for every fact created in the previous iteration
// (the delta, captured via the fact-creation observer), the rule/condition
// pairs with a matching predicate are looked up in a static index, the
// condition is bound directly against that single fact (seed mode of
// Unification -- no snapshot, no scan), and the REMAINING conditions run
// through the unchanged evaluate() machinery. Duplicate derivations are
// harmless: check_fact and the termination guard reject them exactly as in
// classic mode. Completeness follows from every new fact appearing exactly
// once in a delta; facts created in the SAME delta find each other because
// the seeded evaluation of the later-processed fact scans a graph that
// already contains the earlier one.
uint64_t Reasoning::run_fixpoint_seminaive(bool silent)
{
    _nn_pred        = get_node("nn", "zelph");
    _nn_layers_pred = get_node("nn-layers", "zelph");

    // ------------------------------------------------------------------
    // Phase 0: index all rules (cheap: rule sets are small; rebuilt per run)
    // ------------------------------------------------------------------
    std::vector<IndexedRule> rules;
    // concrete predicate -> (rule index, leaf index) pairs
    std::unordered_map<Node, std::vector<std::pair<size_t, size_t>>> pred_index;
    // leaf conditions with a variable predicate: seeded by every delta fact
    std::vector<std::pair<size_t, size_t>> wildcard_index;

    for (Node rule_node : _pImpl->get_left(core.Causes))
    {
        IndexedRule ir;
        ir.rule        = rule_node;
        Node condition = parse_fact(rule_node, ir.deductions);
        if (!condition || condition == core.Causes) continue;
        ir.top_condition = condition;

        const bool is_conjunction = check_fact(condition, core.IsA, {core.Conjunction}).is_known();

        ir.excluded = std::make_shared<std::unordered_set<Node>>();
        if (is_conjunction)
        {
            // Elements are subjects of PartOf facts pointing to the set --
            // the same traversal Reasoning::evaluate performs.
            for (Node rel : _pImpl->get_right(condition))
            {
                if (parse_relation(rel) != core.PartOf) continue;
                adjacency_set objs;
                Node          element = parse_fact(rel, objs);
                if (element && objs.count(condition) == 1)
                    ir.elements.insert(element);
            }
            ir.excluded->insert(condition);
            for (Node e : ir.elements)
                ir.excluded->insert(e);
        }
        else
        {
            ir.elements.insert(condition);
        }

        if (ir.elements.empty()) continue; // malformed; classic evaluation would not fire either

        // Classify elements. A rule is delta-unsafe (and then applied
        // classically in every iteration) when seeding over its positive
        // conditions cannot be proven complete:
        //  - nested conjunction elements (evaluate handles them recursively;
        //    a flat "remaining" reconstruction would lose that structure)
        //  - elements without a unique predicate
        //  - neural (approx) conditions (no fact lookup, epoch-cached nets)
        //  - a negation whose variables are not all covered by positive
        //    conditions: its complementary enumeration ranges over the
        //    pattern relation's DOMAIN, and a new fact can extend that
        //    domain, making the negation newly succeed without any positive
        //    condition of this rule matching a new fact
        //  - no positive leaf at all
        std::unordered_set<Node> positive_vars;
        std::vector<Node>        negation_elements;

        for (Node cond : ir.elements)
        {
            if (check_fact(cond, core.IsA, {core.Conjunction}).is_known())
            {
                ir.delta_unsafe = true;
                continue;
            }

            adjacency_set rels = filter(cond, core.IsA, core.RelationTypeCategory);
            if (rels.size() != 1)
            {
                ir.delta_unsafe = true;
                continue;
            }
            const Node rel = *rels.begin();

            if (_nn_pred != 0 && rel == _nn_pred)
            {
                ir.delta_unsafe = true;
                continue;
            }

            if (is_negated_condition(cond, 1))
            {
                negation_elements.push_back(cond);
                continue;
            }

            if (!Zelph::Impl::is_var(rel) && rel == core.Unequal)
                continue; // guard: never a seed, binds no new variables

            ir.leaves.push_back(cond);
            ir.leaf_preds.push_back(Zelph::Impl::is_var(rel) ? Node{0} : rel);

            std::vector<Node> history;
            collect_variables(this, cond, positive_vars, 1, history);
        }

        if (ir.leaves.empty()) ir.delta_unsafe = true;

        for (Node neg : negation_elements)
        {
            std::unordered_set<Node> neg_vars;
            std::vector<Node>        history;
            collect_variables(this, neg, neg_vars, 1, history);
            for (Node v : neg_vars)
            {
                if (positive_vars.count(v) == 0)
                {
                    ir.delta_unsafe = true;
                    break;
                }
            }
            if (ir.delta_unsafe) break;
        }

        const size_t rule_idx = rules.size();
        if (!ir.delta_unsafe)
        {
            for (size_t li = 0; li < ir.leaves.size(); ++li)
            {
                if (ir.leaf_preds[li] == 0)
                    wildcard_index.emplace_back(rule_idx, li);
                else
                    pred_index[ir.leaf_preds[li]].emplace_back(rule_idx, li);
            }
        }
        rules.push_back(std::move(ir));
    }

    // ------------------------------------------------------------------
    // Phase 1: delta capture + classic first iteration
    // ------------------------------------------------------------------
    std::mutex                         delta_mtx;
    std::vector<std::pair<Node, Node>> delta; // (fact node, predicate)

    set_fact_creation_observer([&delta, &delta_mtx](Node f, Node p)
                               {
        std::lock_guard<std::mutex> lock(delta_mtx);
        delta.emplace_back(f, p); });

    // The observer captures locals by reference; make sure it is gone on
    // every exit path (including exceptions).
    struct ObserverGuard
    {
        Zelph* z;
        ~ObserverGuard() { z->set_fact_creation_observer(nullptr); }
    } observer_guard{this};

    int iteration = 1;
    _done         = false;
    if (!silent)
        diagnostic_stream() << "--- Reasoning iteration 1 (classic) ---" << std::endl;
    for (Node rule_node : _pImpl->get_left(core.Causes))
        apply_rule(rule_node, 0);
    _pool->wait();

    // ------------------------------------------------------------------
    // Helpers for the seeded phase
    // ------------------------------------------------------------------
    auto report_contradiction = [&](const contradiction_error& error)
    {
        std::lock_guard<std::mutex> lock(_mtx_output);
        _contradiction = true;
        ++_total_contradictions;

        if (_print_deductions || _generate_markdown)
        {
            std::string output;
            string::node_to_string(this, output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());
            std::string message = "«" + get_formatted_name(core.Contradiction, _lang) + "» ⇐ " + output;

            if (_print_deductions) out(string::unmark_identifiers(message), true);
            if (_generate_markdown) _markdown->add("Contradictions", message);
        }
    };

    auto seed_rule = [&](const IndexedRule& ir, size_t leaf_idx, Node seed_fact, Node seed_pred)
    {
        const Node cond = ir.leaves[leaf_idx];

        if (logging_active())
            _prof.seminaive_seeds.fetch_add(1, std::memory_order_relaxed);

        ReasoningContext ctx;
        // current_condition stays the TOP condition so that deduce() renders
        // the same "consequence <= {conditions}" explanation as classic mode.
        ctx.current_condition = ir.top_condition;
        ctx.rule_deductions   = ir.deductions;

        auto vars = std::make_shared<Variables>();
        auto uneq = std::make_shared<Variables>();

        Unification u(this, cond, ir.rule, vars, uneq, nullptr, 2, _prof, seed_fact, seed_pred);

        while (std::shared_ptr<Variables> match = u.Next())
        {
            // Mirror the checks of evaluate()'s process_match so that a
            // seeded first condition behaves exactly like a scanned one.
            bool excluded_hit = false;
            for (const auto& [k, v] : *match)
            {
                (void)k;
                if (ir.excluded->count(v))
                {
                    excluded_hit = true;
                    break;
                }
            }
            if (excluded_hit) continue;

            if (contradicts(*match, *u.Unequals())) continue;
            if (match->empty()) continue; // consistent with process_match's empty-join reject

            adjacency_set remaining;
            for (Node e : ir.elements)
                if (e != cond) remaining.insert(e);

            if (remaining.empty())
            {
                ReasoningContext ctx_copy = ctx;
                try
                {
                    deduce(*match, ir.rule, 1, ctx_copy, 1.0);
                }
                catch (const contradiction_error& error)
                {
                    report_contradiction(error);
                }
            }
            else
            {
                // The seed bindings are REAL bindings, so optimize_order's
                // scoring (including bound-pattern grounding) applies with
                // full knowledge; most remaining arithmetic conditions
                // become direct O(1) anchors.
                auto sorted = optimize_order(remaining, *match, 1);

                RulePos          pos({ir.rule, sorted, 0, match, u.Unequals(), ir.excluded});
                ReasoningContext ctx_copy = ctx;
                try
                {
                    evaluate(pos, ctx_copy, 1);
                }
                catch (const contradiction_error& error)
                {
                    report_contradiction(error);
                }
            }
        }
        u.wait_for_completion();
    };

    // ------------------------------------------------------------------
    // Phase 2: seeded iterations until the delta drains
    // ------------------------------------------------------------------
    uint64_t safety_violations = 0;

    while (true)
    {
        std::vector<std::pair<Node, Node>> current;
        {
            std::lock_guard<std::mutex> lock(delta_mtx);
            current.swap(delta);
        }

        if (current.empty())
        {
            if (!_seminaive_check) break;

            // Safety net (test/debug mode): verify the fixpoint with one
            // classic pass. Any fact it creates is a completeness violation
            // of delta seeding -- count it, then keep draining until a
            // classic pass confirms quiescence, so the final graph is
            // complete either way. The caller turns a non-zero count into
            // a hard error AFTER the run finishes.
            _done = false;
            if (!silent)
                diagnostic_stream() << "--- Semi-naive safety check (classic pass) ---" << std::endl;
            for (Node rule_node : _pImpl->get_left(core.Causes))
                apply_rule(rule_node, 0);
            _pool->wait();

            if (!_done) break; // clean fixpoint confirmed

            ++safety_violations;
            if (logging_active())
                _prof.seminaive_safety_extra.fetch_add(1, std::memory_order_relaxed);
            continue; // the extra facts are in the delta now
        }

        ++iteration;
        _done = false;
        if (!silent)
            diagnostic_stream() << "--- Reasoning iteration " << iteration
                                << " (semi-naive, delta=" << current.size() << ") ---" << std::endl;

        // Delta-unsafe rules cannot be seeded completely; apply them
        // classically once per iteration. This set is empty for typical
        // rule bases, including the arithmetic modules.
        for (const IndexedRule& ir : rules)
        {
            if (ir.delta_unsafe) apply_rule(ir.rule, 0);
        }
        _pool->wait();

        for (const auto& [seed_fact, seed_pred] : current)
        {
            // Classic scans exclude variable relations (get_sources with
            // exclude_vars=true); mirror that here.
            if (Zelph::Impl::is_var(seed_pred)) continue;

            auto it = pred_index.find(seed_pred);
            if (it != pred_index.end())
            {
                for (const auto& [ri, li] : it->second)
                    seed_rule(rules[ri], li, seed_fact, seed_pred);
            }
            for (const auto& [ri, li] : wildcard_index)
                seed_rule(rules[ri], li, seed_fact, seed_pred);
        }
        _pool->wait();
    }

    _done = false;
    return safety_violations;
}
