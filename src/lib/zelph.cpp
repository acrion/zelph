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
#include "zelph_impl.hpp"

#include <boost/algorithm/string.hpp>

#include <bitset>
#include <fstream>
#include <iostream>
#include <ranges>
#include <sstream>

using std::ranges::all_of;

using namespace zelph::network;

std::string Zelph::get_version()
{
    return "0.9.4";
}

Zelph::Zelph(const std::function<void(const std::wstring&, const bool)>& print)
    : _pImpl{new Impl}
    , core({_pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create()})
    , _print(print)
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
    // std::wcout << L"Node " << node << L" has name '" << name << L"' (" << std::wstring(lang.begin(), lang.end()) << L")" << std::endl;
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
                std::wclog << L"Warning: Merging Node " << from
                           << L" into Node " << into
                           << L" due to name conflict '" << name
                           << L"' in language '" << string::unicode::from_utf8(lang) << L"'." << std::endl;
            }

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
                    std::wclog << L"Warning: Merging Node " << from
                               << L" into Node " << into
                               << L" due to name conflict '" << name_in_current_lang
                               << L"' in language '" << string::unicode::from_utf8(_lang) << L"'." << std::endl;
                }

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

        if (!_pImpl->_format_fact_level
            && !known
            && !Impl::is_var(subject)
            && !Impl::is_var(predicate)
            && std::all_of(objects.begin(), objects.end(), [&](const Node t)
                           { return Impl::is_var(t); }))
        {
            const bool relationConnectsToSubject         = connectedFromRelation.count(subject) == 1;
            const bool subjectConnectsToRelation         = connectedToRelation.count(subject) == 1;
            const bool allObjectsConnectToRelation       = std::all_of(objects.begin(), objects.end(), [&](Node t)
                                                                 { return connectedToRelation.count(t) != 0; });
            const bool noObjectsAreConnectedFromRelation = std::all_of(objects.begin(), objects.end(), [&](Node t)
                                                                       { return connectedFromRelation.count(t) == 1; });

            // inconsistent state => debug output TODO
            std::wstring output;
            format_fact(output, _lang, relation, 3);
            print(output, true);

            gen_mermaid_html(relation, "debug.html", 2, 3);
            print(L"relationConnectsToSubject         == " + std::to_wstring(relationConnectsToSubject), true);
            print(L"subjectConnectsToRelation         == " + std::to_wstring(subjectConnectsToRelation), true);
            print(L"allObjectsConnectToRelation       == " + std::to_wstring(allObjectsConnectToRelation), true);
            print(L"noObjectsAreConnectedFromRelation == " + std::to_wstring(noObjectsAreConnectedFromRelation), true);

            FactComponents actual = extract_fact_components(relation);
            print(L"Hash collision detected for relation=" + std::to_wstring(relation), true);
            print(L"Expected inputs to create_hash:", true);
            print(L"  Subject:   " + std::to_wstring(subject) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(subject)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(subject).to_string()) + L")", true);
            print(L"  Predicate: " + std::to_wstring(predicate) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(predicate)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(predicate).to_string()) + L")", true);
            print(L"  Objects:", true);
            for (Node obj : objects)
            {
                print(L"    " + std::to_wstring(obj) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(obj)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(obj).to_string()) + L")", true);
            }

            print(L"Actual inputs in existing relation:", true);
            print(L"  Subject:   " + std::to_wstring(actual.subject) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(actual.subject)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(actual.subject).to_string()) + L")", true);
            print(L"  Predicate: " + std::to_wstring(actual.predicate) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(actual.predicate)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(actual.predicate).to_string()) + L")", true);
            print(L"  Objects:", true);
            for (Node obj : actual.objects)
            {
                print(L"    " + std::to_wstring(obj) + L" (hex: 0x" + string::unicode::from_utf8(string::to_hex(obj)) + L", bin: " + string::unicode::from_utf8(std::bitset<64>(obj).to_string()) + L")", true);
            }

            static int hash_collision_count = 0;
            ++hash_collision_count;
            print(L"Hash collision count: " + std::to_wstring(hash_collision_count), true);

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

    Node best_candidate   = 0;
    int  valid_candidates = 0;

    // Phase 1: filter candidates whose outgoing edges include a core structural
    // predicate. This identifies interlopers added by set-membership (PartOf),
    // IsA tagging (conjunction/negation/RelTypeCategory), cons-cell structure
    // (Cons), rule creation (Causes), or inequality constraints (Unequal).
    for (Node cand : candidates)
    {
        bool is_structural = false;
        for (Node pred : _pImpl->get_right(cand))
        {
            if (pred == core.PartOf || pred == core.Cons || pred == core.IsA || pred == core.Causes || pred == core.Unequal)
            {
                is_structural = true;
                break;
            }
        }
        if (!is_structural)
        {
            best_candidate = cand;
            valid_candidates++;
        }
    }

    if (valid_candidates == 1) return best_candidate;
    if (valid_candidates == 0) return 0; // Still ambiguous (all filtered)

    // Phase 2 (fallback): if multiple candidates remain after phase 1, also
    // filter any candidate that is a fact node with a user-defined predicate.
    // This handles spurious bidirectional candidates created when `rule` was
    // used as the subject of another user-defined fact (e.g. a cons cell node
    // later used as the subject of an arithmetic relation).
    //
    // Note: this phase may leave the result ambiguous (0) when a compound
    // nested subject (e.g. the (A+B) in ((A+B)=C)) is simultaneously used as
    // the subject of a user-defined relation. That edge case degrades to `??`
    // rather than producing incorrect output.
    best_candidate   = 0;
    valid_candidates = 0;

    for (Node cand : candidates)
    {
        bool is_structural = false;
        for (Node pred : _pImpl->get_right(cand))
        {
            if (pred == core.PartOf || pred == core.Cons || pred == core.IsA || pred == core.Causes || pred == core.Unequal)
            {
                is_structural = true;
                break;
            }
        }
        if (!is_structural && parse_relation(cand) != 0)
            is_structural = true;

        if (!is_structural)
        {
            best_candidate = cand;
            valid_candidates++;
        }
    }

    // If filtering leaves exactly one candidate, that's our semantic subject.
    if (valid_candidates == 1) return best_candidate;

    return 0; // Still ambiguous after both phases
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

#ifndef NDEBUG
    #define DEBUG_FORMAT_FACT
#endif

void Zelph::format_fact(std::wstring& result, const std::string& lang, Node fact, const int max_objects, const Variables& variables, Node parent, std::shared_ptr<std::unordered_set<Node>> history) const
{
    // Formats a fact into a string representation.

    struct IncDec
    {
        explicit IncDec(int& n)
            : _n(n) { ++_n; }
        ~IncDec() { --_n; }
        int& _n;
    };

    IncDec incDec(_pImpl->_format_fact_level);
#ifdef DEBUG_FORMAT_FACT
    std::string indent(_pImpl->_format_fact_level * 2, ' ');
    std::clog << indent << "[DEBUG format_fact] ENTRY fact=" << fact << " parent=" << parent << std::endl;
#endif

    if (!history) history = std::make_shared<std::unordered_set<Node>>();

    // Helper to resolve variables
    auto resolve_var = [&](Node n) -> Node
    {
        int  limit = 0;
        Node curr  = n;
        while (Impl::is_var(curr) && limit++ < 100)
        {
            auto it = variables.find(curr);
            if (it == variables.end() || it->second == 0 || it->second == curr) break;
            curr = it->second;
        }
        return curr;
    };

    // 1. Variable Substitution
    Node resolved = resolve_var(fact);

    if (history->find(resolved) != history->end())
    {
#ifdef DEBUG_FORMAT_FACT
        std::clog << indent << "[DEBUG format_fact] HIT HISTORY for fact=" << resolved << " -> returning '?'" << std::endl;
#endif
        result = L"?";
        return;
    }

    // 2. Name Check
    // If the node has a direct name, use it.
    std::wstring name = get_formatted_name(resolved, lang);
    if (!name.empty())
    {
#ifdef DEBUG_FORMAT_FACT
        std::clog << indent << "[DEBUG format_fact] Found name '" << string::unicode::to_utf8(name) << "' for node " << resolved << std::endl;
#endif
        result = string::mark_identifier(name);
        return;
    }

    // 3. Cons List Detection (Sequence)
    // Check if 'resolved' is a cons cell (relation node whose predicate is Cons).
    // If so, walk the cons chain and format as < e1 e2 ... en >.
    if (_pImpl->exists(resolved))
    {
        Node rel_type = parse_relation(resolved);
        if (rel_type == core.Cons)
        {
#ifdef DEBUG_FORMAT_FACT
            std::clog << indent << "[DEBUG format_fact] DETECTED CONS LIST starting at " << resolved << std::endl;
#endif
            auto child_history = std::make_shared<std::unordered_set<Node>>(*history);
            child_history->insert(resolved);

            std::vector<Node>        list_elements;
            Node                     current = resolved;
            std::unordered_set<Node> visited_cells;

            while (current != 0 && current != core.Nil && _pImpl->exists(current))
            {
                if (visited_cells.count(current)) break; // cycle protection
                visited_cells.insert(current);

                if (parse_relation(current) != core.Cons) break; // not a cons cell

                adjacency_set objs;
                Node          car = parse_fact(current, objs, 0);
                if (car != 0) car = resolve_var(car);
                if (car != 0)
                    list_elements.push_back(car);

                // Get cdr (rest of list) — the single object of this cons cell
                Node cdr = core.Nil;
                for (Node o : objs)
                {
                    cdr = resolve_var(o);
                    break;
                }
                current = cdr;
            }

            if (!list_elements.empty())
            {
                // Check whether all elements are single-character named nodes (digit-like).
                bool all_single_char = std::all_of(list_elements.begin(), list_elements.end(), [&](Node e) -> bool
                                                   {
                        Node         eff = resolve_var(e);
                        std::wstring nm  = get_formatted_name(eff, lang);
                        return nm.length() == 1; });

                if (all_single_char)
                {
                    // Reverse for display: stored order is LSB-first (e.g. [3,2,1] for 123),
                    // conventional display is MSB-first. Omit spaces to match input syntax.
                    std::vector<Node> display_elements(list_elements.rbegin(), list_elements.rend());

                    result = L"<";
                    for (Node e : display_elements)
                    {
                        std::wstring elem_str;
                        format_fact(elem_str, lang, resolve_var(e), max_objects, variables, resolved, child_history);
                        result += boost::algorithm::trim_copy_if(elem_str, boost::algorithm::is_any_of(L"«»"));
                    }
                    result += L">";
                }
                else
                {
                    result     = L"<";
                    bool first = true;
                    for (Node e : list_elements)
                    {
                        if (!first) result += L" ";
                        std::wstring elem_str;
                        format_fact(elem_str, lang, e, max_objects, variables, resolved, child_history);

                        // Wrap in parentheses if it's a composite expression (not a simple name).
                        if (!elem_str.empty() && elem_str.find(L' ') != std::wstring::npos
                            && elem_str.front() != L'<' && elem_str.front() != L'{')
                        {
                            Node eff_e = resolve_var(e);
                            if (elem_str != get_formatted_name(eff_e, lang))
                                elem_str = L"(" + elem_str + L")";
                        }

                        result += elem_str;
                        first = false;
                    }
                    result += L">";
                }
                return;
            }
        }
    }

    // 4. Container Detection (Set)
    // Check if 'resolved' acts as a container (Object in a PartOf relation).
    std::unordered_set<Node> elements;

    if (_pImpl->exists(resolved))
    {
        for (Node rel : _pImpl->get_right(resolved))
        {
            if (rel == parent) continue;

            Node p = parse_relation(rel);
            if (p == core.PartOf)
            {
                adjacency_set objs;
                Node          s = parse_fact(rel, objs, 0);
                if (s != 0) s = resolve_var(s);

                if (s != 0 && objs.count(resolved) > 0)
                {
                    elements.insert(s);
                }
            }
        }
    }

    if (!elements.empty())
    {
#ifdef DEBUG_FORMAT_FACT
        std::clog << indent << "[DEBUG format_fact] DETECTED SET with " << elements.size() << " elements." << std::endl;
#endif
        auto child_history = std::make_shared<std::unordered_set<Node>>(*history);
        child_history->insert(resolved);

        std::vector<Node> sorted_elements(elements.begin(), elements.end());
        std::sort(sorted_elements.begin(), sorted_elements.end());

        result     = L"{";
        bool first = true;
        for (Node e : sorted_elements)
        {
            if (!first) result += L" ";
            std::wstring elem_str;
            format_fact(elem_str, lang, e, max_objects, variables, resolved, child_history);

            if (!elem_str.empty() && elem_str.find(L' ') != std::wstring::npos
                && elem_str.front() != L'<' && elem_str.front() != L'{')
            {
                Node eff_e = resolve_var(e);
                if (elem_str != get_formatted_name(eff_e, lang))
                {
                    elem_str = L"(" + elem_str + L")";
                }
            }

            result += elem_str;
            first = false;
        }
        result += L"}";

        return;
    }

    // 5. Proxy / Instance Detection
    // If a node is anonymous and not a container, check if it is an instance of a concept (IsA).
    // If so, display the concept instead of the structural fact "Node IsA Concept".
    if (_pImpl->exists(resolved))
    {
        for (Node rel : _pImpl->get_right(resolved))
        {
            if (parse_relation(rel) == core.IsA)
            {
                adjacency_set type_objs;
                Node          type_subj = parse_fact(rel, type_objs, 0);

                // Ensure 'resolved' is the subject (The Instance)
                if (type_subj == resolved && !type_objs.empty())
                {
                    Node concept_node = *type_objs.begin();

                    // Avoid self-reference loops
                    if (concept_node != resolved)
                    {
#ifdef DEBUG_FORMAT_FACT
                        std::clog << indent << "[DEBUG format_fact] Found IsA proxy to concept=" << concept_node << std::endl;
#endif
                        format_fact(result, lang, concept_node, max_objects, variables, parent, history);

                        if (!result.empty() && result != L"?")
                        {
#ifdef DEBUG_FORMAT_FACT
                            std::clog << indent << "[DEBUG format_fact] Proxy resolved to: " << string::unicode::to_utf8(result) << std::endl;
#endif
                            return;
                        }
                    }
                }
            }
        }
    }

    // 6. Standard Fact Formatting (S P O)
    // Only if it wasn't a container or a simple proxy do we treat it as a structural fact.

#ifdef DEBUG_FORMAT_FACT
    std::clog << indent << "[DEBUG format_fact] Standard path (Statement/Fact)." << std::endl;
#endif

    adjacency_set objects;
    Node          subject = parse_fact(resolved, objects, parent);

    bool is_condition = false;

#ifdef DEBUG_FORMAT_FACT
    std::clog << indent << "[DEBUG format_fact] parse_fact result: subject=" << subject << ", objects_count=" << objects.size() << ", is_condition=" << is_condition << std::endl;
#endif

    if (subject == 0 && !is_condition)
    {
#ifdef DEBUG_FORMAT_FACT
        std::clog << indent << "[DEBUG format_fact] INVALID: Subject is 0 and not condition. Returning '??'" << std::endl;
#endif
        result = string::mark_identifier(L"??");
        return;
    }

    auto child_history = std::make_shared<std::unordered_set<Node>>(*history);
    child_history->insert(resolved);

    std::wstring subject_name, relation_name;

    if (!is_condition || subject)
    {
        // Recursion for Subject
        std::wstring s_str;
        format_fact(s_str, lang, subject, max_objects, variables, resolved, child_history);

        // Wrap subject only if it's a composite fact, not a named atom
        bool needs_parens = false;
        if (!s_str.empty() && s_str.find(L' ') != std::wstring::npos && s_str.front() != L'<' && s_str.front() != L'{')
        {
            Node         eff_subj = resolve_var(subject);
            std::wstring raw_name = get_formatted_name(eff_subj, lang);
            // Compare the formatted string with the MARKED raw name
            if (s_str != string::mark_identifier(raw_name))
            {
                needs_parens = true;
            }
        }

        if (needs_parens)
            subject_name = L"(" + s_str + L")";
        else
            subject_name = s_str.empty() ? (is_condition ? L"" : string::mark_identifier(L"?")) : s_str;

        Node relation = parse_relation(resolved);
        // Recursion for Relation (usually just get name, but handle complex relations)
        // Here we can assume relations are mostly named or simple, preventing deep noise
        relation = resolve_var(relation);

        std::wstring raw_rel_name = get_formatted_name(relation, lang);
        if (!raw_rel_name.empty())
        {
            // Relation has a name -> mark it manually, as we didn't recurse
            relation_name = string::mark_identifier(raw_rel_name);
        }
        else
        {
            // Recurse -> returns marked string
            std::wstring r_str;
            format_fact(r_str, lang, relation, max_objects, variables, resolved, child_history);
            relation_name = r_str.empty() ? string::mark_identifier(L"?") : r_str;

            // Optional: Wrap complex unnamed relations in parens too?
            // For consistency with subject/object, we usually assume relations are simple,
            // but if r_str has spaces and isn't a container, wrap it.
            if (relation_name.find(L' ') != std::wstring::npos && relation_name.front() != L'<' && relation_name.front() != L'{')
                relation_name = L"(" + relation_name + L")";
        }
    }

    std::wstring objects_name;

    if (objects.size() > max_objects)
    {
        objects_name = string::mark_identifier(L"(... " + std::to_wstring(objects.size()) + L" objects ...)");
    }
    else
    {
        for (Node object : objects)
        {
            std::wstring o_str;
            format_fact(o_str, lang, object, max_objects, variables, resolved, child_history);

            // Wrap object only if it's a composite fact, not a named atom
            if (!o_str.empty() && o_str.find(L' ') != std::wstring::npos && o_str.front() != L'<' && o_str.front() != L'{')
            {
                Node         eff_obj  = resolve_var(object);
                std::wstring raw_name = get_formatted_name(eff_obj, lang);
                // Compare formatted string with MARKED raw name
                if (o_str != string::mark_identifier(raw_name))
                {
                    o_str = L"(" + o_str + L")";
                }
            }

            if (o_str.empty()) o_str = string::mark_identifier(L"?");

            if (!objects_name.empty()) objects_name += L" ";
            objects_name += o_str;
        }
        if (objects_name.empty()) objects_name = string::mark_identifier(L"?");
    }

    // The components (subject_name, relation_name, objects_name) are already marked.
    result = subject_name + L" " + relation_name + L" " + objects_name;

    boost::replace_all(result, L"\r\n", L" --- ");
    boost::replace_all(result, L"\n", L" --- ");
    boost::trim(result);
#ifdef DEBUG_FORMAT_FACT
    std::clog << indent << "[DEBUG format_fact] EXIT result='" << string::unicode::to_utf8(result) << "'" << std::endl;
#endif
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
            format_fact(output, _lang, node, max_neighbors);
            name = string::unicode::to_utf8(output);
        }
    }
    else if (prepend_num && !Impl::is_hash(node) && !Impl::is_var(node))
    {
        name = "(" + std::to_string(static_cast<unsigned long long>(node)) + ") " + name;
    }

    return name;
}

void Zelph::collect_mermaid_nodes(WrapperNode                                                     current_wrap,
                                  int                                                             max_depth,
                                  std::unordered_set<WrapperNode>&                                visited,
                                  std::unordered_set<Node>&                                       processed_edge_hashes,
                                  const adjacency_set&                                            conditions,
                                  const adjacency_set&                                            deductions,
                                  std::vector<std::tuple<WrapperNode, WrapperNode, std::string>>& raw_edges,
                                  std::unordered_set<WrapperNode>&                                all_nodes,
                                  int                                                             max_neighbors,
                                  size_t&                                                         placeholder_counter) const
{
    if (--max_depth <= 0 || visited.count(current_wrap))
        return;

    visited.insert(current_wrap);
    all_nodes.insert(current_wrap);

    if (current_wrap.is_placeholder) return; // No recursion for placeholders

    Node current = current_wrap.value;

    // Left neighbors (incoming)
    const auto& lefts      = _pImpl->get_left(current);
    size_t      num_left   = lefts.size();
    size_t      limit_left = (max_neighbors > 0) ? std::min(static_cast<size_t>(max_neighbors), num_left) : num_left;
    auto        left_it    = lefts.begin();
    for (size_t i = 0; i < limit_left; ++i, ++left_it)
    {
        Node left = *left_it;
        Node hash = Impl::create_hash({current, left});

        if (processed_edge_hashes.insert(hash).second)
        {
            bool        is_bi = _pImpl->has_left_edge(left, current);
            std::string arrow = is_bi ? "<-->" : "-->";
            raw_edges.emplace_back(WrapperNode{false, left}, WrapperNode{false, current}, arrow);
            all_nodes.insert(WrapperNode{false, left});
        }

        collect_mermaid_nodes(WrapperNode{false, left}, max_depth, visited, processed_edge_hashes, conditions, deductions, raw_edges, all_nodes, max_neighbors, placeholder_counter);
    }
    if (max_neighbors > 0 && num_left > static_cast<size_t>(max_neighbors))
    {
        // Add unique placeholder for lefts
        ++placeholder_counter;
        size_t      total = num_left;
        WrapperNode placeholder_wrap{true, placeholder_counter, total};
        std::string arrow = "-->"; // Simple directed to current
        raw_edges.emplace_back(placeholder_wrap, WrapperNode{false, current}, arrow);
        all_nodes.insert(placeholder_wrap);
    }

    // Right neighbors (outgoing)
    const auto& rights      = _pImpl->get_right(current);
    size_t      num_right   = rights.size();
    size_t      limit_right = (max_neighbors > 0) ? std::min(static_cast<size_t>(max_neighbors), num_right) : num_right;
    auto        right_it    = rights.begin();
    for (size_t i = 0; i < limit_right; ++i, ++right_it)
    {
        Node right = *right_it;
        Node hash  = Impl::create_hash({current, right});

        if (processed_edge_hashes.insert(hash).second)
        {
            bool        is_bi = _pImpl->has_right_edge(right, current);
            std::string arrow = is_bi ? "<-->" : "-->";
            raw_edges.emplace_back(WrapperNode{false, current}, WrapperNode{false, right}, arrow);
            all_nodes.insert(WrapperNode{false, right});
        }

        collect_mermaid_nodes(WrapperNode{false, right}, max_depth, visited, processed_edge_hashes, conditions, deductions, raw_edges, all_nodes, max_neighbors, placeholder_counter);
    }
    if (max_neighbors > 0 && num_right > static_cast<size_t>(max_neighbors))
    {
        // Add unique placeholder for rights
        ++placeholder_counter;
        size_t      total = num_right;
        WrapperNode placeholder_wrap{true, placeholder_counter, total};
        std::string arrow = "-->";
        raw_edges.emplace_back(WrapperNode{false, current}, placeholder_wrap, arrow);
        all_nodes.insert(placeholder_wrap);
    }
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

void Zelph::gen_mermaid_html(Node start, std::string file_name, int max_depth, int max_neighbors) const
{
    adjacency_set conditions, deductions;

    for (Node rule : _pImpl->get_left(core.Causes))
    {
        adjacency_set current_deductions;
        Node          condition = parse_fact(rule, current_deductions);

        if (condition && condition != core.Causes)
        {
            conditions.insert(condition);
            for (Node deduction : current_deductions)
            {
                deductions.insert(deduction);
            }
        }
    }

    std::unordered_set<WrapperNode>                                visited;
    std::unordered_set<Node>                                       processed_edge_hashes;
    std::vector<std::tuple<WrapperNode, WrapperNode, std::string>> raw_edges;
    std::unordered_set<WrapperNode>                                all_nodes;
    size_t                                                         placeholder_counter = 0;

    collect_mermaid_nodes(WrapperNode{false, start}, max_depth, visited, processed_edge_hashes, conditions, deductions, raw_edges, all_nodes, max_neighbors, placeholder_counter);

    // Node IDs, definitions and styles
    std::map<WrapperNode, std::string> node_ids;
    std::vector<std::string>           node_defs;
    std::vector<std::string>           style_defs;

    for (const WrapperNode& wn : all_nodes)
    {
        std::string id;
        std::string raw_label;
        if (wn.is_placeholder)
        {
            id        = "ph_" + std::to_string(wn.value);
            raw_label = "[... " + std::to_string(wn.total_count) + " nodes ...]";
        }
        else
        {
            id        = "n_" + std::to_string(static_cast<unsigned long long>(wn.value));
            raw_label = get_name_hex(wn.value, true, max_neighbors);
        }
        node_ids[wn] = id;

        std::string label = raw_label;
        boost::replace_all(label, "\"", "\\\"");

        node_defs.push_back("    " + id + "[\"" + label + "\"]");

        std::string fill_color;
        if (!wn.is_placeholder)
        {
            Node node = wn.value;
            if (node == start)
            {
                fill_color = "#FFBB00"; // Special color for start node
            }
            else if (Impl::is_var(node))
            {
                fill_color = "#eee8dc"; // cornsilk2
            }
            else if (conditions.count(node))
            {
                fill_color = "#87cefa"; // lightskyblue
            }
            else if (deductions.count(node))
            {
                fill_color = "#bcee68"; // darkolivegreen2
            }
        }
        else
        {
            fill_color = "#d3d3d3";
        }

        if (!fill_color.empty())
        {
            style_defs.push_back("    style " + id + " fill:" + fill_color + ",stroke:#333,stroke-width:2px");
        }
    }

    // Edges
    std::vector<std::string> edge_lines;
    edge_lines.reserve(raw_edges.size());
    for (const auto& [from, to, arrow] : raw_edges)
    {
        edge_lines.push_back("    " + node_ids[from] + " " + arrow + " " + node_ids[to]);
    }

    // Build Mermaid
    std::stringstream mermaid;
    mermaid << "graph TD" << std::endl;

    for (const auto& def : node_defs)
        mermaid << def << std::endl;

    for (const auto& style : style_defs)
        mermaid << style << std::endl;

    for (const auto& edge : edge_lines)
        mermaid << edge << std::endl;

    // HTML template
    const std::string html_header = R"(<!DOCTYPE html>
<html lang="de">
<head>
    <meta charset="UTF-8">
    <title>Zelph Graph</title>
    <script src="https://cdn.jsdelivr.net/npm/mermaid/dist/mermaid.min.js"></script>
    <script>
        mermaid.initialize({
            startOnLoad: true,
            theme: 'default',
            flowchart: { useMaxWidth: true }
        });
    </script>
    <style>
        body { margin: 20px; background: #ffffff; font-family: sans-serif; }
        .mermaid { text-align: center; }
    </style>
</head>
<body>
    <div class="mermaid">
)";

    const std::string html_footer = R"(
    </div>
</body>
</html>
)";

    std::ofstream file(file_name);
    if (!file.is_open())
        throw std::runtime_error("Kann Datei nicht öffnen: " + file_name);

    file << html_header << mermaid.str() << html_footer;
}

void Zelph::print(const std::wstring& msg, const bool o) const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    _print(msg, o);
}

void Zelph::save_to_file(const std::string& filename) const
{
    _pImpl->saveToFile(filename);
}

void Zelph::load_from_file(const std::string& filename) const
{
    _pImpl->loadFromFile(filename);
}
