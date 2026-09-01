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
#include "fact_structure.hpp"
#include "rule_identity.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

#include <string_view>

using namespace zelph::network;

namespace
{
    // The elements of a conjunction set: the subjects of the PartOf facts
    // pointing at it. The same reconstruction Reasoning::evaluate performs
    // when it takes a rule condition apart, and node_to_string when it
    // prints "{...}".
    std::vector<Node> conjunction_members(const Zelph* const z, const Node set_node)
    {
        std::vector<Node> members;
        for (const Node rel : z->get_right(set_node))
        {
            if (z->parse_relation(rel) != z->core.PartOf) continue;
            adjacency_set objs;
            const Node    s = z->parse_fact(rel, objs, 0);
            if (s != 0 && objs.count(set_node) == 1) members.push_back(s);
        }
        return members;
    }
}

bool Reasoning::in_input_focus(const Node node, const int depth_left) const
{
    if (node == 0) return false;
    if (_input_focus.count(node) != 0) return true;
    if (depth_left <= 0 || !Zelph::Impl::is_hash(node)) return false;

    const FactStructure fs = get_preferred_structure(this, node, 3);
    if (fs.subject == 0) return false;

    if (in_input_focus(fs.subject, depth_left - 1)) return true;
    for (const Node o : fs.objects)
        if (in_input_focus(o, depth_left - 1)) return true;

    return false;
}

bool Reasoning::deduction_is_rule(const Node deduction) const
{
    // The edge probe in front is what keeps this off the hot path: `=>` is a
    // neighbour of a fact node only when it is that fact's predicate or its
    // subject, which no ordinary deduction has -- so the exact reading is
    // reconstructed for candidates only.
    return has_right_edge(deduction, core.Causes) && parse_relation(deduction) == core.Causes;
}

Node Reasoning::find_conjunction_set(const std::unordered_set<Node>& members) const
{
    if (members.empty()) return 0;

    // Every set a node belongs to is reachable from it through its PartOf
    // facts, and a rule condition belongs to very few sets -- so one member
    // is enough to enumerate all candidates.
    const Node probe = *members.begin();

    for (const Node rel : get_right(probe))
    {
        if (parse_relation(rel) != core.PartOf) continue;

        adjacency_set objs;
        if (parse_fact(rel, objs, 0) != probe) continue;

        for (const Node candidate : objs)
        {
            if (candidate == probe) continue;
            if (!check_fact(candidate, core.IsA, {core.Conjunction}).is_known()) continue;

            const std::vector<Node> have = conjunction_members(this, candidate);
            if (have.size() != members.size()) continue;

            bool same = true;
            for (const Node m : have)
            {
                if (members.count(m) == 0)
                {
                    same = false;
                    break;
                }
            }
            if (same) return candidate;
        }
    }

    return 0;
}

std::unordered_set<Node> Reasoning::rule_variables(const Node rule, const Node parent, const int depth)
{
    std::unordered_set<Node> vars;

    adjacency_set consequences;
    const Node    condition = parse_fact(rule, consequences, parent);
    if (condition == 0) return vars;

    for (const Node c : consequences)
    {
        std::vector<Node> history;
        collect_variables(this, c, vars, depth, history);
    }

    // A conjunction set node carries no structure a variable walk could
    // follow -- Zelph::set creates it, and its members hang off it as
    // separate PartOf facts. Without this step the variables of a
    // multi-condition rule would be invisible.
    std::vector<Node> pending{condition};
    while (!pending.empty())
    {
        const Node cond = pending.back();
        pending.pop_back();

        if (check_fact(cond, core.IsA, {core.Conjunction}).is_known())
        {
            for (const Node m : conjunction_members(this, cond))
                pending.push_back(m);
            continue;
        }

        std::vector<Node> history;
        collect_variables(this, cond, vars, depth, history);
    }

    return vars;
}

// NOTE on the tag checks in this file: rebuilding a rule REPRODUCES what was
// written, so the `~ conjunction` tag is the right discriminator here and
// Zelph::is_condition_set is not. That helper answers "does the engine read
// this container as a set of conditions", which is deliberately wider -- a
// single untagged member counts -- and applying it here turned a one-element
// container in a generated CONSEQUENCE into a conjunction set.
Node Reasoning::rebuild_condition(const Node pattern, const Variables& variables, const int depth)
{
    // A conjunction nested inside a condition is a set of its own, and
    // evaluate() reads it recursively -- so it has to be rebuilt recursively.
    if (check_fact(pattern, core.IsA, {core.Conjunction}).is_known())
    {
        std::unordered_set<Node> members;
        for (const Node m : conjunction_members(this, pattern))
        {
            const Node inst = rebuild_condition(m, variables, depth);
            if (inst == 0) return 0;
            members.insert(inst);
        }
        if (members.empty()) return 0;

        Node set_node = find_conjunction_set(members);
        if (set_node == 0)
        {
            set_node = set(members);
            fact(set_node, core.IsA, {core.Conjunction});
        }
        return set_node;
    }

    std::vector<Node> history;
    const Node        inst = instantiate_fact(this, pattern, variables, depth, history);
    if (inst == 0) return 0;

    // The negation tag is a fact ABOUT the pattern, not a part of it, so
    // instantiation cannot carry it along -- it has to be restated. When the
    // pattern came back unchanged the tag is already on the node.
    if (inst != pattern && check_fact(pattern, core.IsA, {core.Negation}).is_known())
        fact(inst, core.IsA, {core.Negation});

    return inst;
}

Node Reasoning::rebuild_rule(const Node pattern, const Variables& variables, const int depth, const Node parent, bool& created)
{
    created = false;

    adjacency_set var_consequences;
    const Node    var_condition = parse_fact(pattern, var_consequences, parent);
    if (var_condition == 0 || var_consequences.empty()) return 0;

    adjacency_set consequences;
    for (const Node c : var_consequences)
    {
        std::vector<Node> history;
        const Node        inst = instantiate_fact(this, c, variables, depth, history);
        if (inst == 0) return 0;
        consequences.insert(inst);
    }

    Node condition = 0;

    if (check_fact(var_condition, core.IsA, {core.Conjunction}).is_known())
    {
        std::unordered_set<Node> members;
        for (const Node m : conjunction_members(this, var_condition))
        {
            const Node inst = rebuild_condition(m, variables, depth);
            if (inst == 0) return 0;
            members.insert(inst);
        }
        if (members.empty()) return 0;

        condition = find_conjunction_set(members);
        if (condition == 0)
        {
            condition = set(members);
            fact(condition, core.IsA, {core.Conjunction});
        }
    }
    else
    {
        // A single-condition rule has no set node at all -- its `=>` subject
        // is the condition itself, and that one is hash-consed, so the
        // check_fact below deduplicates it without any help.
        condition = rebuild_condition(var_condition, variables, depth);
        if (condition == 0) return 0;
    }

    // A rule that is already ASSERTED is not news.
    const Answer existing = check_fact(condition, core.Causes, consequences);
    if (existing.is_known())
    {
        if (!is_mentioned(existing.relation())) return existing.relation();

        // It exists, but only as a MENTION -- and a mention was never
        // claimed, so nothing fires (see Zelph::is_mentioned). That is not a
        // corner case here: whenever the outer rule substitutes nothing into
        // the inner one, hash-consing lands the rebuild on the very node the
        // outer rule mentions. It is exactly the shape a switchable rule has,
        //
        //     (K is on) => ((X p Y) => (X q Y))
        //
        // which would otherwise turn the feature on and derive nothing.
        //
        // Claiming it needs a node of its own, and alpha-renaming gives one:
        // each statement names its own variables anyway, so a renamed copy
        // says exactly the same thing while being a statement rather than a
        // reference to one.
        for (const Node candidate : get_left(core.Causes))
        {
            if (candidate == existing.relation()) continue;
            if (is_mentioned(candidate)) continue;
            if (rules_alpha_equivalent(this, candidate, existing.relation())) return candidate;
        }

        Variables renamed = variables;
        for (const Node v : rule_variables(pattern, parent, depth))
        {
            if (renamed.find(v) != renamed.end()) continue;

            const Node fresh = var();
            // The name travels along, or the rule prints as "(?? p ??)".
            // Reusing it is what the parser does too -- every statement
            // creates its own node for the variable it calls X.
            const std::string name = get_name(v, _lang, true);
            if (!name.empty()) set_name(fresh, name, _lang, false);
            renamed[v] = fresh;
        }

        // No variable to rename: a GROUND rule that is asserted and mentioned
        // at once is one node, and the graph cannot tell the two apart. The
        // same corner Zelph::is_mentioned names.
        if (renamed.size() == variables.size()) return existing.relation();

        return rebuild_rule(pattern, renamed, depth, parent, created);
    }

    created = true;
    return fact(condition, core.Causes, consequences);
}

void Reasoning::deduce(const Variables& variables, const Node parent, const int depth, ReasoningContext& ctx, const double confidence)
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

    // A deduction that is itself a RULE is exempt: its variables are
    // quantified by that inner rule, not by the outer one, and turning them
    // into fresh nodes would derive a rule that says nothing -- conditions
    // still carrying the unbound pattern variables, a conclusion over nodes
    // no condition can ever bind. They stay variables; see rebuild_rule.
    std::unordered_set<Node> deduction_vars;
    for (const Node deduction : ctx.rule_deductions)
    {
        if (deduction == core.Contradiction) continue;
        if (deduction_is_rule(deduction)) continue;
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

        if (!rel || Zelph::Impl::is_var(rel))
        {
            if (should_log(depth))
                log(depth, "deduce", "Deduction " + format(deduction) + ": relation resolved to null or to a variable, skipping");
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
        Node          source = 0;
        adjacency_set targets;
        Node          d       = 0;
        bool          wrong   = false;
        bool          created = false;
        std::string   refusal;
        // A derived RULE is not a statement ABOUT anything, so the focus
        // filter has no subject to match it against -- and suppressing the
        // one deduction that changes what the engine will do next is the
        // wrong default. It prints whenever deductions print at all.
        const bool is_rule = rel == core.Causes;

        if (is_rule)
        {
            std::lock_guard<std::mutex> lock_network(_mtx_network);
            d = rebuild_rule(deduction, augmented, depth, parent, created);

            if (should_log(depth))
                log(depth, "deduce", "Derived rule: " + (d ? format(d) : "NULL") + (created ? " (new)" : " (already present)") + " (from pattern " + format(deduction) + ")");
        }
        else
        {
            std::lock_guard<std::mutex> lock_network(_mtx_network);

            // Seed history with the deduction node so that get_preferred_structure()
            // skips it as a parent and does not mistake it for the subject of var_source.
            std::vector<Node> history{deduction};
            source = instantiate_fact(this, var_source, augmented, depth, history);

            if (should_log(depth))
                log(depth, "deduce", "Instantiated source: " + (source ? format(source) : "NULL") + " (from pattern " + format(var_source) + ")");

            // The predicate is a pattern like the rest of the consequence. The
            // substitution above it covers a predicate that IS a variable and
            // nothing else, so a COMPOSITE one kept the rule's own variables:
            // `(X p Y) => (X (Y r s) c)` derived `a (Y r s) c`, a fact carrying
            // a template variable that no query can ever match.
            const Node var_rel = rel;
            history            = {deduction};
            rel                = instantiate_fact(this, rel, augmented, depth, history);

            if (rel != var_rel && should_log(depth))
                log(depth, "deduce", "Instantiated relation: " + (rel ? format(rel) : "NULL") + " (from pattern " + format(var_rel) + ")");

            if (!rel || Zelph::Impl::is_var(rel)) source = 0;

            if (source)
            {
                bool done = true;
                for (Node var_t : var_targets)
                {
                    history = {deduction};
                    // A PartOf object is written INTO, so it stays the very
                    // container the rule names -- that is the accumulator
                    // idiom. Anywhere else a container is a value of this
                    // binding and instantiate_fact rebuilds it.
                    Node t = instantiate_fact(this, var_t, augmented, depth, history, rel != core.PartOf);

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
                    // Ground guard: after instantiation, the deduced fact
                    // must not contain variables at any depth. A residual
                    // variable means a rule variable was bound to a graph
                    // variable (template leak); asserting it would
                    // materialize partially instantiated junk facts.
                    // Defense in depth -- the primary fix is the deep
                    // template reject in extract_bindings.
                    std::unordered_set<Node> residual;
                    std::vector<Node>        ground_history;
                    collect_variables(this, source, residual, depth, ground_history);
                    // The predicate too: it was left out, which is how a
                    // composite predicate carrying a rule variable reached the
                    // graph before the instantiation above was added.
                    ground_history.clear();
                    collect_variables(this, rel, residual, depth, ground_history);
                    for (Node t : targets)
                    {
                        ground_history.clear();
                        collect_variables(this, t, residual, depth, ground_history);
                    }
                    if (!residual.empty())
                    {
                        done = false;
                        if (should_log(depth))
                            log(depth, "deduce", "SKIP: instantiated deduction is not ground (template-leak guard)");
                    }
                }

                if (done)
                {
                    Answer answer = check_fact(source, rel, targets);

                    // The node may exist only because some rule was written
                    // with this very statement as a ground pattern. Deriving
                    // it is a claim, so the mark goes and the deduction
                    // counts as new -- "(A is bad) => (alarm is on)" has to
                    // start answering the moment something IS bad.
                    const bool was_pattern = answer.is_known() && !answer.is_wrong()
                                          && unmark_rule_pattern(answer.relation());

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
                    else if ((was_pattern || !answer.is_known()) && targets.count(rel) == 0)
                    {
                        if (logging_active())
                            _prof.check_fact_new.fetch_add(1, std::memory_order_relaxed);

                        try
                        {
                            // Confidence < 1 (from ≈ conditions) becomes the fact's probability in
                            // the shared weight store. Note: if the fact already exists (known
                            // correct), the existing probability is NOT upgraded or touched.
                            d       = fact(source, rel, targets, confidence);
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

                            // fact() refuses a shape it cannot represent -- a
                            // set constant being extended, a subject that is
                            // also one of several objects. The rule stops here
                            // either way, but reporting it as a bare `!` said
                            // the knowledge base contradicts itself, which is
                            // not what happened and gives the user nothing to
                            // act on. Carry the reason to the report.
                            refusal = ex.what();

                            constexpr std::string_view prefix = "fact(): ";
                            if (refusal.starts_with(prefix)) refusal.erase(0, prefix.size());

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
            throw contradiction_error(ctx.current_condition, augmented, parent, refusal);

        if (created)
        {
            std::lock_guard<std::mutex> lock(_mtx_output);

            // Focus mode: only deductions ABOUT an interactively entered
            // subject are printed. The applied rule is deliberately NOT an
            // anchor: with session-wide accumulation, rule anchors would
            // make focus degenerate to "all" for any interactively entered
            // (or pasted) rule set. A subject the deduction CONSTRUCTED is
            // reached through what it was constructed of -- see
            // in_input_focus.
            const bool focus_reject = _print_deductions && _deduction_filter && !is_rule
                                   && !in_input_focus(source, _focus_subject_depth);

            bool do_print = _print_deductions && !focus_reject;

            if (focus_reject)
            {
                ++_skipped;
            }
            else if (!do_print && _stop_watch.is_running() && _stop_watch.duration() >= 1000)
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

            if (do_print || _export_derivations)
            {
                size_t skipped_val = _skipped.exchange(0);
                if (skipped_val > 0) diagnostic(" (skipped " + std::to_string(skipped_val) + " deductions)", true);

                std::string input, output;
                string::node_to_string(this, input, _lang, ctx.current_condition, 3, augmented, parent);
                string::node_to_string(this, output, _lang, d, 3, {}, parent);

                if (do_print)
                {
                    out(string::unmark_identifiers(output + " ⇐ " + input), true);
                }

                if (_export_derivations)
                {
                    // The export keeps the premises apart, which the printed
                    // line cannot: it renders the condition SET, and a
                    // consumer would have to take the braces back apart.
                    _export->add("deduction", output, render_premises(ctx.current_condition, augmented, parent));
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

        // A rule deduction contributes no fresh variable (deduce() exempts
        // it) and carries its own exact duplicate check in rebuild_rule, so
        // it neither satisfies this guard nor may it block the others.
        if (deduction_is_rule(deduction)) continue;

        adjacency_set relations = filter(deduction, core.IsA, core.RelationTypeCategory);
        if (relations.size() != 1) return false;

        Node rel = Zelph::Impl::is_var(*relations.begin())
                     ? string::get(working, *relations.begin(), *relations.begin())
                     : *relations.begin();
        if (Zelph::Impl::is_var(rel)) return false;

        adjacency_set var_targets;
        Node          var_source = parse_fact(deduction, var_targets, parent);

        // Instantiate subject, predicate and objects with current working
        // bindings -- the predicate for the same reason as in deduce(), and
        // because this check has to ask about the fact deduce() would build.
        std::vector<Node> history;
        Node              source = instantiate_fact(this, var_source, working, depth, history);

        history = {deduction};
        rel     = instantiate_fact(this, rel, working, depth, history);
        if (!rel || Zelph::Impl::is_var(rel)) return false;

        adjacency_set targets;
        for (Node vt : var_targets)
        {
            history = {deduction};
            // Same reading as in deduce(): a PartOf object keeps its identity.
            Node t = instantiate_fact(this, vt, working, depth, history, rel != core.PartOf);
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
