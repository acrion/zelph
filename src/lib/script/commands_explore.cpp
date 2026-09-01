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

#include "script/command_executor_impl.hpp"

#include "io/data_manager.hpp"
#include "io/mermaid.hpp"
#include "network/fact_structure.hpp"
#include "network/network.hpp"
#include "network/reasoning.hpp"
#include "platform/platform_utils.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <ios>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#define DEFAULT_EXCLUDE_NODES {_n->core.RelationTypeCategory, _n->core.IsA}

using namespace zelph;

namespace zelph::console
{
    void CommandExecutor::Impl::display_node_details(network::Node nd, bool resolved_from_name, int depth, int max_neighbors) const
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

        // The negation tag is a fact ABOUT the node, and a ground pattern is
        // hash-consed, so a pattern negated in ONE rule carries it wherever
        // it occurs. It is therefore reported as a property here rather than
        // written into the term -- see node_to_string, where the "¬" is kept
        // for the rule's own condition slot and nowhere else.
        if (_n->check_fact(nd, _n->core.IsA, {_n->core.Negation}).is_known())
        {
            _n->out_stream() << "  Negated by a rule: yes" << std::endl;
        }

        // A refutation is the opposite case and is written INTO the term by
        // the renderer, because it is the claim the node stands for rather
        // than an annotation about it. Said here as well, since the structural
        // reading is what `.node` is for: the marking is an ordinary fact of
        // the predicate "refuted", visible among the connections below.
        if (_n->is_refuted_fact(nd))
        {
            _n->out_stream() << "  Refuted (claimed not to hold): yes" << std::endl;
        }

        // The same reading for the rule-pattern marking, and for the same
        // reason: the renderer no longer substitutes it for the node, so the
        // one place that used to show it is gone. ".explain" says it too.
        if (_n->is_rule_pattern(nd))
        {
            _n->out_stream() << "  Rule pattern (not asserted): yes" << std::endl;
        }

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
            std::string       prefix    = (wikidata_name[0] == 'P') ? "Property:" : "";
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
                    return fact_repr + " (ID " + string::unmark_identifiers(std::to_string(node)) + ")";
                }
                else
                {
                    return "ID " + string::unmark_identifiers(std::to_string(node));
                }
            }
            else
            {
                return node_name + " (ID " + string::unmark_identifiers(std::to_string(node)) + ")";
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
                    _n->out_stream() << "    - " << string::unmark_identifiers(format_node(node)) << std::endl;
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
            _n->out_stream() << "  Representation: " << string::unmark_identifiers(fact_repr) << std::endl;
        }

        _n->out_stream() << "------------------------" << std::endl;
    }

    void CommandExecutor::Impl::generate_and_print_mermaid_link(network::Node nd, int depth, int max_neighbors, const std::unordered_set<network::Node>& exclude_nodes, bool dark_theme, bool horizontal_layout, bool use_subgraphs) const
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

        _repl_state->last_graph_html_path = html_path.string();

#ifndef __EMSCRIPTEN__
        // In a real terminal the OSC 8 link is clickable. In the wasm
        // playground the graph panel fetches the file from MEMFS via
        // take_last_graph_html() instead, and a dead file:// link in the
        // browser terminal would only mislead - so print nothing there.
        std::string file_url = "file://" + html_path.string();

        const std::string OSC_START = "\033]8;;";
        const char        OSC_SEP   = '\a';
        const std::string OSC_END   = "\033]8;;\a";

        _n->out_stream() << "  Mermaid HTML: " << OSC_START << file_url << OSC_SEP << file_url << OSC_END << std::endl;
#endif
    }

    void CommandExecutor::Impl::list_predicate_usage(size_t limit)
    {
        // A fact carrying a VARIABLE is a pattern -- a rule's condition or
        // consequence, or the query the user has just typed -- and a ground
        // rule pattern is one as well. Nobody asserted either, and every
        // other reader skips both (see Zelph::is_asserted_fact), so counting
        // them made the listings report what no query can reproduce: typing
        // `S p O` once added a use of `p` and reported `O` as a value of it.
        // One snapshot per command, and nullptr when there is nothing to skip.
        const auto skip = _n->unasserted_snapshot();

        // Map to store predicate node and its usage count
        std::map<network::Node, size_t> predicate_usage_counts;

        // Get all predicates directly: nodes that IsA RelationTypeCategory
        auto predicates = _n->get_sources(_n->core.IsA, _n->core.RelationTypeCategory, true);

        // This walks every fact of every predicate, so on a full dump it is a
        // pass over the whole network. The predicate count is the only
        // denominator it has, and it is known here.
        const bool report_predicates = predicates.size() >= predicate_report_step;
        if (report_predicates)
            _n->out("Reading the facts of " + std::to_string(predicates.size()) + " predicate(s)...", true);

        size_t predicates_done = 0;

        for (const auto& pred : predicates)
        {
            // Get all facts where this node is used as a relation type --
            // get_left would additionally count every fact ABOUT the
            // predicate, starting with its own `pred ~ ->` declaration, so a
            // declared but unused predicate reported one use.
            const auto& facts_using_predicate = _n->get_facts_of_predicate(pred);

            size_t asserted = 0;
            for (const network::Node fact : facts_using_predicate)
            {
                if (skip == nullptr || skip->count(fact) == 0) ++asserted;
            }

            predicate_usage_counts[pred] = asserted;

            if (report_predicates && ++predicates_done % predicate_report_step == 0)
            {
                _n->out("  " + std::to_string(predicates_done) + " of "
                            + std::to_string(predicates.size()) + " predicate(s) read",
                        true);
            }
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

            // A composite predicate -- a fact or a cons cell -- has no name,
            // and the column was simply blank, so the listing named a count
            // without naming what it counted. It renders like everything else.
            if (predicate_name.empty())
            {
                std::string repr;
                string::node_to_string(_n, repr, _n->lang(), entry.first, 3);
                predicate_name = string::unmark_identifiers(repr);
            }

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

    void CommandExecutor::Impl::list_predicate_value_usage(const network::Node pred, size_t limit /*= 0*/)
    {
        std::string pred_display = _n->get_name(pred, _n->lang(), true);
        if (pred_display.empty())
        {
            // A composite predicate has no name; it renders like everything
            // else rather than leaving the heading half-written.
            std::string repr;
            string::node_to_string(_n, repr, _n->lang(), pred, 3);
            pred_display = string::unmark_identifiers(repr);
        }

        _n->out("Value Usage for predicate " + pred_display + ":", true);
        _n->out("------------------------", true);

        ankerl::unordered_dense::map<network::Node, size_t> value_counts;

        // Each fact below is visited ONCE and never asked about again, which is
        // the exact shape prune_nodes suspends the cache for -- remembering a
        // structure buys a reuse that never comes and costs an entry per fact.
        // On the full Wikidata dump that is tens of millions of entries: this
        // listing was measured growing its own footprint by 6 MiB/s, ~21 GiB
        // per hour, on a run that was already memory bound and swapping. The
        // memory it takes is the memory the graph needs.
        const network::Zelph::SuspendFactStructureCache no_cache(*_n);

        // All fact nodes that use this predicate as their relation type.
        // get_left would also bring in the facts ABOUT the predicate, whose
        // objects then appeared as values of it -- `-> 1` from the
        // `pred ~ ->` declaration on every predicate there is.
        network::adjacency_set facts = _n->get_facts_of_predicate(pred);

        // Patterns are not values either; see .list-predicate-usage.
        const auto skip = _n->unasserted_snapshot();

        // The size of the job is known HERE, before the expensive half starts,
        // and it is the single most useful thing this command can say: the
        // reconstruction below is a random touch per fact, so on a network that
        // does not fit in RAM the count is the only estimate of what is left.
        const bool report = facts.size() >= listing_report_threshold;
        if (report)
            _n->out("Reading " + std::to_string(facts.size()) + " fact(s) of this predicate...", true);

        size_t facts_read = 0;

        for (network::Node fact : facts)
        {
            if (report && ++facts_read % listing_report_step == 0)
            {
                _n->out("  " + std::to_string(facts_read) + " of " + std::to_string(facts.size())
                            + " fact(s) read",
                        true);
            }

            if (skip != nullptr && skip->count(fact) != 0) continue;

            // The EXACT decomposition, not the adjacency reading. A fact's
            // incoming set holds its subject and objects -- and every fact
            // that uses it as a PREDICATE, which points at it and is not
            // pointed back at, exactly like an object. So `x (a p b) y` made
            // itself a value of `a p b`, and the listing for `p` reported a
            // second, nameless value that nobody had written.
            const network::FactStructure fs = network::get_preferred_structure(_n, fact, 3);

            for (network::Node obj : fs.objects)
            {
                value_counts[obj]++;
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

            // A value without a name -- a nested fact, a list, a set or a
            // collection -- left the column BLANK, so the listing counted
            // something it could not name: "x rel (a p b)" and "z rel <1 2>"
            // both reported a bare "1". The heading and the sibling listing
            // already render such a node (02d1597); this column was missed.
            if (value_name.empty())
            {
                std::string repr;
                string::node_to_string(_n, repr, _n->lang(), entry.second, 3);
                value_name = string::unmark_identifiers(repr);
            }

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

    void CommandExecutor::Impl::cmd_node(const std::vector<std::string>& cmd)
    {
        std::string                arg;
        std::vector<network::Node> nodes;

        if (cmd.size() > 2)
        {
            // More than one token is a printed fact, not a name -- see
            // resolve_node_or_fact. ".node a p b" used to be refused outright
            // with "At most one argument required".
            const network::Node fact = resolve_node_or_fact({cmd.begin() + 1, cmd.end()});
            if (fact == 0)
                throw std::runtime_error("Command .node: expected a name, an ID, or a fact pattern denoting an existing node");
            nodes.push_back(fact);
        }
        else if (cmd.size() == 1)
        {
            network::Node last = string::last_node_to_string_node();
            if (last == network::Node{}) throw std::runtime_error("Command .node: No argument given and no previous output node available");
            nodes.push_back(last);
        }
        else
        {
            arg = cmd[1];

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
        }

        if (nodes.size() == 1)
        {
            bool resolved_from_name = !arg.empty() && (!_n->get_name(nodes[0], _n->lang(), false).empty() || std::all_of(arg.begin(), arg.end(), ::iswdigit));
            display_node_details(nodes[0], resolved_from_name && nodes.size() == 1);
        }
        else
        {
            // nodes.size() > 1 only reachable via name search (arg non-empty)
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

    void CommandExecutor::Impl::cmd_list(const std::vector<std::string>& cmd)
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

    void CommandExecutor::Impl::cmd_clist(const std::vector<std::string>& cmd)
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

    void CommandExecutor::Impl::cmd_connections(const std::vector<std::string>& cmd, bool outgoing)
    {
        if (cmd.size() < 2) throw std::runtime_error(std::string("Command ") + cmd[0] + ": Missing node argument");

        // Same resolve logic as .node: a name, an ID, or a printed fact, with
        // the trailing count separated the way .explain separates its depth.
        size_t              max_count = 20; // default
        const network::Node base_nd   = resolve_node_or_fact({cmd.begin() + 1, cmd.end()}, &max_count);

        if (base_nd == 0)
        {
            throw std::runtime_error("Unknown node");
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

    void CommandExecutor::Impl::cmd_mermaid(const std::vector<std::string>& cmd)
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

    void CommandExecutor::Impl::cmd_list_predicate_usage(const std::vector<std::string>& cmd)
    {
        size_t limit = 0;
        if (cmd.size() > 2) throw std::runtime_error("Command .list-predicate-usage accepts at most one optional argument (max entries)");
        // parse_count, not a hand-rolled stoull: the same negative-wraps-to-
        // huge trap applies here, and one place to get it right is enough.
        if (cmd.size() == 2) limit = string::parse_count(cmd[1]);
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

    void CommandExecutor::Impl::cmd_list_predicate_value_usage(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2)
            throw std::runtime_error("Command .list-predicate-value-usage requires one required argument (<predicate>) and one optional (max entries)");

        // Same resolve logic as .node: a name, an ID, or a printed FACT, with
        // the trailing count separated the way .explain separates its depth.
        // A composite predicate could not be named here at all -- the command
        // saw `.list-predicate-value-usage (a p b)` as three arguments and
        // refused on arity, although a fact in predicate position is exactly
        // what one asks this listing about.
        size_t              limit    = 0;
        const network::Node pred     = resolve_node_or_fact({cmd.begin() + 1, cmd.end()}, &limit);
        std::string         pred_arg = cmd[1];
        for (size_t i = 2; i < cmd.size(); ++i)
            pred_arg += " " + cmd[i];

        if (pred == 0)
            throw std::runtime_error("Unknown predicate '" + pred_arg + "' in current language '" + _n->lang() + "'");

        if (_data_manager)
        {
            _data_manager->set_logging(false);
        }
        list_predicate_value_usage(pred, limit);
        if (_data_manager)
        {
            _data_manager->set_logging(true);
        }
    }

    void CommandExecutor::Impl::cmd_stat(const std::vector<std::string>& cmd)
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

            _n->out_stream() << "Node-of-Name Entries by language:" << std::endl;
            for (const std::string& lang : _n->get_languages())
            {
                _n->out_stream() << "  " << lang << ": " << _n->get_node_of_name_size(lang) << std::endl;
            }
        }

        _n->out_stream() << "Languages: " << _n->language_count() << std::endl;
        _n->out_stream() << "Rules: " << _n->rule_count() << std::endl;

        _n->out_stream() << "------------------------" << std::endl;
    }
}
