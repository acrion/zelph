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

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
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
        std::vector<PatternInfo>                  leaf_patterns;    // parallel to leaves: rule-static ctor
                                                                    // decomposition, built once, reused per seed
        std::shared_ptr<std::unordered_set<Node>> excluded;         // rule topology nodes (conjunction set + elements)
        adjacency_set                             deductions;
        bool                                      delta_unsafe{false}; // must be applied classically every iteration
        bool                                      deferred{false};     // contains a negated condition -> stratum 2
        std::size_t                               level{0};            // deferred only: see Reasoning::negation_levels
    };

    std::atomic<bool> default_check{false};
}

void Reasoning::set_default_seminaive_check(const bool on)
{
    default_check = on;
}

bool Reasoning::default_seminaive_check()
{
    return default_check;
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
uint64_t Reasoning::run_fixpoint_seminaive(bool silent, const std::vector<std::pair<Node, Node>>* seed)
{
    _nn_pred           = get_node("nn", "zelph");
    _nn_layers_pred    = get_node("nn-layers", "zelph");
    _closure_pred      = get_node("closure", "zelph");
    _closure_one_plus  = get_node("one-or-more", "zelph");
    _closure_zero_plus = get_node("zero-or-more", "zelph");

    // ------------------------------------------------------------------
    // Phase 0: index all rules (cheap: rule sets are small; rebuilt per run)
    // ------------------------------------------------------------------
    std::vector<IndexedRule> rules;
    // concrete predicate -> (rule index, leaf index) pairs
    std::unordered_map<Node, std::vector<std::pair<size_t, size_t>>> pred_index;
    // leaf conditions with a variable predicate: seeded by every delta fact
    std::vector<std::pair<size_t, size_t>> wildcard_index;
    // The number of negation levels that deferred rules encompass; 0 if
    // no rule performs negation.
    std::size_t levels = 0;
    // Size of the rule set this index was built from. A rule can DERIVE a
    // rule, and one that appears during the run is in no index -- see the
    // rebuild at the delta boundary below.
    size_t indexed_rules = 0;

    const auto build_index = [&]()
    {
        rules.clear();
        pred_index.clear();
        wildcard_index.clear();
        _negating_rules.clear();
        levels        = 0;
        indexed_rules = _pImpl->get_left(core.Causes).size();

        for (Node rule_node : _pImpl->get_left(core.Causes))
        {
            // A rule some other statement merely MENTIONS was never claimed --
            // see Zelph::is_mentioned. The index is built once per run, so this
            // is where the check belongs.
            if (is_mentioned(rule_node)) continue;

            IndexedRule ir;
            ir.rule        = rule_node;
            Node condition = parse_fact(rule_node, ir.deductions);
            if (!condition || condition == core.Causes) continue;
            ir.top_condition = condition;

            const bool is_conjunction = is_condition_set(condition);

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

            // Rules whose condition contains a negation at ANY depth form the
            // deferred stratum: excluded from the classic first pass, never
            // seeded, never applied inside the positive delta loop. They run
            // only at stratum boundaries (positive delta drained), so every
            // negation is tested against the saturated positive fact base.
            // This supersedes the former per-rule negation handling: a rule
            // whose negation variables were all covered by positive conditions
            // used to be seeded like a normal rule, which evaluated its
            // negation against an unsaturated graph and could fire prematurely
            // (negation race, see test_stratified.cpp).
            ir.deferred = condition_contains_negation(condition, 1);
            if (ir.deferred)
            {
                // The function deduce() receives the condition part as its
                // parent -- the conjunction set, for each rule that
                // possesses one -- thereby causing both to lead to the rule.
                _negating_rules[rule_node] = rule_node;
                _negating_rules[condition] = rule_node;
            }

            if (!ir.deferred)
            {
                // Classify elements. A rule is delta-unsafe (and then applied
                // classically in every iteration) when seeding over its
                // positive conditions cannot be proven complete:
                //  - nested conjunction elements (evaluate handles them
                //    recursively; a flat "remaining" reconstruction would lose
                //    that structure)
                //  - elements without a unique predicate
                //  - neural (approx) conditions (no fact lookup, epoch-cached)
                //  - no positive leaf at all
                for (Node cond : ir.elements)
                {
                    if (is_condition_set(cond))
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

                    // A transitive path condition is no fact lookup either, and
                    // it is worse off than the neural one: the facts it depends
                    // on are every edge of the predicate it WALKS, and no
                    // condition of the rule names that predicate. Its own
                    // predicate is `closure`, so seeding waited for a new tag
                    // fact -- which never comes, tag facts being rule
                    // structure. So the rule fired once and was never revisited
                    // when the closure grew underneath it.
                    //
                    // `(A P31 C, C P279⁺ T) => (A below T)` plus
                    // `(X sub Y) => (X P279 Y)`, `a P31 c1`, `c1 sub c2`:
                    // classic derives `(a below c2)`, semi-naive did not, and
                    // semi-naive is the default. `.semi-naive check` named it.
                    if (_closure_pred != 0 && rel == _closure_pred)
                    {
                        ir.delta_unsafe = true;
                        continue;
                    }

                    if (!Zelph::Impl::is_var(rel) && rel == core.Unequal)
                        continue; // guard: never a seed, binds no new variables

                    ir.leaves.push_back(cond);
                    ir.leaf_preds.push_back(Zelph::Impl::is_var(rel) ? Node{0} : rel);
                    // Hoisted out of the Unification constructor: the pattern
                    // decomposition is a pure function of the condition node
                    // (with armed stores: its immutable genuine triple), so one
                    // build here serves every seed of this leaf.
                    ir.leaf_patterns.push_back(build_pattern_info(this, cond, 2));
                }

                if (ir.leaves.empty()) ir.delta_unsafe = true;

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
            }
            rules.push_back(std::move(ir));
        }

        std::vector<Node> rule_nodes;
        rule_nodes.reserve(rules.size());
        for (const IndexedRule& ir : rules)
            rule_nodes.push_back(ir.rule);
        const NegationLevels stratified = negation_levels(rule_nodes);
        levels                          = stratified.levels;
        for (std::size_t i = 0; i < rules.size(); ++i)
            rules[i].level = stratified.level[i];
    };

    build_index();

    // ------------------------------------------------------------------
    // Phase 1: delta capture + classic first iteration
    // ------------------------------------------------------------------
    std::mutex                         delta_mtx;
    std::vector<std::pair<Node, Node>> delta; // (fact node, predicate)

    const bool check = _seminaive_check;
    set_fact_creation_observer([&delta, &delta_mtx, check, this](Node f, Node p)
                               {
        std::lock_guard<std::mutex> lock(delta_mtx);
        delta.emplace_back(f, p);
        if (check) _check_touched.insert(p); });

    // The observer captures locals by reference; make sure it is gone on
    // every exit path (including exceptions).
    struct ObserverGuard
    {
        Zelph* z;
        ~ObserverGuard() { z->set_fact_creation_observer(nullptr); }
    } observer_guard{this};

    int iteration = 1;
    _done         = false;
    if (seed)
    {
        // Incremental entry: the graph is already a fixpoint of these rules,
        // so the classic pass would rediscover nothing and cost a full scan.
        // Start from the facts that arrived since, exactly as if the previous
        // iteration had derived them.
        if (!silent)
            if (progress_due())
                diagnostic_stream() << "--- Reasoning iteration 1 (seeded from " << seed->size()
                                    << " new fact(s)) ---" << std::endl;
        std::lock_guard<std::mutex> lock(delta_mtx);
        delta.insert(delta.end(), seed->begin(), seed->end());
    }
    else
    {
        if (!silent)
            if (progress_due())
                diagnostic_stream() << "--- Reasoning iteration 1 (classic, positive stratum) ---" << std::endl;
        for (const IndexedRule& ir : rules)
            if (!ir.deferred) apply_rule(ir.rule, 0);
        _pool->wait();
    }

    // ------------------------------------------------------------------
    // Helpers for the seeded phase
    // ------------------------------------------------------------------
    auto report_contradiction = [this](const contradiction_error& error)
    {
        this->Reasoning::report_contradiction(error);
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

        Unification u(this, ir.leaf_patterns[leaf_idx], ir.rule, vars, uneq, nullptr, 2, &_prof, seed_fact, seed_pred);

        // Binding the condition to the seed constitutes the identical
        // unification that a scan executes, thus qualifying as a match in
        // the run summary just like a binding retrieved during scanning
        // does in evaluate(): each binding the search returns, even those
        // subsequently rejected by the checks below.
        std::size_t seeded = 0;

        while (std::shared_ptr<Variables> match = u.Next())
        {
            ++seeded;

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
            // Same reading as process_match's reject: an empty binding set
            // is only a non-match where the pattern had a variable to bind.
            if (match->empty() && var_in_closure(cond)) continue;

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
        _total_matches.fetch_add(seeded, std::memory_order_relaxed);
    };

    // ------------------------------------------------------------------
    // Phase 2: seeded iterations until the delta drains
    // ------------------------------------------------------------------
    uint64_t safety_violations = 0;
    // The negation level evaluated at the next stratum boundary. Levels beneath
    // it are definitive: no influence from a higher level or a positive rule
    // can reach them (that is how the levels are established), thus it
    // only ever advances -- except when the rule set or the safety net
    // alters the foundation beneath all of them.
    std::size_t next_level = 0;

    while (true)
    {
        std::vector<std::pair<Node, Node>> current;
        {
            std::lock_guard<std::mutex> lock(delta_mtx);
            current.swap(delta);
        }

        // A construction that returned a rule in force removed what it had
        // built, and the observer announced those facts as they were created
        // (Reasoning::rebuild_rule). A node that no longer exists is not a
        // delta: it seeds nothing, and kept it would keep the loop from
        // finding that the delta has been depleted.
        current.erase(std::remove_if(current.begin(), current.end(), [this](const std::pair<Node, Node>& entry)
                                     { return !_pImpl->exists(entry.first); }),
                      current.end());

        if (current.empty())
        {
            // ---- A rule derived a rule. ----
            // The index was built from the rule set as it stood at the start
            // of the run, so a rule created since is in none of it -- and it
            // has to see the facts that are older than itself, which is
            // exactly what the classic first pass does. Re-index, repeat that
            // pass, and let the observer refill the delta; the seeded loop
            // takes it from there. Rules only accumulate and an identical one
            // is never created twice (rebuild_rule), so this terminates.
            if (_pImpl->get_left(core.Causes).size() != indexed_rules)
            {
                build_index();
                _done = false;
                if (!silent && progress_due())
                    diagnostic_stream() << "--- Rule set grew (a rule derived a rule): classic pass over "
                                        << rules.size() << " rule(s) ---" << std::endl;
                for (const IndexedRule& ir : rules)
                    if (!ir.deferred) apply_rule(ir.rule, 0);
                _pool->wait();
                next_level = 0;
                continue;
            }

            // ---- Stratum boundary: the positive delta has been depleted. ----
            // Deferred rules (negated conditions) are assessed
            // precisely at this point, using the saturated positive fact
            // base, advancing one level of negation at a time. Their
            // outcomes flow into the delta through the observer and re-open
            // the positive stratum; the boundary is then encountered once
            // more, and the same level is processed again (duplicates are
            // filtered out by deduce, just as everywhere else, ensuring
            // termination).
            if (next_level < levels)
            {
                _done = false;
                if (!silent)
                    if (progress_due())
                        diagnostic_stream() << "--- Deferred stratum (negation level " << next_level + 1 << " of " << levels
                                            << ", classic pass) ---" << std::endl;
                for (const IndexedRule& ir : rules)
                    if (ir.deferred && ir.level == next_level) apply_rule(ir.rule, 0);
                _pool->wait();
                // Mirror the classic loop: a deferred pass that derived
                // something may trigger additional deferred
                // derivations after the positive stratum has consumed its
                // consequences (for instance, the identity fallback from
                // symbolic-core, required across two nesting levels within a
                // single term), hence the next boundary must run this level again.
                // A pass that yields no new derivations constitutes the
                // fixpoint of the level, allowing the following one to
                // commence -- facts accumulate exclusively, ensuring
                // termination.
                if (!_done) ++next_level;
                continue; // any new consequences are in the delta now
            }

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

            // Boundary marker, intentionally NOT restricted by `silent`:
            // the REPL automatically executes reasoning with silent=true,
            // suppressing the diagnostic banner above while still
            // outputting deductions. Each deduction situated between this
            // marker and the pass result below is part of the classic
            // verification pass. It appears before the first line the pass
            // outputs, and is absent entirely when the pass produces no
            // output -- which is the standard scenario, and a marker
            // following every run caused the last line of any
            // output to be the marker instead of the actual answer.
            {
                std::lock_guard<std::mutex> lock(_mtx_output);
                _verification_marker_pending = true;
            }

            for (Node rule_node : _pImpl->get_left(core.Causes))
                if (!is_mentioned(rule_node)) apply_rule(rule_node, 0);
            _pool->wait();

            {
                std::lock_guard<std::mutex> lock(_mtx_output);
                _verification_marker_pending = false;
            }

            if (!_done) break; // clean fixpoint confirmed

            ++safety_violations;
            next_level = 0; // violation facts must re-open the deferred strata too

            // Diagnosis aid: name the missed facts explicitly. The observer
            // stayed active during the classic pass, so the delta now holds
            // exactly the facts the seeded phase failed to derive (including
            // inner facts materialized by instantiate_fact). The printed
            // predicate is the pred_index key that would have been seeded.
            {
                std::vector<std::pair<Node, Node>> missed;
                {
                    std::lock_guard<std::mutex> lock(delta_mtx);
                    missed = delta; // copy -- the seeded loop still consumes them
                }
                out_finding("Semi-naive check: classic pass #" + std::to_string(safety_violations)
                                + " derived " + std::to_string(missed.size())
                                + " fact(s) missed by delta seeding:",
                            true);
                for (const auto& [f, p] : missed)
                {
                    std::string rendered;
                    string::node_to_string(this, rendered, _lang, f, 3);
                    out_finding("  [missed, pred=" + get_formatted_name(p, _lang) + "] "
                                    + string::unmark_identifiers(rendered),
                                true);
                }
            }

            if (logging_active())
                _prof.seminaive_safety_extra.fetch_add(1, std::memory_order_relaxed);
            continue; // the extra facts are in the delta now
        }

        ++iteration;
        _done = false;
        if (!silent && progress_due())
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

void Reasoning::print_pending_verification_marker(const bool finding)
{
    if (!_verification_marker_pending) return;
    _verification_marker_pending = false;
    emit(io::OutputChannel::Out, ">>> semi-naive check: classic verification pass <<<", true, finding);
}

std::vector<Reasoning::NegationRecord> Reasoning::recheck_negations()
{
    std::vector<NegationRecord>             lost;
    std::vector<NegationRecord>             kept;
    std::unordered_map<Node, RuleFootprint> footprints;

    for (NegationRecord& record : _negation_records)
    {
        if (!_pImpl->exists(record.fact)) continue; // removed since: nothing left to justify

        auto it = footprints.find(record.rule);
        if (it == footprints.end()) it = footprints.emplace(record.rule, footprint(record.rule)).first;
        const RuleFootprint& f = it->second;

        // Only a fact whose predicate is negated by the rule can make one
        // of its negated patterns hold, thus a record whose predicates have
        // added nothing since the last check is skipped without being read.
        bool touched = _check_touched_all || f.negates_any;
        for (auto p = f.negates.begin(); !touched && p != f.negates.end(); ++p)
            touched = _check_touched.count(*p) != 0;

        // Verified the way the forward pass verifies it, using the bindings
        // under which the rule was triggered: the negation is valid if no
        // fact matches.
        bool refuted = false;
        if (touched && f.checkable)
        {
            for (const Node condition : f.negated_conditions)
            {
                const auto  vars = std::make_shared<Variables>(record.bindings);
                const auto  uneq = std::make_shared<Variables>();
                Unification u(this, condition, record.parent, vars, uneq, nullptr, 1, nullptr);
                if (u.Next())
                {
                    refuted = true;
                    break;
                }
            }
        }

        // Refuted premises do not yet constitute a lost fact: a different
        // rule, or the same rule with alternative bindings, might still
        // derive it. The explain() function queries every rule with the same
        // question that the forward pass would pose.
        if (refuted && explain(record.fact, 1)->status == ProofNode::Status::Unfounded)
            lost.push_back(std::move(record));
        else
            kept.push_back(std::move(record));
    }

    _negation_records.swap(kept);
    _check_touched.clear();
    _check_touched_all = false;
    return lost;
}
