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
#include "network/reasoning.hpp"
#include "repl_state.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace zelph;

namespace zelph::console
{
    void CommandExecutor::Impl::cmd_run(const std::vector<std::string>&)
    {
        require_full_graph_mode(".run");
        _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, false);
        _n->diagnostic("Ready.", true);
    }

    void CommandExecutor::Impl::cmd_run_once(const std::vector<std::string>&)
    {
        require_full_graph_mode(".run-once");
        _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, true);
        _n->diagnostic("Ready.", true);
    }

    void CommandExecutor::Impl::cmd_run_delta(const std::vector<std::string>&)
    {
        require_full_graph_mode(".run-delta");
        _n->run(_repl_state->deduction_mode != DeductionMode::Off, false, false, false, true);
        _n->diagnostic("Ready.", true);
    }

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_run_export(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".run-export");
        if (cmd.size() != 2)
            throw std::runtime_error("Command .run-export requires exactly one argument: the output file path");

        _n->set_export_file(cmd[1]);
        _n->diagnostic("Running full inference; derivations are written to " + cmd[1] + " as JSON Lines.", true);

        // Rendering every derived term to the console dominates the cost of
        // a large export, and the file is the point of the command.
        if (_data_manager) _data_manager->set_logging(false);

        _n->run(false, true, false);
        _n->diagnostic("Ready.", true);
    }
#endif

    void CommandExecutor::Impl::cmd_list_rules(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .list-rules takes no arguments");

        // Get all nodes that are subjects of a core.Causes() relation
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
            string::node_to_string(_n, output, _n->lang(), rule, 3);

            // node_to_string leaves the identifier markers in; every other
            // command strips them. Printing them made the listing the one
            // place where zelph shows its internals -- and the one place
            // where a rule could not be copied back in, because a
            // multi-word predicate came out as «is part of» rather than
            // quoted.
            _n->out(string::unmark_identifiers(output), true);
        }
        _n->out("------------------------", true);
    }

    void CommandExecutor::Impl::cmd_strata(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .strata takes no arguments");

        if (_n->get_rules().empty())
        {
            _n->out("No rules found.", true);
            return;
        }

        const network::Reasoning::NegationLevels strata = _n->strata();
        if (strata.levels == 0)
        {
            _n->out("No rule has a negated condition, so there are no negation levels.", true);
            return;
        }

        // Rendered as .list-rules renders them, ensuring that a rule
        // appears identically in both listings. The text serves as the sort
        // key: rules are drawn from a hash set, and the listing must remain
        // consistent across every call.
        const auto render = [this](const network::Node rule)
        {
            std::string output;
            string::node_to_string(_n, output, _n->lang(), rule, 3);
            return string::unmark_identifiers(output);
        };

        std::vector<std::vector<std::string>>                                   by_level(strata.levels);
        std::map<std::size_t, std::pair<std::size_t, std::vector<std::string>>> cycles; // component -> (level, rules)
        for (std::size_t i = 0; i < strata.rules.size(); ++i)
        {
            const std::size_t c      = strata.component[i];
            const bool        cyclic = strata.negation_inside[c];
            if (!strata.negates[i] && !cyclic) continue;

            const std::string text = render(strata.rules[i]);
            if (strata.negates[i])
            {
                by_level[strata.level[i]].push_back(text);
                if (cyclic) cycles[c].first = strata.level[i];
            }
            if (cyclic) cycles[c].second.push_back(text);
        }

        const std::size_t levels = strata.levels;
        _n->out(std::to_string(levels) + (levels == 1 ? " negation level:" : " negation levels:"), true);
        for (std::size_t level = 0; level < levels; ++level)
        {
            std::sort(by_level[level].begin(), by_level[level].end());
            _n->out("Level " + std::to_string(level + 1) + ":", true);
            for (const std::string& rule : by_level[level])
                _n->out("  " + rule, true);
        }

        // A component that contains a negation negates what it itself
        // derives. Each is presented in full, as its positive rules
        // constitute just as integral a part of the cycle as those that
        // perform the negation.
        std::vector<std::pair<std::size_t, std::vector<std::string>>> unstratified;
        for (auto& [c, entry] : cycles)
        {
            std::sort(entry.second.begin(), entry.second.end());
            unstratified.push_back(std::move(entry));
        }
        std::sort(unstratified.begin(), unstratified.end());
        for (const auto& [level, rules] : unstratified)
        {
            _n->out("Not stratifiable, on level " + std::to_string(level + 1) + " -- these rules negate what they derive:", true);
            for (const std::string& rule : rules)
                _n->out("  " + rule, true);
        }
    }

    void CommandExecutor::Impl::cmd_remove_rules(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1) throw std::runtime_error("Command .remove-rules takes no arguments");
        require_full_graph_mode(".remove-rules");
        _n->remove_rules();
        _n->out("All rules removed.", true);
    }

    void CommandExecutor::Impl::cmd_auto_run(const std::vector<std::string>& cmd)
    {
        // A toggle standing among .anchors/.semi-naive/.fact-stores, which
        // all take [on|off]. Silently ignoring an argument meant that
        // ".auto-run off" ENABLED auto-run whenever it happened to be off.
        if (cmd.size() != 1) throw std::runtime_error("Usage: .auto-run  (a toggle; it takes no argument)");
        _repl_state->auto_run = !_repl_state->auto_run;
        _n->out("Auto-run is now " + std::string(_repl_state->auto_run ? "enabled" : "disabled") + ".", true);
    }

    void CommandExecutor::Impl::cmd_deductions(const std::vector<std::string>& cmd)
    {
        constexpr const char* usage = "Usage: .deductions [all|focus|quiet|off]";
        if (cmd.size() > 2) throw std::runtime_error(usage);

        bool named = false;
        if (cmd.size() >= 2)
        {
            named = true;
            if (cmd[1] == "all")
                _repl_state->deduction_mode = DeductionMode::All;
            else if (cmd[1] == "focus")
                _repl_state->deduction_mode = DeductionMode::Focus;
            else if (cmd[1] == "quiet")
                _repl_state->deduction_mode = DeductionMode::Quiet;
            else if (cmd[1] == "off")
                _repl_state->deduction_mode = DeductionMode::Off;
            else
                throw std::runtime_error(usage);

            _n->clear_input_focus();
        }

        const DeductionMode mode = _repl_state->deduction_mode;
        _n->set_deduction_filter(filters_deductions(mode));
        _n->set_deduction_notice(announces_withheld(*_repl_state));

        const char* name = mode == DeductionMode::All   ? "all"
                         : mode == DeductionMode::Focus ? "focus"
                         : mode == DeductionMode::Quiet ? "quiet"
                                                        : "off";
        _n->out("Deduction printing mode: " + std::string(name), true);

        // Naming a mode is where its effect gets explained, and now the only
        // place. A run in the mode says at most how much it hid: telling
        // somebody after every answer how to reach the mode they are already
        // in is noise, and it is noise in the middle of the result they asked
        // for.
        if (named)
        {
            // The mark is the '+', and the prompt around it is not fixed: its
            // first part is the current language, which .lang changes. So the
            // whole prompt is shown as an EXAMPLE and the character is named
            // as the thing that carries the meaning.
            const std::string mark = "marks the prompt with '+' (e.g. \"" + _n->get_lang() + "+> \")";

            switch (mode)
            {
            case DeductionMode::All:
                _n->out("  Every derivation is printed.", true);
                break;
            case DeductionMode::Focus:
                _n->out("  Only derivations about statements you entered are printed, and a run says how many it hid.", true);
                break;
            case DeductionMode::Quiet:
                _n->out("  Only derivations about statements you entered are printed, and a run that hid any " + mark + ".", true);
                break;
            case DeductionMode::Off:
                _n->out("  No derivations are printed; a run says how many it hid and " + mark + ".", true);
                break;
            }
        }
    }
}
