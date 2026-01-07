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

#include "markdown.hpp"
#include "string_utils.hpp"
#include "zelph.hpp"

#include <ankerl/unordered_dense.h>

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace zelph::wikidata
{
    static std::string get_wikidata_url(const std::string& id)
    {
        return "https://www.wikidata.org/wiki/" + ((*id.begin() == 'P') ? "Property:" + id : id);
    }

    Markdown::Markdown(const std::filesystem::path& base_directory, network::Zelph* zelph)
        : _base_directory(base_directory)
        , _zelph(zelph)
    {
        if (!std::filesystem::exists(_base_directory))
            throw std::runtime_error("Base directory does not exist: " + _base_directory.string());
        if (!std::filesystem::is_directory(_base_directory))
            throw std::runtime_error("Base path is not a directory: " + _base_directory.string());

        writer_thread = std::thread(&Markdown::writer_loop, this);
    }

    Markdown::~Markdown()
    {
        {
            std::lock_guard<std::mutex> lock(add_mutex);
            shutdown = true;
        }
        add_cv.notify_all();
        if (writer_thread.joinable())
            writer_thread.join();
    }

    std::string Markdown::get_template(const std::string& id) const
    {
        std::string         name;
        const network::Node node = _zelph->get_node(string::unicode::from_utf8(id), "wikidata");
        if (node != 0)
        {
            const std::wstring w_name = _zelph->get_name(node, "en", true);
            name                      = string::unicode::to_utf8(string::unicode::unescape(w_name));
        }
        if (name.empty()) name = id;

        std::stringstream ss;
        ss << "# [" << name << "](" << get_wikidata_url(id) << ")\n\n";
        return ss.str();
    }

    // Simple but sufficient 64-bit hash for block content
    uint64_t Markdown::hash_block(const std::vector<std::wstring>& block_lines)
    {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (const auto& line : block_lines)
        {
            std::string utf8 = string::unicode::to_utf8(line);
            for (char c : utf8)
            {
                h ^= static_cast<uint64_t>(c);
                h *= 0x100000001b3ULL;
            }
            h ^= 0x9e3779b97f4a7c15ULL; // separate lines
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    std::string Markdown::get_wikidata_id(const std::wstring& token, const std::string& lang) const
    {
        // Get node for this token
        uint64_t node = _zelph->get_node(token, lang);

        if (node == 0 && lang != "zelph")
        {
            node = _zelph->get_node(token, "zelph"); // fallback
        }

        return node == 0 ? "" : string::unicode::to_utf8(_zelph->get_name(node, "wikidata", true));
    }

    std::pair<std::list<std::string>, std::wstring> Markdown::convert_to_md(const std::wstring& message) const
    {
        // Converts the formatted fact string (from format_fact) to Markdown.
        // Parses tokens within « », extracts ID and text if " - " present (Wikidata mode), else assumes token is the Wikidata ID or fallback name.
        // Creates links like [text](ID.md), italicizing if property (P...). Uses the pre-formatted ID from format_fact for accurate linking.

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

        // Process each token to collect IDs
        for (std::wsregex_iterator i = tokensBegin; i != tokensEnd; ++i)
        {
            const std::wsmatch& match = *i;
            std::wstring        token = match[1].str();

            size_t sep_pos = token.find(L" - ");
            if (sep_pos != std::wstring::npos)
            {
                std::wstring id_part = token.substr(0, sep_pos);
                wikidataIds.push_back(string::unicode::to_utf8(id_part));
            }
            else
            {
                wikidataIds.push_back(string::unicode::to_utf8(token)); // Assume token is ID
            }
        }

        // Replace all «token» with [token](token.md) in the Markdown content
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
            std::wstring token = contentMatch[1].str();

            size_t       sep_pos = token.find(L" - ");
            std::wstring tokenText;
            std::string  id_str;
            if (sep_pos != std::wstring::npos)
            {
                std::wstring id_part = token.substr(0, sep_pos);
                tokenText            = token.substr(sep_pos + 3);
                id_str               = string::unicode::to_utf8(id_part);
            }
            else
            {
                tokenText = token;
                id_str    = string::unicode::to_utf8(token);
            }

            std::string url = id_str + ".md";

            if (!id_str.empty() && id_str[0] == 'P')
            {
                tokenText = L"*" + tokenText + L"*";
            }

            // Add the formatted Markdown link
            result += L"[" + tokenText + L"](" + string::unicode::from_utf8(url) + L")";

            // Move the start iterator past the match
            contentStart = contentMatch[0].second;
        }

        // Add any remaining content after the last match
        result = L"- " + result + std::wstring(contentStart, contentEnd);
        result = string::unicode::unescape(result);

        return std::make_pair(wikidataIds, result);
    }

    void Markdown::add(const std::wstring& heading, const std::wstring& message) const
    {
        auto [ids, markdown_code] = convert_to_md(message);

        {
            std::lock_guard<std::mutex> lock(add_mutex);
            for (const auto& id : ids)
            {
                pending_adds[id].emplace_back(heading, markdown_code);
            }
        }
        add_cv.notify_one(); // only one needed, even for multiple files
    }

    void Markdown::writer_loop() const
    {
        for (;;)
        {
            std::unique_lock<std::mutex> lock(add_mutex);
            add_cv.wait(lock, [this]
                        { return shutdown || !pending_adds.empty(); });

            if (shutdown && pending_adds.empty())
                return;

            // Process all pending files in one go
            decltype(pending_adds) current_batch;
            current_batch.swap(pending_adds);
            lock.unlock();

            for (auto& [id, adds] : current_batch)
            {
                std::filesystem::path file_path = _base_directory / (id + ".md");
                std::filesystem::path temp_path = file_path.string() + ".tmp";
                FileState&            state     = file_states[id];

                // Load file if not in memory yet (first time)
                if (state.lines.empty())
                {
                    if (std::filesystem::exists(file_path))
                    {
                        std::wifstream file(file_path, std::ios::binary);
                        file.imbue(std::locale(""));
                        std::wstring line;
                        while (std::getline(file, line))
                            state.lines.push_back(line);
                    }
                    else
                    {
                        std::string         templ = get_template(id);
                        std::wistringstream ss(string::unicode::from_utf8(templ));
                        ss.imbue(std::locale(""));
                        std::wstring line;
                        while (std::getline(ss, line))
                            state.lines.push_back(line);
                    }

                    // Build initial hash set
                    size_t i = 0;
                    while (i < state.lines.size())
                    {
                        if (state.lines[i].starts_with(L"## "))
                        {
                            size_t start = i;
                            ++i;
                            while (i < state.lines.size() && !state.lines[i].starts_with(L"## "))
                                ++i;
                            std::vector<std::wstring> block(state.lines.begin() + start, state.lines.begin() + i);
                            state.block_hashes.insert(hash_block(block));
                        }
                        else
                        {
                            ++i;
                        }
                    }
                }

                // Process all pending adds for this file
                bool changed = false;
                for (auto& [heading, markdown_code] : adds)
                {
                    std::vector<std::wstring> markdown_lines;
                    std::wistringstream       mstream(markdown_code);
                    mstream.imbue(std::locale(""));
                    std::wstring ml;
                    while (std::getline(mstream, ml))
                        markdown_lines.push_back(ml);

                    if (markdown_lines.empty())
                        continue;

                    // Build full block: heading + empty line + markdown lines
                    std::vector<std::wstring> new_block;
                    new_block.push_back(L"## " + heading);
                    new_block.push_back(L"");
                    new_block.insert(new_block.end(), markdown_lines.begin(), markdown_lines.end());

                    uint64_t block_hash = hash_block(new_block);
                    if (state.block_hashes.contains(block_hash))
                        continue; // duplicate → skip

                    // Insert into content
                    std::wstring formatted_heading = new_block[0];
                    auto         it                = std::find(state.lines.begin(), state.lines.end(), formatted_heading);
                    if (it == state.lines.end())
                    {
                        // New heading → append at end
                        if (!state.lines.empty() && !state.lines.back().empty())
                            state.lines.push_back(L"");
                        state.lines.insert(state.lines.end(), new_block.begin(), new_block.end());
                    }
                    else
                    {
                        // Existing heading → insert after heading + empty line
                        auto insert_pos = std::next(it);
                        while (insert_pos != state.lines.end() && insert_pos->empty())
                            ++insert_pos;
                        state.lines.insert(insert_pos, new_block.begin() + 2, new_block.end()); // skip heading & empty
                    }

                    state.block_hashes.insert(block_hash);
                    changed = true;
                }

                // Write back only if something changed
                if (changed)
                {
                    std::ofstream out(temp_path);
                    if (!out.is_open())
                    {
                        std::cerr << "Failed to write temp " << temp_path << "\n";
                        continue;
                    }
                    for (const auto& l : state.lines)
                        out << string::unicode::to_utf8(l) << "\n";

                    out.close();

                    try
                    {
                        std::filesystem::rename(temp_path, file_path);
                    }
                    catch (const std::filesystem::filesystem_error& e)
                    {
                        std::cerr << "Failed to rename temp to " << file_path << ": " << e.what() << "\n";
                        std::filesystem::remove(temp_path); // Cleanup
                    }
                }
            }
        }
    }
}
