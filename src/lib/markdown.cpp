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

#include "markdown.hpp"
#include "utils.hpp"
#include "zelph.hpp"

#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace zelph::wikidata
{
    static std::unordered_map<std::string, std::mutex> file_mutexes;
    static std::mutex                                  mutexes_mutex;

    static std::string get_wikidata_url(const std::string& id)
    {
        return "https://www.wikidata.org/wiki/" + ((*id.begin() == 'P') ? "Property:" + id : id);
    }

    Markdown::Markdown(const std::filesystem::path& base_directory, network::Zelph* const zelph)
        : _base_directory(base_directory)
        , _zelph(zelph)
    {
        if (!std::filesystem::exists(_base_directory))
        {
            throw std::runtime_error("The base directory does not exist: " + _base_directory.string());
        }

        if (!std::filesystem::is_directory(_base_directory))
        {
            throw std::runtime_error("The given base directory path exists but is not a directory: " + _base_directory.string());
        }
    }

    std::string Markdown::get_template(const std::string& id) const
    {
        std::string         name;
        const network::Node node = _zelph->get_node(network::utils::wstr(id), "wikidata");

        if (node != 0)
        {
            const std::wstring w_name = _zelph->get_name(node, "en", true);
            name                      = network::utils::str(network::utils::convert_unicode_escapes(w_name));
        }

        if (name.empty())
        {
            name = id;
        }

        std::stringstream template_stream;
        template_stream << "# [" << name << "]("
                        << get_wikidata_url(id) << ")\n\n";
        return template_stream.str();
    }

    std::string Markdown::get_wikidata_id(const std::wstring& token, const std::string& lang) const
    {
        // Get node for this token
        uint64_t node = _zelph->get_node(token, "en");

        if (node == 0)
        {
            node = _zelph->get_node(token, "zelph"); // fallback
        }

        return node == 0 ? "" : network::utils::str(_zelph->get_name(node, "wikidata", true));
    }

    std::pair<std::list<std::string>, std::wstring> Markdown::convert_to_md(const std::wstring& message) const
    {
        std::list<std::string> wikidataIds;
        std::wstring           markdownContent = message;

        // Find the position of the '⇐' character
        size_t       cutoffPos    = *message.begin() == '!' ? std::wstring::npos : message.find(L"⇐");
        std::wstring processRange = (cutoffPos != std::wstring::npos) ? message.substr(0, cutoffPos) : message;

        // Regular expression to find tokens enclosed in «» characters
        std::wregex tokenRegex(L"«([^»]+)»");

        // Find all tokens in the message
        std::wsregex_iterator tokensBegin(processRange.begin(), processRange.end(), tokenRegex);
        std::wsregex_iterator tokensEnd;

        // Process each token
        for (std::wsregex_iterator i = tokensBegin; i != tokensEnd; ++i)
        {
            const std::wsmatch& match = *i;
            std::wstring        token = match[1].str();

            const std::string wikiDataId = get_wikidata_id(token, "en");
            if (wikiDataId.empty())
            {
                std::cout << "-------- Could not find wikidata ID of '" << network::utils::str(token) << "'" << std::endl;
            }
            else
            {
                wikidataIds.push_back(wikiDataId);
            }
        }

        // Replace all «token» with [token](token.md) in the markdown content
        std::wstring::const_iterator contentStart = markdownContent.begin();
        std::wstring::const_iterator contentEnd   = markdownContent.end();
        std::wsmatch                 contentMatch;

        // Create a copy of the original content to build the result
        std::wstring result;

        // Find and replace all tokens in the original message
        while (std::regex_search(contentStart, contentEnd, contentMatch, tokenRegex))
        {
            // Add the part before the match
            result += std::wstring(contentStart, contentMatch[0].first);

            // Get the token without «»
            std::wstring tokenText = contentMatch[1].str();

            const std::string wikiDataId = get_wikidata_id(tokenText, "en");
            std::string       url;

            if (wikiDataId.empty())
            {
                url = get_wikidata_url(network::utils::str(tokenText));
            }
            else
            {
                url = wikiDataId + ".md";

                if (wikiDataId[0] == 'P')
                {
                    tokenText = L"*" + tokenText + L"*";
                }
            }

            // Add the formatted Markdown link
            result += L"[" + tokenText + L"](" + network::utils::wstr(url) + L")";

            // Move the start iterator past the match
            contentStart = contentMatch[0].second;
        }

        // Add any remaining content after the last match
        result = L"- " + result + std::wstring(contentStart, contentEnd);
        result = network::utils::convert_unicode_escapes(result);

        return std::make_pair(wikidataIds, result);
    }

    void Markdown::add(const std::wstring& heading, const std::wstring& message) const
    {
        auto [ids, markdown_code] = convert_to_md(message);

        for (const auto& id : ids)
        {
            std::filesystem::path file_path = _base_directory / (id + ".md");

            std::mutex* file_mutex;
            {
                std::lock_guard<std::mutex> lock(mutexes_mutex);
                file_mutex = &file_mutexes[file_path.string()];
            }

            std::lock_guard<std::mutex> file_lock(*file_mutex);

            std::wstring formatted_heading = L"## " + heading;

            if (!std::filesystem::exists(file_path))
            {
                std::ofstream new_file(file_path);
                if (!new_file.is_open())
                {
                    throw std::runtime_error("Failed to create file: " + file_path.string());
                }
                new_file << get_template(id);
                new_file.close();
            }

            std::wifstream file(file_path, std::ios::binary);
            file.imbue(std::locale(""));
            if (!file.is_open())
            {
                throw std::runtime_error("Failed to open file for reading: " + file_path.string());
            }

            std::vector<std::wstring> lines;
            std::wstring              line;
            while (std::getline(file, line))
            {
                lines.push_back(line);
            }
            file.close();

            std::vector<std::wstring> markdown_lines;
            std::wistringstream       markdown_stream(markdown_code);
            markdown_stream.imbue(std::locale(""));
            std::wstring markdown_line;
            while (std::getline(markdown_stream, markdown_line))
            {
                markdown_lines.push_back(markdown_line);
            }

            bool markdown_exists = false;
            if (!markdown_lines.empty())
            {
                for (int i = 0; i <= static_cast<int>(lines.size()) - static_cast<int>(markdown_lines.size()); ++i)
                {
                    bool match = true;
                    for (size_t j = 0; j < markdown_lines.size(); ++j)
                    {
                        if (lines[i + j] != markdown_lines[j])
                        {
                            match = false;
                            break;
                        }
                    }
                    if (match)
                    {
                        markdown_exists = true;
                        break;
                    }
                }
            }

            if (markdown_exists)
            {
                return;
            }

            int heading_index = -1;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (lines[i] == formatted_heading)
                {
                    heading_index = static_cast<int>(i);
                    break;
                }
            }

            std::ofstream output_file(file_path);
            if (!output_file.is_open())
            {
                throw std::runtime_error("Failed to open file for writing: " + file_path.string());
            }

            if (heading_index == -1)
            {
                for (const auto& l : lines)
                {
                    output_file << network::utils::str(l) << "\n";
                }

                if (!lines.empty() && !lines.back().empty())
                {
                    output_file << "\n";
                }

                output_file << network::utils::str(formatted_heading) << "\n\n";
                output_file << network::utils::str(markdown_code) << "\n";
            }
            else
            {
                for (int i = 0; i <= heading_index; ++i)
                {
                    output_file << network::utils::str(lines[i]) << "\n";
                }

                bool has_blank_line = (heading_index + 1 < static_cast<int>(lines.size()) && lines[heading_index + 1].empty());

                if (has_blank_line)
                {
                    output_file << "\n";

                    output_file << network::utils::str(markdown_code) << "\n";

                    for (size_t i = heading_index + 2; i < lines.size(); ++i)
                    {
                        output_file << network::utils::str(lines[i]) << "\n";
                    }
                }
                else
                {
                    output_file << "\n";

                    output_file << network::utils::str(markdown_code) << "\n";

                    for (size_t i = heading_index + 1; i < lines.size(); ++i)
                    {
                        output_file << network::utils::str(lines[i]) << "\n";
                    }
                }
            }

            output_file.close();
        }
    }
}