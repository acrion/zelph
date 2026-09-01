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

#include <string>
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
        if (cmd.size() > 2) throw std::runtime_error("Usage: .deductions [all|focus|off]");

        if (cmd.size() >= 2)
        {
            if (cmd[1] == "all")
            {
                _repl_state->deduction_mode = DeductionMode::All;
                _n->clear_input_focus();
            }
            else if (cmd[1] == "focus")
            {
                _repl_state->deduction_mode = DeductionMode::Focus;
                _n->clear_input_focus();
            }
            else if (cmd[1] == "off")
            {
                _repl_state->deduction_mode = DeductionMode::Off;
                _n->clear_input_focus();
            }
            else
            {
                throw std::runtime_error("Usage: .deductions [all|focus|off]");
            }
        }
        _n->set_deduction_filter(_repl_state->deduction_mode == DeductionMode::Focus);
        const char* name = _repl_state->deduction_mode == DeductionMode::All   ? "all"
                         : _repl_state->deduction_mode == DeductionMode::Focus ? "focus"
                                                                               : "off";
        _n->out("Deduction printing mode: " + std::string(name), true);
    }
}
