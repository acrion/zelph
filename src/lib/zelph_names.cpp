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

#include "zelph.hpp"

#include "node_to_string.hpp"
#include "zelph_impl.hpp"

using namespace zelph::network;

// Assigns or updates the name of an existing node for a specific language.
//
// This overload is used when you already have a valid Node handle and want to
// directly assign or update its name in the given language.
// It does not create new nodes — it only updates the bidirectional name mappings
// for that language.
//
// If another node already has this name *and* merge_on_conflict is true,
// the other node's connections are merged into this node, and the other node
// is subsequently removed.
void Zelph::set_name(const Node node, const std::wstring& name, std::string lang, const bool merge_on_conflict)
{
    if (lang.empty()) lang = _lang; // Use current default language if none specified

#if _DEBUG
    diagnostic_stream() << L"Node " << node << L" has name '" << name << L"' (" << std::wstring(lang.begin(), lang.end()) << L")" << std::endl;
#endif

    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    std::lock_guard lock2(_pImpl->_mtx_name_of_node);

    // Store the forward mapping: node → name (in this language)
    _pImpl->_name_of_node[lang][node] = name;

    if (merge_on_conflict)
    {
        // Check if this name is already mapped to a different node in this language
        auto existing = _pImpl->_node_of_name[lang].find(name);
        if (existing == _pImpl->_node_of_name[lang].end())
        {
            // Name is new in this language → create clean bidirectional mapping
            _pImpl->_node_of_name[lang][name] = node;
        }
        else if (existing->second != node)
        {
            // Conflict: the same name is already used by another node
            Node from = existing->second;
            Node into = node;

            if (Impl::is_var(from) != Impl::is_var(into))
            {
                std::stringstream s;
                s << "Requested name '" << string::unicode::to_utf8(name) << "' is already used by node " << existing->second << " in language '" << lang << "'. Merging the two nodes is impossible because one node is a variable, the other not.";
                throw std::runtime_error(s.str());
            }

            if (!Impl::is_var(from))
            {
                out_stream() << "Warning: Merging Node " << from
                             << " into Node " << into
                             << " due to name conflict '" << name
                             << "' in language '" << lang << "'." << std::endl;
            }

            invalidate_fact_structures_cache();

            // Merge connections from the conflicting node into this one
            _pImpl->merge(from, into);

            // Transfer names from the merged-away node to the surviving node
            _pImpl->transfer_names(from, into);

            // Update reverse mapping to point to the surviving node
            _pImpl->_node_of_name[lang][name] = into;
        }
    }
    else // !merge_on_conflict
    {
        // If merging is not requested, we override the name → node mapping
        // regardless of whether the name already exists for another node.
        //
        // This is intentionally used for:
        // - Wikidata import (names in other languages are ambiguous)
        //   See Wikidata::process_import()
        // - Variable nodes in rules (they only ever refer to the current rule)
        //   This keeps the graph cleaner. See console::Interactive::Impl::process_fact()
        _pImpl->_node_of_name[lang][name] = node;
    }
}

// Assigns or links a name in a foreign language to a node and ensures the name in the current default language (_lang) is correctly set.
// This overload is primarily used by the interactive `.name` command.
// It either finds an existing node via the foreign-language name or creates a new one if none exists.
// At the same time, it updates or corrects the name in the current default language.
Node Zelph::set_name(const std::wstring& name_in_current_lang, const std::wstring& name_in_given_lang, std::string lang)
{
    if (lang.empty() || lang == _lang)
    {
        throw std::runtime_error("Zelph::set_name: Source and target language must not be the same");
    }

    Node result_node;

    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    std::lock_guard lock2(_pImpl->_mtx_name_of_node);

    // 1. Look for an existing node that already has this name in the foreign language
    auto existing = _pImpl->_node_of_name[lang].find(name_in_given_lang);
    if (existing == _pImpl->_node_of_name[lang].end())
    {
        // No node found for this foreign name → create a new node using the current-language name
        result_node = node(name_in_current_lang, _lang);

        // Establish bidirectional mappings for the foreign language
        _pImpl->_node_of_name[lang][name_in_given_lang] = result_node;
        _pImpl->_name_of_node[lang][result_node]        = name_in_given_lang;
    }
    else
    {
        // Node already exists via the foreign-language name
        result_node = existing->second;

        // Consistency check: the reverse mapping (name_of_node) must exist
        auto existing2 = _pImpl->_name_of_node[lang].find(result_node);
        if (existing2 == _pImpl->_name_of_node[lang].end())
        {
            throw std::runtime_error("Zelph::set_name: Internal error – name mappings are inconsistent.");
        }

        // Retrieve current-language name mappings for updates
        auto& name_of_node_cur = _pImpl->_name_of_node[_lang];
        auto& node_of_name_cur = _pImpl->_node_of_name[_lang];

        // Get the previously stored name in the current language (if any)
        auto         it_current_name  = name_of_node_cur.find(result_node);
        std::wstring old_current_name = (it_current_name != name_of_node_cur.end()) ? it_current_name->second : L"";

        // If the stored current-language name differs from the desired one → update it
        if (old_current_name != name_in_current_lang)
        {
            if (!old_current_name.empty())
            {
                // Remove the old current-language name from the reverse mapping
                node_of_name_cur.erase(old_current_name);

                // Remove from forward mapping
                name_of_node_cur.erase(result_node);
            }

            // Check if the desired new current-language name is already assigned to another node
            auto it_conflict = node_of_name_cur.find(name_in_current_lang);
            if (it_conflict != node_of_name_cur.end() && it_conflict->second != result_node)
            {
                Node conflicting_node = it_conflict->second;

                // Determine merge direction: higher ID into lower ID
                Node from = result_node;
                Node into = conflicting_node;

                if (Impl::is_var(from) != Impl::is_var(into))
                {
                    std::stringstream s;
                    s << "Requested name '" << string::unicode::to_utf8(name_in_current_lang) << "' is already used by node " << into << " in language '" << lang << "'. Merging the two nodes is impossible because one node is a variable, the other not.";
                    throw std::runtime_error(s.str());
                }

                if (!Impl::is_var(from))
                {
                    out_stream() << "Warning: Merging Node " << from
                                 << " into Node " << into
                                 << " due to name conflict '" << name_in_current_lang
                                 << "' in language '" << _lang << "'." << std::endl;
                }

                invalidate_fact_structures_cache();

                // Merge
                _pImpl->merge(from, into);

                // Transfer names from the merged node
                _pImpl->transfer_names(from, into);

                // Update result_node to the surviving node
                result_node = into;
            }

            // Apply the new current-language mappings
            node_of_name_cur[name_in_current_lang] = result_node;
            name_of_node_cur[result_node]          = name_in_current_lang;
        }
    }

    return result_node; // The (possibly newly created or merged) node
}

std::wstring Zelph::get_name(const Node node, std::string lang, const bool fallback) const
{
    if (lang.empty()) lang = _lang;

    // return `node` in the requested language (if available)
    {
        std::lock_guard lock(_pImpl->_mtx_name_of_node);
        auto&           name_of_node = _pImpl->_name_of_node[lang];
        auto            it           = name_of_node.find(node);
        if (it != name_of_node.end())
        {
            return it->second;
        }
    }

    if (!fallback)
    {
        // return empty string if this node has no name
        return L"";
    }

    // try English as fallback language
    {
        std::lock_guard lock(_pImpl->_mtx_name_of_node);
        auto&           name_of_node = _pImpl->_name_of_node["en"];
        auto            it           = name_of_node.find(node);
        if (it != name_of_node.end())
        {
            return it->second;
        }
    }

    // try zelph as fallback language
    {
        std::lock_guard lock(_pImpl->_mtx_name_of_node);
        auto&           name_of_node = _pImpl->_name_of_node["zelph"];
        auto            it           = name_of_node.find(node);
        if (it != name_of_node.end())
        {
            return it->second;
        }
    }

    // try an arbitrary language as fallback
    {
        std::lock_guard lock(_pImpl->_mtx_name_of_node);
        for (const auto& l : _pImpl->_name_of_node)
        {
            auto it2 = l.second.find(node);
            if (it2 != l.second.end())
                return it2->second;
        }
    }

    // Fallback to core node names
    {
        auto it = _core_names.left.find(node);
        if (it != _core_names.left.end())
        {
            return it->second;
        }
    }

    return L"";
}

// If in Wikidata mode (has_language("wikidata") && lang != "wikidata"), get_formatted_name prepends Wikidata IDs to names with " - " separator
// for nodes that have both a name in the requested language and a Wikidata ID. This allows Markdown::convert_to_md to parse
// and create appropriate links using the ID for the URL and the name for display text.
std::wstring Zelph::get_formatted_name(const Node node, const std::string& lang) const
{
    const bool is_wikidata_mode = has_language("wikidata") && lang != "wikidata";
    if (!is_wikidata_mode)
    {
        return get_name(node, lang, true);
    }

    std::wstring wikidata_name = get_name(node, "wikidata", false);

    std::wstring name;
    if (lang == "zelph")
    {
        // In Wikidata mode, the output of get_formatted_name may be used by the Markdown export (command `.run-md`).
        // In this case, we want to use a natural language as the primary language.
        // The "zelph" language is only intended to offer an agnostic language in addition to natural languages, not for the Markdown export.
        // So in case lang is not a natural language, we fall back to English.
        name = get_name(node, "en", false);
    }

    if (name.empty() || name == wikidata_name)
    {
        // either lang != "zelph", or there is no English name of `node`, or its English nam is identical with the Wikidata name
        name = get_name(node, lang, false);
    }

    if (name.empty())
    {
        if (wikidata_name.empty())
        {
            return get_name(node, lang, true); // Fallback, no prepend
        }
        else
        {
            return wikidata_name; // No prepend since lang name was empty
        }
    }
    else
    {
        if (!wikidata_name.empty() && wikidata_name != name)
        {
            name = wikidata_name + L" - " + name;
        }
        return name;
    }
}

bool Zelph::has_name(const Node node, const std::string& lang) const
{
    std::lock_guard lock(_pImpl->_mtx_name_of_node);

    auto& name_of_node = _pImpl->_name_of_node[lang];
    auto  it           = name_of_node.find(node);
    return it != name_of_node.end();
}

void Zelph::remove_name(Node node, std::string lang)
{
    if (lang.empty()) lang = _lang;

    std::lock_guard lock1(_pImpl->_mtx_name_of_node);
    std::lock_guard lock2(_pImpl->_mtx_node_of_name);

    auto& name_map = _pImpl->_name_of_node[lang];
    auto  name_it  = name_map.find(node);
    if (name_it == name_map.end())
    {
        return; // nothing to remove
    }

    std::wstring old_name = name_it->second;
    name_map.erase(name_it);

    auto& rev_map = _pImpl->_node_of_name[lang];
    rev_map.erase(old_name);
}

void Zelph::unset_name(Node node, std::string lang /*= ""*/)
{
    if (lang.empty()) lang = _lang;

    std::lock_guard lock1(_pImpl->_mtx_name_of_node);
    std::lock_guard lock2(_pImpl->_mtx_node_of_name);

    auto& name_map = _pImpl->_name_of_node[lang];
    auto  it       = name_map.find(node);
    if (it != name_map.end())
    {
        std::wstring old_name = it->second;
        name_map.erase(it);

        auto& reverse_map = _pImpl->_node_of_name[lang];
        reverse_map.erase(old_name);
    }
}

Node Zelph::get_node(const std::wstring& name, std::string lang) const
{
    if (lang.empty()) lang = _lang;
    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    auto            node_of_name = _pImpl->_node_of_name.find(lang);
    if (node_of_name == _pImpl->_node_of_name.end())
    {
        return 0;
    }
    else
    {
        auto it = node_of_name->second.find(name);
        if (it == node_of_name->second.end())
        {
            return 0;
        }
        else
        {
            return it->second;
        }
    }
}

void Zelph::register_core_node(Node n, const std::wstring& name)
{
    _core_names.insert({n, name});
}

Node Zelph::get_core_node(const std::wstring& name) const
{
    auto it = _core_names.right.find(name);
    return (it != _core_names.right.end()) ? it->second : 0;
}

std::wstring Zelph::get_core_name(Node n) const
{
    auto it = _core_names.left.find(n);
    return (it != _core_names.left.end()) ? it->second : L"";
}

std::string Zelph::get_name_hex(Node node, bool prepend_num, int max_neighbors) const
{
    std::string name = string::unicode::to_utf8(get_name(node, _lang, true));

    if (name.empty())
    {
        if (Impl::is_var(node))
        {
            name = std::to_string(static_cast<int>(node));
        }
        else
        {
            std::wstring output;
            console::node_to_wstring(this, output, _lang, node, max_neighbors);
            name = string::unicode::to_utf8(output);
        }
    }
    else if (prepend_num && !Impl::is_hash(node) && !Impl::is_var(node))
    {
        name = "(" + std::to_string(static_cast<unsigned long long>(node)) + ") " + name;
    }

    return name;
}

std::string Zelph::format(Node node) const
{
    std::wstring result;
    console::node_to_wstring(this, result, _lang, node);
    return string::unicode::to_utf8(result);
}

std::vector<std::string> Zelph::get_languages() const
{
    std::lock_guard          lock(_pImpl->_mtx_node_of_name);
    std::vector<std::string> result;

    for (const auto& outer_pair : _pImpl->_node_of_name)
    {
        result.push_back(outer_pair.first);
    }

    return result;
}

bool Zelph::has_language(const std::string& language) const
{
    const auto& languages = get_languages();
    return std::find(languages.begin(), languages.end(), language) != languages.end();
}

name_of_node_map Zelph::get_nodes_in_language(const std::string& lang) const
{
    std::lock_guard lock(_pImpl->_mtx_name_of_node);

    const auto& outer = _pImpl->_name_of_node;
    auto        it    = outer.find(lang);
    if (it == outer.end())
    {
        return name_of_node_map{};
    }
    return it->second;
}

std::vector<Node> Zelph::resolve_nodes_by_name(const std::wstring& name) const
{
    std::vector<network::Node> results;

    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    auto            lang_it = _pImpl->_node_of_name.find(lang());
    if (lang_it != _pImpl->_node_of_name.end())
    {
        const auto& rev_map = lang_it->second;
        auto        range   = rev_map.equal_range(name);
        for (auto it = range.first; it != range.second; ++it)
        {
            results.push_back(it->second);
        }
    }

    return results;
}

size_t Zelph::get_name_of_node_size(const std::string& lang) const
{
    std::lock_guard lock(_pImpl->_mtx_name_of_node);
    auto            it = _pImpl->_name_of_node.find(lang);
    return (it != _pImpl->_name_of_node.end()) ? it->second.size() : 0;
}

size_t Zelph::get_node_of_name_size(const std::string& lang) const
{
    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    auto            it = _pImpl->_node_of_name.find(lang);
    return (it != _pImpl->_node_of_name.end()) ? it->second.size() : 0;
}

size_t Zelph::language_count() const
{
    return get_languages().size();
}
