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

#if defined(__APPLE__) && defined(__clang__)
// TODO Workaround to build boost 1.71 boost with AppleClang 15
namespace std
{
    template <typename Arg, typename Result>
    struct unary_function
    {
        typedef Arg    argument_type;
        typedef Result result_type;
    };
}
#endif

#include <zelph_export.h>

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

    template <typename T>
    static std::basic_string<T> concatenate(std::vector<std::basic_string<T>> list, T separator)
    {
        std::basic_string<T> connected;
        for (std::basic_string<T> t : list)
        {
            if (!connected.empty()) connected += separator;
            connected += t;
        }
        return connected;
    }

    inline size_t parse_count(const std::string& str)
    {
        try
        {
            size_t pos = 0;
            size_t c   = std::stoull(str, &pos);
            if (pos != str.length() || c == 0)
                throw std::exception();
            return c;
        }
        catch (...)
        {
            throw std::runtime_error("Invalid count value");
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
}
