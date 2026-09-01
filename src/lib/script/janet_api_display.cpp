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
#include <string>
#include <vector>

namespace zelph
{
    // Register the digit alphabet for &-literal display (inverse of the
    // &-input syntax). Digits are given in ascending order of value; the
    // base is the array length. C++ makes no assumptions about the digit
    // names, their count, or their internal order -- the only hardcoded
    // convention is that &-literals are always decimal, on input and output.
    Janet ScriptEngine::Impl::janet_cfun_zelph_set_number_digits(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set-number-digits", argc, argv, true);

        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(argv[0], &data, &len))
            janet_panicf("zelph/set-number-digits: expected an array or tuple of digit nodes/names");

        std::vector<network::Node> digits;
        digits.reserve(static_cast<size_t>(len));
        for (int32_t i = 0; i < len; ++i)
        {
            network::Node nd = s_instance->resolve_janet_arg(data[i]);
            if (!nd) janet_panicf("zelph/set-number-digits: digit at index %d could not be resolved", i);
            digits.push_back(nd);
        }

        std::string err;
        try
        {
            s_instance->_n->set_number_digits(digits);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/set-number-digits: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Register a display scheme: the delimiters a script uses to enclose its
    // own notation, its numeral prefix, and the identifier grammar of its
    // leaves. C++ stores these verbatim -- it never parses the scheme's
    // syntax and knows nothing about the domain.
    Janet ScriptEngine::Impl::janet_cfun_zelph_register_display_scheme(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 3, 4);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/register-display-scheme", argc, argv, true);

        network::DisplayScheme scheme;
        scheme.name  = reinterpret_cast<const char*>(janet_getstring(argv, 0));
        scheme.open  = reinterpret_cast<const char*>(janet_getstring(argv, 1));
        scheme.close = reinterpret_cast<const char*>(janet_getstring(argv, 2));

        if (argc >= 4 && !janet_checktype(argv[3], JANET_NIL))
        {
            const auto option = [&](const char* key, std::string& out)
            {
                const Janet v = janet_get(argv[3], janet_ckeywordv(key));
                if (janet_checktype(v, JANET_STRING))
                    out = reinterpret_cast<const char*>(janet_unwrap_string(v));
                else if (janet_checktype(v, JANET_BUFFER))
                {
                    JanetBuffer* b = janet_unwrap_buffer(v);
                    out            = std::string(reinterpret_cast<const char*>(b->data), b->count);
                }
            };
            option("numeral-prefix", scheme.numeral_prefix);
            option("name-first", scheme.name_first);
            option("name-chars", scheme.name_chars);
        }

        std::string err;
        try
        {
            s_instance->_n->register_display_scheme(scheme);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/register-display-scheme: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Register infix operators into a previously declared scheme.
    Janet ScriptEngine::Impl::janet_cfun_zelph_set_infix_display(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set-infix-display", argc, argv, true);

        const std::string name = reinterpret_cast<const char*>(janet_getstring(argv, 0));

        std::size_t scheme = 0;
        if (!s_instance->_n->find_display_scheme(name, scheme))
            janet_panicf("zelph/set-infix-display: unknown display scheme '%s' (register it first)", name.c_str());

        const Janet* rows;
        int32_t      row_count;
        if (!janet_indexed_view(argv[1], &rows, &row_count))
            janet_panicf("zelph/set-infix-display: expected an array of [predicate precedence associativity] entries");

        std::vector<network::InfixEntry> ops;
        ops.reserve(static_cast<size_t>(row_count));

        for (int32_t i = 0; i < row_count; ++i)
        {
            const Janet* cells;
            int32_t      cell_count;
            if (!janet_indexed_view(rows[i], &cells, &cell_count) || cell_count < 2)
                janet_panicf("zelph/set-infix-display: entry %d must be [predicate precedence &opt associativity]", i);

            network::InfixEntry entry;

            // Creating the node is intentional: a module registers its
            // operators up front, possibly before any fact mentions them.
            entry.predicate = s_instance->resolve_janet_arg(cells[0]);
            if (!entry.predicate)
                janet_panicf("zelph/set-infix-display: predicate of entry %d could not be resolved", i);

            if (!janet_checktype(cells[1], JANET_NUMBER))
                janet_panicf("zelph/set-infix-display: precedence of entry %d must be a number", i);
            entry.precedence = static_cast<int>(janet_unwrap_number(cells[1]));

            if (cell_count >= 3 && janet_checktype(cells[2], JANET_KEYWORD))
            {
                const std::string a = reinterpret_cast<const char*>(janet_unwrap_keyword(cells[2]));
                if (a == "left")
                    entry.assoc = -1;
                else if (a == "right")
                    entry.assoc = 1;
                else if (a == "none")
                    entry.assoc = 0;
                else
                    janet_panicf("zelph/set-infix-display: associativity of entry %d must be :left, :right or :none", i);
            }

            ops.push_back(entry);
        }

        std::string err;
        try
        {
            s_instance->_n->set_infix_display(scheme, ops);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/set-infix-display: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Register application-form predicates in a scheme: (S P O) is written
    // "S(O)". Shares the scheme's predicate namespace with the infix form.
    Janet ScriptEngine::Impl::janet_cfun_zelph_set_application_display(int32_t argc, Janet* argv)
    {
        janet_fixarity(argc, 2);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/set-application-display", argc, argv, true);

        const std::string name = reinterpret_cast<const char*>(janet_getstring(argv, 0));

        std::size_t scheme = 0;
        if (!s_instance->_n->find_display_scheme(name, scheme))
            janet_panicf("zelph/set-application-display: unknown display scheme '%s' (register it first)", name.c_str());

        const Janet* rows;
        int32_t      row_count;
        if (!janet_indexed_view(argv[1], &rows, &row_count))
            janet_panicf("zelph/set-application-display: expected an array of predicates");

        std::vector<network::Node> preds;
        preds.reserve(static_cast<size_t>(row_count));
        for (int32_t i = 0; i < row_count; ++i)
        {
            // Creating the node is intentional, as in set-infix-display.
            network::Node p = s_instance->resolve_janet_arg(rows[i]);
            if (!p) janet_panicf("zelph/set-application-display: predicate %d could not be resolved", i);
            preds.push_back(p);
        }

        std::string err;
        try
        {
            s_instance->_n->set_application_display(scheme, preds);
            return janet_wrap_nil();
        }
        catch (const std::exception& e)
        {
            err = e.what();
        }
        janet_panicf("zelph/set-application-display: %s", err.c_str());
        return janet_wrap_nil(); // unreachable
    }

    // Register predicates whose self-facts must render verbose. Additive
    // (unlike the replace-the-set semantics of zelph/set-number-digits):
    // arithmetic, symbolic-core and eml load incrementally, and a later
    // module must not clobber an earlier module's registrations.
    Janet ScriptEngine::Impl::janet_cfun_zelph_no_selffact_sugar(int32_t argc, Janet* argv)
    {
        janet_arity(argc, 1, -1);
        if (!s_instance) return janet_wrap_nil();
        if (s_instance->_log_janet_functions) s_instance->log_janet_call("zelph/no-selffact-sugar", argc, argv, true);

        std::vector<network::Node> preds;
        preds.reserve(static_cast<size_t>(argc));
        for (int32_t i = 0; i < argc; ++i)
        {
            // Creating the node is intentional: modules register their
            // operators up front, possibly before any fact mentions them.
            network::Node p = s_instance->resolve_janet_arg(argv[i]);
            if (!p) janet_panicf("zelph/no-selffact-sugar: argument %d could not be resolved", i);
            preds.push_back(p);
        }

        s_instance->_n->add_verbose_selffact_predicates(preds);
        return janet_wrap_nil();
    }
}
