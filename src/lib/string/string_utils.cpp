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

#include "string_utils.hpp"

#include <algorithm>
#include <sstream>

namespace zelph::string
{
    namespace unicode
    {
        // Read a \uXXXX escape at `i` (which points at the backslash) and
        // return its code unit, or -1 if the four hex digits are not there.
        static int read_u_escape(const std::string& input, size_t i)
        {
            if (i + 5 >= input.size() || input[i] != '\\' || input[i + 1] != 'u') return -1;

            // cppcheck-suppress stlcstrConstructor
            std::string_view hexCode(input.data() + i + 2, 4);
            if (!std::all_of(hexCode.begin(), hexCode.end(), [](unsigned char c)
                             { return std::isxdigit(c); }))
                return -1;

            return std::stoi(std::string(hexCode), nullptr, 16);
        }

        std::string unescape(const std::string& input)
        {
            // The Wikidata dump escapes every non-ASCII character, so a label
            // that is not decoded here reaches the network with the six
            // characters of the escape sequence in place of the letter --
            // unsearchable under its real name and not re-enterable as input.
            // The whole JSON escape set is handled, because a label may just
            // as well contain an escaped quote or backslash.
            if (input.find('\\') == std::string::npos) return input;

            std::string result;
            result.reserve(input.size());

            for (size_t i = 0; i < input.size(); ++i)
            {
                if (input[i] != '\\' || i + 1 >= input.size())
                {
                    result.push_back(input[i]);
                    continue;
                }

                const char esc = input[i + 1];

                if (esc == 'u')
                {
                    const int unit = read_u_escape(input, i);
                    if (unit < 0)
                    {
                        result.push_back(input[i]); // not an escape after all
                        continue;
                    }
                    i += 5;

                    char32_t cp = static_cast<char32_t>(unit);

                    // Code points above the BMP arrive as a surrogate pair.
                    // Appending the halves separately would emit CESU-8, i.e.
                    // invalid UTF-8 that every later reader has to cope with.
                    if (unit >= 0xD800 && unit <= 0xDBFF)
                    {
                        const int low = read_u_escape(input, i + 1);
                        if (low >= 0xDC00 && low <= 0xDFFF)
                        {
                            cp = 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10)
                               + (static_cast<char32_t>(low) - 0xDC00);
                            i += 6;
                        }
                        else
                        {
                            cp = 0xFFFD; // unpaired high surrogate
                        }
                    }
                    else if (unit >= 0xDC00 && unit <= 0xDFFF)
                    {
                        cp = 0xFFFD; // unpaired low surrogate
                    }

                    utf8::append(result, cp);
                    continue;
                }

                switch (esc)
                {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case '/':
                    result.push_back('/');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                default:
                    // Not a JSON escape -- keep both characters, so that
                    // text which merely contains a backslash survives.
                    result.push_back(input[i]);
                    result.push_back(esc);
                    break;
                }
                ++i;
            }

            return result;
        }
    }

    // Converts a uint64_t value to its hexadecimal string representation (without '0x' prefix).
    std::string to_hex(uint64_t value)
    {
        std::stringstream ss;
        ss << std::hex << value;
        return ss.str();
    }

    std::string escape_atom(const std::string& name)
    {
        std::string out;
        out.reserve(name.size());
        for (const char c : name)
        {
            if (c == '\\' || c == '"') out += '\\';
            out += c;
        }
        return out;
    }

    std::string unescape_atom(const std::string& body)
    {
        std::string out;
        out.reserve(body.size());
        for (std::size_t i = 0; i < body.size(); ++i)
        {
            // Only `\"` and `\\` are escapes. A backslash in front of
            // anything else is an ordinary character, so a Windows path or
            // a LaTeX fragment reads back as itself.
            if (body[i] == '\\' && i + 1 < body.size()
                && (body[i + 1] == '"' || body[i + 1] == '\\'))
            {
                ++i;
            }
            out += body[i];
        }
        return out;
    }

    // A name that the parser would not read back as ONE atom has to be
    // quoted on output -- zelph's own printed form is meant to be
    // re-enterable. The set is the PEG's reserved characters plus
    // whitespace. A double quote is among them: the quoted-atom rule now
    // has an escape for it, so such a name is writable and gets quoted like
    // any other instead of being printed bare and read back as several
    // atoms.
    static bool needs_quotes(const std::string& name)
    {
        // Names the grammar has a dedicated token for. They consist of
        // reserved characters yet read back as ONE atom, so quoting them
        // would be noise -- and worse than noise for `*`, which the
        // mathematical modules use as a predicate everywhere and which the
        // term-island parser does not accept in quoted form.
        static const std::string_view bare_atoms[] = {
            "*", "<", ">", "=>", "->", "-->", "<=>", "<=", ">="};
        for (const auto& atom : bare_atoms)
            if (name == atom) return false;

        // Some tokens the grammar recognises by their FIRST character rather
        // than by the characters they contain, so a reserved SET cannot see
        // them: "_x" and a single uppercase letter are variables, "&12" is a
        // number literal, ":foo" opens the self-fact sugar, "≈net" a neural
        // condition. A node really named that way exists -- Wikidata has
        // single-letter labels -- and printing it bare made the line read
        // back as something else. Two of them do so without any complaint:
        // a variable, and `c rel2 :foo d`, where the sugar swallows the
        // following object and yields a nested self-fact instead.
        //
        // The renderer's own ":pred subject" never reaches this function:
        // the colon is emitted beside the marked name, not inside it, which
        // is what lets the colon be judged here as part of a NAME.
        if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return true;
        if (name.front() == '_' || name.front() == '&' || name.front() == ':') return true;
        if (name.rfind("≈", 0) == 0) return true;

        for (const unsigned char c : name)
        {
            if (c <= ' ') return true; // whitespace and control characters
            switch (c)
            {
            case '<':
            case '>':
            case '(':
            case ')':
            case '{':
            case '}':
            case '*':
            case ',':
                return true;
            default:
                break;
            }
            if (c == 0xC2) return true; // UTF-8 lead byte of '¬', '«', '»'
        }
        return false;
    }

    // Does this name reach the parser unchanged, i.e. does printing it add
    // no quotes? The renderer asks before using a form that has no way to
    // quote the name it contains -- the self-fact sugar is the one such
    // form -- so that the quoting rules are followed rather than restated.
    bool prints_bare(const std::string& name)
    {
        return !needs_quotes(name);
    }

    // Marks an identifier with guillemets so that later stages can tell a
    // leaf NAME from the surrounding structure (brackets, spacing).
    //
    // Whether a string is a leaf name at all is the CALLER's knowledge, not
    // something to be guessed from its shape: this function used to leave
    // anything bracket-shaped alone, which silently demoted every genuine
    // name that looks like one -- "Mercury (planet)" and every other
    // disambiguated Wikidata label, and the predicate ">". It also left
    // variable-SHAPED names alone, although a node named "A" or "_x" is a
    // node like any other; node_to_string asks the graph instead (mark_leaf).
    // An unmarked name loses its quoting on output ("Mercury (planet) orbits
    // sun" reads back as a four-atom statement) and degrades into literal
    // text in the derivation export, where it is a node reference the
    // converter needs.
    //
    // A name containing a guillemet itself cannot be marked: the markers are
    // in-band, so the first » inside the name would end the marker and split
    // it in two ("«Le Monde»" came out as "\"«Le Monde\"»"). French and
    // German Wikidata labels make that a real case, not a hypothetical one.
    // Such a name is emitted directly instead -- already quoted if it needs
    // quoting, since nothing downstream will do it. Consumers of the marking
    // may therefore RELY on a marked name containing no guillemets.
    std::string mark_identifier(const std::string& str)
    {
        if (str.empty())
        {
            return str;
        }

        if (str.find("«") != std::string::npos || str.find("»") != std::string::npos)
        {
            return needs_quotes(str) ? "\"" + escape_atom(str) + "\"" : str;
        }

        return "«" + str + "»";
    }

    // Remove the guillemets that mark_identifier added, turning them into
    // double quotes wherever the parser would otherwise not read the name
    // back as one atom. Quoting only names with a SPACE was not enough: a
    // node named "x>y" printed as `a rel x>y`, which re-reads as something
    // else entirely -- and zelph's output is supposed to be its own input.
    std::string unmark_identifiers(const std::string& str)
    {
        static const std::string open  = "«"; // U+00AB, 2 bytes in UTF-8
        static const std::string close = "»"; // U+00BB, 2 bytes in UTF-8

        std::string result;
        result.reserve(str.size());

        std::size_t pos = 0;
        while (pos < str.size())
        {
            const std::size_t open_pos  = str.find(open, pos);
            const std::size_t quote_pos = str.find('"', pos);

            // A double quote OUTSIDE a marker can only come from
            // mark_identifier's guillemet branch -- nothing else in a
            // rendering produces one, and a quote INSIDE a name is reached
            // through its marker first. Its content is therefore already
            // finished and is copied through, so that the guillemets in
            // "«Le Monde»" are read as the name they are and not as a
            // marker pair that happens to sit there.
            if (quote_pos != std::string::npos && (open_pos == std::string::npos || quote_pos < open_pos))
            {
                const std::size_t end = str.find('"', quote_pos + 1);
                if (end == std::string::npos)
                {
                    result.append(str, pos, std::string::npos);
                    break;
                }
                result.append(str, pos, end + 1 - pos);
                pos = end + 1;
                continue;
            }

            if (open_pos == std::string::npos)
            {
                result.append(str, pos, std::string::npos);
                break;
            }

            // Copy everything before the opening marker unchanged.
            result.append(str, pos, open_pos - pos);

            const std::size_t content_start = open_pos + open.size();
            const std::size_t close_pos     = str.find(close, content_start);
            if (close_pos == std::string::npos)
            {
                // Unbalanced opening marker: strip it, keep the rest (previous behavior).
                result.append(str, content_start, std::string::npos);
                break;
            }

            const std::string content = str.substr(content_start, close_pos - content_start);
            if (needs_quotes(content))
            {
                // escape_atom, not the bare name: a quote inside would end
                // the atom, and a backslash would start an escape.
                result += '"';
                result += escape_atom(content);
                result += '"';
            }
            else
            {
                result += content;
            }

            pos = close_pos + close.size();
        }

        return result;
    }

    std::string sanitize_filename(const std::string& name)
    {
        std::string result;
        result.reserve(name.size());
        const std::string_view invalid_chars = "/\\:*?\"<>|";

        for_each_codepoint(name, [&](std::string_view cp_str)
                           {
            if (cp_str.size() == 1
                && invalid_chars.find(cp_str[0]) != std::string_view::npos)
            {
                result += '_';
            }
            else
            {
                result += cp_str;
            } });

        return result;
    }

    std::vector<QuotedToken> tokenize_quoted_marked(const std::string& input)
    {
        std::vector<QuotedToken> tokens;
        QuotedToken              current;
        std::string              segment; // the current quoted run
        bool                     in_quotes = false;
        bool                     escape    = false;

        // A quoted run is closed back into `source` with its quotes and
        // escapes restored, so that the parser reads the same atom the user
        // wrote. Everything outside the quotes is copied through: that is
        // where a nested fact, a term island or a bracket lives.
        const auto close_segment = [&]
        {
            current.source += '"' + escape_atom(segment) + '"';
            segment.clear();
        };

        const auto flush = [&]
        {
            // An empty token is dropped, quoted or not -- the behaviour
            // this function has always had.
            if (!current.text.empty()) tokens.push_back(std::move(current));
            current = QuotedToken{};
        };

        for (char c : input)
        {
            if (escape)
            {
                const bool known = (c == '"' || c == '\\');
                if (in_quotes)
                {
                    if (known)
                    {
                        segment.push_back(c);
                    }
                    else
                    {
                        segment.push_back('\\');
                        segment.push_back(c);
                    }
                }
                else if (known)
                {
                    current.source.push_back(c);
                }
                else
                {
                    current.source.push_back('\\');
                    current.source.push_back(c);
                }

                if (known)
                {
                    current.text.push_back(c);
                }
                else
                {
                    current.text.push_back('\\');
                    current.text.push_back(c);
                }
                escape = false;
                continue;
            }

            if (c == '\\')
            {
                escape = true;
                continue;
            }

            if (c == '"')
            {
                if (in_quotes) close_segment();
                in_quotes = !in_quotes;
                continue;
            }

            if (!in_quotes && (c == ' ' || c == '\t'))
            {
                flush();
                continue;
            }

            current.text.push_back(c);
            if (in_quotes)
                segment.push_back(c);
            else
                current.source.push_back(c);
        }

        // An unterminated quote keeps what it collected rather than losing
        // it; the parser then reports the syntax error, which is where it
        // belongs.
        if (in_quotes) close_segment();
        flush();

        return tokens;
    }

    std::vector<std::string> tokenize_quoted(const std::string& input)
    {
        std::vector<std::string> tokens;
        for (auto& token : tokenize_quoted_marked(input))
            tokens.push_back(std::move(token.text));
        return tokens;
    }
}
