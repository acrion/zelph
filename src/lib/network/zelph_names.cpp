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

#include "string/node_to_string.hpp"
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
void Zelph::set_name(const Node         node,
                     const std::string& name,
                     std::string        lang,
                     const bool         merge_on_conflict)
{
    if (lang.empty()) lang = _lang;

#if _DEBUG
    diagnostic_stream() << "Node " << node << " has name '" << name
                        << "' (" << std::string(lang.begin(), lang.end()) << ")"
                        << std::endl;
#endif

    std::unique_lock lock_node(_pImpl->_mtx_node_of_name);
    std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

    Impl::ExclusiveNameAccessScope scope_node(Impl::_tls_node_of_name_exclusive_depth);
    Impl::ExclusiveNameAccessScope scope_name(Impl::_tls_name_of_node_exclusive_depth);

    Node target_node = node;

    if (merge_on_conflict)
    {
        Node conflict_node    = 0;
        bool conflict_is_core = false;

        // Check regular mapping first
        auto rev_outer_it = _pImpl->_node_of_name.find(lang);
        if (rev_outer_it != _pImpl->_node_of_name.end())
        {
            auto it = rev_outer_it->second.find(name);
            if (it != rev_outer_it->second.end() && it->second != node)
            {
                conflict_node = it->second;
            }
        }

        // If not found in regular map, check core names
        if (conflict_node == 0)
        {
            Node core_node = get_core_node(name);
            if (core_node != 0 && core_node != node)
            {
                conflict_node    = core_node;
                conflict_is_core = true;
            }
        }

        if (conflict_node != 0)
        {
            Node from;
            Node into;

            if (conflict_is_core)
            {
                // Core nodes must never be merged away
                from = node;
                into = conflict_node;
            }
            else
            {
                // Default behaviour: merge the conflicting node into the requested node
                from = conflict_node;
                into = node;

                // Defensive: if "from" is unexpectedly a core node, preserve it
                if (!get_core_name(from).empty())
                {
                    std::swap(from, into);
                }
            }

            if (Impl::is_var(from) != Impl::is_var(into))
            {
                std::stringstream s;
                if (conflict_is_core)
                {
                    s << "Requested name '" << name << "' is already used by core node "
                      << into
                      << ". Merging is impossible because one node is a variable, the other not.";
                }
                else
                {
                    s << "Requested name '" << name << "' is already used by node "
                      << conflict_node
                      << " in language '" << lang
                      << "'. Merging the two nodes is impossible because one node is a variable, the other not.";
                }
                throw std::runtime_error(s.str());
            }

            if (!Impl::is_var(from))
            {
                if (conflict_is_core)
                {
                    out_stream() << "Warning: (A) Merging Node \"" << format(from)
                                 << "\" into core Node \"" << format(into)
                                 << "\" due to name conflict '" << name
                                 << "' in language '" << lang << "'."
                                 << std::endl;
                }
                else
                {
                    out_stream() << "Warning: (B) Merging Node " << from
                                 << " into Node " << into
                                 << " due to name conflict '" << name
                                 << "' in language '" << lang << "'."
                                 << std::endl;
                }
            }

            invalidate_fact_structures_cache();

            _pImpl->merge(from, into);
            _pImpl->transfer_names_locked(from, into);

            target_node = into;
        }
    }

    // Also correct in the !merge_on_conflict case:
    // - old name of target_node is removed
    // - previous owner of "name" loses that forward mapping
    _pImpl->assign_name_locked(target_node, name, lang);
}

// Assigns or links a name in a foreign language to a node and ensures the name in the current default language (_lang) is correctly set.
// This overload is primarily used by the interactive `.name` command.
// It either finds an existing node via the foreign-language name or creates a new one if none exists.
// At the same time, it updates or corrects the name in the current default language.
Node Zelph::set_name(const std::string& name_in_current_lang,
                     const std::string& name_in_given_lang,
                     std::string        lang)
{
    if (lang.empty() || lang == _lang)
    {
        throw std::runtime_error("Zelph::set_name: Source and target language must not be the same");
    }

    // -------------------------------------------------------------------------
    // 1) Read-mostly fast path:
    //    If foreign name already exists and current-language name is already correct,
    //    we can return under shared locks only.
    // -------------------------------------------------------------------------
    {
        std::shared_lock lock_node(_pImpl->_mtx_node_of_name);

        auto foreign_reverse_outer_it = _pImpl->_node_of_name.find(lang);
        if (foreign_reverse_outer_it != _pImpl->_node_of_name.end())
        {
            auto foreign_reverse_it = foreign_reverse_outer_it->second.find(name_in_given_lang);
            if (foreign_reverse_it != foreign_reverse_outer_it->second.end())
            {
                const Node candidate = foreign_reverse_it->second;

                auto current_reverse_outer_it = _pImpl->_node_of_name.find(_lang);

                std::shared_lock lock_name(_pImpl->_mtx_name_of_node);

                auto foreign_forward_outer_it = _pImpl->_name_of_node.find(lang);
                auto current_forward_outer_it = _pImpl->_name_of_node.find(_lang);

                if (foreign_forward_outer_it != _pImpl->_name_of_node.end() && current_forward_outer_it != _pImpl->_name_of_node.end())
                {
                    auto foreign_forward_it = foreign_forward_outer_it->second.find(candidate);
                    auto current_forward_it = current_forward_outer_it->second.find(candidate);

                    if (foreign_forward_it != foreign_forward_outer_it->second.end() && foreign_forward_it->second == name_in_given_lang && current_forward_it != current_forward_outer_it->second.end() && current_forward_it->second == name_in_current_lang && current_reverse_outer_it != _pImpl->_node_of_name.end())
                    {
                        auto current_reverse_it = current_reverse_outer_it->second.find(name_in_current_lang);
                        if (current_reverse_it != current_reverse_outer_it->second.end() && current_reverse_it->second == candidate)
                        {
                            return candidate;
                        }
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2) Slow path: exclusive locks
    //    Global lock order must be consistent everywhere:
    //    _mtx_node_of_name -> _mtx_name_of_node
    // -------------------------------------------------------------------------
    std::unique_lock lock_node(_pImpl->_mtx_node_of_name);
    std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

    Impl::ExclusiveNameAccessScope scope_node(Impl::_tls_node_of_name_exclusive_depth);
    Impl::ExclusiveNameAccessScope scope_name(Impl::_tls_name_of_node_exclusive_depth);

    // Helper: assign/replace one name for one node in one language, while keeping
    // both maps consistent. Caller must have both exclusive locks.
    auto assign_name_locked =
        [&](Node node, const std::string& new_name, const std::string& target_lang)
    {
        auto [reverse_outer_it, inserted_reverse_outer] = _pImpl->_node_of_name.try_emplace(target_lang);
        auto [forward_outer_it, inserted_forward_outer] = _pImpl->_name_of_node.try_emplace(target_lang);
        (void)inserted_reverse_outer;
        (void)inserted_forward_outer;

        auto& reverse = reverse_outer_it->second;
        auto& forward = forward_outer_it->second;

        // If the node already has a different name in this language, remove the old reverse entry.
        auto old_forward_it = forward.find(node);
        if (old_forward_it != forward.end() && old_forward_it->second != new_name)
        {
            auto old_reverse_it = reverse.find(old_forward_it->second);
            if (old_reverse_it != reverse.end() && old_reverse_it->second == node)
            {
                reverse.erase(old_reverse_it);
            }
            forward.erase(old_forward_it);
        }

        std::string_view sv = _pImpl->_string_pool.intern(new_name);

        auto [reverse_it, inserted_reverse] = reverse.emplace(sv, node);
        if (!inserted_reverse)
        {
            reverse_it->second = node;
        }

        auto [forward_it, inserted_forward] = forward.emplace(node, sv);
        if (!inserted_forward)
        {
            forward_it->second = sv;
        }
    };

    // Helper: find existing current-language node or create one, but do it
    // under the locks already held (no recursive lock via node()).
    auto find_or_create_current_node_locked = [&]() -> Node
    {
        auto current_reverse_outer_it = _pImpl->_node_of_name.find(_lang);
        if (current_reverse_outer_it != _pImpl->_node_of_name.end())
        {
            auto it = current_reverse_outer_it->second.find(name_in_current_lang);
            if (it != current_reverse_outer_it->second.end())
            {
                return it->second;
            }
        }

        auto core_it = _core_names_by_name.find(name_in_current_lang);
        if (core_it != _core_names_by_name.end())
        {
            return core_it->second;
        }

        Node new_node = _pImpl->create();

        auto [reverse_outer_it, inserted_reverse_outer] = _pImpl->_node_of_name.try_emplace(_lang);
        auto [forward_outer_it, inserted_forward_outer] = _pImpl->_name_of_node.try_emplace(_lang);
        (void)inserted_reverse_outer;
        (void)inserted_forward_outer;

        std::string_view sv = _pImpl->_string_pool.intern(name_in_current_lang);
        reverse_outer_it->second.emplace(sv, new_node);
        forward_outer_it->second.emplace(new_node, sv);

        return new_node;
    };

    Node result_node = 0;

    // Re-check foreign mapping under exclusive lock
    auto foreign_reverse_outer_it = _pImpl->_node_of_name.find(lang);
    if (foreign_reverse_outer_it == _pImpl->_node_of_name.end())
    {
        result_node = find_or_create_current_node_locked();
        assign_name_locked(result_node, name_in_given_lang, lang);
        return result_node;
    }

    auto foreign_reverse_it = foreign_reverse_outer_it->second.find(name_in_given_lang);
    if (foreign_reverse_it == foreign_reverse_outer_it->second.end())
    {
        result_node = find_or_create_current_node_locked();
        assign_name_locked(result_node, name_in_given_lang, lang);
        return result_node;
    }

    // Foreign-language name already exists
    result_node = foreign_reverse_it->second;

    // Consistency check: reverse mapping (node -> name) must exist and match
    auto foreign_forward_outer_it = _pImpl->_name_of_node.find(lang);
    if (foreign_forward_outer_it == _pImpl->_name_of_node.end())
    {
        throw std::runtime_error("Zelph::set_name: Internal error – name mappings are inconsistent.");
    }

    auto foreign_forward_it = foreign_forward_outer_it->second.find(result_node);
    if (foreign_forward_it == foreign_forward_outer_it->second.end() || foreign_forward_it->second != name_in_given_lang)
    {
        throw std::runtime_error("Zelph::set_name: Internal error – name mappings are inconsistent.");
    }

    auto [current_reverse_outer_it, inserted_current_reverse_outer] = _pImpl->_node_of_name.try_emplace(_lang);
    auto [current_forward_outer_it, inserted_current_forward_outer] = _pImpl->_name_of_node.try_emplace(_lang);
    (void)inserted_current_reverse_outer;
    (void)inserted_current_forward_outer;

    auto& node_of_name_cur = current_reverse_outer_it->second;
    auto& name_of_node_cur = current_forward_outer_it->second;

    auto             current_forward_it = name_of_node_cur.find(result_node);
    std::string_view old_current_name =
        (current_forward_it != name_of_node_cur.end()) ? current_forward_it->second : std::string_view{};

    // Already correct? Then optionally repair missing reverse entry cheaply and return.
    if (old_current_name == name_in_current_lang)
    {
        auto current_reverse_it = node_of_name_cur.find(name_in_current_lang);
        if (current_reverse_it == node_of_name_cur.end() || current_reverse_it->second == result_node)
        {
            assign_name_locked(result_node, name_in_current_lang, _lang);
            return result_node;
        }
        // else: inconsistent reverse entry -> continue into normal conflict handling
    }

    // Remove old current-language mapping for result_node, if any
    if (!old_current_name.empty())
    {
        auto old_reverse_it = node_of_name_cur.find(old_current_name);
        if (old_reverse_it != node_of_name_cur.end() && old_reverse_it->second == result_node)
        {
            node_of_name_cur.erase(old_reverse_it);
        }

        name_of_node_cur.erase(result_node);
    }

    // Check whether the desired current-language name is already used by another regular node
    auto conflict_it = node_of_name_cur.find(name_in_current_lang);
    if (conflict_it != node_of_name_cur.end() && conflict_it->second != result_node)
    {
        Node conflicting_node = conflict_it->second;

        // Merge direction: higher / non-core into lower / core-preserving target as before
        Node from = result_node;
        Node into = conflicting_node;

        // Core nodes must never be merged away.
        if (!get_core_name(from).empty())
        {
            std::swap(from, into);
        }

        if (Impl::is_var(from) != Impl::is_var(into))
        {
            std::stringstream s;
            s << "Requested name '" << name_in_current_lang
              << "' is already used by node " << into
              << " in language '" << lang
              << "'. Merging the two nodes is impossible because one node is a variable, the other not.";
            throw std::runtime_error(s.str());
        }

        if (!Impl::is_var(from))
        {
            out_stream() << "Warning: (C) Merging Node " << from
                         << " into Node " << into
                         << " due to name conflict '" << name_in_current_lang
                         << "' in language '" << _lang << "'."
                         << std::endl;
        }

        invalidate_fact_structures_cache();

        _pImpl->merge(from, into);
        _pImpl->transfer_names_locked(from, into);

        result_node = into;
    }
    else if (conflict_it == node_of_name_cur.end())
    {
        // Not in regular map — but it might be a core node
        auto core_it = _core_names_by_name.find(name_in_current_lang);
        if (core_it != _core_names_by_name.end() && core_it->second != result_node)
        {
            Node core_node = core_it->second;

            if (Impl::is_var(result_node) != Impl::is_var(core_node))
            {
                std::stringstream s;
                s << "Requested name '" << name_in_current_lang
                  << "' is already used by core node " << core_node
                  << ". Merging is impossible because one node is a variable, the other not.";
                throw std::runtime_error(s.str());
            }

            if (!Impl::is_var(result_node))
            {
                out_stream() << "Warning: (D) Merging Node \"" << format(result_node)
                             << "\" into core Node \"" << format(core_node)
                             << "\" due to name conflict '" << name_in_current_lang
                             << "' in language '" << _lang << "'."
                             << std::endl;
            }

            invalidate_fact_structures_cache();

            _pImpl->merge(result_node, core_node);
            _pImpl->transfer_names_locked(result_node, core_node);

            result_node = core_node;
        }
    }

    // Finally assign the desired current-language name
    assign_name_locked(result_node, name_in_current_lang, _lang);

    return result_node;
}

std::string Zelph::get_name(const Node node, std::string lang, const bool fallback) const
{
    if (lang.empty()) lang = _lang;

    auto lookup = [&](const std::string& language) -> std::string_view
    {
        auto outer_it = _pImpl->_name_of_node.find(language);
        if (outer_it == _pImpl->_name_of_node.end())
        {
            return {};
        }

        auto it = outer_it->second.find(node);
        return (it != outer_it->second.end()) ? it->second : std::string_view{};
    };

    auto impl = [&]() -> std::string
    {
        if (std::string_view sv = lookup(lang); !sv.empty())
        {
            return std::string(sv);
        }

        if (fallback)
        {
            if (lang != "en")
            {
                if (std::string_view sv = lookup("en"); !sv.empty())
                {
                    return std::string(sv);
                }
            }

            if (lang != "zelph")
            {
                if (std::string_view sv = lookup("zelph"); !sv.empty())
                {
                    return std::string(sv);
                }
            }

            for (const auto& [language, map] : _pImpl->_name_of_node)
            {
                if (language == lang || language == "en" || language == "zelph")
                {
                    continue;
                }

                auto it = map.find(node);
                if (it != map.end())
                {
                    return std::string(it->second);
                }
            }
        }

        auto it = _core_names_by_node.find(node);
        return (it != _core_names_by_node.end()) ? it->second : "";
    };

    if (Impl::_tls_name_of_node_exclusive_depth > 0)
    {
        return impl();
    }

    std::shared_lock lock(_pImpl->_mtx_name_of_node);
    return impl();
}

// If in Wikidata mode (has_language("wikidata") && lang != "wikidata"), get_formatted_name prepends Wikidata IDs to names with " - " separator
// for nodes that have both a name in the requested language and a Wikidata ID. This allows Markdown::convert_to_md to parse
// and create appropriate links using the ID for the URL and the name for display text.
std::string Zelph::get_formatted_name(const Node node, const std::string& lang) const
{
    const bool is_wikidata_mode = has_language("wikidata") && lang != "wikidata";
    if (!is_wikidata_mode)
    {
        return get_name(node, lang, true);
    }

    std::string wikidata_name = get_name(node, "wikidata", false);

    std::string name;
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
            name = wikidata_name + " - " + name;
        }
        return name;
    }
}

bool Zelph::has_name(const Node node, const std::string& lang) const
{
    auto impl = [&]() -> bool
    {
        auto outer_it = _pImpl->_name_of_node.find(lang);
        if (outer_it == _pImpl->_name_of_node.end())
        {
            return false;
        }

        return outer_it->second.find(node) != outer_it->second.end();
    };

    if (Impl::_tls_name_of_node_exclusive_depth > 0)
    {
        return impl();
    }

    std::shared_lock lock(_pImpl->_mtx_name_of_node);
    return impl();
}

void Zelph::remove_name(Node node, std::string lang)
{
    if (lang.empty()) lang = _lang;

    std::unique_lock lock_node(_pImpl->_mtx_node_of_name);
    std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

    _pImpl->remove_name_locked(node, lang);
}

void Zelph::unset_name(Node node, std::string lang)
{
    if (lang.empty()) lang = _lang;

    std::unique_lock lock_node(_pImpl->_mtx_node_of_name);
    std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

    _pImpl->remove_name_locked(node, lang);
}

Node Zelph::get_node(const std::string& name, std::string lang) const
{
    if (lang.empty()) lang = _lang;

    std::shared_lock lock(_pImpl->_mtx_node_of_name);
    auto             lang_it = _pImpl->_node_of_name.find(lang);
    if (lang_it == _pImpl->_node_of_name.end())
    {
        return 0;
    }

    auto it = lang_it->second.find(name);
    return (it == lang_it->second.end()) ? 0 : it->second;
}

void Zelph::register_core_node(Node n, const std::string& name)
{
    _core_names_by_node[n]    = name;
    _core_names_by_name[name] = n;
}

Node Zelph::get_core_node(const std::string& name) const
{
    auto it = _core_names_by_name.find(name);
    return (it != _core_names_by_name.end()) ? it->second : 0;
}

std::string Zelph::get_core_name(Node n) const
{
    auto it = _core_names_by_node.find(n);
    return (it != _core_names_by_node.end()) ? it->second : "";
}

std::string Zelph::get_name_hex(Node node, bool prepend_num, int max_neighbors) const
{
    std::string name = get_name(node, _lang, true);

    if (name.empty())
    {
        if (Impl::is_var(node))
        {
            name = std::to_string(static_cast<int>(node));
        }
        else
        {
            std::string output;
            string::node_to_string(this, output, _lang, node, max_neighbors);
            name = output;
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
    std::string result;
    string::node_to_string(this, result, _lang, node);
    return result;
}

std::vector<std::string> Zelph::get_languages() const
{
    std::shared_lock lock(_pImpl->_mtx_node_of_name);

    std::vector<std::string> result;
    result.reserve(_pImpl->_node_of_name.size());

    for (const auto& [language, _] : _pImpl->_node_of_name)
    {
        result.push_back(language);
    }

    return result;
}

bool Zelph::has_language(const std::string& language) const
{
    if (Impl::_tls_node_of_name_exclusive_depth > 0)
    {
        return _pImpl->_node_of_name.find(language) != _pImpl->_node_of_name.end();
    }

    std::shared_lock lock(_pImpl->_mtx_node_of_name);
    return _pImpl->_node_of_name.find(language) != _pImpl->_node_of_name.end();
}

name_of_node_map Zelph::get_nodes_in_language(const std::string& lang) const
{
    std::shared_lock lock(_pImpl->_mtx_name_of_node);

    auto it = _pImpl->_name_of_node.find(lang);
    if (it == _pImpl->_name_of_node.end())
    {
        return name_of_node_map{};
    }

    return it->second;
}

std::vector<Node> Zelph::resolve_nodes_by_name(const std::string& name) const
{
    std::vector<Node> results;

    std::shared_lock lock(_pImpl->_mtx_node_of_name);

    auto lang_it = _pImpl->_node_of_name.find(lang());
    if (lang_it == _pImpl->_node_of_name.end())
    {
        return results;
    }

    auto it = lang_it->second.find(name);
    if (it != lang_it->second.end())
    {
        results.push_back(it->second);
    }

    return results;
}

size_t Zelph::get_name_of_node_size(const std::string& lang) const
{
    std::shared_lock lock(_pImpl->_mtx_name_of_node);
    auto             it = _pImpl->_name_of_node.find(lang);
    return (it != _pImpl->_name_of_node.end()) ? it->second.size() : 0;
}

size_t Zelph::get_node_of_name_size(const std::string& lang) const
{
    std::shared_lock lock(_pImpl->_mtx_node_of_name);
    auto             it = _pImpl->_node_of_name.find(lang);
    return (it != _pImpl->_node_of_name.end()) ? it->second.size() : 0;
}

size_t Zelph::language_count() const
{
    std::shared_lock lock(_pImpl->_mtx_node_of_name);
    return _pImpl->_node_of_name.size();
}
