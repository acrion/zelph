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

#include "zelph.hpp"
#include "zelph_impl.hpp"
#include <boost/algorithm/string.hpp>

#include <fstream>
#include <iomanip>
#include <sstream>

using namespace zelph::network;

std::string Zelph::get_version()
{
    return "0.5.3";
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

void Zelph::set_name(const Node node, const std::wstring& name, std::string lang) const
{
    if (lang.empty()) lang = _lang;
#if _DEBUG
    // std::wcout << L"Node " << node << L" has name '" << name << L"' (" << std::wstring(lang.begin(), lang.end()) << L")" << std::endl;
#endif

    std::lock_guard<std::mutex> lock(_pImpl->_mtx_name_of_node);
    std::lock_guard<std::mutex> lock2(_pImpl->_mtx_node_of_name);
    _pImpl->_node_of_name[lang][name] = node;
    _pImpl->_name_of_node[lang][node] = name;
}

Node Zelph::node(const std::wstring& name, std::string lang) const
{
    if (lang.empty()) lang = _lang;

    {
        std::lock_guard<std::mutex> lock(_pImpl->_mtx_node_of_name);
        auto&                       node_of_name = _pImpl->_node_of_name[lang];
        auto                        it           = node_of_name.find(name);
        if (it != node_of_name.end())
        {
            return it->second;
        }
    }

    Node new_node = _pImpl->create();
    set_name(new_node, name, lang);
    return new_node;
}

bool Zelph::has_name(const Node node, const std::string& lang) const
{
    auto& name_of_node = _pImpl->_name_of_node[lang];
    auto  it           = name_of_node.find(node);
    return it != name_of_node.end();
}

std::wstring Zelph::get_name(const Node node, std::string lang, const bool fallback) const
{
    if (lang.empty()) lang = _lang;

    if (_process_node)
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
        for (auto l : _pImpl->_name_of_node)
        {
            auto it2 = l.second.find(node);
            if (it2 != l.second.end())
                return it2->second;
        }
    }

    return L""; // return empty string if this node has no name (which can happen for internally generated nodes, see Interactive::Impl::process_fact and Interactive::Impl::process_rule)
}

std::map<Node, std::wstring> Zelph::get_nodes_in_language(const std::string& lang) const
{
    return _pImpl->_name_of_node[lang];
}
std::vector<std::string> Zelph::get_languages() const
{
    std::vector<std::string> result;

    for (const auto& outer_pair : _pImpl->_node_of_name) {
        result.push_back(outer_pair.first);
    }

    return result;
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

std::unordered_set<Node> Zelph::get_sources(const Node relationType, const Node target, const bool exclude_vars) const
{
    std::unordered_set<Node> sources;

    for (Node relation : _pImpl->get_right(target))
        if (_pImpl->get_right(relation).count(relationType) == 1)
            for (Node source : _pImpl->get_left(relation))
                if (source != target && (!exclude_vars || !_pImpl->is_var(source)))
                    sources.insert(source);

    return sources;
}

std::unordered_set<Node> Zelph::filter(const std::unordered_set<Node>& source, const Node target) const
{
    std::unordered_set<Node> result;

    for (Node nd : source)
    {
        if (_pImpl->get_right(nd).count(target) == 1)
        {
            result.insert(nd);
        }
    }

    return result;
}

std::unordered_set<Node> Zelph::filter(const Node fact, const Node relationType, const Node target) const
{
    std::unordered_set<Node> source     = _pImpl->get_right(fact);
    std::unordered_set<Node> left_nodes = _pImpl->get_left(fact);
    std::unordered_set<Node> result;

    for (Node nd : source)
    {
        std::unordered_set<Node> possible_relations = _pImpl->get_right(nd);
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

std::unordered_set<Node> Zelph::filter(const std::unordered_set<Node>& source, const std::function<bool(const Node nd)>& f)
{
    std::unordered_set<Node> result;

    for (const Node nd : source)
    {
        if (f(nd)) result.insert(nd);
    }

    return result;
}

Answer Zelph::check_fact(const Node source, const Node relationType, const std::unordered_set<Node>& targets)
{
    bool known = false;

    Node relation = _pImpl->create_hash(relationType, source, targets);

    if (_pImpl->exists(relation))
    {
        const std::unordered_set<Node>& connectedFromRelationType = _pImpl->get_right(relation);
        const std::unordered_set<Node>& connectedToRelationType   = _pImpl->get_left(relation);

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

Node Zelph::fact(const Node source, const Node relationType, const std::unordered_set<Node>& targets, const long double probability)
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
            // check_fact can return !answer.is_known() though answer.relation exists, which should not happen. Indicates corrupt database or hash collision.
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

                const std::wstring name_subject_object = get_name(source, _lang, true);
                const std::wstring name_relationType   = get_name(relationType, _lang, true);

                throw std::runtime_error("fact(): facts with same subject and object are not supported: " + utils::str(name_subject_object) + " " + utils::str(name_relationType) + " " + utils::str(name_subject_object));
            }

            _pImpl->connect(t, answer.relation());
        }
        _pImpl->connect(answer.relation(), relationType, probability);
    }

    return answer.relation();
}

Node Zelph::condition(Node op, const std::unordered_set<Node>& conditions) const
{
    Node relation = _pImpl->create_hash(op, conditions);
    _pImpl->create(relation);
    _pImpl->connect(relation, op);
    for (Node condition : conditions)
        _pImpl->connect(condition, relation);
    return relation;
}

Node Zelph::parse_fact(Node rule, std::unordered_set<Node>& deductions, Node parent) const
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

void Zelph::format_fact(std::wstring& result, const std::string& lang, Node fact, const Variables& variables, Node parent, std::shared_ptr<std::unordered_set<Node>> history)
{
    utils::IncDec incDec(_pImpl->_format_fact_level);
    if (!history) history = std::make_shared<std::unordered_set<Node>>();

    if (history->find(fact) != history->end())
    {
        result = L"?";
        return;
    }

    std::unordered_set<Node> objects;
    Node                     subject = parse_fact(fact, objects, parent);

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
        subject      = utils::get(variables, subject);
        subject_name = subject ? get_name(subject, lang, true) : (is_condition ? L"" : L"?");
        if (subject_name.empty())
        {
            format_fact(subject_name, lang, subject, variables, fact, history);
            subject_name = L"(" + subject_name + L")";
        }

        Node relation = parse_relation(fact);
        relation      = utils::get(variables, relation);
        relation_name = relation ? get_name(relation, lang, true) : L"?";
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
        if (!objects_name.empty()) objects_name += get_name(core.And, lang, true) + L" ";
        std::wstring object_name = object ? get_name(object, lang, true) : L"?";
        if (object_name.empty())
        {
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

NodeView Zelph::get_all_nodes() const
{
    return NodeView(_pImpl->_name_of_node);
}

void Zelph::add_nodes(Node current, std::unordered_set<Node>& touched, const std::unordered_set<Node>& conditions, const std::unordered_set<Node>& deductions, std::ofstream& dot, int max_depth)
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
                dot << "\"" << left_name << "\" -> \"" << current_name << "\"";

                if (_pImpl->find_left(left)->second.count(current) == 1)
                {
                    dot << R"( [dir="both"])";
                }

                dot << ";" << std::endl;
            }
            add_nodes(left, touched, conditions, deductions, dot, max_depth);
        }

        for (const Node& right : _pImpl->get_right(current))
        {
            Node hash = _pImpl->create_hash({current, right});
            if (touched.count(hash) == 0)
            {
                touched.insert(hash);
                std::string right_name = get_name_hex(right);
                dot << "\"" << current_name << "\" -> \"" << right_name << "\"";

                if (_pImpl->find_right(right)->second.count(current) == 1)
                {
                    dot << R"( [dir="both"])";
                }

                dot << ";" << std::endl;
            }
            add_nodes(right, touched, conditions, deductions, dot, max_depth);
        }
    }
}

void Zelph::gen_dot(Node start, std::string file_name, int max_depth)
{
    std::unordered_set<Node> conditions, deductions;

    for (Node rule : _pImpl->get_left(core.Causes))
    {
        std::unordered_set<Node> current_deductions;
        Node                     condition = parse_fact(rule, current_deductions);

        if (condition && condition != core.Causes)
        {
            conditions.insert(condition);
            for (Node deduction : current_deductions)
            {
                deductions.insert(deduction);
            }
        }
    }

    std::unordered_set<Node> touched;
    std::ofstream            dot(file_name, std::ios_base::out);

    dot << "digraph graphname{" << std::endl;

    add_nodes(start, touched, conditions, deductions, dot, max_depth);

    dot << "}" << std::endl;
}

void Zelph::print(const std::wstring& msg, const bool o) const
{
    std::lock_guard<std::mutex> lock(_pImpl->_mtx_print);
    _print(msg, o);
}
