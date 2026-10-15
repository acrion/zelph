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

        // Set during the execution of a rule's construction -- more precisely
        // while `zelph/dedup-rule`, `zelph/build-rule`, the build of the
        // `zelph/rule` macro, or, outside all of them, `zelph/rule-text` runs
        // its thunk: the facts generated within that scope function as
        // patterns for the rule, not assertions, thus `zelph/fact` must avoid
        // removing a pattern marking at that time. None of the first three
        // operations initiates while it is set, nor does `zelph/rule*` execute
        // (refer to refuse_nested_rule), and `zelph/rule-text` delegates the
        // handling of the marker to the build it is part of.
        bool _building_rule = false;

        // Set while `zelph/mention` runs its thunk, alongside
        // `_building_rule`: a variable resolved within it pertains to the
        // statement, which subsequently asks, whereas a variable contained
        // within a rule that the mention holds belongs to that rule
        // (`zelph/rule-text` clears it).
        bool _mentioning = false;

        // Set while a user's Janet BLOCK runs -- `%(...)` in the REPL or within
        // a script -- and while the handler of a block keyword runs, which
        // responds to a block of user input exactly as such a block would.
        // Only there is a zelph/fact call a statement of its own and
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
        //
        // The operand in a condition operator functions as a pattern
        // for the same reason, and `zelph/operand` sets the flag during
        // its construction.
        bool _resolving_pattern = false;

        // Set while a command evaluates the pattern it names (evaluate_expression
        // with `resolving_pattern`), within a scratch cluster that the command
        // discards before retrieving the answer. The operand of a condition
        // operator also sets `_resolving_pattern`, yet within a rule or a query
        // that remains, so it leaves this flag alone.
        bool _resolving_command = false;

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

        // How deeply the evaluations of a user's Janet code are nested: a `%`
        // block or a statement (process_janet), a keyword handler
        // (invoke_keyword), a `.janet` script (run_janet_script). The `=>`
        // facts zelph/fact noted are verified upon completion of the outermost
        // one (Reasoning::settle_fact_rules), as only then has every call run
        // that may hold them: an `.import` inside a block nests the lines of
        // the file. An island is not an evaluation in its own right, since the
        // statement it stands in may hold what it returns.
        int _evaluation_depth = 0;

        struct EvaluationScope
        {
            explicit EvaluationScope(Impl& impl)
                : _impl(impl)
            {
                ++_impl._evaluation_depth;
            }
            ~EvaluationScope() { --_impl._evaluation_depth; }
            EvaluationScope(const EvaluationScope&)            = delete;
            EvaluationScope& operator=(const EvaluationScope&) = delete;

            // When this is the outermost evaluation, the refusals of the noted
            // `=>` facts; otherwise, none.
            std::string settle() const;

            Impl& _impl;
        };

        // The statement whose parser-generated code is running, enabling
        // `zelph/rule-text` to distinguish a mention within a claim from one
        // within a query. Both build the rule using identical function calls,
        // with only the end of the statement indicating which one it was:
        // `S noted ((a p b) => (c q d))` resolves S after the rule it names.
        // `zelph/rule-text` thus records what a mention created, while
        // `process_janet` marks it once the statement has succeeded as a
        // claim. The marks of a query would persist beyond it and hide `a p b`
        // from a later claim, hence they are discarded, as are those from a
        // statement that fails, since not all of its variables may have been
        // resolved at that point. Null outside a parsed statement: Janet code
        // of the user's own marks at once.
        struct ParsedStatement
        {
            // What each `zelph/rule-text` invocation outside a rule build
            // created.
            std::vector<std::vector<network::Node>> mentions;
            // The collections its `@{...}` literals and its comma lists
            // created outside a rule build: what the statement says about
            // them is part of the claim (Zelph::claim_nested); however, a
            // collection merely referenced by name is not entered.
            std::vector<network::Node> collections;
            // Whether a variable was resolved outside every rule build: the
            // statement asks. The variables of a rule it mentions do not
            // count.
            bool asks = false;
            // What a query containing a condition operator created
            // (`zelph/query-text`). Of these elements, those lying under an
            // operator are a pattern, and they are marked at the end of the
            // statement, just before the query is evaluated
            // (Zelph::mark_query_operands).
            std::vector<network::Node> query_created;
            // The relation-type declarations among `query_created` that the
            // query's first use of a predicate authored, each accompanied by
            // its predicate (Zelph::cluster_declarations). They accompany the
            // query's text (Zelph::drop_query_text).
            std::vector<std::pair<network::Node, network::Node>> query_declared;
            // What the statement's `$( )` islands created before any of its
            // code ran (expand_inline_keywords). A rule authored by the
            // statement and a query that carries an operator both count it
            // among what they created themselves.
            std::vector<network::Node> island_created;
            // The facts that a query's own ground markings `(F) ~ refuted`
            // claim refuted (zelph/query-text). A query builds before
            // evaluation, and a refutation applied during construction
            // remained present when the query subsequently failed; they are
            // applied once the statement completes.
            std::vector<network::Node> query_refutations;
            // The facts that its `zelph/refute` calls have refuted. A
            // refutation claims its marking rather than its fact, which is
            // what `¬(F)` on its own line evaluates to, thus the
            // statement's claim does not start there.
            std::vector<network::Node> refuted;
            // What each `zelph/mention` situated outside a rule build wrote:
            // the statement it produced, what it created, and the
            // refutation markings it held back. Marked at the end of the
            // statement, unless the statement asks (Zelph::mark_mention).
            struct Mention
            {
                network::Node              root = 0;
                std::vector<network::Node> created;
                std::vector<network::Node> markings;
            };
            std::vector<Mention> statement_mentions;
            // The rule a typed rule statement keeps (zelph/dedup-rule): either
            // the new one, or the twin that was found to duplicate it. A focus
            // makes the statement evaluate to a part of that rule, and the
            // statement continues to claim the rule while asking nothing.
            network::Node rule = 0;
            // The facts a focus wrote beside the part it hands on, outside
            // every mention (zelph/aside): the statement claims them just as
            // it claims what it writes, even if they had been rule patterns
            // previously.
            std::vector<network::Node> aside;
            // Whether the scratch cluster associated with zelph/claim-text
            // remained active for the claim at the end of the statement,
            // along with the cluster that was active before it
            // (end_claim_scratch).
            bool        claim_scratch = false;
            std::string claim_previous;
        };
        ParsedStatement* _statement = nullptr;

        // End the scratch cluster that zelph/claim-text left active for the
        // current statement: merge it into the cluster that was active
        // before it once the statement concludes, or drop it if the
        // statement does not succeed.
        void end_claim_scratch(bool keep);

        // Set while zelph/query-text executes the construction of a
        // query: in which a ground marking within the query's own
        // components collects its fact.
        std::vector<network::Node>* _query_refutations = nullptr;

        // Save/restore of _statement, just as BlockScope does for a flag.
        struct StatementScope
        {
            StatementScope(ParsedStatement*& current, ParsedStatement* const statement)
                : _current(current)
                , _saved(current)
            {
                _current = statement;
            }
            ~StatementScope() { _current = _saved; }
            ParsedStatement*&      _current;
            ParsedStatement* const _saved;
        };

        // Where zelph/aside records the fact that a focus writes: the list
        // of the statement (ParsedStatement::aside), or the list of the
        // mention whose thunk runs, whose facts are marked with what the
        // mention wrote. `_mention_aside` is the list into which a mention
        // inside a rule's build records, and the build marks its facts with
        // the rule's parts. Null when nothing records it, as in a Janet
        // block.
        std::vector<network::Node>* _aside         = nullptr;
        std::vector<network::Node>* _mention_aside = nullptr;

        // Save/restore of _aside and _mention_aside.
        struct AsideScope
        {
            AsideScope(std::vector<network::Node>*& current, std::vector<network::Node>* const sink)
                : _current(current)
                , _saved(current)
            {
                _current = sink;
            }
            ~AsideScope() { _current = _saved; }
            std::vector<network::Node>*&      _current;
            std::vector<network::Node>* const _saved;
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

        // The reader associated with an inline `%` expression that remains
        // open persists in a rooted state during its lifetime; null between
        // expressions. See ScriptEngine::start_inline_janet.
        JanetParser* _inline_reader = nullptr;

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

        // What the islands of the statement under parsing create is logged
        // within a scratch cluster, progressing from the first island to the
        // end of the parsing phase. A line rejected by the parser retains
        // none of it; a line accepted by the parser preserves it within the
        // currently active cluster, and once process_janet executes, that
        // line's list is transferred to the statement
        // (ParsedStatement::island_created).
        bool                       _island_recording = false;
        std::string                _island_previous;
        std::vector<network::Node> _island_created;

        enum class HandlerCall
        {
            Dispatched,
            Incomplete
        };

        // Compiled neural networks (session-scoped caches, discarded on .new).
        // The handles handed to Janet serve as indices within this vector.
        // When a load overwrites the graph, all entries are cleared while
        // preserving the slots, ensuring that a handle acquired before the
        // replacement never refers to a network compiled afterwards.
        std::vector<std::shared_ptr<network::NeuralNet>> _neural_nets;

        // Track variables used in the current scope/statement
        std::map<std::string, network::Node> _scoped_variables;

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

        // See ScriptEngine::set_auto_run_predicate.
        AutoRunPredicate _auto_run_predicate;

        // See ScriptEngine::set_full_graph_guard.
        FullGraphGuard _full_graph_guard;

        // The thread that owns _janet_env. zelph/import must run here: the REPL
        // pipeline it delegates to executes Janet code in the main VM, which is
        // not usable from other Janet threads (each has its own VM).
        // Every function that writes the graph or changes the engine's state also
        // executes here (require_main_thread).
        std::thread::id _main_thread_id;

        enum class NestedOp
        {
            None,
            Negation,
            Approx,
            Path
        };

        using RuleBuildStep = Janet (*)(Janet);

        // --- Implemented in script_engine_setup.cpp ---

        explicit Impl(network::Reasoning* n);
        ~Impl();
        void init();
        void register_zelph_functions() const;
        void setup_module_paths() const;
        void setup_script_runner() const;
        void setup_rule_macro() const;
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
        network::Node      resolve_janet_arg_no_create(Janet arg, const std::string& lang) const;

        // --- Implemented in janet_api_graph.cpp ---

        static Janet fact_probe(const char* name, const bool asserted_only, int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_exists(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_mentioned(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_name(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_lookup(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_lookup_all(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_lang(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_epoch(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_degree(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_annotate(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_annotations(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_unannotate(int32_t argc, Janet* argv);
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
        static Janet  janet_cfun_zelph_parse(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_negate(int32_t argc, Janet* argv);
        network::Node find_duplicate_rule(const network::Node rule);
        void          refuse_nested_rule(const char* caller) const;
        JanetSignal   run_rule_build(const char* caller, JanetFunction* thunk, std::string& previous, Janet& out, RuleBuildStep build = nullptr, std::vector<network::Node>* mention_markings = nullptr);
        static Janet  janet_cfun_zelph_dedup_rule(int32_t argc, Janet* argv);
        void          assert_kept_rule(network::Node rule) const;
        Janet         build_marked_rule(JanetFunction* thunk, RuleBuildStep build);
        static Janet  janet_cfun_zelph_build_rule(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_build_rule_parts(int32_t argc, Janet* argv);
        Janet         make_rule(int32_t argc, const Janet* argv);
        static Janet  rule_from_parts(Janet parts);
        static Janet  janet_cfun_zelph_rule_star(int32_t argc, Janet* argv);
        void          mark_mentioned_rules(const std::vector<network::Node>& created) const;
        void          add_island_created(std::vector<network::Node>& created) const;
        static Janet  janet_cfun_zelph_rule_text(int32_t argc, Janet* argv);
        void          mark_mentioned_aside(const std::vector<network::Node>& facts, const std::vector<network::Node>& created) const;
        static Janet  janet_cfun_zelph_aside(int32_t argc, Janet* argv);
        void          finish_mention(network::Node root, std::vector<network::Node>& created, std::vector<network::Node>& markings) const;
        void          mark_mention(const ParsedStatement::Mention& mention) const;
        static Janet  janet_cfun_zelph_mention(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_query_text(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_claim_text(int32_t argc, Janet* argv);
        static Janet  janet_cfun_zelph_operand(int32_t argc, Janet* argv);
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

        std::shared_ptr<network::NeuralNet>                  get_net(int32_t handle);
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

        static void  require_main_thread(const char* name);
        static void  refuse_inside_rule_build(const char* name);
        static Janet janet_cfun_zelph_import(int32_t argc, Janet* argv);
        static Janet command_impl(int32_t argc, Janet* argv, const char* name, const char* command);
        static Janet janet_cfun_zelph_save(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_load(int32_t argc, Janet* argv);
        static Janet run_command_impl(int32_t argc, Janet* argv, const char* name, const char* command);
        static Janet quiet_run_result(bool derived);
        static Janet janet_cfun_zelph_run(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_run_once(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_run_delta(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_auto_run_p(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_strata(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_rules(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_defeated(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_negation_records(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_explain(int32_t argc, Janet* argv);
        static void  cluster_preamble(int32_t argc, Janet* argv, const char* name);
        static Janet janet_cfun_zelph_cluster(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_cluster_drop(int32_t argc, Janet* argv);
        static Janet janet_cfun_zelph_clusters(int32_t argc, Janet* argv);

        struct FactArgument
        {
            network::Node fact     = 0;
            bool          variable = false;
        };
        static FactArgument  fact_argument(int32_t argc, Janet* argv);
        static network::Node claimed_fact_argument(const char* name, int32_t argc, Janet* argv);
        static Janet         janet_cfun_zelph_prune_fact(int32_t argc, Janet* argv);
        static Janet         janet_cfun_zelph_holders(int32_t argc, Janet* argv);
        static Janet         janet_cfun_zelph_dependents(int32_t argc, Janet* argv);

        // --- Implemented in script_keywords.cpp ---

        HandlerCall  call_keyword_handler(const std::string& name, const Janet handler, const std::string& text, const bool force, Janet& result);
        std::string  expand_inline_keywords(const std::string& input);
        void         end_island_recording(const bool keep);
        static Janet janet_cfun_zelph_register_keyword(int32_t argc, Janet* argv);

        // --- Implemented in zelph_to_janet.cpp ---

        static bool     split_path_marker(const std::string& token, std::string& base, std::string& mode);
        static bool     atom_text(Janet arg, std::string& text);
        static bool     is_path_ast(Janet node);
        static bool     writes_path_marker(Janet node);
        static NestedOp nested_condition_op(Janet node, const bool into_rules = true);
        static NestedOp misplaced_condition_op(Janet node, const bool in_condition, const bool skip_rules = false, const bool skip_mentions = false);
        NestedOp        set_literal_op(Janet node, const bool in_set = false) const;
        bool            denotes_zelph_name(Janet token, const char* name) const;
        bool            denotes_opaque_predicate(Janet token) const;
        bool            focused_refutation(Janet node, const bool in_rule) const;
        bool            writes_refuted_marking(Janet node) const;
        static bool     takes_reference(Janet node);
        NestedOp        explicit_condition_op(const Janet* parts, const int32_t count) const;
        NestedOp        explicit_condition_op(Janet node) const;
        NestedOp        written_out_condition_op(Janet node) const;
        bool            reads_complete(const std::string& code) const;
        std::string     build_smart_call(const std::string& func_name, const std::vector<Janet>& args, const std::string& role = "", const bool with_fact = false, const std::string& focused_code = "") const;
        std::string     transform_arg(Janet arg_tuple) const;

        // Set while the code for a QUERY is generated. A ground path within a
        // query functions as a reachability test, just as it does in a rule;
        // therefore, the guard that refuses one in a statement that asserts
        // is not emitted (janet_cfun_zelph_path_guard).
        mutable bool _query_code = false;

        // Set while generating the code for a statement that CLAIMS: a
        // statement entered outside a rule, a query and a command's
        // pattern, and a comma-separated list lacking variables, except
        // within a rule the statement mentions. A refuted marking it
        // writes anywhere is the refutation that "(F) ~ refuted" on its own
        // line is (build_smart_call).
        mutable bool _claim_code = false;
    };
}
