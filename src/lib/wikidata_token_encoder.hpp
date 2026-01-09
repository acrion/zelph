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

#include "string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace zelph
{
    class WikidataTokenEncoder
    {
    public:
        // base_codepoint: Start of CJK range (default U+4E00)
        // num_symbols: Number of CJK characters to use (default 4096)
        // The first symbol is reserved as negative marker
        explicit WikidataTokenEncoder(
            char32_t base_codepoint = 0x4E00,
            uint32_t num_symbols    = 4096)
            : _base_codepoint(base_codepoint)
            , _base(num_symbols - 1) // One symbol reserved for sign marker
        {
            if (num_symbols < 2)
            {
                throw std::invalid_argument("num_symbols must be at least 2");
            }
        }

        // Encode integer to compact string (Q-items positive, P-items negative)
        [[nodiscard]] std::string encode(int64_t value) const
        {
            std::string result;

            bool     negative  = value < 0;
            uint64_t abs_value = negative ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);

            // Convert to base-N (least significant digit first)
            std::u32string digits;
            if (abs_value == 0)
            {
                digits.push_back(_base_codepoint + 1); // '0' is second symbol
            }
            else
            {
                while (abs_value > 0)
                {
                    // +1 offset because first symbol is sign marker
                    digits.push_back(_base_codepoint + 1 + (abs_value % _base));
                    abs_value /= _base;
                }
            }

            // Reverse to most-significant-first order
            std::reverse(digits.begin(), digits.end());

            // Prepend negative marker if needed
            if (negative)
            {
                digits.insert(digits.begin(), _base_codepoint); // First symbol = negative
            }

            // Convert UTF-32 to UTF-8
            for (char32_t cp : digits)
            {
                string::utf8::append(result, cp);
            }

            return result;
        }

        // Overloaded encode that directly handles Wikidata item strings (Q... or P...)
        // If the input matches the Wikidata pattern, it is encoded using the int64_t version.
        // Otherwise, the input is returned unchanged (treated as a literal token).
        [[nodiscard]] std::string encode(std::string_view item) const
        {
            if (item.empty())
            {
                return {};
            }

            char prefix = item[0];
            if (item.size() >= 2 && (prefix == 'Q' || prefix == 'P') && std::all_of(item.begin() + 1, item.end(), [](unsigned char c)
                                                                                    { return std::isdigit(c); }))
            {
                std::string num_str(item.substr(1));
                try
                {
                    uint64_t num = std::stoull(num_str);
                    if (num == 0)
                    {
                        // Q0/P0 are not valid Wikidata items → treat as literal string
                        return std::string(item);
                    }
                    int64_t value = (prefix == 'Q') ? static_cast<int64_t>(num)
                                                    : -static_cast<int64_t>(num);
                    return encode(value); // Call the int64_t overload
                }
                catch (...)
                {
                    // Parsing failed (e.g. number too large) → treat as literal
                    return std::string(item);
                }
            }

            // Not a Wikidata item pattern → return unchanged
            return std::string(item);
        }

        // Decode string back to integer
        [[nodiscard]] int64_t decode(std::string_view encoded) const
        {
            if (encoded.empty())
            {
                throw std::invalid_argument("Empty string cannot be decoded");
            }

            // Parse UTF-8 to codepoints
            std::u32string codepoints;
            size_t         i = 0;
            while (i < encoded.size())
            {
                char32_t cp = string::utf8::read(encoded, i);
                codepoints.push_back(cp);
            }

            if (codepoints.empty())
            {
                throw std::invalid_argument("No valid codepoints found");
            }

            // Check for negative marker
            bool   negative = (codepoints[0] == _base_codepoint);
            size_t start    = negative ? 1 : 0;

            if (start >= codepoints.size())
            {
                throw std::invalid_argument("Missing digits after sign marker");
            }

            // Decode base-N
            uint64_t result = 0;
            for (size_t j = start; j < codepoints.size(); ++j)
            {
                char32_t cp = codepoints[j];
                if (cp < _base_codepoint + 1 || cp >= _base_codepoint + 1 + _base)
                {
                    throw std::invalid_argument("Invalid codepoint in encoded string");
                }
                uint64_t digit = cp - _base_codepoint - 1;
                result         = result * _base + digit;
            }

            return negative ? -static_cast<int64_t>(result) : static_cast<int64_t>(result);
        }

        // Decode to Wikidata item string (Q... or P...)
        // If the input is a valid encoded Wikidata item (i.e. decode() succeeds),
        // reconstruct and return the original item string (e.g. "Q42" or "P31").
        // If decode() fails (invalid codepoints, wrong range, etc.), treat the input
        // as a literal/non-Wikidata token and return it unchanged.
        [[nodiscard]] std::string decode_item(std::string_view encoded) const
        {
            try
            {
                int64_t value = decode(encoded);

                if (value == 0)
                {
                    // Should never happen for real Wikidata items, but handle gracefully
                    return "Q0";
                }

                uint64_t abs_value = value < 0 ? static_cast<uint64_t>(-value)
                                               : static_cast<uint64_t>(value);

                std::string num_str = std::to_string(abs_value);
                char        prefix  = (value > 0) ? 'Q' : 'P';

                return std::string(1, prefix) + num_str;
            }
            catch (const std::invalid_argument&)
            {
                // Not a valid encoded integer → treat as literal token
                return std::string(encoded);
            }
        }

        // Utility: Get number of symbols (tokens) needed for a value
        [[nodiscard]] size_t token_count(int64_t value) const
        {
            uint64_t abs_value = value < 0 ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);
            size_t   count     = (value < 0) ? 1 : 0; // Sign marker
            if (abs_value == 0) return count + 1;
            count += static_cast<size_t>(std::floor(std::log(abs_value) / std::log(_base))) + 1;
            return count;
        }

    private:
        char32_t _base_codepoint;
        uint64_t _base;
    };
}
