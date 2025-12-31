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
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace zelph::network;

std::string Zelph::get_version()
{
    return "0.9";
}

Zelph::Zelph(const std::function<void(const std::wstring&, const bool)>& print)
    : _pImpl{new Impl}
    , core({_pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create()})
    , _print(print)
{
    fact(core.IsA, core.IsA, {core.RelationTypeCategory});
    fact(core.Unequal, core.IsA, {core.RelationTypeCategory});
    fact(core.Causes, core.IsA, {core.RelationTypeCategory});
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

        const std::vector<Node> cores = {
            core.RelationTypeCategory,
            core.Causes,
            core.And,
            core.IsA,
            core.Unequal,
            core.Contradiction};

        for (Node c : cores)
        {
            if (!has_name(c, lang))
            {
                std::wstring name = get_name(c, "zelph", false);
                if (!name.empty())
                {
                    set_name(c, name, lang);
                }
            }
        }
    }
}

void Zelph::set_name(const Node node, const std::wstring& name, std::string lang)
{
    if (lang.empty()) lang = _lang;
#if _DEBUG
    // std::wcout << L"Node " << node << L" has name '" << name << L"' (" << std::wstring(lang.begin(), lang.end()) << L")" << std::endl;
#endif

    std::lock_guard<std::mutex> lock(_pImpl->_mtx_node_of_name);
    std::lock_guard<std::mutex> lock2(_pImpl->_mtx_name_of_node);
    _pImpl->_node_of_name[lang][name] = node;
    _pImpl->_name_of_node[lang][node] = name;
}

Node Zelph::node(const std::wstring& name, std::string lang)
{
    if (lang.empty()) lang = _lang;
    if (name.empty())
    {
        throw std::invalid_argument("Zelph::node(): name cannot be empty");
    }

    std::lock_guard<std::mutex> lock(_pImpl->_mtx_node_of_name);

    auto& node_of_name = _pImpl->_node_of_name[lang];
    auto  it           = node_of_name.find(name);
    if (it != node_of_name.end())
    {
        return it->second;
    }

    Node                        new_node = _pImpl->create();
    std::lock_guard<std::mutex> lock2(_pImpl->_mtx_name_of_node);
    node_of_name[name]                    = new_node;
    _pImpl->_name_of_node[lang][new_node] = name;

    return new_node;
}

bool Zelph::has_name(const Node node, const std::string& lang) const
{
    std::lock_guard<std::mutex> lock(_pImpl->_mtx_name_of_node);

    auto& name_of_node = _pImpl->_name_of_node[lang];
    auto  it           = name_of_node.find(node);
    return it != name_of_node.end();
}

std::wstring Zelph::get_name(const Node node, std::string lang, const bool fallback, const bool process_node) const
{
    if (lang.empty()) lang = _lang;

    if (_process_node && process_node)
    {
        _process_node(node, lang);
    }

    // return `node` in the requested language (if available)
    {
        std::lock_guard<std::mutex> lock(_pImpl->_mtx_name_of_node);
        auto&                       name_of_node = _pImpl->_name_of_node[lang];
        auto                        it           = name_of_node.find(node);
        if (it != name_of_node.end())
        {
            return it->second;
        }
    }

    // try English as fallback language
    if (fallback)
    {
        std::lock_guard<std::mutex> lock(_pImpl->_mtx_name_of_node);
        auto&                       name_of_node = _pImpl->_name_of_node["en"];
        auto                        it           = name_of_node.find(node);
        if (it != name_of_node.end())
        {
            return it->second;
        }
    }

    // try zelph as fallback language
    if (fallback)
    {
        std::lock_guard<std::mutex> lock(_pImpl->_mtx_name_of_node);
        auto&                       name_of_node = _pImpl->_name_of_node["zelph"];
        auto                        it           = name_of_node.find(node);
        if (it != name_of_node.end())
        {
            return it->second;
        }
    }

    // try an arbitrary language as fallback
    if (fallback)
    {
        std::lock_guard<std::mutex> lock(_pImpl->_mtx_name_of_node);
        for (const auto& l : _pImpl->_name_of_node)
        {
            auto it2 = l.second.find(node);
            if (it2 != l.second.end())
                return it2->second;
        }
    }

    return L""; // return empty string if this node has no name (which can happen for internally generated nodes, see Interactive::Impl::process_fact and Interactive::Impl::process_rule)
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

std::string Zelph::get_name_hex(Node node, bool prepend_num)
{
    std::string name = utils::str(get_name(node, _lang, true));

    if (name.empty())
    {
        if (_pImpl->is_var(node))
        {
            name = std::to_string(static_cast<int>(node));
        }
        else
        {
            std::wstring output;
            format_fact(output, _lang, node);
            name = utils::str(output);
        }
    }
    else if (prepend_num && !_pImpl->is_hash(node) && !_pImpl->is_var(node))
    {
        name = "(" + std::to_string(node) + ") " + name;
    }

    boost::replace_all(name, "\r\n", "\\n");
    boost::replace_all(name, "\n", "\\n");
    return name;
}

Node Zelph::get_node(const std::wstring& name, std::string lang) const
{
    if (lang.empty()) lang = _lang;
    std::lock_guard<std::mutex> lock(_pImpl->_mtx_node_of_name);
    auto                        node_of_name = _pImpl->_node_of_name.find(lang);
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

adjacency_set Zelph::get_left(const Node b)
{
    return _pImpl->get_left(b);
}

adjacency_set Zelph::get_right(const Node b)
{
    return _pImpl->get_right(b);
}

Answer Zelph::check_fact(const Node source, const Node relationType, const adjacency_set& targets)
{
    bool known = false;

    Node relation = _pImpl->create_hash(relationType, source, targets);

    if (_pImpl->exists(relation))
    {
        const adjacency_set& connectedFromRelationType = _pImpl->get_right(relation);
        const adjacency_set& connectedToRelationType   = _pImpl->get_left(relation);

        known = connectedFromRelationType.count(source) == 1
             && connectedToRelationType.count(source) == 1 // source must be connected with and from <--> relation (i.e. bidirectional, to distinguish it from targets)
             && [&]()
        {
            for (Node t : targets)
                if (connectedToRelationType.count(t) == 0) return false;
            return true;
        }() // targets must all be connected with relation
             && [&]()
        {
            for (Node t : targets)
                if (connectedFromRelationType.count(t) == 1) return false;
            return true;
        }(); // no target must be connected from relation

        if (!_pImpl->_format_fact_level && !known && !_pImpl->is_var(source) && !_pImpl->is_var(relationType) && [&]()
            { for (const Node t : targets) if (_pImpl->is_var(t)) return false; return true; }())
        {
            const bool relationConnectsToSource              = connectedFromRelationType.count(source) == 1;
            const bool sourceConnectsToRelation              = connectedToRelationType.count(source) == 1;
            const bool targetsMustAllBeConnectedWithRelation = [&]()
            {
                for (Node t : targets)
                    if (connectedToRelationType.count(t) == 0) return false;
                return true;
            }();
            const bool noTargetMustBeConnectedFromRelation = [&]()
            {
                for (Node t : targets)
                    if (connectedFromRelationType.count(t) == 1) return false;
                return true;
            }();

            // inconsistent state => debug output TODO
            std::wstring output;
            format_fact(output, _lang, relation);
            print(output, true);

            gen_dot(relation, "debug.dot", 2);
            print(L"relationConnectsToSource              == " + std::to_wstring(relationConnectsToSource), true);
            print(L"sourceConnectsToRelation              == " + std::to_wstring(sourceConnectsToRelation), true);
            print(L"targetsMustAllBeConnectedWithRelation == " + std::to_wstring(targetsMustAllBeConnectedWithRelation), true);
            print(L"noTargetMustBeConnectedFromRelation   == " + std::to_wstring(noTargetMustBeConnectedFromRelation), true);
        }
    }

    if (known)
    {
        return {_pImpl->probability(relation, relationType), relation};
    }
    else
    {
        return Answer(relation); // unknown
    }
}

Node Zelph::fact(const Node source, const Node relationType, const adjacency_set& targets, const long double probability)
{
    const Answer answer = check_fact(source, relationType, targets);

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
        if (targets.count(relationType) == 1)
        {
            // 1 13 13
            // ~ is for example is for example <= (~  is opposite of  is for example), (is for example  ~  ->)
            throw std::runtime_error("fact(): facts with same relation type and object are not supported.");
        }

        if (relationType != core.IsA) // note that the initial constructor call fact(core.IsA, core.IsA, core.RelationTypeCategory) is executed as intended
        {
            fact(relationType, core.IsA, {core.RelationTypeCategory});
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

        _pImpl->connect(source, answer.relation());
        _pImpl->connect(answer.relation(), source);
        for (const Node t : targets)
        {
            if (t == source)
            {
                // We do not support relations that have the same subject and object. Real life example (from invalid wikidata entries):
                // South Africa (Q258)  country (P17)  South Africa (Q258)
                // or
                // chemical substance  has part  chemical substance ⇐ (matter  has part  chemical substance), (chemical substance  is subclass of  matter)

                const std::wstring name_subject_object = get_name(source, _lang, true, false);
                const std::wstring name_relationType   = get_name(relationType, _lang, true, false);

                throw std::runtime_error("fact(): facts with same subject and object are not supported: " + utils::str(name_subject_object) + " " + utils::str(name_relationType) + " " + utils::str(name_subject_object));
            }

            _pImpl->connect(t, answer.relation());
        }
        _pImpl->connect(answer.relation(), relationType, probability);
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
                assert(nd != parent); // indicates corrupt database

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

void Zelph::format_fact(std::wstring& result, const std::string& lang, Node fact, const Variables& variables, Node parent, std::shared_ptr<std::unordered_set<Node>> history)
{
    // Formats a fact into a string representation.

    utils::IncDec incDec(_pImpl->_format_fact_level);
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
        subject      = utils::get(variables, subject);
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
            format_fact(subject_name, lang, subject, variables, fact, history);
            subject_name = L"(" + subject_name + L")";
        }

        Node relation = parse_relation(fact);
        relation      = utils::get(variables, relation);
        relation_name = relation ? get_formatted_name(relation, lang) : L"?";
        if (relation_name.empty())
        {
            format_fact(relation_name, lang, relation, variables, fact, history);
            relation_name = L"(" + relation_name + L")";
        }
    }

    std::wstring objects_name;
    for (Node object : objects)
    {
        object = utils::get(variables, object);
        if (!objects_name.empty()) objects_name += get_formatted_name(core.And, lang) + L" ";
        std::wstring object_name = object ? get_formatted_name(object, lang) : L"?";
        if (object_name.empty())
        {
#ifdef _DEBUG
            std::clog << "[DEBUG format_fact] object_name is empty for object=" << object
                      << ", is_hash=" << _pImpl->is_hash(object)
                      << ", will recurse" << std::endl;
#endif
            format_fact(object_name, lang, object, variables, fact, history);
            object_name = L"(" + object_name + L")";
        }
        objects_name += object_name;
    }
    if (objects_name.empty()) objects_name = L"?";

    result = utils::mark_identifier(subject_name) + L" " + utils::mark_identifier(relation_name) + L" " + utils::mark_identifier(objects_name);

    boost::replace_all(result, L"\r\n", L" --- ");
    boost::replace_all(result, L"\n", L" --- ");
    boost::trim(result);
}

Node Zelph::count() const
{
    return _pImpl->count();
}

NodeView Zelph::get_all_nodes() const
{
    return NodeView(_pImpl->_name_of_node);
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

void Zelph::add_nodes(Node current, adjacency_set& touched, const adjacency_set& conditions, const adjacency_set& deductions, std::ofstream& dot, int max_depth, std::unordered_set<std::string>& written_edges)
{
    if (--max_depth > 0 && touched.count(current) == 0)
    {
        touched.insert(current);
        std::string current_name = get_name_hex(current);

        if (_pImpl->is_var(current))
        {
            dot << "\"" << current_name << "\" [color=cornsilk2, style=filled]" << std::endl;
        }
        else if (conditions.find(current) != conditions.end())
        {
            dot << "\"" << current_name << "\" [color=lightskyblue, style=filled]" << std::endl;
        }
        else if (deductions.find(current) != deductions.end())
        {
            dot << "\"" << current_name << "\" [color=darkolivegreen2, style=filled]" << std::endl;
        }

        for (const Node& left : _pImpl->get_left(current))
        {
            Node hash = _pImpl->create_hash({current, left});
            if (touched.count(hash) == 0)
            {
                touched.insert(hash);
                std::string left_name = get_name_hex(left);

                std::string key;
                bool        is_bidirectional = _pImpl->has_left_edge(left, current);

                if (is_bidirectional)
                {
                    if (left_name < current_name)
                        key = left_name + "<->" + current_name;
                    else
                        key = current_name + "<->" + left_name;
                }
                else
                {
                    key = left_name + "->" + current_name;
                }

                if (written_edges.find(key) == written_edges.end())
                {
                    written_edges.insert(key);
                    dot << "\"" << left_name << "\" -> \"" << current_name << "\"";
                    if (is_bidirectional)
                    {
                        dot << R"( [dir="both"])";
                    }
                    dot << ";" << std::endl;
                }
            }
            add_nodes(left, touched, conditions, deductions, dot, max_depth, written_edges);
        }

        for (const Node& right : _pImpl->get_right(current))
        {
            Node hash = _pImpl->create_hash({current, right});
            if (touched.count(hash) == 0)
            {
                touched.insert(hash);
                std::string right_name = get_name_hex(right);

                std::string key;
                bool        is_bidirectional = _pImpl->has_left_edge(right, current);

                if (is_bidirectional)
                {
                    if (current_name < right_name)
                        key = current_name + "<->" + right_name;
                    else
                        key = right_name + "<->" + current_name;
                }
                else
                {
                    key = current_name + "->" + right_name;
                }

                if (written_edges.find(key) == written_edges.end())
                {
                    written_edges.insert(key);
                    dot << "\"" << current_name << "\" -> \"" << right_name << "\"";
                    if (is_bidirectional)
                    {
                        dot << R"( [dir="both"])";
                    }
                    dot << ";" << std::endl;
                }
            }
            add_nodes(right, touched, conditions, deductions, dot, max_depth, written_edges);
        }
    }
}

void Zelph::gen_dot(Node start, std::string file_name, int max_depth)
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

    adjacency_set                   touched;
    std::unordered_set<std::string> written_edges;
    std::ofstream                   dot(file_name, std::ios_base::out);

    dot << "digraph graphname{" << std::endl;

    add_nodes(start, touched, conditions, deductions, dot, max_depth, written_edges);

    dot << "}" << std::endl;
}

void Zelph::print(const std::wstring& msg, const bool o) const
{
    std::lock_guard<std::mutex> lock(_pImpl->_mtx_print);
    _print(msg, o);
}

void Zelph::save_to_file(const std::string& filename)
{
    std::ofstream                   ofs(filename, std::ios::binary);
    boost::archive::binary_oarchive oa(ofs);
    oa << *_pImpl;
}

void Zelph::load_from_file(const std::string& filename)
{
    std::ifstream                   ifs(filename, std::ios::binary);
    boost::archive::binary_iarchive ia(ifs);
    ia >> *_pImpl;
}