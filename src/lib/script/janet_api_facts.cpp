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

#include "script/script_engine_impl.hpp"

#include "network/reasoning.hpp"
#include "string/string_utils.hpp"

#include <janet.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    // The cluster in which a rule is constructed. A cluster records precisely
    // the nodes that were CREATED during its active phase, which is what
    // tells a rule's own parts from the nodes it reuses.
    const char* const kRuleScratch = "__rule";

    // The cluster where a query carrying a condition operator runs
    // (zelph/query-text): the records it captures are those the statement
    // created.
    const char* const kQueryScratch = "__query";

    // Whether a thunk concluded in a suspended state instead of
    // returning a result or encountering an error.
    bool suspended(const JanetSignal signal)
    {
        return signal != JANET_SIGNAL_OK && signal != JANET_SIGNAL_ERROR;
    }

    // A thunk that writes a rule executes within a C call, within the confines
    // of the template scope. A thunk that awaits the event loop -- ev/sleep, a
    // channel, a stream -- or yields becomes suspended at that point, and a C
    // call cannot be resumed: the rule would vanish without a word. Nor could
    // the scope remain open during the wait, as every other fibre of the
    // thread would otherwise write rule text in the interim. Thus, the rule is
    // refused.
    [[noreturn]] void refuse_suspended(const std::string& message)
    {
#ifdef JANET_EV
        // The wait is registered for the root fibre: a newly assigned
        // schedule id voids the wake-up, which would resume that fibre at
        // whatever it does next, or, once the refusal has ended it, report a
        // second error.
        if (JanetFiber* const root = janet_root_fiber()) ++root->sched_id;
#endif
        throw std::runtime_error(message);
    }

    [[noreturn]] void refuse_suspended_rule(const char* const caller)
    {
        refuse_suspended(std::string(caller) + ": the forms that write a rule cannot wait for the event loop or yield. "
                                               "Compute what they wait for before the rule, and write the rule from the result.");
    }

    // A value rooted for the lifetime of this
    // object's existence.
    struct Rooted
    {
        explicit Rooted(const Janet v)
            : value(v)
        {
            janet_gcroot(value);
        }
        ~Rooted() { janet_gcunroot(value); }
        Rooted(const Rooted&)                = delete;
        Rooted&     operator=(const Rooted&) = delete;
        const Janet value;
    };

    // The claim of a fact asserted from Janet: it enters only a part that
    // remains a rule's pattern, and the parts are at hand, so a fact whose
    // parts carry no mark (every fact from a bulk import) costs one probe
    // per part that constitutes a fact, and its structure is not read.
    void claim_marked_parts(const zelph::network::Zelph& n, const zelph::network::Node f, const zelph::network::Node s, const zelph::network::Node p, const zelph::network::adjacency_set& objs)
    {
        using zelph::network::Zelph;
        const auto marked = [&n](const zelph::network::Node x)
        { return x != 0 && !Zelph::is_var(x) && Zelph::is_hash(x) && n.is_rule_pattern(x); };
        if (!marked(s) && !marked(p) && std::none_of(objs.begin(), objs.end(), marked)) return;
        n.claim_nested(f, [](zelph::network::Node, bool)
                       { return Zelph::ClaimStep::Claim; });
    }
}

namespace zelph
{
    // Extract the car (first element / subject) of a cons cell.
    // Returns nil if the argument is nil or not a valid cons cell.
    Janet ScriptEngine::Impl::janet_cfun_zelph_car(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, true);

        network::Node cell = zelph_unwrap_node(argv[0]);
        if (!cell || cell == s_instance->_n->core.Nil)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, false, res);
            return res;
        }

        // Verify this is a cons cell
        if (s_instance->_n->parse_relation(cell) != s_instance->_n->core.Cons)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        network::Node          subject = s_instance->_n->parse_fact(cell, objs, 0);
        if (!subject)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, false, res);
            return res;
        }

        Janet res = zelph_wrap_node(subject);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/car", argc, argv, false, res);
        return res;
    }

    // Extract the cdr (rest of list / object) of a cons cell.
    // Returns nil-node if the argument is nil or not a valid cons cell.
    Janet ScriptEngine::Impl::janet_cfun_zelph_cdr(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, true);

        network::Node cell = zelph_unwrap_node(argv[0]);
        if (!cell || cell == s_instance->_n->core.Nil)
        {
            Janet res = zelph_wrap_node(s_instance->_n->core.Nil);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, false, res);
            return res;
        }

        // Verify this is a cons cell
        if (s_instance->_n->parse_relation(cell) != s_instance->_n->core.Cons)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        s_instance->_n->parse_fact(cell, objs, 0);
        if (objs.empty())
        {
            Janet res = zelph_wrap_node(s_instance->_n->core.Nil);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, false, res);
            return res;
        }

        Janet res = zelph_wrap_node(*objs.begin());
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/cdr", argc, argv, false, res);
        return res;
    }

    // `¬(pattern)`: the fact ((pattern) ~ negation), which serves as the
    // negated condition a rule names (Zelph::negate). The Janet equivalent
    // of ¬(pattern) and ((pattern) ~ negation) in zelph syntax.
    Janet ScriptEngine::Impl::janet_cfun_zelph_negate(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/negate");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, true);

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, false, res);
            return res;
        }

        Janet res = zelph_wrap_node(s_instance->_n->negate(n));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negate", argc, argv, false, res);
        return res;
    }

    // The rule currently present in the graph that `rule` duplicates, or
    // 0, retrieved via the fingerprint index
    // (Zelph::find_equivalent_rule).
    //
    // A rule in force takes precedence over one merely mentioned in a
    // statement, such as the template associated with a switch next to the
    // copy the switch claimed: the text names both, yet only the claimed one
    // is the rule that fires. A mention is not at all equivalent to a rule
    // being written -- removing the new rule as its duplicate would result in
    // no rule being in force, and a rule typed after a switch that mentions
    // it would never fire. Only when resolving a pattern (via .explain, the
    // .prune commands) does the mention get named, since naming it is the
    // purpose in that context.
    network::Node ScriptEngine::Impl::find_duplicate_rule(const network::Node rule)
    {
        return _n->find_equivalent_rule(rule, _resolving_pattern);
    }

    // A rule built while another one is being built would activate the same
    // scratch cluster, merge it into itself upon completion, and forfeit
    // what the outer build recorded, so the outer rule's parts would never
    // be marked. `zelph/rule*` initiates no build, but when called within
    // that context, it would construct a rule independently, in force by
    // itself rather than as a part of the outer rule. A rule nested in another
    // is part of the outer rule's text, which the outer build writes.
    void ScriptEngine::Impl::refuse_nested_rule(const char* const caller) const
    {
        if (_building_rule)
            janet_panicf("%s: a rule cannot be built while another rule is being built. "
                         "A rule nested in another is a part of the outer rule: write it there as a `=>` fact, "
                         "(zelph/fact condition \"=>\" consequence), or build it before.",
                         caller);
    }

    // The part of a rule build shared among zelph/dedup-rule,
    // zelph/build-rule, the zelph/rule macro, and zelph/rule-text: execute
    // the thunk tasked with writing the rule into the scratch cluster, then
    // make the cluster that was active before -- returned via `previous` --
    // active again. With `build`, the rule is made prior to exiting the
    // scratch, using the output the thunk returned. The scratch retains the
    // content the thunk generated, allowing the caller to read it and
    // subsequently either merge it into `previous` or drop it.
    JanetSignal ScriptEngine::Impl::run_rule_build(const char* const caller, JanetFunction* const thunk, std::string& previous, Janet& out, const RuleBuildStep build)
    {
        refuse_nested_rule(caller);

        previous = _n->active_cluster_name();
        _n->set_active_cluster(kRuleScratch);

        // The thunk builds for the rule a rule STRUCTURE, not a claim -- refer
        // to the revocation in janet_cfun_zelph_fact, which must remain
        // excluded from rule construction or else a second rule mentioning the
        // same ground statement would turn the initial rule's pattern into
        // data. The caller marks the rule's parts within what the thunk
        // created, thus any fact the thunk creates alongside the rule stays
        // unmarked: a claim. A collection it writes is the rule's own, and its
        // identifier says so for good (Zelph::enter_template_scope).
        _building_rule = true;

        JanetSignal signal = JANET_SIGNAL_OK;
        {
            const network::Zelph::TemplateScope scope(*_n);
            signal = pcall_rooted(thunk, 0, nullptr, &out);

            // Here, the step that builds the rule from the value returned by
            // the thunk executes, remaining within the scratch cluster, the
            // template scope, and the build. It throws an exception rather than
            // triggering a Janet panic, which is a longjmp and would bypass the
            // scope's destructor. This exception is treated as the build's
            // error, just as an error from the thunk would be.
            if (signal == JANET_SIGNAL_OK && build != nullptr)
            {
                try
                {
                    const Rooted returned(out);
                    out = build(returned.value);
                }
                catch (const std::exception& ex)
                {
                    out    = janet_cstringv(ex.what());
                    signal = JANET_SIGNAL_ERROR;
                }
                catch (...)
                {
                    out    = janet_cstringv("unknown error");
                    signal = JANET_SIGNAL_ERROR;
                }
            }
        }

        _building_rule = false;

        // A scratch cluster of our own must not swallow the user's: whatever
        // survives is handed back to the cluster that was active, so
        // .cluster-drop still rolls a rule back with the rest of an experiment.
        if (previous.empty())
            _n->deactivate_cluster();
        else
            _n->set_active_cluster(previous);

        // Nothing from a half-built rule is kept
        // (refuse_suspended_rule).
        if (suspended(signal))
        {
            _n->drop_scratch_cluster(kRuleScratch);
            refuse_suspended_rule(caller);
        }

        return signal;
    }

    // Build a rule statement, and keep it only if it says something new.
    //
    // The thunk performs the whole construction -- condition patterns, the
    // conjunction set, the => fact. Running it inside a scratch cluster
    // makes that construction undoable: a cluster records exactly the nodes
    // CREATED while it is active, and every part of an alpha-equivalent
    // rule that is not a variable is hash-consed, hence already present and
    // therefore never recorded. Dropping the scratch removes the second
    // copy and nothing else.
    Janet ScriptEngine::Impl::janet_cfun_zelph_dedup_rule(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/dedup-rule");

        JanetFunction* const thunk = janet_getfunction(argv, 0);

        std::string       previous;
        Janet             out    = janet_wrap_nil();
        const JanetSignal signal = s_instance->run_rule_build("zelph/dedup-rule", thunk, previous, out);

        if (signal != JANET_SIGNAL_OK)
        {
            s_instance->_n->drop_scratch_cluster(kRuleScratch); // see build_marked_rule
            janet_signalv(signal, out);
        }

        const network::Node rule = zelph_unwrap_node(out);
        const network::Node twin = rule ? s_instance->find_duplicate_rule(rule) : 0;

        if (twin == 0)
        {
            // What the scratch cluster recorded is exactly what this statement
            // brought into being -- which is how a GROUND pattern can be told
            // from the same statement asserted earlier. Read it before the
            // merge, which drops the bookkeeping.
            const std::vector<network::Node> created = s_instance->_n->cluster_nodes(kRuleScratch);
            s_instance->_n->merge_cluster(kRuleScratch, previous);
            if (rule) s_instance->_n->mark_rule_patterns(rule, created);
            return out;
        }

        // The scratch drop must not disarm the fact stores: re-entering an
        // existing rule is an ordinary thing to do, and it used to cost the
        // session its genuine-structure store. See drop_scratch_cluster.
        s_instance->_n->drop_scratch_cluster(kRuleScratch);
        return zelph_wrap_node(twin);
    }

    // Build a rule in the manner a parsed rule is constructed, omitting
    // the check for duplication: the build of the zelph/rule macro
    // (zelph/build-rule-parts), and zelph/build-rule, the scope in which a
    // rule authored by hand operates.
    //
    // Before a function executes, its arguments are evaluated, meaning the
    // function cannot know which parts of a rule its call built. Instead, the
    // macro encloses its argument forms within a thunk, which executes here
    // within the scratch cluster. This cluster records exactly what the thunk
    // creates -- a helper it calls included -- and the thunk executes within
    // the template scope, so a collection it writes is the rule's own.
    // Subsequently, the rule's ground parts are marked in the same manner as
    // the parser marks a typed rule's: namely, the parts of each `=>` fact
    // over the rule's condition, which corresponds to the condition set
    // returned by the build (the rule that `build`, the macro's step, made
    // from the parts the thunk returned) or the subject of the `=>` fact
    // that the thunk returns. A rule featuring multiple consequences consists
    // of one `=>` fact for each consequence, yet a thunk that writes them by
    // hand returns only one of them. Any node that predated the call was not
    // created by it and stays what it was.
    Janet ScriptEngine::Impl::build_marked_rule(JanetFunction* const thunk, const RuleBuildStep build)
    {
        std::string       previous;
        Janet             out    = janet_wrap_nil();
        const JanetSignal signal = run_rule_build("zelph/rule", thunk, previous, out, build);

        // A failed build retains nothing that its thunk built, just as a wait
        // already retained nothing. After being merged, a rule that the forms
        // wrote as a `=>` fact on the side was in effect independently, a rule
        // that failed after its first consequence was in force with that one,
        // and the ground conditions of a rule that was never completed turned
        // into data.
        if (signal != JANET_SIGNAL_OK)
        {
            _n->drop_scratch_cluster(kRuleScratch);
            janet_signalv(signal, out);
        }

        // Read before the merge, which drops the accounting
        // records.
        const std::vector<network::Node> created = _n->cluster_nodes(kRuleScratch);
        _n->merge_cluster(kRuleScratch, previous);

        const network::Node result = zelph_unwrap_node(out);
        if (result == 0) return out;

        network::adjacency_set consequences;
        const network::Node    condition = _n->predicate_of(result) == _n->core.Causes
                                             ? _n->parse_fact(result, consequences)
                                             : result;

        for (const network::Node rule : created)
        {
            if (_n->predicate_of(rule) != _n->core.Causes) continue;
            if (rule == result || (condition != 0 && _n->parse_fact(rule, consequences) == condition))
                _n->mark_rule_patterns(rule, created);
        }
        return out;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_build_rule(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/build-rule");

        return s_instance->build_marked_rule(janet_getfunction(argv, 0), nullptr);
    }

    // The C function that the zelph/rule macro expands into. Its thunk
    // evaluates the macro's argument forms and returns them as a single tuple,
    // with conditions appearing first, and the rule is built from this tuple
    // within the build (rule_from_parts). A builder invoked by the thunk would
    // run after the argument forms, originating from the same frame as any
    // call those forms initiate, making it impossible to distinguish the
    // macro's call from any other. No function in the expansion builds a rule
    // outside a build: a program that takes this one via macex1 initiates its
    // own build, which is rejected when attempted within another, just as
    // zelph/rule* is.
    Janet ScriptEngine::Impl::janet_cfun_zelph_build_rule_parts(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/rule");

        return s_instance->build_marked_rule(janet_getfunction(argv, 0), &rule_from_parts);
    }

    // Mark as rule patterns the ground parts of each rule within `created`,
    // which is what a mention brought into existence. Each `=>` fact present
    // there counts, just as it does during a firing (Reasoning::deduce), and
    // not merely the one that the thunk returns: a thunk authored manually
    // might return the statement that names the rule.
    void ScriptEngine::Impl::mark_mentioned_rules(const std::vector<network::Node>& created) const
    {
        for (const network::Node n : created)
            if (_n->predicate_of(n) == _n->core.Causes) _n->mark_rule_patterns(n, created);
    }

    // Write a rule that another statement mentions: the parser wraps every
    // `=>` found inside a different statement in this. Mentioning a rule
    // claims none of its parts. The thunk runs as that of zelph/build-rule
    // does, within the scratch cluster and the template scope, and the ground
    // parts of every rule it created are marked as rule patterns, just as
    // those of a typed rule are: `((a p b) => (c q d)) is noted` answers
    // neither `a p b` nor `c q d`, and the collections the thunk produces
    // belong exclusively to the rule, so `((X p Y) => (X q @{c})) is noted`
    // does not answer `c in @{c}`. A part that was present beforehand retains
    // its prior status. A mentioned rule is marked exclusively as a part of
    // another rule the thunk created; otherwise, it may serve as data and a
    // premise. Inside a parsed statement, the marking is deferred until the
    // statement concludes and is then discarded for a query (ParsedStatement).
    // During the construction of a rule, the enclosing build marks the mention
    // with the rest of its parts.
    Janet ScriptEngine::Impl::janet_cfun_zelph_rule_text(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/rule-text");

        JanetFunction* const thunk = janet_getfunction(argv, 0);
        Janet                out   = janet_wrap_nil();

        if (!s_instance->_building_rule)
        {
            std::string       previous;
            const JanetSignal signal = s_instance->run_rule_build("zelph/rule-text", thunk, previous, out);

            // A mention that does not succeed leaves no trace: if merged, its
            // parts would remain unmarked, as claims that nobody made.
            if (signal != JANET_SIGNAL_OK)
            {
                s_instance->_n->drop_scratch_cluster(kRuleScratch);
                janet_signalv(signal, out);
            }

            // Read before the merge, which drops the accounting
            // records.
            std::vector<network::Node> created = s_instance->_n->cluster_nodes(kRuleScratch);
            s_instance->_n->merge_cluster(kRuleScratch, previous);

            if (s_instance->_statement)
                s_instance->_statement->mentions.push_back(std::move(created));
            else
                s_instance->mark_mentioned_rules(created);
            return out;
        }

        JanetSignal signal = JANET_SIGNAL_OK;
        {
            const network::Zelph::TemplateScope scope(*s_instance->_n);
            signal = pcall_rooted(thunk, 0, nullptr, &out);
            if (suspended(signal)) refuse_suspended_rule("zelph/rule-text");
        }

        if (signal != JANET_SIGNAL_OK) janet_signalv(static_cast<JanetSignal>(signal), out);
        return out;
    }

    // Run the code of a query that carries a condition operator. A query
    // consists of a condition list evaluated once, and what it constructs
    // under an operator is a pattern, exactly as in a rule -- "X go Y, ¬(a p
    // b)" asserted `a p b` and subsequently returned nothing, since the
    // query had constructed its own counterexample. Which of the created
    // nodes stand beneath an operator depends on structure, determined after
    // the statement finishes (Zelph::mark_query_operands); this records what
    // the statement generated, within a scratch cluster that gets merged
    // into the one that was active before it, ensuring that `.cluster-drop`
    // still rolls the query back. A failing query retains none of the
    // structure it built. Outside a parsed statement and inside a rule
    // build, the thunk executes directly.
    Janet ScriptEngine::Impl::janet_cfun_zelph_query_text(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/query-text");

        JanetFunction* const thunk  = janet_getfunction(argv, 0);
        Janet                out    = janet_wrap_nil();
        JanetSignal          signal = JANET_SIGNAL_OK;

        if (s_instance->_statement == nullptr || s_instance->_building_rule)
        {
            signal = pcall_rooted(thunk, 0, nullptr, &out);
            if (suspended(signal)) refuse_suspended("zelph/query-text: a statement's code cannot wait for the event loop or yield.");
            if (signal != JANET_SIGNAL_OK) janet_signalv(signal, out);
            return out;
        }

        {
            const std::string previous = s_instance->_n->active_cluster_name();
            s_instance->_n->set_active_cluster(kQueryScratch);

            signal = pcall_rooted(thunk, 0, nullptr, &out);

            if (previous.empty())
                s_instance->_n->deactivate_cluster();
            else
                s_instance->_n->set_active_cluster(previous);

            if (signal == JANET_SIGNAL_OK)
            {
                // Read before the merge, which drops the accounting records.
                std::vector<network::Node> created = s_instance->_n->cluster_nodes(kQueryScratch);
                s_instance->_n->merge_cluster(kQueryScratch, previous);
                s_instance->_statement->query_created.insert(s_instance->_statement->query_created.end(), created.begin(), created.end());
                return out;
            }

            s_instance->_n->drop_scratch_cluster(kQueryScratch);
        }

        if (suspended(signal)) refuse_suspended("zelph/query-text: a statement's code cannot wait for the event loop or yield.");
        janet_signalv(signal, out);
    }

    // Run the code that builds a condition operator -- `¬`, `≈`, `⁺`/`∗`,
    // or any of these expressed explicitly as the fact they generate. In
    // both a rule and a query, its operand is a pattern, not a claim, thus
    // it is LOOKED UP wherever the graph holds it (_resolving_pattern):
    // following "¬(a p b)", the rule "(X go Y, ¬(a p b)) => (X ok Y)" and
    // the query "X go Y, ¬(a p b)" both failed with "this fact is known to
    // be wrong", even though a refuted fact is precisely one that does not
    // hold. What the graph does not hold is built as before.
    Janet ScriptEngine::Impl::janet_cfun_zelph_operand(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/operand");

        JanetFunction* const thunk  = janet_getfunction(argv, 0);
        Janet                out    = janet_wrap_nil();
        JanetSignal          signal = JANET_SIGNAL_OK;
        {
            const BlockScope looking_up(s_instance->_resolving_pattern, true);
            signal = pcall_rooted(thunk, 0, nullptr, &out);
        }

        if (suspended(signal)) refuse_suspended("zelph/operand: a condition cannot wait for the event loop or yield.");
        if (signal != JANET_SIGNAL_OK) janet_signalv(signal, out);
        return out;
    }

    // Create a complete inference rule: conjunction of conditions => consequence(s).
    // First argument: array or tuple of condition fact nodes.
    // Remaining arguments: one or more consequence fact nodes.
    // Returns the condition set node (the rule's identity in the graph).
    //
    // Equivalent zelph syntax:
    //   (*{cond1 cond2 ...} ~ conjunction) => consequence1
    //   (*{cond1 cond2 ...} ~ conjunction) => consequence2
    //
    // Janet usage:
    //   (zelph/rule* [cond1 cond2] consequence1 consequence2)
    //
    // The builder behind zelph/rule* and the zelph/rule macro. Invoked via
    // zelph/rule*, it receives its arguments already constructed, thus it
    // marks nothing, and any collection within them is a value. Within the
    // macro, execution occurs inside a rule's build, where a Janet panic would
    // longjmp past the template scope's destructor, hence it throws. Each
    // argument is validated before any writing: a consequence refused after
    // the first one had been written left the rule in force with that one. An
    // identifier that refers to no node within the graph is not a node
    // either: it may be a number, or a node that a rolled-back build
    // generated and that a Janet binding still holds. Such an identifier
    // passed the check but failed solely during the rule's writing phase,
    // once its condition set had already been established.
    Janet ScriptEngine::Impl::make_rule(const int32_t argc, const Janet* const argv)
    {
        // No fewer than conditions plus 1 consequence, as stated in the words
        // of janet_arity.
        if (argc < 2) throw std::runtime_error("arity mismatch, expected at least 2, got " + std::to_string(argc));
        if (_log_janet_functions) log_janet_call("zelph/rule*", argc, const_cast<Janet*>(argv), true);

        // First argument: indexed collection of condition fact nodes
        const Janet* cond_data;
        int32_t      cond_len;
        if (!janet_indexed_view(argv[0], &cond_data, &cond_len) || cond_len == 0)
            throw std::runtime_error("zelph/rule: first argument must be a non-empty array or tuple of conditions");

        // Collect condition nodes
        std::unordered_set<network::Node> condition_nodes;
        for (int32_t i = 0; i < cond_len; ++i)
        {
            network::Node n = zelph_unwrap_node(cond_data[i]);
            if (!n || !_n->exists(n))
                throw std::runtime_error("zelph/rule: condition at index " + std::to_string(i) + " is not a valid zelph/node");

            // Same reason as in zelph/conjunction: a condition that is not a
            // pattern makes the rule inert, and a generator that builds one
            // has no other way to find out.
            if (!is_condition_pattern(n))
                throw std::runtime_error("zelph/rule: condition at index " + std::to_string(i) + " is \"" + _n->format(n)
                                         + "\", which is not a fact pattern and can never match");

            condition_nodes.insert(n);
        }

        std::vector<network::Node> consequences;
        for (int32_t i = 1; i < argc; ++i)
        {
            const network::Node consequence = zelph_unwrap_node(argv[i]);
            if (!consequence || !_n->exists(consequence))
                throw std::runtime_error("zelph/rule: consequence at index " + std::to_string(i - 1) + " is not a valid zelph/node");
            consequences.push_back(consequence);
        }

        if (condition_nodes.empty())
        {
            Janet res = janet_wrap_nil();
            if (_log_janet_functions) log_janet_call("zelph/rule*", argc, const_cast<Janet*>(argv), false, res);
            return res;
        }

        // Create condition set and mark as conjunction
        network::Node condition_set = _n->conjunction_collection(condition_nodes);
        _n->fact(condition_set, _n->core.IsA, {_n->core.Conjunction});

        // Link each consequence via =>
        for (const network::Node consequence : consequences)
            _n->fact(condition_set, _n->core.Causes, {consequence});

        Janet res = zelph_wrap_node(condition_set);
        if (_log_janet_functions) log_janet_call("zelph/rule*", argc, const_cast<Janet*>(argv), false, res);
        return res;
    }

    // The step in the zelph/rule macro's build (run_rule_build): the thunk
    // returns the macro's arguments as a single tuple, and the rule is made
    // from it while the build remains active.
    Janet ScriptEngine::Impl::rule_from_parts(const Janet parts)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(parts, &data, &len))
            throw std::runtime_error("zelph/rule: the forms did not return the rule's conditions and consequences");
        return s_instance->make_rule(len, data);
    }

    // `zelph/rule*`, refused while a rule is being built
    // (`refuse_nested_rule`).
    Janet ScriptEngine::Impl::janet_cfun_zelph_rule_star(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/rule*");
        s_instance->refuse_nested_rule("zelph/rule*");
        return s_instance->make_rule(argc, argv);
    }

    // Build a cons list from string characters (for compact <abc> syntax).
    // Characters are reversed before list construction so that the last (rightmost)
    // character — the least significant digit in a numeric string — becomes the
    // outermost cons cell. This matches the node-list syntax where the user writes
    // digits in reverse order: <3 2 1> and <123> produce the same internal structure.
    Janet ScriptEngine::Impl::janet_cfun_zelph_list_chars(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/list-chars");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list-chars", argc, argv, true);

        const uint8_t* str   = janet_getstring(argv, 0);
        std::string    raw_s = reinterpret_cast<const char*>(str);

        if (raw_s.empty())
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list-chars", argc, argv, false, res);
            return res; // Empty lists are not supported
        }

        // Split into individual characters, then reverse so the rightmost character
        // (least significant digit) becomes element[0] and thus the outermost cons cell.
        // Example: "123" -> ['3','2','1'] -> list builds 3 cons (2 cons (1 cons nil))
        // This matches the node-list syntax where the user writes <3 2 1> for the number 123.
        std::vector<std::string> elements;
        string::for_each_codepoint(raw_s, [&](const std::string& cp)
                                   { elements.push_back(cp); });
        std::reverse(elements.begin(), elements.end());

        network::Node list_node = s_instance->_n->list(elements);
        Janet         res       = zelph_wrap_node(list_node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list-chars", argc, argv, false, res);
        return res;
    }

    // Build a cons list from existing nodes (for < A B > node-list syntax).
    // The first node in the input becomes the outermost cons cell (= head of the cons list).
    // For numbers, write digits in reverse so that the LSB comes first:
    // <3 2 1> gives 3 as the outermost car (= LSB of "123"), matching the internal
    // structure of the compact <123> syntax.
    Janet ScriptEngine::Impl::janet_cfun_zelph_list(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/list");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list", argc, argv, true);

        std::vector<network::Node> elements;
        elements.reserve(argc);

        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.push_back(n);
        }

        // The empty cons list IS nil -- the same node zelph/set returns for
        // the empty set, and the terminator every non-empty list ends at.
        // Returning Janet's nil instead made `<>` evaluate to nothing at
        // all, so a statement containing it was silently dropped.
        if (elements.empty())
        {
            Janet res = zelph_wrap_node(s_instance->_n->core.Nil);
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list", argc, argv, false, res);
            return res;
        }

        network::Node list_node = s_instance->_n->list(elements);
        Janet         res       = zelph_wrap_node(list_node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/list", argc, argv, false, res);
        return res;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_set(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/set");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set", argc, argv, true);

        std::unordered_set<network::Node> elements;
        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.insert(n);
        }

        network::Node set_node = s_instance->_n->set(elements);
        Janet         res      = zelph_wrap_node(set_node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set", argc, argv, false, res);
        return res;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_collection(int32_t argc, Janet* argv)
    {
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/collection");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/collection", argc, argv, true);

        std::unordered_set<network::Node> elements;
        for (int i = 0; i < argc; ++i)
        {
            network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (n) elements.insert(n);
        }

        network::Node node = s_instance->_n->collection(elements);
        if (node && s_instance->_statement && !s_instance->_building_rule) s_instance->_statement->collections.push_back(node);
        Janet res = zelph_wrap_node(node);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/collection", argc, argv, false, res);
        return res;
    }

    // Is `n` something a rule may hold as a condition?
    //
    // Unification matches a PATTERN, so a condition that carries no statement
    // matches nothing and the rule containing it can never fire. A nested
    // condition set is the one member that is not itself a fact: the
    // evaluator descends into it.
    bool ScriptEngine::Impl::is_condition_pattern(const network::Node n) const
    {
        if (n == 0) return false;
        if (_n->predicate_of(n) != 0) return true;
        return _n->check_fact(n, _n->core.IsA, {_n->core.Conjunction}).is_known();
    }

    // Every member of a set that has just been tagged `~ conjunction`.
    void ScriptEngine::Impl::check_conditions_are_patterns(const network::Node set) const
    {
        network::adjacency_set members;
        if (!_n->condition_set_members(set, members)) return;

        for (const network::Node m : members)
        {
            if (is_condition_pattern(m)) continue;

            const std::string offender = _n->format(m);
            janet_panicf("\"%s\" is a condition of this rule but not a statement, so the rule can never match", offender.c_str());
        }
    }

    // Build a rule's condition set from the `(cond, cond, ...)` comma list:
    // a collection tagged `~ conjunction`.
    //
    // A member that is not a fact pattern used to be taken as it came, and
    // the resulting rule was inert -- accepted, listed, and unable to fire.
    // The focus operator is the way to write one by accident, because it does
    // exactly what it promises: `(*A p C, C q b)` evaluates its first member
    // to the node A, so the rule's conditions are the node A and one fact.
    // A focus one level down stays legitimate -- `((*A p C) q b, ...)` is the
    // condition `A q b` with a second fact built on the side -- which is why
    // the test is what the member EVALUATES to rather than how it is written.
    Janet ScriptEngine::Impl::janet_cfun_zelph_conjunction(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, -1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/conjunction");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/conjunction", argc, argv, true);

        std::unordered_set<network::Node> conditions;
        for (int32_t i = 0; i < argc; ++i)
        {
            const network::Node n = s_instance->resolve_janet_arg(argv[i]);
            if (!n) janet_panicf("zelph/conjunction: condition %d is not a node", i + 1);

            if (!s_instance->is_condition_pattern(n))
                janet_panicf("condition %d of the comma list is \"%s\", which is not a statement and can never match. "
                             "A focus makes its statement evaluate to the focused node, so a condition written "
                             "\"*A p C\" is the node A -- write it \"A p C\" instead.",
                             i + 1,
                             s_instance->_n->format(n).c_str());

            conditions.insert(n);
        }

        const network::Node set = s_instance->_n->conjunction_collection(conditions);
        s_instance->_n->fact(set, s_instance->_n->core.IsA, {s_instance->_n->core.Conjunction});

        Janet res = zelph_wrap_node(set);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/conjunction", argc, argv, false, res);
        return res;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_fact(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/fact");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, true);

        network::Node s = s_instance->resolve_janet_arg(argv[0]);
        network::Node p = s_instance->resolve_janet_arg(argv[1]);
        if (!s || !p)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        for (int i = 2; i < argc; ++i)
        {
            network::Node o = s_instance->resolve_janet_arg(argv[i]);
            if (o) objs.insert(o);
        }
        if (objs.empty())
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
            return res;
        }

        // Resolving a printed pattern is not claiming it: look the fact up,
        // and answer with the node the graph HAS. Only when the graph has none
        // -- a pattern carrying variables, or a ground one nobody entered --
        // does the construction below run, and then there is nothing to
        // contradict; the scratch cluster the caller holds rolls it back.
        //
        // Asked as is_known, not for the id check_fact hands back either way.
        // That id is the hash of the triple and is meaningful for a fact the
        // graph does NOT hold -- it is what lets .node say "Unknown node"
        // rather than invent one -- but it comes with no edges under it, and a
        // pattern is more than an id to anything that has to MATCH with it:
        // answering with it gave the prune commands a bare number for
        // "(S p O)", which unification then printed as "??" and matched
        // nothing. A caller that wants the id of an absent pattern still gets
        // it, from the construction below, which produces the same hash inside
        // the scratch cluster the caller holds.
        //
        // Exactly the fact that was asked for, too. A WIDER one -- "a p b c"
        // when "a p b" was named -- is what unification matches, and the proof
        // search asks for it in those terms itself (resolve_pattern's
        // `containing` flag); a command that names a pattern must not silently
        // resolve to a fact carrying an object it was not told about, least of
        // all a destructive one.
        if (s_instance->_resolving_pattern)
        {
            const network::Answer known = s_instance->_n->check_fact(s, p, objs);

            if (const network::Node found = known.is_known() ? known.relation() : network::Node{0}; found != 0)
            {
                Janet res = zelph_wrap_node(found);
                if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
                return res;
            }
        }

        network::Node f = s_instance->_n->fact(s, p, objs);

        // The tag is what MAKES a container a rule's condition set, so this is
        // where the explicit spelling `(*{cond cond} ~ conjunction)` says what
        // the comma list says -- and it has to be asked here, because the tag
        // is the only thing that tells a set of conditions from a set of
        // anything else. Asked after the fact exists: condition_set_members
        // reads the tag, and several members mean nothing without it.
        if (f && p == s_instance->_n->core.IsA && objs.count(s_instance->_n->core.Conjunction) == 1)
            s_instance->check_conditions_are_patterns(s);

        // zelph/fact IS the assertion API, so calling it is a CLAIM and
        // revokes the pattern-only status the same statement may have
        // acquired by appearing in a rule -- exactly as a typed statement
        // does. Without this, a ground rule condition asserted from Janet
        // stayed invisible to unification and the rule never fired, while
        // zelph/exists still answered true off the rule's own pattern.
        // Only in a user's Janet block, and not while a rule is being
        // built; see _in_janet_block and _building_rule.
        //
        // The facts nested in it are likewise claimed, outside a `=>`, just
        // as a typed statement claims them. Every nested fact that a
        // `zelph/fact` call in the block built was claimed by that call, and
        // one provided by reference was claimed once it turned into data, so
        // only a node still marked -- one obtained from a rule's text -- is
        // entered. A set constant or a collection handed in is not
        // traversed: a marked fact among its members remains a pattern.
        if (f && s_instance->_in_janet_block && !s_instance->_building_rule)
        {
            s_instance->_n->unmark_rule_pattern(f);
            claim_marked_parts(*s_instance->_n, f, s, p, objs);
        }

        Janet res = zelph_wrap_node(f);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/fact", argc, argv, false, res);
        return res;
    }

    // The claim that a fact does NOT hold, which is what `¬(F)` says when it
    // stands on its own line rather than in a rule condition.
    //
    // Same arguments and the same node identity as zelph/fact -- what differs
    // is the probability the fact is created with. zelph has always been able
    // to hold a fact as known-wrong (Answer::is_wrong, and the two refusals in
    // Zelph::fact that keep a graph from claiming both), and that mechanism is
    // what a top-level negation now reaches. It had no spelling before, which
    // is why `¬(a p b)` ASSERTED `a p b`: the operand was built by zelph/fact
    // before the tag was applied to it.
    //
    // Asserting the opposite of something the graph already claims is refused
    // by Zelph::fact rather than silently overwritten, in both directions.
    Janet ScriptEngine::Impl::janet_cfun_zelph_refute(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, -1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/refute");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, true);

        network::Node s = s_instance->resolve_janet_arg(argv[0]);
        network::Node p = s_instance->resolve_janet_arg(argv[1]);
        if (!s || !p)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, false, res);
            return res;
        }

        network::adjacency_set objs;
        for (int i = 2; i < argc; ++i)
        {
            network::Node o = s_instance->resolve_janet_arg(argv[i]);
            if (o) objs.insert(o);
        }
        if (objs.empty())
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, false, res);
            return res;
        }

        network::Node f = s_instance->_n->fact(s, p, objs, 0.0L);

        // A refutation is a claim about the fact, so it revokes pattern-only
        // status exactly as an assertion does -- the reasoning in
        // janet_cfun_zelph_fact applies unchanged.
        if (f && s_instance->_in_janet_block && !s_instance->_building_rule)
        {
            s_instance->_n->unmark_rule_pattern(f);
            claim_marked_parts(*s_instance->_n, f, s, p, objs);
        }

        // The probability says what the graph believes; this is what the read
        // surface consults, because a fact's probability sits on an edge and
        // asking for it per candidate would be a lock on the hot path.
        if (f) s_instance->_n->mark_refuted_fact(f);

        Janet res = zelph_wrap_node(f);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/refute", argc, argv, false, res);
        return res;
    }

    // Execute a query: print the pattern and trigger matching via apply_rule.
    // This is the Janet equivalent of entering a zelph statement that contains
    // variables (e.g. "X ~ human"). Takes a single zelph/node argument
    // (typically the return value of a zelph/fact call containing variables).
    Janet ScriptEngine::Impl::janet_cfun_zelph_query(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        require_main_thread("zelph/query");
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/query", argc, argv, true);

        // A query resets the variable scope (below). Within the argument
        // forms of a rule, that scope binds the rule's variables, and a
        // symbol that appeared after the query was transformed into a fresh
        // variable: the X and Y in the consequence were no longer those of
        // the condition, and the rule derived `?? q ??`.
        refuse_inside_rule_build("zelph/query");

        network::Node n = zelph_unwrap_node(argv[0]);
        if (!n)
        {
            Janet res = janet_wrap_nil();
            if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/query", argc, argv, false, res);
            return res;
        }

        // Build inverse mapping: variable Node -> symbol name
        // (must be done before apply_rule clears anything)
        std::map<network::Node, std::string> var_to_name;
        {
            std::lock_guard<std::mutex> lock(s_instance->_state_mutex);
            for (const auto& [name, node] : s_instance->_scoped_variables)
            {
                var_to_name[node] = name;
            }
        }

        // Collect results instead of printing them.
        //
        // This used to run only when the CURRENT statement had created scoped
        // variables, which tied a query to the expression that built its
        // pattern: storing a pattern in a Janet binding and querying it later
        // -- or simply querying the same pattern twice -- silently returned an
        // empty array, indistinguishable from "no matches". The scope is not
        // needed to run the query at all, only to label the bindings, and
        // resolve_janet_arg names every variable node it creates, so the names
        // can be recovered from the graph instead (see below).
        std::vector<std::shared_ptr<network::Variables>> results;

        s_instance->_n->set_query_collector(&results);
        s_instance->_n->apply_rule(0, n);
        s_instance->_n->set_query_collector(nullptr);

        // Reset variable scope for the next query/statement
        s_instance->clear_scoped_variables();

        // Convert results to Janet array of tables:
        // @[@{X <zelph/node ...> Y <zelph/node ...>} ...]
        JanetArray* result_array = janet_array(static_cast<int32_t>(results.size()));

        for (const auto& vars : results)
        {
            JanetTable* entry = janet_table(static_cast<int32_t>(vars->size()));

            for (const auto& [var_node, bound_node] : *vars)
            {
                // Prefer the name the current statement used; fall back to the
                // name the variable node carries in the graph, which is what
                // makes a pattern built in an earlier expression usable.
                auto        it = var_to_name.find(var_node);
                std::string key_name =
                    (it != var_to_name.end())
                        ? it->second
                        : s_instance->_n->get_name(var_node, s_instance->_n->lang(), true);

                if (key_name.empty()) continue;

                Janet key = janet_wrap_symbol(janet_symbol(
                    reinterpret_cast<const uint8_t*>(key_name.c_str()),
                    static_cast<int32_t>(key_name.size())));
                Janet val = zelph_wrap_node(bound_node);
                janet_table_put(entry, key, val);
            }

            janet_array_push(result_array, janet_wrap_table(entry));
        }

        Janet res = janet_wrap_array(result_array);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/query", argc, argv, false, res);
        return res;
    }
}
