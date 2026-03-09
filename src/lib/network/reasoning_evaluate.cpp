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

using namespace zelph::network;

void Reasoning::evaluate(RulePos rule, ReasoningContext& ctx, int depth)
{
    if (logging_active())
    {
        _prof.evaluate_calls.fetch_add(1, std::memory_order_relaxed);
        ReasoningProfiler::atomic_max(_prof.max_reasoning_depth, (uint64_t)depth);
    }

    if (!rule.conditions || rule.index >= rule.conditions->size()) return;

    Node condition = (*rule.conditions)[rule.index]; // Current condition from the sorted vector

    if (should_log(depth))
        log(depth, "evaluate", "Processing condition node: " + format(condition));

    // A Condition can be a Set which is an instance of core.Conjunction.
    // Check: (condition ~ conjunction) ?
    bool is_conjunction = false;

    // Check outgoing relations of 'condition' (Subject -> Relations)
    if (_pImpl->exists(condition))
    {
        for (Node rel : _pImpl->get_right(condition))
        {
            if (parse_relation(rel) == core.IsA)
            {
                adjacency_set targets;
                parse_fact(rel, targets);
                if (targets.count(core.Conjunction))
                {
                    is_conjunction = true;
                    break;
                }
            }
        }
    }

    if (is_conjunction)
    {
        if (logging_active())
            _prof.conjunction_sets.fetch_add(1, std::memory_order_relaxed);

        if (should_log(depth))
            log(depth, "evaluate", "Node " + format(condition) + " identified as Conjunction Set.");

        // It is a Conjunction Set.
        // We need to retrieve its elements. In Zelph topology, elements are subjects of PartOf relations pointing to the set.
        // Fact: Element PartOf Set
        // Topology: Set -> RelationNode (Object connects to Relation)
        // Therefore, we must look in get_right(condition) to find the relations where 'condition' is the object.

        adjacency_set sub_conditions;
        for (Node rel : _pImpl->get_right(condition))
        {
            Node p = parse_relation(rel);
            if (p == core.PartOf)
            {
                adjacency_set objs;
                Node          element = parse_fact(rel, objs); // subject of the PartOf relation (the element)

                // Verify that 'condition' is indeed one of the objects (it should be, since we found 'rel' via get_right(condition))
                if (element && objs.count(condition) == 1)
                {
                    if (should_log(depth))
                        log(depth, "evaluate", "Found element of conjunction: " + format(element) + " (via relation " + format(rel) + ")");
                    sub_conditions.insert(element);
                }
            }
        }

        if (!sub_conditions.empty())
        {
            // Optimization: Sort conditions to evaluate most constrained ones first
            auto sorted_conditions = optimize_order(sub_conditions, *rule.variables, depth);

            // Push next alternative (sibling of current Conjunction-node logic) to stack if applicable
            RulePos next_branch(rule);
            if (++next_branch.index < next_branch.conditions->size())
            {
                ctx.next.push_back(next_branch);
            }

            // Build exclusion set: the conjunction set node and all its
            // element nodes are part of the rule topology and must not
            // be matched as data facts by the Unification engine.
            auto excluded = std::make_shared<std::unordered_set<Node>>();
            excluded->insert(condition); // The conjunction set node
            for (Node elem : sub_conditions)
            {
                excluded->insert(elem); // Each condition pattern node
            }

            if (should_log(depth))
                log(depth, "evaluate", "Entering conjunction " + format(condition) + " with " + std::to_string(sorted_conditions->size()) + " sub-conditions");

            // Recurse into the conjunction
            evaluate(RulePos({condition, sorted_conditions, 0, rule.variables, rule.unequals, excluded}), ctx, depth + 1);
        }
        else if (should_log(depth))
        {
            log(depth, "evaluate", "Conjunction set " + format(condition) + " appears empty or malformed.");
        }
    }
    else
    {
        // Leaf Condition (Atomic Fact)
        if (logging_active())
            _prof.leaf_conditions.fetch_add(1, std::memory_order_relaxed);

        if (should_log(depth))
            log(depth, "evaluate", "Processing leaf condition: " + format(condition));

        bool is_negated = is_negated_condition(condition, depth);

        if (logging_active() && is_negated)
            _prof.negated_conditions.fetch_add(1, std::memory_order_relaxed);

        std::unique_ptr<Unification> u = std::make_unique<Unification>(
            this, condition, rule.node, rule.variables, rule.unequals, is_negated ? nullptr : _pool.get(), depth + 1, _prof);

        // --- Negation Handling ---
        // A condition tagged with `negation` succeeds if and only if
        // no match exists. Variables bound by earlier conditions are
        // used; no new bindings are produced by a negated condition.
        if (is_negated)
        {
            // Lambda: proceed after a successful negation (advance to next
            // condition, pop stacked conjunction branch, or reach terminal).
            auto proceed_with_bindings = [&](std::shared_ptr<Variables> bindings)
            {
                size_t next_index = rule.index + 1;

                if (next_index < rule.conditions->size())
                {
                    RulePos next              = rule;
                    next.variables            = bindings;
                    next.index                = next_index;
                    ReasoningContext ctx_copy = ctx;
                    evaluate(next, ctx_copy, depth + 1);
                }
                else if (!ctx.next.empty())
                {
                    RulePos next   = ctx.next.back();
                    next.variables = bindings;
                    ctx.next.pop_back();
                    ReasoningContext ctx_copy = ctx;
                    evaluate(next, ctx_copy, depth + 1);
                }
                else
                {
                    // Terminal: all conditions satisfied
                    ReasoningContext ctx_copy = ctx;

                    if (!ctx_copy.rule_deductions.empty())
                    {
                        if (should_log(depth))
                            log(depth, "evaluate", "TERMINAL: All conditions satisfied. Calling deduce with rule " + format(rule.node));

                        try
                        {
                            deduce(*bindings, rule.node, depth, ctx_copy);
                        }
                        catch (const contradiction_error& error)
                        {
                            std::lock_guard<std::mutex> lock(_mtx_output);
                            _contradiction = true;
                            ++_total_contradictions;

                            std::string output;
                            string::node_to_string(this, output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());
                            std::string message = "«" + get_formatted_name(core.Contradiction, _lang) + "» ⇐ " + output;

                            if (_print_deductions) out(string::unmark_identifiers(message), true);
                            if (_generate_markdown) _markdown->add("Contradictions", message);
                        }
                    }
                    else if (_prune_mode)
                    {
                        adjacency_set objects;
                        Node          subject = parse_fact(ctx_copy.current_condition, objects, rule.node);
                        subject               = string::get(*bindings, subject, subject);
                        Node relation         = parse_relation(ctx_copy.current_condition);
                        relation              = string::get(*bindings, relation, relation);

                        adjacency_set targets;
                        for (Node obj : objects)
                        {
                            Node iobj = string::get(*bindings, obj, obj);
                            if (iobj && !Zelph::Impl::is_var(iobj)) targets.insert(iobj);
                        }

                        if (subject && relation && !targets.empty()
                            && !Zelph::Impl::is_var(subject) && !Zelph::Impl::is_var(relation))
                        {
                            Answer ans = check_fact(subject, relation, targets);
                            if (ans.is_known() && ans.relation())
                            {
                                _facts_to_prune.insert(ans.relation());
                                if (_prune_nodes_mode)
                                {
                                    if (Zelph::Impl::is_var(parse_fact(ctx_copy.current_condition, objects)))
                                        _nodes_to_prune.insert(subject);
                                    else if (objects.size() == 1)
                                        _nodes_to_prune.insert(*targets.begin());
                                }
                            }
                        }
                    }
                    else
                    {
                        // Normal query output / collection
                        std::lock_guard<std::mutex> lock(_mtx_output);
                        if (_query_results)
                        {
                            _query_results->push_back(bindings);
                        }
                        else
                        {
                            std::string output;
                            string::node_to_string(this, output, _lang, ctx_copy.current_condition, 3, *bindings, rule.node);
                            out("Answer: " + string::unmark_identifiers(output), true);
                        }
                    }
                }
            };

            if (should_log(depth))
            {
                std::string cond_str;
                string::node_to_string(this, cond_str, _lang, condition, 3, *rule.variables, rule.node);
                log(depth, "neg-eval", "Processing negated condition " + cond_str);

                log(depth, "neg-eval", "Current variable bindings:");
                for (const auto& [var, val] : *rule.variables)
                {
                    log(depth, "neg-eval", get_name(var, _lang, true) + " (id=" + std::to_string(var) + ") -> " + get_name(val, _lang, true) + " (id=" + std::to_string(val) + ")");
                }
            }

            // Parse the negated pattern to inspect its subject.
            adjacency_set pattern_objects;
            Node          pattern_subject = parse_fact(condition, pattern_objects, rule.node);

            bool subject_is_unbound = Zelph::Impl::is_var(pattern_subject)
                                   && rule.variables->find(pattern_subject) == rule.variables->end();

            if (!subject_is_unbound)
            {
                // --- Step 1: Try standard Unification ---
                // This handles the common case where all variables are already
                // bound by prior positive conditions.
                std::shared_ptr<Variables> match = u->Next();
                u->wait_for_completion();

                if (match)
                {
                    if (logging_active())
                        _prof.negation_fail.fetch_add(1, std::memory_order_relaxed);

                    if (should_log(depth))
                    {
                        log(depth, "neg-eval", "MATCH FOUND => negation FAILS. Bindings:");
                        for (const auto& [var, val] : *match)
                        {
                            log(depth, "neg-eval", get_name(var, _lang, true) + " (id=" + std::to_string(var) + ") -> " + get_name(val, _lang, true) + " (id=" + std::to_string(val) + ")");
                        }
                    }
                    // Match found => negation fails => prune this branch
                    return;
                }

                if (logging_active())
                    _prof.negation_success.fetch_add(1, std::memory_order_relaxed);

                if (should_log(depth))
                    log(depth, "neg-eval", "NO MATCH => negation SUCCEEDS");

                proceed_with_bindings(rule.variables);
            }
            else
            {
                // --- Step 2: Complementary enumeration ---
                // Subject is unbound. We use complementary enumeration:
                // iterate all facts of this relation, collect unique
                // subjects (= the domain), and for each check whether
                // the full pattern holds. Those where it does NOT hold
                // are successful negation bindings.

                adjacency_set rels = filter(condition, core.IsA, core.RelationTypeCategory);
                if (rels.empty()) { return; }
                Node pattern_rel = *rels.begin();
                if (Zelph::Impl::is_var(pattern_rel))
                    pattern_rel = string::get(*rule.variables, pattern_rel, pattern_rel);
                if (Zelph::Impl::is_var(pattern_rel)) { return; }

                adjacency_set rel_facts;
                if (!_pImpl->snapshot_left_of(pattern_rel, rel_facts)) { return; }

                std::unordered_set<Node> processed_subjects;

                for (Node fn : rel_facts)
                {
                    if (logging_active())
                        _prof.neg_complement_subjects_tested.fetch_add(1, std::memory_order_relaxed);

                    // Skip nodes belonging to the rule's own topology
                    if (rule.excluded && rule.excluded->count(fn)) continue;

                    adjacency_set fact_objs;
                    Node          fact_subj = parse_fact(fn, fact_objs, pattern_rel);
                    if (fact_subj == 0 || Zelph::Impl::is_var(fact_subj)) continue;
                    if (processed_subjects.count(fact_subj)) continue;
                    processed_subjects.insert(fact_subj);

                    // Substitute the unbound subject with this candidate
                    Variables test        = *rule.variables;
                    test[pattern_subject] = fact_subj;

                    // Fully instantiate the pattern with the test bindings
                    std::vector<Node> hist;
                    Node              inst_subj = instantiate_fact(this, pattern_subject, test, depth, hist);

                    adjacency_set inst_objs;
                    bool          resolved = true;
                    for (Node po : pattern_objects)
                    {
                        hist.clear();
                        Node io = instantiate_fact(this, po, test, depth, hist);
                        if (io && !Zelph::Impl::is_var(io))
                            inst_objs.insert(io);
                        else
                        {
                            resolved = false;
                            break;
                        }
                    }

                    if (!resolved || !inst_subj || Zelph::Impl::is_var(inst_subj)) continue;

                    // Check if the positive fact exists in the network
                    Answer ans = check_fact(inst_subj, pattern_rel, inst_objs);
                    if (!ans.is_known())
                    {
                        // The positive pattern does NOT hold for this entity
                        // → negation succeeds with this binding
                        proceed_with_bindings(std::make_shared<Variables>(test));
                    }
                }
            }

            return;
        }

        // Define the processing logic for a single match (extracted to be usable in both serial and parallel loops)
        auto process_match = [&](std::shared_ptr<Variables> match)
        {
            if (should_log(depth))
            {
                std::string bindings_str;
                for (const auto& [k, v] : *match)
                    bindings_str += " " + format(k) + "=" + format(v);
                log(depth, "match", "Candidate bindings:" + bindings_str);
            }

            // Reject matches that bind variables to nodes belonging to
            // the current rule's own topology (conjunction set, condition
            // pattern nodes). Without this check, PartOf facts connecting
            // condition patterns to their conjunction set would be matched
            // by conditions like (A in _Seq), causing spurious deductions.
            if (rule.excluded && !rule.excluded->empty())
            {
                for (const auto& [k, v] : *match)
                {
                    if (rule.excluded->count(v))
                    {
                        if (should_log(depth))
                            log(depth, "match", "REJECTED: binding " + format(k) + "=" + format(v) + " hits excluded node");
                        return;
                    }
                }
            }

            std::shared_ptr<Variables> joined          = join(*rule.variables, *match);
            std::shared_ptr<Variables> joined_unequals = join(*rule.unequals, *u->Unequals());

            if (should_log(1 /* always log this case */) && match->empty() && !rule.variables->empty())
            {
                log(depth, "match", "match is EMPTY, rule.variables has " + std::to_string(rule.variables->size()) + " entries, joined has " + std::to_string(joined->size()) + " entries");
                log(depth, "match", "rule.variables:");
                for (const auto& [k, v] : *rule.variables)
                    log(depth, "    variable", format(k) + " = " + format(v));
                log(depth, "match", "joined:");
                for (const auto& [k, v] : *joined)
                    log(depth, "    variable", format(k) + " = " + format(v));
            }

            if (contradicts(*joined, *joined_unequals))
            {
                if (should_log(depth))
                    log(depth, "match", "REJECTED: contradicts unequal constraints");
                return;
            }

            if (joined->empty())
            {
                if (should_log(depth))
                    log(depth, "match", "REJECTED: joined bindings empty");
                return;
            }

            if (should_log(depth))
            {
                std::string joined_str;
                for (const auto& [k, v] : *joined)
                    joined_str += " " + format(k) + "=" + format(v);
                log(depth, "match", "ACCEPTED joined:" + joined_str);
            }

            // Move to next condition in the sorted vector
            size_t next_index = rule.index + 1;

            if (next_index < rule.conditions->size())
            {
                if (should_log(depth))
                    log(depth, "match", "Advancing to condition " + std::to_string(next_index) + "/" + std::to_string(rule.conditions->size()));

                RulePos next   = rule;
                next.variables = joined;
                next.unequals  = joined_unequals;
                next.index     = next_index;

                ReasoningContext ctx_copy = ctx;
                evaluate(next, ctx_copy, depth + 1);
            }
            else if (!ctx.next.empty())
            {
                if (should_log(depth))
                    log(depth, "match", "Popping stacked branch (" + std::to_string(ctx.next.size()) + " remaining)");

                RulePos next = ctx.next.back();
                ctx.next.pop_back();
                ReasoningContext ctx_copy = ctx;
                evaluate(next, ctx_copy, depth + 1);
            }
            else
            {
                if (should_log(depth))
                    log(depth, "match", "All conditions satisfied -> TERMINAL");

                // Leaf: query or prune
                ReasoningContext ctx_copy = ctx;

                if (!ctx_copy.rule_deductions.empty())
                {
                    if (should_log(depth))
                        log(depth, "evaluate", "TERMINAL: All conditions satisfied. Calling deduce with rule " + format(rule.node));

                    try
                    {
                        deduce(*joined, rule.node, depth, ctx_copy);
                    }
                    catch (const contradiction_error& error)
                    {
                        std::lock_guard<std::mutex> lock(_mtx_output);
                        _contradiction = true;
                        ++_total_contradictions;

                        std::string output;
                        string::node_to_string(this, output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());
                        std::string message = "«" + get_formatted_name(core.Contradiction, _lang) + "» ⇐ " + output;

                        if (_print_deductions)
                        {
                            out(string::unmark_identifiers(message), true);
                        }
                        if (_generate_markdown)
                        {
                            _markdown->add("Contradictions", message);
                        }
                    }
                }
                else if (_prune_mode)
                {
                    adjacency_set objects;
                    Node          subject = parse_fact(ctx_copy.current_condition, objects, rule.node);
                    subject               = string::get(*joined, subject, subject);
                    Node relation         = parse_relation(ctx_copy.current_condition);
                    relation              = string::get(*joined, relation, relation);

                    adjacency_set targets;
                    for (Node obj : objects)
                    {
                        Node iobj = string::get(*joined, obj, obj);
                        if (iobj && !Zelph::Impl::is_var(iobj)) targets.insert(iobj);
                    }

                    if (subject && relation && !targets.empty() && !Zelph::Impl::is_var(subject) && !Zelph::Impl::is_var(relation))
                    {
                        Answer ans = check_fact(subject, relation, targets);
                        if (ans.is_known() && ans.relation())
                        {
                            _facts_to_prune.insert(ans.relation());
                            if (_prune_nodes_mode)
                            {
                                if (Zelph::Impl::is_var(parse_fact(ctx_copy.current_condition, objects)))
                                    _nodes_to_prune.insert(subject);
                                else if (objects.size() == 1)
                                    _nodes_to_prune.insert(*targets.begin());
                            }
                        }
                    }
                }
                else
                {
                    // normal query output / collection
                    std::lock_guard<std::mutex> lock(_mtx_output);
                    if (_query_results)
                    {
                        _query_results->push_back(joined);
                    }
                    else
                    {
                        std::string output;
                        string::node_to_string(this, output, _lang, ctx_copy.current_condition, 3, *joined, rule.node);
                        out("Answer: " + string::unmark_identifiers(output), true);
                    }
                }
            }
        };

        if (u->uses_parallel())
        {
            // In parallel mode, Unification's producers read the graph while scanning.
            // If we process matches immediately (and thus call deduce()), we mutate the graph
            // concurrently with those reads -> data race / undefined behaviour.
            //
            // Therefore: wait for all producers to finish, then drain matches and process
            // them sequentially (serial semantics, but parallel match discovery).

            u->wait_for_completion(); // ensures all producer tasks finished scanning/pushing

            int local_matches = 0;
            while (std::shared_ptr<Variables> match = u->Next())
            {
                ++local_matches;
                process_match(match);
            }

            _total_matches.fetch_add(local_matches, std::memory_order_relaxed);

            if (should_log(1))
            {
                if (local_matches == 0)
                {
                    log(depth, "evaluate", "Leaf condition " + format(condition) + " => 0 matches (vars=" + std::to_string(rule.variables->size()) + ")");
                }
                else if (should_log(depth))
                {
                    log(depth, "evaluate", "Leaf condition " + format(condition) + " => " + std::to_string(local_matches) + " match(es)");
                }
            }
        }
        else
        {
            // Standard serial execution on the main thread
            int local_serial_matches = 0;
            while (std::shared_ptr<Variables> match = u->Next())
            {
                ++_total_matches;
                ++local_serial_matches;
                process_match(match);
            }
            // Ensure cleanup
            u->wait_for_completion();

            if (should_log(1))
            {
                if (local_serial_matches == 0)
                {
                    // Unconditional diagnostic: always log when a leaf yields 0 matches.
                    log(depth, "evaluate", "Leaf condition " + format(condition) + " => 0 matches (vars=" + std::to_string(rule.variables->size()) + ")");
                }
                else if (should_log(depth))
                {
                    log(depth, "evaluate", "Leaf condition " + format(condition) + " => " + std::to_string(local_serial_matches) + " match(es)");
                }
            }
        }
    }
}

bool Reasoning::is_negated_condition(Node condition, int depth)
{
    if (!_pImpl->exists(condition))
    {
        if (should_log(depth))
            log(depth, "neg-check", "condition " + std::to_string(condition) + " does not exist!");
        return false;
    }

    Answer ans    = check_fact(condition, core.IsA, {core.Negation});
    bool   result = ans.is_known();

    // Only log at depth+1 to reduce noise (these are almost always NO)
    // Exception: log at current depth when the result is YES (rare, important)
    if (result && should_log(depth))
        log(depth, "neg-check", "condition=" + format(condition) + " IsA Negation? YES");
    else if (should_log(depth + 1))
        log(depth + 1, "neg-check", "condition=" + format(condition) + " IsA Negation? NO");

    return result;
}
