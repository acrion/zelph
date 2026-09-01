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

#pragma once

// The script engine's implementation, declared so that it can be written in
// parts. The parts follow what the engine DOES: building the Janet
// environment, converting values across the boundary, the zelph/... functions
// grouped by what they act on, and the transformation of zelph syntax into
// Janet code.
//
// Internal. The public header names Janet via forward declarations alone, and
// that is what keeps <janet.h> out of everything that merely wants to run a
// script.

#include "network/network_types.hpp"
#include "network/neural.hpp"
#include "script/script_engine.hpp"

#include <janet.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zelph
{
    namespace network
    {
        class Reasoning;
    }

    class ScriptEngine::Impl
    {
    public:
        static Impl* s_instance; // Required for static Janet C-function callbacks

        network::Reasoning* _n;
        JanetTable*         _janet_env = nullptr;
        Janet               _zelph_peg{};
        bool                _log_janet_functions = false;

        // Set while zelph/dedup-rule runs its thunk: the facts built there are a
        // rule's patterns, not claims, so zelph/fact must not revoke a
        // pattern marking then.
        bool _building_rule = false;

        // Set while a user's Janet BLOCK runs -- `%(...)` in the REPL or in a
        // script. Only there is a zelph/fact call a statement of its own and
        // therefore a claim. Everywhere else the same call builds part of
        // something else: the subterms of a parsed statement, the fact pattern a
        // command like .explain evaluates read-only, the term an inline keyword
        // island returns. Naming a statement is not claiming it, so the
        // revocation in zelph/fact asks for this flag rather than for the
        // absence of one of those contexts.
        bool _in_janet_block = false;

        // Set while a command RESOLVES a printed pattern to the node it denotes --
        // ".explain (a p b)", ".prune-facts (a p b)", ".node a p b". The code is
        // generated the same way a statement's is, so it runs through zelph/fact,
        // which is the assertion API; the scratch cluster around it is what has
        // always kept the assertion from surviving.
        //
        // That was enough until a fact could be REFUTED. Zelph::fact refuses to
        // claim a fact the graph holds as known-wrong -- rightly -- so the moment
        // ¬(a p b) became writable, every command that addresses a pattern by
        // printing it back answered "Unknown node" for exactly the facts a user
        // has most reason to look at. Under this flag the pattern is LOOKED UP
        // instead of asserted -- exactly, and only where the graph holds it; see
        // the note at the lookup itself for why both halves of that matter.
        bool _resolving_pattern = false;

        // Save/restore around a nested evaluation (an .import inside a block, a
        // keyword handler): a plain assignment would leak the inner context.
        struct BlockScope
        {
            BlockScope(bool& flag, const bool value)
                : _flag(flag)
                , _saved(flag)
            {
                _flag = value;
            }
            ~BlockScope() { _flag = _saved; }
            bool&      _flag;
            const bool _saved;
        };

        // `janet_dostring` prints the stack trace itself, through `janet_eprintf`,
        // BEFORE it hands the error status back, so there is no catch downstream
        // that can decide about it. The `:err` dyn is where that printing goes,
        // and it is pointed at a buffer of ours ONCE, at init: every trace lands
        // there and the caller decides what becomes of it.
        //
        // Pointing it there on each call was the first shape and it must not come
        // back. A fiber's env IS its dyn table (`janet_dobytes` sets `fiber->env`
        // to the module environment), so `janet_setdyn` writes into whichever of
        // the two is current -- and reading it back with `janet_dyn` on the
        // statement path segfaulted inside `janet_table_get`, while a fresh
        // `janet_buffer` per call corrupted the heap within a few thousand
        // statements. One buffer, set once, touches no VM state after init.
        JanetBuffer* _err_sink = nullptr;

        // A registered syntax keyword. Two kinds share this entry, the
        // registration API (zelph/register-keyword) and the handler protocol
        // (text in, :incomplete veto, result out):
        //   - block keywords (inline_mode == false): line-based. Detected as the
        //     first token of a REPL/script line, accumulated until an empty line
        //     (see Interactive::process). The handler result is printed.
        //   - inline keywords (inline_mode == true): expression islands. Whenever
        //     the opening delimiter (the map key) occurs inside a zelph statement,
        //     the text up to `close` is passed to the handler, which must return a
        //     zelph/node; the node replaces the island in the statement (spliced
        //     via the unquote mechanism). :incomplete extends the island to the
        //     next occurrence of `close`, so nested delimiters work without the
        //     host knowing the island's grammar.
        struct KeywordEntry
        {
            Janet       handler{};
            bool        inline_mode = false;
            std::string close; // inline mode only
        };
        std::map<std::string, KeywordEntry> _keyword_handlers;

        // True while inline-keyword expansion has already prepared (cleared) the
        // scoped-variable map for the statement being processed; process_janet
        // and evaluate_expression then skip their own clear exactly once, so
        // island handlers and the surrounding statement share one variable scope.
        bool _scoped_vars_preloaded = false;

        enum class HandlerCall
        {
            Dispatched,
            Incomplete
        };

        // Compiled neural networks (session-scoped caches, discarded on .reset).
        // Handles handed to Janet are indexes into this vector.
        std::vector<std::unique_ptr<network::NeuralNet>> _neural_nets;

        // Track variables used in the current scope/statement
        std::map<std::string, network::Node> _scoped_variables;

        // Memoized rule fingerprints, see find_duplicate_rule. Keyed by node,
        // which is a structural hash, so an entry can never go stale. 0 means
        // "not a rule".
        std::unordered_map<network::Node, std::size_t> _rule_shapes;

        // Guards the script engine's own bookkeeping (_scoped_variables,
        // _neural_nets) against concurrent access from Janet threads
        // (ev/spawn-thread). Calls INTO the reasoning engine are synchronized
        // by zelph itself and are not covered here.
        std::mutex _state_mutex;

        // Set by Interactive; backs the Janet function zelph/import.
        ImportHandler _import_handler;

        // Set by Interactive; backs zelph/save and zelph/load.
        CommandHandler _command_handler;

        // See ScriptEngine::set_echo_predicate.
        EchoPredicate _echo_predicate;

        // The thread that owns _janet_env. zelph/import must run here: the REPL
        // pipeline it delegates to executes Janet code in the main VM, which is
        // not usable from other Janet threads (each has its own VM).
        std::thread::id _main_thread_id;

        enum class NestedOp
        {
            None,
            Negation,
            Approx,
            Path
        };

        // --- Implemented in script_engine_setup.cpp ---

        explicit Impl(network::Reasoning* n);
        ~Impl();
        void init();
        void register_zelph_functions() const;
        void setup_module_paths() const;
        void setup_script_runner() const;
        void setup_peg();
        void setup_numbers() const;

        // --- Implemented in janet_values.cpp ---

        std::string        take_err_trace() const;
        void               flush_err_trace() const;
        static JanetSignal pcall_rooted(JanetFunction* const fun, const int32_t argc, const Janet* const argv, Janet* const out);
        bool               echo_enabled() const;
        void               clear_scoped_variables();
        bool               has_scoped_variables();
        static std::string format_janet(Janet j);
        void               log_janet_call(const std::string& func_name, int32_t argc, Janet* argv, bool is_entry, Janet ret = janet_wrap_nil()) const;
        network::Node      resolve_janet_arg(Janet arg);
        network::Node      resolve_janet_arg_no_create(Janet arg) const;

        // --- Implemented in janet_api_graph.cpp ---

        static Janet fact_probe(const char* name, const bool asserted_only, int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_exists(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_mentioned(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_name(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_sources(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_targets(int32_t argc, Janet* argv);
        static Janet closure_impl(int32_t argc, Janet* argv, const char* name, bool forward);
        static Janet janet_cfun_zelph_closure(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_closure_sources(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_approx(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_path(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_path_guard(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_out(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_var(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_resolve(int32_t argc, Janet* argv);

        // --- Implemented in janet_api_facts.cpp ---

        static Janet  janet_cfun_zelph_car(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_cdr(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_negate(int32_t argc, Janet* argv);
        network::Node find_duplicate_rule(const network::Node rule);
        static Janet  janet_cfun_zelph_dedup_rule(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_rule(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_list_chars(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_list(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_set(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_collection(int32_t argc, Janet* argv);
        bool          is_condition_pattern(const network::Node n) const;
        void          check_conditions_are_patterns(const network::Node set) const;
        static Janet  janet_cfun_zelph_conjunction(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_fact(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_refute(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_query(int32_t argc, Janet* argv);

        // --- Implemented in janet_api_neural.cpp ---

        network::NeuralNet*                                  get_net(int32_t handle);
        static std::vector<double>                           janet_number_vector(Janet v, const char* what);
        static Janet                                         janet_cfun_zelph_nn_connect(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_weight(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_set_weight(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_compile(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_nodes(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_eval(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_train(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_snapshot(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_restore(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_write_back(int32_t argc, Janet* argv);
        static std::vector<std::pair<network::Node, double>> janet_node_activations(Janet v, const char* what);
        static Janet                                         janet_cfun_zelph_nn_connect_layers(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_train_nodes(int32_t argc, Janet* argv);
        static Janet                                         janet_cfun_zelph_nn_eval_nodes(int32_t argc, Janet* argv);

        // --- Implemented in janet_api_display.cpp ---

        static Janet janet_cfun_zelph_set_number_digits(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_register_display_scheme(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_set_infix_display(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_set_application_display(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_no_selffact_sugar(int32_t argc, Janet* argv);

        // --- Implemented in janet_api_commands.cpp ---

        static Janet janet_cfun_zelph_import(int32_t argc, Janet* argv);
        static Janet command_impl(int32_t argc, Janet* argv, const char* name, const char* command);
        static Janet janet_cfun_zelph_save(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_load(int32_t argc, Janet* argv);
        static Janet command_noarg_impl(int32_t argc, Janet* argv, const char* name, const char* command);
        static Janet janet_cfun_zelph_run(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_run_once(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_run_delta(int32_t argc, Janet* argv);
        static void  cluster_preamble(int32_t argc, Janet* argv, const char* name);
        static Janet janet_cfun_zelph_cluster(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_cluster_drop(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_clusters(int32_t argc, Janet* argv);

        // --- Implemented in script_keywords.cpp ---

        HandlerCall  call_keyword_handler(const std::string& name, const Janet handler, const std::string& text, const bool force, Janet& result);
        std::string  expand_inline_keywords(const std::string& input);
        static Janet janet_cfun_zelph_register_keyword(int32_t argc, Janet* argv);

        // --- Implemented in zelph_to_janet.cpp ---

        static bool     split_path_marker(const std::string& token, std::string& base, std::string& mode);
        static bool     atom_text(Janet arg, std::string& text);
        static bool     is_path_ast(Janet node);
        static NestedOp nested_condition_op(Janet node);
        static NestedOp misplaced_condition_op(Janet node, const bool in_condition);
        std::string     build_smart_call(const std::string& func_name, const std::vector<Janet>& args, const std::string& role = "") const;
        std::string     transform_arg(Janet arg_tuple) const;
    };
}
