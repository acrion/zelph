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
#include "unification.hpp"
#include "zelph_impl.hpp"

#include <cassert>
#include <iostream> // For std::clog
#include <vector>

using namespace zelph::network;

struct FactStructure
{
    Node                     subject{};
    Node                     predicate{};
    std::unordered_set<Node> objects;
};

// Find the right structure for instantiation. Use history to exclude parent nodes (cycles).
static FactStructure get_preferred_structure(Zelph* n, Node fact, const std::vector<Node>& history)
{
    FactStructure preferred;
    preferred.subject = 0;

    std::vector<FactStructure> structures;

    if (fact == 0 || !n->exists(fact)) return preferred;

    adjacency_set right = n->get_right(fact);
    adjacency_set left  = n->get_left(fact);

    adjacency_set predicates;
    for (Node p : right)
    {
        if (n->check_fact(p, n->core.IsA, {n->core.RelationTypeCategory}).is_known())
            predicates.insert(p);
    }

    if (predicates.empty()) return preferred;

    for (Node p : predicates)
    {
        for (Node s : right)
        {
            if (s == p || left.count(s) == 0) continue;

            // If `s` (the candidate for the subject) is already in the history,
            // `s` is a parent node that is currently instantiating `fact`.
            // We must not return to it.
            bool is_parent = false;
            for (Node visited : history)
            {
                if (visited == s)
                {
                    is_parent = true;
                    break;
                }
            }
            if (is_parent) continue;

            FactStructure fs;
            fs.subject   = s;
            fs.predicate = p;

            for (Node o : left)
            {
                if (o != s && o != p && right.count(o) == 0)
                {
                    fs.objects.insert(o);
                }
            }

            if (!fs.objects.empty()) structures.push_back(fs);
        }
    }

    if (!structures.empty())
    {
        preferred = structures[0];
        for (const auto& fs : structures)
        {
            if (!Zelph::Impl::is_hash(fs.subject))
            {
                // Prefer non-hash subjects (atoms/variables)
                preferred = fs;
                break;
            }
        }
    }

    return preferred;
}

static Node instantiate_fact(Zelph* z, Node pattern, const Variables& variables, std::vector<Node>& history)
{
    // 1. Variable substitution
    if (Zelph::Impl::is_var(pattern))
    {
        return zelph::string::get(variables, pattern, pattern);
    }

    // 2. Cycle Check (Safety net)
    for (Node visited : history)
    {
        if (visited == pattern) return pattern; // Should be caught by get_preferred_structure, but safe is safe
    }
    history.push_back(pattern);

    // 3. Structural recursion
    FactStructure fs = get_preferred_structure(z, pattern, history);

    if (fs.subject == 0)
    {
        history.pop_back();
        return pattern; // Atomic / No structure found
    }

    Node inst_subject  = instantiate_fact(z, fs.subject, variables, history);
    Node inst_relation = instantiate_fact(z, fs.predicate, variables, history);

    adjacency_set inst_objects;
    bool          changed = (inst_subject != fs.subject) || (inst_relation != fs.predicate);

    for (Node o : fs.objects)
    {
        Node io = instantiate_fact(z, o, variables, history);
        inst_objects.insert(io);
        if (io != o) changed = true;
    }

    history.pop_back();

    if (!changed)
    {
        return pattern;
    }

    return z->fact(inst_subject, inst_relation, inst_objects);
}

Reasoning::Reasoning(const std::function<void(const std::wstring&, const bool)>& print)
    : Zelph(print)
    , _pool(std::make_unique<ThreadPool>(std::thread::hardware_concurrency()))
{
}

void Reasoning::set_markdown_subdir(const std::string& subdir)
{
    _markdown_subdir = subdir;
}

void Reasoning::prune_facts(Node pattern, size_t& removed_count)
{
    _prune_mode       = true;
    _prune_nodes_mode = false;
    _facts_to_prune.clear();
    _nodes_to_prune.clear();

    apply_rule(0, pattern);

    _pool->wait();

    removed_count = _facts_to_prune.size();

    if (removed_count > 0)
    {
        std::lock_guard<std::mutex> lock(_mtx_network);
        for (Node fact : _facts_to_prune)
        {
            _pImpl->remove(fact);
        }
    }

    _prune_mode = false;
}

void Reasoning::prune_nodes(Node pattern, size_t& removed_facts, size_t& removed_nodes)
{
    _prune_mode       = true;
    _prune_nodes_mode = true;
    _facts_to_prune.clear();
    _nodes_to_prune.clear();

    apply_rule(0, pattern);

    _pool->wait();

    removed_facts = _facts_to_prune.size();
    removed_nodes = _nodes_to_prune.size();

    std::lock_guard<std::mutex> lock(_mtx_network);

    for (Node fact : _facts_to_prune)
    {
        _pImpl->remove(fact);
    }

    for (Node node : _nodes_to_prune)
    {
        _pImpl->remove(node);
    }

    _prune_mode       = false;
    _prune_nodes_mode = false;
}

void Reasoning::purge_unused_predicates(size_t& removed_facts, size_t& removed_predicates)
{
    removed_facts      = 0;
    removed_predicates = 0;

    std::vector<Node> all_predicates;

    {
        std::lock_guard<std::mutex> lock(_mtx_network);

        adjacency_set def_facts = _pImpl->get_right(core.RelationTypeCategory);

        for (Node def_fact : def_facts)
        {
            if (_pImpl->get_right(def_fact).count(core.IsA) == 1)
            {
                for (Node cand : _pImpl->get_left(def_fact))
                {
                    if (cand != core.RelationTypeCategory && cand != core.IsA)
                    {
                        if (_pImpl->get_right(cand).count(def_fact) == 1)
                        {
                            all_predicates.push_back(cand);
                        }
                    }
                }
            }
        }
    }

    auto is_protected = [&](Node n)
    {
        return n == core.IsA || n == core.Causes || n == core.RelationTypeCategory || n == core.Unequal || n == core.Contradiction || n == core.FollowedBy || n == core.PartOf || n == core.Conjunction;
    };

    std::clog << "Found " << all_predicates.size() << " predicates. Starting deep scan..." << std::endl;

    std::lock_guard<std::mutex> lock(_mtx_network);

    for (size_t i = 0; i < all_predicates.size(); ++i)
    {
        Node pred = all_predicates[i];

        if (is_protected(pred)) continue;

        adjacency_set incoming_to_pred = _pImpl->get_left(pred);

        if (incoming_to_pred.size() > 200000)
        {
            std::wstring name = get_name(pred, "wikidata", true);
            std::clog << "[" << (i + 1) << "/" << all_predicates.size() << "] Checking "
                      << string::unicode::to_utf8(name) << " (" << pred << ") with "
                      << incoming_to_pred.size() << " entries..." << std::endl;
        }

        size_t valid_usage_count = 0;
        size_t local_removed     = 0;

        for (Node fact : incoming_to_pred)
        {
            if (_pImpl->get_right(fact).count(core.IsA) == 1)
            {
                continue;
            }

            bool is_zombie = false;

            adjacency_set incoming_to_fact = _pImpl->get_left(fact);
            if (incoming_to_fact.empty())
            {
                is_zombie = true;
            }
            else
            {
                adjacency_set outgoing_from_fact = _pImpl->get_right(fact);

                bool has_subject = false;
                bool has_object  = false;

                for (Node out_node : outgoing_from_fact)
                {
                    if (out_node == pred) continue;

                    if (incoming_to_fact.count(out_node) == 1)
                    {
                        has_subject = true;
                        break;
                    }
                }

                if (has_subject)
                {
                    for (Node in_node : incoming_to_fact)
                    {
                        if (outgoing_from_fact.count(in_node) == 0)
                        {
                            has_object = true;
                            break;
                        }
                    }
                }

                if (!has_subject || !has_object)
                {
                    is_zombie = true;
                }
            }

            if (is_zombie)
            {
                _pImpl->remove(fact);
                local_removed++;
            }
            else
            {
                valid_usage_count++;
            }
        }

        removed_facts += local_removed;

        if (local_removed > 0 && incoming_to_pred.size() > 200000)
        {
            std::clog << "   -> Purged " << local_removed << " broken facts." << std::endl;
        }

        if (valid_usage_count == 0)
        {
            _pImpl->remove(pred);
            removed_predicates++;
        }
    }
}

void Reasoning::run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition, const bool silent)
{
    StopWatch watch;
    watch.start();

    _print_deductions     = print_deductions;
    _generate_markdown    = generate_markdown;
    _skipped              = 0;
    _contradiction        = false;
    _total_matches        = 0;
    _total_contradictions = 0;

    if (_generate_markdown)
    {
        if (_markdown_subdir.empty())
        {
            throw std::runtime_error("Markdown subdirectory not set for .run-md command");
        }
        _markdown = std::make_unique<wikidata::Markdown>(std::filesystem::path("mkdocs") / "docs" / _markdown_subdir, this);
    }

    if (!silent)
        std::clog << "Starting reasoning with " << _pool->count() << " worker threads." << std::endl;

    do
    {
        _done = false;
        for (Node rule : _pImpl->get_left(core.Causes))
        {
            apply_rule(rule, 0);
        }

        _pool->wait();
    } while (_done && !suppress_repetition);

    if (!silent)
        std::clog << "Reasoning complete. Total unification matches processed: " << _total_matches
                  << ". Total contradictions found: " << _total_contradictions << "." << std::endl;

    if (_skipped > 0) print(L" (skipped " + std::to_wstring(_skipped) + L" deductions)", true);

    if (_contradiction)
    {
        print(L"Found one or more contradictions!", true);
    }

    if (_done && suppress_repetition)
    {
        print(L"Warning: Additional reasoning iterations are required, but have been suppressed.", true);
    }

    if (!silent)
        std::clog << "Reasoning summary: " << _total_matches << " matches processed, "
                  << _total_contradictions << " contradictions found." << std::endl;
    static std::unordered_set<Node> logged_relations;

    if (_pool && !silent)
    {
        std::clog << "Parallel unifications activated for " << logged_relations.size()
                  << " distinct fixed relations." << std::endl;
    }

    logged_relations.clear();

    watch.stop();

    if (!silent)
        std::clog << "Reasoning complete in " << watch.format() << " – "
                  << _total_matches << " matches processed, "
                  << _total_contradictions << " contradictions found." << std::endl;
}

void Reasoning::apply_rule(const Node& rule, Node condition)
{
    ReasoningContext ctx;

    if (rule == 0)
    {
        assert(condition != 0);
    }
    else
    {
        condition = parse_fact(rule, ctx.rule_deductions);
    }

    if (condition && condition != core.Causes)
    {
        ctx.current_condition = condition;
        ctx.next.clear();

        // Create initial vector with single condition
        auto conditions = std::make_shared<std::vector<Node>>();
        conditions->push_back(condition);

        try
        {
            evaluate(RulePos({rule, conditions, 0}), ctx);
        }
        catch (const contradiction_error& error)
        {
            std::lock_guard<std::mutex> lock(_mtx_output);
            _contradiction = true;
            ++_total_contradictions;

            if (_print_deductions || _generate_markdown)
            {
                std::wstring output;
                format_fact(output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());
                std::wstring message = L"«" + get_formatted_name(core.Contradiction, _lang) + L"» ⇐ " + output;

                if (_print_deductions)
                {
                    print(message, true);
                }

                if (_generate_markdown)
                {
                    _markdown->add(L"Contradictions", message);
                }
            }
        }

        _pool->wait();
    }
}

// Greedy Sort to optimize execution order based on variable bindings
std::shared_ptr<std::vector<Node>> Reasoning::optimize_order(const adjacency_set& conditions, const Variables& current_vars)
{
    auto sorted = std::make_shared<std::vector<Node>>();
    sorted->reserve(conditions.size());

    // Copy to a temporary list we can destructively consume
    std::vector<Node> pending(conditions.begin(), conditions.end());
    Variables         simulated_vars = current_vars;

    while (!pending.empty())
    {
        auto best_it   = pending.end();
        int  max_score = -999999;

        for (auto it = pending.begin(); it != pending.end(); ++it)
        {
            Node          cond  = *it;
            int           score = 0;
            adjacency_set objects;
            Node          subject = parse_fact(cond, objects); // Relation is ignored for scoring for now, could be added

            bool s_is_var = Zelph::Impl::is_var(subject);
            bool s_bound  = s_is_var && simulated_vars.count(subject);

            if (!s_is_var || s_bound)
                score += 100; // Subject is constant or bound variable (Great!)
            else
                score -= 10; // Subject is unbound variable (Bad)

            // Heuristic for objects (simplified, assumes 1 object usually)
            for (Node obj : objects)
            {
                bool o_is_var = Zelph::Impl::is_var(obj);
                bool o_bound  = o_is_var && simulated_vars.count(obj);
                if (!o_is_var || o_bound)
                    score += 50; // Object is constant or bound variable (Good)
                else
                    score -= 10; // Object is unbound variable (Bad)
            }

            if (score > max_score)
            {
                max_score = score;
                best_it   = it;
            }
        }

        if (best_it != pending.end())
        {
            Node best_cond = *best_it;
            sorted->push_back(best_cond);

            // "Bind" variables for next iteration
            adjacency_set objects;
            Node          subject = parse_fact(best_cond, objects);
            if (Zelph::Impl::is_var(subject)) simulated_vars[subject] = 1; // Dummy bind
            for (Node obj : objects)
            {
                if (Zelph::Impl::is_var(obj)) simulated_vars[obj] = 1; // Dummy bind
            }

            pending.erase(best_it);
        }
        else
        {
            // Should not happen unless empty
            break;
        }
    }
    return sorted;
}

void Reasoning::evaluate(RulePos rule, ReasoningContext& ctx)
{
    if (!rule.conditions || rule.index >= rule.conditions->size()) return;

    Node condition = (*rule.conditions)[rule.index]; // Current condition from the sorted vector

#ifdef _DEBUG
    std::clog << "[DEBUG evaluate] Processing condition node: " << condition << std::endl;
#endif

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
#ifdef _DEBUG
        std::clog << "[DEBUG evaluate] Node " << condition << " identified as Conjunction Set." << std::endl;
#endif
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
#ifdef _DEBUG
                    std::clog << "[DEBUG evaluate] Found element of conjunction: " << element << " (via relation " << rel << ")" << std::endl;
#endif
                    sub_conditions.insert(element);
                }
            }
        }

        if (!sub_conditions.empty())
        {
            // Optimization: Sort conditions to evaluate most constrained ones first
            auto sorted_conditions = optimize_order(sub_conditions, *rule.variables);

            // Push next alternative (sibling of current Conjunction-node logic) to stack if applicable
            RulePos next_branch(rule);
            if (++next_branch.index < next_branch.conditions->size())
            {
                ctx.next.push_back(next_branch);
            }

            // Recurse into the conjunction
            evaluate(RulePos({condition, sorted_conditions, 0, rule.variables, rule.unequals}), ctx);
        }
        else
        {
#ifdef _DEBUG
            std::clog << "[DEBUG evaluate] Conjunction set " << condition << " appears empty or malformed." << std::endl;
#endif
        }
    }
    else
    {
        // Leaf Condition (Atomic Fact)
#ifdef _DEBUG
        std::clog << "[DEBUG evaluate] Processing leaf condition: " << condition << std::endl;
#endif
        std::unique_ptr<Unification> u = std::make_unique<Unification>(
            this, condition, rule.node, rule.variables, rule.unequals, _pool.get());

        // Define the processing logic for a single match (extracted to be usable in both serial and parallel loops)
        auto process_match = [&](std::shared_ptr<Variables> match)
        {
            std::shared_ptr<Variables> joined          = join(*rule.variables, *match);
            std::shared_ptr<Variables> joined_unequals = join(*rule.unequals, *u->Unequals());

            if (contradicts(*joined, *joined_unequals))
            {
                return;
            }

            if (joined->empty())
            {
                return;
            }

            // Move to next condition in the sorted vector
            size_t next_index = rule.index + 1;

            if (next_index < rule.conditions->size())
            {
                RulePos next   = rule;
                next.variables = joined;
                next.unequals  = joined_unequals;
                next.index     = next_index;

                ReasoningContext ctx_copy = ctx;
                evaluate(next, ctx_copy);
            }
            else if (!ctx.next.empty())
            {
                RulePos next = ctx.next.back();
                ctx.next.pop_back();
                ReasoningContext ctx_copy = ctx;
                evaluate(next, ctx_copy);
            }
            else
            {
                // Leaf: query or prune
                ReasoningContext ctx_copy = ctx;

                if (!ctx_copy.rule_deductions.empty())
                {
                    try
                    {
                        deduce(*joined, rule.node, ctx_copy);
                    }
                    catch (const contradiction_error& error)
                    {
                        std::lock_guard<std::mutex> lock(_mtx_output);
                        _contradiction = true;
                        ++_total_contradictions;

                        std::wstring output;
                        format_fact(output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());
                        std::wstring message = L"«" + get_formatted_name(core.Contradiction, _lang) + L"» ⇐ " + output;

                        if (_print_deductions)
                        {
                            print(message, true);
                        }
                        if (_generate_markdown)
                        {
                            _markdown->add(L"Contradictions", message);
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
                    // normal query output
                    std::lock_guard<std::mutex> lock(_mtx_output);
                    std::wstring                output;
                    format_fact(output, _lang, ctx_copy.current_condition, 3, *joined, rule.node);
                    print(L"Answer: " + output, true);
                }
            }
        };

        if (u->uses_parallel())
        {
            // The unification object has already started its own producer tasks in the pool.
            // We now launch consumer tasks in the same pool. They will execute concurrently with producers.

            size_t num_threads = _pool->count();
            for (size_t i = 0; i < num_threads; ++i)
            {
                _pool->enqueue([&, u_ptr = u.get()]()
                               {
                    int local_matches = 0;
                    while (std::shared_ptr<Variables> match = u_ptr->Next())
                    {
                        local_matches++;
                        process_match(match);
                    }
                    _total_matches += local_matches; });
            }

            // Wait for all consumers (and consequently all producers) to finish
            _pool->wait();
        }
        else
        {
            // Standard serial execution on the main thread
            while (std::shared_ptr<Variables> match = u->Next())
            {
                ++_total_matches;
                process_match(match);
            }
            // Ensure cleanup
            u->wait_for_completion();
        }
    }
}

bool Reasoning::contradicts(const Variables& variables, const Variables& unequals)
{
    for (const auto& var : unequals)
    {
        Node item1 = var.first;
        if (Zelph::Impl::is_var(item1))
        {
            auto it = variables.find(item1);
            if (it != variables.end())
                item1 = it->second;
            else
                continue; // though this variable is included in the unequals, it is missing in the normal variables (meaningless unequal condition)
        }

        Node item2 = var.second;
        if (Zelph::Impl::is_var(item2))
        {
            auto it = variables.find(item2);
            if (it != variables.end())
                item2 = it->second;
            else
                continue; // though this variable is included in the unequals, it is missing in the normal variables (meaningless unequal condition)
        }

        if (item1 == item2)
            return true; // contradiction, because item1 and item2 must be unequal
    }

    return false; // no contradiction
}

void Reasoning::deduce(const Variables& variables, const Node parent, ReasoningContext& ctx)
{
    for (const Node deduction : ctx.rule_deductions)
    {
        if (deduction == core.Contradiction)
        {
            throw contradiction_error(ctx.current_condition, variables, parent);
        }

        adjacency_set relations = filter(deduction, core.IsA, core.RelationTypeCategory);

        if (relations.size() == 1) // more than one relation for given condition makes no sense. _relation_list is empty, so Next() won't return anything
        {
            Node rel = Zelph::Impl::is_var(*relations.begin())
                         ? string::get(variables, *relations.begin(), 0ull)
                         : *relations.begin();

            if (rel)
            {
                adjacency_set var_targets;
                Node          var_source = parse_fact(deduction, var_targets, parent);

                if (!var_targets.empty())
                {
                    std::vector<Node> history;
                    const Node        source = instantiate_fact(this, var_source, variables, history);

                    if (source)
                    {
                        adjacency_set targets;
                        bool          done = true;
                        for (Node var_t : var_targets)
                        {
                            history.clear();
                            Node t = instantiate_fact(this, var_t, variables, history);

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

                            if (answer.is_wrong())
                            {
                                throw contradiction_error(ctx.current_condition, variables, parent);
                            }
                            else if (!answer.is_known()
                                     && targets.count(rel) == 0     // ignore deductions with same relation and target type as we do not support these
                                     && targets.count(source) == 0) // ignore deductions with the same subject and object as we do not support these
                            {
                                try
                                {
                                    Node d;
                                    {
                                        std::lock_guard<std::mutex> lock_network(_mtx_network);
                                        d = fact(source, rel, targets);
                                    }

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
                                            if (skipped_val > 0) print(L" (skipped " + std::to_wstring(skipped_val) + L" deductions)", true);

                                            std::wstring input, output;
                                            format_fact(input, _lang, ctx.current_condition, 3, variables, parent);
                                            format_fact(output, _lang, d, 3, {}, parent);

                                            std::wstring message = output + L" ⇐ " + input;

                                            if (do_print)
                                            {
                                                print(message, true);
                                            }

                                            if (_generate_markdown)
                                            {
                                                _markdown->add(L"Deductions", message);
                                            }
                                        }

                                        _done = true;
                                    }
                                }
                                catch (const std::exception&)
                                {
                                    throw contradiction_error(ctx.current_condition, variables, parent);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
