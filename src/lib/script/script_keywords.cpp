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

#include <janet.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zelph
{
    // Shared invocation for both keyword kinds: pcall with error wrapping and
    // the :incomplete veto protocol. Under force a veto is an error (EOF in
    // scripts for block keywords; see expand_inline_keywords for islands).
    ScriptEngine::Impl::HandlerCall ScriptEngine::Impl::call_keyword_handler(const std::string& name, const Janet handler, const std::string& text, const bool force, Janet& result)
    {
        JanetFunction* f   = janet_unwrap_function(handler);
        Janet          arg = janet_cstringv(text.c_str());

        if (pcall_rooted(f, 1, &arg, &result) != JANET_SIGNAL_OK)
        {
            std::string err = "Janet error in handler for keyword '" + name + "'";
            if (janet_checktype(result, JANET_STRING))
                err += ": " + std::string(reinterpret_cast<const char*>(janet_unwrap_string(result)));
            else if (janet_checktype(result, JANET_BUFFER))
            {
                JanetBuffer* b = janet_unwrap_buffer(result);
                err += ": " + std::string(reinterpret_cast<const char*>(b->data), b->count);
            }
            throw std::runtime_error(err);
        }

        if (janet_checktype(result, JANET_KEYWORD))
        {
            const uint8_t* kw = janet_unwrap_keyword(result);
            if (std::string(reinterpret_cast<const char*>(kw)) == "incomplete")
            {
                if (!force) return HandlerCall::Incomplete;
                throw std::runtime_error("Keyword block for '" + name + "' is incomplete");
            }
        }

        return HandlerCall::Dispatched;
    }

    // Expand inline keywords ("expression islands") in a zelph statement.
    // Runs in parse_zelph_to_janet BEFORE the PEG, so every consumer of zelph
    // syntax (REPL statements, imported scripts, command patterns) gets the
    // expansion. Each island's handler receives the raw text between the
    // delimiters and must return a zelph/node; the node is bound to a fresh
    // Janet name and spliced back via the existing unquote mechanism
    // (",$inlineN"), which makes islands valid in every value position.
    //
    // Close-delimiter search is a raw substring scan: the HANDLER owns the
    // island's grammar and arbitrates false splits via the :incomplete veto
    // (a ')' nested inside the island extends it to the next ')'). Handlers
    // must therefore be side-effect-free until they accept their input --
    // the contract sparql.zph already follows.
    //
    // Openers are not matched inside quoted atoms or comments (mirroring
    // is_zelph_complete's scanning rules).
    std::string ScriptEngine::Impl::expand_inline_keywords(const std::string& input)
    {
        _scoped_vars_preloaded = false;

        std::vector<std::pair<const std::string*, const KeywordEntry*>> inline_kws;
        for (const auto& [open, entry] : _keyword_handlers)
            if (entry.inline_mode) inline_kws.push_back({&open, &entry});
        if (inline_kws.empty()) return input;

        std::string out;
        out.reserve(input.size());

        bool   in_string  = false;
        bool   escape     = false;
        bool   in_comment = false;
        size_t island     = 0;

        size_t i = 0;
        while (i < input.size())
        {
            const char c = input[i];

            if (in_comment)
            {
                if (c == '\n') in_comment = false;
                out += c;
                ++i;
                continue;
            }
            if (escape)
            {
                escape = false;
                out += c;
                ++i;
                continue;
            }
            if (in_string)
            {
                if (c == '\\')
                    escape = true;
                else if (c == '"')
                    in_string = false;
                out += c;
                ++i;
                continue;
            }
            if (c == '#')
            {
                in_comment = true;
                out += c;
                ++i;
                continue;
            }
            if (c == '"')
            {
                in_string = true;
                out += c;
                ++i;
                continue;
            }

            // Longest matching opener at this position wins.
            const std::string*  open  = nullptr;
            const KeywordEntry* entry = nullptr;
            for (const auto& [o, e] : inline_kws)
            {
                if (input.compare(i, o->size(), *o) == 0 && (!open || o->size() > open->size()))
                {
                    open  = o;
                    entry = e;
                }
            }
            if (!open)
            {
                out += c;
                ++i;
                continue;
            }

            if (!_scoped_vars_preloaded)
            {
                // One shared variable scope for the whole statement: a variable
                // created inside an island must unify with the same name in the
                // surrounding statement (rule authoring). Cleared here, at the
                // true start of statement processing; process_janet skips its
                // own clear once (_scoped_vars_preloaded).
                {
                    std::lock_guard<std::mutex> lock(_state_mutex);
                    _scoped_variables.clear();
                }
                _scoped_vars_preloaded = true;
            }

            const std::string& close         = entry->close;
            const size_t       content_begin = i + open->size();

            size_t k = input.find(close, content_begin);
            if (k == std::string::npos)
                throw std::runtime_error("Inline keyword '" + *open + "': missing closing delimiter '" + close + "'");

            Janet result = janet_wrap_nil();
            while (true)
            {
                const std::string inner = input.substr(content_begin, k - content_begin);
                if (call_keyword_handler(*open, entry->handler, inner, false, result) == HandlerCall::Dispatched)
                    break;

                k = input.find(close, k + close.size());
                if (k == std::string::npos)
                    throw std::runtime_error("Inline keyword '" + *open + "': handler reports incomplete input and no further '" + close + "' follows");
            }

            const network::Node n = zelph_unwrap_node(result);
            if (!n)
                throw std::runtime_error("Inline keyword '" + *open + "': handler must return a zelph/node");

            const std::string name = "$inline" + std::to_string(island++);
            janet_def(_janet_env, name.c_str(), result, "inline keyword expansion result");
            out += "," + name;

            i = k + close.size();
        }

        return out;
    }

    Janet ScriptEngine::Impl::janet_cfun_zelph_register_keyword(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 2, 3);
        if (!s_instance) return janet_wrap_nil();

        const bool inline_mode = argc == 3;

        const uint8_t* str     = janet_getstring(argv, 0);
        std::string    keyword = reinterpret_cast<const char*>(str);

        if (keyword.empty() || keyword[0] == '.' || keyword[0] == '%' || keyword[0] == '#')
            janet_panicf("zelph/register-keyword: invalid keyword '%s'", keyword.c_str());
        if (keyword.find_first_of(" \t\r\n") != std::string::npos)
            janet_panicf("zelph/register-keyword: keyword must not contain whitespace");

        std::string close;
        if (inline_mode)
        {
            close = reinterpret_cast<const char*>(janet_getstring(argv, 1));
            if (close.empty())
                janet_panicf("zelph/register-keyword: closing delimiter must not be empty");
            if (close.find_first_of(" \t\r\n") != std::string::npos)
                janet_panicf("zelph/register-keyword: closing delimiter must not contain whitespace");
        }

        const Janet handler = argv[inline_mode ? 2 : 1];
        if (!janet_checktype(handler, JANET_FUNCTION))
            janet_panicf("zelph/register-keyword: handler must be a function");

        auto it = s_instance->_keyword_handlers.find(keyword);
        if (it != s_instance->_keyword_handlers.end())
            janet_gcunroot(it->second.handler);

        janet_gcroot(handler);
        s_instance->_keyword_handlers[keyword] = KeywordEntry{handler, inline_mode, close};
        return janet_wrap_nil();
    }
}
