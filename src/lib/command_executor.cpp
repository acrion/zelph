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

#include "command_executor.hpp"

#include "chrono/stopwatch.hpp"
#include "io/data_manager.hpp"
#include "io/mermaid.hpp"
#include "network/network.hpp"
#include "network/reasoning.hpp"
#include "platform/platform_utils.hpp"
#include "script_engine.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "wikidata/wikidata.hpp"
#include "wikidata/wikidata_text_compressor.hpp"

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <iomanip>
#include <map>

using namespace zelph;

class console::CommandExecutor::Impl
{
public:
    Impl(network::Reasoning*               n,
         ScriptEngine*                     se,
         std::shared_ptr<io::DataManager>& dm,
         std::shared_ptr<ReplState>        rs,
         CommandExecutor::LineProcessor    lp)
        : _n(n)
        , _script_engine(se)
        , _data_manager(dm)
        , _repl_state(std::move(rs))
        , _process_line_callback(std::move(lp))
    {
        register_commands();
    }

    void execute(const std::vector<std::string>& cmd)
    {
        if (cmd.empty()) return;

        auto it = _command_map.find(cmd[0]);
        if (it != _command_map.end())
        {
            // Execute the specific handler
            it->second(cmd);
        }
        else
        {
            throw std::runtime_error("Unknown command " + cmd[0] + ". Type .help for a list.");
        }
    }

private:
    // --- Context References ---
    network::Reasoning*               _n;
    ScriptEngine*                     _script_engine;
    std::shared_ptr<io::DataManager>& _data_manager;
    std::shared_ptr<ReplState>        _repl_state;
    CommandExecutor::LineProcessor    _process_line_callback;

    // --- Dispatch Map ---
    using Handler = std::function<void(const std::vector<std::string>&)>;
    std::map<std::string, Handler> _command_map;

    // --- Registration ---
    void register_commands()
    {
        _command_map[".help"] = [this](auto& c)
        { cmd_help(c); };
        _command_map[".exit"] = [this](auto& c) { /* Exit handled by caller loop, usually acts as no-op here or throws */ };
        _command_map[".lang"] = [this](auto& c)
        { cmd_lang(c); };
        _command_map[".name"] = [this](auto& c)
        { cmd_name(c); };
        _command_map[".delname"] = [this](auto& c)
        { cmd_delname(c); };
        _command_map[".node"] = [this](auto& c)
        { cmd_node(c); };
        _command_map[".list"] = [this](auto& c)
        { cmd_list(c); };
        _command_map[".clist"] = [this](auto& c)
        { cmd_clist(c); };
        _command_map[".out"] = [this](auto& c)
        { cmd_connections(c, true); };
        _command_map[".in"] = [this](auto& c)
        { cmd_connections(c, false); };
        _command_map[".remove"] = [this](auto& c)
        { cmd_remove(c); };
        _command_map[".mermaid"] = [this](auto& c)
        { cmd_mermaid(c); };
        _command_map[".run"] = [this](auto& c)
        { cmd_run(c); };
        _command_map[".run-once"] = [this](auto& c)
        { cmd_run_once(c); };
        _command_map[".run-md"] = [this](auto& c)
        { cmd_run_md(c); };
        _command_map[".run-file"] = [this](auto& c)
        { cmd_run_file(c); };
        _command_map[".decode"] = [this](auto& c)
        { cmd_decode(c); };
        _command_map[".load"] = [this](auto& c)
        { cmd_load(c); };
        _command_map[".wikidata-constraints"] = [this](auto& c)
        { cmd_wikidata_constraints(c); };
        _command_map[".list-rules"] = [this](auto& c)
        { cmd_list_rules(c); };
        _command_map[".list-predicate-usage"] = [this](auto& c)
        { cmd_list_predicate_usage(c); };
        _command_map[".list-predicate-value-usage"] = [this](auto& c)
        { cmd_list_predicate_value_usage(c); };
        _command_map[".remove-rules"] = [this](auto& c)
        { cmd_remove_rules(c); };
        _command_map[".prune-facts"] = [this](auto& c)
        { cmd_prune(c, true); };
        _command_map[".prune-nodes"] = [this](auto& c)
        { cmd_prune(c, false); };
        _command_map[".cleanup"] = [this](auto& c)
        { cmd_cleanup(c); };
        _command_map[".stat"] = [this](auto& c)
        { cmd_stat(c); };
        _command_map[".log"] = [this](auto& c)
        { cmd_log(c); };
        _command_map[".log-janet"] = [this](auto& c)
        { cmd_log_janet(c); };
        _command_map[".save"] = [this](auto& c)
        { cmd_save(c); };
        _command_map[".import"] = [this](auto& c)
        { cmd_import(c); };
        _command_map[".auto-run"] = [this](auto& c)
        { cmd_auto_run(c); };
        _command_map[".export-wikidata"] = [this](auto& c)
        { cmd_export_wikidata(c); };
        _command_map[".parallel"] = [this](auto& c)
        { cmd_parallel(c); };
    }

    // --- Helpers ---

#define DEFAULT_EXCLUDE_NODES {_n->core.RelationTypeCategory, _n->core.IsA}

    void display_node_details(network::Node nd, bool resolved_from_name, int depth = 1, int max_neighbors = string::default_display_max_neighbors) const
    {
        if (resolved_from_name)
        {
            _n->out_stream() << "Resolved to node ID: " << nd << std::endl;
        }

        _n->out_stream() << "Node ID: " << nd << std::endl;

        {
            std::string core_name = _n->get_core_name(nd);
            if (!core_name.empty())
            {
                _n->out_stream() << "  Core node: " << core_name << std::endl;
            }
        }

        _n->out_stream() << "  Variable: " << (network::Network::is_var(nd) ? "yes" : "no") << std::endl;

        bool        has_wikidata = false;
        std::string wikidata_name;
        bool        has_any_name = false;

        for (const std::string& lang : _n->get_languages())
        {
            std::string name = _n->get_name(nd, lang, false);
            if (!name.empty())
            {
                has_any_name = true;
                _n->out_stream() << "  Name in language '" << lang << "': '" << name << "'" << std::endl;
                if (lang == "wikidata")
                {
                    has_wikidata  = true;
                    wikidata_name = name;
                }
            }
        }

        if (!has_any_name)
        {
            _n->out_stream() << "  (No names in any language)" << std::endl;
        }

        if (has_wikidata)
        {
            std::string       prefix    = (wikidata_name[0] == L'P') ? "Property:" : "";
            std::string       url       = "https://www.wikidata.org/wiki/" + prefix + wikidata_name;
            const std::string OSC_START = "\033]8;;";
            const char        OSC_SEP   = '\a';
            const std::string OSC_END   = "\033]8;;\a";
            _n->out_stream() << "  Wikidata URL: " << OSC_START << url << OSC_SEP << url << OSC_END << std::endl;
        }

        if (depth > 0)
        {
            generate_and_print_mermaid_link(nd,
                                            depth,
                                            max_neighbors,
                                            DEFAULT_EXCLUDE_NODES);
        }

        auto format_node = [this, max_neighbors](network::Node node) -> std::string
        {
            std::string node_str  = std::to_string(node);
            std::string node_name = _n->get_name(node, _n->lang(), true); // fallback active
            if (node_str == node_name || node_name.empty())
            {
                std::string fact_repr;
                string::node_to_string(_n, fact_repr, _n->lang(), node, max_neighbors);
                if (!fact_repr.empty() && fact_repr != "??")
                {
                    return fact_repr + " (ID " + std::to_string(node) + ")";
                }
                else
                {
                    return "ID " + std::to_string(node);
                }
            }
            else
            {
                return node_name + " (ID " + std::to_string(node) + ")";
            }
        };

        auto display_connections = [&](const network::adjacency_set& conns, const std::string& header)
        {
            if (conns.empty())
            {
                return;
            }

            _n->out_stream() << "  " << header << ":" << std::endl;
            if (conns.size() <= max_neighbors)
            {
                for (network::Node node : conns)
                {
                    _n->out_stream() << "    - " << format_node(node) << std::endl;
                }
            }
            else
            {
                _n->out_stream() << "    (" << conns.size() << " connections)" << std::endl;
            }
        };

        display_connections(_n->get_left(nd), "Incoming connections from");
        display_connections(_n->get_right(nd), "Outgoing connections to");

        std::string fact_repr;
        string::node_to_string(_n, fact_repr, _n->lang(), nd, max_neighbors);
        if (!fact_repr.empty() && fact_repr != "??")
        {
            _n->out_stream() << "  Representation: " << fact_repr << std::endl;
        }

        _n->out_stream() << "------------------------" << std::endl;
    }

    void generate_and_print_mermaid_link(network::Node                            nd,
                                         int                                      depth,
                                         int                                      max_neighbors,
                                         const std::unordered_set<network::Node>& exclude_nodes,
                                         bool                                     dark_theme        = true,
                                         bool                                     horizontal_layout = true,
                                         bool                                     use_subgraphs     = true) const
    {
        std::filesystem::path temp_dir  = std::filesystem::temp_directory_path();
        std::string           hex_name  = _n->get_name_hex(nd, false, max_neighbors);
        std::string           safe_name = string::sanitize_filename(hex_name);
        std::filesystem::path html_path = temp_dir / (safe_name + ".html");

        io::gen_mermaid_html(_n,
                             nd,
                             html_path.string(),
                             depth,
                             max_neighbors,
                             exclude_nodes,
                             dark_theme,
                             horizontal_layout,
                             use_subgraphs);

        std::string abs_path = html_path.string();
        std::string file_url = "file://" + abs_path;

        const std::string OSC_START = "\033]8;;";
        const char        OSC_SEP   = '\a';
        const std::string OSC_END   = "\033]8;;\a";

        _n->out_stream() << "  Mermaid HTML: " << OSC_START << file_url << OSC_SEP << file_url << OSC_END << std::endl;
    }

    network::Node resolve_node(const std::string& arg, const std::string& lang) const
    {
        network::Node nd = _n->get_node(arg, lang);
        if (nd == 0)
        {
            try
            {
                size_t pos = 0;
                nd         = std::stoull(arg, &pos);
                if (pos != arg.length())
                    nd = 0;
                else if (!_n->exists(nd))
                    throw std::runtime_error("Node does not exist");
            }
            catch (...)
            {
                nd = 0;
            }
        }
        return nd;
    }

    network::Node resolve_single_node(const std::string& arg, bool prioritize_id) const
    {
        bool is_numeric = std::all_of(arg.begin(), arg.end(), ::iswdigit);

        if (is_numeric && prioritize_id)
        {
            try
            {
                size_t        pos = 0;
                network::Node nd  = std::stoull(arg, &pos);
                if (pos == arg.length() && _n->exists(nd)) return nd;
            }
            catch (...)
            {
            }
        }

        network::Node nd = _n->get_node(arg);
        if (nd != 0) return nd;

        if (is_numeric && !prioritize_id)
        {
            try
            {
                size_t        pos   = 0;
                network::Node nd_id = std::stoull(arg, &pos);
                if (pos == arg.length() && _n->exists(nd_id)) return nd_id;
            }
            catch (...)
            {
            }
        }

        throw std::runtime_error("Unknown node '" + arg + "'");
    }

public:
    void import_file(const std::string& file, const std::vector<std::string>& args = {}) const
    {
        AutoRunSuspender suspend(_repl_state);

        if (!args.empty())
            _script_engine->set_script_args(args);

        _n->diagnostic_stream() << "Importing file " << file << "..." << std::endl;
        std::ifstream stream(file);
        if (stream.fail()) throw std::runtime_error("Could not open file '" + file + "'");
        for (std::string line_utf8; std::getline(stream, line_utf8);)
        {
            _process_line_callback(line_utf8);
        }

        // Flush any remaining accumulated zelph statement (incomplete file would be a script bug)
        if (_repl_state->accumulating_zelph && !_repl_state->zelph_buffer.empty())
        {
            std::string transformed = _script_engine->parse_zelph_to_janet(_repl_state->zelph_buffer);
            if (!transformed.empty())
                _script_engine->process_janet(transformed, true);
            _repl_state->zelph_buffer.clear();
        }
        _repl_state->accumulating_zelph = false;

        // Flush any remaining accumulated Janet code
        if (!_repl_state->janet_buffer.empty())
        {
            _script_engine->process_janet(_repl_state->janet_buffer, false);
            _repl_state->janet_buffer.clear();
        }
        _repl_state->accumulating_inline_janet = false;
        _repl_state->script_mode               = ScriptMode::Zelph;

        if (suspend.was_active())
        {
            _n->run(true, false, false, true);
        }
    }

private:
    void list_predicate_usage(size_t limit)
    {
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

        _n->out("Predicate Usage:", true);
        _n->out("------------------------", true);

        size_t total           = sorted_predicates.size();
        size_t entries_to_show = limit ? std::min(limit, total) : total;
        size_t start_idx       = (limit && limit < total) ? total - entries_to_show : 0;

        for (size_t i = start_idx; i < total; ++i)
        {
            const auto& entry          = sorted_predicates[i];
            std::string predicate_name = _n->get_name(entry.first, "", true); // Current language, with fallback
            std::string line_output;

            if (has_wikidata_lang && _n->get_lang() != "wikidata")
            {
                // Three columns: current lang name \t wikidata name \t count
                // For the first column, `lang` is an empty string to use the current language.
                // For the second column (wikidata name), `lang` is "wikidata" and `fallback` is `false`.
                std::string wikidata_name = _n->get_name(entry.first, "wikidata", false);
                line_output               = predicate_name + "\t" + wikidata_name + "\t" + std::to_string(entry.second);
            }
            else
            {
                // Two columns: current lang name \t count
                // `lang` is an empty string to use the current language, `fallback` is `true`.
                line_output = predicate_name + "\t" + std::to_string(entry.second);
            }
            _n->out(line_output, true);
        }
        _n->out("------------------------", true);
        if (limit && limit < total)
            _n->out("Showing top " + std::to_string(limit) + " of " + std::to_string(total) + " predicates.", true);
    }

    void list_predicate_value_usage(const std::string& pred_arg, size_t limit /*= 0*/)
    {
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
                throw std::runtime_error("Unknown predicate '" + pred_arg + "' in current language '" + _n->lang() + "'");
            }
        }

        std::string pred_display = _n->get_name(pred, _n->lang(), true);
        if (pred_display.empty())
            pred_display = pred_arg;

        _n->out("Value Usage for predicate " + pred_display + ":", true);
        _n->out("------------------------", true);

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
            const auto& entry      = sorted[i];
            std::string value_name = _n->get_name(entry.second, "", true); // current language with fallback

            std::string line;
            if (has_wikidata_lang && curr_lang != "wikidata")
            {
                std::string wikidata_name = _n->get_name(entry.second, "wikidata", false);
                if (wikidata_name.empty())
                    wikidata_name = "(no ID)";
                line = value_name + "\t" + wikidata_name + "\t" + std::to_string(entry.first);
            }
            else
            {
                line = value_name + "\t" + std::to_string(entry.first);
            }
            _n->out(line, true);
        }

        _n->out("------------------------", true);
        _n->out("Total unique values: " + std::to_string(total), true);
        if (limit && limit < total)
            _n->out("Showing top " + std::to_string(limit) + " of " + std::to_string(total) + " values.", true);
        if (total == 0)
        {
            _n->out("(No values found for this predicate)", true);
        }
    }

    // --- Command Handlers ---

    void cmd_help(const std::vector<std::string>& cmd)
    {
        static const std::vector<std::string> general_help_lines = {
            "zelph Interactive Help",
            "",
            "Available Commands",
            "──────────────────",
            ".help [command]             – Show this help or detailed help for a specific command",
            ".exit                       – Exit interactive mode",
            ".lang [code]                – Show or set current language",
            ".name <node|id> <new_name>         – Set name in current language",
            ".name <node|id> <lang> <new_name>  – Set name in specific language",
            ".delname <node|id> [lang]          – Delete name in current language (or specified language)",
            ".node <name|id>                    – Show detailed node information (names, connections, representation, Wikidata URL)",
            ".list <count>                      – List first N existing nodes (internal map order, with details)",
            ".clist <count>                     – List first N nodes named in current language (sorted by ID if reasonable size, otherwise map order)",
            ".out <name|id> [count]             – List details of outgoing connected nodes (default 20)",
            ".in <name|id> [count]              – List details of incoming connected nodes (default 20)",
            ".mermaid <node_name> [max_depth]   – Generate Mermaid HTML file for a node",
            ".run                        – Run full inference",
            ".run-once                   – Run a single inference pass",
            ".run-md <subdir>            – Run inference and export results as Markdown",
            ".run-file <file>            – Run inference, write deduced facts (reversed order) to <file> (encoded if lang=wikidata)",
            ".decode <file>              – Decode an encoded/plain file and print readable facts",
            ".list-rules                 – List all defined inference rules",
            ".list-predicate-usage [max] – Show predicate usage statistics (top N most frequent predicates)",
            ".list-predicate-value-usage <pred> [max] – Show object/value usage statistics for a specific predicate (top N most frequent values)",
            ".remove-rules               – Remove all inference rules",
            ".remove <name|id>           – Remove a node (destructive: disconnects all edges and cleans names)",
            ".import <file.zph>          – Load and execute a zelph script file",
            ".load <file>                – Load a saved network (.bin) or import Wikidata JSON dump (creates .bin cache)",
            ".save <file.bin>            – Save the current network to a binary file",
            ".prune-facts <pattern>      – Remove all facts matching the query pattern (only statements)",
            ".prune-nodes <pattern>      – Remove matching facts AND all involved subject/object nodes",
            ".cleanup                    – Remove isolated nodes and clean name mappings",
            ".stat                       – Show network statistics (nodes, RAM usage, name entries, languages, rules)",
            ".log <max-depth>            – Enable detailed reasoning logging up to given recursion depth (0 = off, -1 = only statistics)",
            ".log-janet                  – Toggle logging of Janet function calls (inputs/outputs)",
            ".auto-run                   – Toggle automatic execution of .run after each input",
            ".parallel                   – Toggle parallel processing (default: on)",
            ".wikidata-constraints <json> <dir> – Export constraints to a directory",
            ".export-wikidata <json> <id1> [id2 ...] – Extracts exact JSON lines for Q-IDs (no import)",
            "",
            "Type \".help <command>\" for detailed information about a specific command.",
            "",
            "Basic Syntax",
            "────────────",
            "Facts:    <subject> <predicate> <object>",
            "          Predicates with spaces must be quoted.",
            "          Example: peter \"is father of\" paul",
            "",
            "Queries:  Statements containing variables (A-Z or starting with _).",
            "          Example:",
            "          _who \"is father of\" paul",
            "          Answer:  peter   is father of   paul",
            "",
            "Rules:    (*{(condition1) (condition2) (...)} ~ conjunction) => (deduction)",
            "          Example:",
            "          Berlin \"is capital of\" Germany",
            "          Germany \"is located in\" Europe",
            "          (*{(X \"is capital of\" Y)",
            "             (Y \"is located in\" Z)} ~ conjunction)",
            "          => (X \"is located in\" Z)",
            "          Answer: Berlin   is located in   Europe",
            "                  ⇐ {( Germany   is located in   Europe )",
            "                      ( Berlin   is capital of   Germany )}",
            "",
            "Janet Scripting",
            "───────────────",
            "Janet:    %<code> (inline, one line) or bare % (toggle block mode until next %).",
            "          Janet generates facts/rules programmatically – then zelph inference runs as usual.",
            "          Example (using the Berlin/Germany facts from above):",
            "          %(zelph/fact \"Berlin\" \"is capital of\" \"Germany\")",
            "          Germany \"is located in\" Europe",
            "          %",
            "          (let [cond (zelph/set",
            "                      (zelph/fact 'X \"is capital of\" 'Y)",
            "                      (zelph/fact 'Y \"is located in\" 'Z))]",
            "            (zelph/fact cond \"~\" \"conjunction\")",
            "            (zelph/fact cond \"=>\" (zelph/fact 'X \"is located in\" 'Z)))",
            "          %",
            "          Answer: Berlin   is located in   Europe",
            "                  ⇐ {( Germany   is located in   Europe )",
            "                      ( Berlin   is capital of   Germany )}",
            "",
            "Unquote:  ,janet-var inside zelph lines (after defining in Janet).",
            "          Example:",
            "          %(def berlin (zelph/resolve \"Berlin\"))",
            "          ,berlin \"is capital of\" Germany"};

        static const std::map<std::string, std::string> detailed_help = {
            {".help", ".help [command]\n"
                      "Without argument: shows this general help text with syntax and command overview.\n"
                      "With argument: shows detailed help for the specified command."},

            {".exit", ".exit\n"
                      "Exits the interactive REPL session."},

            {".lang", ".lang [language_code]\n"
                      "Without argument: displays the current language used for node names.\n"
                      "With argument: sets the language (e.g., 'zelph', 'en', 'de', 'wikidata')."},

            {".name", ".name <node|id> <new_name>\n"
                      "Sets the name of the node in the current language.\n"
                      ".name <node|id> <lang> <new_name>\n"
                      "Sets the name in the specified language.\n"
                      "The <node|id> can be a name (in current language) or numeric node ID.\n"
                      "Empty <new_name> is not allowed – use .delname to remove a name."},

            {".delname", ".delname <node|id> [lang]\n"
                         "Removes the name of the node in the current language (or the specified language if provided).\n"
                         "The <node|id> can be a name (in current language) or numeric node ID.\n"
                         "If the node had no name in the target language, nothing happens."},

            {".list", ".list <count>\n"
                      "Lists the first N existing nodes in the network (in internal map iteration order).\n"
                      "For each node: ID, non-empty names in all languages, connection counts, representation, and Wikidata URL if available."},

            {".clist", ".clist <count>\n"
                       "Lists the first N nodes that have a name in the current language.\n"
                       "If the language has a reasonable number of entries (≤ ~50k), nodes are sorted by ID.\n"
                       "For very large languages (e.g. 'wikidata'), order follows the internal map (fast, no full sort)."},

            {".out", ".out <name|id> [count]\n"
                     "Lists detailed information for up to <count> nodes reachable via outgoing connections\n"
                     "from the given node (default 20, sorted by node ID)."},

            {".in", ".in <name|id> [count]\n"
                    "Lists detailed information for up to <count> nodes that have outgoing connections\n"
                    "to the given node (default 20, sorted by node ID)."},

            {".node", ".node <name_or_id>\n"
                      "Displays details for a single node: its ID, non-empty names in all languages,\n"
                      "incoming/outgoing connection counts, and a clickable Wikidata URL if it has a Wikidata ID.\n"
                      "The argument can be a name (in current language) or a numeric node ID."},

            {".nodes", ".nodes <count>\n"
                       "Lists the first N named nodes (nodes that have at least one name in any language),\n"
                       "sorted by node ID. For each node: ID, non-empty names in all languages,\n"
                       "incoming/outgoing connection counts, and Wikidata URL if available."},

            {".clist", ".clist <count>\n"
                       "Lists the first N nodes that have a name in the current language, sorted by node ID.\n"
                       "Output format is identical to .list (names in all languages, connection counts, Wikidata URL)."},

            {".mermaid", ".mermaid <node_name> [max_depth]\n"
                         "Generates a Mermaid HTML file visualizing the specified node and its connections\n"
                         "up to the given depth (default 3). The file is named <node_name>.html in the system temp dir.\n"
                         "Outputs a clickable file:// link to the generated HTML."},

            {".run", ".run\n"
                     "Performs full inference: repeatedly applies all rules until no new facts are derived.\n"
                     "Deductions are printed as they are found."},

            {".run-once", ".run-once\n"
                          "Performs a single inference pass."},

            {".run-md", ".run-md <subdir>\n"
                        "Runs full inference and exports all deductions and contradictions as Markdown files\n"
                        "in the directory mkdocs/docs/<subdir> for use with MkDocs."},

            {".run-file", ".run-file <file>\n"
                          "Performs full inference. Deduced facts (positive conclusions and contradictions) are written to <file>\n"
                          "in reversed order (reasons first, then ⇒ conclusion), without any brackets or markup.\n"
                          "Console output remains unchanged (original order with ⇐ explanations).\n"
                          "If the current language is 'wikidata' (set via .lang wikidata), Wikidata identifiers are heavily\n"
                          "compressed for minimal file size. Otherwise the file contains plain readable text."},

            {".decode", ".decode <file>\n"
                        "Reads a file created by .run-file (encoded or plain) and prints the decoded facts\n"
                        "in readable form to standard output."},

            {".list-rules", ".list-rules\n"
                            "Lists all currently defined inference rules in readable format."},

            {".list-predicate-usage", ".list-predicate-usage [max_entries]\n"
                                      "Shows how often each predicate (relation type) is used, sorted by frequency.\n"
                                      "If <max_entries> is specified, only the top N most frequent predicates are shown.\n"
                                      "If Wikidata language is active, Wikidata IDs are shown alongside names."},

            {".list-predicate-value-usage", ".list-predicate-value-usage <predicate> [max_entries]\n"
                                            "Shows how often each object (value) is used with the specified predicate, sorted by frequency.\n"
                                            "The <predicate> can be a name (in the current language) or a numeric node ID.\n"
                                            "If <max_entries> is specified, only the top N most frequent values are shown.\n"
                                            "If the Wikidata language is available and active, Wikidata IDs are shown alongside names."},

            {".remove-rules", ".remove-rules\n"
                              "Deletes all inference rules from the network."},

            {".remove", ".remove <name_or_id>\n"
                        "Removes the specified node from the network, disconnecting all its edges\n"
                        "and cleaning all name mappings. The argument can be a node name (looked up in the current language)\n"
                        "or a numeric node ID.\n"
                        "WARNING: This operation is destructive and irreversible!"},

            {".import", ".import <file.zph>\n"
                        "Loads and immediately executes a zelph script file."},

            {".load", ".load <file>\n"
                      "Loads a previously saved network state.\n"
                      "- If <file> ends with '.bin': loads the serialized network directly (fast).\n"
                      "- If <file> ends with '.json' or '.json.bz2' (Wikidata dump): imports the data and automatically creates a '.bin' cache file\n"
                      "  in the same directory for faster future loads."},

            {".save", ".save <file.bin>\n"
                      "Saves the current network state to a binary file.\n"
                      "The filename must end with '.bin'."},

            {".prune-facts", ".prune-facts <pattern>\n"
                             "Removes only the matching facts (statement nodes).\n"
                             "The pattern may contain variables in any position.\n"
                             "Reports how many facts were removed."},

            {".prune-nodes", ".prune-nodes <pattern>\n"
                             "Removes all matching facts AND all nodes that appear as subject or object in these facts.\n"
                             "Requirements:\n"
                             "- The relation (predicate) must be fixed (no variable allowed in predicate position)\n"
                             "- Variables are allowed in subject and/or object positions\n"
                             "WARNING: This is highly destructive! It removes ALL connections of the affected nodes.\n"
                             "The relation node itself becomes isolated and can be removed with .cleanup.\n"
                             "Reports removed facts and nodes."},

            {".cleanup", ".cleanup\n"
                         "Removes all nodes that have no connections (isolated nodes).\n"
                         "Also cleans up associated entries in name mappings."},

            {".stat", ".stat\n"
                      "Shows current network statistics:\n"
                      "- Number of nodes\n"
                      "- RAM usage (in GiB, if available)\n"
                      "- Total entries in name-of-node mappings\n"
                      "- Total entries in node-of-name mappings\n"
                      "- Number of languages\n"
                      "- Number of rules"},

            {".log", ".log <max-depth>\n"
                     "Enables detailed reasoning logging up to the given recursion depth.\n"
                     "0 disables it.\n"
                     "Every line is correctly indented according to depth."},

            {".log-janet", ".log-janet\n"
                           "Toggles detailed logging of inputs and outputs for all zelph/* Janet functions.\n"
                           "Logs inputs at function entry and both inputs and output at exit."},

            {".auto-run", ".auto-run\n"
                          "Toggles the automatic execution of the inference engine (.run) after every input.\n"
                          "Default is ON. Automatically switches to OFF when .load is used."},

            {".parallel", ".parallel\n"
                          "Toggles parallel processing on/off.\n"
                          "Default is on for performance."},

            {".wikidata-constraints", ".wikidata-constraints <json_file> <output_dir>\n"
                                      "Processes the Wikidata dump and exports constraint scripts\n"
                                      "to the specified output directory."},

            {".export-wikidata", ".export-wikidata <wikidata-dump.json> <Qid1> [Qid2 ...]\n"
                                 "Extracts the exact JSON line for each given Wikidata ID (Q…)\n"
                                 "from the dump and writes it to <id>.json in the current directory.\n"
                                 "The dump can be .json or .json.bz2.\n"
                                 "No import, no .bin cache, no network – pure extraction."}};

        if (cmd[0] == ".help")
        {
            if (cmd.size() == 1)
            {
                for (const auto& l : general_help_lines)
                    _n->out(l, true);
            }
            else if (cmd.size() == 2)
            {
                auto it = detailed_help.find(cmd[1]);
                if (it != detailed_help.end())
                {
                    _n->out(it->second, true);
                }
                else
                {
                    _n->error("Unknown command: " + cmd[1] + ". Use \".help\" for a list of all commands.", true);
                }
            }
            else
            {
                throw std::runtime_error("Usage: .help [command]");
            }
        }
    }
    void cmd_lang(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2)
        {
            _n->out_stream() << "The current language is '" << _n->get_lang() << "'" << std::endl;
        }
        else
        {
            _n->set_lang(cmd[1]);
        }
    }

    void cmd_name(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 3 || cmd.size() > 4)
            throw std::runtime_error("Command .name: Invalid arguments. Usage: .name <node> <new_name>  or  .name <node> <lang> <new_name>");

        const std::string& name_in_current_lang = cmd[1];
        const std::string& name_in_target_lang  = cmd.size() == 3 ? cmd[2] : cmd[3];
        std::string        current_lang         = _n->get_lang();
        std::string        target_lang          = cmd.size() == 3 ? _n->lang() : cmd[2];

        network::Node node_in_current_lang = resolve_node(name_in_current_lang, current_lang);
        network::Node node_in_target_lang  = resolve_node(name_in_target_lang, target_lang);

        if (current_lang == target_lang)
        {
            // In this case, name_in_current_lang is strictly interpreted as the old name that we use to reference
            // the existing node. It does not make sense to support creating a new node in this mode.
            if (node_in_current_lang == 0)
            {
                throw std::runtime_error("Node '" + name_in_current_lang + "' does not exist");
            }
            else if (node_in_target_lang != 0)
            {
                throw std::runtime_error("Name '" + name_in_target_lang + "' is already in use by node " + std::to_string(node_in_target_lang));
            }
            else
            {
                _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
            }
        }
        else if (node_in_current_lang == 0)
        {
            if (node_in_target_lang == 0)
            {
                node_in_current_lang = _n->node(name_in_current_lang);
                _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
                _n->out("Node '" + name_in_current_lang + "' ('" + current_lang + "') / '" + name_in_target_lang + "' ('" + target_lang + "') does not exist yet in either language => created it.", true);
            }
            else
            {
                _n->set_name(node_in_target_lang, name_in_current_lang, current_lang, true);
                _n->out("Node '" + name_in_target_lang + "' ('" + target_lang + "') exists, assigned name '" + name_in_current_lang + "' in '" + current_lang + "'.", true);
            }
        }
        else if (node_in_target_lang == 0)
        {
            _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
            _n->out("Node '" + name_in_current_lang + "' ('" + current_lang + "') exists, assigned name '" + name_in_target_lang + "' in '" + target_lang + "'.", true);
        }
        else if (name_in_current_lang == _n->get_name(node_in_current_lang, current_lang, false) && name_in_target_lang == _n->get_name(node_in_target_lang, target_lang, false))
        {
            _n->out("Node '" + name_in_current_lang + "' ('" + current_lang + "') / '" + name_in_target_lang + "' ('" + target_lang + "') have the requested names, but are different nodes => Merging them.", true);
            _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, true);
        }
        else
        {
            throw std::runtime_error("Node '" + name_in_current_lang + "' ('" + current_lang + "') / '" + name_in_target_lang + "' ('" + target_lang + "') exists in both languages as different nodes => did not do anything)");
        }
    }
    void cmd_delname(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2 || cmd.size() > 3)
            throw std::runtime_error("Command .delname: Invalid arguments. Usage: .delname <node|id> [lang]");

        network::Node nd = resolve_single_node(cmd[1], true); // prioritize ID

        std::string target_lang = _n->lang();
        if (cmd.size() == 3)
        {
            target_lang = cmd[2];
        }

        _n->remove_name(nd, target_lang);

        _n->out("Removed name of node " + std::to_string(nd) + " in language '" + target_lang + "' (if it existed).", true);
    }
    void cmd_node(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .node: Exactly one argument required");

        std::string arg = cmd[1];

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
            nodes = _n->resolve_nodes_by_name(arg);
            if (nodes.empty())
            {
                throw std::runtime_error("No node found with name '" + arg + "' in current language '" + _n->lang() + "'");
            }
        }

        if (nodes.size() == 1)
        {
            bool resolved_from_name = !_n->get_name(nodes[0], _n->lang(), false).empty() || std::all_of(arg.begin(), arg.end(), ::iswdigit);
            display_node_details(nodes[0], resolved_from_name && nodes.size() == 1);
        }
        else
        {
            _n->out_stream() << "Found " << nodes.size() << " nodes with name '" << arg
                             << "' in current language '" << _n->lang() << "':" << std::endl;
            _n->out_stream() << "------------------------" << std::endl;

            // Sort by ID for consistent output
            std::sort(nodes.begin(), nodes.end());

            for (network::Node nd : nodes)
            {
                display_node_details(nd, true);
            }
        }
    }
    void cmd_list(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .list: Missing count parameter");

        size_t count = string::parse_count(cmd[1]);

        auto view = _n->get_all_nodes_view();

        _n->out_stream() << "Listing " << count << " nodes:" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;

        size_t displayed = 0;
        for (auto it = view.begin(); it != view.end() && displayed < count; ++it, ++displayed)
        {
            display_node_details(it->first, false);
        }

        _n->out_stream() << "Displayed " << displayed << " nodes." << std::endl;
    }
    void cmd_clist(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .clist: Missing count parameter");

        size_t count = string::parse_count(cmd[1]);

        auto view = _n->get_lang_nodes_view(_n->lang());

        _n->out_stream() << "Listing first " << count << " nodes named in current language '" << _n->lang() << "'" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;

        size_t displayed = 0;
        for (auto it = view.begin(); it != view.end() && displayed < count; ++it, ++displayed)
        {
            display_node_details(it->second, false);
        }
    }
    void cmd_connections(const std::vector<std::string>& cmd, bool outgoing)
    {
        if (cmd.size() < 2) throw std::runtime_error(std::string("Command ") + cmd[0] + ": Missing node argument");

        const std::string& arg     = cmd[1];
        network::Node      base_nd = resolve_node(arg, _n->lang()); // same resolve logic as .node/.remove

        if (base_nd == 0)
        {
            throw std::runtime_error("Unknown node");
        }

        size_t max_count = 20; // default
        if (cmd.size() >= 3)
        {
            max_count = string::parse_count(cmd[2]);
        }

        network::adjacency_set neighbors = outgoing ? _n->get_right(base_nd) : _n->get_left(base_nd);

        std::vector<network::Node> vec(neighbors.begin(), neighbors.end());
        std::sort(vec.begin(), vec.end());

        size_t to_display = std::min(max_count, vec.size());

        _n->out_stream() << (outgoing ? "Outgoing" : "Incoming")
                         << " connected nodes of " << base_nd
                         << " (first " << to_display << " of " << vec.size() << ", sorted by ID):" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;

        for (size_t i = 0; i < to_display; ++i)
        {
            display_node_details(vec[i], false);
        }
    }
    void cmd_remove(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Command .remove requires exactly one argument: name or ID");

        const std::string& arg = cmd[1];
        network::Node      nd  = resolve_single_node(arg, true); // prioritize ID

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
                throw std::runtime_error("Command .remove: Unknown node '" + arg + "' in current language '" + _n->lang() + "'");
            }

            if (!_n->exists(nd))
            {
                throw std::runtime_error("Command .remove: Node '" + std::to_string(nd) + "' does not exist");
            }
        }

        _n->remove_node(nd);
        _n->out("Removed node " + std::to_string(nd) + " (all edges disconnected, name mappings cleaned).", true);
        _n->diagnostic("Consider running .cleanup afterwards if needed.", true);
    }
    void cmd_mermaid(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .mermaid: Missing node name to visualise");
        const std::string& arg = cmd[1];
        network::Node      nd  = resolve_single_node(arg, true);
        if (nd == 0) throw std::runtime_error("Command .mermaid: Unknown node '" + arg + "'");
        int max_depth     = 1;
        int max_neighbors = string::default_display_max_neighbors;
        if (cmd.size() >= 3)
        {
            max_depth = std::stoi(cmd[2]);
            if (max_depth < 1) throw std::runtime_error("Command .mermaid: Maximum depth must be greater than 0. Note: when using 1, a dynamic depth based on the node count will be used.");
        }
        if (cmd.size() >= 4)
        {
            max_neighbors = std::stoi(cmd[3]);
            if (max_neighbors < 1) throw std::runtime_error("Command .mermaid: Maximum neighbors must be at least 1");
        }
        generate_and_print_mermaid_link(nd,
                                        max_depth,
                                        max_neighbors,
                                        DEFAULT_EXCLUDE_NODES);
    }
    void cmd_run(const std::vector<std::string>&)
    {
        _n->run(true, false, false);
        _n->diagnostic("Ready.", true);
    }
    void cmd_run_once(const std::vector<std::string>&)
    {
        _n->run(true, false, true);
        _n->diagnostic("Ready.", true);
    }
    void cmd_run_md(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .run-md: Missing subdirectory parameter (e.g., '.run-md tree')");
        std::string subdir = cmd[1];
        _n->set_markdown_subdir(subdir);
        _n->diagnostic("Running with markdown export...", true);
        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        _n->run(false, true, false);
    }
    void cmd_run_file(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .run-file requires exactly one argument: the output file path");

        std::string   outfile = cmd[1];
        std::ofstream out(outfile);
        if (!out.is_open())
            throw std::runtime_error("Command .run-file: Cannot open output file '" + outfile + "'");

        zelph::wikidata::WikidataTextCompressor compressor({U' ', U'\t', U'\n', U','});

        bool is_wikidata = (_n->get_lang() == "wikidata");

        auto normal_print = [&](const std::string& str, bool)
        {
            _n->out_stream() << str << std::endl;
        };

        std::function<void(const std::string&, bool)> encode_print =
            [&compressor, &out, normal_print, is_wikidata](const std::string& str, bool important)
        {
            normal_print(str, important);

            size_t pos = str.find(" ⇐ ");
            if (pos == std::string::npos)
                return;

            std::string deduction = str.substr(0, pos);
            boost::trim(deduction);

            std::string reasons = str.substr(pos + 3);
            boost::trim(reasons);

            if (!reasons.empty() && reasons.front() == L'(')
                reasons.erase(0, 1);
            if (!reasons.empty() && reasons.back() == L')')
                reasons.erase(reasons.size() - 1);
            boost::trim(reasons);

            boost::replace_all(reasons, "(", "");
            boost::replace_all(reasons, ")", "");

            boost::replace_all(deduction, "«", "");
            boost::replace_all(deduction, "»", "");
            boost::replace_all(reasons, "«", "");
            boost::replace_all(reasons, "»", "");

            std::string line_for_file;
            if (!reasons.empty())
                line_for_file = reasons + " ⇒ " + deduction;
            else
                line_for_file = deduction; // Fallback (sehr selten)

            boost::trim(line_for_file);

            std::string utf8_line = line_for_file;

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

        _n->diagnostic("Starting full inference in encode mode – deduced facts (reversed order, no brackets/markup) will be written to " + cmd[1] + (is_wikidata ? " (with Wikidata token encoding)." : " (plain text)."), true);

        _n->set_print(encode_print);

        _n->run(true, false, false);

        _n->set_print(normal_print);

        _n->diagnostic("Ready.", true);
    }

    void cmd_decode(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .decode requires exactly one argument: the input file path");

        std::string   infile = cmd[1];
        std::ifstream in(infile);
        if (!in.is_open())
            throw std::runtime_error("Command .decode: Cannot open input file '" + infile + "'");

        zelph::wikidata::WikidataTextCompressor compressor({U' ', U'\t', U'\n', U','});

        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty())
            {
                std::string decoded = compressor.decode(line);
                _n->out_stream() << decoded << std::endl;
            }
        }
    }
    void cmd_load(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .load: Missing bin or json file name");
        if (cmd.size() > 2) throw std::runtime_error("Command .load: Unknown argument after file name");

        if (_repl_state->auto_run)
        {
            _repl_state->auto_run = false;
            _n->out("Auto-run has been disabled due to loading a large dataset.", true);
        }

        std::ofstream log("load.log");
        _n->set_print([&](const std::string& str, bool o)
                      {
          log << str << std::endl;

          if (o)
          {
            _n->out_stream() << str << std::endl;
          } });

        if (cmd.size() == 2)
        {
            chrono::StopWatch watch;
            watch.start();

            // This detects if it's Wikidata (json/bz2 OR bin with source) or Generic (bin only)
            _data_manager = io::DataManager::create(_n, cmd[1]);
            _data_manager->load();

            watch.stop();
            _n->diagnostic(" Time needed for loading/importing: " + watch.format(), true);
        }
        else
        {
            throw std::runtime_error("Command .load: You need to specify one argument: the *.bin or *.json file to import");
        }
    }
    void cmd_wikidata_constraints(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 3) throw std::runtime_error("Command .wikidata-constraints: Missing json file name or directory name");
        if (cmd.size() > 3) throw std::runtime_error("Command .wikidata-constraints: Unknown argument after directory name");

        chrono::StopWatch watch;
        watch.start();

        std::string           dir        = cmd[2];
        std::filesystem::path input_path = cmd[1];

        // Specific Logic: This command strictly requires Wikidata capability.
        // We update the global manager to reflect this load context.
        _data_manager = io::DataManager::create(_n, input_path);

        // Dynamic cast to check if the factory returned a Wikidata manager
        auto wikidata_mgr = std::dynamic_pointer_cast<wikidata::Wikidata>(_data_manager);

        if (wikidata_mgr)
        {
            wikidata_mgr->import_all(dir);
        }
        else
        {
            // Fallback: If create() returned Generic (e.g. user pointed to a bin file without source),
            // but user wants constraints. This implies user error (missing source) or misuse.
            // But if user supplied JSON, create() definitely returns Wikidata.
            // If user supplied BIN, create() checks for source. If no source, it returns Generic.
            // If Generic, we can't export constraints.
            throw std::runtime_error("Cannot export constraints: Original Wikidata source file not found or invalid format.");
        }

        _n->diagnostic(" Time needed for exporting constraints: " + std::to_string(static_cast<double>(watch.duration()) / 1000) + "s", true);
    }
    void cmd_export_wikidata(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 3)
            throw std::runtime_error("Usage: .export-wikidata <wikidata-dump.json> <Q...> [Q...]");

        const std::string&       json_file = cmd[1];
        std::vector<std::string> ids(cmd.begin() + 2, cmd.end());

        auto dm       = io::DataManager::create(_n, json_file);
        auto wikidata = std::dynamic_pointer_cast<wikidata::Wikidata>(dm);

        if (!wikidata)
            throw std::runtime_error("File is not recognized as Wikidata JSON (no matching .json/.json.bz2 found).");

        wikidata->export_entities(ids);
        _n->diagnostic("Export finished. *.json files are in the current directory.", true);
    }
    void cmd_list_rules(const std::vector<std::string>&)
    {
        // Get all nodes that are subjects of a core.Causes relation
        network::adjacency_set rule_nodes = _n->get_rules();
        if (rule_nodes.empty())
        {
            _n->out("No rules found.", true);
            return;
        }

        _n->out("Listing all rules:", true);
        _n->out("------------------------", true);

        for (const auto& rule : rule_nodes)
        {
            std::string output;
            // Format the rule for printing
            string::node_to_string(_n, output, _n->lang(), rule, 3);
            _n->out(output, true);
        }
        _n->out("------------------------", true);
    }
    void cmd_list_predicate_usage(const std::vector<std::string>& cmd)
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
        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        list_predicate_usage(limit);
        if (_data_manager)
        {
            _data_manager->set_logging(true);
        }
    }
    void cmd_list_predicate_value_usage(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2 || cmd.size() > 3)
            throw std::runtime_error("Command .list-predicate-value-usage requires one required argument (<predicate>) and one optional (max entries)");

        size_t             limit    = 0;
        const std::string& pred_arg = cmd[1];
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
        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        list_predicate_value_usage(pred_arg, limit);
        if (_data_manager)
        {
            _data_manager->set_logging(true);
        }
    }

    void cmd_remove_rules(const std::vector<std::string>&)
    {
        _n->remove_rules();
        _n->out("All rules removed.", true);
    }
    void cmd_prune(const std::vector<std::string>& cmd, bool facts_mode)
    {
        if (cmd.size() < 2)
            throw std::runtime_error("Command requires a pattern");

        // Reconstruct the pattern string from arguments to feed into the parser
        std::string pattern_str;
        for (size_t i = 1; i < cmd.size(); ++i)
        {
            const std::string& token = cmd[i];
            // If it's a variable, keep as is (A). If not, quote it ("is") so PEG treats it as value.
            // Note: Since cmd is already tokenized by escaped_list_separator, quotes were stripped.
            // We re-add them for non-vars to be safe for parse_zelph_to_janet.
            if (ScriptEngine::is_var(token))
                pattern_str += token + " ";
            else
                pattern_str += "\"" + token + "\" ";
        }

        std::string utf8_pattern = pattern_str;

        // Delegate parsing and evaluation to ScriptEngine
        std::string janet_code = _script_engine->parse_zelph_to_janet(utf8_pattern);

        if (janet_code.empty())
            throw std::runtime_error("Could not parse pattern");

        network::Node pattern_fact = _script_engine->evaluate_expression(janet_code);

        if (pattern_fact == 0)
            throw std::runtime_error("Invalid pattern");

        if (facts_mode)
        {
            size_t removed = 0;
            _n->prune_facts(pattern_fact, removed);
            _n->out("Pruned " + std::to_string(removed) + " matching facts.", true);
            if (removed > 0) _n->diagnostic("Consider running .cleanup.", true);
        }
        else
        {
            network::Node relation = _n->parse_relation(pattern_fact);
            if (network::Network::is_var(relation))
            {
                throw std::runtime_error("Command .prune-nodes: relation (predicate) must be fixed");
            }
            size_t removed_facts = 0;
            size_t removed_nodes = 0;
            _n->prune_nodes(pattern_fact, removed_facts, removed_nodes);
            _n->out("Pruned " + std::to_string(removed_facts) + " matching facts and " + std::to_string(removed_nodes) + " nodes.", true);
            if (removed_facts > 0 || removed_nodes > 0)
            {
                _n->diagnostic("Consider running .cleanup.", true);
            }
        }
    }
    void cmd_cleanup(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .cleanup takes no arguments");

        size_t removed_facts = 0;
        size_t removed_preds = 0;

        _n->diagnostic("Scanning for unused predicates and zombie facts...", true);

        _n->purge_unused_predicates(removed_facts, removed_preds);

        _n->out("Purged " + std::to_string(removed_facts) + " zombie facts.", true);
        _n->out("Removed " + std::to_string(removed_preds) + " unused predicates.", true);

        _n->diagnostic("Cleaning up isolated nodes...", true);

        size_t cleanup_count = 0;
        _n->cleanup_isolated(cleanup_count);
        _n->out("Cleanup: removed " + std::to_string(cleanup_count) + " isolated nodes/names.", true);

        _n->diagnostic("Cleaning up name mappings...", true);
        size_t names_removed = _n->cleanup_names();
        _n->out("Removed " + std::to_string(names_removed) + " dangling name entries.", true);
    }
    void cmd_stat(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .stat takes no arguments");

        _n->out_stream() << "Network Statistics:" << std::endl;
        _n->out_stream() << "------------------------" << std::endl;

        _n->out_stream() << "Nodes: " << _n->count() << std::endl;

        size_t ram_usage = zelph::platform::get_process_memory_usage();
        if (ram_usage > 0)
        {
            _n->out_stream() << "RAM Usage: " << std::fixed << std::setprecision(1)
                             << (static_cast<double>(ram_usage) / (1024 * 1024 * 1024)) << " GiB" << std::endl;
        }

        if (_n->language_count() > 0)
        {
            _n->out_stream() << "Name-of-Node Entries by language:" << std::endl;
            for (const std::string& lang : _n->get_languages())
            {
                _n->out_stream() << "  " << lang << ": " << _n->get_name_of_node_size(lang) << std::endl;
            }

            _n->out_stream() << "Name-of-Node Entries by language:" << std::endl;
            for (const std::string& lang : _n->get_languages())
            {
                _n->out_stream() << "  " << lang << ": " << _n->get_node_of_name_size(lang) << std::endl;
            }
        }

        _n->out_stream() << "Languages: " << _n->language_count() << std::endl;
        _n->out_stream() << "Rules: " << _n->rule_count() << std::endl;

        _n->out_stream() << "------------------------" << std::endl;
    }
    void cmd_log(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .log: exactly one maximum recursion depth required (0 = off, -1 = only statistics).");

        int depth;
        try
        {
            depth = std::stoi(cmd[1]);
        }
        catch (...)
        {
            throw std::runtime_error("Command .log: invalid depth value.");
        }

        _n->set_logging(depth);
    }
    void cmd_log_janet(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .log-janet takes no arguments");

        _script_engine->toggle_janet_logging();
        _n->out("Janet function logging is now " + _script_engine->get_janet_logging_status() + ".", true);
    }
    void cmd_save(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2)
            throw std::runtime_error("Command .save requires exactly one argument: the output file (must end with .bin)");

        const std::string& file = cmd[1];
        if (!boost::algorithm::ends_with(file, ".bin"))
            throw std::runtime_error("Command .save: filename must end with '.bin'");

        std::string utf8_file = file;
        _n->save_to_file(utf8_file);
        _n->diagnostic("Saved network to " + file, true);
    }
    void cmd_import(const std::vector<std::string>& cmd) const
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .import: Missing script path");
        const std::string& path = cmd[1];
        if (!boost::algorithm::ends_with(path, ".zph")) throw std::runtime_error("Command .import: Script must end with .zph");
        import_file(path);
    }
    void cmd_auto_run(const std::vector<std::string>&)
    {
        _repl_state->auto_run = !_repl_state->auto_run;
        _n->out("Auto-run is now " + std::string(_repl_state->auto_run ? "enabled" : "disabled") + ".", true);
    }
    void cmd_parallel(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .parallel takes no arguments");

        _n->toggle_parallel();
        _n->out("Parallel processing is now " + std::string(_n->use_parallel() ? "enabled" : "disabled") + ".", true);
    }
};

console::CommandExecutor::CommandExecutor(network::Reasoning*               reasoning,
                                          ScriptEngine*                     script_engine,
                                          std::shared_ptr<io::DataManager>& data_manager,
                                          std::shared_ptr<ReplState>        repl_state,
                                          LineProcessor                     line_processor)
    : _pImpl(new Impl(reasoning, script_engine, data_manager, repl_state, std::move(line_processor)))
{
}

console::CommandExecutor::~CommandExecutor() = default;

void console::CommandExecutor::execute(const std::vector<std::string>& cmd)
{
    _pImpl->execute(cmd);
}

void console::CommandExecutor::import_file(const std::string& file, const std::vector<std::string>& args) const
{
    _pImpl->import_file(file, args);
}
