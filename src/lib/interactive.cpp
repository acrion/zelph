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

#include "interactive.hpp"
#include "reasoning.hpp"
#include "stopwatch.hpp"
#include "utils.hpp"
#include "wikidata.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/bimap.hpp>
#include <boost/tokenizer.hpp>

#include <fstream>
#include <iostream>

#ifdef _WIN32
    #include <io.h>    // for _setmode
    #include <fcntl.h> // for _O_U16TEXT
    #include <stdio.h> // for _fileno
#endif

using namespace zelph;
using boost::escaped_list_separator;
using boost::tokenizer;

class console::Interactive::Impl
{
public:
    Impl()
        : _n(new network::Reasoning([](const std::wstring& str, const bool)
        {
#ifdef _WIN32
            std::wcout << str << std::endl;
#else
            std::clog << network::utils::str(str) << std::endl;
#endif
        }))
    {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_U16TEXT);
#endif

        _n->set_lang("zelph");
    }

    void          process_command(const std::vector<std::wstring>& cmd);
    network::Node process_fact(const std::vector<std::wstring>& tokens, boost::bimap<std::wstring, network::Node>& variables);
    network::Node process_rule(const std::vector<std::wstring>& tokens, const std::wstring& line, boost::bimaps::bimap<std::wstring, zelph::network::Node>& variables, const std::wstring& And, const std::wstring& Causes);
    void          process_token(std::vector<std::wstring>& tokens, bool& is_rule, const std::wstring& first_var, std::wstring& assigns_to_var, const std::wstring& token, const std::wstring& And, const std::wstring& Causes) const;
    static bool   is_var(std::wstring token);

    std::shared_ptr<Wikidata> _wikidata;
    network::Reasoning* const _n;

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;
};

console::Interactive::Interactive()
    : _pImpl(new Impl)
{
    _pImpl->_n->set_name(_pImpl->_n->core.RelationTypeCategory, L"->", "zelph");
    _pImpl->_n->set_name(_pImpl->_n->core.Causes, L"=>", "zelph");
    _pImpl->_n->set_name(_pImpl->_n->core.And, L",", "zelph");
    _pImpl->_n->set_name(_pImpl->_n->core.IsA, L"~", "zelph");
    _pImpl->_n->set_name(_pImpl->_n->core.Unequal, L"!=", "zelph");
    _pImpl->_n->set_name(_pImpl->_n->core.Contradiction, L"!", "zelph");
}

console::Interactive::~Interactive()
{
    delete _pImpl;
}

std::string console::Interactive::get_version()
{
    return network::Zelph::get_version();
}

void console::Interactive::process(std::wstring line) const
{
    try
    {
        if (boost::starts_with(line, "#")) return; // comment

        tokenizer<escaped_list_separator<wchar_t>, std::wstring::const_iterator, std::wstring> tok(line,
                                                                                                   escaped_list_separator<wchar_t>(L"\\", L" \t", L"\""));

        decltype(tok)::iterator it = tok.begin();

        while (it != tok.end() && it->empty())
            ++it;

        if (it == tok.end())
        {
            return; // empty line
        }

        if ((*it)[0] == L'.')
        {
            std::vector<std::wstring> cmd;
            while (it != tok.end())
            {
                if (!it->empty()) cmd.push_back(*it);
                ++it;
            }

            _pImpl->process_command(cmd);
        }
        else
        {
            const std::wstring        And     = _pImpl->_n->get_name(_pImpl->_n->core.And, _pImpl->_n->lang());
            const std::wstring        Causes  = _pImpl->_n->get_name(_pImpl->_n->core.Causes, _pImpl->_n->lang());
            bool                      is_rule = false;
            std::wstring              assigns_to_var;
            std::wstring              first_var = Impl::is_var(*it) ? L"" : *it;
            std::vector<std::wstring> tokens;
            for (; it != tok.end(); ++it)
            {
                _pImpl->process_token(tokens, is_rule, first_var, assigns_to_var, *it, And, Causes);
            }

            boost::bimap<std::wstring, network::Node> variables;
            network::Node                             fact = 0;
            if (is_rule)
            {
                fact = _pImpl->process_rule(tokens, line, variables, And, Causes);
            }
            else
            {
                fact = _pImpl->process_fact(tokens, variables);
            }

            if (!assigns_to_var.empty())
            {
                _pImpl->_n->set_name(fact, assigns_to_var, _pImpl->_n->lang());
            }

            std::wstring output;
            //_pImpl->_n->gen_dot(fact, "debug.dot", 5);
            _pImpl->_n->format_fact(output, _pImpl->_n->lang(), fact);
            _pImpl->_n->print(L"> " + output, false);

            if (!is_rule)
            {
                bool contains_variable = false;

                for (const auto& token : tokens)
                {
                    if (Impl::is_var(token))
                    {
                        contains_variable = true;
                        break;
                    }
                }

                if (contains_variable)
                {
                    _pImpl->_n->apply_rule(0, fact, 0); // query
                }
            }

            run(true, false);
        }
    }
    catch (std::exception& ex)
    {
        throw std::runtime_error("Error in line \"" + network::utils::str(line) + "\": " + ex.what());
    }
}

void console::Interactive::Impl::process_command(const std::vector<std::wstring>& cmd)
{
    _n->set_print([](const std::wstring& str, bool)
    {
#ifdef _WIN32
        std::wcout << str << std::endl;
#else
        std::clog << network::utils::str(str) << std::endl;
#endif
    });

    if (cmd[0] == L".lang")
    {
        if (cmd.size() < 2)
        {
            std::clog << "The current language is '" << _n->get_lang() << "'" << std::endl;
        }
        else
        {
            _n->set_lang(network::utils::str(cmd[1]));
        }
    }
    else if (cmd[0] == L".name")
    {
        if (cmd.size() == 1) throw std::runtime_error("Command .name: Missing current name. Usage: .name <current name in " + _n->lang() + "> <language identifier> <name in that language>");
        if (cmd.size() == 2) throw std::runtime_error("Command .name: Missing language identifier. Usage: .name <current name in " + _n->lang() + "> <language identifier> <name in that language>");
        if (cmd.size() == 3) throw std::runtime_error("Command .name: Missing " + network::utils::str(cmd[2]) + " name of " + network::utils::str(cmd[1]) + ". Usage: .name <current name in " + _n->lang() + "> <language identifier> <name in that language>");
        _n->set_name(_n->node(cmd[1]), cmd[3], network::utils::str(cmd[2]));
    }
    else if (cmd[0] == L".node")
    {
        if (cmd.size() == 1) throw std::runtime_error("Command .node: Missing node name or ID");
        network::Node nd = _n->get_node(cmd[1]);
        if (nd == 0) {
            try {
                size_t pos = 0;
                nd = std::stoull(cmd[1], &pos);
                if (pos != cmd[1].length()) {
                    throw std::runtime_error("Command .node: Invalid node ID format '" + network::utils::str(cmd[1]) + "'");
                }
            } catch (const std::exception&) {
                throw std::runtime_error("Command .node: Unknown node '" + network::utils::str(cmd[1]) + "'");
            }
        }
        else
        {
            std::clog << "ID of node: " << nd << std::endl;
        }
        for (const auto& lang : _n->get_languages()) {
            std::clog << "Name of node in language '" << lang << "': '"
                      << network::utils::str(_n->get_name(nd, lang, false)) << "'" << std::endl;
        }
    }
    else if (cmd[0] == L".nodes")
    {
        if (cmd.size() == 1) throw std::runtime_error("Command .nodes: Missing count parameter");

        size_t count;
        try {
            size_t pos = 0;
            count = std::stoull(cmd[1], &pos);
            if (pos != cmd[1].length()) {
                throw std::runtime_error("Command .nodes: Invalid count format '" + network::utils::str(cmd[1]) + "'");
            }
        } catch (const std::exception&) {
            throw std::runtime_error("Command .nodes: Invalid count '" + network::utils::str(cmd[1]) + "'");
        }

        if (count <= 0) throw std::runtime_error("Command .nodes: Count must be greater than 0");

        std::clog << "Listing first " << count << " nodes:" << std::endl;
        std::clog << "------------------------" << std::endl;

        size_t displayed = 0;
        for (const auto& node : _n->get_all_nodes()) {
            std::clog << "Node ID: " << node << std::endl;

            for (const auto& lang : _n->get_languages()) {
                std::clog << "  Name in language '" << lang << "': '"
                          << network::utils::str(_n->get_name(node, lang, false)) << "'" << std::endl;
            }

            std::clog << "------------------------" << std::endl;

            displayed++;
            if (displayed >= count) break;
        }

        std::clog << "Displayed " << displayed << " of " << count << " requested nodes." << std::endl;
    }
    else if (cmd[0] == L".dot")
    {
        if (cmd.size() == 1) throw std::runtime_error("Command .dot: Missing node name to visualize");
        network::Node nd = _n->get_node(cmd[1]);
        if (nd == 0) throw std::runtime_error("Command .dot: Unknown node '" + network::utils::str(cmd[1]) + "'");
        if (cmd.size() < 3) throw std::runtime_error("Command .dot: Missing maximum depth");
        int max_depth = std::stoi(network::utils::str(cmd[2]));
        if (max_depth < 2) throw std::runtime_error("Command .dot: Maximum depth must be greater than 1");
        _n->gen_dot(nd, network::utils::str(cmd[1]) + ".dot", max_depth);
    }
    else if (cmd[0] == L".run")
    {
        _n->run(true, false);
        _n->print(L"> Ready.", true);
    }
    else if (cmd[0] == L".run-md")
    {
        network::StopWatch watch;
        _n->run(true, true);
        _n->print(L" Time needed: " + std::to_wstring(static_cast<double>(watch.duration()) / 1000) + L"s", true);
    }
    else if (cmd[0] == L".wikidata")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .wikidata: Missing json file name");

        std::ofstream log("wikidata.log");
        _n->set_print([&](const std::wstring& str, bool o)
                      {
          log << network::utils::str(str) << std::endl;

          if (o)
          {
            std::wcout << str << std::endl;
          } });

        if (cmd.size() == 2)
        {
            network::StopWatch watch;
            watch.start();
            // _wikidata = std::make_shared<Wikidata>(_n, cmd[1]);
            // _wikidata->import_all();

            _wikidata = std::make_shared<Wikidata>(_n, cmd[1]);
            _wikidata->generate_index();
            _wikidata->traverse();
            _n->print(L" Time needed for importing: " + std::to_wstring(static_cast<double>(watch.duration()) / 1000) + L"s", true);
        }
        else if (cmd.size() == 3)
        {
            const std::wstring& start_entry = cmd[2];
            _n->print(L"start entry='" + start_entry + L"'", true);
            _wikidata = std::make_shared<Wikidata>(_n, cmd[1]);
            _wikidata->generate_index();
            _wikidata->traverse(start_entry);
        }
        else
        {
            throw std::runtime_error("Command .wikidata: You need to specify the json file to import and optionally add the maximum link distance and the start entry to be imported");
        }
    }
    else
    {
        throw std::runtime_error(network::utils::str(L"Unknown command " + cmd[0]));
    }
}

void console::Interactive::Impl::process_token(std::vector<std::wstring>& tokens, bool& is_rule, const std::wstring& first_var, std::wstring& assigns_to_var, const std::wstring& token, const std::wstring& And, const std::wstring& Causes) const
{
    if (!token.empty())
    {
        std::wstring current;

        if (boost::algorithm::starts_with(token, And)) // we allow the "and" symbol - usually a comma - to be connected with the following token
        {
            current = token.substr(And.size());
            tokens.push_back(And);
            tokens.push_back(current);
        }

        if (boost::algorithm::ends_with(token, And)) // we allow the "and" symbol - usually a comma - to be connected with the preceding token
        {
            if (current.empty())
            {
                current = token.substr(0, token.size() - And.size());
                tokens.push_back(current);
            }
            tokens.push_back(And);
        }

        if (current.empty())
        {
            current = token;
            tokens.push_back(current);
        }

        if (current == Causes)
        {
            if (is_rule)
                throw std::runtime_error("Line contains two times " + network::utils::str(_n->get_name(_n->core.Causes))); // this is just a parser restriction to distinguish between condition and deduction, might be useful to support it
            else
                is_rule = true;
        }
        else if (tokens.size() == 2 && token == L"=" && !first_var.empty())
        {
            assigns_to_var = first_var;
            tokens.clear();
        }
    }
}

network::Node console::Interactive::Impl::process_fact(const std::vector<std::wstring>& tokens, boost::bimap<std::wstring, network::Node>& variables)
{
    class C
    {
        Interactive::Impl* _enclosing{nullptr};

    public:
        C(Interactive::Impl* enclosing, network::Node node = 0, const std::wstring& token = L"")
            : _enclosing(enclosing)
            , _node(node)
            , _token(token)
        {
        }
        network::Node _node{0};
        std::wstring  _token;

        static C grab(const std::vector<C>& list, size_t& i, const size_t max_index)
        {
            if (list[i]._node) return list[i++];
            C result(list[0]._enclosing);
            while (i <= max_index && !list[i]._node)
            {
                if (!result._token.empty()) result._token += L" ";

                if (list[i]._token.find(L' ') != std::wstring::npos)
                {
                    if (result._token.empty())
                    {
                        result._token += list[i++]._token;
                        break;
                    }
                    else
                    {
                        throw std::runtime_error("Could not parse this line");
                    }
                }
                else
                {
                    result._token += list[i++]._token;
                }
            }
            return result;
        }

        operator network::Node()
        {
            if (_node)
                return _node;
            else
            {
                if (_token.empty())
                    throw std::logic_error("Internal error: empty token in process_fact::C");
                else
                    return _node = _enclosing->_n->node(_token, _enclosing->_n->lang());
            }
        }
    };

    if (tokens.size() == 1)
    {
        if (*tokens.begin() == _n->get_name(_n->core.Contradiction))
        {
            return _n->core.Contradiction;
        }
        else
        {
            throw std::runtime_error(network::utils::str(L"Fact '" + *tokens.begin() + L"' consists of only 1 token, which is only allowed for contradiction '" + _n->get_name(_n->core.Contradiction) + L"'"));
        }
    }

    std::vector<C> combined;
    bool           done_all = true;

    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const std::wstring& token = tokens[i];

        if (is_var(token))
        {
            auto var = variables.left.find(token);
            if (var == variables.left.end())
            {
                network::Node node = _n->var();
                _n->set_name(node, token, _n->lang());
                variables.left.insert(boost::bimap<std::wstring, network::Node>::left_value_type(token, node));

                if (variables.left.find(token) == variables.left.end())
                {
                    throw std::runtime_error(network::utils::str(L"Variable " + token + L" has been set to a different node."));
                }

                combined.emplace_back(this, node, token);
            }
            else
            {
                combined.emplace_back(this, var->second, token);
            }
        }
        else if (token.find(L' ') != std::wstring::npos)
        {
            network::Node node = _n->get_node(token, _n->lang());
            combined.emplace_back(this, node, token);
        }
        else
        {
            // Find the longest sequence of words that makes sense to start searching a sequence that matches a relation name.
            // We start at the longest to prefer longer word combinations over shorter, e.g. "is a" is found before "is".
            size_t max_len = tokens.size() - i;
            if (combined.empty())
                max_len -= 2;
            else                                     // we are at the object and will need predicate and subject, so reduce by 2
                if (combined.size() == 1) --max_len; // we are at the predicate and will need the subject, so reduce by 1
            size_t len = 1;
            for (; len <= max_len && !is_var(tokens[i + len - 1]); ++len)
                ; // if the first is a variable then len=1
            --len;

            bool done = false;
            for (; len >= 1; --len)
            {
                std::vector<std::wstring> test;
                test.insert(test.end(), tokens.begin() + static_cast<long>(i), tokens.begin() + static_cast<long>(i) + static_cast<long>(len));
                std::wstring  connected = network::utils::concatenate(test, L' ');
                network::Node node      = _n->get_node(connected, _n->lang());
                if (node)
                {
                    combined.emplace_back(this, node, connected);
                    done = true;
                    break;
                }
            }

            if (done)
            {
                i += len - 1;
            }
            else
            {
                combined.emplace_back(this, 0, token);
                done_all = false;
            }
        }
    }

    if (combined.size() < 3) throw std::runtime_error("A fact must consist of at least 3 tokens.");

    if (!done_all && combined.size() > 3)
    {
        // combined is longer than 3 elements, so aggregate it to get subject, predicate, object
        decltype(combined) temp;
        size_t             i = 0;
        temp.emplace_back(C::grab(combined, i, combined.size() - 3));
        temp.emplace_back(C::grab(combined, i, combined.size() - 2));
        temp.emplace_back(C::grab(combined, i, combined.size() - 1));

        if (i != combined.size())
        {
            throw std::runtime_error(network::utils::str(L"cannot match fact or condition (use quotation marks?): " + temp[0]._token + L"  " + temp[1]._token + L"  " + temp[2]._token + L" ..."));
        }

        combined = std::move(temp);
    }

    std::unordered_set<network::Node> objects;
    for (size_t i = 2; i < combined.size(); ++i)
        objects.insert(combined[i]);

    return _n->fact(combined[0], combined[1], objects);
}

network::Node console::Interactive::Impl::process_rule(const std::vector<std::wstring>& tokens, const std::wstring& line, boost::bimaps::bimap<std::wstring, zelph::network::Node>& variables, const std::wstring& And, const std::wstring& Causes)
{
    std::vector<std::vector<std::wstring>> conditions, deductions;
    bool                                   is_deduction = false;

    for (size_t i = 0; i < tokens.size(); ++i)
    {
        std::vector<std::wstring> tokens_for_fact;
        while (i < tokens.size() && tokens[i] != And && tokens[i] != Causes)
            tokens_for_fact.push_back(tokens[i++]);

        if (tokens_for_fact.empty())
        {
            throw std::runtime_error("Found empty condition or deduction in " + network::utils::str(line));
        }

        if (is_deduction)
            deductions.emplace_back(tokens_for_fact);
        else
            conditions.emplace_back(tokens_for_fact);

        if (i < tokens.size() && tokens[i] == Causes) is_deduction = true;
    }

    if (conditions.empty()) throw std::runtime_error("Found rule without condition in " + network::utils::str(line));
    if (deductions.empty()) throw std::runtime_error("Found rule without condition in " + network::utils::str(line));

    std::unordered_set<network::Node> condition_nodes;
    for (const auto& condition : conditions)
    {
        condition_nodes.insert(process_fact(condition, variables));
    }

    const network::Node combined_condition = conditions.size() == 1
                                               ? *condition_nodes.begin()
                                               : _n->condition(_n->core.And, condition_nodes);

    std::unordered_set<network::Node> deduction_list;
    for (const auto& deduction : deductions)
    {
        deduction_list.insert(process_fact(deduction, variables));
    }

    return _n->fact(combined_condition, _n->core.Causes, deduction_list);
}

void console::Interactive::run(const bool print_deductions, const bool generate_markdown) const
{
    _pImpl->_n->run(print_deductions, generate_markdown);
}

bool console::Interactive::Impl::is_var(std::wstring token)
{
    static const std::wstring variable_names(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ_");

    switch (token.size())
    {
    case 0:
        return false;
    case 1:
        return variable_names.find(*token.begin()) != std::wstring::npos;
    default:
        return *token.begin() == L'_';
    }
}

#ifdef PROVIDE_C_INTERFACE
Interactive interactive;

extern "C" void zelph_process_c(const char* line, size_t len)
{
    if (len > 0)
    {
        std::string  l(line, 0, len);
        std::wstring wline(l.begin(), l.end());
        interactive.process(wline);
    }
}

extern "C" void zelph_run()
{
    interactive.run(true, false);
}
#endif
