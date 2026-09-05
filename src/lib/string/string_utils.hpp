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

#include <zelph_export.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace zelph::string
{
    namespace unicode
    {
        std::string ZELPH_EXPORT unescape(const std::string& input);
    }

    namespace utf8
    {
        inline void append(std::string& out, char32_t cp)
        {
            if (cp < 0x80)
            {
                out.push_back(static_cast<char>(cp));
            }
            else if (cp < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        inline char32_t read(std::string_view s, size_t& pos)
        {
            if (pos >= s.size())
                throw std::invalid_argument("Truncated UTF-8");

            unsigned char c = s[pos++];
            if (c < 0x80) return c;

            char32_t cp;
            size_t   extra;
            if ((c & 0xE0) == 0xC0)
            {
                cp    = c & 0x1F;
                extra = 1;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                cp    = c & 0x0F;
                extra = 2;
            }
            else if ((c & 0xF8) == 0xF0)
            {
                cp    = c & 0x07;
                extra = 3;
            }
            else
                throw std::invalid_argument("Invalid UTF-8 sequence");

            for (size_t j = 0; j < extra; ++j)
            {
                if (pos >= s.size())
                    throw std::invalid_argument("Truncated UTF-8");
                unsigned char next = s[pos++];
                if ((next & 0xC0) != 0x80)
                    throw std::invalid_argument("Invalid UTF-8 continuation byte");
                cp = (cp << 6) | (next & 0x3F);
            }
            return cp;
        }

        // Return the number of Unicode codepoints in a UTF-8 string.
        inline size_t codepoint_count(std::string_view s)
        {
            size_t count = 0;
            size_t pos   = 0;
            while (pos < s.size())
            {
                read(s, pos);
                ++count;
            }
            return count;
        }

        // Return the first Unicode codepoint of a non-empty UTF-8 string.
        inline char32_t front(std::string_view s)
        {
            size_t pos = 0;
            return read(s, pos);
        }

        // Return the last Unicode codepoint of a non-empty UTF-8 string.
        inline char32_t back(std::string_view s)
        {
            if (s.empty())
                throw std::invalid_argument("Empty string has no last codepoint");

            // Walk backwards to find the start of the last codepoint
            size_t i = s.size() - 1;
            while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
                --i;

            size_t pos = i;
            return read(s, pos);
        }
    }

    template <typename U, typename V>
    static typename U::mapped_type get(const U& container, V key, typename U::mapped_type return_if_not_found)
    {
        auto it = container.find(key);
        return it == container.end() ? return_if_not_found : it->second;
    }

    template <typename U, typename V>
    static typename U::mapped_type get(const U& container, V key)
    {
        auto it = container.find(key);
        return it == container.end() ? key : it->second;
    }

    // A positive count argument (".list 20", ".out node 5"). std::stoull
    // WRAPS a negative literal into a huge unsigned instead of failing, so
    // ".list -1" reported "Listing 18446744073709551615 nodes" -- rejecting
    // the sign explicitly is the only way to tell the two apart afterwards.
    inline size_t parse_count(const std::string& str)
    {
        try
        {
            if (str.empty() || str.front() == '-' || str.front() == '+')
                throw std::exception();

            size_t pos = 0;
            size_t c   = std::stoull(str, &pos);
            if (pos != str.length() || c == 0)
                throw std::exception();
            return c;
        }
        catch (...)
        {
            throw std::runtime_error("Invalid count value '" + str + "': expected a positive number");
        }
    }

    // Iterate over Unicode codepoints in a UTF-8 string, calling f(codepoint_string) for each.
    // Each codepoint_string is the 1-4 byte UTF-8 sequence for that codepoint.
    template <typename F>
    void for_each_codepoint(const std::string& utf8, F&& f)
    {
        size_t i = 0;
        while (i < utf8.size())
        {
            unsigned char c = utf8[i];
            size_t        len;
            if (c < 0x80)
                len = 1;
            else if ((c & 0xE0) == 0xC0)
                len = 2;
            else if ((c & 0xF0) == 0xE0)
                len = 3;
            else if ((c & 0xF8) == 0xF0)
                len = 4;
            else
            {
                ++i;
                continue;
            } // invalid leading byte, skip

            if (i + len > utf8.size()) break; // truncated sequence
            f(utf8.substr(i, len));
            i += len;
        }
    }

    std::string to_hex(uint64_t value);
    std::string mark_identifier(const std::string& str);
    std::string unmark_identifiers(const std::string& str);
    std::string sanitize_filename(const std::string& name);

    /// The two escapes a quoted atom knows, and nothing else: `\"` for a
    /// quote and `\\` for a backslash. They are deliberately the same two
    /// Janet spells that way, so an encoded atom can be handed to Janet as
    /// a string literal unchanged -- but the DEcoding has to happen here,
    /// or Janet's much larger escape set applies to zelph text and a node
    /// written `a\b` ends up named `a<backspace>`.
    /// Does printing this name add no quotes? Asked by a rendering that
    /// has no way to quote the name it embeds.
    bool prints_bare(const std::string& name);

    std::string escape_atom(const std::string& name);
    std::string unescape_atom(const std::string& body);

    /// Length in bytes of the whitespace character that starts at `s[i]`, or
    /// 0 when what stands there is not whitespace.
    ///
    /// ASCII whitespace, plus the characters an editor or a copy out of a PDF
    /// puts into a line without showing it. Those matter here because zelph
    /// decides what a line IS from its first character: a leading U+FEFF made
    /// a line miss both the comment test and the command test, so a `.zph`
    /// file saved with a byte order mark had its first line parsed as the
    /// beginning of a statement, which then stayed incomplete and swallowed
    /// the line after it. A whole script rewritten, without one message.
    /// U+FEFF is a zero-width no-break SPACE wherever it is not a byte order
    /// mark, so dropping it is not a special case for files.
    ///
    /// Reading the encoding rather than decoding first is deliberate: every
    /// byte listed below is a LEAD byte (>= 0xC2), and a UTF-8 continuation
    /// byte (0x80..0xBF) can never be one, so no scan can mistake the tail of
    /// a multi-byte character for the start of a space.
    inline std::size_t whitespace_length(const std::string& s, const std::size_t i)
    {
        if (i >= s.size()) return 0;

        const auto byte = [&s](const std::size_t k) -> unsigned char
        { return k < s.size() ? static_cast<unsigned char>(s[k]) : 0; };

        const unsigned char c = byte(i);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') return 1;

        if (c == 0xC2 && byte(i + 1) == 0xA0) return 2;                        // U+00A0 no-break space
        if (c == 0xE1 && byte(i + 1) == 0x9A && byte(i + 2) == 0x80) return 3; // U+1680 ogham space mark
        if (c == 0xE2 && byte(i + 1) == 0x80)
        {
            const unsigned char t = byte(i + 2);
            if (t >= 0x80 && t <= 0x8A) return 3; // U+2000..U+200A the typographic spaces
            if (t == 0xA8 || t == 0xA9) return 3; // U+2028, U+2029 line and paragraph separator
            if (t == 0xAF) return 3;              // U+202F narrow no-break space
        }
        if (c == 0xE2 && byte(i + 1) == 0x81 && byte(i + 2) == 0x9F) return 3; // U+205F medium mathematical space
        if (c == 0xE3 && byte(i + 1) == 0x80 && byte(i + 2) == 0x80) return 3; // U+3000 ideographic space
        if (c == 0xEF && byte(i + 1) == 0xBB && byte(i + 2) == 0xBF) return 3; // U+FEFF byte order mark

        return 0;
    }

    /// Index of the first byte that does not start a whitespace character, or
    /// npos when the string holds nothing else.
    inline std::size_t first_non_whitespace(const std::string& s)
    {
        std::size_t i = 0;
        while (i < s.size())
        {
            const std::size_t n = whitespace_length(s, i);
            if (n == 0) return i;
            i += n;
        }
        return std::string::npos;
    }

    /// Trim whitespace from both ends -- ASCII and the invisible Unicode
    /// spaces, see whitespace_length.
    inline std::string trim(const std::string& s)
    {
        const std::size_t start = first_non_whitespace(s);
        if (start == std::string::npos) return {};

        std::size_t end = start; // one past the last byte that is not whitespace
        for (std::size_t i = start; i < s.size();)
        {
            const std::size_t n = whitespace_length(s, i);
            if (n == 0)
                end = ++i;
            else
                i += n;
        }
        return s.substr(start, end - start);
    }

    inline void trim_in_place(std::string& s)
    {
        s = trim(s);
    }

    /// Trim whitespace from the left only, same character set as trim.
    inline std::string trim_left(const std::string& s)
    {
        const std::size_t start = first_non_whitespace(s);
        if (start == std::string::npos) return {};
        return s.substr(start);
    }

    /// Replace all occurrences of `from` with `to` in-place.
    inline void replace_all(std::string& s, const std::string& from, const std::string& to)
    {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos)
        {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    /// Replace all occurrences of `from` with `to`, returning a new string.
    inline std::string replace_all_copy(const std::string& s, const std::string& from, const std::string& to)
    {
        std::string result = s;
        replace_all(result, from, to);
        return result;
    }

    /// Tokenize a string respecting quoted substrings and backslash escapes.
    /// Replicates the semantics of boost::escaped_list_separator<char>("\\", " \t", "\""):
    ///   - Backslash escapes the next character inside and outside quotes.
    ///   - Quoted regions preserve whitespace; the quotes themselves are stripped.
    ///   - Empty tokens between delimiters are discarded.
    /// One token of a command line, in the two forms a command needs.
    /// `text` has the quotes stripped, which is what a name lookup wants.
    /// `source` puts them back where they were, which is what the PARSER
    /// wants: the quotes are the only thing that tells the NAME `x>y` from
    /// the three atoms `x > y`, and they are gone by the time a command
    /// sees its tokens -- so the line zelph printed could not be pasted
    /// into `.explain` or `.prune-facts`. Quoting is per SEGMENT, so a
    /// token that is only partly quoted (`"x>y")` at the end of a
    /// parenthesised pattern) keeps both halves.
    struct QuotedToken
    {
        std::string text;
        std::string source;
    };

    std::vector<QuotedToken> tokenize_quoted_marked(const std::string& input);
    std::vector<std::string> tokenize_quoted(const std::string& input);

    /// Remove all leading and trailing occurrences of any string in `chars` (UTF-8 safe).
    inline std::string trim_any_of(const std::string& s, const std::vector<std::string>& chars)
    {
        std::string result = s;
        // Trim left
        bool found = true;
        while (found && !result.empty())
        {
            auto it = std::ranges::find_if(chars.begin(), chars.end(), [&](const auto& c)
                                           { return result.size() >= c.size() && result.compare(0, c.size(), c) == 0; });
            found   = (it != chars.end());
            if (found)
                result.erase(0, it->size());
        }
        // Trim right
        found = true;
        while (found && !result.empty())
        {
            auto it = std::ranges::find_if(chars.begin(), chars.end(), [&](const auto& c)
                                           { return result.size() >= c.size()
                                                 && result.compare(result.size() - c.size(), c.size(), c) == 0; });
            found   = (it != chars.end());
            if (found)
                result.erase(result.size() - it->size());
        }
        return result;
    }

    inline std::string to_lower_ascii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return s;
    }
}
