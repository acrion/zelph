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

#include "script/command_executor_impl.hpp"

#include "network/network.hpp"
#include "network/reasoning.hpp"
#include "script/script_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace zelph;

namespace zelph::console
{
    void CommandExecutor::Impl::cmd_remove(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".remove");

        if (cmd.size() != 2) throw std::runtime_error("Command .remove requires exactly one argument: name or ID");

        const std::string& arg = cmd[1];
        network::Node      nd  = resolve_single_node(arg, true); // prioritize ID

        if (nd == 0)
        {
            try
            {
                size_t pos = 0;
                nd         = std::stoull(arg, &pos);
                if (pos != arg.length())
                {
                    throw std::exception();
                }
            }
            catch (const std::exception&)
            {
                throw std::runtime_error("Command .remove: Unknown node '" + arg + "' in current language '" + _n->lang() + "'");
            }

            if (!_n->exists(nd))
            {
                throw std::runtime_error("Command .remove: Node '" + std::to_string(nd) + "' does not exist");
            }
        }

        // A node connected by an edge constitutes a fact or a part of one (see
        // note_user_removal).
        if (_n->left_count(nd) != 0 || _n->right_count(nd) != 0) _n->note_user_removal();
        const size_t removed = _n->remove_node(nd);
        _n->out("Removed node " + std::to_string(nd) + " and " + std::to_string(removed - 1)
                    + " node(s) it was part of (names cleaned).",
                true);
        _n->diagnostic("Consider running .cleanup afterwards if needed.", true);
    }

    void CommandExecutor::Impl::cmd_prune(const std::vector<std::string>& cmd, bool facts_mode)
    {
        const char* const command = facts_mode ? ".prune-facts" : ".prune-nodes";
        require_full_graph_mode(command);
        if (cmd.size() < 2)
            throw std::runtime_error("Command requires a pattern");

        // ".prune-nodes A (A P31 C, C P279∗ Q6999)": the leading token names
        // whose bindings die, which is what makes a CONJUNCTION usable -- it
        // has one variable per condition, and without this nothing says which
        // of them is meant.
        //
        // The reading is unambiguous, and that is why the pattern has to be
        // parenthesised for it: a statement needs three elements, so a
        // variable followed by a bracketed pattern is never one, while
        // ".prune-nodes A p B" -- the single-fact form, three tokens, the
        // first a variable -- keeps its meaning untouched.
        //
        // The focus operator was the first idea and it does not work:
        // "(*A P31 C, ...)" makes the condition EVALUATE to the focused node,
        // so the condition is replaced by a bare variable and disappears from
        // the conjunction.
        std::size_t pattern_first = 1;
        std::string target_name;
        if (cmd.size() >= 3 && string::is_var(cmd[1]) && !cmd[2].empty() && cmd[2].front() == '(')
        {
            if (facts_mode)
                throw std::runtime_error(
                    ".prune-facts takes a pattern only. A leading variable names what gets DELETED, "
                    "which is what .prune-nodes does; .prune-facts removes the facts its pattern "
                    "matches, and a conjunction matches several per solution with nothing to say "
                    "which was meant.");

            target_name   = cmd[1];
            pattern_first = 2;
        }

        // The same reading .explain gives the same tokens -- see
        // pattern_code. Quoting every non-variable token, as this used to,
        // reduces the pattern to a triple of literal names and takes every
        // structured pattern with it: a nested fact, a term island, ¬, an
        // &-literal, a list, a set, and a pattern the user wrapped in
        // parentheses the way .explain and the documentation write them.
        //
        // `pattern_first` is where the tokens start in the COMMAND, and it
        // indexes _sources -- passing 1 for a pattern that begins at 2
        // reconstructs it shifted by one token and drops the last one, which
        // fails as "Could not parse pattern" without saying why.
        // The generator's own refusal is the specific one -- it names the
        // fragment and what is missing from it -- and "Could not parse
        // pattern" says none of that. It is kept only for a pattern that
        // failed without a reason to give.
        std::string       why;
        const std::string janet_code = pattern_code({cmd.begin() + static_cast<long>(pattern_first), cmd.end()}, pattern_first, nullptr, &why);

        if (janet_code.empty())
            throw std::runtime_error(why.empty() ? "Could not parse pattern" : why);

        // Evaluating the pattern MATERIALIZES it, exactly as it does for
        // .explain -- and a REMOVAL command that adds what it was asked to
        // delete is the worst kind of surprise: ".prune-facts Q42 typo Q7"
        // used to insert that very fact. The construction therefore runs
        // inside a scratch cluster which is dropped afterwards, so a
        // pattern the graph did not already contain leaves no trace.
        //
        // The cluster keeps the assertion from SURVIVING; it cannot keep it
        // from being refused. `Zelph::fact` will not claim a fact the graph
        // holds as known-wrong, so ".prune-facts (a p b)" answered "fact():
        // this fact is known to be wrong" for a refuted fact instead of saying
        // anything about pruning -- the same failure f60f05d fixed for .node
        // and .explain, on the one command family its own comment names and
        // did not reach.
        //
        // A pattern with VARIABLES is still built, and must be: it is what
        // unification matches against, so it needs its edges and not merely
        // the id of a triple. That is why the flag answers only for a fact the
        // graph holds -- see the note in `janet_cfun_zelph_fact`.
        static const std::string scratch  = "__prune";
        const std::string        previous = _n->active_cluster_name();
        _n->set_active_cluster(scratch);

        network::Node pattern_fact = 0;
        try
        {
            pattern_fact = _script_engine->evaluate_expression(janet_code, /*quiet*/ true, /*resolving_pattern*/ true);
        }
        catch (...)
        {
            restore_cluster(previous);
            _n->drop_cluster(scratch);
            throw;
        }

        const auto discard_pattern = [&]
        {
            restore_cluster(previous);
            _n->drop_cluster(scratch);
        };

        if (pattern_fact == 0)
        {
            discard_pattern();
            throw std::runtime_error("Invalid pattern");
        }

        // A condition operator matches no fact: a negation holds when its
        // pattern is ABSENT, a path is walked, a net is queried. When
        // treated as a pattern, ".prune-facts ¬(X p b)" removed every `X p
        // b` -- the facts the line says are absent.
        if (_n->is_condition_tag(pattern_fact))
        {
            discard_pattern();
            throw std::runtime_error(std::string(command) + " removes what its pattern matches, and a condition operator "
                                                            "(¬, ≈, ⁺, ∗) matches no fact: it is read only as a condition. "
                                                            "Write the plain fact pattern.");
        }

        // A pattern without variables denotes exactly ONE fact, and the
        // unification scan that finds "all facts matching the pattern" has
        // nothing to bind, so it found none -- ".prune-facts a rel b"
        // silently did nothing. Dropping the scratch first answers the only
        // question that remains: whatever survives existed beforehand.
        std::unordered_set<network::Node> pattern_vars;
        {
            std::vector<network::Node> history;
            network::collect_variables(_n, pattern_fact, pattern_vars, 1, history);
        }

        // The variable whose bindings die, as the NODE of this very pattern.
        // Resolved here rather than by name at match time: the letter is text,
        // a variable is quantified per statement, and many nodes may display
        // one letter (`be16650`) -- but within the one expression that built
        // this pattern a symbol is one variable node, so the letter has
        // exactly one answer.
        //
        // collect_variables stops AT a conjunction container: its variables
        // sit one level down, in the conditions, which is also why the
        // pattern_vars above is empty for one.
        network::Node target_var = 0;
        if (!target_name.empty())
        {
            std::unordered_set<network::Node> vars;
            std::vector<network::Node>        history;
            network::adjacency_set            members;

            if (_n->condition_set_members(pattern_fact, members))
                for (const network::Node member : members)
                    network::collect_variables(_n, member, vars, 1, history);
            else
                vars = pattern_vars;

            std::vector<std::string> spelled;
            for (const network::Node v : vars)
            {
                const std::string name = _n->get_name(v, _n->lang(), true);
                if (name == target_name) target_var = v;
                if (!name.empty()) spelled.push_back(name);
            }

            if (target_var == 0)
            {
                std::sort(spelled.begin(), spelled.end());
                std::string list;
                for (const std::string& s : spelled)
                {
                    if (!list.empty()) list += ", ";
                    list += s;
                }

                discard_pattern();
                throw std::runtime_error(
                    "Command .prune-nodes: the pattern has no variable " + target_name
                    + ", so nothing says what to delete. Its variables are: "
                    + (list.empty() ? std::string("none") : list) + ".");
            }
        }

        // A CONJUNCTION reaches here with no variables of its own -- its
        // conditions carry them, one level down -- and fell into the branch
        // below, which reported "a pattern without variables binds nothing to
        // delete" about a pattern full of variables and pruned nothing.
        // Without a leading variable the command genuinely cannot take one: it
        // deletes what ONE variable binds, and a conjunction has one per
        // condition. Naming that variable is exactly what the leading token
        // does, so the refusal says how instead of only that.
        if (target_var == 0 && _n->check_fact(pattern_fact, _n->core.IsA, {_n->core.Conjunction}).is_known())
        {
            discard_pattern();
            throw std::runtime_error(
                std::string(command)
                + (facts_mode
                       ? " takes a single fact pattern, not a conjunction: it removes the facts its"
                         " pattern matches, and a conjunction matches several per solution with"
                         " nothing to say which is meant. Write one command per condition."
                       : " over a conjunction needs to be told which variable names what gets"
                         " deleted: write \".prune-nodes <variable> (<conditions>)\", e.g."
                         " \".prune-nodes A (A P31 C, C P279∗ Q6999)\"."));
        }

        // A set pattern contains variables within its constituents, which
        // are not inspected by collect_variables, and behaves as any
        // pattern involving variables when matched.
        if (target_var == 0 && pattern_vars.empty() && !network::holds_set_pattern(_n, pattern_fact))
        {
            discard_pattern();

            // Present is not the same as CLAIMED. The variable form below goes
            // through unification, which skips a rule's own ground patterns
            // (afc0f3e), so ".prune-facts (S p O)" correctly prunes nothing
            // where the only "a p b" in the graph is a rule's condition. This
            // form read the node structurally and deleted it -- taking the
            // rule with it, since a rule goes with a node its condition is
            // built from. One statement, two notions of matching, and the
            // ground one destroyed data the user was not told about.
            // is_asserted_fact is the reading the whole read surface settled
            // on (0d0d0a6); .explain keeps the structural probe because it
            // REPORTS the state instead of acting on it.
            //
            // Neither the engine's bookkeeping nor a rule's own text
            // amounts to a claim: the variable form skips both,
            // just like a scan of a prune does (Zelph::is_bookkeeping,
            // Zelph::is_rule_text_fact).
            const bool                           present = _n->check_fact(pattern_fact).is_known();
            const network::Reasoning::PrunedFact pruned  = present ? _n->prune_fact(pattern_fact) : network::Reasoning::PrunedFact{};
            const bool                           exists  = pruned.outcome != network::Reasoning::FactPrune::none;

            const std::string what = std::to_string(pruned.count);
            if (facts_mode)
                _n->out("Pruned " + what + " matching facts.", true);
            else
                _n->out("Pruned " + what + " matching facts and 0 nodes (a pattern without variables binds nothing to delete).", true);

            if (exists) _n->diagnostic("Consider running .cleanup.", true);

            // is_asserted_fact has three ways of saying no, and the hint named
            // only the first of them -- so a REFUTED fact was reported as a
            // rule's own pattern, which is a diagnosis of the wrong mechanism
            // and sends the reader to look for a rule that is not there. Each
            // reason gets its own sentence, because each has a different next
            // step: a pattern belongs to a rule, a refutation is a claim in
            // its own right, and a pattern with variables in its closure was
            // never a statement about anything.
            if (present && !exists && !_n->is_core_declaration(pattern_fact))
            {
                if (_n->is_refuted_fact(pattern_fact))
                    _n->diagnostic("That statement is held as REFUTED -- the graph claims it does not "
                                   "hold, so there is no claim of it to remove. Use .node to get its ID "
                                   "and .remove to delete the node itself.",
                                   true);
                else if (_n->is_rule_pattern(pattern_fact))
                    _n->diagnostic("That statement exists only as a rule's own pattern, or as one a query wrote "
                                   "beneath a condition operator, not as data -- the prune commands remove "
                                   "claims. Use .node to get its ID and .remove to delete graph structure.",
                                   true);
                // A fourth reason and a fifth, which is_asserted_fact does not
                // give: the engine's internal bookkeeping concerning a rule or
                // a contradiction, and a rule's own text.
                else if (_n->is_bookkeeping(pattern_fact))
                    _n->diagnostic("That statement is the engine's own bookkeeping -- the mark of a rule's pattern, "
                                   "a contradiction record or one of its members -- not data, and the prune commands "
                                   "remove claims. Use .node to get its ID and .remove to delete graph structure.",
                                   true);
                else if (_n->is_rule_text_fact(pattern_fact))
                    _n->diagnostic("That statement is part of a rule's text -- a condition set's tag or one of its "
                                   "members -- not a claim, and the prune commands remove claims. Use .node to get "
                                   "its ID and .remove to delete graph structure.",
                                   true);
                else
                    _n->diagnostic("That statement carries variables, so nobody claimed it -- the prune "
                                   "commands remove claims. Use .node to get its ID and .remove to delete "
                                   "graph structure.",
                                   true);
            }
            if (!present) explain_collection_literal({cmd.begin() + static_cast<long>(pattern_first), cmd.end()});
            return;
        }

        if (facts_mode)
        {
            size_t removed = 0;
            _n->prune_facts(pattern_fact, removed);
            discard_pattern();
            _n->out("Pruned " + std::to_string(removed) + " matching facts.", true);
            if (removed > 0) _n->diagnostic("Consider running .cleanup.", true);
        }
        else
        {
            // Both checks below ask which side of ONE fact is meant, and a
            // named target variable has already answered that -- for a
            // conjunction there is no single predicate to be fixed in the
            // first place, and having several variables is the point.
            if (target_var == 0)
            {
                network::Node relation = _n->parse_relation(pattern_fact);
                if (network::Network::is_var(relation))
                {
                    discard_pattern();
                    throw std::runtime_error("Command .prune-nodes: relation (predicate) must be fixed");
                }

                // One variable is the documented requirement, and it is the
                // only one the command can honour: with two, it deleted the
                // SUBJECT bindings and left the object ones alone, without
                // saying so. On a loaded dump that is half a deletion nobody
                // asked for.
                if (pattern_vars.size() > 1)
                {
                    discard_pattern();
                    throw std::runtime_error("Command .prune-nodes: exactly one variable is allowed -- "
                                             "it names what gets deleted, or name it yourself with \".prune-nodes <variable> (<conditions>)\". "
                                             "Use .prune-facts to remove facts without deleting nodes.");
                }

                // When there is no variable present, there is no entity
                // for a name to refer to: the variables of a rule the set
                // holds belong to that rule.
                if (pattern_vars.empty())
                {
                    discard_pattern();
                    throw std::runtime_error("Command .prune-nodes: the pattern has no variable outside the rules it mentions, "
                                             "so nothing in it says what gets deleted.");
                }

                // A variable serving as the predicate within a fact the
                // pattern nests binds a relation, just as one at the top
                // level does, and its binding carries along every fact
                // belonging to that relation: `.prune-nodes (a X b) q c`
                // deleted p, `e p f` included. It is refused at any level,
                // just as at the top; named, the variable says that this is
                // meant.
                const network::Node                variable = *pattern_vars.begin();
                std::unordered_set<network::Node>  seen;
                std::function<bool(network::Node)> as_predicate = [&](const network::Node nd)
                {
                    if (nd == 0 || network::Network::is_var(nd) || !seen.insert(nd).second) return false;
                    network::adjacency_set parts;
                    if (_n->set_pattern_members(nd, parts)) return std::any_of(parts.begin(), parts.end(), as_predicate);
                    // Only the pattern's own structure is walked: the members
                    // of a set pattern, the subject and objects of a fact. An
                    // atom like `c` in `X q c` or `Q5` in `X P31 Q5` ends the
                    // traversal: interpreted as a fact, it handed on every
                    // adjacent node, and the walk covered the whole graph.
                    const network::Node relation = _n->predicate_of(nd);
                    if (relation == 0 || _n->is_named_any(nd)) return false;
                    if (relation == variable) return true;
                    parts.insert(_n->parse_fact(nd, parts));
                    return std::any_of(parts.begin(), parts.end(), as_predicate);
                };
                if (as_predicate(pattern_fact))
                {
                    discard_pattern();
                    throw std::runtime_error("Command .prune-nodes: relation (predicate) must be fixed, inside a nested fact or a set as at the top -- "
                                             "the variable would delete the relation and every fact of it. To mean that, name it: "
                                             "\".prune-nodes <variable> (<pattern>)\".");
                }

                // The single variable determines what is removed, regardless
                // of its location. Whether in subject or object position, the
                // matched fact specifies this; when embedded within a fact or
                // a set, it is the variable's binding, read as the named form
                // reads it. Read by position, `.prune-nodes (X p b) q c`
                // deleted c, the object, and `.prune-nodes {(X enters c)}
                // holds k` deleted k.
                network::adjacency_set objects;
                if (_n->parse_fact(pattern_fact, objects) != variable && objects.count(variable) == 0) target_var = variable;
            }

            size_t removed_facts = 0;
            size_t removed_nodes = 0;
            _n->prune_nodes(pattern_fact, target_var, removed_facts, removed_nodes);
            discard_pattern();
            _n->out("Pruned " + std::to_string(removed_facts) + " matching facts and " + std::to_string(removed_nodes) + " nodes.", true);
            if (removed_facts > 0 || removed_nodes > 0)
            {
                _n->diagnostic("Consider running .cleanup.", true);
            }
        }
    }

    void CommandExecutor::Impl::cmd_cleanup(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".cleanup");
        if (cmd.size() != 1)
            throw std::runtime_error("Command .cleanup takes no arguments");

        size_t removed_facts = 0;
        size_t removed_preds = 0;

        _n->diagnostic("Scanning for unused predicates and zombie facts...", true);

        _n->purge_unused_predicates(removed_facts, removed_preds);

        _n->out("Purged " + std::to_string(removed_facts) + " zombie facts.", true);
        _n->out("Removed " + std::to_string(removed_preds) + " unused predicates.", true);

        _n->diagnostic("Cleaning up isolated nodes...", true);

        size_t cleanup_count = 0;
        _n->cleanup_isolated(cleanup_count);
        _n->out("Cleanup: removed " + std::to_string(cleanup_count) + " isolated nodes/names.", true);

        _n->diagnostic("Cleaning up name mappings...", true);
        size_t names_removed = _n->cleanup_names();
        _n->out("Removed " + std::to_string(names_removed) + " dangling name entries.", true);
    }

    void CommandExecutor::Impl::cmd_new(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .new takes no arguments");

        // A module does not clear the session that loads it, just as a `.quit`
        // within it does not end that session. Nor is a module capable of being
        // continued after a reset, unlike a session script: the `.import`
        // command responsible for loading it, or the Janet call `zelph/import`,
        // resides within the engine that the reset eliminates, and carrying out
        // the reset there ended in a segmentation fault.
        if (_repl_state->import_depth > 0)
            throw std::runtime_error("Command .new cannot run inside a module (a file loaded with .import or zelph/import): "
                                     "a module does not clear the session that loads it");

        _repl_state->reset_requested = true;
    }
}
