/*
Copyright (c) 2025 acrion innovations GmbH
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
namespace std {
    template <typename Arg, typename Result>
    struct unary_function {
        typedef Arg argument_type;
        typedef Result result_type;
    };
}
#endif

#include <zelph_export.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace zelph
{
    namespace network
    {
        using Node      = uint64_t;
        using Variables = std::map<Node, Node>;

        static_assert(sizeof(Node) == 8,
                      "Node must be exactly 64 bits (8 bytes) in size. This implementation uses "
                      "bit-shift operations and other bit-specific logic that requires exactly "
                      "64 bits. Please modify the implementation for different bit sizes.");

        static_assert(sizeof(Node) == sizeof(std::size_t),
                      "Node and size_t must have the same size. "
                      "This implementation requires a 64-bit architecture where size_t is also 64 bits. "
                      "Compilation has been halted to prevent undefined behavior.");

        namespace utils
        {
            std::shared_ptr<Variables> join(const Variables& v1, const Variables& v2);
            std::string ZELPH_EXPORT   str(const std::wstring& str);
            std::wstring ZELPH_EXPORT  wstr(std::string str);
            std::wstring ZELPH_EXPORT   convert_unicode_escapes(const std::wstring& input);

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

            struct IncDec
            {
                explicit IncDec(int& n)
                    : _n(n) { ++_n; }
                ~IncDec() { --_n; }
                int& _n;
            };

            std::wstring mark_identifier(const std::wstring& str);
        }
    }
}
