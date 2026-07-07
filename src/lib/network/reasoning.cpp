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
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

#include <cassert>
#include <cmath>
#include <vector>

using namespace zelph::network;

Reasoning::Reasoning(const io::OutputHandler& output)
    : Zelph{output}
    , _pool{std::make_unique<concurrency::ThreadPool>(std::thread::hardware_concurrency())}
    , _prof{this}
{
}

void Reasoning::set_markdown_subdir(const std::string& subdir)
{
    _markdown_subdir = subdir;
}

void Reasoning::set_query_collector(std::vector<std::shared_ptr<Variables>>* collector)
{
    _query_results = collector;
}

void Reasoning::run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition, const bool silent)
{
    chrono::StopWatch watch;
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
        _markdown = std::make_unique<io::Markdown>(std::filesystem::path("mkdocs") / "docs" / _markdown_subdir, this);
    }

    if (!silent)
        diagnostic("Starting reasoning with " + std::to_string(_pool->count()) + " worker threads.");

    int iteration = 0;
    do
    {
        _done = false;
        ++iteration;
        if (!silent)
            diagnostic_stream() << "--- Reasoning iteration " << iteration << " ---" << std::endl;
        for (Node rule : _pImpl->get_left(core.Causes))
        {
            apply_rule(rule, 0);
        }

        _pool->wait();
    } while (_done && !suppress_repetition);

    if (!silent)
        diagnostic_stream() << "Reasoning complete. Total unification matches processed: " << _total_matches
                            << ". Total contradictions found: " << _total_contradictions << "." << std::endl;

    if (_skipped > 0) diagnostic(" (skipped " + std::to_string(_skipped) + " deductions)", true);

    if (_contradiction)
    {
        diagnostic("Found one or more contradictions!", true);
    }

    if (_done && suppress_repetition)
    {
        out("Warning: Additional reasoning iterations are required, but have been suppressed.", true);
    }

    if (!silent)
        diagnostic_stream() << "Reasoning summary: " << _total_matches << " matches processed, "
                            << _total_contradictions << " contradictions found." << std::endl;
    static std::unordered_set<Node> logged_relations;

    if (_pool && !silent)
    {
        diagnostic_stream() << "Parallel unifications activated for " << logged_relations.size()
                            << " distinct fixed relations." << std::endl;
    }

    logged_relations.clear();

    watch.stop();

    if (!silent)
        diagnostic_stream() << "Reasoning complete in " << watch.format() << " – "
                            << _total_matches << " matches processed, "
                            << _total_contradictions << " contradictions found." << std::endl;
}

void Reasoning::apply_rule(const Node& rule, Node condition)
{
    _prof.note_rule_applied(rule ? rule : condition);

    _nn_pred        = get_node("nn", "zelph");
    _nn_layers_pred = get_node("nn-layers", "zelph");

    if (should_log(1))
    {
        std::string formatted_rule;
        string::node_to_string(this, formatted_rule, _lang, rule, 3);
        log(0, "rule", "=== Applying rule " + formatted_rule + " ===");
    }
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
            evaluate(RulePos({rule, conditions, 0}), ctx, 1);
        }
        catch (const contradiction_error& error)
        {
            std::lock_guard<std::mutex> lock(_mtx_output);
            _contradiction = true;
            ++_total_contradictions;

            if (_print_deductions || _generate_markdown)
            {
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

        _pool->wait();
    }
}

// Greedy Sort to optimize execution order based on variable bindings
std::shared_ptr<std::vector<Node>> Reasoning::optimize_order(const adjacency_set& conditions, const Variables& current_vars, int depth)
{
    if (logging_active())
        _prof.optimize_order_calls.fetch_add(1, std::memory_order_relaxed);

    auto sorted = std::make_shared<std::vector<Node>>();
    sorted->reserve(conditions.size());

    // Copy to a temporary list we can destructively consume
    std::vector<Node> pending(conditions.begin(), conditions.end());
    Variables         simulated_vars = current_vars;

    // A term counts as "bound" for scoring purposes if the unification engine
    // can resolve it to a concrete node without scanning: atoms, bound
    // variables, and structured patterns whose variables are all
    // (simulated-)bound -- the latter thanks to bound-pattern grounding in
    // Unification. A pattern with unbound inner variables is as expensive as
    // an unbound variable and must not receive the bonus.
    auto is_bound_term = [&](Node nd) -> bool
    {
        if (nd == 0) return false;
        if (Zelph::Impl::is_var(nd)) return simulated_vars.count(nd) != 0;
        if (!Zelph::Impl::is_hash(nd)) return true; // plain atom

        std::unordered_set<Node> vars;
        std::vector<Node>        history;
        collect_variables(this, nd, vars, depth, history);
        for (Node v : vars)
            if (simulated_vars.count(v) == 0) return false;
        return true;
    };

    while (!pending.empty())
    {
        auto   best_it   = pending.end();
        double max_score = -999999;

        for (auto it = pending.begin(); it != pending.end(); ++it)
        {
            Node          cond  = *it;
            double        score = 0;
            adjacency_set objects;
            Node          subject = parse_fact(cond, objects); // Relation is ignored for scoring for now, could be added

            if (is_bound_term(subject))
                score += 100; // subject resolvable without scanning (atom, bound var, groundable pattern)
            else
                score -= 10; // subject requires scanning

            for (Node obj : objects)
            {
                if (is_bound_term(obj))
                    score += 50;
                else
                    score -= 10;
            }

            // Among otherwise comparable conditions, prefer the one that binds
            // more still-unbound variables: every variable it binds can turn a
            // later condition's pattern into a direct lookup (bound-pattern
            // grounding) instead of a scan. Weight 2 keeps this a tie-breaker
            // relative to the +/-100, +/-50 and log2 terms.
            {
                std::unordered_set<Node> cond_vars;
                std::vector<Node>        history;
                collect_variables(this, cond, cond_vars, depth, history);
                size_t new_vars = 0;
                for (Node v : cond_vars)
                    if (simulated_vars.count(v) == 0) ++new_vars;
                score += 2.0 * static_cast<double>(new_vars);
            }

            // Negated conditions must be evaluated last to ensure
            // maximum variable binding before the existence check.
            if (is_negated_condition(cond, depth))
                score -= 1000;

            adjacency_set rels_for_score = filter(cond, core.IsA, core.RelationTypeCategory);

            // Inequality guards must be evaluated after the involved
            // variables are bound — similar to negation, but higher priority
            // (negation at -1000 is always last, != at -500 is second-to-last).
            if (rels_for_score.size() == 1 && *rels_for_score.begin() == core.Unequal)
                score -= 500;

            // Neural conditions want maximal bindings and are comparatively
            // expensive: evaluate after != (-500), before negation (-1000).
            if (_nn_pred != 0 && rels_for_score.size() == 1 && *rels_for_score.begin() == _nn_pred)
                score -= 800;

            // Prefer conditions whose predicate has fewer matching facts
            if (rels_for_score.size() == 1)
            {
                Node rel = *rels_for_score.begin();
                if (!Zelph::Impl::is_var(rel))
                {
                    // Size-only lookup: snapshot_left_of copied the entire
                    // relation extent per scoring iteration just to read its
                    // size -- catastrophic for high-cardinality relations
                    // such as P31 on a full Wikidata load.
                    const size_t n = _pImpl->left_count_of(rel);
                    if (n > 0)
                    {
                        // Subtract a small penalty proportional to log(fact_count)
                        // so that high-cardinality relations are tried last
                        score -= std::log2(static_cast<double>(n));
                    }
                }
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
            if (should_log(depth + 1))
            // Log the selected condition with its score
            {
                adjacency_set rels     = filter(best_cond, core.IsA, core.RelationTypeCategory);
                std::string   rel_name = "?";
                if (rels.size() == 1) rel_name = get_name(*rels.begin(), _lang, true);
                log(depth + 1, "optorder", "Selected condition=" + format(best_cond) + " rel=" + rel_name + " score=" + std::to_string(static_cast<int>(max_score)));
            }

            sorted->push_back(best_cond);

            // Bind variables for next iteration.
            // Simulate the bindings this condition will produce: matching a
            // condition binds ALL variables occurring anywhere in it, at any
            // structural depth -- the previous shallow parse_fact() binding
            // never marked variables inside nested patterns as bound, so the
            // planner could not see that later conditions become groundable.
            {
                std::unordered_set<Node> cond_vars;
                std::vector<Node>        history;
                collect_variables(this, best_cond, cond_vars, depth, history);
                for (Node v : cond_vars)
                    simulated_vars[v] = 1; // dummy bind
            }

            pending.erase(best_it);
        }
        else
        {
            // Should not happen unless empty
            break;
        }
    }

    if (should_log(depth) && !sorted->empty())
    {
        std::string order_str;
        for (size_t i = 0; i < sorted->size(); ++i)
        {
            adjacency_set rels     = filter((*sorted)[i], core.IsA, core.RelationTypeCategory);
            std::string   rel_name = "?";
            if (rels.size() == 1) rel_name = get_name(*rels.begin(), _lang, true);
            order_str += " [" + std::to_string(i) + "]=" + rel_name;
        }
        log(depth, "optorder", "Final order:" + order_str);
    }

    return sorted;
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

Node zelph::network::instantiate_fact(Zelph* z, Node pattern, const Variables& variables, const int depth, std::vector<Node>& history)
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
    FactStructure fs = get_preferred_structure(z, pattern, depth);

    if (z->should_log(depth))
        z->log(depth, "instantiate", "fact=" + z->format(pattern) + " subj=" + z->format(fs.subject) + " pred=" + z->format(fs.predicate) + " objs=" + std::to_string(fs.objects.size()));

    if (fs.subject == 0)
    {
        history.pop_back();
        return pattern; // Atomic / No structure found
    }

    Node inst_subject  = instantiate_fact(z, fs.subject, variables, depth, history);
    Node inst_relation = instantiate_fact(z, fs.predicate, variables, depth, history);

    adjacency_set inst_objects;
    bool          changed = (inst_subject != fs.subject) || (inst_relation != fs.predicate);

    for (Node o : fs.objects)
    {
        Node io = instantiate_fact(z, o, variables, depth, history);
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

// Recursively collect all variable nodes from a fact pattern.
// Used to detect "fresh variables" — variables that appear only in rule
// consequences and need to be bound to newly created nodes.
void zelph::network::collect_variables(Zelph* z, Node pattern, std::unordered_set<Node>& vars, const int depth, std::vector<Node>& history)
{
    if (pattern == 0) return;

    if (Zelph::Impl::is_var(pattern))
    {
        vars.insert(pattern);
        return;
    }

    // Cycle check
    for (Node visited : history)
    {
        if (visited == pattern) return;
    }
    history.push_back(pattern);

    FactStructure fs = get_preferred_structure(z, pattern, depth);
    if (fs.subject == 0)
    {
        history.pop_back();
        return; // Atomic, non-variable node
    }

    collect_variables(z, fs.subject, vars, depth, history);
    collect_variables(z, fs.predicate, vars, depth, history);
    for (Node o : fs.objects)
    {
        collect_variables(z, o, vars, depth, history);
    }

    history.pop_back();
}
