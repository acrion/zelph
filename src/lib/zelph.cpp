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
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace zelph::network;

std::string Zelph::get_version()
{
    return "0.9.4";
}

Zelph::Zelph(const std::unordered_map<network::Node, std::wstring>& core_node_names, const std::function<void(const std::wstring&, const bool)>& print)
    : _pImpl{new Impl}
    , core({_pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create()})
    , _core_node_names(core_node_names)
    , _print(print)
{
    fact(core.IsA, core.IsA, {core.RelationTypeCategory});
    fact(core.Unequal, core.IsA, {core.RelationTypeCategory});
    fact(core.Causes, core.IsA, {core.RelationTypeCategory});
    fact(core.FollowedBy, core.IsA, {core.RelationTypeCategory});
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

// Sets the name of an already existing node in a specific language.
// This overload is used when you have a known Node handle and want to directly assign or update its name.
// It does not create a new node – it only updates the name mappings for the given language.
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
            // Conflict: the same name is already used for another node
            Node from = existing->second;
            Node into = node;

            if (_pImpl->is_var(from) != _pImpl->is_var(into))
            {
                std::stringstream s;
                s << "Requested name '" << string::unicode::to_utf8(name) << "' is already used by node " << existing->second << " in language '" << lang << "'. Merging the two nodes is impossible because one node is a variable, the other not.";
                throw std::runtime_error(s.str());
            }

            if (!_pImpl->is_var(from))
            {
                std::wclog << L"Warning: Merging Node " << from
                           << L" into Node " << into
                           << L" due to name conflict '" << name
                           << L"' in language '" << string::unicode::from_utf8(lang) << L"'." << std::endl;
            }

            // Merge
            _pImpl->merge(from, into);

            // Transfer names from the merged node
            _pImpl->transfer_names(from, into);

            // Update the reverse mapping to the surviving node
            _pImpl->_node_of_name[lang][name] = into;
        }
    }
    else // !merge_on_conflict
    {
        // In case the caller does not request merging, we set the node of this name independent of its existence.
        // This is useful for Wikidata import, when setting names in a different language than "wikidata", which are all ambiguous.
        // See Wikidata::process_import()
        // We also use it in case of variable names in rules, because they can only refer to the current rule, never a different one.
        // This strategy regarding variables is not strictly required, but it makes the topology of the network cleaner, because
        // that way a variable node is only connect a single rule. See console::Interactive::Impl::process_fact()
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

                if (_pImpl->is_var(from) != _pImpl->is_var(into))
                {
                    std::stringstream s;
                    s << "Requested name '" << string::unicode::to_utf8(name_in_current_lang) << "' is already used by node " << into << " in language '" << lang << "'. Merging the two nodes is impossible because one node is a variable, the other not.";
                    throw std::runtime_error(s.str());
                }

                if (!_pImpl->is_var(from))
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

void Zelph::cleanup_isolated(size_t& removed_count)
{
    removed_count = 0;

    _pImpl->remove_isolated_nodes(removed_count);
}

size_t Zelph::cleanup_names()
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

    std::lock_guard lock(_pImpl->_mtx_node_of_name);

    auto& node_of_name = _pImpl->_node_of_name[lang];
    auto  it           = node_of_name.find(name);
    if (it != node_of_name.end())
    {
        return it->second;
    }

    Node            new_node = _pImpl->create();
    std::lock_guard lock2(_pImpl->_mtx_name_of_node);
    node_of_name[name]                    = new_node;
    _pImpl->_name_of_node[lang][new_node] = name;

    return new_node;
}

bool Zelph::exists(uint64_t nd)
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

    {
        auto it = _core_node_names.find(node);

        if (it != _core_node_names.end())
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
                if (source != target && (!exclude_vars || !_pImpl->is_var(source)))
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

Answer Zelph::check_fact(const Node subject, const Node predicate, const adjacency_set& objects)
{
    bool known = false;

    Node relation = _pImpl->create_hash(predicate, subject, objects);

    if (_pImpl->exists(relation))
    {
        const adjacency_set& connectedFromRelation = _pImpl->get_right(relation);
        const adjacency_set& connectedToRelation   = _pImpl->get_left(relation);

        known = connectedFromRelation.count(subject) == 1
             && connectedToRelation.count(subject) == 1 // subject must be connected from and to <--> relation node (i.e. bidirectional, to distinguish it from objects)
             && [&]()
        {
            for (Node t : objects)
                if (connectedToRelation.count(t) == 0) return false;
            return true;
        }() // objects must all be connected to relation
             && [&]()
        {
            for (Node t : objects)
                if (connectedFromRelation.count(t) == 1) return false;
            return true;
        }(); // no object must be connected from relation node

        if (!_pImpl->_format_fact_level
            && !known
            && !_pImpl->is_var(subject)
            && !_pImpl->is_var(predicate)
            && [&]()
            { for (const Node t : objects) if (_pImpl->is_var(t)) return false; return true; }())
        {
            const bool relationConnectsToSubject   = connectedFromRelation.count(subject) == 1;
            const bool subjectConnectsToRelation   = connectedToRelation.count(subject) == 1;
            const bool allObjectsConnectToRelation = [&]()
            {
                for (Node t : objects)
                    if (connectedToRelation.count(t) == 0) return false;
                return true;
            }();
            const bool noObjectsAreConnectedFromRelation = [&]()
            {
                for (Node t : objects)
                    if (connectedFromRelation.count(t) == 1) return false;
                return true;
            }();

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

        if (predicate != core.IsA) // note that the initial constructor call fact(core.IsA, core.IsA, core.RelationTypeCategory) is executed as intended
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
                // We do not support relations that have the same subject and object. Real life example (from invalid wikidata entries):
                // South Africa (Q258)  country (P17)  South Africa (Q258)
                // or
                // chemical substance  has part  chemical substance ⇐ (matter  has part  chemical substance), (chemical substance  is subclass of  matter)

                const std::wstring name_subject_object = get_name(subject, _lang, true);
                const std::wstring name_relationType   = get_name(predicate, _lang, true);

                throw std::runtime_error("fact(): facts with same subject and object are not supported: " + string::unicode::to_utf8(name_subject_object) + " " + string::unicode::to_utf8(name_relationType) + " " + string::unicode::to_utf8(name_subject_object));
            }

            _pImpl->connect(t, answer.relation());
        }
        _pImpl->connect(answer.relation(), predicate, probability);
    }

    return answer.relation();
}

Node Zelph::condition(Node op, const adjacency_set& conditions) const
{
    Node relation = _pImpl->create_hash(op, conditions);
    _pImpl->create(relation);
    _pImpl->connect(relation, op);
    for (Node condition : conditions)
        _pImpl->connect(condition, relation);
    return relation;
}

Node Zelph::sequence(const std::vector<std::wstring>& elements)
{
    if (elements.empty()) return 0;

    // Create the super-node representing the sequence itself
    Node seq_node = _pImpl->create();

    Node prev_node = 0;

    for (const auto& elem_name : elements)
    {
        // Create a distinct node for this element instance (e.g., the first "t")
        Node current_node = _pImpl->create();
        set_name(current_node, elem_name, _lang, false);

        // Retrieve or create the concept node for the name (e.g., the concept of "t")
        Node concept_node = node(elem_name, _lang);

        // Define what this node is (an instance of the concept)
        fact(current_node, core.IsA, {concept_node});

        // Link to the sequence container
        fact(current_node, core.PartOf, {seq_node});

        // Maintain order
        if (prev_node != 0)
        {
            fact(prev_node, core.FollowedBy, {current_node});
        }

        prev_node = current_node;
    }

    return seq_node;
}

Node Zelph::parse_fact(Node rule, adjacency_set& deductions, Node parent) const
{
    Node subject = 0; // 0 means failure
    deductions.clear();
    bool empty_subject = false;
    for (Node nd : _pImpl->get_left(rule))
    {
        if (_pImpl->get_left(nd).count(rule) == 1)
        {
            if (_pImpl->get_right(nd).count(core.Causes) == 0) // TODO: this might be not sufficient if rule is a sub-condition!?
            {
                // assert(nd != parent); // indicates corrupt database

                if (subject)
                    empty_subject = true; // there may be only 1 subject
                else
                    subject = nd;
            }
        }
        else
        {
            assert(nd != parent); // indicates corrupt database
            deductions.insert(nd);
        }
    }

    return empty_subject ? 0 : subject;
}

Node Zelph::parse_relation(const Node rule)
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

void Zelph::format_fact(std::wstring& result, const std::string& lang, Node fact, const int max_objects, const Variables& variables, Node parent, std::shared_ptr<std::unordered_set<Node>> history)
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
    if (!history) history = std::make_shared<std::unordered_set<Node>>();

    if (history->find(fact) != history->end())
    {
        result = L"?";
        return;
    }

    adjacency_set objects;
    Node          subject = parse_fact(fact, objects, parent);

    bool is_condition = _pImpl->get_right(fact).count(core.And) == 1; // if fact is a condition node (having relation type core.And), there is no subject
    if (subject == 0 && !is_condition)
    {
        result = L"??";
        return;
    }
    history->insert(fact);
    std::wstring subject_name, relation_name;

    if (!is_condition || subject)
    {
#if _DEBUG
        Node subject_before = subject;
#endif
        subject      = string::get(variables, subject);
        subject_name = subject ? get_formatted_name(subject, lang) : (is_condition ? L"" : L"?");
#if _DEBUG
        if (subject_name == L"?")
        {
            std::clog << "[DEBUG format_fact] subject_name='?' for fact=" << fact
                      << ", subject_before_subst=" << subject_before
                      << ", is_var=" << _pImpl->is_var(subject_before)
                      << ", subject_after_subst=" << subject
                      << ", variables.size()=" << variables.size();

            std::clog << ", variables={";
            for (const auto& v : variables)
            {
                std::clog << v.first << "->" << v.second << " ";
            }
            std::clog << "}" << std::endl;
        }
#endif
        if (subject_name.empty())
        {
            format_fact(subject_name, lang, subject, max_objects, variables, fact, history);
            subject_name = L"(" + subject_name + L")";
        }

        Node relation = parse_relation(fact);
        relation      = string::get(variables, relation);
        relation_name = relation ? get_formatted_name(relation, lang) : L"?";
        if (relation_name.empty())
        {
            format_fact(relation_name, lang, relation, max_objects, variables, fact, history);
            relation_name = L"(" + relation_name + L")";
        }
    }

    std::wstring objects_name;

    if (objects.size() > max_objects)
    {
        objects_name = L"(... " + std::to_wstring(objects.size()) + L" objects ...)";
    }
    else
    {
        for (Node object : objects)
        {
            object = string::get(variables, object);
            if (!objects_name.empty()) objects_name += get_formatted_name(core.And, lang) + L" ";
            std::wstring object_name = object ? get_formatted_name(object, lang) : L"?";
            if (object_name.empty())
            {
#ifdef _DEBUG
                std::clog << "[DEBUG format_fact] object_name is empty for object=" << object
                          << ", is_hash=" << _pImpl->is_hash(object)
                          << ", will recurse" << std::endl;
#endif
                format_fact(object_name, lang, object, max_objects, variables, fact, history);
                object_name = L"(" + object_name + L")";
            }
            objects_name += object_name;
        }
        if (objects_name.empty()) objects_name = L"?";
    }

    result = string::mark_identifier(subject_name) + L" " + string::mark_identifier(relation_name) + L" " + string::mark_identifier(objects_name);

    boost::replace_all(result, L"\r\n", L" --- ");
    boost::replace_all(result, L"\n", L" --- ");
    boost::trim(result);
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

void Zelph::remove_rules()
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

void Zelph::remove_node(Node node)
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

std::string Zelph::get_name_hex(Node node, bool prepend_num, int max_neighbors)
{
    std::string name = string::unicode::to_utf8(get_name(node, _lang, true));

    if (name.empty())
    {
        if (_pImpl->is_var(node))
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
    else if (prepend_num && !_pImpl->is_hash(node) && !_pImpl->is_var(node))
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
                                  size_t&                                                         placeholder_counter)
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
        Node hash = _pImpl->create_hash({current, left});

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
        Node hash  = _pImpl->create_hash({current, right});

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

void Zelph::gen_mermaid_html(Node start, std::string file_name, int max_depth, int max_neighbors)
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
            else if (_pImpl->is_var(node))
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

void Zelph::save_to_file(const std::string& filename)
{
    _pImpl->saveToFile(filename);
}

void Zelph::load_from_file(const std::string& filename)
{
    _pImpl->loadFromFile(filename);
}
