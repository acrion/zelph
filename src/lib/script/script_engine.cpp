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
    if (_pImpl->_scoped_vars_preloaded)
        _pImpl->_scoped_vars_preloaded = false; // scope prepared by inline-keyword expansion
    else
        _pImpl->_scoped_variables.clear();

    // Parser-generated code builds a statement's SUBTERMS with the same
    // zelph/fact calls as its top level, and a subterm is not claimed by the
    // statement that names it: "x documents ((a p b) => (c q d))" says
    // nothing about `a p b`. So the revocation for a parsed statement stays
    // with the node the statement EVALUATES to (below), while the one inside
    // zelph/fact belongs to a user's Janet block -- which this is when
    // is_zelph_ast is false.
    Impl::BlockScope block(_pImpl->_in_janet_block, !is_zelph_ast);

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
    else
    {
        if (is_zelph_ast)
        {
            network::Node n = zelph_unwrap_node(out);
            if (n)
            {
                // A statement typed at the top level is a CLAIM, and a claim
                // revokes the pattern-only status the same statement may have
                // acquired by appearing in a rule. Nothing else can revoke it:
                // the rule construction itself runs through the same fact()
                // calls, so only the top level knows that this one was meant.
                if (!_pImpl->has_scoped_variables()) _pImpl->_n->unmark_rule_pattern(n);

                if (_pImpl->echo_enabled())
                {
                    std::string output;
                    string::node_to_string(_pImpl->_n, output, _pImpl->_n->lang(), n, 3);
                    if (!output.empty() && output != "??") _pImpl->_n->out(string::unmark_identifiers(output), true);
                }

                if (_pImpl->has_scoped_variables())
                {
                    _pImpl->_n->apply_rule(0, n);
                }
            }
        }
        else
        {
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

    if (failed)
        throw std::runtime_error("Script '" + path + "' failed: " + err);
}

// Helper function to evaluate a Janet expression and return a Node (used by prune commands)
network::Node ScriptEngine::evaluate_expression(const std::string& janet_code, const bool quiet, const bool resolving_pattern)
{
    Impl::BlockScope resolving(_pImpl->_resolving_pattern, resolving_pattern);

    if (_pImpl->_scoped_vars_preloaded)
        _pImpl->_scoped_vars_preloaded = false; // scope prepared by inline-keyword expansion
    else
        _pImpl->_scoped_variables.clear();

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

    Janet result;
    if (_pImpl->call_keyword_handler(keyword, it->second.handler, text, force, result) == Impl::HandlerCall::Incomplete)
        return false;

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

bool ScriptEngine::is_expression_complete(const std::string& code)
{
    int  depth      = 0;
    bool in_string  = false;
    bool escape     = false;
    bool in_comment = false;

    for (char c : code)
    {
        if (in_comment)
        {
            if (c == '\n') in_comment = false;
            continue;
        }

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

        if (c == '#')
        {
            in_comment = true;
            continue;
        }
        if (c == '"')
        {
            in_string = true;
            continue;
        }

        if (c == '(' || c == '[' || c == '{') depth++;
        if (c == ')' || c == ']' || c == '}') depth--;
    }

    return depth <= 0;
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
bool ScriptEngine::is_zelph_complete(const std::string& code)
{
    int  depth      = 0;
    bool in_string  = false;
    bool escape     = false;
    bool in_comment = false;

    int  top_tokens         = 0;
    bool in_top_token       = false;
    char second_token_first = '\0';

    for (char c : code)
    {
        if (in_comment)
        {
            if (c == '\n') in_comment = false;
            continue;
        }

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

        if (c == '#')
        {
            in_comment = true;
            continue;
        }
        if (c == '"')
        {
            in_string = true;
            if (depth == 0 && !in_top_token)
            {
                top_tokens++;
                in_top_token = true;
                if (top_tokens == 2 && second_token_first == '\0') second_token_first = c;
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
                if (top_tokens == 2 && second_token_first == '\0') second_token_first = c;
            }
            depth++;
        }
        else if (c == ')' || c == '}')
        {
            depth--;
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
                if (top_tokens == 2 && second_token_first == '\0') second_token_first = c;
            }
        }
    }

    if (depth != 0 || in_string) return false;
    if (top_tokens == 0) return false;

    const size_t first_char_idx = code.find_first_not_of(" \t\r\n\v\f");
    const char   first_c        = first_char_idx == std::string::npos ? '\0' : code[first_char_idx];

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
            second_token_first = code[first_char_idx + 1];
        }

        if (top_tokens >= 4) return true;
        if (top_tokens == 3 && second_token_first == ':') return true;
        if (top_tokens == 2
            && (second_token_first == '(' || second_token_first == '$'
                || second_token_first == '{' || second_token_first == '<'
                || second_token_first == '\xC2'))
            return true;
        return false;
    }

    if (top_tokens <= 2)
    {
        size_t first_char_idx = code.find_first_not_of(" \t\r\n\v\f");
        if (first_char_idx != std::string::npos)
        {
            char c = code[first_char_idx];
            // '$' admits a lone inline-keyword island ($( ... )) as a
            // complete statement -- the calculator idiom. '\xC2' is the first
            // byte of "¬", which leads a complete statement of its own.
            if (top_tokens == 1 && (c == '{' || c == '<' || c == '*' || c == '\xC2' || c == '$'))
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
