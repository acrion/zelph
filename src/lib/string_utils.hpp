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
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace zelph
{
    namespace string
    {
        namespace unicode
        {
            std::string ZELPH_EXPORT  to_utf8(const std::wstring& str);
            std::wstring ZELPH_EXPORT from_utf8(std::string str);
            std::wstring ZELPH_EXPORT unescape(const std::wstring& input);
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
        }

        template <typename T, typename U, typename V>
        static T get(U container, V key, T return_if_not_found)
        {
            auto it = container.find(key);
            return it == container.end() ? return_if_not_found : it->second;
        }

        template <typename T, typename U>
        static T get(U container, T key)
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

        inline size_t parse_count(const std::wstring& str)
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

        std::wstring mark_identifier(const std::wstring& str);
    }
}
