/*
Copyright (c) 2025 acrion innovations GmbH
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

using namespace zelph::network;

using adjacency_set = std::unordered_set<Node>;

Reasoning::Reasoning(const std::function<void(const std::wstring&, const bool)>& print)
    : Zelph(print)
    , _pool(std::make_unique<ThreadPool>(std::thread::hardware_concurrency()))
{
}

void Reasoning::run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition)
{
    auto reasoning_start = std::chrono::steady_clock::now();

    _print_deductions     = print_deductions;
    _generate_markdown    = generate_markdown;
    _skipped              = 0;
    _contradiction        = false;
    _total_matches        = 0;
    _total_contradictions = 0;

    if (_generate_markdown)
    {
        _markdown = std::make_unique<wikidata::Markdown>(std::filesystem::path("mkdocs") / "docs" / "tree", this);
    }

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

    std::clog << "Reasoning summary: " << _total_matches << " matches processed, "
              << _total_contradictions << " contradictions found." << std::endl;
    static std::unordered_set<Node> logged_relations;

    if (_pool)
    {
        std::clog << "Parallel unifications activated for " << logged_relations.size()
                  << " distinct fixed relations." << std::endl;
    }

    logged_relations.clear(); // Für nächsten Run

    auto   reasoning_end = std::chrono::steady_clock::now();
    double total_ms      = std::chrono::duration<double, std::milli>(reasoning_end - reasoning_start).count();

    std::clog << "Reasoning complete in " << total_ms << " ms – "
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
        adjacency_set conditions;
        conditions.insert(condition);

        ctx.current_condition = condition;
        ctx.next.clear();

        try
        {
            evaluate(RulePos({rule, conditions.end(), conditions.begin()}), ctx);
        }
        catch (const contradiction_error& error)
        {
            std::lock_guard<std::mutex> lock(_mtx_output);

            _contradiction = true;
            ++_total_contradictions;

            if (_print_deductions || _generate_markdown)
            {
                std::wstring output;
                format_fact(output, _lang, error.get_fact(), error.get_variables(), error.get_parent());
                std::wstring message = L"«!» ⇐ " + output;

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

void Reasoning::evaluate(RulePos rule, ReasoningContext& ctx)
{
    Node condition  = *rule.index;                  // points to the relation: normally 'and', connecting several conditions, or the relation of a fact (which may consist of variables)
    auto current_op = _pImpl->get_right(condition); // get the set of Nodes the relation points to, which is its type. Empty if the relation is a variable.

    // condition points to
    //    (1) the rule itself (i.e. the relaton node pointing to core.Causes), and
    //    (2) the type of relation, which may be any relation type (i.e. any node pointing having a core.IsA relation to core.RelationTypeCategory),
    //        but especially core.And (which makes this rule node a set of conditions)
    if (current_op.count(core.And) == 1)
    {
        auto condition_back_link = filter(
            _pImpl->get_left(condition),
            [&](const Node nd)
            { return nd != rule.node; });

        if (!condition_back_link.empty())
        {
            RulePos next(rule);
            if (++next.index != next.end) // if we are on first recursion level, this will always fail, since the top list of conditions always has length 1
            {
                ctx.next.push_back(next);
            }

            evaluate(RulePos({condition, condition_back_link.end(), condition_back_link.begin(), rule.variables, rule.unequals}), ctx);
        }
    }
    else
    {
        // here we run into for each part of a condition
        std::unique_ptr<Unification> u = std::make_unique<Unification>(
            this, condition, rule.node, rule.variables, rule.unequals, _pool.get());

        size_t local_match_count = 0;

        while (std::shared_ptr<Variables> match = u->Next())
        {
            ++local_match_count;
            ++_total_matches;

            std::shared_ptr<Variables> joined          = join(*rule.variables, *match);
            std::shared_ptr<Variables> joined_unequals = join(*rule.unequals, *u->Unequals());

            if (contradicts(*joined, *joined_unequals))
            {
                continue;
            }

            if (joined->empty())
            {
                continue;
            }

            auto temp_index = rule.index;
            ++temp_index;

            if (temp_index != rule.end) // if we are on first recursion level, this will always fail, since the top list of conditions always has length 1
            {
                RulePos next   = rule;
                next.variables = joined;
                next.unequals  = joined_unequals;
                ++next.index;

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
                // Leaf: sequentiell deduce/reporting
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
                        format_fact(output, _lang, error.get_fact(), error.get_variables(), error.get_parent());
                        std::wstring message = L"«!» ⇐ " + output;

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
                else
                {
                    std::lock_guard<std::mutex> lock(_mtx_output);
                    std::wstring                output;
                    format_fact(output, _lang, ctx_copy.current_condition, *joined, rule.node);
                    print(L"Answer: " + output, true);
                }
            }
        }

        u->wait_for_completion();

        if (!_generate_markdown && local_match_count > 0)
        {
            std::clog << "Condition processed: " << local_match_count << " raw matches." << std::endl;
        }
    }
}

bool Reasoning::contradicts(const Variables& variables, const Variables& unequals) const
{
    for (const auto& var : unequals)
    {
        Node item1 = var.first;
        if (_pImpl->is_var(item1))
        {
            auto it = variables.find(item1);
            if (it != variables.end())
                item1 = it->second;
            else
                continue; // though this variable is included in the unequals, it is missing in the normal variables (meaningless unequal condition)
        }

        Node item2 = var.second;
        if (_pImpl->is_var(item2))
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
            Node rel = _pImpl->is_var(*relations.begin())
                         ? utils::get(variables, *relations.begin(), 0ull)
                         : *relations.begin();

            if (rel)
            {
                adjacency_set var_targets;
                Node          var_source = parse_fact(deduction, var_targets, parent);

                if (!var_targets.empty())
                {
                    const Node source = _pImpl->is_var(var_source)
                                          ? utils::get(variables, var_source, 0ull)
                                          : var_source;

                    if (source)
                    {
                        adjacency_set targets;
                        bool          done = true;
                        for (Node var_t : var_targets)
                        {
                            Node t = _pImpl->is_var(var_t)
                                       ? utils::get(variables, var_t, 0ull)
                                       : var_t;

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
                                            format_fact(input, _lang, ctx.current_condition, variables, parent);
                                            format_fact(output, _lang, d, {}, parent);

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