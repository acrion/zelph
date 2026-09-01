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
}
