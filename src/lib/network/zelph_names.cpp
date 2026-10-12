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
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    thread_local unsigned node_of_name_exclusive_depth = 0;
    thread_local unsigned name_of_node_exclusive_depth = 0;

    struct ExclusiveNameAccessScope
    {
        explicit ExclusiveNameAccessScope(unsigned& depth)
            : _depth(depth)
        {
            ++_depth;
        }

        ~ExclusiveNameAccessScope()
        {
            --_depth;
        }

    private:
        unsigned& _depth;
    };

    // A collection that a rule's text holds forms a component of the rule
    // itself. When merged with a data node, it would pull the node's facts
    // into the rule's text, or turn the rule's own collection into a value
    // along with the rule; when merged with another rule's collection, it
    // would integrate the texts of both rules. Neither is what giving two
    // nodes one name means, hence the merge is refused, just as it is when
    // attempting to merge a variable with a constant.
    std::string template_merge_refusal(const std::string& name, const zelph::network::Node holder, const std::string& lang, const zelph::network::Node rule_collection)
    {
        std::stringstream s;
        s << "Requested name '" << name << "' is already used by node " << holder
          << " in language '" << lang
          << "'. Merging the two nodes is impossible because " << rule_collection
          << " is a collection of a rule's text.";
        return s.str();
    }

    // A core node's spelling ("~", "in", "=>", ...) is not a name within any
    // single language: the parser reads it as the core node across all
    // languages unless the language gives it to a node of its own. Handing it
    // to another node merged that node INTO the core node, as core nodes
    // never disappear, and the merged node's names then re-spelled the core
    // node: following ".name prep3 en ~" every `~` fact printed as `prep3`.
    std::string core_name_refusal(const std::string& name, const zelph::network::Node core_node)
    {
        std::stringstream s;
        s << "Requested name '" << name << "' is the spelling of core node " << core_node
          << ", which it denotes in every language. A core node's name cannot be given away.";
        return s.str();
    }

    // It is impossible for two core nodes to merge into a single one:
    // whichever of them vanished, the engine would lose a node it relies on.
    std::string core_merge_refusal(const std::string& name, const zelph::network::Node holder, const std::string& lang)
    {
        std::stringstream s;
        s << "Requested name '" << name << "' is already used by node " << holder
          << " in language '" << lang
          << "'. Merging the two nodes is impossible because both are core nodes, and a core node never disappears.";
        return s.str();
    }

    // Using `.name`, two variables merge exclusively across distinct languages:
    // it refuses them within a single language. A variable belongs to the
    // statement that quantifies it, hence the merge produces a single
    // variable common to two statements -- a rule written in one language and
    // another rule expressed in a different language, both then print the same
    // variable. The merge is the action that the cross-language `.name` performs
    // for any pair of nodes; this specifies its meaning within this context.
    std::string variable_merge_warning(const char* step, const zelph::network::Node from, const zelph::network::Node into, const std::string& name, const std::string& lang)
    {
        std::stringstream s;
        s << "Warning: (" << step << ") Merging variable node " << from << " into variable node " << into
          << " due to name conflict '" << name << "' in language '" << lang
          << "'. A variable belongs to the statement that quantifies it; the statements that used these two now share one variable.";
        return s.str();
    }
}

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
// The name-conflict decision, taken with the two name locks HELD: which node
// keeps the name and which one disappears into it. Reads only, so it can be
// re-derived cheaply -- which is what set_name does, once to plan the repair
// and once to carry it out.
//
// Returns false when there is no conflict. Throws when the two cannot be
// merged at all.
bool Zelph::resolve_name_conflict_locked(const Node         node,
                                         const std::string& name,
                                         const std::string& lang,
                                         Node&              from,
                                         Node&              into) const
{
    Node conflict_node = 0;

    // Check regular mapping first
    auto rev_outer_it = _pImpl->_node_of_name.find(lang);
    if (rev_outer_it != _pImpl->_node_of_name.end())
    {
        auto it = rev_outer_it->second.find(name);
        if (it != rev_outer_it->second.end())
        {
            conflict_node = it->second;
        }
    }

    // A spelling that the language does not give to a node of its own
    // remains the core node's, and it continues to stay the core node's
    // (core_name_refusal).
    if (conflict_node == 0)
    {
        const Node core_node = get_core_node(name);
        if (core_node != 0 && core_node != node) throw std::runtime_error(core_name_refusal(name, core_node));
    }

    if (conflict_node == 0 || conflict_node == node) return false;

    // Default behaviour: merge the conflicting node into the requested node
    from = conflict_node;
    into = node;

    // A core node that carries the name in this language keeps it, and
    // the node the name was requested for merges into it rather than the
    // other way around. Core nodes must never be merged away, hence it is
    // impossible for two such nodes to merge under any circumstances.
    if (!get_core_name(from).empty())
    {
        if (!get_core_name(into).empty()) throw std::runtime_error(core_merge_refusal(name, conflict_node, lang));
        std::swap(from, into);
    }

    if (Impl::is_var(from) != Impl::is_var(into))
    {
        std::stringstream s;
        s << "Requested name '" << name << "' is already used by node "
          << conflict_node
          << " in language '" << lang
          << "'. Merging the two nodes is impossible because one node is a variable, the other not.";
        throw std::runtime_error(s.str());
    }

    if (is_rule_template(from) || is_rule_template(into))
    {
        throw std::runtime_error(template_merge_refusal(name, conflict_node, lang, is_rule_template(from) ? from : into));
    }

    return true;
}

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

    // A merge runs in four steps because two of them must NOT hold the name
    // locks. Reading what a dependent node is built from, and re-creating it
    // under its new id, both go through the fact readers -- whose logging
    // renders nodes, and rendering takes exactly the locks the merge needs.
    // So: decide under the locks, read the recipes without them, merge under
    // them again, repair without them.
    Node merge_from = 0;
    Node merge_into = 0;

    if (merge_on_conflict)
    {
        std::unique_lock lock_node(_pImpl->_mtx_node_of_name);
        std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

        ExclusiveNameAccessScope scope_node(node_of_name_exclusive_depth);
        ExclusiveNameAccessScope scope_name(name_of_node_exclusive_depth);

        resolve_name_conflict_locked(node, name, lang, merge_from, merge_into);
    }

    // Read WHILE THE HASHES STILL HOLD: after the merge the components no
    // longer hash back to the node, and the verifying reader refuses to
    // decompose it.
    std::vector<std::pair<Node, HashRecipe>> dependents;
    if (merge_from != 0) dependents = collect_hash_dependents(merge_from);

    {
        std::unique_lock lock_node(_pImpl->_mtx_node_of_name);
        std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

        ExclusiveNameAccessScope scope_node(node_of_name_exclusive_depth);
        ExclusiveNameAccessScope scope_name(name_of_node_exclusive_depth);

        Node target_node = node;

        if (merge_from != 0)
        {
            // The decision is re-derived rather than assumed. Nothing renames
            // concurrently in this engine, and this is what keeps that a
            // checked assumption instead of an unwritten one.
            Node from = 0;
            Node into = 0;
            if (!resolve_name_conflict_locked(node, name, lang, from, into)
                || from != merge_from || into != merge_into)
                throw std::runtime_error("Zelph::set_name: the name conflict changed while it was being repaired");

            if (!Impl::is_var(from))
            {
                out_stream() << "Warning: (B) Merging Node " << from
                             << " into Node " << into
                             << " due to name conflict '" << name
                             << "' in language '" << lang << "'."
                             << std::endl;
            }
            else
            {
                out_stream() << variable_merge_warning("B", from, into, name, lang) << std::endl;
            }

            invalidate_fact_structures_cache();

            _pImpl->merge(from, into);

            // In `lang`, the survivor is assigned `name` below, so a different
            // name that either node carries there is dropped first: the
            // transfer would otherwise report it as a conflict ("Keeping
            // existing name 'bob'" just before 'bob' was overwritten). The
            // vanishing node keeps `name` itself for the transfer, positioning
            // the survivor in the group of nodes sharing the label. Removed
            // ahead of time, the label would pass to the next carrier, and the
            // assignment below would take it away again.
            for (const Node n : {from, into})
            {
                const auto fwd_outer_it = _pImpl->_name_of_node.find(lang);
                if (fwd_outer_it == _pImpl->_name_of_node.end()) break;
                const auto fwd_it = fwd_outer_it->second.find(n);
                if (fwd_it != fwd_outer_it->second.end() && fwd_it->second != name) _pImpl->remove_name_locked(n, lang);
            }
            _pImpl->transfer_names_locked(from, into);

            target_node = into;
        }

        // Also correct in the !merge_on_conflict case:
        // - old name of target_node is removed
        // - previous owner of "name" loses that forward mapping
        _pImpl->assign_name_locked(target_node, name, lang);
    }

    // A node IS the hash of what it is built from, so everything built on the
    // node that just disappeared has to be re-created under the id its new
    // components give it.
    if (merge_from != 0) rehash_dependents(dependents, merge_from, merge_into);
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

    ExclusiveNameAccessScope scope_node(node_of_name_exclusive_depth);
    ExclusiveNameAccessScope scope_name(name_of_node_exclusive_depth);

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
            const std::string_view old_name = old_forward_it->second;
            forward.erase(old_forward_it);
            _pImpl->release_label_locked(target_lang, node, old_name);
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

    // A merge is rejected before any modification, including the names, if
    // a collection of a rule's text is involved, if the name matches the
    // spelling of a core node, or if both nodes are core nodes -- the
    // refusals of resolve_name_conflict_locked.
    {
        Node other         = 0;
        bool core_spelling = false;
        if (const auto it = node_of_name_cur.find(name_in_current_lang); it != node_of_name_cur.end())
            other = it->second;
        else if (const auto core = _core_names_by_name.find(name_in_current_lang); core != _core_names_by_name.end())
        {
            other         = core->second;
            core_spelling = true;
        }

        if (other != 0 && other != result_node)
        {
            if (core_spelling) throw std::runtime_error(core_name_refusal(name_in_current_lang, other));
            if (!get_core_name(other).empty() && !get_core_name(result_node).empty())
                throw std::runtime_error(core_merge_refusal(name_in_current_lang, other, _lang));
            if (is_rule_template(other) || is_rule_template(result_node))
                throw std::runtime_error(template_merge_refusal(name_in_current_lang, other, _lang, is_rule_template(result_node) ? result_node : other));
        }
    }

    // Remove old current-language mapping for result_node, if any
    if (!old_current_name.empty())
    {
        name_of_node_cur.erase(result_node);
        _pImpl->release_label_locked(_lang, result_node, old_current_name);
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
        else
        {
            out_stream() << variable_merge_warning("C", from, into, name_in_current_lang, _lang) << std::endl;
        }

        invalidate_fact_structures_cache();

        _pImpl->merge(from, into);
        _pImpl->transfer_names_locked(from, into);

        result_node = into;
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

        // The operator spelling of a core node ranks ABOVE a name from
        // another language, because it is what the parser accepts in every
        // language -- the output stays re-enterable. Once "!" carried an
        // English name, a query answer in the zelph language printed
        // "contradiction is unsatisfiable", and re-entering that line created
        // a new node named "contradiction" instead of addressing "!". A name
        // in the CURRENT language still wins: that mapping was asked for, and
        // it is re-enterable by construction.
        if (const auto core_it = _core_names_by_node.find(node); core_it != _core_names_by_node.end())
        {
            return core_it->second;
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

        return "";
    };

    if (name_of_node_exclusive_depth > 0)
    {
        return impl();
    }

    std::shared_lock lock(_pImpl->_mtx_name_of_node);
    return impl();
}

// If in Wikidata mode (has_language("wikidata") && lang != "wikidata"), get_formatted_name prepends Wikidata IDs to names with " - " separator
// for nodes that have both a name in the requested language and a Wikidata ID, so that a rendered leaf carries both.
// DerivationExport::identifier undoes the composition again to recover the node; a reader of the rendered text sees both.
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
        // In Wikidata mode, the output of get_formatted_name may end up in the derivation export (command `.run-export`).
        // In this case, we want to use a natural language as the primary language.
        // The "zelph" language is only intended to offer an agnostic language in addition to natural languages, not for reports.
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

bool Zelph::is_named_any(const Node node) const
{
    if (_core_names_by_node.count(node) != 0) return true;

    auto impl = [&]() -> bool
    {
        for (const auto& [lang, names] : _pImpl->_name_of_node)
        {
            if (names.find(node) != names.end()) return true;
        }
        return false;
    };

    // Within a name operation, which holds the map exclusively (refer
    // to ExclusiveNameAccessScope), taking the shared lock would result
    // in a deadlock.
    if (name_of_node_exclusive_depth > 0)
    {
        return impl();
    }

    std::shared_lock lock(_pImpl->_mtx_name_of_node);
    return impl();
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

    if (name_of_node_exclusive_depth > 0)
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

Node Zelph::get_node(const std::string& name, const std::string& lang, const bool atoms_only) const
{
    const std::string& language = lang.empty() ? _lang : lang;

    {
        std::shared_lock lock(_pImpl->_mtx_node_of_name);
        auto             lang_it = _pImpl->_node_of_name.find(language);
        if (lang_it != _pImpl->_node_of_name.end())
        {
            auto it = lang_it->second.find(name);
            if (it != lang_it->second.end() && !(atoms_only && Impl::is_var(it->second))) return it->second;
        }
    }

    // The operator spellings of the core nodes ("~", "=>", "!", "cons", ...)
    // are not name-map entries; the parser knows them, and so does node(),
    // which consults the same table after the language map. Without this the
    // non-creating lookup disagreed with the creating one: every command that
    // takes a node by name refused the very predicate the line above had just
    // used -- ".node ~" reported "No node found with name '~'". The order is
    // node()'s: a name given in the current language wins over a core
    // spelling.
    return get_core_node(name);
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
    return string::unmark_identifiers(result);
}

// A language counts as present when EITHER direction knows it. The two maps
// only ever agree in a complete network; a partial load addresses nameOfNode
// and nodeOfName as separate sections, so "nameOfNode=0 nodeOfName=none" used
// to leave .stat reporting "Languages: 0" over a million loaded names.
// Lock order is the one documented for the two maps: node_of_name first.
std::vector<std::string> Zelph::get_languages() const
{
    std::shared_lock lock_node(_pImpl->_mtx_node_of_name);
    std::shared_lock lock_name(_pImpl->_mtx_name_of_node);

    std::vector<std::string> result;
    result.reserve(_pImpl->_node_of_name.size());

    for (const auto& [language, _] : _pImpl->_node_of_name)
    {
        result.push_back(language);
    }

    for (const auto& [language, _] : _pImpl->_name_of_node)
    {
        if (std::find(result.begin(), result.end(), language) == result.end())
        {
            result.push_back(language);
        }
    }

    return result;
}

bool Zelph::has_language(const std::string& language) const
{
    if (node_of_name_exclusive_depth > 0)
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

bool Zelph::name_maps_consistent() const
{
    std::shared_lock lock_node(_pImpl->_mtx_node_of_name);
    std::shared_lock lock_name(_pImpl->_mtx_name_of_node);

    for (const auto& [lang, rev] : _pImpl->_node_of_name)
    {
        const auto fwd_it = _pImpl->_name_of_node.find(lang);
        for (const auto& [name, node] : rev)
        {
            if (fwd_it == _pImpl->_name_of_node.end()) return false;
            const auto it = fwd_it->second.find(node);
            if (it == fwd_it->second.end() || it->second != name) return false;
        }
    }
    return true;
}

size_t Zelph::language_count() const
{
    // Same rule as get_languages(), so that a count and the list below it in
    // .stat cannot contradict each other.
    return get_languages().size();
}

void Zelph::Impl::transfer_names_locked(const Node from, const Node into)
{
    if (from == into) return;

    // PRECONDITION:
    // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively,
    // always in this order: _mtx_node_of_name -> _mtx_name_of_node

    for (auto& [lang, forward] : _name_of_node)
    {
        auto it_from = forward.find(from);
        if (it_from == forward.end())
        {
            continue;
        }

        const std::string_view from_name = it_from->second;

        auto [reverse_outer_it, inserted_reverse_outer] = _node_of_name.try_emplace(lang);
        (void)inserted_reverse_outer;
        auto& reverse = reverse_outer_it->second;

        // Remove reverse entry for the disappearing pair (from_name -> from), if it exists.
        auto       rev_from_it = reverse.find(from_name);
        const bool from_held   = rev_from_it != reverse.end() && rev_from_it->second == from;
        if (from_held)
        {
            reverse.erase(rev_from_it);
        }

        std::vector<Node>* const carriers = label_carriers_locked(lang, from_name);

        auto it_into = forward.find(into);
        if (it_into != forward.end())
        {
            const std::string_view into_name = it_into->second;

            if (into_name != from_name)
            {
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Warning: Name conflict in language '" << lang << "': '"
                    << from_name << "' (from merged node) vs '" << into_name
                    << "'. Keeping existing name '" << into_name << "'.";
            }

            // Drop the old forward mapping for "from".
            forward.erase(it_from);

            // "from" no longer carries its name. A name it previously
            // held, which other nodes now possess, is transferred to the
            // first of them, unless "into" also bears it, in which case
            // "into" holds it as before.
            if (carriers != nullptr)
            {
                std::erase(*carriers, from);
                if (from_held && into_name != from_name && !carriers->empty()) reverse.emplace(from_name, first_carrier_locked(*carriers));
            }

            // Repair reverse mapping for the kept name.
            auto rev_into_it = reverse.find(into_name);
            if (rev_into_it == reverse.end())
            {
                reverse.emplace(into_name, into);
            }
            else if (rev_into_it->second == from)
            {
                rev_into_it->second = into;
            }

            if (carriers != nullptr) trim_label_locked(lang, from_name);
        }
        else
        {
            // Move forward mapping from -> into
            forward.erase(it_from);
            auto [new_into_it, inserted_into] = forward.emplace(into, from_name);
            if (!inserted_into)
            {
                new_into_it->second = from_name;
            }

            if (carriers != nullptr)
            {
                std::erase(*carriers, from);
                if (may_carry_label(into)) carriers->push_back(into);
            }

            // Move reverse mapping from_name -> into
            auto rev_it = reverse.find(from_name);
            if (rev_it == reverse.end())
            {
                reverse.emplace(from_name, into);
            }
            else if (rev_it->second == from)
            {
                rev_it->second = into;
            }
            else if (rev_it->second != into)
            {
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Warning: Skipping reverse mapping update for name '" << from_name
                    << "' in language '" << lang
                    << "' due to existing conflicting mapping.";
            }

            if (carriers != nullptr) trim_label_locked(lang, from_name);
        }
    }
}

void Zelph::Impl::transfer_names(const Node from, const Node into)
{
    if (from == into) return;

    // Global lock order must be consistent everywhere:
    // _mtx_node_of_name -> _mtx_name_of_node
    std::unique_lock lock_node(_mtx_node_of_name);
    std::unique_lock lock_name(_mtx_name_of_node);

    transfer_names_locked(from, into);
}

void Zelph::Impl::assign_name_locked(const Node node, const std::string& name, const std::string& lang)
{
    // PRECONDITION:
    // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively
    // in this order: _mtx_node_of_name -> _mtx_name_of_node

    auto [rev_outer_it, rev_outer_inserted] = _node_of_name.try_emplace(lang);
    auto [fwd_outer_it, fwd_outer_inserted] = _name_of_node.try_emplace(lang);
    (void)rev_outer_inserted;
    (void)fwd_outer_inserted;

    auto& rev = rev_outer_it->second; // name -> node
    auto& fwd = fwd_outer_it->second; // node -> name

    // 1. Remove old name of this node, if different
    auto fwd_it = fwd.find(node);
    if (fwd_it != fwd.end() && fwd_it->second != name)
    {
        const std::string_view old_name = fwd_it->second;
        fwd.erase(fwd_it);
        release_label_locked(lang, node, old_name);
    }

    // 2. Intern new name
    std::string_view sv = _string_pool.intern(name);

    // 3. If another node currently owns that name, decide who keeps
    //    what. A VARIABLE's name is purely cosmetic and
    //    statement-scoped -- parsing resolves variables through the
    //    scoped variable table, never through this map -- and every
    //    statement makes fresh ones, so many nodes legitimately
    //    carry the same variable name.
    //
    //    Both directions matter:
    //      - a new owner must not strip an EARLIER VARIABLE's
    //        display name, or every rule that used the same letter
    //        would start rendering as «??»;
    //      - a new VARIABLE must not take anything from a real node.
    //        A graph with a node named "A" -- a Wikidata label can
    //        be a single letter -- lost that name to the first query
    //        mentioning A, and the node afterwards rendered as
    //        "(?? ?? ??)": asking a question deleted data.
    //
    //    So a VARIABLE never takes this mapping over, from a real
    //    node or from an earlier variable. The variable case is not
    //    cosmetic either: the TRANSIENT variables a read-only
    //    command builds -- .explain evaluates its pattern in a
    //    scratch cluster and drops it again -- would otherwise take
    //    the name and carry it into the removal, leaving the name
    //    unresolvable while the surviving variables still display
    //    it. `.node A` answered before such a command and reported
    //    "No node found with name 'A'" afterwards, and the two maps
    //    disagreed from then on, in the session and in a .bin saved
    //    from it. Whichever node holds the mapping is arbitrary
    //    among same-named variables anyway; first wins is the
    //    reading that nothing transient can disturb.
    auto rev_it = rev.find(sv);
    if (rev_it != rev.end() && rev_it->second != node)
    {
        const Node previous_owner = rev_it->second;
        const bool takes_over     = !is_var(node);

        if (takes_over && !is_var(previous_owner))
        {
            auto prev_fwd_it = fwd.find(previous_owner);
            if (prev_fwd_it != fwd.end() && prev_fwd_it->second == sv)
            {
                fwd.erase(prev_fwd_it);
            }
        }

        if (takes_over) rev_it->second = node;

        // The remaining carriers of a shared name keep it; the previous
        // owner has lost it, and the new one now bears it.
        if (std::vector<Node>* const carriers = takes_over ? label_carriers_locked(lang, sv) : nullptr)
        {
            std::erase(*carriers, previous_owner);
            if (may_carry_label(node) && std::find(carriers->begin(), carriers->end(), node) == carriers->end()) carriers->push_back(node);
            trim_label_locked(lang, sv);
        }
    }
    else if (rev_it == rev.end())
    {
        rev.emplace(sv, node);
    }

    // 4. Write/repair forward mapping
    auto [new_fwd_it, inserted] = fwd.emplace(node, sv);
    if (!inserted)
    {
        new_fwd_it->second = sv;
    }
}

// Arranged such that the smaller value ranks first: the kind of entity
// encoded in the two top bits, the number of its identifier positioned
// beneath. Identifiers contain no more than 18 digits in this context, which
// is sufficient.
bool Zelph::Impl::label_precedes_locked(const Node a, const Node b) const
{
    // PRECONDITION:
    // caller holds _mtx_name_of_node (shared or exclusive)

    if (is_var(a) != is_var(b)) return is_var(b);

    const auto rank = [this](const Node n) -> uint64_t
    {
        constexpr uint64_t property = 0, item = 1, other = 2, unnamed = 3;

        const auto wd = _name_of_node.find("wikidata");
        if (wd == _name_of_node.end()) return unnamed << 62;
        const auto it = wd->second.find(n);
        if (it == wd->second.end()) return unnamed << 62;

        const std::string_view id = it->second;
        if (id.size() < 2 || id.size() > 19
            || !std::all_of(id.begin() + 1, id.end(), [](const char c)
                            { return c >= '0' && c <= '9'; }))
            return other << 62;

        uint64_t number = 0;
        for (const char c : id.substr(1))
            number = number * 10 + static_cast<uint64_t>(c - '0');

        const uint64_t kind = id[0] == 'P' ? property : id[0] == 'Q' ? item
                                                                     : other;
        return (kind << 62) | number;
    };

    const uint64_t rank_a = rank(a);
    const uint64_t rank_b = rank(b);
    return rank_a != rank_b ? rank_a < rank_b : a < b;
}

bool Zelph::label_precedes(const Node a, const Node b) const
{
    if (name_of_node_exclusive_depth > 0) return _pImpl->label_precedes_locked(a, b);

    std::shared_lock lock(_pImpl->_mtx_name_of_node);
    return _pImpl->label_precedes_locked(a, b);
}

// assign_name_locked hands a name to its most recent owner and strips the
// previous one, a mechanism that `.name` and every other caller rely on. An
// import cannot function in this manner: the label from whichever line a
// thread had last processed ended up attached to its item, and every prior
// item sharing that label lost its English name -- "man" was a Drosophila
// gene, and P31 and P279 possessed no English name whatsoever. Here, the node
// gets the label regardless of the name → node entry, no other node loses its
// own, and that entry goes to the carrier label_precedes ranks first, the
// same carrier irrespective of the order of the lines.
void Zelph::Impl::assign_label_ranked_locked(const Node node, const std::string& label, const std::string& lang)
{
    // PRECONDITION:
    // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively
    // in this order: _mtx_node_of_name -> _mtx_name_of_node

    auto& rev = _node_of_name[lang];
    auto& fwd = _name_of_node[lang];

    // The label previously carried by the node is replaced, as a rename
    // operation takes it.
    if (const auto it = fwd.find(node); it != fwd.end() && it->second != label)
    {
        const std::string_view old_label = it->second;
        fwd.erase(it);
        release_label_locked(lang, node, old_label);
    }

    const std::string_view sv = _string_pool.intern(label);
    fwd[node]                 = sv;

    const auto [rev_it, inserted] = rev.try_emplace(sv, node);
    if (inserted || rev_it->second == node) return;

    // A second carrier: the label enters the index alongside both, enabling
    // the discovery of the others upon removal of its holder. Not following a
    // partial load, which builds no index (see nodes_named).
    const Node holder = rev_it->second;
    if (!_names_loaded_partially.load(std::memory_order_relaxed))
    {
        std::vector<Node>& carriers = _label_index[lang][sv];
        for (const Node n : {holder, node})
        {
            if (may_carry_label(n) && std::find(carriers.begin(), carriers.end(), n) == carriers.end()) carriers.push_back(n);
        }
    }

    if (label_precedes_locked(node, holder)) rev_it->second = node;

    if (!_names_loaded_partially.load(std::memory_order_relaxed)) trim_label_locked(lang, sv);
}

void Zelph::assign_label_ranked(const Node node, const std::string& label, std::string lang)
{
    if (lang.empty()) lang = _lang;

    std::unique_lock lock_node(_pImpl->_mtx_node_of_name);
    std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

    ExclusiveNameAccessScope scope_node(node_of_name_exclusive_depth);
    ExclusiveNameAccessScope scope_name(name_of_node_exclusive_depth);

    _pImpl->assign_label_ranked_locked(node, label, lang);
}

// The importer's vocabulary, not the engine's: `.cleanup` keeps the node
// even when nothing references it, and a predicate slice takes it along.
const char* Zelph::wikidata_labels_marker_name()
{
    return "wikidata:labels-v2";
}

bool Zelph::Impl::may_carry_label(const Node n) const
{
    return !is_var(n) && (_core_names == nullptr || _core_names->count(n) == 0);
}

std::vector<Node>* Zelph::Impl::label_carriers_locked(const std::string& lang, const std::string_view label)
{
    if (_label_index.empty()) return nullptr;
    const auto lang_it = _label_index.find(lang);
    if (lang_it == _label_index.end()) return nullptr;
    const auto it = lang_it->second.find(label);
    return it == lang_it->second.end() ? nullptr : &it->second;
}

const std::vector<Node>* Zelph::Impl::label_carriers_locked(const std::string& lang, const std::string_view label) const
{
    return const_cast<Impl*>(this)->label_carriers_locked(lang, label);
}

Node Zelph::Impl::first_carrier_locked(const std::vector<Node>& carriers) const
{
    return *std::min_element(carriers.begin(), carriers.end(), [this](const Node a, const Node b)
                             { return label_precedes_locked(a, b); });
}

// A label departs from the index once no other node carries it any
// more: no carrier remains, or the sole one left is the node to which
// it resolves.
void Zelph::Impl::trim_label_locked(const std::string& lang, const std::string_view label)
{
    const auto lang_it = _label_index.find(lang);
    if (lang_it == _label_index.end()) return;
    const auto it = lang_it->second.find(label);
    if (it == lang_it->second.end()) return;

    const std::vector<Node>& carriers = it->second;
    if (carriers.size() > 1) return;
    if (carriers.size() == 1)
    {
        const auto rev_outer_it = _node_of_name.find(lang);
        if (rev_outer_it == _node_of_name.end()) return;
        const auto rev_it = rev_outer_it->second.find(label);
        if (rev_it == rev_outer_it->second.end() || rev_it->second != carriers.front()) return;
    }

    lang_it->second.erase(it);
    if (lang_it->second.empty()) _label_index.erase(lang_it);
}

// Once the `node` has relinquished its `label` in `lang` -- the node → name
// entry is already absent. If the `node` was previously holding the `label`,
// the name → node mapping shifts to the carrier that ranks first among the
// remaining ones, or vanishes if no others remain.
void Zelph::Impl::release_label_locked(const std::string& lang, const Node node, const std::string_view label)
{
    std::vector<Node>* const carriers = label_carriers_locked(lang, label);
    if (carriers != nullptr) std::erase(*carriers, node);

    if (const auto rev_outer_it = _node_of_name.find(lang); rev_outer_it != _node_of_name.end())
    {
        auto&      rev    = rev_outer_it->second;
        const auto rev_it = rev.find(label);
        if (rev_it != rev.end() && rev_it->second == node)
        {
            if (carriers != nullptr && !carriers->empty())
                rev_it->second = first_carrier_locked(*carriers);
            else
                rev.erase(rev_it);
        }
    }

    if (carriers != nullptr) trim_label_locked(lang, label);
}

// The release_label_locked batch form: each node `gone` names has already
// forfeited its node -> name entry, and every label undergoes filtering
// just once, regardless of how many of its carriers went.
void Zelph::Impl::settle_labels_locked(const std::string& lang, const std::vector<std::string_view>& labels, const std::function<bool(Node)>& gone)
{
    const auto rev_outer_it = _node_of_name.find(lang);

    for (const std::string_view label : labels)
    {
        std::vector<Node>* const carriers = label_carriers_locked(lang, label);
        if (carriers == nullptr) continue; // already settled
        std::erase_if(*carriers, gone);

        if (rev_outer_it != _node_of_name.end())
        {
            auto&      rev    = rev_outer_it->second;
            const auto rev_it = rev.find(label);
            if (rev_it == rev.end())
            {
                if (!carriers->empty()) rev.emplace(label, first_carrier_locked(*carriers));
            }
            else if (gone(rev_it->second))
            {
                if (!carriers->empty())
                    rev_it->second = first_carrier_locked(*carriers);
                else
                    rev.erase(rev_it);
            }
        }

        trim_label_locked(lang, label);
    }
}

// A single traversal through the node → name entries of a language where the
// two maps differ in size: given the invariant that the reverse map
// preserves, the discrepancy amounts precisely to the entries no name → node
// entry points back at, and a language lacking any costs nothing. A language
// whose maps differ solely in the display names of variables costs one pass
// that finds nothing.
//
// A label with carriers but no node it resolves to -- a slice that left the
// holder behind, a network pruned by an engine that failed to re-elect -- or
// whose name → node entry corresponds to a variable's display name gets the
// carrier that ranks first, just as the removal of the holder would have
// granted, and just as an atom takes a name from a variable.
//
// Returns the count of labels that received a holder in this way.
size_t Zelph::Impl::rebuild_label_index_locked()
{
    _label_index.clear();
    if (_names_loaded_partially.load(std::memory_order_relaxed)) return 0;

    size_t elected = 0;

    for (const auto& [lang, fwd] : _name_of_node)
    {
        const auto   rev_outer_it = _node_of_name.find(lang);
        const size_t rev_size     = rev_outer_it == _node_of_name.end() ? 0 : rev_outer_it->second.size();
        if (fwd.size() == rev_size) continue;

        label_carriers carriers;
        for (const auto& [node, label] : fwd)
        {
            if (!may_carry_label(node)) continue;
            if (rev_outer_it != _node_of_name.end())
            {
                const auto rev_it = rev_outer_it->second.find(label);
                if (rev_it != rev_outer_it->second.end() && rev_it->second == node) continue;
            }
            carriers[label].push_back(node);
        }
        if (carriers.empty()) continue;

        auto& rev = _node_of_name[lang];
        for (auto it = carriers.begin(); it != carriers.end();)
        {
            const std::string_view label  = it->first;
            std::vector<Node>&     nodes  = it->second;
            const auto             rev_it = rev.find(label);

            Node holder = 0;
            if (rev_it != rev.end() && !is_var(rev_it->second))
            {
                holder = rev_it->second;
                if (may_carry_label(holder)) nodes.push_back(holder);
            }
            else
            {
                holder = first_carrier_locked(nodes);
                if (rev_it != rev.end())
                    rev_it->second = holder;
                else
                    rev.emplace(label, holder);
                ++elected;
            }

            if (nodes.size() == 1 && nodes.front() == holder)
            {
                it = carriers.erase(it);
            }
            else
            {
                nodes.shrink_to_fit();
                ++it;
            }
        }

        if (!carriers.empty()) _label_index.emplace(lang, std::move(carriers));
    }
    return elected;
}

// A holder elected here is one the file does not store, meaning the
// loaded network differs from the one the file holds. This engine writes
// each holder, in a slice as well (saveToFile), and the note says when a
// file from another source did not.
void Zelph::Impl::rebuild_label_index()
{
    size_t elected = 0;
    {
        std::unique_lock lock_node(_mtx_node_of_name);
        std::unique_lock lock_name(_mtx_name_of_node);

        ExclusiveNameAccessScope scope_node(node_of_name_exclusive_depth);
        ExclusiveNameAccessScope scope_name(name_of_node_exclusive_depth);

        elected = rebuild_label_index_locked();
    }

    if (elected == 0) return;
    io::OutputStream(_output, io::OutputChannel::Out, true)
        << "Note: " << elected << (elected == 1 ? " label in this file is carried by nodes but resolves" : " labels in this file are carried by nodes but resolve")
        << " to none of them, as in a slice or a pruned network from an engine that did not hand a removed holder's labels on. "
        << (elected == 1 ? "It now resolves" : "Each now resolves")
        << " to the carrier ranked first (see .help .load); the file does not record that, a .save of the network does.";
}

std::optional<std::vector<Node>> Zelph::nodes_named(const std::string& name, std::string lang) const
{
    if (lang.empty()) lang = _lang;

    std::shared_lock lock_node(_pImpl->_mtx_node_of_name);
    std::shared_lock lock_name(_pImpl->_mtx_name_of_node);

    if (_pImpl->_names_loaded_partially.load(std::memory_order_relaxed)) return std::nullopt;

    if (const std::vector<Node>* const carriers = _pImpl->label_carriers_locked(lang, name))
    {
        std::vector<Node> sorted(*carriers);
        std::sort(sorted.begin(), sorted.end(), [this](const Node a, const Node b)
                  { return _pImpl->label_precedes_locked(a, b); });
        return sorted;
    }

    if (const auto rev_outer_it = _pImpl->_node_of_name.find(lang); rev_outer_it != _pImpl->_node_of_name.end())
    {
        if (const auto it = rev_outer_it->second.find(name); it != rev_outer_it->second.end() && _pImpl->may_carry_label(it->second))
            return std::vector<Node>{it->second};
    }
    return std::vector<Node>{};
}

bool Zelph::label_index_consistent() const
{
    std::shared_lock lock_node(_pImpl->_mtx_node_of_name);
    std::shared_lock lock_name(_pImpl->_mtx_name_of_node);

    if (_pImpl->_names_loaded_partially.load(std::memory_order_relaxed)) return _pImpl->_label_index.empty();

    size_t languages = 0;
    for (const auto& [lang, fwd] : _pImpl->_name_of_node)
    {
        const auto rev_outer_it = _pImpl->_node_of_name.find(lang);

        // The index that the build would produce, excluding its elections.
        Impl::label_carriers expected;
        for (const auto& [node, label] : fwd)
        {
            if (!_pImpl->may_carry_label(node)) continue;
            if (rev_outer_it != _pImpl->_node_of_name.end())
            {
                const auto rev_it = rev_outer_it->second.find(label);
                if (rev_it != rev_outer_it->second.end() && rev_it->second == node) continue;
            }
            expected[label].push_back(node);
        }

        for (auto& [label, nodes] : expected)
        {
            if (rev_outer_it == _pImpl->_node_of_name.end()) return false;
            const auto rev_it = rev_outer_it->second.find(label);
            if (rev_it == rev_outer_it->second.end()) return false;
            if (_pImpl->may_carry_label(rev_it->second)) nodes.push_back(rev_it->second);
            std::sort(nodes.begin(), nodes.end());
        }

        const auto   index_it = _pImpl->_label_index.find(lang);
        const size_t indexed  = index_it == _pImpl->_label_index.end() ? 0 : index_it->second.size();
        if (indexed != expected.size()) return false;
        if (indexed > 0) ++languages;

        for (const auto& [label, nodes] : expected)
        {
            const auto it = index_it->second.find(label);
            if (it == index_it->second.end()) return false;
            std::vector<Node> kept(it->second);
            std::sort(kept.begin(), kept.end());
            if (kept != nodes) return false;
        }
    }
    return languages == _pImpl->_label_index.size();
}

void Zelph::Impl::remove_name_locked(const Node node, const std::string& lang)
{
    // PRECONDITION:
    // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively
    // in this order: _mtx_node_of_name -> _mtx_name_of_node

    auto fwd_outer_it = _name_of_node.find(lang);
    if (fwd_outer_it == _name_of_node.end())
    {
        return;
    }

    auto& fwd    = fwd_outer_it->second;
    auto  fwd_it = fwd.find(node);
    if (fwd_it == fwd.end())
    {
        return;
    }

    const std::string_view old_name = fwd_it->second;
    fwd.erase(fwd_it);

    release_label_locked(lang, node, old_name);
}

size_t Zelph::Impl::cleanup_dangling_names()
{
    size_t removed_count = 0;

    ankerl::unordered_dense::set<Node> valid_nodes;
    {
        std::shared_lock<std::shared_mutex> lock(_smtx_left);
        valid_nodes.reserve(_left.size());
        for (const auto& pair : _left)
        {
            valid_nodes.insert(pair.first);
        }
    }

    const auto gone = [&valid_nodes](const Node n)
    { return valid_nodes.count(n) == 0; };

    std::unique_lock lock_node(_mtx_node_of_name);
    std::unique_lock lock_name(_mtx_name_of_node);

    // A label that other nodes carry remains intact when a node goes: its
    // name → node entry goes to the first carrier remaining, following both
    // walks.
    std::unordered_map<std::string, std::vector<std::string_view>> shared;

    for (auto& lang_pair : _name_of_node)
    {
        auto& map = lang_pair.second;
        for (auto it = map.begin(); it != map.end();)
        {
            if (gone(it->first))
            {
                if (label_carriers_locked(lang_pair.first, it->second) != nullptr) shared[lang_pair.first].push_back(it->second);
                it = map.erase(it);
                removed_count++;
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& lang_pair : _node_of_name)
    {
        auto& map = lang_pair.second;
        for (auto it = map.begin(); it != map.end();)
        {
            if (gone(it->second))
            {
                removed_count++;
                if (label_carriers_locked(lang_pair.first, it->first) != nullptr)
                {
                    shared[lang_pair.first].push_back(it->first);
                    ++it;
                }
                else
                {
                    it = map.erase(it);
                }
            }
            else
            {
                ++it;
            }
        }
    }

    for (const auto& [lang, labels] : shared)
        settle_labels_locked(lang, labels, gone);

    // Note: removed strings remain in _string_pool (append-only).

    return removed_count;
}

// The reverse entries of a node are discovered via its forward entries:
// each writer maintains the invariant "node_of_name[l][s] = n implies
// name_of_node[l][n] = s", meaning the names a node holds in the reverse
// map are limited to at most one name per language as provided by the
// forward map. Consequently, a removal requires a single lookup per
// language and walks nothing.
//
// Previously, it performed `_node_of_name` walks for entries matching the
// node, executing one such walk per node. Measured during the live run on
// 11 August 2026: pruning the full Wikidata dump achieved a rate of 1.11
// nodes per second and spent 97.7 % of its time within that walk, as the
// map contains approximately 204 million entries at that location;
// conducting one walk per batch rendered pruning feasible, and each cluster
// drop and every claim associated with a marked pattern still paid one walk
// per node.
//
// Following a partial load, the two sections might not align, and the walk
// remains unchanged: see _names_loaded_partially.
void Zelph::Impl::remove_names_of(const adjacency_set& dead)
{
    if (dead.empty()) return;

    std::unique_lock lock1(_mtx_node_of_name);
    std::unique_lock lock2(_mtx_name_of_node);

    if (_names_loaded_partially.load(std::memory_order_relaxed))
    {
        for (auto& lang_map : _name_of_node)
        {
            for (const Node nd : dead)
                lang_map.second.erase(nd);
        }

        _name_map_scans.fetch_add(1, std::memory_order_relaxed);

        for (auto& lang_map : _node_of_name)
        {
            auto& map = lang_map.second;
            for (auto it = map.begin(); it != map.end();)
            {
                if (dead.count(it->second) != 0)
                    it = map.erase(it);
                else
                    ++it;
            }
        }
    }
    else
    {
        for (auto& [lang, fwd] : _name_of_node)
        {
            const auto                    rev_outer_it = _node_of_name.find(lang);
            std::vector<std::string_view> shared;
            for (const Node nd : dead)
            {
                const auto it = fwd.find(nd);
                if (it == fwd.end()) continue;
                const std::string_view name = it->second;
                fwd.erase(it);

                // A label that other nodes carry becomes settled once
                // across the entire batch, as shown below.
                if (label_carriers_locked(lang, name) != nullptr)
                {
                    shared.push_back(name);
                    continue;
                }

                if (rev_outer_it == _node_of_name.end()) continue;
                const auto rev_it = rev_outer_it->second.find(name);
                if (rev_it != rev_outer_it->second.end() && rev_it->second == nd) rev_outer_it->second.erase(rev_it);
            }

            if (!shared.empty())
                settle_labels_locked(lang, shared, [&dead](const Node n)
                                     { return dead.count(n) != 0; });
        }
    }

    erase_empty_languages_locked();
}

void Zelph::Impl::remove_node_names(Node nd)
{
    std::unique_lock lock1(_mtx_node_of_name);
    std::unique_lock lock2(_mtx_name_of_node);

    if (_names_loaded_partially.load(std::memory_order_relaxed))
    {
        for (auto& lang_map : _name_of_node)
        {
            lang_map.second.erase(nd);
        }

        _name_map_scans.fetch_add(1, std::memory_order_relaxed);

        for (auto& lang_map : _node_of_name)
        {
            auto& map = lang_map.second;
            for (auto it = map.begin(); it != map.end();)
            {
                if (it->second == nd)
                {
                    it = map.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
    else
    {
        for (auto& [lang, fwd] : _name_of_node)
        {
            const auto it = fwd.find(nd);
            if (it == fwd.end()) continue;
            const std::string_view name = it->second;
            fwd.erase(it);
            release_label_locked(lang, nd, name);
        }
    }

    erase_empty_languages_locked();
}

// A language remains registered as long as at least one map knows it
// (get_languages), and a registered language changes more than a count:
// `wikidata` by itself triggers the display update for every node bearing
// an `en` name (get_formatted_name). Thus, a removal that empties both maps
// of a language removes the language as well.
void Zelph::Impl::erase_empty_languages_locked()
{
    std::vector<std::string> empty;
    for (const auto& [lang, fwd] : _name_of_node)
    {
        if (!fwd.empty()) continue;
        const auto rev_it = _node_of_name.find(lang);
        if (rev_it == _node_of_name.end() || rev_it->second.empty()) empty.push_back(lang);
    }
    for (const auto& [lang, rev] : _node_of_name)
    {
        if (rev.empty() && _name_of_node.find(lang) == _name_of_node.end()) empty.push_back(lang);
    }
    for (const std::string& lang : empty)
    {
        _name_of_node.erase(lang);
        _node_of_name.erase(lang);
    }
}
