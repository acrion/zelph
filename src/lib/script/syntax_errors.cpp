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

#include "script/syntax_errors.hpp"

#include "string/node_to_string.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace zelph::script
{
    namespace
    {
        // The children of a node, joined by `sep` and wrapped in `open`/`close`.
        std::string joined(const Janet* data, int32_t len, int32_t from, const char* open, const char* sep, const char* close)
        {
            std::string out = open;
            for (int32_t i = from; i < len; ++i)
            {
                if (i > from) out += sep;
                out += arg_text(data[i]);
            }
            return out + close;
        }
    }

    std::string arg_text(Janet arg)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(arg, &data, &len) || len < 1) return "(...)";
        if (!janet_checktype(data[0], JANET_KEYWORD)) return "(...)";

        const std::string kind = reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]));

        // A LEAF the grammar captured as one token, printed the way it was
        // typed. A variable is NOT an :atom, and treating it as structure
        // turned "A father" into "(...) father" -- a message naming nothing
        // the reader had written.
        if (len >= 2 && janet_checktype(data[1], JANET_STRING))
        {
            const std::string text = reinterpret_cast<const char*>(janet_unwrap_string(data[1]));

            if (kind == "atom") return text;
            if (kind == "var") return text;
            if (kind == "number") return "&" + text;
            if (kind == "unquote") return "," + text;
            if (kind == "list-compact") return "<" + text + ">";
        }

        // Anything COMPOSITE is rebuilt from its own children, in the brackets
        // the grammar read them out of. This is the counterpart of rendering a
        // node with node_to_string, and it is the only one available here: the
        // refusal is emitted before the fragment is built, so there is no node
        // to render -- which is the property that keeps a rejected line from
        // leaving anything behind. Where a node DOES exist, the graph's own
        // renderer is the right one and is what the neighbouring refusals use
        // (`Zelph::format`, e.g. in zelph/conjunction).
        if (kind == "nested") return joined(data, len, 1, "(", " ", ")");
        if (kind == "condition") return joined(data, len, 1, "", " ", "");
        if (kind == "conjunction") return joined(data, len, 1, "(", ", ", ")");
        if (kind == "set") return joined(data, len, 1, "{", " ", "}");
        if (kind == "collection") return joined(data, len, 1, "@{", " ", "}");
        if (kind == "list-nodes") return joined(data, len, 1, "<", " ", ">");
        if (kind == "focused" && len >= 2) return "*" + arg_text(data[1]);
        if (kind == "negation" && len >= 2) return "¬" + arg_text(data[1]);
        if (kind == "selffact" && len >= 3 && janet_checktype(data[1], JANET_STRING))
            return ":" + std::string(reinterpret_cast<const char*>(janet_unwrap_string(data[1]))) + " " + arg_text(data[2]);
        if (kind == "approx" && len >= 3 && janet_checktype(data[1], JANET_STRING))
            return "≈" + std::string(reinterpret_cast<const char*>(janet_unwrap_string(data[1]))) + arg_text(data[2]);

        return "(...)";
    }

    int nested_value_count(Janet arg)
    {
        const Janet* data;
        int32_t      len;
        if (!janet_indexed_view(arg, &data, &len) || len < 1) return -1;
        if (!janet_checktype(data[0], JANET_KEYWORD)) return -1;
        if (std::string(reinterpret_cast<const char*>(janet_unwrap_keyword(data[0]))) != "nested") return -1;
        return len - 1;
    }

    void refuse_short_statement(const std::string& role, const std::vector<Janet>& args)
    {
        std::string written;
        for (const Janet& a : args)
        {
            if (!written.empty()) written += " ";
            written += arg_text(a);
        }

        const std::string what = role.empty() ? "\"" + written + "\"" : role + ", \"" + written + "\",";

        if (args.size() < 2)
            throw std::runtime_error(
                what + " is only a subject. A statement needs a predicate and at least one object after it.");

        const std::string subject   = arg_text(args[0]);
        const std::string predicate = arg_text(args[1]);

        // The self-fact is named using the user's own tokens, because it is
        // the one reading under which a two-part statement IS a statement --
        // and the reading somebody reaches for when the predicate is unary.
        //
        // Offered only where it can be TYPED. The sugar has nowhere to put
        // quotes, so its predicate must be a single bare token, and the gate
        // for that is the display's own -- the same function that decides
        // whether a self-fact is printed in the short form. Without it the
        // message advised ":(q r s) x", which no parser accepts.
        if (!zelph::string::selffact_sugar_safe(predicate))
            throw std::runtime_error(
                what + " is a subject and a predicate with the object left off. "
                       "Write the object after it.");

        throw std::runtime_error(
            what + " is a subject and a predicate with the object left off. Write the object after it -- "
                   "or, if the statement is about one node, write the self-fact \":"
            + predicate + " " + subject + "\", which is \""
            + subject + " " + predicate + " " + subject + "\".");
    }

    namespace
    {
        // Byte length of the reserved character at `i`, or 0. This is the
        // grammar's own `:reserved` set (script_engine_setup.cpp), and it has
        // to be asked for a LENGTH rather than tested one byte at a time,
        // because `¬` is two bytes: reading it as one byte reported `¬(a b)`
        // -- which parses -- as a value glued to a parenthesis.
        std::size_t reserved_length(const std::string& s, const std::size_t i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (std::string(" \t\r\n\v<\"(){}*>,").find(static_cast<char>(c)) != std::string::npos) return 1;
            if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xAC) return 2; // ¬
            return 0;
        }

        // Values whose grammar rule reads its own operand with `:s*`, so that
        // an operand glued to them is CORRECT and says nothing about spacing:
        // `:tag-approx` (≈net(x)) and `:tag-selffact` (:pred(x)). Reporting
        // those would name a token the user got right.
        bool takes_a_glued_operand(const std::string& token)
        {
            return token.front() == ':' || token.starts_with("≈"); // ≈
        }

        std::string quoted(const std::string& token)
        {
            constexpr std::size_t limit = 40;
            return "\"" + (token.size() <= limit ? token : token.substr(0, limit) + "...") + "\"";
        }

        std::string explain_glued(const std::string& token)
        {
            if (token == "%")
                return " '%' escapes a whole LINE to Janet, so it cannot stand as a term inside a statement. "
                       "To use a Janet VALUE in a statement, bind it and unquote the name -- one line "
                       "\"%(def v ...)\", then \",v z+ (pos zint &5)\".";

            if (token == "$")
                return " \"$( ... )\" is a notation island of the standard library rather than core syntax. "
                       "\".import math-syntax\" registers it.";

            return " " + quoted(token + "(") + " is a value glued to a \"(\": the grammar separates two values by "
                                               "whitespace, so write "
                 + quoted(token + " (") + " if a group was meant. Function notation such as \"f(x)\" "
                                          "exists only inside a notation island -- see \".import math-syntax\".";
        }
    }

    std::string diagnose_unparsable(const std::string& statement)
    {
        bool        in_quotes   = false;
        bool        escape      = false;
        std::size_t token_start = 0; // first byte of the value being read
        std::size_t i           = 0;

        while (i < statement.size())
        {
            const char c = statement[i];

            // Inside a quoted atom everything is text, including "(" -- and
            // the two escapes the atom knows have to be honoured, or a
            // trailing `\"` ends the atom one character early and the rest of
            // the line is scanned in the wrong state.
            if (in_quotes)
            {
                if (escape)
                    escape = false;
                else if (c == '\\')
                    escape = true;
                else if (c == '"')
                    in_quotes = false;
                ++i;
                continue;
            }

            if (c == '"')
            {
                in_quotes   = true;
                token_start = ++i;
                continue;
            }

            if (c == '(')
            {
                if (i > token_start)
                {
                    const std::string token = statement.substr(token_start, i - token_start);
                    if (!takes_a_glued_operand(token)) return explain_glued(token);
                }
                else if (i > 0 && statement[i - 1] == ','
                         && statement.find_first_not_of(" \t\r\n\v") == i - 1)
                {
                    // Only at the very beginning of the statement. Further in,
                    // ",(" is the conjunction separator followed by a group
                    // (`:comma-sep` requires a reserved character after the
                    // comma), so "x p y,(a q b)" is correct and must not be
                    // diagnosed.
                    return " The unquote \",\" takes the NAME of a Janet binding, not an expression: "
                           "bind it with \"%(def v ...)\" and write \",v\".";
                }
                token_start = ++i;
                continue;
            }

            const std::size_t r = reserved_length(statement, i);
            if (r > 0)
            {
                i += r;
                token_start = i;
                continue;
            }

            ++i;
        }

        return {};
    }
}
