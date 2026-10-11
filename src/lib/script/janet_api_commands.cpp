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
#include <stdexcept>
#include <string>
#include <thread>
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

    // Same delegation as command_impl, for the REPL commands that take no
    // argument. Kept separate rather than making the argument optional,
    // because the arity check is what tells a caller which of the two it is.
    Janet ScriptEngine::Impl::command_noarg_impl(int32_t argc, Janet* argv, const char* name, const char* command)
    {
        janet_fixarity(argc, 0);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call(name, argc, argv, true);

        require_main_thread(name);

        refuse_inside_rule_build(name);

        if (!s_instance->_command_handler)
            janet_panicf("%s: no command handler registered (script engine not fully initialized)", name);

        std::string err;
        try
        {
            s_instance->_command_handler({command});
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("%s: %s", name, err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Run the inference engine. Without this, a program that drives zelph as
    // a library can assert facts and define rules from Janet but cannot ask
    // for their consequences: forward chaining is otherwise reached only
    // through the .run command or through auto-run at the end of a Janet
    // block, neither of which exists outside the REPL.
    Janet ScriptEngine::Impl::janet_cfun_zelph_run(int32_t argc, Janet* argv)
    {
        return command_noarg_impl(argc, argv, "zelph/run", ".run");
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_run_once(int32_t argc, Janet* argv)
    {
        return command_noarg_impl(argc, argv, "zelph/run-once", ".run-once");
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_run_delta(int32_t argc, Janet* argv)
    {
        return command_noarg_impl(argc, argv, "zelph/run-delta", ".run-delta");
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
