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

#include "interactive.hpp"

#include "network.hpp"
#include "reasoning.hpp"
#include "stopwatch.hpp"
#include "string_utils.hpp"
#include "wikidata.hpp"
#include "wikidata_text_compressor.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/bimap.hpp>
#include <boost/tokenizer.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>

#ifdef _WIN32
    #include <fcntl.h> // for _O_U16TEXT
    #include <io.h>    // for _setmode
    #include <stdio.h> // for _fileno
#endif

using namespace zelph;
using boost::escaped_list_separator;
using boost::tokenizer;

class console::Interactive::Impl
{
public:
    Impl(Interactive* enclosing)
        : _n(new network::Reasoning(_core_node_names,
                                    [](const std::wstring& str, const bool)
                                    {
#ifdef _WIN32
                                        std::wcout << str << std::endl;
#else
                                        std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
                                    }))
        , _interactive(enclosing)
    {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_U16TEXT);
#endif

        _n->set_lang("zelph");

        _core_node_names[_n->core.RelationTypeCategory] = L"->";
        _core_node_names[_n->core.Causes]               = L"=>";
        _core_node_names[_n->core.And]                  = L",";
        _core_node_names[_n->core.IsA]                  = L"~";
        _core_node_names[_n->core.Unequal]              = L"!=";
        _core_node_names[_n->core.Contradiction]        = L"!";
    }

    void                       import_file(const std::wstring& file) const;
    void                       process_command(const std::vector<std::wstring>& cmd);
    void                       display_node_details(network::Node nd, bool resolved_from_name = false) const;
    network::Node              process_fact(const std::vector<std::wstring>& tokens, boost::bimap<std::wstring, network::Node>& variables);
    network::Node              process_rule(const std::vector<std::wstring>& tokens, const std::wstring& line, boost::bimaps::bimap<std::wstring, zelph::network::Node>& variables, const std::wstring& And, const std::wstring& Causes);
    void                       process_token(std::vector<std::wstring>& tokens, bool& is_rule, const std::wstring& first_var, std::wstring& assigns_to_var, const std::wstring& token, const std::wstring& And, const std::wstring& Causes) const;
    static bool                is_var(std::wstring token);
    void                       list_predicate_usage(size_t limit);
    void                       list_predicate_value_usage(const std::wstring& pred_arg, size_t limit);
    network::Node              resolve_node(const std::wstring& arg) const;
    network::Node              resolve_single_node(const std::wstring& arg, bool prioritize_id = false) const;
    std::vector<network::Node> resolve_nodes_by_name(const std::wstring& name) const;

    std::shared_ptr<Wikidata>                       _wikidata;
    std::unordered_map<network::Node, std::wstring> _core_node_names;
    network::Reasoning* const                       _n;

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

private:
    const Interactive* _interactive;
};

console::Interactive::Interactive()
    : _pImpl(new Impl(this))
{
}

console::Interactive::~Interactive()
{
    delete _pImpl;
}

void console::Interactive::Impl::import_file(const std::wstring& file) const
{
    std::clog << "Importing file " << string::unicode::to_utf8(file) << "..." << std::endl;
    std::wifstream stream(string::unicode::to_utf8(file));

    if (stream.fail()) throw std::runtime_error("Could not open file '" + string::unicode::to_utf8(file) + "'");

    for (std::wstring line; std::getline(stream, line);)
    {
        _interactive->process(line);
    }
}

std::string console::Interactive::get_version()
{
    return network::Zelph::get_version();
}

void console::Interactive::process(std::wstring line) const
{
    _pImpl->_n->set_print([](const std::wstring& str, bool)
                          {
#ifdef _WIN32
                              std::wcout << str << std::endl;
#else
                              std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
                          });

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
            const std::wstring        And     = _pImpl->_core_node_names.at(_pImpl->_n->core.And);
            const std::wstring        Causes  = _pImpl->_core_node_names.at(_pImpl->_n->core.Causes);
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
            _pImpl->_n->format_fact(output, _pImpl->_n->lang(), fact);
            _pImpl->_n->print(output, false);

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
                    _pImpl->_n->apply_rule(0, fact); // query
                }
            }
        }
    }
    catch (std::exception& ex)
    {
        throw std::runtime_error("Error in line \"" + string::unicode::to_utf8(line) + "\": " + ex.what());
    }
}

void console::Interactive::import_file(const std::wstring& file) const
{
    _pImpl->import_file(file);
}

void console::Interactive::Impl::display_node_details(network::Node nd, bool resolved_from_name /*= false*/) const
{
    if (resolved_from_name)
    {
        std::clog << "Resolved to node ID: " << nd << std::endl;
    }

    std::clog << "Node ID: " << nd << std::endl;

    {
        auto it = _core_node_names.find(nd);
        if (it != _core_node_names.end())
        {
            std::clog << "  Core node: " << string::unicode::to_utf8(it->second) << std::endl;
        }
    }

    if (network::Network::is_var(nd))
    {
        std::clog << "  Variable: yes" << std::endl;
    }
    else
    {
        std::clog << "  Variable: no" << std::endl;
    }

    bool         has_wikidata = false;
    std::wstring wikidata_name;

    bool has_any_name = false;
    for (const std::string& lang : _n->get_languages())
    {
        std::wstring name = _n->get_name(nd, lang, false);
        if (!name.empty())
        {
            has_any_name = true;
            std::clog << "  Name in language '" << lang << "': '"
                      << string::unicode::to_utf8(name) << "'" << std::endl;

            if (lang == "wikidata")
            {
                has_wikidata  = true;
                wikidata_name = name;
            }
        }
    }

    if (!has_any_name)
    {
        std::clog << "  (No names in any language)" << std::endl;
    }

    if (has_wikidata)
    {
        std::string prefix = (wikidata_name[0] == L'P') ? "Property:" : "";
        std::string url    = "https://www.wikidata.org/wiki/" + prefix + string::unicode::to_utf8(wikidata_name);

        const std::string OSC_START = "\033]8;;";
        const char        OSC_SEP   = '\a';
        const std::string OSC_END   = "\033]8;;\a";

        std::clog << "  Wikidata URL: " << OSC_START << url << OSC_SEP << url << OSC_END << std::endl;
    }

    auto format_node = [this](network::Node node) -> std::string
    {
        std::wstring node_str  = std::to_wstring(node);
        std::wstring node_name = _n->get_name(node, _n->lang(), true); // fallback active

        if (node_str == node_name || node_name.empty())
        {
            std::wstring fact_repr;
            _n->format_fact(fact_repr, _n->lang(), node);
            if (!fact_repr.empty() && fact_repr != L"??")
            {
                return string::unicode::to_utf8(fact_repr) + " (ID " + std::to_string(node) + ")";
            }
            else
            {
                return "ID " + std::to_string(node);
            }
        }
        else
        {
            return string::unicode::to_utf8(node_name) + " (ID " + std::to_string(node) + ")";
        }
    };

    auto display_connections = [&](const network::adjacency_set& conns, const std::string& header)
    {
        if (conns.empty())
        {
            return;
        }

        std::clog << "  " << header << ":" << std::endl;

        if (conns.size() <= 3)
        {
            for (network::Node node : conns)
            {
                std::clog << "    - " << format_node(node) << std::endl;
            }
        }
        else
        {
            std::clog << "    (" << conns.size() << " connections)" << std::endl;
        }
    };

    network::adjacency_set incoming = _n->get_left(nd);
    network::adjacency_set outgoing = _n->get_right(nd);

    display_connections(incoming, "Incoming connections from");
    display_connections(outgoing, "Outgoing connections to");

    std::wstring fact_repr;
    _n->format_fact(fact_repr, _n->lang(), nd);
    if (!fact_repr.empty() && fact_repr != L"??")
    {
        std::clog << "  Representation: " << string::unicode::to_utf8(fact_repr) << std::endl;
    }

    std::clog << "------------------------" << std::endl;
}

void console::Interactive::Impl::process_command(const std::vector<std::wstring>& cmd)
{
    static const std::vector<std::wstring> general_help_lines = {
        L"zelph Interactive Help",
        L"",
        L"Basic Syntax",
        L"────────────",
        L"Facts:    <subject> <predicate> <object>",
        L"          Predicates with spaces must be quoted on first use.",
        L"          Example: peter \"is father of\" paul",
        L"          → «peter» «is father of» «paul»",
        L"          Subsequent use: peter is father of paul",
        L"          → «peter» «is father of» «paul»",
        L"",
        L"Rules:    <condition1>, <condition2>, ... => <deduction1>, <deduction2>, ...",
        L"          Rules are stored but not automatically applied.",
        L"          Use .run to perform inference and see deductions.",
        L"",
        L"Queries:  Statements containing variables (A-Z or starting with _).",
        L"          Queries are answered immediately (no .run needed).",
        L"          Example: A is father of paul",
        L"          → Answer: «peter» «is father of» «paul»",
        L"",
        L"Examples",
        L"Berlin \"is capital of\" Germany",
        L"Germany \"is located in\" Europe",
        L"X is capital of Y, Y is located in Z => X is located in Z",
        L".run",
        L"→ «Berlin» «is located in» «Europe» ⇐ («Germany» «is located in» «Europe»), («Berlin» «is capital of» «Germany»)",
        L"",
        L"Available Commands",
        L"──────────────────",
        L".help [command]             – Show this help or detailed help for a specific command",
        L".exit                       – Exit interactive mode",
        L".lang [code]                – Show or set current language",
        L".name <node|id> <new_name>         – Set name in current language",
        L".name <node|id> <lang> <new_name>  – Set name in specific language",
        L".delname <node|id> [lang]          – Delete name in current language (or specified language)",
        L".node <name|id>                    – Show detailed node information (names, connections, representation, Wikidata URL)",
        L".list <count>                      – List first N existing nodes (internal map order, with details)",
        L".clist <count>                     – List first N nodes named in current language (sorted by ID if reasonable size, otherwise map order)",
        L".out <name|id> [count]             – List details of outgoing connected nodes (default 20)",
        L".in <name|id> [count]              – List details of incoming connected nodes (default 20)",
        L".display <name> <depth>     – Generate and opens SVG file for a node",
        L".run                        – Run full inference",
        L".run-once                   – Run a single inference pass",
        L".run-md <subdir>            – Run inference and export results as Markdown",
        L".run-file <file>            – Run inference, write deduced facts (reversed order) to <file> (encoded if lang=wikidata)",
        L".decode <file>              – Decode an encoded/plain file and print readable facts",
        L".list-rules                 – List all defined inference rules",
        L".list-predicate-usage [max] – Show predicate usage statistics (top N most frequent predicates)",
        L".list-predicate-value-usage <pred> [max] – Show object/value usage statistics for a specific predicate (top N most frequent values)",
        L".remove-rules               – Remove all inference rules",
        L".remove <name|id>           – Remove a node (destructive: disconnects all edges and cleans names)",
        L".import <file.zph>          – Load and execute a zelph script file",
        L".load <file>                – Load a saved network (.bin) or import Wikidata JSON dump (creates .bin cache)",
        L".save <file.bin>            – Save the current network to a binary file",
        L".prune-facts <pattern>      – Remove all facts matching the query pattern (only statements)",
        L".prune-nodes <pattern>      – Remove matching facts AND all involved subject/object nodes",
        L".cleanup                    – Remove isolated nodes and clean name mappings",
        L".wikidata-index <json>      – Generate index only (for faster future loads)",
        L".wikidata-export <wid>      – Export a single Wikidata entry as JSON",
        L".wikidata-constraints <json> <dir> – Export constraints to a directory",
        L"",
        L"Type \".help <command>\" for detailed information about a specific command."};

    static const std::map<std::wstring, std::wstring> detailed_help = {
        {L".help", L".help [command]\n"
                   L"Without argument: shows this general help text with syntax and command overview.\n"
                   L"With argument: shows detailed help for the specified command."},

        {L".exit", L".exit\n"
                   L"Exits the interactive REPL session."},

        {L".lang", L".lang [language_code]\n"
                   L"Without argument: displays the current language used for node names.\n"
                   L"With argument: sets the language (e.g., 'zelph', 'en', 'de', 'wikidata')."},

        {L".name", L".name <node|id> <new_name>\n"
                   L"Sets the name of the node in the current language.\n"
                   L".name <node|id> <lang> <new_name>\n"
                   L"Sets the name in the specified language.\n"
                   L"The <node|id> can be a name (in current language) or numeric node ID.\n"
                   L"Empty <new_name> is not allowed – use .delname to remove a name."},

        {L".delname", L".delname <node|id> [lang]\n"
                      L"Removes the name of the node in the current language (or the specified language if provided).\n"
                      L"The <node|id> can be a name (in current language) or numeric node ID.\n"
                      L"If the node had no name in the target language, nothing happens."},

        {L".list", L".list <count>\n"
                   L"Lists the first N existing nodes in the network (in internal map iteration order).\n"
                   L"For each node: ID, non-empty names in all languages, connection counts, representation, and Wikidata URL if available."},

        {L".clist", L".clist <count>\n"
                    L"Lists the first N nodes that have a name in the current language.\n"
                    L"If the language has a reasonable number of entries (≤ ~50k), nodes are sorted by ID.\n"
                    L"For very large languages (e.g. 'wikidata'), order follows the internal map (fast, no full sort)."},

        {L".out", L".out <name|id> [count]\n"
                  L"Lists detailed information for up to <count> nodes reachable via outgoing connections\n"
                  L"from the given node (default 20, sorted by node ID)."},

        {L".in", L".in <name|id> [count]\n"
                 L"Lists detailed information for up to <count> nodes that have outgoing connections\n"
                 L"to the given node (default 20, sorted by node ID)."},

        {L".node", L".node <name_or_id>\n"
                   L"Displays details for a single node: its ID, non-empty names in all languages,\n"
                   L"incoming/outgoing connection counts, and a clickable Wikidata URL if it has a Wikidata ID.\n"
                   L"The argument can be a name (in current language) or a numeric node ID."},

        {L".nodes", L".nodes <count>\n"
                    L"Lists the first N named nodes (nodes that have at least one name in any language),\n"
                    L"sorted by node ID. For each node: ID, non-empty names in all languages,\n"
                    L"incoming/outgoing connection counts, and Wikidata URL if available."},

        {L".clist", L".clist <count>\n"
                    L"Lists the first N nodes that have a name in the current language, sorted by node ID.\n"
                    L"Output format is identical to .list (names in all languages, connection counts, Wikidata URL)."},

        {L".display", L".display <node_name> <max_depth>\n"
                      L"Generates and opens an svg file visualizing the specified node and its connections\n"
                      L"up to the given depth (depth ≥ 2 recommended). The file is named <node_name>.svg."},

        {L".run", L".run\n"
                  L"Performs full inference: repeatedly applies all rules until no new facts are derived.\n"
                  L"Deductions are printed as they are found."},

        {L".run-once", L".run-once\n"
                       L"Performs a single inference pass."},

        {L".run-md", L".run-md <subdir>\n"
                     L"Runs full inference and exports all deductions and contradictions as Markdown files\n"
                     L"in the directory mkdocs/docs/<subdir> for use with MkDocs."},

        {L".run-file", L".run-file <file>\n"
                       L"Performs full inference. Deduced facts (positive conclusions and contradictions) are written to <file>\n"
                       L"in reversed order (reasons first, then ⇒ conclusion), without any brackets or markup.\n"
                       L"Console output remains unchanged (original order with ⇐ explanations).\n"
                       L"If the current language is 'wikidata' (set via .lang wikidata), Wikidata identifiers are heavily\n"
                       L"compressed for minimal file size. Otherwise the file contains plain readable text."},

        {L".decode", L".decode <file>\n"
                     L"Reads a file created by .run-file (encoded or plain) and prints the decoded facts\n"
                     L"in readable form to standard output."},

        {L".list-rules", L".list-rules\n"
                         L"Lists all currently defined inference rules in readable format."},

        {L".list-predicate-usage", L".list-predicate-usage [max_entries]\n"
                                   L"Shows how often each predicate (relation type) is used, sorted by frequency.\n"
                                   L"If <max_entries> is specified, only the top N most frequent predicates are shown.\n"
                                   L"If Wikidata language is active, Wikidata IDs are shown alongside names."},

        {L".list-predicate-value-usage", L".list-predicate-value-usage <predicate> [max_entries]\n"
                                         L"Shows how often each object (value) is used with the specified predicate, sorted by frequency.\n"
                                         L"The <predicate> can be a name (in the current language) or a numeric node ID.\n"
                                         L"If <max_entries> is specified, only the top N most frequent values are shown.\n"
                                         L"If the Wikidata language is available and active, Wikidata IDs are shown alongside names."},

        {L".remove-rules", L".remove-rules\n"
                           L"Deletes all inference rules from the network."},

        {L".remove", L".remove <name_or_id>\n"
                     L"Removes the specified node from the network, disconnecting all its edges\n"
                     L"and cleaning all name mappings. The argument can be a node name (looked up in the current language)\n"
                     L"or a numeric node ID.\n"
                     L"WARNING: This operation is destructive and irreversible!"},

        {L".import", L".import <file.zph>\n"
                     L"Loads and immediately executes a zelph script file."},

        {L".load", L".load <file>\n"
                   L"Loads a previously saved network state.\n"
                   L"- If <file> ends with '.bin': loads the serialized network directly (fast).\n"
                   L"- If <file> ends with '.json' (Wikidata dump): imports the data and automatically creates a '.bin' cache file\n"
                   L"  in the same directory for faster future loads."},

        {L".save", L".save <file.bin>\n"
                   L"Saves the current network state to a binary file.\n"
                   L"The filename must end with '.bin'."},

        {L".prune-facts", L".prune-facts <pattern>\n"
                          L"Removes only the matching facts (statement nodes).\n"
                          L"The pattern may contain variables in any position.\n"
                          L"Reports how many facts were removed."},

        {L".prune-nodes", L".prune-nodes <pattern>\n"
                          L"Removes all matching facts AND all nodes that appear as subject or object in these facts.\n"
                          L"Requirements:\n"
                          L"- The relation (predicate) must be fixed (no variable allowed in predicate position)\n"
                          L"- Variables are allowed in subject and/or object positions\n"
                          L"WARNING: This is highly destructive! It removes ALL connections of the affected nodes.\n"
                          L"The relation node itself becomes isolated and can be removed with .cleanup.\n"
                          L"Reports removed facts and nodes."},

        {L".cleanup", L".cleanup\n"
                      L"Removes all nodes that have no connections (isolated nodes).\n"
                      L"Also cleans up associated entries in name mappings."},

        {L".wikidata-index", L".wikidata-index <json_file>\n"
                             L"Only generates the index file for the specified Wikidata dump"},

        {L".wikidata-export", L".wikidata-export <wikidata_id>\n"
                              L"Exports a single Wikidata entry (e.g., Q42 or P31) as a JSON file.\n"
                              L"Requires that an index has been generated previously."},

        {L".wikidata-constraints", L".wikidata-constraints <json_file> <output_dir>\n"
                                   L"Processes the Wikidata dump and exports constraint scripts\n"
                                   L"to the specified output directory."}};

    if (cmd[0] == L".help")
    {
        if (cmd.size() == 1)
        {
            for (const auto& l : general_help_lines)
                _n->print(l, true);
        }
        else if (cmd.size() == 2)
        {
            auto it = detailed_help.find(cmd[1]);
            if (it != detailed_help.end())
            {
                _n->print(it->second, true);
            }
            else
            {
                _n->print(L"Unknown command: " + cmd[1] + L". Use \".help\" for a list of all commands.", true);
            }
        }
        else
        {
            throw std::runtime_error("Usage: .help [command]");
        }
        return;
    }
    else if (cmd[0] == L".lang")
    {
        if (cmd.size() < 2)
        {
            std::clog << "The current language is '" << _n->get_lang() << "'" << std::endl;
        }
        else
        {
            _n->set_lang(string::unicode::to_utf8(cmd[1]));
        }
    }
    else if (cmd[0] == L".name")
    {
        if (cmd.size() < 3 || cmd.size() > 4)
            throw std::runtime_error("Command .name: Invalid arguments. Usage: .name <node> <new_name>  or  .name <node> <lang> <new_name>");

        network::Node nd = resolve_node(cmd[1]);

        if (cmd.size() == 3)
        {
            // .name <node> <new_name> → current language
            const std::wstring& new_name = cmd[2];
            if (new_name.empty())
                throw std::runtime_error("Command .name: New name cannot be empty. Use .delname to remove a name.");

            _n->set_name(nd, new_name, _n->lang());
        }
        else // size == 4
        {
            // .name <node> <lang> <new_name>
            std::string         target_lang = string::unicode::to_utf8(cmd[2]);
            const std::wstring& new_name    = cmd[3];
            if (new_name.empty())
                throw std::runtime_error("Command .name: New name cannot be empty. Use .delname to remove a name.");

            _n->set_name(nd, new_name, target_lang);
        }
    }
    else if (cmd[0] == L".delname")
    {
        if (cmd.size() < 2 || cmd.size() > 3)
            throw std::runtime_error("Command .delname: Invalid arguments. Usage: .delname <node|id> [lang]");

        network::Node nd = resolve_single_node(cmd[1], true); // prioritize ID

        std::string target_lang = _n->lang();
        if (cmd.size() == 3)
        {
            target_lang = string::unicode::to_utf8(cmd[2]);
        }

        _n->remove_name(nd, target_lang);

        _n->print(L"Removed name of node " + std::to_wstring(nd) + L" in language '" + string::unicode::from_utf8(target_lang) + L"' (if it existed).", true);
    }
    else if (cmd[0] == L".node")
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .node: Exactly one argument required");

        std::wstring arg = cmd[1];

        std::vector<network::Node> nodes;

        try
        {
            // Try single resolve (non-destructive: ID last)
            network::Node single = resolve_single_node(arg, false);
            nodes.push_back(single);
        }
        catch (...)
        {
            // Not a single node/ID → try name search (multiple possible)
            nodes = resolve_nodes_by_name(arg);
            if (nodes.empty())
            {
                throw std::runtime_error("No node found with name '" + string::unicode::to_utf8(arg) + "' in current language '" + _n->lang() + "'");
            }
        }

        if (nodes.size() == 1)
        {
            bool resolved_from_name = !_n->get_name(nodes[0], _n->lang(), false).empty() || std::all_of(arg.begin(), arg.end(), ::iswdigit);
            display_node_details(nodes[0], resolved_from_name && nodes.size() == 1);
        }
        else
        {
            std::clog << "Found " << nodes.size() << " nodes with name '" << string::unicode::to_utf8(arg)
                      << "' in current language '" << _n->lang() << "':" << std::endl;
            std::clog << "------------------------" << std::endl;

            // Sort by ID for consistent output
            std::sort(nodes.begin(), nodes.end());

            for (network::Node nd : nodes)
            {
                display_node_details(nd, true);
            }
        }
    }
    else if (cmd[0] == L".list")
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .list: Missing count parameter");

        size_t count = string::parse_count(cmd[1]);

        auto view = _n->get_all_nodes_view();

        std::clog << "Listing " << count << " nodes:" << std::endl;
        std::clog << "------------------------" << std::endl;

        size_t displayed = 0;
        for (auto it = view.begin(); it != view.end() && displayed < count; ++it, ++displayed)
        {
            display_node_details(it->first);
        }

        std::clog << "Displayed " << displayed << " nodes." << std::endl;
    }

    else if (cmd[0] == L".clist")
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .clist: Missing count parameter");

        size_t count = string::parse_count(cmd[1]);

        auto view = _n->get_lang_nodes_view(_n->lang());

        std::clog << "Listing first " << count << " nodes named in current language '" << _n->lang() << "'" << std::endl;
        std::clog << "------------------------" << std::endl;

        size_t displayed = 0;
        for (auto it = view.begin(); it != view.end() && displayed < count; ++it, ++displayed)
        {
            display_node_details(it->second);
        }
    }
    else if (cmd[0] == L".out" || cmd[0] == L".in")
    {
        bool outgoing = (cmd[0] == L".out");

        if (cmd.size() < 2) throw std::runtime_error(std::string("Command ") + string::unicode::to_utf8(cmd[0]) + ": Missing node argument");

        std::wstring  arg     = cmd[1];
        network::Node base_nd = resolve_node(arg); // same resolve logic as .node/.remove

        size_t max_count = 20; // default
        if (cmd.size() >= 3)
        {
            max_count = string::parse_count(cmd[2]);
        }

        network::adjacency_set neighbors = outgoing ? _n->get_right(base_nd) : _n->get_left(base_nd);

        std::vector<network::Node> vec(neighbors.begin(), neighbors.end());
        std::sort(vec.begin(), vec.end());

        size_t to_display = std::min(max_count, vec.size());

        std::clog << (outgoing ? "Outgoing" : "Incoming")
                  << " connected nodes of " << base_nd
                  << " (first " << to_display << " of " << vec.size() << ", sorted by ID):" << std::endl;
        std::clog << "------------------------" << std::endl;

        for (size_t i = 0; i < to_display; ++i)
        {
            display_node_details(vec[i]);
        }
    }
    else if (cmd[0] == L".remove")
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .remove requires exactly one argument: name or ID");

        std::wstring  arg = cmd[1];
        network::Node nd  = resolve_single_node(arg, true); // prioritize ID

        if (nd == 0)
        {
            try
            {
                size_t pos = 0;
                nd         = std::stoull(arg, &pos);
                if (pos != arg.length())
                {
                    throw std::exception();
                }
            }
            catch (const std::exception&)
            {
                throw std::runtime_error("Command .remove: Unknown node '" + string::unicode::to_utf8(arg) + "' in current language '" + _n->lang() + "'");
            }

            if (!_n->exists(nd))
            {
                throw std::runtime_error("Command .remove: Node '" + std::to_string(nd) + "' does not exist");
            }
        }

        _n->remove_node(nd);
        _n->print(L"Removed node " + std::to_wstring(nd) + L" (all edges disconnected, name mappings cleaned).", true);
        _n->print(L"Consider running .cleanup afterwards if needed.", true);
    }
    else if (cmd[0] == L".display")
    {
        if (cmd.size() == 1) throw std::runtime_error("Command .display: Missing node name to visualize");
        network::Node nd = _n->get_node(cmd[1]);
        if (nd == 0) throw std::runtime_error("Command .display: Unknown node '" + string::unicode::to_utf8(cmd[1]) + "'");
        if (cmd.size() < 3) throw std::runtime_error("Command .display: Missing maximum depth");
        int max_depth = std::stoi(string::unicode::to_utf8(cmd[2]));
        if (max_depth < 2) throw std::runtime_error("Command .display: Maximum depth must be greater than 1");
        _n->gen_svg(nd, string::unicode::to_utf8(cmd[1]) + ".svg", max_depth);
    }
    else if (cmd[0] == L".run")
    {
        _n->run(true, false, false);
        _n->print(L"> Ready.", true);
    }
    else if (cmd[0] == L".run-once")
    {
        _n->run(true, false, true);
        _n->print(L"> Ready.", true);
    }
    else if (cmd[0] == L".run-md")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .run-md: Missing subdirectory parameter (e.g., '.run-md tree')");
        std::string subdir = string::unicode::to_utf8(cmd[1]);
        _n->set_markdown_subdir(subdir);
        _n->print(L"> Running with markdown export...", true);
        if (_wikidata)
        {
            _wikidata->set_logging(false);
        }
        _n->run(false, true, false);
    }
    else if (cmd[0] == L".run-file")
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .run-file requires exactly one argument: the output file path");

        std::string   outfile = string::unicode::to_utf8(cmd[1]);
        std::ofstream out(outfile);
        if (!out.is_open())
            throw std::runtime_error("Command .run-file: Cannot open output file '" + outfile + "'");

        zelph::WikidataTextCompressor compressor({U' ', U'\t', U'\n', U','});

        bool is_wikidata = (_n->get_lang() == "wikidata");

        auto normal_print = [](const std::wstring& str, bool)
        {
#ifdef _WIN32
            std::wcout << str << std::endl;
#else
            std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
        };

        std::function<void(const std::wstring&, bool)> encode_print =
            [&compressor, &out, normal_print, is_wikidata](const std::wstring& str, bool important)
        {
            normal_print(str, important);

            size_t pos = str.find(L" ⇐ ");
            if (pos == std::wstring::npos)
                return;

            std::wstring deduction = str.substr(0, pos);
            boost::trim(deduction);

            std::wstring reasons = str.substr(pos + 3);
            boost::trim(reasons);

            if (!reasons.empty() && reasons.front() == L'(')
                reasons.erase(0, 1);
            if (!reasons.empty() && reasons.back() == L')')
                reasons.erase(reasons.size() - 1);
            boost::trim(reasons);

            boost::replace_all(reasons, L"(", L"");
            boost::replace_all(reasons, L")", L"");

            boost::replace_all(deduction, L"«", L"");
            boost::replace_all(deduction, L"»", L"");
            boost::replace_all(reasons, L"«", L"");
            boost::replace_all(reasons, L"»", L"");

            std::wstring line_for_file;
            if (!reasons.empty())
                line_for_file = reasons + L" ⇒ " + deduction;
            else
                line_for_file = deduction; // Fallback (sehr selten)

            boost::trim(line_for_file);

            std::string utf8_line = string::unicode::to_utf8(line_for_file);

            if (is_wikidata)
            {
                std::string encoded = compressor.encode(utf8_line);
                out << encoded << '\n';
            }
            else
            {
                out << utf8_line << '\n';
            }
        };

        _n->print(L"Starting full inference in encode mode – deduced facts (reversed order, no brackets/markup) will be written to " + cmd[1] + (is_wikidata ? L" (with Wikidata token encoding)." : L" (plain text)."), true);

        _n->set_print(encode_print);

        _n->run(true, false, false);

        _n->set_print(normal_print);

        _n->print(L"> Ready.", true);
    }
    else if (cmd[0] == L".decode")
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .decode requires exactly one argument: the input file path");

        std::string   infile = string::unicode::to_utf8(cmd[1]);
        std::ifstream in(infile);
        if (!in.is_open())
            throw std::runtime_error("Command .decode: Cannot open input file '" + infile + "'");

        zelph::WikidataTextCompressor compressor({U' ', U'\t', U'\n', U','});

        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty())
            {
                std::string decoded = compressor.decode(line);
                std::cout << decoded << std::endl;
            }
        }
    }
    else if (cmd[0] == L".load")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .load: Missing bin or json file name");
        if (cmd.size() > 2) throw std::runtime_error("Command .load: Unknown argument after file name");

        std::ofstream log("load.log");
        _n->set_print([&](const std::wstring& str, bool o)
                      {
          log << string::unicode::to_utf8(str) << std::endl;

          if (o)
          {
#ifdef _WIN32
              std::wcout << str << std::endl;
#else
            std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
          } });

        if (cmd.size() == 2)
        {
            network::StopWatch watch;
            watch.start();
            _wikidata = std::make_shared<Wikidata>(_n, cmd[1]);
            _wikidata->generate_index();
            _wikidata->import_all(false); // false, i.e. we do no filtering anymore (was: _n->has_language("wikidata") - so we only imported statements that were connected to existing nodes in the script)
            watch.stop();
            _n->print(L" Time needed for importing: " + string::unicode::from_utf8(watch.format()), true);
        }
        else
        {
            throw std::runtime_error("Command .load: You need to specify one argument: the *.bin or *.json file to import");
        }
    }
    else if (cmd[0] == L".wikidata-constraints")
    {
        if (cmd.size() < 3) throw std::runtime_error("Command .wikidata-constraints: Missing json file name or directory name");
        if (cmd.size() > 3) throw std::runtime_error("Command .wikidata-constraints: Unknown argument after directory name");

        network::StopWatch watch;
        watch.start();
        _wikidata = std::make_shared<Wikidata>(_n, cmd[1]);
        _wikidata->generate_index();
        std::string dir = string::unicode::to_utf8(cmd[2]);
        _wikidata->import_all(false, dir);
        _n->print(L" Time needed for exporting constraints: " + std::to_wstring(static_cast<double>(watch.duration()) / 1000) + L"s", true);
    }
    else if (cmd[0] == L".list-rules")
    {
        // Get all nodes that are subjects of a core.Causes relation
        network::adjacency_set rule_nodes = _n->get_rules();
        if (rule_nodes.empty())
        {
            _n->print(L"No rules found.", true);
            return;
        }

        _n->print(L"Listing all rules:", true);
        _n->print(L"------------------------", true);

        for (const auto& rule : rule_nodes)
        {
            std::wstring output;
            // Format the rule for printing
            _n->format_fact(output, _n->lang(), rule);
            _n->print(output, true);
        }
        _n->print(L"------------------------", true);
    }
    else if (cmd[0] == L".list-predicate-usage")
    {
        size_t limit = 0;
        if (cmd.size() > 2) throw std::runtime_error("Command .list-predicate-usage accepts at most one optional argument (max entries)");
        if (cmd.size() == 2)
        {
            try
            {
                size_t pos = 0;
                limit      = std::stoull(cmd[1], &pos);
                if (pos != cmd[1].length() || limit == 0)
                    throw std::runtime_error("Could not parse max entries argument");
            }
            catch (...)
            {
                throw std::runtime_error("Invalid max entries argument");
            }
        }
        list_predicate_usage(limit);
    }
    else if (cmd[0] == L".list-predicate-value-usage")
    {
        if (cmd.size() < 2 || cmd.size() > 3)
            throw std::runtime_error("Command .list-predicate-value-usage requires one required argument (<predicate>) and one optional (max entries)");

        size_t       limit    = 0;
        std::wstring pred_arg = cmd[1];
        if (cmd.size() == 3)
        {
            try
            {
                size_t pos = 0;
                limit      = std::stoull(cmd[2], &pos);
                if (pos != cmd[2].length() || limit == 0)
                    throw std::runtime_error("Could not parse max entries argument");
            }
            catch (...)
            {
                throw std::runtime_error("Invalid max entries argument");
            }
        }
        list_predicate_value_usage(pred_arg, limit);
    }
    else if (cmd[0] == L".wikidata-index")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .wikidata-index: Missing json file name");
        if (cmd.size() > 2) throw std::runtime_error("Command .wikidata-index: Unknown argument after json file name");

        _n->set_print([&](const std::wstring& str, bool o)
                      {
          if (o)
          {
#ifdef _WIN32
              std::wcout << str << std::endl;
#else
            std::clog << string::unicode::to_utf8(str) << std::endl;
#endif
          } });

        _wikidata = std::make_shared<Wikidata>(_n, cmd[1]);
        _wikidata->generate_index();
    }
    else if (cmd[0] == L".wikidata-export")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .wikidata-export: Missing Wikidata ID (e.g., P31 or Q42)");
        if (!_wikidata) throw std::runtime_error("Command .wikidata-export: Wikidata not loaded. Run .wikidata-index <file> first.");

        std::wstring wid = cmd[1];
        std::string  id  = string::unicode::to_utf8(wid);

        _wikidata->export_entry(wid);

        _n->print(L"Exported '" + wid + L"' to '" + string::unicode::from_utf8(id + ".json") + L"'", true);
    }
    else if (cmd[0] == L".remove-rules")
    {
        _n->remove_rules();
        _n->print(L"All rules removed.", true);
    }
    else if (cmd[0] == L".prune-facts")
    {
        if (cmd.size() < 2)
            throw std::runtime_error("Command .prune-facts requires a pattern with at least one variable");

        std::vector<std::wstring> pattern_tokens(cmd.begin() + 1, cmd.end());

        const std::wstring And    = _core_node_names.at(_n->core.And);
        const std::wstring Causes = _core_node_names.at(_n->core.Causes);

        bool                      is_rule = false;
        std::wstring              assigns_to_var;
        std::wstring              first_var = pattern_tokens.empty() ? L"" : (Impl::is_var(pattern_tokens[0]) ? L"" : pattern_tokens[0]);
        std::vector<std::wstring> tokens;

        for (const auto& token : pattern_tokens)
        {
            process_token(tokens, is_rule, first_var, assigns_to_var, token, And, Causes);
        }

        if (is_rule)
            throw std::runtime_error("Command .prune-facts: pattern must not be a rule (no '=>' allowed)");

        if (!assigns_to_var.empty())
            throw std::runtime_error("Command .prune-facts: assignment to variable (=) not supported");

        boost::bimap<std::wstring, network::Node> variables;
        network::Node                             pattern_fact = process_fact(tokens, variables);

        if (variables.empty())
            throw std::runtime_error("Command .prune-facts: pattern must contain at least one variable (A-Z or starting with _)");

        size_t removed = 0;
        _n->prune_facts(pattern_fact, removed);

        _n->print(L"Pruned " + std::to_wstring(removed) + L" matching facts.", true);
        if (removed > 0)
            _n->print(L"Consider running .cleanup to remove newly isolated nodes.", true);
    }
    else if (cmd[0] == L".prune-nodes")
    {
        if (cmd.size() < 2)
            throw std::runtime_error("Command .prune-nodes requires a pattern");

        std::vector<std::wstring> pattern_tokens(cmd.begin() + 1, cmd.end());

        const std::wstring And    = _core_node_names.at(_n->core.And);
        const std::wstring Causes = _core_node_names.at(_n->core.Causes);

        bool                      is_rule = false;
        std::wstring              assigns_to_var;
        std::wstring              first_var = pattern_tokens.empty() ? L"" : (Impl::is_var(pattern_tokens[0]) ? L"" : pattern_tokens[0]);
        std::vector<std::wstring> tokens;

        for (const auto& token : pattern_tokens)
        {
            process_token(tokens, is_rule, first_var, assigns_to_var, token, And, Causes);
        }

        if (is_rule)
            throw std::runtime_error("Command .prune-nodes: pattern must not be a rule (no '=>' allowed)");

        if (!assigns_to_var.empty())
            throw std::runtime_error("Command .prune-nodes: assignment to variable (=) not supported");

        boost::bimap<std::wstring, network::Node> variables;
        network::Node                             pattern_fact = process_fact(tokens, variables);

        network::Node relation = _n->parse_relation(pattern_fact);
        if (network::Network::is_var(relation))
        {
            throw std::runtime_error("Command .prune-nodes: the relation (predicate) must be fixed – no variable allowed in predicate position");
        }

        size_t removed_facts = 0;
        size_t removed_nodes = 0;
        _n->prune_nodes(pattern_fact, removed_facts, removed_nodes);

        _n->print(L"Pruned " + std::to_wstring(removed_facts) + L" matching facts and " + std::to_wstring(removed_nodes) + L" nodes.", true);
        if (removed_facts > 0 || removed_nodes > 0)
            _n->print(L"Consider running .cleanup to remove any remaining isolated nodes.", true);
    }
    else if (cmd[0] == L".cleanup")
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .cleanup takes no arguments");

        size_t removed_facts = 0;
        size_t removed_preds = 0;

        _n->print(L"Scanning for unused predicates and zombie facts...", true);

        _n->purge_unused_predicates(removed_facts, removed_preds);

        _n->print(L"Purged " + std::to_wstring(removed_facts) + L" zombie facts.", true);
        _n->print(L"Removed " + std::to_wstring(removed_preds) + L" unused predicates.", true);

        _n->print(L"Cleaning up isolated nodes...", true);

        size_t cleanup_count = 0;
        _n->cleanup_isolated(cleanup_count);
        _n->print(L"Cleanup: removed " + std::to_wstring(cleanup_count) + L" isolated nodes/names.", true);

        _n->print(L"Cleaning up name mappings...", true);
        size_t names_removed = _n->cleanup_names();
        _n->print(L"Removed " + std::to_wstring(names_removed) + L" dangling name entries.", true);
    }
    else if (cmd[0] == L".save")
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .save requires exactly one argument: the output file (must end with .bin)");

        std::wstring file = cmd[1];
        if (!boost::algorithm::ends_with(file, L".bin"))
            throw std::runtime_error("Command .save: filename must end with '.bin'");

        std::string utf8_file = string::unicode::to_utf8(file);
        _n->save_to_file(utf8_file);
        _n->print(L"Saved network to " + file, true);
    }
    else if (cmd[0] == L".import")
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .import: Missing script path");
        std::wstring path = cmd[1];
        if (!boost::algorithm::ends_with(path, L".zph")) throw std::runtime_error("Command .import: Script must end with .zph");
        import_file(path);
    }
    else
    {
        throw std::runtime_error(string::unicode::to_utf8(L"Unknown command " + cmd[0]));
    }
}

void console::Interactive::Impl::list_predicate_usage(size_t limit)
{
    if (_wikidata)
    {
        _wikidata->set_logging(false);
    }

    // Map to store predicate node and its usage count
    std::map<network::Node, size_t> predicate_usage_counts;

    // Get all predicates directly: nodes that IsA RelationTypeCategory
    auto predicates = _n->get_sources(_n->core.IsA, _n->core.RelationTypeCategory, true);

    for (const auto& pred : predicates)
    {
        // Get all facts where this node is used as a relation type
        const auto& facts_using_predicate = _n->get_left(pred);
        predicate_usage_counts[pred]      = facts_using_predicate.size();
    }

    // Convert map to vector for sorting
    std::vector<std::pair<network::Node, size_t>> sorted_predicates(predicate_usage_counts.begin(), predicate_usage_counts.end());

    // Sort the predicates by usage count in ascending order
    std::sort(sorted_predicates.begin(), sorted_predicates.end(), [](const auto& a, const auto& b)
              {
                  return a.second < b.second; // Sort by count, ascending
              });

    // Determine if wikidata language is available for three-column output
    bool has_wikidata_lang = _n->has_language("wikidata");

    _n->print(L"Predicate Usage:", true);
    _n->print(L"------------------------", true);

    size_t total           = sorted_predicates.size();
    size_t entries_to_show = limit ? std::min(limit, total) : total;
    size_t start_idx       = (limit && limit < total) ? total - entries_to_show : 0;

    for (size_t i = start_idx; i < total; ++i)
    {
        const auto&  entry          = sorted_predicates[i];
        std::wstring predicate_name = _n->get_name(entry.first, "", true); // Current language, with fallback
        std::wstring line_output;

        if (has_wikidata_lang && _n->get_lang() != "wikidata")
        {
            // Three columns: current lang name \t wikidata name \t count
            // For the first column, `lang` is an empty string to use the current language.
            // For the second column (wikidata name), `lang` is "wikidata" and `fallback` is `false`.
            std::wstring wikidata_name = _n->get_name(entry.first, "wikidata", false);
            line_output                = predicate_name + L"\t" + wikidata_name + L"\t" + std::to_wstring(entry.second);
        }
        else
        {
            // Two columns: current lang name \t count
            // `lang` is an empty string to use the current language, `fallback` is `true`.
            line_output = predicate_name + L"\t" + std::to_wstring(entry.second);
        }
        _n->print(line_output, true);
    }
    _n->print(L"------------------------", true);
    if (limit && limit < total)
        _n->print(L"Showing top " + std::to_wstring(limit) + L" of " + std::to_wstring(total) + L" predicates.", true);

    // Restore Wikidata logging state
    if (_wikidata)
    {
        _wikidata->set_logging(true);
    }
}

void console::Interactive::Impl::list_predicate_value_usage(const std::wstring& pred_arg, size_t limit /*= 0*/)
{
    if (_wikidata)
    {
        _wikidata->set_logging(false);
    }

    // Resolve the predicate node (accept name in current language or raw numeric ID)
    network::Node pred = _n->get_node(pred_arg);
    if (pred == 0)
    {
        try
        {
            size_t pos = 0;
            pred       = std::stoull(pred_arg, &pos);
            if (pos != pred_arg.length())
                throw std::runtime_error("");
        }
        catch (...)
        {
            throw std::runtime_error("Unknown predicate '" + string::unicode::to_utf8(pred_arg) + "' in current language '" + _n->lang() + "'");
        }
    }

    std::wstring pred_display = _n->get_name(pred, _n->lang(), true);
    if (pred_display.empty())
        pred_display = pred_arg;

    _n->print(L"Value Usage for predicate " + pred_display + L":", true);
    _n->print(L"------------------------", true);

    ankerl::unordered_dense::map<network::Node, size_t> value_counts;

    // All fact nodes that use this predicate: fact --> pred
    network::adjacency_set facts = _n->get_left(pred);

    for (network::Node fact : facts)
    {
        network::adjacency_set incoming = _n->get_left(fact); // subject(s) + object(s) --> fact

        // Objects are incoming nodes where fact does NOT point back to them
        for (network::Node cand : incoming)
        {
            if (!_n->has_right_edge(fact, cand))
            {
                value_counts[cand]++;
            }
        }
    }

    // Sort by count ascending
    std::vector<std::pair<size_t, network::Node>> sorted;
    sorted.reserve(value_counts.size());
    for (const auto& p : value_counts)
    {
        sorted.emplace_back(p.second, p.first);
    }
    std::sort(sorted.begin(), sorted.end());

    bool        has_wikidata_lang = _n->has_language("wikidata");
    std::string curr_lang         = _n->get_lang();

    size_t total           = sorted.size();
    size_t entries_to_show = limit ? std::min(limit, total) : total;
    size_t start_idx       = (limit && limit < total) ? total - entries_to_show : 0;

    for (size_t i = start_idx; i < total; ++i)
    {
        const auto&  entry      = sorted[i];
        std::wstring value_name = _n->get_name(entry.second, "", true); // current language with fallback

        std::wstring line;
        if (has_wikidata_lang && curr_lang != "wikidata")
        {
            std::wstring wikidata_name = _n->get_name(entry.second, "wikidata", false);
            if (wikidata_name.empty())
                wikidata_name = L"(no ID)";
            line = value_name + L"\t" + wikidata_name + L"\t" + std::to_wstring(entry.first);
        }
        else
        {
            line = value_name + L"\t" + std::to_wstring(entry.first);
        }
        _n->print(line, true);
    }

    _n->print(L"------------------------", true);
    _n->print(L"Total unique values: " + std::to_wstring(total), true);
    if (limit && limit < total)
        _n->print(L"Showing top " + std::to_wstring(limit) + L" of " + std::to_wstring(total) + L" values.", true);
    if (total == 0)
    {
        _n->print(L"(No values found for this predicate)", true);
    }

    if (_wikidata)
    {
        _wikidata->set_logging(true);
    }
}

network::Node console::Interactive::Impl::resolve_node(const std::wstring& arg) const
{
    network::Node nd = _n->get_node(arg);
    if (nd == 0)
    {
        try
        {
            size_t pos = 0;
            nd         = std::stoull(arg, &pos);
            if (pos != arg.length())
                throw std::exception();
        }
        catch (...)
        {
            throw std::runtime_error("Unknown node/argument");
        }
        if (!_n->exists(nd))
        {
            throw std::runtime_error("Node does not exist");
        }
    }
    return nd;
}

network::Node console::Interactive::Impl::resolve_single_node(const std::wstring& arg, bool prioritize_id) const
{
    // prioritize_id = true für destructive commands (.delname, .remove)

    bool is_numeric = true;
    for (wchar_t c : arg)
    {
        if (!std::iswdigit(c))
        {
            is_numeric = false;
            break;
        }
    }

    if (is_numeric && prioritize_id)
    {
        try
        {
            size_t        pos = 0;
            network::Node nd  = std::stoull(arg, &pos);
            if (pos == arg.length() && _n->exists(nd))
            {
                return nd;
            }
        }
        catch (...)
        {
        }
    }

    network::Node nd = _n->get_node(arg);
    if (nd != 0)
    {
        return nd;
    }

    if (is_numeric && !prioritize_id)
    {
        try
        {
            size_t        pos   = 0;
            network::Node nd_id = std::stoull(arg, &pos);
            if (pos == arg.length() && _n->exists(nd_id))
            {
                return nd_id;
            }
        }
        catch (...)
        {
        }
    }

    throw std::runtime_error("Unknown node '" + string::unicode::to_utf8(arg) + "'");
}

std::vector<network::Node> console::Interactive::Impl::resolve_nodes_by_name(const std::wstring& name) const
{
    return _n->resolve_nodes_by_name(name);
}

void console::Interactive::Impl::process_token(std::vector<std::wstring>& tokens, bool& is_rule, const std::wstring& first_var, std::wstring& assigns_to_var, const std::wstring& token, const std::wstring& And, const std::wstring& Causes) const
{
    if (!token.empty())
    {
        std::wstring current;

        if (!And.empty())
        {
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
        }

        if (current.empty())
        {
            current = token;
            tokens.push_back(current);
        }

        if (current == Causes)
        {
            if (is_rule)
                throw std::runtime_error("Line contains two times " + string::unicode::to_utf8(_n->get_name(_n->core.Causes))); // this is just a parser restriction to distinguish between condition and deduction, might be useful to support it
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
        if (*tokens.begin() == _core_node_names.at(_n->core.Contradiction))
        {
            return _n->core.Contradiction;
        }
        else
        {
            throw std::runtime_error(string::unicode::to_utf8(L"Fact '" + *tokens.begin() + L"' consists of only 1 token, which is only allowed for contradiction '" + _core_node_names.at(_n->core.Contradiction) + L"'"));
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
                    throw std::runtime_error(string::unicode::to_utf8(L"Variable " + token + L" has been set to a different node."));
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
                std::wstring  connected = string::concatenate(test, L' ');
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
            throw std::runtime_error(string::unicode::to_utf8(L"cannot match fact or condition (use quotation marks?): " + temp[0]._token + L"  " + temp[1]._token + L"  " + temp[2]._token + L" ..."));
        }

        combined = std::move(temp);
    }

    network::adjacency_set objects;
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
            throw std::runtime_error("Found empty condition or deduction in " + string::unicode::to_utf8(line));
        }

        if (is_deduction)
            deductions.emplace_back(tokens_for_fact);
        else
            conditions.emplace_back(tokens_for_fact);

        if (i < tokens.size() && tokens[i] == Causes) is_deduction = true;
    }

    if (conditions.empty()) throw std::runtime_error("Found rule without condition in " + string::unicode::to_utf8(line));
    if (deductions.empty()) throw std::runtime_error("Found rule without deduction in " + string::unicode::to_utf8(line));

    network::adjacency_set condition_nodes;
    for (const auto& condition : conditions)
    {
        condition_nodes.insert(process_fact(condition, variables));
    }

    const network::Node combined_condition = conditions.size() == 1
                                               ? *condition_nodes.begin()
                                               : _n->condition(_n->core.And, condition_nodes);

    network::adjacency_set deduction_list;
    for (const auto& deduction : deductions)
    {
        deduction_list.insert(process_fact(deduction, variables));
    }

    return _n->fact(combined_condition, _n->core.Causes, deduction_list);
}

void console::Interactive::run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition) const
{
    _pImpl->_n->run(print_deductions, generate_markdown, suppress_repetition);
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
console::Interactive interactive;

extern "C" void zelph_process_c(const char* line, size_t len)
{
    if (len > 0)
    {
        std::string l(line, 0, len);
        interactive.process(string::unicode::from_utf8(l));
    }
}

extern "C" void zelph_run()
{
    interactive.run(true, false, false);
}
#endif
