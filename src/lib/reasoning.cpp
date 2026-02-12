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

// Recursively collect all variable nodes from a fact pattern.
// Used to detect "fresh variables" — variables that appear only in rule
// consequences and need to be bound to newly created nodes.
static void collect_variables(Zelph* z, Node pattern, std::unordered_set<Node>& vars, std::vector<Node>& history)
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

    FactStructure fs = get_preferred_structure(z, pattern, history);
    if (fs.subject == 0)
    {
        history.pop_back();
        return; // Atomic, non-variable node
    }

    collect_variables(z, fs.subject, vars, history);
    collect_variables(z, fs.predicate, vars, history);
    for (Node o : fs.objects)
    {
        collect_variables(z, o, vars, history);
    }

    history.pop_back();
}

// =============================================================================
// New member function: is_negated_condition
// =============================================================================

bool Reasoning::is_negated_condition(Node condition)
{
    if (!_pImpl->exists(condition))
    {
#ifdef _DEBUG
        std::clog << "[NEG-CHECK] condition " << condition << " does not exist!" << std::endl;
#endif
        return false;
    }

    Answer ans    = check_fact(condition, core.IsA, {core.Negation});
    bool   result = ans.is_known();

#ifdef _DEBUG
    std::clog << "[NEG-CHECK] condition=" << condition
              << " name='" << string::unicode::to_utf8(get_name(condition, _lang, true))
              << "' IsA Negation? " << (result ? "YES" : "NO")
              << " (Negation core node=" << core.Negation << ")" << std::endl;
#endif

    return result;
}

// =============================================================================
// New member function: consequences_already_exist
//
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
// =============================================================================

bool Reasoning::consequences_already_exist(
    const Variables&     condition_bindings,
    const adjacency_set& deductions,
    Node                 parent)
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
        Node              source = instantiate_fact(this, var_source, working, history);

        adjacency_set targets;
        for (Node vt : var_targets)
        {
            history.clear();
            Node t = instantiate_fact(this, vt, working, history);
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

            // Negated conditions must be evaluated last to ensure
            // maximum variable binding before the existence check.
            if (is_negated_condition(cond))
                score -= 1000;

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

            // Build exclusion set: the conjunction set node and all its
            // element nodes are part of the rule topology and must not
            // be matched as data facts by the Unification engine.
            auto excluded = std::make_shared<std::unordered_set<Node>>();
            excluded->insert(condition); // The conjunction set node
            for (Node elem : sub_conditions)
            {
                excluded->insert(elem); // Each condition pattern node
            }

            // Recurse into the conjunction
            evaluate(RulePos({condition, sorted_conditions, 0, rule.variables, rule.unequals, excluded}), ctx);
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
        bool is_negated = is_negated_condition(condition);

        std::unique_ptr<Unification> u = std::make_unique<Unification>(
            this, condition, rule.node, rule.variables, rule.unequals, is_negated ? nullptr : _pool.get());

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
                    evaluate(next, ctx_copy);
                }
                else if (!ctx.next.empty())
                {
                    RulePos next   = ctx.next.back();
                    next.variables = bindings;
                    ctx.next.pop_back();
                    ReasoningContext ctx_copy = ctx;
                    evaluate(next, ctx_copy);
                }
                else
                {
                    // Terminal: all conditions satisfied
                    ReasoningContext ctx_copy = ctx;

                    if (!ctx_copy.rule_deductions.empty())
                    {
                        try
                        {
                            deduce(*bindings, rule.node, ctx_copy);
                        }
                        catch (const contradiction_error& error)
                        {
                            std::lock_guard<std::mutex> lock(_mtx_output);
                            _contradiction = true;
                            ++_total_contradictions;

                            std::wstring output;
                            format_fact(output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());
                            std::wstring message = L"«" + get_formatted_name(core.Contradiction, _lang) + L"» ⇐ " + output;

                            if (_print_deductions) print(message, true);
                            if (_generate_markdown) _markdown->add(L"Contradictions", message);
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
                        // Normal query output
                        std::lock_guard<std::mutex> lock(_mtx_output);
                        std::wstring                output;
                        format_fact(output, _lang, ctx_copy.current_condition, 3, *bindings, rule.node);
                        print(L"Answer: " + output, true);
                    }
                }
            };

            // No match => negation succeeds => continue with current bindings

#ifdef _DEBUG
            std::clog << "[NEG-EVAL] === Processing negated condition ===" << std::endl;
            std::clog << "[NEG-EVAL] condition=" << condition << std::endl;

            // Show what the negated pattern actually is
            {
                std::wstring cond_str;
                format_fact(cond_str, _lang, condition, 3, *rule.variables, rule.node);
                std::clog << "[NEG-EVAL] Formatted condition: "
                          << string::unicode::to_utf8(cond_str) << std::endl;
            }

            std::clog << "[NEG-EVAL] Current variable bindings:" << std::endl;
            for (const auto& [var, val] : *rule.variables)
            {
                std::clog << "[NEG-EVAL]   "
                          << string::unicode::to_utf8(get_name(var, _lang, true))
                          << " (id=" << var << ") -> "
                          << string::unicode::to_utf8(get_name(val, _lang, true))
                          << " (id=" << val << ")" << std::endl;
            }
#endif

            // --- Step 1: Try standard Unification ---
            // This handles the common case where all variables are already
            // bound by prior positive conditions.
            std::shared_ptr<Variables> match = u->Next();
            u->wait_for_completion();

            if (match)
            {
#ifdef _DEBUG
                std::clog << "[NEG-EVAL] MATCH FOUND => negation FAILS. Bindings:" << std::endl;
                for (const auto& [var, val] : *match)
                {
                    std::clog << "[NEG-EVAL]   "
                              << string::unicode::to_utf8(get_name(var, _lang, true))
                              << " (id=" << var << ") -> "
                              << string::unicode::to_utf8(get_name(val, _lang, true))
                              << " (id=" << val << ")" << std::endl;
                }
#endif
                // Match found => negation fails => prune this branch
                return;
            }
#ifdef _DEBUG
            std::clog << "[NEG-EVAL] NO MATCH => negation SUCCEEDS" << std::endl;
#endif

            // --- Step 2: No positive match. Determine how to proceed. ---
            // Parse the negated pattern to inspect its subject.
            adjacency_set pattern_objects;
            Node          pattern_subject = parse_fact(condition, pattern_objects, rule.node);

            bool subject_is_unbound = Zelph::Impl::is_var(pattern_subject)
                                   && rule.variables->find(pattern_subject) == rule.variables->end();

            if (!subject_is_unbound)
            {
                // Subject is bound or constant. The Unification already
                // checked all facts correctly (iterating by relation,
                // matching the bound subject). No match → negation
                // genuinely succeeds with the current bindings.
                proceed_with_bindings(rule.variables);
            }
            else
            {
                // Subject is unbound. The Unification couldn't produce
                // matches because it requires ALL parts to unify
                // simultaneously. We use complementary enumeration:
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
                    Node              inst_subj = instantiate_fact(this, pattern_subject, test, hist);

                    adjacency_set inst_objs;
                    bool          resolved = true;
                    for (Node po : pattern_objects)
                    {
                        hist.clear();
                        Node io = instantiate_fact(this, po, test, hist);
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
                        return;
                    }
                }
            }

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
        collect_variables(this, deduction, deduction_vars, history);
    }

    std::unordered_set<Node> fresh_vars;
    for (Node var : deduction_vars)
    {
        if (variables.find(var) == variables.end())
            fresh_vars.insert(var);
    }

    // --- Termination Check ---
    // If fresh variables exist, check whether these consequences have already
    // been created for the current condition bindings. Without this check,
    // each reasoning iteration would create new fresh nodes indefinitely.
    // The check is done against the live network, so it survives serialization.
    if (!fresh_vars.empty())
    {
        if (consequences_already_exist(variables, ctx.rule_deductions, parent))
            return;
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
    }

    // --- Process Deductions ---
    for (const Node deduction : ctx.rule_deductions)
    {
        if (deduction == core.Contradiction)
        {
            throw contradiction_error(ctx.current_condition, augmented, parent);
        }

        adjacency_set relations = filter(deduction, core.IsA, core.RelationTypeCategory);

        if (relations.size() == 1)
        {
            Node rel = Zelph::Impl::is_var(*relations.begin())
                         ? string::get(augmented, *relations.begin(), 0ull)
                         : *relations.begin();

            if (rel)
            {
                adjacency_set var_targets;
                Node          var_source = parse_fact(deduction, var_targets, parent);

                if (!var_targets.empty())
                {
                    std::vector<Node> history;
                    const Node        source = instantiate_fact(this, var_source, augmented, history);

                    if (source)
                    {
                        adjacency_set targets;
                        bool          done = true;
                        for (Node var_t : var_targets)
                        {
                            history.clear();
                            Node t = instantiate_fact(this, var_t, augmented, history);

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
                                throw contradiction_error(ctx.current_condition, augmented, parent);
                            }
                            else if (!answer.is_known()
                                     && targets.count(rel) == 0
                                     && targets.count(source) == 0)
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
                                            format_fact(input, _lang, ctx.current_condition, 3, augmented, parent);
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
                                    throw contradiction_error(ctx.current_condition, augmented, parent);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
