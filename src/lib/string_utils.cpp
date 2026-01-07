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

#include <boost/locale/encoding_utf.hpp>

#include <codecvt>
#include <cwctype>
#include <iostream>
#include <locale>
#include <regex>
#include <stdexcept>

namespace zelph
{
    namespace string
    {
        namespace unicode
        {
            std::string to_utf8(const std::wstring& wstr)
            {
                try
                {
                    return boost::locale::conv::utf_to_utf<char>(wstr);
                }
                catch (const boost::locale::conv::conversion_error& e)
                {
                    std::wcerr << L"Error converting wstring to UTF-8 string: " << e.what() << std::endl;

                    std::string result;
                    for (size_t i = 0; i < wstr.length(); ++i)
                    {
                        wchar_t wc = wstr[i];

                        if (wc >= 0 && wc <= 127)
                        {
                            result.push_back(static_cast<char>(wc));
                        }
                        else
                        {
                            try
                            {
                                std::wstring                                     singleChar(1, wc);
                                std::wstring_convert<std::codecvt_utf8<wchar_t>> singleConverter;
                                result += singleConverter.to_bytes(singleChar);
                            }
                            catch (...)
                            {
                                result += "?";
                                std::wcerr << L"Replaced invalid character at position " << i << std::endl;
                            }
                        }
                    }

                    return result;
                }
            }

            std::wstring from_utf8(std::string str)
            {
                return boost::locale::conv::utf_to_utf<wchar_t>(str);
            }

            std::wstring unescape(const std::wstring& input)
            {
                std::wstring result;
                result.reserve(input.size());

                for (size_t i = 0; i < input.size(); ++i)
                {
                    if (i + 5 < input.size() && input[i] == L'\\' && input[i + 1] == L'u')
                    {
                        std::wstring hexCode    = input.substr(i + 2, 4);
                        bool         isValidHex = true;

                        for (wchar_t c : hexCode)
                        {
                            if (!std::isxdigit(static_cast<unsigned char>(c)))
                            {
                                isValidHex = false;
                                break;
                            }
                        }

                        if (isValidHex)
                        {
                            int codePoint = std::stoi(std::string(hexCode.begin(), hexCode.end()), nullptr, 16);

                            result.push_back(static_cast<wchar_t>(codePoint));

                            i += 5;
                            continue;
                        }
                    }

                    result.push_back(input[i]);
                }

                return result;
            }
        }

        // This function adds guillemets (« and ») around the identifier unless it's empty, a single uppercase letter (variable),
        // or enclosed in parentheses (sub-expression). This marking helps in parsing the formatted string in Markdown::convert_to_md,
        // where guillemets distinguish identifiers from other elements like sub-expressions or variables.
        std::wstring mark_identifier(const std::wstring& str)
        {
            if (str.empty()
                || (str.length() == 1 && std::iswupper(str[0]))
                || str.front() == L'('
                || str.back() == L')')
            {
                return str;
            }

            return L"«" + str + L"»";
        }
    }
}
