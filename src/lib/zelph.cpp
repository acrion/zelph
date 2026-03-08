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
#include "mermaid.hpp"
#include "node_to_string.hpp"
#include "string_utils.hpp"
#include "zelph_impl.hpp"

#include <boost/algorithm/string.hpp>

#include <bitset>
#include <ranges>
#include <sstream>

using std::ranges::all_of;

using namespace zelph::network;

std::string Zelph::get_version()
{
    return "0.9.4";
}

Zelph::Zelph(const OutputHandler& output)
    : _pImpl{new Impl(output)}
    , core({_pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create()})
{
    fact(core.IsA, core.IsA, {core.RelationTypeCategory});
    fact(core.Unequal, core.IsA, {core.RelationTypeCategory});
    fact(core.Causes, core.IsA, {core.RelationTypeCategory});
    fact(core.Cons, core.IsA, {core.RelationTypeCategory});
    fact(core.PartOf, core.IsA, {core.RelationTypeCategory});
}

Zelph::~Zelph()
{
    delete _pImpl;
}

Node Zelph::var() const
{
    return _pImpl->var();
}

void Zelph::set_lang(const std::string& lang)
{
    if (lang != _lang)
    {
        _lang = lang;
    }
}

void Zelph::set_print(std::function<void(std::wstring, bool)> print) const
{
    _pImpl->_print = print;
}

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

void Zelph::cleanup_isolated(size_t& removed_count) const
{
    removed_count = 0;

    invalidate_fact_structures_cache();

    _pImpl->remove_isolated_nodes(removed_count);
}

size_t Zelph::cleanup_names() const
{
    return _pImpl->cleanup_dangling_names();
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

size_t Zelph::rule_count() const
{
    return get_rules().size();
}

Node Zelph::node(const std::wstring& name, std::string lang)
{
    if (lang.empty()) lang = _lang;
    if (name.empty())
    {
        throw std::invalid_argument("Zelph::node(): name cannot be empty");
    }

    // 1. Check existing regular nodes
    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    {
        auto& node_of_name = _pImpl->_node_of_name[lang];
        auto  it           = node_of_name.find(name);
        if (it != node_of_name.end())
        {
            return it->second;
        }
    }

    // 2. Check core nodes
    {
        auto it = _core_names.right.find(name);
        if (it != _core_names.right.end())
        {
            return it->second;
        }
    }

    // we do not call invalidate_fact_structures_cache() here, because creating a node is isolated from the network

    // 3. Create new node
    Node            new_node = _pImpl->create();
    std::lock_guard lock2(_pImpl->_mtx_name_of_node);

    _pImpl->_node_of_name[lang][name]     = new_node;
    _pImpl->_name_of_node[lang][new_node] = name;

    return new_node;
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

bool Zelph::exists(uint64_t nd) const
{
    return _pImpl->exists(nd);
}

bool Zelph::has_name(const Node node, const std::string& lang) const
{
    std::lock_guard lock(_pImpl->_mtx_name_of_node);

    auto& name_of_node = _pImpl->_name_of_node[lang];
    auto  it           = name_of_node.find(node);
    return it != name_of_node.end();
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

adjacency_set Zelph::get_sources(const Node relationType, const Node target, const bool exclude_vars) const
{
    adjacency_set sources;

    for (Node relation : _pImpl->get_right(target))
        if (_pImpl->get_right(relation).count(relationType) == 1)
            for (Node source : _pImpl->get_left(relation))
                if (source != target && (!exclude_vars || !Impl::is_var(source)))
                    sources.insert(source);

    return sources;
}

adjacency_set Zelph::filter(const adjacency_set& source, const Node target) const
{
    adjacency_set result;

    for (Node nd : source)
    {
        if (_pImpl->get_right(nd).count(target) == 1)
        {
            result.insert(nd);
        }
    }

    return result;
}

adjacency_set Zelph::filter(const Node fact, const Node relationType, const Node target) const
{
    adjacency_set source     = _pImpl->get_right(fact);
    adjacency_set left_nodes = _pImpl->get_left(fact);
    adjacency_set result;

    for (Node nd : source)
    {
        adjacency_set possible_relations = _pImpl->get_right(nd);
        for (Node relation : filter(possible_relations, relationType))
        {
            if (_pImpl->get_left(relation).count(target) == 1
                && left_nodes.count(nd) == 0) // exclude the subject of the fact, since it is connected bidirectional. If <subject relationType target> is true, the subject would be included in the result by mistake
            {
                result.insert(nd);
            }
        }
    }

    return result;
}

adjacency_set Zelph::filter(const adjacency_set& source, const std::function<bool(const Node nd)>& f)
{
    adjacency_set result;

    for (const Node nd : source)
    {
        if (f(nd)) result.insert(nd);
    }

    return result;
}

adjacency_set Zelph::get_left(const Node b) const
{
    return _pImpl->get_left(b);
}

adjacency_set Zelph::get_right(const Node b) const
{
    return _pImpl->get_right(b);
}

bool Zelph::has_left_edge(Node b, Node a) const
{
    return _pImpl->has_left_edge(b, a);
}

bool Zelph::has_right_edge(Node a, Node b) const
{
    return _pImpl->has_right_edge(a, b);
}

Node Zelph::create_hash(const adjacency_set& vec)
{
    return Network::create_hash(vec);
}

bool Zelph::is_hash(Node a)
{
    return Network::is_hash(a);
}

bool Zelph::is_var(Node a)
{
    return Network::is_var(a);
}

Answer Zelph::check_fact(const Node subject, const Node predicate, const adjacency_set& objects) const
{
    bool known = false;

    Node relation = Impl::create_hash(predicate, subject, objects);

    if (_pImpl->exists(relation))
    {
        const adjacency_set& connectedFromRelation = _pImpl->get_right(relation);
        const adjacency_set& connectedToRelation   = _pImpl->get_left(relation);

        known = connectedFromRelation.count(subject) == 1
             && connectedToRelation.count(subject) == 1 // subject must be connected from and to <--> relation node (i.e. bidirectional, to distinguish it from objects)
             && std::all_of(objects.begin(), objects.end(), [&](Node t)
                            { return connectedToRelation.count(t) != 0; }) // objects must all be connected to relation
             && std::all_of(objects.begin(), objects.end(), [&](Node t)
                            { return t == subject || connectedFromRelation.count(t) == 0; }); // no object must be connected from relation node

        if (!known
            && !Impl::is_var(subject)
            && !Impl::is_var(predicate)
            && std::all_of(objects.begin(), objects.end(), [&](const Node t)
                           { return Impl::is_var(t); })
            && !console::is_inside_node_to_wstring())
        {
            const bool relationConnectsToSubject         = connectedFromRelation.count(subject) == 1;
            const bool subjectConnectsToRelation         = connectedToRelation.count(subject) == 1;
            const bool allObjectsConnectToRelation       = std::all_of(objects.begin(), objects.end(), [&](Node t)
                                                                 { return connectedToRelation.count(t) != 0; });
            const bool noObjectsAreConnectedFromRelation = std::all_of(objects.begin(), objects.end(), [&](Node t)
                                                                       { return connectedFromRelation.count(t) == 1; });

            // inconsistent state => debug output TODO
            std::wstring output;
            console::node_to_wstring(this, output, _lang, relation, 3);
            error(output, true);

            console::gen_mermaid_html(this,
                                      relation,
                                      "debug.html",
                                      1,
                                      3,
                                      {},
                                      true,
                                      true,
                                      true);
            error(L"relationConnectsToSubject         == " + std::to_wstring(relationConnectsToSubject), true);
            error(L"subjectConnectsToRelation         == " + std::to_wstring(subjectConnectsToRelation), true);
            error(L"allObjectsConnectToRelation       == " + std::to_wstring(allObjectsConnectToRelation), true);
            error(L"noObjectsAreConnectedFromRelation == " + std::to_wstring(noObjectsAreConnectedFromRelation), true);

            FactComponents actual = extract_fact_components(relation);
            error(L"Hash collision detected for relation=" + std::to_wstring(relation), true);
            error(L"Expected inputs to create_hash:", true);
            error(L"  Subject:   " + std::to_wstring(subject) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(subject)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(subject).to_string()) + L")", true);
            error(L"  Predicate: " + std::to_wstring(predicate) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(predicate)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(predicate).to_string()) + L")", true);
            error(L"  Objects:", true);
            for (Node obj : objects)
            {
                error(L"    " + std::to_wstring(obj) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(obj)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(obj).to_string()) + L")", true);
            }

            error(L"Actual inputs in existing relation:", true);
            error(L"  Subject:   " + std::to_wstring(actual.subject) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(actual.subject)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(actual.subject).to_string()) + L")", true);
            error(L"  Predicate: " + std::to_wstring(actual.predicate) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(actual.predicate)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(actual.predicate).to_string()) + L")", true);
            error(L"  Objects:", true);
            for (Node obj : actual.objects)
            {
                error(L"    " + std::to_wstring(obj) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(obj)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(obj).to_string()) + L")", true);
            }

            static int hash_collision_count = 0;
            ++hash_collision_count;
            error(L"Hash collision count: " + std::to_wstring(hash_collision_count), true);

            assert(false);
        }
    }

    if (known)
    {
        return {_pImpl->probability(relation, predicate), relation};
    }
    else
    {
        return Answer(relation); // unknown
    }
}

Node Zelph::fact(const Node subject, const Node predicate, const adjacency_set& objects, const long double probability)
{
    const Answer answer = check_fact(subject, predicate, objects);

    if (answer.is_known())
    {
        if (answer.is_wrong() && probability > 0.5L)
        {
            throw std::runtime_error("fact(): this fact is known to be wrong");
        }
        else if (answer.is_correct() && probability < 0.5L)
        {
            throw std::runtime_error("fact(): this fact is known to be true");
        }
    }
    else
    {
        if (objects.count(predicate) == 1)
        {
            // 1 13 13
            // ~ is for example is for example <= (~  is opposite of  is for example), (is for example  ~  ->)
            throw std::runtime_error("fact(): facts with same relation type and object are not supported.");
        }

        if (predicate != core.IsA && (!Impl::is_hash(predicate) || Network::is_var(predicate))) // note that the initial constructor call fact(core.IsA, core.IsA, core.RelationTypeCategory) is executed as intended
        {
            fact(predicate, core.IsA, {core.RelationTypeCategory});
        }

        if (_pImpl->exists(answer.relation()))
        {
            // check_fact returns !answer.is_known() though answer.relation exists, which must not happen. Indicates corrupt database or hash collision.
            assert(false);
        }
        else
        {
            _pImpl->create(answer.relation());
        }

        invalidate_fact_structures_cache();
        _pImpl->connect(subject, answer.relation());
        _pImpl->connect(answer.relation(), subject);
        for (const Node t : objects)
        {
            if (t == subject)
            {
                if (objects.size() > 1)
                {
                    // We only allow relations with the same subject and object in the case of a single object. If there are several
                    // objects and one of them is identical to the subject, we wouldn't know that such an object exists.
                    // Real life examples from Wikidata:
                    // South Africa (Q258)  country (P17)  South Africa (Q258)
                    // or
                    // chemical substance  has part  chemical substance ⇐ (matter  has part  chemical substance), (chemical substance  is subclass of  matter)

                    const std::wstring name_subject_object = get_name(subject, _lang, true);
                    const std::wstring name_relationType   = get_name(predicate, _lang, true);

                    throw std::runtime_error("fact(): facts with same subject and object are only supported for facts with a single object: " + string::unicode::to_utf8(name_subject_object) + " " + string::unicode::to_utf8(name_relationType) + " " + string::unicode::to_utf8(name_subject_object));
                }
            }
            else
            {
                _pImpl->connect(t, answer.relation());
            }
        }
        _pImpl->connect(answer.relation(), predicate, probability);
    }

    return answer.relation();
}

/**
 * Builds a Lisp-style singly linked list from a vector of Node elements using cons cells.
 *
 * This implements exactly the classic Lisp representation:
 * (cons A (cons B (cons C nil)))
 *
 * Fundamental Lisp principle since McCarthy 1958: The entire list is represented solely
 * by the pointer to the outermost (first) cons cell. There is no additional list header
 * or wrapper node anywhere. This is why we can say "the outermost cons cell IS the list".
 *
 * Empty input returns core.Nil, which is the canonical empty list in Lisp.
 *
 * Crucial for identity: Repeated calls to sequence() with identical input vectors of Nodes
 * (or equivalently with identical strings via the other overload) will always return exactly
 * the same Node value. This is guaranteed because fact(subject, predicate, objects) computes
 * the Node via a reproducible hash based on the triple (subject, predicate, objects) and
 * returns the existing Node if one with that exact triple already exists; it never creates
 * duplicates. For the string-based overload, node(const std::wstring&) additionally ensures
 * that identical names map to the same Node before fact() is called.
 *
 * This structural identity is essential for rule-based arithmetic and consistent
 * reasoning in zelph, as it ensures that equivalent lists are literally the same object.
 */
Node Zelph::list(const std::vector<Node>& elements)
{
    if (elements.empty()) return core.Nil;

    // Build from right to left (Lisp-style cons list)
    // (cons A (cons B (cons C nil)))
    Node rest = core.Nil;

    for (const Node current_node : std::ranges::reverse_view(elements))
    {
        if (current_node == 0) continue;

        rest = fact(current_node, core.Cons, {rest});
    }

    return rest; // The outermost cons cell IS the list
}

/**
 * Builds a Lisp-style cons list from a vector of wide strings (typically single characters
 * or digits).
 *
 * Each wstring is first converted to a Node via node(element), then the general
 * Node-based sequence() overload is called. This centralizes the cons-building logic
 * and guarantees both overloads produce exactly the same Lisp-style structure.
 *
 * See the detailed explanation of structural identity in the Node-based overload above.
 *
 * Note that we could name the outermost cons cell like the concatenation of all element
 * node names using set_name(result, value, _lang, false). This would make some sense for
 * numbers, e.g. the elements "4" and "2" would give the list the name "42". Two nodes in
 * zelph can have the same name without any issues. We don't do this for several reasons:
 *  - It would only make sense for sequences that represent numbers.
 *  - It would raise several issues, e.g. what to do if a preloaded dataset like Wikidata
 *    includes that number as a named node already.
 *  - A natural distinction between digits and numbers already exists in this representation:
 *    the digit "4" is node("4"), while the number 4 is the cons cell fact(node("4"), Cons,
 *    {Nil}) — a structurally different node. Giving the cons cell the same name "4" would
 *    conflate two concepts that are better kept separate.
 */
Node Zelph::list(const std::vector<std::wstring>& elements)
{
    if (elements.empty()) return core.Nil;

    std::vector<Node> node_elements;
    node_elements.reserve(elements.size());

    for (const auto& element : elements)
    {
        node_elements.emplace_back(node(element));
    }

    return list(node_elements);
}

/**
 * Creates a set represented as a dedicated node in the knowledge graph.
 *
 * In classic Lisp there is no direct equivalent to an unordered set as a primitive data structure.
 * Lisp traditionally uses lists (cons cells) for collections, and sets are usually simulated
 * with lists while manually ensuring uniqueness (member, adjoin, etc.) or with hash-tables in Common Lisp.
 *
 * This implementation follows a graph-theoretic / triple-store approach that fits Zelph perfectly:
 * - A dedicated "set node" is created that represents the set as a whole (the super-node).
 * - Each element is linked to this set node via the core.PartOf predicate: (element PartOf set_node).
 * - This allows natural, rule-based queries such as "which nodes are PartOf this set?" or
 *   "create the union of all sets that contain X" directly in Zelph's reasoning engine.
 * - The representation is inherently unordered (no head/tail like cons lists) and supports
 *   easy extension for future rule-based arithmetic (union, intersection, cardinality etc.).
 *
 * Empty input returns core.Nil (consistent with sequence() and the canonical empty list/set in Lisp).
 */
Node Zelph::set(const std::unordered_set<Node>& elements)
{
    if (elements.empty()) return core.Nil;

    // Create the super-node representing the set itself
    Node set_node = _pImpl->create();

    for (const auto& current_node : elements)
    {
        // Link to the set container
        fact(current_node, core.PartOf, {set_node});
    }

    return set_node;
}

Node Zelph::parse_fact(Node rule, adjacency_set& deductions, Node parent) const
{
    deductions.clear();
    adjacency_set candidates;

    for (Node nd : _pImpl->get_left(rule))
    {
        // Check for bidirectional link (characteristic of Subject <-> Relation connection)
        if (_pImpl->get_left(nd).count(rule) == 1)
        {
            if (nd != parent)
            {
                candidates.insert(nd);
            }
        }
        else
        {
            if (nd != parent) deductions.insert(nd);
        }
    }

    if (candidates.empty()) return 0;
    if (candidates.size() == 1)
    {
        if (deductions.empty())
            deductions.insert(*candidates.begin()); // Self-referential: subject is its own object.
        return *candidates.begin();
    }

    // Conflict detected: Multiple nodes look like the subject.
    // This happens when a fact node is also the subject of other facts,
    // creating extra bidirectional links. For example, a cons cell <3>
    // that is also the subject of (<3> .. <4>) and (<3> ~ digit) will
    // have the relation nodes for those facts as additional candidates.
    //
    // Strategy: Filter out candidates that are themselves relation nodes
    // (i.e., nodes that represent other facts). A relation node always has
    // a recognized predicate (a RelationTypeCategory instance) in its
    // outgoing connections. We also filter the original structural cases.

    // --- Disambiguation ---
    // Multiple candidates look like the subject.  This happens when `rule`
    // is also the subject of other facts, creating extra bidirectional links.
    //
    // Strategy: identify and filter out "child-fact" candidates — hash nodes
    // whose only bidirectional neighbor (besides their own predicate) is `rule`
    // itself, meaning `rule` is THEIR subject, not the other way around.
    // This mirrors the proven logic in get_fact_structures().

    std::vector<Node> valid;
    valid.reserve(candidates.size());

    for (Node cand : candidates)
    {
        bool is_child_fact = false;

        // A candidate is a child-fact if 'rule' is its only subject.
        // Rule variables act as hash nodes but are primitive subjects, so exclude them from check.
        if (Impl::is_hash(cand) && !Impl::is_var(cand))
        {
            Node cand_pred = parse_relation(cand);
            if (cand_pred != 0)
            {
                adjacency_set cand_right = _pImpl->get_right(cand);
                adjacency_set cand_left  = _pImpl->get_left(cand);

                // `rule` must be bidirectional with `cand` for a child-fact relationship
                if (cand_right.count(rule) > 0 && cand_left.count(rule) > 0)
                {
                    // Check whether `cand` has another bidirectional neighbor
                    // besides `rule` and `cand_pred`.  If not, `rule` is cand's
                    // only subject candidate → cand is a child-fact of `rule`.
                    bool has_alternative_subject = false;
                    for (Node x : cand_right)
                    {
                        if (x == rule || x == cand_pred) continue;
                        if (cand_left.count(x) > 0)
                        {
                            // x is bidirectional with cand.
                            // If x is a hash node (and not a var) with different predicate,
                            // check if it is just a grandchild.
                            if (Impl::is_hash(x) && !Impl::is_var(x))
                            {
                                Node x_pred = parse_relation(x);
                                if (x_pred != 0 && x_pred != cand_pred)
                                {
                                    // x has a different predicate — check if its
                                    // only bidi neighbor (besides its own pred) is cand.
                                    adjacency_set x_right            = _pImpl->get_right(x);
                                    adjacency_set x_left             = _pImpl->get_left(x);
                                    bool          x_is_child_of_cand = true;
                                    for (Node y : x_right)
                                    {
                                        if (y == cand || y == x_pred) continue;
                                        if (x_left.count(y) > 0)
                                        {
                                            x_is_child_of_cand = false;
                                            break;
                                        }
                                    }
                                    if (x_is_child_of_cand) continue; // x is grandchild, not alt subject
                                }
                            }
                            has_alternative_subject = true;
                            break;
                        }
                    }
                    if (!has_alternative_subject)
                    {
                        is_child_fact = true;
                    }
                }
            }
        }

        if (!is_child_fact)
        {
            valid.push_back(cand);
        }
    }

    if (valid.size() == 1) return valid[0];
    if (valid.empty()) return 0;

    // Heuristic Preferences if still ambiguous

    // 1) Prefer Variable (Rule Pattern)
    Node var_pick = 0;
    for (Node cand : valid)
    {
        if (Impl::is_var(cand))
        {
            if (var_pick != 0)
            {
                var_pick = 0;
                break;
            }
            var_pick = cand;
        }
    }
    if (var_pick != 0) return var_pick;

    // 2) Prefer Atomic (Non-Hash)
    Node atom_pick = 0;
    for (Node cand : valid)
    {
        if (!Impl::is_hash(cand))
        {
            if (atom_pick != 0)
            {
                atom_pick = 0;
                break;
            }
            atom_pick = cand;
        }
    }
    if (atom_pick != 0) return atom_pick;

    // 3) Prefer Cons Cell (List/Number)
    Node cons_pick = 0;
    for (Node cand : valid)
    {
        if (Impl::is_hash(cand) && parse_relation(cand) == core.Cons)
        {
            if (cons_pick != 0)
            {
                cons_pick = 0;
                break;
            }
            cons_pick = cand;
        }
    }
    if (cons_pick != 0) return cons_pick;

    return 0; // Still ambiguous
}

Node Zelph::parse_relation(const Node rule) const
{
    Node relation = 0; // 0 means failure
    Node subject  = 0;
    for (Node nd : _pImpl->get_right(rule))
    {
        if (check_fact(nd, core.IsA, {core.RelationTypeCategory}).is_correct())
        {
            if (_pImpl->get_right(nd).count(rule) == 1) // In case nd is the subject of the rule, it may be also a relation, but not the one of the current rule. So exclude it by checking for bidirectional connection.
                subject = nd;                           // The rule has a subject that is a relation. We don't know yet if it is a rule that has same subject and predicate.
            else if (relation)
                return 0; // there may be only 1 relation
            else
                relation = nd;
        }
    }

    if (relation == 0)
    {
        // Since we exclude setting relation to the subject of the rule, now that we have a rule without a relation, it must be a rule where subject and relation are identical.
        relation = subject;
    }

    return relation;
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

// #define DEBUG_FORMAT_FACT

std::string Zelph::format(Node node) const
{
    std::wstring result;
    console::node_to_wstring(this, result, _lang, node);
    return string::unicode::to_utf8(result);
}

Node Zelph::count() const
{
    return _pImpl->count();
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

// Returns all nodes that are subjects of a core.Causes relation
adjacency_set Zelph::get_rules() const
{
    const adjacency_set& rule_candidates = _pImpl->get_left(core.Causes);

    adjacency_set rules;

    for (Node rule_candidate : rule_candidates)
    {
        // We filter the rule candidates in the same way as Reasoning::apply_rule() does it.
        // Note that a rule candidate with empty deductions is interpreted as a question, see Reasoning::evaluate()
        if (rule_candidate)
        {
            adjacency_set deductions;
            Node          condition = parse_fact(rule_candidate, deductions);
            if (condition && condition != core.Causes && !deductions.empty())
            {
                rules.insert(rule_candidate);
            }
        }
    }

    return rules;
}

void Zelph::remove_rules() const
{
    adjacency_set rules = get_rules();
    for (Node rule : rules)
    {
        invalidate_fact_structures_cache();

        _pImpl->remove(rule);
        // Clean up names
        for (auto& lang_map : _pImpl->_name_of_node)
        {
            lang_map.second.erase(rule);
        }
        for (auto& lang_map : _pImpl->_node_of_name)
        {
            for (auto it = lang_map.second.begin(); it != lang_map.second.end();)
            {
                if (it->second == rule)
                {
                    it = lang_map.second.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
}

void Zelph::remove_node(Node node) const
{
    if (!_pImpl->exists(node))
    {
        throw std::runtime_error("Cannot remove non-existent node " + std::to_string(node));
    }

    invalidate_fact_structures_cache();

    _pImpl->remove(node);            // Disconnects edges and removes from adjacency maps
    _pImpl->remove_node_names(node); // Separate method for name cleanup
}

Zelph::AllNodeView Zelph::get_all_nodes_view() const
{
    return AllNodeView(_pImpl->_left);
}

Zelph::LangNodeView Zelph::get_lang_nodes_view(const std::string& lang) const
{
    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    auto            it = _pImpl->_node_of_name.find(lang);
    if (it == _pImpl->_node_of_name.end())
    {
        static const Impl::node_of_name_map empty;
        return LangNodeView(empty);
    }
    return LangNodeView(it->second);
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

bool Zelph::try_get_fact_structures_cached(Node fact, std::vector<FactStructure>& out) const
{
    // If cache is currently empty/known-invalid, avoid locking
    if (!_pImpl->_fs_cache_has_entries.load(std::memory_order_acquire))
        return false;

    std::shared_lock lock(_pImpl->_fs_cache_mtx);
    auto             it = _pImpl->_fs_cache.find(fact);
    if (it == _pImpl->_fs_cache.end()) return false;

    out = it->second; // copy (function returns by value anyway)
    return true;
}

void Zelph::store_fact_structures_cached(Node fact, const std::vector<FactStructure>& value) const
{
    {
        std::unique_lock lock(_pImpl->_fs_cache_mtx);
        _pImpl->_fs_cache[fact] = value;
    }
    _pImpl->_fs_cache_has_entries.store(true, std::memory_order_release);
}

void Zelph::invalidate_fact_structures_cache() const noexcept
{
    // If cache already empty, do nothing (avoid lock)
    if (!_pImpl->_fs_cache_has_entries.exchange(false, std::memory_order_acq_rel))
        return;

    std::unique_lock lock(_pImpl->_fs_cache_mtx);
    _pImpl->_fs_cache.clear();
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

// Extracts the components (subject, predicate, objects) from a relation node.
Zelph::FactComponents Zelph::extract_fact_components(Node relation) const
{
    FactComponents components;
    auto           left  = get_left(relation);
    auto           right = get_right(relation);

    // Find subject: The node present in both left and right (bidirectional connection)
    for (Node candidate : right)
    {
        if (left.count(candidate) == 1)
        {
            components.subject = candidate;
            break;
        }
    }

    if (components.subject == 0)
    {
        // No subject found (possibly corrupted data)
        return components;
    }

    // Find predicate: In right, but not the subject
    for (Node candidate : right)
    {
        if (candidate != components.subject)
        {
            components.predicate = candidate;
            break;
        }
    }

    // Find objects: In left, but not the subject
    for (Node candidate : left)
    {
        if (candidate != components.subject)
        {
            components.objects.insert(candidate);
        }
    }

    return components;
}

void Zelph::set_output_handler(OutputHandler output) const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    _pImpl->_output = std::move(output);
}

void Zelph::emit(OutputChannel channel, const std::wstring& text, bool newline) const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    _pImpl->emit(channel, text, newline);
}

void Zelph::out(const std::wstring& msg, bool newline) const
{
    emit(OutputChannel::Out, msg, newline);
}

void Zelph::error(const std::wstring& msg, bool newline) const
{
    emit(OutputChannel::Error, msg, newline);
}

void Zelph::diagnostic(const std::wstring& msg, bool newline) const
{
    emit(OutputChannel::Diagnostic, msg, newline);
}

void Zelph::prompt(const std::wstring& msg, bool newline) const
{
    emit(OutputChannel::Prompt, msg, newline);
}

zelph::OutputStream Zelph::out_stream() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return OutputStream(_pImpl->_output, OutputChannel::Out, false);
}

zelph::OutputStream Zelph::diagnostic_stream() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return OutputStream(_pImpl->_output, OutputChannel::Diagnostic, false);
}

zelph::OutputStream Zelph::error_stream() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return OutputStream(_pImpl->_output, OutputChannel::Error, false);
}

zelph::OutputStream Zelph::prompt_stream() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return OutputStream(_pImpl->_output, OutputChannel::Prompt, false);
}

void Zelph::save_to_file(const std::string& filename) const
{
    _pImpl->saveToFile(filename);
}

void Zelph::load_from_file(const std::string& filename) const
{
    invalidate_fact_structures_cache();

    _pImpl->loadFromFile(filename);
}

void Zelph::set_logging(int max_depth) const
{
    _pImpl->_logging       = max_depth != 0;
    _pImpl->_max_log_depth = max_depth;
    out_stream() << (_pImpl->_logging ? "Logging enabled with max depth " : "Logging disabled. ") << max_depth << std::endl;
}

bool Zelph::should_log(int depth) const
{
    return _pImpl->_logging && depth <= _pImpl->_max_log_depth;
}

bool Zelph::logging_active() const
{
    return _pImpl->_logging;
}

void Zelph::log(int depth, const std::string& category, const std::string& message) const
{
    if (!should_log(depth)) return;
    std::string indent(depth * 2, ' ');
    out_stream() << indent << "[depth " << depth << ", " << category << "] " << message << std::endl;
}
