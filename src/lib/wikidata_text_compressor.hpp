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

#include "wikidata_token_encoder.hpp"

#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zelph
{
    class WikidataTextCompressor
    {
    public:
        explicit WikidataTextCompressor(
            std::vector<char32_t> delimiters     = {U' ', U',', U'\t', U'\n'},
            char32_t              base_codepoint = 0x4E00,
            uint32_t              num_symbols    = 4096)
            : _encoder(base_codepoint, num_symbols)
        {
            if (delimiters.empty())
            {
                throw std::invalid_argument("At least one delimiter must be provided");
            }
            _delimiters.insert(delimiters.begin(), delimiters.end());
        }

        [[nodiscard]] std::string encode(std::string_view input) const
        {
            std::string result;
            std::string current_token;
            size_t      pos = 0;

            while (pos < input.size())
            {
                char32_t cp = string::utf8::read(input, pos);

                if (_delimiters.count(cp))
                {
                    if (!current_token.empty())
                    {
                        result += _encoder.encode(std::string_view(current_token));
                        current_token.clear();
                    }
                    string::utf8::append(result, cp);
                }
                else
                {
                    string::utf8::append(current_token, cp);
                }
            }

            if (!current_token.empty())
            {
                result += _encoder.encode(std::string_view(current_token));
            }

            return result;
        }

        [[nodiscard]] std::string decode(std::string_view encoded) const
        {
            std::string result;
            std::string current_token;
            size_t      pos = 0;

            while (pos < encoded.size())
            {
                char32_t cp = string::utf8::read(encoded, pos);

                if (_delimiters.count(cp))
                {
                    if (!current_token.empty())
                    {
                        result += _encoder.decode_item(std::string_view(current_token));
                        current_token.clear();
                    }
                    string::utf8::append(result, cp);
                }
                else
                {
                    string::utf8::append(current_token, cp);
                }
            }

            if (!current_token.empty())
            {
                result += _encoder.decode_item(std::string_view(current_token));
            }

            return result;
        }

    private:
        WikidataTokenEncoder _encoder;
        std::set<char32_t>   _delimiters;
    };
}
