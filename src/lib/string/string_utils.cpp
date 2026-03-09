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

#include <sstream>

namespace zelph::string
{
    namespace unicode
    {
        std::string unescape(const std::string& input)
        {
            std::string result;
            result.reserve(input.size());

            for (size_t i = 0; i < input.size(); ++i)
            {
                if (i + 5 < input.size() && input[i] == '\\' && input[i + 1] == 'u')
                {
                    std::string_view hexCode(input.data() + i + 2, 4);
                    bool             isValidHex = true;

                    for (char c : hexCode)
                    {
                        if (!std::isxdigit(static_cast<unsigned char>(c)))
                        {
                            isValidHex = false;
                            break;
                        }
                    }

                    if (isValidHex)
                    {
                        int codePoint = std::stoi(std::string(hexCode), nullptr, 16);

                        utf8::append(result, static_cast<char32_t>(codePoint));

                        i += 5;
                        continue;
                    }
                }

                result.push_back(input[i]);
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

    // This function adds guillemets (« and ») around the identifier unless it's empty, a single uppercase letter (variable),
    // or enclosed in parentheses (sub-expression). This marking helps in parsing the formatted string in Markdown::convert_to_md,
    // where guillemets distinguish identifiers from other elements like sub-expressions or variables.
    std::string mark_identifier(const std::string& str)
    {
        if (str.empty())
        {
            return str;
        }

        // All sentinel characters are ASCII, so checking raw bytes is safe in UTF-8.
        char front = str.front();
        char back  = str.back();

        // Single uppercase ASCII letter = variable
        if (str.size() == 1 && front >= 'A' && front <= 'Z')
        {
            return str;
        }

        if (front == '_'                    // variable
            || front == '(' || back == ')'  // sub expression
            || front == '<' || back == '>'  // sequence
            || front == '{' || back == '}'  // set
            || front == '[' || back == ']') // currently unused
        {
            return str;
        }

        return "«" + str + "»";
    }

    // Remove all guillemets (« and ») that were added by above function mark_identifier
    std::string unmark_identifiers(const std::string& str)
    {
        return string::replace_all_copy(replace_all_copy(str, "«", " "), "»", " ");
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

    std::vector<std::string> tokenize_quoted(const std::string& input)
    {
        std::vector<std::string> tokens;
        std::string              current;
        bool                     in_quotes = false;
        bool                     escape    = false;

        for (char c : input)
        {
            if (escape)
            {
                current.push_back(c);
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
                in_quotes = !in_quotes;
                continue;
            }

            if (!in_quotes && (c == ' ' || c == '\t'))
            {
                if (!current.empty())
                {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
                continue;
            }

            current.push_back(c);
        }

        if (!current.empty())
            tokens.push_back(std::move(current));

        return tokens;
    }
}
