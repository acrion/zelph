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
#include "zelph_impl.hpp"

using namespace zelph::network;

void Reasoning::deduce(const Variables& variables, const Node parent, const int depth, ReasoningContext& ctx)
{
    if (logging_active())
        _prof.deduce_calls.fetch_add(1, std::memory_order_relaxed);

    if (should_log(depth))
    {
        std::string vars_str;
        for (const auto& [k, v] : variables)
            vars_str += " " + format(k) + "=" + format(v);
        log(depth, "deduce", "BEGIN with bindings:" + vars_str);
    }

    // --- Fresh Variable Detection ---
    // Variables that appear in consequences but are not bound by conditions
    // are "fresh variables": each rule firing creates a new node for them.
    // This allows rules to construct new graph topology, enabling general-
    // purpose structural transformations such as arithmetic.

    std::unordered_set<Node> deduction_vars;
    for (const Node deduction : ctx.rule_deductions)
    {
        if (deduction == core.Contradiction) continue;
        std::vector<Node> history;
        collect_variables(this, deduction, deduction_vars, depth, history);
    }

    std::unordered_set<Node> fresh_vars;
    for (Node var : deduction_vars)
    {
        if (variables.find(var) == variables.end())
            fresh_vars.insert(var);
    }

    // --- Termination Check ---
    if (!fresh_vars.empty())
    {
        if (logging_active())
        {
            _prof.fresh_vars_total.fetch_add(fresh_vars.size(), std::memory_order_relaxed);

            if (should_log(depth))
            {
                std::string fresh_str;
                for (Node fv : fresh_vars)
                    fresh_str += " " + format(fv);
                log(depth, "deduce", "Fresh variables:" + fresh_str);
            }
        }

        if (consequences_already_exist(variables, ctx.rule_deductions, parent, depth))
        {
            if (logging_active())
            {
                _prof.termination_guard_checks.fetch_add(1, std::memory_order_relaxed);
                _prof.termination_guard_skips.fetch_add(1, std::memory_order_relaxed);

                if (should_log(depth))
                    log(depth, "deduce", "consequences_already_exist => SKIP (termination guard)");
            }
            return;
        }
        if (logging_active())
        {
            _prof.termination_guard_checks.fetch_add(1, std::memory_order_relaxed);

            if (should_log(depth))
                log(depth, "deduce", "consequences_already_exist => false, proceeding");
        }
    }
    else if (should_log(depth))
    {
        log(depth, "deduce", "No fresh variables");
    }

    // --- Create Fresh Nodes ---
    Variables augmented = variables;
    for (Node var : fresh_vars)
    {
        Node fresh;
        {
            std::lock_guard<std::mutex> lock(_mtx_network);
            fresh = _pImpl->create();
        }
        augmented[var] = fresh;

        if (logging_active())
            _prof.fresh_nodes_created.fetch_add(1, std::memory_order_relaxed);

        if (should_log(depth))
            log(depth, "deduce", "Created fresh node " + std::to_string(fresh) + " for " + format(var));
    }

    // --- Process Deductions ---
    for (const Node deduction : ctx.rule_deductions)
    {
        if (deduction == core.Contradiction)
        {
            throw contradiction_error(ctx.current_condition, augmented, parent);
        }

        adjacency_set relations = filter(deduction, core.IsA, core.RelationTypeCategory);

        if (relations.size() != 1)
        {
            if (should_log(depth))
                log(depth, "deduce", "Deduction " + format(deduction) + " has " + std::to_string(relations.size()) + " relations, skipping");
            continue;
        }

        Node rel = Zelph::Impl::is_var(*relations.begin())
                     ? string::get(augmented, *relations.begin(), Node{0})
                     : *relations.begin();

        if (!rel)
        {
            if (should_log(depth))
                log(depth, "deduce", "Deduction " + format(deduction) + ": relation resolved to null, skipping");
            continue;
        }

        adjacency_set var_targets;
        Node          var_source = parse_fact(deduction, var_targets, parent);

        if (var_targets.empty())
        {
            if (should_log(depth))
                log(depth, "deduce", "Deduction " + format(deduction) + ": no targets found, skipping");
            continue;
        }

        // All instantiation and fact creation happens under one lock to
        // prevent races where parallel threads create the same node.
        Node          source;
        adjacency_set targets;
        Node          d       = 0;
        bool          wrong   = false;
        bool          created = false;

        {
            std::lock_guard<std::mutex> lock_network(_mtx_network);

            // Seed history with the deduction node so that get_preferred_structure()
            // skips it as a parent and does not mistake it for the subject of var_source.
            std::vector<Node> history{deduction};
            source = instantiate_fact(this, var_source, augmented, depth, history);

            if (should_log(depth))
                log(depth, "deduce", "Instantiated source: " + (source ? format(source) : "NULL") + " (from pattern " + format(var_source) + ")");

            if (source)
            {
                bool done = true;
                for (Node var_t : var_targets)
                {
                    history = {deduction};
                    Node t  = instantiate_fact(this, var_t, augmented, depth, history);

                    if (should_log(depth))
                        log(depth, "deduce", "Instantiated target: " + (t ? format(t) : "NULL") + " (from pattern " + format(var_t) + ")");

                    if (t)
                    {
                        targets.insert(t);
                    }
                    else
                    {
                        done = false;
                        break;
                    }
                }

                if (done)
                {
                    Answer answer = check_fact(source, rel, targets);

                    if (should_log(depth))
                    {
                        std::string targets_str;
                        for (Node t : targets)
                            targets_str += " " + format(t);
                        log(depth, "deduce", "check_fact(" + format(source) + ", " + format(rel) + "," + targets_str + ") => " + (answer.is_known() ? (answer.is_wrong() ? "WRONG" : "KNOWN/exists") : "UNKNOWN/new") + (targets.count(rel) ? " [target==rel, skip]" : ""));
                    }

                    if (answer.is_wrong())
                    {
                        if (logging_active())
                            _prof.check_fact_wrong.fetch_add(1, std::memory_order_relaxed);

                        wrong = true;
                    }
                    else if (!answer.is_known() && targets.count(rel) == 0)
                    {
                        if (logging_active())
                            _prof.check_fact_new.fetch_add(1, std::memory_order_relaxed);

                        try
                        {
                            d       = fact(source, rel, targets);
                            created = true;

                            if (logging_active())
                            {
                                _prof.note_rule_created_fact(parent);
                                _prof.facts_created.fetch_add(1, std::memory_order_relaxed);
                                _prof.log_after_deduction(parent, d, depth);
                            }
                        }
                        catch (const std::exception& ex)
                        {
                            if (should_log(depth))
                                log(depth, "deduce", "fact() threw: " + std::string(ex.what()));

                            wrong = true;
                        }
                    }
                    else if (logging_active() && answer.is_known())
                        _prof.check_fact_known.fetch_add(1, std::memory_order_relaxed);
                }
                else if (should_log(depth))
                {
                    log(depth, "deduce", "Target instantiation incomplete, skipping deduction");
                }
            }
        } // _mtx_network released

        if (wrong)
            throw contradiction_error(ctx.current_condition, augmented, parent);

        if (created)
        {
            std::lock_guard<std::mutex> lock(_mtx_output);
            bool                        do_print = _print_deductions;

            if (!do_print && _stop_watch.is_running() && _stop_watch.duration() >= 1000)
            {
                do_print = true;
                _stop_watch.start();
            }
            else if (!do_print)
            {
                ++_skipped;
            }
            else
            {
                _stop_watch.start();
            }

            if (do_print || _generate_markdown)
            {
                size_t skipped_val = _skipped.exchange(0);
                if (skipped_val > 0) diagnostic(L" (skipped " + std::to_wstring(skipped_val) + L" deductions)", true);

                std::wstring input, output;
                string::node_to_wstring(this, input, _lang, ctx.current_condition, 3, augmented, parent);
                string::node_to_wstring(this, output, _lang, d, 3, {}, parent);

                std::wstring message = output + L" ⇐ " + input;

                if (do_print)
                {
                    out(string::unmark_identifiers(message), true);
                }

                if (_generate_markdown)
                {
                    _markdown->add(L"Deductions", message);
                }
            }

            _done = true;

            if (should_log(depth))
                log(depth, "deduce", "CREATED fact " + format(d));
        }
        else if (should_log(depth) && !wrong)
        {
            log(depth, "deduce", "No new fact created (already exists or skipped)");
        }
    }
}

// Checks whether all deduction patterns of a fresh-variable rule are already
// satisfied in the network. Condition-bound variables are substituted; fresh
// variables act as wildcards. Bindings discovered for one deduction carry
// over to subsequent deductions, ensuring consistency across shared fresh
// variables (e.g. the same N in multiple consequences).
//
// Returns true only if ALL deductions already have matching facts.
//
// NOTE: Under concurrent reasoning, a parallel thread may create the same
// consequences between the check and the subsequent creation. This is
// benign: fact() itself is idempotent (check_fact prevents duplicates),
// but orphaned fresh nodes may result. Use .cleanup to remove them.
bool Reasoning::consequences_already_exist(
    const Variables&     condition_bindings,
    const adjacency_set& deductions,
    Node                 parent,
    const int            depth)
{
    Variables working = condition_bindings;

    for (Node deduction : deductions)
    {
        if (deduction == core.Contradiction) continue;

        adjacency_set relations = filter(deduction, core.IsA, core.RelationTypeCategory);
        if (relations.size() != 1) return false;

        Node rel = Zelph::Impl::is_var(*relations.begin())
                     ? string::get(working, *relations.begin(), *relations.begin())
                     : *relations.begin();
        if (Zelph::Impl::is_var(rel)) return false;

        adjacency_set var_targets;
        Node          var_source = parse_fact(deduction, var_targets, parent);

        // Instantiate subject and objects with current working bindings
        std::vector<Node> history;
        Node              source = instantiate_fact(this, var_source, working, depth, history);

        adjacency_set targets;
        for (Node vt : var_targets)
        {
            history = {deduction};
            Node t  = instantiate_fact(this, vt, working, depth, history);
            if (t == 0) return false;
            targets.insert(t);
        }

        if (source == 0 || targets.empty()) return false;

        bool source_is_var = Zelph::Impl::is_var(source);

        // Determine if any target is still a variable (fresh)
        bool has_var_target = false;
        for (Node t : targets)
        {
            if (Zelph::Impl::is_var(t))
            {
                has_var_target = true;
                break;
            }
        }

        if (!source_is_var && !has_var_target)
        {
            // Fully concrete — direct check
            if (!check_fact(source, rel, targets).is_known()) return false;
        }
        else if (!source_is_var && has_var_target)
        {
            // Source concrete, target is fresh wildcard — search from subject side
            // Topology: subject <-> fact_node -> predicate, objects -> fact_node
            Node fresh_target = *targets.begin();
            bool found        = false;

            for (Node fn : _pImpl->get_right(source))
            {
                if (fn == source) continue;
                adjacency_set fn_right = _pImpl->get_right(fn);
                adjacency_set fn_left  = _pImpl->get_left(fn);

                // fact_node must have: rel in right, source in right AND left (bidirectional subject)
                if (fn_right.count(rel) == 0 || fn_right.count(source) == 0 || fn_left.count(source) == 0) continue;

                // Find object: in left but not in right, and not source
                for (Node obj : fn_left)
                {
                    if (obj != source && fn_right.count(obj) == 0)
                    {
                        working[fresh_target] = obj;
                        found                 = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (!found) return false;
        }
        else if (source_is_var && !has_var_target)
        {
            // Target concrete, source is fresh wildcard — search from object side
            // Topology: object -> fact_node, fact_node -> predicate
            Node target = *targets.begin();
            bool found  = false;

            for (Node fn : _pImpl->get_right(target))
            {
                if (fn == target) continue;
                adjacency_set fn_right = _pImpl->get_right(fn);
                adjacency_set fn_left  = _pImpl->get_left(fn);

                // fact_node must have: rel in right
                if (fn_right.count(rel) == 0) continue;

                // target must be object: in left but NOT in right (not bidirectional)
                if (fn_left.count(target) == 0 || fn_right.count(target) != 0) continue;

                // Find subject: in both right and left (bidirectional), not rel, not target
                for (Node subj : fn_right)
                {
                    if (subj != rel && fn_left.count(subj) != 0)
                    {
                        working[source] = subj;
                        found           = true;
                        break;
                    }
                }
                if (found) break;
            }
            if (!found) return false;
        }
        else
        {
            // Both subject and object are fresh — conservative: assume not existing
            return false;
        }
    }

    return true; // All deductions already exist in the network
}
