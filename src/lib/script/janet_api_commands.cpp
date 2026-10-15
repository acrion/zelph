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

#include <janet.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zelph
{
    // All operations that write the graph, build a rule, or change the engine's
    // state are confined to the main thread -- the one responsible for hosting the
    // Janet VM where the REPL, scripts, and parsed statements run. A thread
    // initiated via `ev/thread`, `ev/spawn-thread`, or `ev/do-thread` possesses
    // its own VM, yet invokes identical functions on the shared engine, and the
    // state underlying rule builds, statement processing, and scratch clusters is
    // singular per process. In earlier versions, writes stemming from that
    // thread were lost when the main thread dropped a scratch cluster, a correct
    // rule was rejected as nested, a claim disappeared, and the main thread's
    // active cluster was reset. A thread may read the graph and use a compiled
    // network; any computation it performs is passed to the main thread, which
    // performs the actual writing.
    void ScriptEngine::Impl::require_main_thread(const char* const name)
    {
        if (std::this_thread::get_id() != s_instance->_main_thread_id)
            janet_panicf("%s: main thread only. A thread started with ev/thread, ev/spawn-thread or ev/do-thread "
                         "may read the graph and use a compiled network, but writing the graph, building a rule and "
                         "everything else that changes the engine belong to the main thread: hand the thread's result "
                         "to the main thread (a thread channel) and write it there.",
                         name);
    }

    // The forms that write a rule operate within its build: inside the rule's
    // scratch cluster, which records what is the rule's own. Switching the
    // cluster at that point causes the rule's ground components to become
    // data, and the switch concludes silently once the build reverts to the
    // cluster it originally detected. Dropping a cluster, importing a file,
    // loading a network, or running the engine changes the graph beneath a
    // rule that is still being constructed, and saving the network writes the
    // incomplete rule in the file as data. Each of these operations is
    // rejected at that stage.
    //
    // Only the main thread constructs a rule, and each caller checks that it
    // runs on the main thread before it makes this request, thus the build in
    // progress always includes the caller's own forms.
    void ScriptEngine::Impl::refuse_inside_rule_build(const char* const name)
    {
        if (s_instance->_building_rule)
            janet_panicf("%s: refused while a rule is being built. "
                         "Switch or drop a cluster, import a file, load or save a network, run the engine or query the graph "
                         "before the rule is built or after it.",
                         name);
    }

    // Load and execute a script via the .import machinery. This is the way
    // to pull .zph files (facts, rules, arithmetic definitions) into the
    // network from Janet code.
    //
    // .janet files are rejected: run_janet_script drives janet_loop, and a
    // nested janet_loop (script importing a script) is not supported by
    // Janet - and Janet's own module system is the right tool for that job.
    //
    // Main thread only: the import pipeline executes Janet code in the main
    // VM (_janet_env), which must not be entered from other Janet threads.
    Janet ScriptEngine::Impl::janet_cfun_zelph_import(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/import", argc, argv, true);

        require_main_thread("zelph/import");

        refuse_inside_rule_build("zelph/import");

        if (!s_instance->_import_handler)
            janet_panicf("zelph/import: no import handler registered (script engine not fully initialized)");

        const std::string path = reinterpret_cast<const char*>(janet_getstring(argv, 0));

        if (std::filesystem::path(path).extension() == ".janet")
            janet_panicf("zelph/import: .janet files are not importable this way - use Janet's own (import ...), (use ...) or (dofile ...) instead");

        std::vector<std::string> args;
        args.reserve(static_cast<size_t>(argc) - 1);
        for (int32_t i = 1; i < argc; ++i)
            args.emplace_back(reinterpret_cast<const char*>(janet_getstring(argv, i)));

        std::string err;
        try
        {
            s_instance->_import_handler(path, args);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/import: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Shared implementation for zelph/save and zelph/load. Both delegate to
    // the corresponding REPL command (".save"/".load") via the command
    // handler, so they share every check and side effect with the
    // interactive commands: extension validation, partial-load guard,
    // auto-run handling, format detection (.bin vs. Wikidata JSON), and
    // timing diagnostics.
    //
    // Main thread only: the commands manipulate REPL state (auto_run,
    // partial_load_mode), which is owned by the main thread and not
    // synchronized.
    Janet ScriptEngine::Impl::command_impl(int32_t argc, Janet* argv, const char* name, const char* command)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        require_main_thread(name);

        refuse_inside_rule_build(name);

        if (!s_instance->_command_handler)
            janet_panicf("%s: no command handler registered (script engine not fully initialized)", name);

        const std::string file = reinterpret_cast<const char*>(janet_getstring(argv, 0));

        std::string err;
        try
        {
            s_instance->_command_handler({command, file});
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("%s: %s", name, err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_save(int32_t argc, Janet* argv)
    {
        return command_impl(argc, argv, "zelph/save", ".save");
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_load(int32_t argc, Janet* argv)
    {
        return command_impl(argc, argv, "zelph/load", ".load");
    }

    // The run commands, delegated in the way command_impl delegates the
    // others, accept a single optional parameter, `:quiet`. Omitting it
    // results in the invocation being treated as the command and yielding
    // nil. Including it causes the run to print nothing and instead return
    // what it met as data (quiet_run_result). Any other argument is
    // rejected, just as the command itself refuses one.
    //
    // `:derived`, whether placed before or after `:quiet`, incorporates
    // the facts the run derived into that data. If `:quiet` is absent,
    // the call returns nil, meaning the facts would be collected for
    // nobody, and the call is refused.
    Janet ScriptEngine::Impl::run_command_impl(int32_t argc, Janet* argv, const char* name, const char* command)
    {
        janet_arity(argc, 0, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        require_main_thread(name);

        refuse_inside_rule_build(name);

        bool quiet   = false;
        bool derived = false;
        for (int32_t i = 0; i < argc; ++i)
        {
            bool* const flag = janet_keyeq(argv[i], "quiet") ? &quiet : janet_keyeq(argv[i], "derived") ? &derived
                                                                                                        : nullptr;
            if (flag == nullptr || *flag) janet_panicf("%s: the arguments are :quiet and :derived", name);
            *flag = true;
        }
        if (derived && !quiet) janet_panicf("%s: :derived needs :quiet", name);

        if (!s_instance->_command_handler)
            janet_panicf("%s: no command handler registered (script engine not fully initialized)", name);

        std::string err;
        bool        failed = false;
        try
        {
            if (derived)
                s_instance->_command_handler({command, "quiet", "derived"});
            else if (quiet)
                s_instance->_command_handler({command, "quiet"});
            else
                s_instance->_command_handler({command});
        }
        catch (const std::exception& e)
        {
            err    = e.what();
            failed = true;
        }
        if (failed) janet_panicf("%s: %s", name, err.c_str());

        return quiet ? quiet_run_result(derived) : janet_wrap_nil();
    }

    // What a quiet run met, retrieved directly from the engine after it:
    // {:contradictions C :naf-final N}. C contains a struct for each
    // contradiction (Reasoning::met_contradictions), N holds a {:fact F
    // :rule R} for every fact the negation check identified as unsupported
    // (defeated_negations), listed in the order the check lists them. Only
    // Janet values are constructed at this stage, so nothing can panic
    // while a C++ container remains alive.
    Janet ScriptEngine::Impl::quiet_run_result(const bool derived)
    {
        network::Reasoning& n = *s_instance->_n;

        const auto& met            = n.met_contradictions();
        Janet*      contradictions = janet_tuple_begin(static_cast<int32_t>(met.size()));
        for (std::size_t i = 0; i < met.size(); ++i)
        {
            const network::Reasoning::MetContradiction& m = met[i];

            Janet* conditions = janet_tuple_begin(static_cast<int32_t>(m.conditions.size()));
            for (std::size_t c = 0; c < m.conditions.size(); ++c)
                conditions[c] = zelph_wrap_node(m.conditions[c]);

            JanetTable* bindings = janet_table(static_cast<int32_t>(m.bindings.size()));
            for (const auto& [variable, value] : m.bindings)
                janet_table_put(bindings, zelph_wrap_node(variable), zelph_wrap_node(value));

            const char* record = m.record == network::Reasoning::ContradictionRecord::New      ? "new"
                               : m.record == network::Reasoning::ContradictionRecord::Existing ? "existing"
                                                                                               : "none";

            // A consequence the engine refused to build carries no
            // :conclusion, and :refused indicates the reason, just as the
            // loud run's "refused:" line does.
            JanetKV* entry = janet_struct_begin(4 + (m.conclusion != 0 ? 1 : 0) + (m.refusal.empty() ? 0 : 1));
            janet_struct_put(entry, janet_ckeywordv("rule"), zelph_wrap_node(m.rule));
            janet_struct_put(entry, janet_ckeywordv("conditions"), janet_wrap_tuple(janet_tuple_end(conditions)));
            janet_struct_put(entry, janet_ckeywordv("conclusion"), m.conclusion != 0 ? zelph_wrap_node(m.conclusion) : janet_wrap_nil());
            janet_struct_put(entry, janet_ckeywordv("record"), janet_ckeywordv(record));
            janet_struct_put(entry, janet_ckeywordv("bindings"), janet_wrap_table(bindings));
            if (!m.refusal.empty()) janet_struct_put(entry, janet_ckeywordv("refused"), janet_cstringv(m.refusal.c_str()));
            contradictions[i] = janet_wrap_struct(janet_struct_end(entry));
        }

        const auto& defeated  = n.defeated_negations();
        Janet*      naf_final = janet_tuple_begin(static_cast<int32_t>(defeated.size()));
        for (std::size_t i = 0; i < defeated.size(); ++i)
        {
            JanetKV* entry = janet_struct_begin(2);
            janet_struct_put(entry, janet_ckeywordv("fact"), zelph_wrap_node(defeated[i].first));
            janet_struct_put(entry, janet_ckeywordv("rule"), zelph_wrap_node(defeated[i].second));
            naf_final[i] = janet_wrap_struct(janet_struct_end(entry));
        }

        // Using :derived, {:derived D} likewise: a distinct struct for
        // every fact the run derived (Reasoning::DerivedFact), in the
        // sequence of derivation.
        Janet derivations = janet_wrap_nil();
        if (derived)
        {
            const auto nodes = [](const std::vector<network::Node>& from)
            {
                Janet* to = janet_tuple_begin(static_cast<int32_t>(from.size()));
                for (std::size_t i = 0; i < from.size(); ++i)
                    to[i] = zelph_wrap_node(from[i]);
                return janet_wrap_tuple(janet_tuple_end(to));
            };

            const std::vector<network::Reasoning::DerivedFact> facts = n.take_derived_facts();
            Janet*                                             d     = janet_tuple_begin(static_cast<int32_t>(facts.size()));
            for (std::size_t i = 0; i < facts.size(); ++i)
            {
                const network::Reasoning::DerivedFact& f = facts[i];

                JanetTable* bindings = janet_table(static_cast<int32_t>(f.bindings.size()));
                for (const auto& [variable, value] : f.bindings)
                    janet_table_put(bindings, zelph_wrap_node(variable), zelph_wrap_node(value));

                JanetKV* entry = janet_struct_begin(5);
                janet_struct_put(entry, janet_ckeywordv("fact"), zelph_wrap_node(f.fact));
                janet_struct_put(entry, janet_ckeywordv("rule"), zelph_wrap_node(f.rule));
                janet_struct_put(entry, janet_ckeywordv("premises"), nodes(f.premises));
                janet_struct_put(entry, janet_ckeywordv("absent"), nodes(f.absent));
                janet_struct_put(entry, janet_ckeywordv("bindings"), janet_wrap_table(bindings));
                d[i] = janet_wrap_struct(janet_struct_end(entry));
            }
            derivations = janet_wrap_tuple(janet_tuple_end(d));
        }

        JanetKV* result = janet_struct_begin(derived ? 3 : 2);
        janet_struct_put(result, janet_ckeywordv("contradictions"), janet_wrap_tuple(janet_tuple_end(contradictions)));
        janet_struct_put(result, janet_ckeywordv("naf-final"), janet_wrap_tuple(janet_tuple_end(naf_final)));
        if (derived) janet_struct_put(result, janet_ckeywordv("derived"), derivations);
        return janet_wrap_struct(janet_struct_end(result));
    }

    // Run the inference engine. Without this, a program that drives zelph as
    // a library can assert facts and define rules from Janet but cannot ask
    // for their consequences: forward chaining is otherwise reached only
    // through the .run command or through auto-run at the end of a Janet
    // block, neither of which exists outside the REPL.
    Janet ScriptEngine::Impl::janet_cfun_zelph_run(int32_t argc, Janet* argv)
    {
        return run_command_impl(argc, argv, "zelph/run", ".run");
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_run_once(int32_t argc, Janet* argv)
    {
        return run_command_impl(argc, argv, "zelph/run-once", ".run-once");
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_run_delta(int32_t argc, Janet* argv)
    {
        return run_command_impl(argc, argv, "zelph/run-delta", ".run-delta");
    }

    // Whether the host performs inference following input processing: the
    // effective `.auto-run`, which a module import disables until the
    // module concludes. A keyword handler that runs inference
    // independently consults this to follow the session's setting.
    //
    // Main thread only: the value resides within the REPL state, which
    // the main thread owns and changes without synchronization.
    Janet ScriptEngine::Impl::janet_cfun_zelph_auto_run_p(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_false();
        require_main_thread("zelph/auto-run?");

        const bool on = s_instance->_auto_run_predicate && s_instance->_auto_run_predicate();
        return janet_wrap_boolean(on ? 1 : 0);
    }

    // The negation levels of the active rules, as data, as .strata prints
    // them. For each rule: its level, counted from 1 as .strata counts it
    // and omitted for a rule lacking a negated condition; its component;
    // and whether that component is stratifiable. A front end that writes
    // rules can thus refuse a program based on the engine's own verdict
    // rather than a second analysis, and every rule currently in effect is
    // counted, not just the rules it authored.
    //
    // Only on the main thread: the analysis updates the engine's state
    // that a run reads (Reasoning::negation_levels).
    Janet ScriptEngine::Impl::janet_cfun_zelph_strata(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/strata", argc, argv, true);

        require_main_thread("zelph/strata");

        const network::Reasoning::NegationLevels strata = s_instance->_n->strata();
        JanetArray*                              out    = janet_array(static_cast<int32_t>(strata.rules.size()));
        for (std::size_t i = 0; i < strata.rules.size(); ++i)
        {
            // A `=>` fact lacking the form of a rule, like the declaration that
            // `=>` denotes a relation, is omitted, just as .list-rules omits it.
            if (!s_instance->_n->has_rule_form(strata.rules[i])) continue;

            const std::size_t component = strata.component[i];
            JanetTable*       entry     = janet_table(4);
            janet_table_put(entry, janet_ckeywordv("rule"), zelph_wrap_node(strata.rules[i]));
            if (strata.negates[i]) janet_table_put(entry, janet_ckeywordv("level"), janet_wrap_integer(static_cast<int32_t>(strata.level[i] + 1)));
            janet_table_put(entry, janet_ckeywordv("component"), janet_wrap_integer(static_cast<int32_t>(component)));
            janet_table_put(entry, janet_ckeywordv("stratifiable"), janet_wrap_boolean(strata.negation_inside[component] ? 0 : 1));
            // A rule over predicates merely extends what it reads, much like
            // transitivity applied to a predicate variable. As long as one
            // remains in force, the levels compare predicates alone, not
            // predicate and object (negation_levels), and any caller relying
            // on the verdict must be aware of this.
            if (strata.transparent[i]) janet_table_put(entry, janet_ckeywordv("transparent"), janet_wrap_true());
            janet_array_push(out, janet_wrap_table(entry));
        }
        return janet_wrap_array(out);
    }

    // The rules in force along with their parts, as Janet data: a tuple
    // sorted by rule node, comprising structs {:rule :conditions :negated
    // :guards :paths :neural :heads}, each part a tuple of nodes
    // (Reasoning::rules_in_force). It is the engine's own reading of the
    // conditions, thus a front end that reasons about the rules in force --
    // whether its own or those it did not write -- requires no second one.
    // A rule that a statement only mentions is not in force and does not
    // appear in the listing, and nothing is created. Main thread only, akin
    // to zelph/strata: the reading sets the predicates that a run reads.
    Janet ScriptEngine::Impl::janet_cfun_zelph_rules(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_tuple(janet_tuple_n(nullptr, 0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/rules", argc, argv, true);

        require_main_thread("zelph/rules");

        const auto nodes = [](const std::vector<network::Node>& list)
        {
            Janet* t = janet_tuple_begin(static_cast<int32_t>(list.size()));
            for (std::size_t i = 0; i < list.size(); ++i)
                t[i] = zelph_wrap_node(list[i]);
            return janet_wrap_tuple(janet_tuple_end(t));
        };

        const std::vector<network::Reasoning::ListedRule> rules = s_instance->_n->rules_in_force();
        Janet*                                            out   = janet_tuple_begin(static_cast<int32_t>(rules.size()));
        for (std::size_t i = 0; i < rules.size(); ++i)
        {
            const network::Reasoning::ListedRule& r     = rules[i];
            JanetKV*                              entry = janet_struct_begin(7);
            janet_struct_put(entry, janet_ckeywordv("rule"), zelph_wrap_node(r.rule));
            janet_struct_put(entry, janet_ckeywordv("conditions"), nodes(r.conditions));
            janet_struct_put(entry, janet_ckeywordv("negated"), nodes(r.negated));
            janet_struct_put(entry, janet_ckeywordv("guards"), nodes(r.guards));
            janet_struct_put(entry, janet_ckeywordv("paths"), nodes(r.paths));
            janet_struct_put(entry, janet_ckeywordv("neural"), nodes(r.neural));
            janet_struct_put(entry, janet_ckeywordv("heads"), nodes(r.heads));
            out[i] = janet_wrap_struct(janet_struct_end(entry));
        }
        Janet res = janet_wrap_tuple(janet_tuple_end(out));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/rules", argc, argv, false, res);
        return res;
    }

    // Janet's perspective on the warning about defeated negations. A
    // program executing the engine through Janet cannot read the warning,
    // thus it reads the identical facts presented here. A front end that
    // translates its own input can subsequently inform its user which
    // earlier conclusion a newly introduced statement has defeated.
    Janet ScriptEngine::Impl::janet_cfun_zelph_defeated(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_array(janet_array(0));
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/defeated", argc, argv, true);

        require_main_thread("zelph/defeated");

        const auto& defeated = s_instance->_n->defeated_negations();
        JanetArray* out      = janet_array(static_cast<int32_t>(defeated.size()));
        for (const auto& [fact, rule] : defeated)
        {
            Janet pair[2] = {zelph_wrap_node(fact), zelph_wrap_node(rule)};
            janet_array_push(out, janet_wrap_tuple(janet_tuple_n(pair, 2)));
        }
        Janet res = janet_wrap_array(out);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/defeated", argc, argv, false, res);
        return res;
    }

    // The .negation-records switch, when used by a front end that controls
    // zelph via Janet and lacks the ability to input a command: if invoked
    // without a parameter, it reads the current state; if provided with true
    // or false, it modifies the switch setting; in both cases, it returns the
    // resulting state, mirroring how zelph/cluster reports the state of the
    // active cluster. A boolean is required: a string like "off" is considered
    // truthy, and reading it as such would switch the records on.
    Janet ScriptEngine::Impl::janet_cfun_zelph_negation_records(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 0, 1);
        if (!s_instance) return janet_wrap_false();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negation-records", argc, argv, true);

        require_main_thread("zelph/negation-records");

        if (argc == 1) s_instance->_n->record_negations(janet_getboolean(argv, 0) != 0);

        Janet res = janet_wrap_boolean(s_instance->_n->record_negations() ? 1 : 0);
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/negation-records", argc, argv, false, res);
        return res;
    }

    // The fact that the inputs to zelph/explain, zelph/holders, and
    // zelph/prune-fact name: a fact node, or the fact's subject, predicate,
    // and objects, resolved without creating anything and taken precisely,
    // ensuring that `a p b` never refers to `a p b c`. `fact` equals 0 when
    // the graph holds no such fact, and `variable` says that a node given was
    // a variable.
    ScriptEngine::Impl::FactArgument ScriptEngine::Impl::fact_argument(const int32_t argc, Janet* const argv)
    {
        FactArgument out;
        if (argc == 1)
        {
            out.fact     = s_instance->resolve_janet_arg_no_create(argv[0]);
            out.variable = network::Network::is_var(out.fact);
            return out;
        }

        network::Reasoning* const n = s_instance->_n;
        const network::Node       s = s_instance->resolve_janet_arg_no_create(argv[0]);
        const network::Node       p = s_instance->resolve_janet_arg_no_create(argv[1]);
        network::adjacency_set    objects;
        bool                      resolved = s != 0 && p != 0;
        out.variable                       = network::Network::is_var(s) || network::Network::is_var(p);
        for (int32_t i = 2; resolved && i < argc; ++i)
        {
            const network::Node o = s_instance->resolve_janet_arg_no_create(argv[i]);
            resolved              = o != 0;
            out.variable          = out.variable || network::Network::is_var(o);
            objects.insert(o);
        }
        if (resolved)
            if (const network::Answer answer = n->check_fact(s, p, objects); answer.is_known()) out.fact = answer.relation();
        return out;
    }

    // The proof .explain prints, as data: a single struct for each step,
    // containing the rule and the premises of a derived fact, intended for a
    // front end that responds to the query why using its own language and
    // needs the parts rather than the printed tree. The fact is resolved
    // without creating anything, just as zelph/exists resolves it, and is
    // accepted precisely as provided, just as .explain takes it: zelph/fact
    // would claim a derived fact, and a fact involving additional objects
    // constitutes a different fact. A statement whose text holds a comma
    // list builds a new condition set every time it is written, so only its
    // node names it.
    //
    // The full proof is delivered, provided the search stays within the
    // session's .explain-budget, which also sets the limit on what gets
    // constructed here; otherwise, the proof is the one found within the
    // default depth of .explain, and its root carries :fallback. A subproof
    // shared across two steps is represented as a single struct in both
    // locations. Only on the main thread: the search retains the rules it
    // has read for use in the next search.
    Janet ScriptEngine::Impl::janet_cfun_zelph_explain(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/explain", argc, argv, true);

        require_main_thread("zelph/explain");
        if (argc == 2) janet_panicf("zelph/explain: pass a fact node, or the fact's subject, predicate and objects");

        network::Reasoning* const n   = s_instance->_n;
        Janet                     res = janet_wrap_nil();
        {
            const network::Node target = fact_argument(argc, argv).fact;

            if (target != 0 && n->check_fact(target).is_known())
            {
                const std::shared_ptr<network::ProofNode> proof    = n->explain(target, network::Reasoning::default_explain_depth);
                const bool                                fallback = n->last_explain_counts().limited;

                // Every status name is explicitly listed, with no default
                // case present, meaning that adding a new status to
                // ProofNode::Status earns a compiler warning until it is
                // assigned its own dedicated keyword.
                const auto status = [](const network::ProofNode::Status st) -> Janet
                {
                    switch (st)
                    {
                    case network::ProofNode::Status::Derived:
                        return janet_ckeywordv("derived");
                    case network::ProofNode::Status::Axiom:
                        return janet_ckeywordv("axiom");
                    case network::ProofNode::Status::Unfounded:
                        return janet_ckeywordv("unfounded");
                    case network::ProofNode::Status::RulePattern:
                        return janet_ckeywordv("rule-pattern");
                    case network::ProofNode::Status::Mentioned:
                        return janet_ckeywordv("mentioned");
                    case network::ProofNode::Status::RuleMentioned:
                        return janet_ckeywordv("rule-mentioned");
                    case network::ProofNode::Status::Truncated:
                        return janet_ckeywordv("truncated");
                    }
                    return janet_wrap_nil();
                };
                const auto nodes = [](const std::vector<network::Node>& list)
                {
                    Janet* const items = janet_tuple_begin(static_cast<int32_t>(list.size()));
                    for (std::size_t i = 0; i < list.size(); ++i)
                        items[i] = zelph_wrap_node(list[i]);
                    return janet_wrap_tuple(janet_tuple_end(items));
                };

                // Constructed starting from the leaves, using an
                // independent stack: a proof may extend as deep as the
                // sequence of facts permits, and each level would require a
                // call frame, risking stack overflow (refer to ProofNode's
                // destructor). A step is assembled only after all
                // underlying steps are complete; a step revisited is the
                // struct already built.
                std::unordered_map<const network::ProofNode*, Janet>    built;
                std::unordered_set<const network::ProofNode*>           open;
                std::vector<std::pair<const network::ProofNode*, bool>> todo{{proof.get(), false}};
                const auto                                              steps = [&](const std::vector<std::shared_ptr<network::ProofNode>>& list)
                {
                    Janet* const items = janet_tuple_begin(static_cast<int32_t>(list.size()));
                    for (std::size_t i = 0; i < list.size(); ++i)
                        items[i] = built.at(list[i].get());
                    return janet_wrap_tuple(janet_tuple_end(items));
                };
                while (!todo.empty())
                {
                    const auto [p, expanded] = todo.back();
                    if (built.contains(p))
                    {
                        todo.pop_back();
                        continue;
                    }
                    if (!expanded)
                    {
                        todo.back().second = true;
                        open.insert(p);
                        const auto enter = [&](const std::shared_ptr<network::ProofNode>& q)
                        {
                            if (open.contains(q.get())) throw std::logic_error("zelph/explain: a proof step lies below itself");
                            if (!built.contains(q.get())) todo.emplace_back(q.get(), false);
                        };
                        for (const auto& q : p->premises)
                            enter(q);
                        for (const auto& edges : p->walk_edges)
                            for (const auto& q : edges)
                                enter(q);
                        continue;
                    }

                    JanetTable* const step = janet_table(11);
                    janet_table_put(step, janet_ckeywordv("fact"), zelph_wrap_node(p->fact));
                    janet_table_put(step, janet_ckeywordv("status"), status(p->status));
                    if (p->rule != 0) janet_table_put(step, janet_ckeywordv("rule"), zelph_wrap_node(p->rule));
                    janet_table_put(step, janet_ckeywordv("premises"), steps(p->premises));
                    janet_table_put(step, janet_ckeywordv("absent"), nodes(p->absent));
                    janet_table_put(step, janet_ckeywordv("walked"), nodes(p->walked));
                    Janet* const edges = janet_tuple_begin(static_cast<int32_t>(p->walk_edges.size()));
                    for (std::size_t i = 0; i < p->walk_edges.size(); ++i)
                        edges[i] = steps(p->walk_edges[i]);
                    janet_table_put(step, janet_ckeywordv("walk-edges"), janet_wrap_tuple(janet_tuple_end(edges)));
                    janet_table_put(step, janet_ckeywordv("cycle-leaf"), janet_wrap_boolean(p->cycle_leaf ? 1 : 0));
                    janet_table_put(step, janet_ckeywordv("more"), janet_wrap_boolean(p->more_justifications ? 1 : 0));
                    JanetTable* const bindings = janet_table(static_cast<int32_t>(p->bindings.size()));
                    for (const auto& [variable, value] : p->bindings)
                        janet_table_put(bindings, zelph_wrap_node(variable), zelph_wrap_node(value));
                    janet_table_put(step, janet_ckeywordv("bindings"), janet_wrap_struct(janet_table_to_struct(bindings)));
                    if (fallback && p == proof.get()) janet_table_put(step, janet_ckeywordv("fallback"), janet_wrap_true());

                    built.emplace(p, janet_wrap_struct(janet_table_to_struct(step)));
                    open.erase(p);
                    todo.pop_back();
                }
                res = built.at(proof.get());
            }
        }

        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/explain", argc, argv, false, res);
        return res;
    }

    // The claimed fact that zelph/prune-fact and zelph/holders act on, or 0
    // if there is none: a pattern belongs to .prune-facts, and a node that
    // is not a fact names nothing to withdraw.
    network::Node ScriptEngine::Impl::claimed_fact_argument(const char* const name, const int32_t argc, Janet* const argv)
    {
        if (argc == 2) janet_panicf("%s: pass a fact node, or the fact's subject, predicate and objects", name);
        refuse_inside_rule_build(name);

        const FactArgument        argument = fact_argument(argc, argv);
        network::Reasoning* const n        = s_instance->_n;
        if (argument.variable || (argument.fact != 0 && n->check_fact(argument.fact).is_known() && n->var_in_closure(argument.fact)))
            janet_panicf("%s: the fact holds a variable, which makes it a pattern; .prune-facts removes what a pattern matches", name);
        if (argument.fact == 0 || !n->exists(argument.fact)) return 0;
        if (!n->check_fact(argument.fact).is_known())
            janet_panicf("%s: %s is no fact", name, n->format(argument.fact).c_str());
        return argument.fact;
    }

    // One claimed fact withdrawn, as the ground form of .prune-facts
    // withdraws it (Reasoning::prune_fact), in a scenario where a program
    // reverses its prior assertion: `:removed` if the node went with the
    // statements that held it, `:unclaimed` if it remains as a rule's pattern
    // or a mention, `nil` if nothing claimed it. A removal is not equivalent
    // to a creation, hence dropping the active cluster does not bring the
    // fact back.
    Janet ScriptEngine::Impl::janet_cfun_zelph_prune_fact(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/prune-fact", argc, argv, true);

        require_main_thread("zelph/prune-fact");
        if (s_instance->_full_graph_guard) s_instance->_full_graph_guard("zelph/prune-fact");

        Janet               res  = janet_wrap_nil();
        const network::Node fact = claimed_fact_argument("zelph/prune-fact", argc, argv);
        if (fact != 0)
        {
            switch (s_instance->_n->prune_fact(fact).outcome)
            {
            case network::Reasoning::FactPrune::removed:
                res = janet_ckeywordv("removed");
                break;
            case network::Reasoning::FactPrune::unclaimed:
                res = janet_ckeywordv("unclaimed");
                break;
            case network::Reasoning::FactPrune::none:
                break;
            }
        }

        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/prune-fact", argc, argv, false, res);
        return res;
    }

    // The statements that claim a fact through nesting (Reasoning::holders):
    // what zelph/prune-fact would take along with the fact or withdraw,
    // queried before pruning by a program that must not lose any.
    Janet ScriptEngine::Impl::janet_cfun_zelph_holders(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/holders", argc, argv, true);

        require_main_thread("zelph/holders");
        // Blocked where `zelph/prune-fact` is: a partial view may lack a
        // holder or a rule that was not loaded, with no indication of the
        // absence, and a program that prunes on an empty response reads it
        // as safe.
        if (s_instance->_full_graph_guard) s_instance->_full_graph_guard("zelph/holders");

        Janet               res  = janet_wrap_nil();
        const network::Node fact = claimed_fact_argument("zelph/holders", argc, argv);
        if (fact != 0)
        {
            const std::vector<network::Node> holders = s_instance->_n->holders(fact);
            Janet* const                     items   = janet_tuple_begin(static_cast<int32_t>(holders.size()));
            for (std::size_t i = 0; i < holders.size(); ++i)
                items[i] = zelph_wrap_node(holders[i]);
            res = janet_wrap_tuple(janet_tuple_end(items));
        }

        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/holders", argc, argv, false, res);
        return res;
    }

    // The facts that are derived from a fact directly
    // (Reasoning::dependents): what remains after a prune, lacking its
    // justification. A search limited by the budget concludes with
    // :truncated, ensuring it never answers empty even if a dependent might
    // still be unfound.
    Janet ScriptEngine::Impl::janet_cfun_zelph_dependents(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/dependents", argc, argv, true);

        require_main_thread("zelph/dependents");
        if (s_instance->_full_graph_guard) s_instance->_full_graph_guard("zelph/dependents"); // as `zelph/holders`

        Janet               res  = janet_wrap_nil();
        const network::Node fact = claimed_fact_argument("zelph/dependents", argc, argv);
        if (fact != 0)
        {
            bool                             truncated  = false;
            const std::vector<network::Node> dependents = s_instance->_n->dependents(fact, truncated);
            Janet* const                     items      = janet_tuple_begin(static_cast<int32_t>(dependents.size() + (truncated ? 1 : 0)));
            for (std::size_t i = 0; i < dependents.size(); ++i)
                items[i] = zelph_wrap_node(dependents[i]);
            if (truncated) items[dependents.size()] = janet_ckeywordv("truncated");
            res = janet_wrap_tuple(janet_tuple_end(items));
        }

        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/dependents", argc, argv, false, res);
        return res;
    }

    // --- Clusters: the graph as a workspace rather than a store ---
    //
    // A cluster maintains a record of the node IDs CREATED during its active
    // period, thus dropping it removes exactly those again. That is the sole
    // mechanism for retraction provided by the Janet API, and in its
    // absence, a program which asserts a fact base, performs reasoning upon
    // it, and retrieves outcomes retains every question it ever posed within
    // the graph permanently. .explain already internally relies on a cluster
    // for precisely this reason.
    //
    // These do NOT delegate to the REPL commands the way zelph/save and
    // zelph/run do. .cluster and .cluster-drop each print a status line, and
    // a caller that scopes one question per step of its own loop invokes
    // them thousands of times, where that line is not information but noise
    // on the caller's output channel. Returning the values instead is also
    // what a program wants: the active cluster's name, and how many nodes a
    // drop actually removed.
    //
    // Main thread only, like the commands they correspond to.
    void ScriptEngine::Impl::cluster_preamble(int32_t argc, Janet* argv, const char* name)
    {
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        require_main_thread(name);
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_cluster(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 0, 1);
        if (!s_instance) return janet_wrap_nil();
        cluster_preamble(argc, argv, "zelph/cluster");
        refuse_inside_rule_build("zelph/cluster");

        if (argc == 1)
        {
            // nil and "default" both mean "no cluster", matching .cluster.
            if (janet_checktype(argv[0], JANET_NIL))
            {
                s_instance->_n->deactivate_cluster();
            }
            else
            {
                const std::string name = reinterpret_cast<const char*>(janet_getstring(argv, 0));
                if (name.empty() || name == "default")
                    s_instance->_n->deactivate_cluster();
                else if (network::Zelph::is_scratch_cluster_name(name))
                    throw std::runtime_error("zelph/cluster: " + network::Zelph::scratch_cluster_name_refusal(name));
                else
                    s_instance->_n->set_active_cluster(name);
            }
        }

        const std::string active = s_instance->_n->active_cluster_name();
        return active.empty() ? janet_wrap_nil() : janet_cstringv(active.c_str());
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_cluster_drop(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        cluster_preamble(argc, argv, "zelph/cluster-drop");
        refuse_inside_rule_build("zelph/cluster-drop");

        const std::string name = reinterpret_cast<const char*>(janet_getstring(argv, 0));
        if (name.empty() || name == "default")
            janet_panicf("zelph/cluster-drop: the default cluster cannot be dropped");

        return janet_wrap_integer(static_cast<int32_t>(s_instance->_n->drop_cluster(name)));
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_clusters(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_nil();
        cluster_preamble(argc, argv, "zelph/clusters");

        const auto  listed = s_instance->_n->list_clusters();
        JanetArray* out    = janet_array(static_cast<int32_t>(listed.size()));
        for (const auto& [name, count] : listed)
        {
            Janet pair[2] = {janet_cstringv(name.c_str()),
                             janet_wrap_integer(static_cast<int32_t>(count))};
            janet_array_push(out, janet_wrap_tuple(janet_tuple_n(pair, 2)));
        }
        return janet_wrap_array(out);
    }
}
