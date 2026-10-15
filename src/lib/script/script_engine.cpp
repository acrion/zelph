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
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"

#include <janet.h>
#include <janetconf.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace zelph;

// --- Static Helper Functions for Janet/zelph Bridge ---

int ScriptEngine::zelph_node_compare(void* p1, void* p2)
{
    network::Node n1 = *static_cast<network::Node*>(p1);
    network::Node n2 = *static_cast<network::Node*>(p2);
    return (n1 > n2) ? 1 : ((n1 < n2) ? -1 : 0);
}

int ScriptEngine::zelph_node_hash(void* p, size_t size)
{
    (void)size;
    network::Node n = *static_cast<network::Node*>(p);
    return static_cast<int32_t>(n ^ (n >> 32));
}

void ScriptEngine::zelph_node_tostring(void* p, JanetBuffer* buffer)
{
    network::Node n = *static_cast<network::Node*>(p);
    std::string   s = (Impl::s_instance && Impl::s_instance->_n) ? Impl::s_instance->_n->format(n) : ("<zelph/node " + std::to_string(n) + ">");
    janet_buffer_push_bytes(buffer, (const uint8_t*)s.c_str(), (int32_t)s.size());
}

const JanetAbstractType ScriptEngine::zelph_node_type = {
    "zelph/node",
    nullptr,             // gc
    nullptr,             // gcmark
    nullptr,             // get
    nullptr,             // put
    nullptr,             // marshal
    nullptr,             // unmarshal
    zelph_node_tostring, // tostring
    zelph_node_compare,  // compare
    zelph_node_hash,     // hash
    nullptr,             // next
    nullptr,             // call
    nullptr,             // length
    nullptr,             // bytes
};

Janet ScriptEngine::zelph_wrap_node(network::Node n)
{
    network::Node* ptr = (network::Node*)janet_abstract(&zelph_node_type, sizeof(network::Node));
    *ptr               = n;
    return janet_wrap_abstract(ptr);
}

network::Node ScriptEngine::zelph_unwrap_node(Janet val)
{
    if (janet_checktype(val, JANET_ABSTRACT))
    {
        void* abstract = janet_unwrap_abstract(val);
        if (janet_abstract_type(abstract) == &zelph_node_type)
        {
            return *static_cast<network::Node*>(abstract);
        }
    }
    if (janet_checktype(val, JANET_NUMBER))
    {
        return (network::Node)janet_unwrap_number(val);
    }
    return 0;
}

ScriptEngine::Impl* ScriptEngine::Impl::s_instance = nullptr;

ScriptEngine::ScriptEngine(network::Reasoning* reasoning)
    : _pImpl(new Impl(reasoning))
{
}

ScriptEngine::~ScriptEngine()
{
    drop_inline_janet(); // before the VM goes, like the roots ~Impl gives back
    delete _pImpl;
}

void ScriptEngine::initialize()
{
    _pImpl->init();
}

std::string ScriptEngine::get_janet_version()
{
    return JANET_VERSION;
}

void ScriptEngine::toggle_janet_logging()
{
    _pImpl->_log_janet_functions = !_pImpl->_log_janet_functions;
}

std::string ScriptEngine::get_janet_logging_status() const
{
    return _pImpl->_log_janet_functions ? "enabled" : "disabled";
}

void ScriptEngine::process_janet(const std::string& code, bool is_zelph_ast)
{
    const bool preloaded = _pImpl->_scoped_vars_preloaded;
    if (_pImpl->_scoped_vars_preloaded)
        _pImpl->_scoped_vars_preloaded = false; // scope prepared by inline-keyword expansion
    else
        _pImpl->_scoped_variables.clear();

    // The mentions of a parsed statement are marked at its end, provided it
    // constitutes a claim (Impl::ParsedStatement); a nested statement
    // maintains its own set of mentions. The variables of an island were
    // resolved before the statement started and are considered the
    // statement's.
    Impl::ParsedStatement statement;
    statement.asks = preloaded && _pImpl->has_scoped_variables();
    if (is_zelph_ast) statement.island_created = std::exchange(_pImpl->_island_created, {});
    const Impl::StatementScope current(_pImpl->_statement, is_zelph_ast ? &statement : nullptr);
    const Impl::AsideScope     aside(_pImpl->_aside, is_zelph_ast ? &statement.aside : nullptr);
    const Impl::AsideScope     mentioned_aside(_pImpl->_mention_aside, nullptr);

    // The scratch cluster that zelph/claim-text leaves active for the claim
    // at the end of the statement vanishes if the statement fails at any
    // point; a statement that succeeds merges it below.
    struct ClaimScratch
    {
        Impl* const impl;
        ~ClaimScratch()
        {
            try
            {
                impl->end_claim_scratch(false);
            }
            catch (...)
            {
            }
        }
    } const claim_scratch{_pImpl};

    // Parser-generated code builds a statement's SUBTERMS using the same
    // zelph/fact calls as those at the top level, and a call is unable to
    // discern whether its fact resides within a rule mentioned by the
    // statement or within a query: "x documents ((a p b) => (c q d))" says
    // nothing about `a p b`. Thus, the claim of a parsed statement
    // originates at the node to which the statement EVALUATES (below) and
    // proceeds by walking through what is nested within it, whereas the
    // claim inside zelph/fact belongs to a user's Janet block -- which this
    // is when is_zelph_ast is false.
    Impl::BlockScope block(_pImpl->_in_janet_block, !is_zelph_ast);

    const Impl::EvaluationScope evaluation(*_pImpl);

    Janet out;
    int   status = janet_dostring(_pImpl->_janet_env, code.c_str(), "zelph-script", &out);

    // A zelph statement is compiled to Janet, so Janet's frames name code the
    // user never wrote -- "in <cfunction>", "in thunk [zelph-script] on line 1,
    // column 32". They came out AHEAD of the handler's own report, so every
    // refusal the engine words carefully reached the user twice with two lines
    // of internals between the copies, and the second copy was the readable
    // one. The message is not lost by dropping them: it travels with the value
    // and is what "Error in line ..." prints.
    //
    // A user's own Janet block keeps its frames, and the difference is exactly
    // whose code they name -- "in f [zelph-script] on line 1, column 13" is
    // the function they wrote. The cost of drawing the line here is an inline
    // keyword island inside a statement: an error the handler raises keeps its
    // message and loses its frames.
    const std::string trace = _pImpl->take_err_trace();
    if (!is_zelph_ast && !trace.empty()) std::fputs(trace.c_str(), stderr);

    if (status != JANET_SIGNAL_OK)
    {
        // Throw a C++ exception so the error propagates correctly through import
        // chains and other nested call contexts (e.g. .import, process_file).
        // An error value that is not a string or a buffer is printed by
        // format_janet, the same output that .import of a .janet file prints
        // for such cases. Previously, `%(error {:a 1})` and a
        // `zelph/run-script` whose script raised a struct were reported as a
        // bare "Janet error".
        //
        // A signal distinct from an error stops the code identically, since
        // nothing resumes execution, and its value does not constitute a
        // message: `%(yield 1)` printed `Error in line "%(yield 1)": 1`.
        // This signal is named in the report, accompanied by its value as the
        // echo would print it. janet_dostring merely indicates that the code
        // encountered failure. The status of the `fiber` that ceased
        // execution leads Janet's stack trace, as in "pending: 1", and the
        // status of a `fiber` corresponds to the signal it raised
        // (janet_status_names, janet_signal_names).
        std::string err;
        if (!is_zelph_ast)
        {
            std::string head = trace.substr(0, trace.find(": "));
            if (head.starts_with("\x1b[31m")) head.erase(0, 5);
            for (int signal = JANET_SIGNAL_DEBUG; signal <= JANET_SIGNAL_USER9 && trace.find(": ") != std::string::npos; ++signal)
            {
                if (head != janet_status_names[signal]) continue;
                janet_gcroot(out);
                const std::string value = Impl::format_janet(out);
                janet_gcunroot(out);
                err = std::string("signal :") + janet_signal_names[signal] + " with the value " + value
                    + ", not an error: nothing resumes the code of a line, so it stopped there";
                break;
            }
        }

        if (err.empty())
        {
            if (janet_checktype(out, JANET_STRING))
                err = reinterpret_cast<const char*>(janet_unwrap_string(out));
            else if (janet_checktype(out, JANET_BUFFER))
            {
                JanetBuffer* b = janet_unwrap_buffer(out);
                err            = std::string(reinterpret_cast<const char*>(b->data), b->count);
            }
            else
            {
                janet_gcroot(out);
                err = Impl::format_janet(out);
                janet_gcunroot(out);
            }
        }

        // After an error, whatever the code wrote remains in the graph, a rule
        // in force included, and is checked as it is after successful code
        // execution; the refusal follows the error.
        if (const std::string refused = evaluation.settle(); !refused.empty()) err += "\n" + refused;
        throw std::runtime_error(err);
    }
    else
    {
        if (is_zelph_ast)
        {
            if (!statement.asks)
            {
                for (const auto& created : statement.mentions)
                    _pImpl->mark_mentioned_rules(created);
                for (const auto& mention : statement.statement_mentions)
                    _pImpl->mark_mention(mention);
            }

            // The statement now holds what its islands returned, and the `=>`
            // facts they authored are verified before the line echoes, just as
            // its other refusals are.
            if (const std::string refused = evaluation.settle(); !refused.empty()) throw std::runtime_error(refused);

            // A pattern is what a query constructed under a condition
            // operator becomes, and it must be established as such
            // before the query is echoed and evaluated -- marked
            // afterwards, the query would find the operand it had just
            // built (zelph/query-text). Similarly, when the statement
            // evaluates to no node: a focus on "negation" returns the
            // name.
            network::Node n = zelph_unwrap_node(out);
            if (!statement.query_created.empty()) _pImpl->_n->mark_query_operands(n, statement.query_created, statement.query_refutations);

            // How far the claim of a statement walks (below).
            const auto reach = [&statement](const network::Node node, bool)
            {
                const bool wrote = network::Zelph::is_hash(node)
                                || std::find(statement.collections.begin(), statement.collections.end(), node) != statement.collections.end();
                return wrote ? network::Zelph::ClaimStep::Enter : network::Zelph::ClaimStep::Claim;
            };

            // The query's ground markings, now claimed with the query since
            // it stands complete, as "(F) ~ refuted" on a line by itself is.
            for (const network::Node refuted : statement.query_refutations)
                _pImpl->_n->mark_refuted_fact(refuted);

            // What a rule statement wrote beside its rule is claimed as the
            // line of that fact would claim it: following
            // "(x says *(a p b)) => (c q d)", the fact `x says (a p b)` was
            // answered, and `a p b` stayed the rule's pattern, hence the rule
            // never fired; when typed as two lines, it did fire.
            //
            // The same applies to any fact a focus writes, on a line of its
            // own just like in a rule's slot, even if a rule previously wrote
            // it as a pattern: following "(m n o) => (x says (a p b))", the
            // line "x says *(a p b)" claimed `a p b` and left
            // `x says (a p b)` the rule's pattern. A statement that evaluates
            // to a name also claims that fact, as "x *says y" does.
            //
            // These facts are claimed before the line's echo, just as the
            // statement is claimed before it. When such a claim was
            // refused, the line had already echoed, and subsequently, the
            // error followed.
            if (!statement.asks)
            {
                for (const network::Node fact : statement.aside)
                {
                    if (!_pImpl->_n->exists(fact)) continue;
                    _pImpl->_n->unmark_rule_pattern(fact);
                    _pImpl->_n->claim_nested(fact, reach);
                }
            }

            if (n)
            {
                // A statement typed at the top level is a CLAIM, and a claim
                // revokes the pattern-only status the same statement may have
                // acquired by appearing in a rule. Nothing else can revoke it:
                // the rule construction itself runs through the same fact()
                // calls, so only the top level knows that this one was meant.
                //
                // It claims the nested facts as well, outside a `=>`,
                // regardless of which was written first: following
                // `(a p b) => (c q d)`, `(a p b) q z` causes `a p b` to be
                // treated as data and fires the rule, just as it does when
                // the order is reversed. A query claims nothing; the
                // variables of a rule the statement mentions do not render
                // it one. The walk proceeds into what the statement wrote:
                // its facts, its set constants, and the collections created
                // by its literals, not any it merely names.
                //
                // A rule statement claims its rule, also when a focus causes it
                // to evaluate to a part of the rule: `*(a p b) => (c q d)`
                // asserted `a p b`, which fired the rule it had just written.
                //
                // A refutation claims its marking, which zelph/refute has
                // already claimed, and not the fact it evaluates to: following
                // `(a p b) => (c q d)`, the line `¬(a p b)` removed `a p b`
                // from the rule's patterns, and after its marking was pruned,
                // `a p b` answered and fired the rule.
                const network::Node claimed = statement.rule != 0 ? statement.rule : n;
                const bool          refuted = statement.rule == 0 && std::find(statement.refuted.begin(), statement.refuted.end(), n) != statement.refuted.end();
                if (!statement.asks && !refuted)
                {
                    _pImpl->_n->unmark_rule_pattern(claimed);
                    _pImpl->_n->claim_nested(claimed, reach);

                    // A ground comma list claims every one of its members,
                    // just as the line of each member does. The claim
                    // beginning at the conjunction set, which lacks a fact
                    // structure, never reached them: following
                    // "(a p b) => (c q d)", the line "a p b, x q y" left
                    // `a p b` a pattern of the rule, which never fired, and
                    // "a p b, (a p b) ~ refuted" was accepted and lost the
                    // assertion without a word.
                    //
                    // A member that constitutes a comma list the statement
                    // wrote causes the statement to assert its members, just
                    // as its line would: the claim originated at a
                    // conjunction set too, and following the rule stated
                    // earlier, "(a p b, j r s), x q y" treated `a p b` as a
                    // pattern. A conjunction set that the statement only
                    // references, a rule's condition set among them, is not
                    // entered.
                    const auto is_list = [this](const network::Node node)
                    { return _pImpl->_n->check_fact(node, _pImpl->_n->core.IsA, {_pImpl->_n->core.Conjunction}).is_known(); };
                    std::vector<network::Node> lists;
                    if (statement.rule == 0 && is_list(n)) lists.push_back(n);
                    while (!lists.empty())
                    {
                        network::adjacency_set members;
                        const network::Node    list = lists.back();
                        lists.pop_back();
                        if (!_pImpl->_n->condition_set_members(list, members)) continue;
                        for (const network::Node member : members)
                        {
                            if (reach(member, false) == network::Zelph::ClaimStep::Enter && !network::Zelph::is_hash(member) && is_list(member))
                            {
                                lists.push_back(member);
                                continue;
                            }
                            _pImpl->_n->unmark_rule_pattern(member);
                            _pImpl->_n->claim_nested(member, reach);
                        }
                    }
                }

                // A query's echo corresponds to its condition, expressed
                // exactly as it is typed: a negation on its own line prints
                // as "¬(X p b)", which re-enters as the identical query.
                // A ground comma list, which asserts each of its members,
                // echoes back as the very same comma list.
                const bool asks = _pImpl->has_scoped_variables();
                if (_pImpl->echo_enabled())
                {
                    const network::Zelph* const z = _pImpl->_n;
                    std::string                 output;
                    if (asks || z->check_fact(n, z->core.IsA, {z->core.Conjunction}).is_known())
                        string::condition_to_string(_pImpl->_n, output, _pImpl->_n->lang(), n, 3);
                    else
                        string::node_to_string(_pImpl->_n, output, _pImpl->_n->lang(), n, 3);
                    if (!output.empty() && output != "??") _pImpl->_n->out(string::unmark_identifiers(output), true);
                }

                // The variables within a rule statement belong to the rule
                // itself, so the statement asks nothing: when evaluated as a
                // query, it answered with a rule its own pattern matched, and
                // with the data its focused consequence matched.
                if (asks && statement.rule == 0)
                {
                    _pImpl->_n->apply_rule(0, n);
                }
            }

            _pImpl->end_claim_scratch(true);

            // Once the query has been answered, what it wrote that holds its
            // variables goes (Zelph::drop_query_text). The input focus first
            // examines the statement's structure: reduced following the
            // removal, its components no longer had the fact that encompassed
            // them, and the subterms within `(&6 * &7) = X` became independent
            // anchors.
            if (statement.rule == 0 && !statement.query_created.empty())
            {
                _pImpl->_n->settle_input_capture();
                _pImpl->_n->drop_query_text(statement.query_created, statement.query_declared);
            }
        }
        else
        {
            if (const std::string refused = evaluation.settle(); !refused.empty()) throw std::runtime_error(refused);

            if (_pImpl->echo_enabled() && !janet_checktype(out, JANET_NIL))
            {
                _pImpl->_n->out(Impl::format_janet(out), true);
            }
        }
    }
}

void ScriptEngine::run_janet_script(const std::string& path, const std::vector<std::string>& args)
{
    _pImpl->clear_scoped_variables();
    const Impl::StatementScope  outside(_pImpl->_statement, nullptr); // marks at once
    const Impl::EvaluationScope evaluation(*_pImpl);

    Janet runner;
    if (janet_resolve(_pImpl->_janet_env, janet_csymbol("zelph/run-script"), &runner) != JANET_BINDING_DEF
        || !janet_checktype(runner, JANET_FUNCTION))
    {
        throw std::runtime_error("Internal error: zelph/run-script is not initialized");
    }
    JanetFunction* fn = janet_unwrap_function(runner);

    // Block the GC while assembling the call: neither the freshly created
    // strings nor the fiber are rooted yet, and any janet allocation could
    // otherwise trigger a collection.
    const int gc_handle = janet_gclock();

    std::vector<Janet> jargs;
    jargs.reserve(args.size() + 1);
    jargs.push_back(janet_cstringv(path.c_str()));
    for (const auto& a : args)
        jargs.push_back(janet_cstringv(a.c_str()));

    JanetFiber* fiber = janet_fiber(fn, 64, static_cast<int32_t>(jargs.size()), jargs.data());
    if (!fiber)
    {
        janet_gcunlock(gc_handle);
        throw std::runtime_error("Internal error: could not create fiber for zelph/run-script");
    }
    fiber->env = _pImpl->_janet_env;
    janet_gcroot(janet_wrap_fiber(fiber));
    janet_gcunlock(gc_handle);

    bool  failed = false;
    Janet out    = janet_wrap_nil();

#ifdef JANET_EV
    // Run the script as the root task of the Janet event loop - the same way
    // the janet CLI runs scripts (see janet's shell.c). This is what makes
    // ev/... usable: ev/spawn-thread, thread channels, timers. janet_loop
    // returns once the loop has drained, i.e. the root fiber has finished
    // AND all spawned threads/tasks are done.
    janet_schedule(fiber, janet_wrap_nil());
    janet_loop();

    if (janet_fiber_status(fiber) == JANET_STATUS_ERROR)
    {
        // The event loop has already printed the stacktrace to stderr;
        // propagate a concise error to the REPL/import chain. last_value is
        // the public JanetFiber field backing the fiber/last-value builtin:
        // after completion it holds the return value or the error payload.
        failed = true;
        out    = fiber->last_value;
    }
#else
    // No Janet event loop on this platform (e.g. Emscripten): run the script
    // synchronously; ev/... is not available here.
    JanetSignal sig = janet_continue(fiber, janet_wrap_nil(), &out);
    if (sig != JANET_SIGNAL_OK)
    {
        janet_stacktrace(fiber, out);
        failed = true;
    }
#endif

    _pImpl->flush_err_trace();

    // Extract the error text BEFORE unrooting the fiber: 'out' is only
    // reachable through the rooted fiber (last_value).
    std::string err;
    if (failed)
    {
        err = "Janet error";
        if (janet_checktype(out, JANET_STRING))
            err = reinterpret_cast<const char*>(janet_unwrap_string(out));
        else if (janet_checktype(out, JANET_BUFFER))
        {
            JanetBuffer* b = janet_unwrap_buffer(out);
            err            = std::string(reinterpret_cast<const char*>(b->data), b->count);
        }
        else
        {
            err = Impl::format_janet(out);
        }
    }

    janet_gcunroot(janet_wrap_fiber(fiber));

    const std::string refused = evaluation.settle(); // as the `process_janet` function performs
    if (failed)
        throw std::runtime_error("Script '" + path + "' failed: " + err + (refused.empty() ? "" : "\n" + refused));
    if (!refused.empty()) throw std::runtime_error(refused);
}

std::string ScriptEngine::Impl::EvaluationScope::settle() const
{
    return _impl._evaluation_depth == 1 ? _impl._n->settle_fact_rules() : std::string{};
}

// Helper function to evaluate a Janet expression and return a Node (used by prune commands)
network::Node ScriptEngine::evaluate_expression(const std::string& janet_code, const bool quiet, const bool resolving_pattern)
{
    Impl::BlockScope resolving(_pImpl->_resolving_pattern, resolving_pattern);
    Impl::BlockScope command(_pImpl->_resolving_command, resolving_pattern);

    if (_pImpl->_scoped_vars_preloaded)
        _pImpl->_scoped_vars_preloaded = false; // scope prepared by inline-keyword expansion
    else
        _pImpl->_scoped_variables.clear();

    // A pattern that a command evaluates does not constitute a user's
    // statement, and the mentions within it mark nothing: what they recorded
    // is discarded, as is the list of what its islands built.
    _pImpl->_island_created.clear();
    Impl::ParsedStatement      pattern;
    const Impl::StatementScope current(_pImpl->_statement, &pattern);

    Janet out;
    int   status = janet_dostring(_pImpl->_janet_env, janet_code.c_str(), "eval_expr", &out);

    // A speculative evaluation yields a normal outcome that the caller
    // handles, so its trace is dropped; anything else is code the caller is
    // not silencing.
    const std::string trace = _pImpl->take_err_trace();
    if (!quiet && !trace.empty()) std::fputs(trace.c_str(), stderr);

    if (status != JANET_SIGNAL_OK)
    {
        std::string err = "Janet error";
        if (janet_checktype(out, JANET_STRING))
            err = reinterpret_cast<const char*>(janet_unwrap_string(out));
        else if (janet_checktype(out, JANET_BUFFER))
        {
            JanetBuffer* b = janet_unwrap_buffer(out);
            err            = std::string(reinterpret_cast<const char*>(b->data), b->count);
        }
        throw std::runtime_error(err);
    }
    return zelph_unwrap_node(out);
}

void ScriptEngine::set_script_args(const std::vector<std::string>& args)
{
    JanetArray* jargs = janet_array(static_cast<int32_t>(args.size()));
    for (const auto& arg : args)
    {
        janet_array_push(jargs, janet_cstringv(arg.c_str()));
    }
    janet_table_put(_pImpl->_janet_env, janet_ckeywordv("args"), janet_wrap_array(jargs));
}

void ScriptEngine::clear_scoped_variables()
{
    _pImpl->clear_scoped_variables();
}

void ScriptEngine::discard_compiled_nets()
{
    std::lock_guard<std::mutex> lock(_pImpl->_state_mutex);
    for (auto& net : _pImpl->_neural_nets)
        net.reset();
}

void ScriptEngine::set_import_handler(ImportHandler handler)
{
    _pImpl->_import_handler = std::move(handler);
}

void ScriptEngine::set_command_handler(CommandHandler handler)
{
    _pImpl->_command_handler = std::move(handler);
}

void ScriptEngine::set_echo_predicate(EchoPredicate predicate)
{
    _pImpl->_echo_predicate = std::move(predicate);
}

void ScriptEngine::set_auto_run_predicate(AutoRunPredicate predicate)
{
    _pImpl->_auto_run_predicate = std::move(predicate);
}

void ScriptEngine::set_full_graph_guard(FullGraphGuard guard)
{
    _pImpl->_full_graph_guard = std::move(guard);
}

bool ScriptEngine::has_keyword(const std::string& keyword) const
{
    // Block keywords only: inline keywords are detected inside statements
    // by expand_inline_keywords, never as a line-starting token.
    const auto it = _pImpl->_keyword_handlers.find(keyword);
    return it != _pImpl->_keyword_handlers.end() && !it->second.inline_mode;
}

bool ScriptEngine::invoke_keyword(const std::string& keyword, const std::string& text, const bool force)
{
    auto it = _pImpl->_keyword_handlers.find(keyword);
    if (it == _pImpl->_keyword_handlers.end() || it->second.inline_mode)
        throw std::runtime_error("No handler registered for keyword '" + keyword + "'");

    _pImpl->_scoped_variables.clear();

    // A zelph/fact belonging to a handler constitutes a claim, just as it
    // does within a `%` block. Without this scope, the handler
    // inherited whatever the surrounding input had set: a ground rule
    // condition that the handler asserted stayed a pattern following
    // `.import`, transformed into data when under `%(zelph/import ...)`,
    // and at the prompt was determined by whether the rule or the handler's
    // fact was encountered first.
    Impl::BlockScope            block(_pImpl->_in_janet_block, true);
    const Impl::EvaluationScope evaluation(*_pImpl);

    // When a handler returns, its `=>` facts undergo verification, just as
    // a block's do (process_janet). A veto performs no checks: the handler
    // is invoked once more with additional text, and the next evaluation
    // checks what was written.
    Janet             result;
    Impl::HandlerCall call = Impl::HandlerCall::Dispatched;
    try
    {
        call = _pImpl->call_keyword_handler(keyword, it->second.handler, text, force, result);
    }
    catch (const std::exception& ex)
    {
        const std::string refused = evaluation.settle();
        if (refused.empty()) throw;
        throw std::runtime_error(std::string(ex.what()) + "\n" + refused);
    }
    if (call == Impl::HandlerCall::Incomplete) return false;
    if (const std::string refused = evaluation.settle(); !refused.empty()) throw std::runtime_error(refused);

    // String results are emitted verbatim, line by line, through the output
    // handler (so they reach OutputCollector in tests and the REPL alike).
    // Other non-nil results are emitted via their Janet description.
    if (janet_checktype(result, JANET_STRING) || janet_checktype(result, JANET_BUFFER))
    {
        std::string text;
        if (janet_checktype(result, JANET_STRING))
            text = reinterpret_cast<const char*>(janet_unwrap_string(result));
        else
        {
            JanetBuffer* b = janet_unwrap_buffer(result);
            text           = std::string(reinterpret_cast<const char*>(b->data), b->count);
        }
        std::istringstream iss(text);
        for (std::string l; std::getline(iss, l);)
            _pImpl->_n->out(l, true);
    }
    else if (!janet_checktype(result, JANET_NIL))
    {
        _pImpl->_n->out(Impl::format_janet(result), true);
    }

    return true;
}

// Janet's built-in reader is invoked, not emulated. A scanner capable of
// recognizing double-quoted strings and '#' comments took a '#' or a
// bracket within a backtick string as syntactic, causing
// `%(zelph/out `J # B`)` to remain open and swallow the following lines.
// Pending signifies unfinished. A form read in full and a syntax error both
// indicate completeness, so that process_janet reports the error once the
// code runs.
//
// An expression extending across multiple lines is read one line at a time
// by a single reader, persisting for the duration that the expression
// remains open. Previously, passing the whole buffer to a new reader for
// each line caused both time and memory usage to increase proportionally to
// the square of the expression's length: the reader allocates memory for
// each segment it reads, and that memory remains unreclaimed until the VM
// runs again, which does not occur until the expression is complete. The
// reader is a Janet abstract and remains rooted during its lifetime, since a
// command embedded between two lines might execute Janet code, and a
// collection at that point would otherwise free the memory held by the
// reader.
namespace
{
    // A single line along with the newline terminating it, which closes
    // a symbol or a comment that the line ends with.
    JanetParserStatus read_line(JanetParser* reader, const std::string& line)
    {
        for (const char c : line)
        {
            janet_parser_consume(reader, static_cast<uint8_t>(c));

            // Upon encountering an error, the reader refuses the subsequent
            // byte by triggering a panic.
            if (janet_parser_status(reader) == JANET_PARSE_ERROR) return JANET_PARSE_ERROR;
        }
        janet_parser_consume(reader, '\n');
        return janet_parser_status(reader);
    }
}

bool ScriptEngine::start_inline_janet(const std::string& line)
{
    drop_inline_janet();

    auto* reader = static_cast<JanetParser*>(janet_abstract(&janet_parser_type, sizeof(JanetParser)));
    janet_parser_init(reader);
    janet_gcroot(janet_wrap_abstract(reader));
    _pImpl->_inline_reader = reader;

    return continue_inline_janet(line);
}

bool ScriptEngine::continue_inline_janet(const std::string& line)
{
    if (read_line(_pImpl->_inline_reader, line) == JANET_PARSE_PENDING) return false;

    drop_inline_janet();
    return true;
}

void ScriptEngine::drop_inline_janet()
{
    if (_pImpl->_inline_reader == nullptr) return;

    // When unrooted, the reader becomes garbage, and the next
    // collection releases it together with all data it has read.
    janet_gcunroot(janet_wrap_abstract(_pImpl->_inline_reader));
    _pImpl->_inline_reader = nullptr;
}

// Determine whether a zelph statement is complete.
// A statement is complete when:
//   - All parentheses and braces are balanced
//   - The top-level token count indicates a full triple (>= 3),
//     OR it is exactly 1 token that is NOT a parenthesized group.
//     (A single paren group like "(*{...} ~ conj)" is a subject waiting for P and O.)
//     A bare atom, set {}, or list <> with count == 1 is a valid standalone expression.
//   - "Top-level token" = a contiguous non-whitespace chunk at paren/brace depth 0.
//     Note: < > (list delimiters) and all other chars are treated as plain characters
//     for token boundaries; only () and {} affect depth.
//   - `code` is received devoid of its comments (string::strip_comments),
//     meaning that a '#' here constitutes part of a name, just as it does
//     within the grammar.
//   - `balanced` reports whether each bracket and quotation mark has a
//     corresponding closing counterpart.
static bool looks_complete(const std::string& code, bool& balanced)
{
    int  depth     = 0;
    bool in_string = false;
    bool escape    = false;
    bool stray     = false; // a closing bracket without a corresponding opening one
    balanced       = false;

    int         top_tokens      = 0;
    bool        in_top_token    = false;
    std::size_t second_token_at = std::string::npos; // the first byte of the second top-level token

    for (std::size_t i = 0; i < code.size(); ++i)
    {
        const char c = code[i];

        if (escape)
        {
            escape = false;
            continue;
        }

        if (in_string)
        {
            if (c == '\\')
            {
                escape = true;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }

        if (c == '"')
        {
            in_string = true;
            if (depth == 0 && !in_top_token)
            {
                top_tokens++;
                in_top_token = true;
                if (top_tokens == 2) second_token_at = i;
            }
            continue;
        }

        bool is_ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');

        if (c == '(' || c == '{')
        {
            if (depth == 0 && !in_top_token)
            {
                top_tokens++;
                in_top_token = true;
                if (top_tokens == 2) second_token_at = i;
            }
            depth++;
        }
        else if (c == ')' || c == '}')
        {
            depth--;
            if (depth < 0) stray = true;
            if (depth == 0) in_top_token = false;
        }
        else if (is_ws)
        {
            if (depth == 0) in_top_token = false;
        }
        else
        {
            if (depth == 0 && !in_top_token)
            {
                top_tokens++;
                in_top_token = true;
                if (top_tokens == 2) second_token_at = i;
            }
        }
    }

    // A closing bracket that has no corresponding opening one cannot be
    // balanced by any subsequent line, so waiting for one caused every
    // following line to be absorbed: "a p b)" took the next statement into
    // itself, and the input concluded while still within it. The statement
    // is complete as it stands, and the parser says what is wrong with it.
    if (stray) return true;

    balanced = depth == 0 && !in_string;
    if (!balanced) return false;
    if (top_tokens == 0) return false;

    const size_t first_char_idx = code.find_first_not_of(" \t\r\n\v\f");
    const char   first_c        = first_char_idx == std::string::npos ? '\0' : code[first_char_idx];

    // Does a token starting at `at` begin with "¬"? Considered across the
    // entire pair of bytes, C2 AC: the lead byte C2 alone is common to all
    // characters ranging from U+0080 to U+00BF, so a single name like °C
    // counted as a finished statement, and the subsequent line commenced a
    // new one.
    const auto starts_negation = [&code](const std::size_t at)
    { return at < code.size() && code.compare(at, 2, "¬") == 0; };

    // Result-query prefix "? <statement>": '?' counts as the first
    // top-level token; the remainder must itself look complete -- a full
    // triple ("? S P O"), a self-fact with operand ("? :testprime &53"),
    // or a single bracketed value ("? (S P O)", "? $( ... )"). A lone
    // "? :pred" keeps accumulating like the native self-fact sugar.
    if (first_c == '?'
        && (first_char_idx + 1 >= code.size()
            || code[first_char_idx + 1] == ' ' || code[first_char_idx + 1] == '\t'
            || code[first_char_idx + 1] == '\n' || code[first_char_idx + 1] == '('
            || code[first_char_idx + 1] == '$'))
    {
        // Written WITHOUT the space -- "?(S P O)", "?$( ... ) ≡ $( ... )" --
        // the bracket opens inside the token the '?' started, so the whole
        // request counted as ONE token less than the spaced form and none of
        // the counts below could match: the statement looked unfinished
        // forever, although the character after '?' is one this very
        // condition accepts. Split it here and the two spellings are the same
        // question again. Balanced brackets are guaranteed (depth == 0 above).
        if (first_char_idx + 1 < code.size()
            && (code[first_char_idx + 1] == '(' || code[first_char_idx + 1] == '$'))
        {
            ++top_tokens;
            second_token_at = first_char_idx + 1;
        }
        const char second_token_first = second_token_at < code.size() ? code[second_token_at] : '\0';

        if (top_tokens >= 4) return true;
        if (top_tokens == 3 && second_token_first == ':') return true;
        if (top_tokens == 2
            && (second_token_first == '(' || second_token_first == '$'
                || second_token_first == '{' || second_token_first == '<'
                || starts_negation(second_token_at)))
            return true;
        return false;
    }

    if (top_tokens <= 2)
    {
        size_t first_char_idx = code.find_first_not_of(" \t\r\n\v\f");
        if (first_char_idx != std::string::npos)
        {
            char c = code[first_char_idx];
            // The symbol '$' permits a lone inline-keyword island
            // ($( ... )) as a complete statement -- the calculator
            // idiom. The symbol "¬" initiates a standalone complete
            // statement.
            if (top_tokens == 1 && (c == '{' || c == '<' || c == '*' || starts_negation(first_char_idx) || c == '$'))
            {
                return true;
            }

            // "≈net(a p b)" is one top-level token too, and it is not the
            // beginning of anything: `≈` is a condition operator, and the
            // transform refuses it in a plain statement. Without this the line
            // was never handed to the transform at all -- it looked unfinished,
            // so it was buffered and SWALLOWED THE NEXT LINE, and a script
            // containing one silently ran a statement nobody wrote. Tested on
            // the whole three-byte sequence rather than the lead byte, which
            // "≡", "⁺" and "∗" share.
            if (top_tokens == 1 && code.compare(first_char_idx, 3, "≈") == 0)
            {
                return true;
            }
            // Self-fact sugar ":pred X" is a complete statement of exactly
            // two top-level tokens (the operand doubles as subject and
            // object). A lone ":pred" keeps accumulating, so the operand
            // may follow on the next line.
            if (top_tokens == 2 && c == ':')
            {
                return true;
            }
        }
        return false;
    }
    return true;
}

// The scanner tracks chunks separated by whitespace, while the grammar counts
// values, and a single value may extend across multiple chunks: "<a b>", "¬
// (a p b)", and "≈net (a p b)" each constitute one value, as does "@{a b}",
// even though its first character is not among those the scanner looks for.
// Each of these was treated as the beginning of a statement and SWALLOWED THE
// NEXT LINE, as a "?" prefix in front of any of them did as well. A text the
// scanner deems incomplete is thus submitted to the grammar once its brackets
// and quotation marks are properly closed, with the grammar's interpretation
// determining completeness (Impl::reads_complete). The scanner continues to
// serve as the fast path: a line composed of three chunks is considered
// complete without requiring a parse.
bool ScriptEngine::is_zelph_complete(const std::string& code) const
{
    bool balanced = false;
    return looks_complete(code, balanced) || (balanced && _pImpl->reads_complete(code));
}
