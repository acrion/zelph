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

#include "network/reasoning.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace zelph;

namespace zelph::console
{
    void CommandExecutor::Impl::cmd_cluster(const std::vector<std::string>& cmd)
    {
        if (cmd.size() == 1)
        {
            const std::string active = _n->active_cluster_name();
            _n->out("Active cluster: " + (active.empty() ? "default" : active), true);
            for (const auto& [name, count] : _n->list_clusters())
                _n->out("  " + name + ": " + std::to_string(count) + " node(s)", true);
            return;
        }
        if (cmd.size() != 2) throw std::runtime_error("Usage: .cluster [name]");

        if (cmd[1] == "default")
        {
            _n->deactivate_cluster();
            _n->out("Active cluster: default", true);
        }
        else
        {
            _n->set_active_cluster(cmd[1]);
            _n->out("Active cluster: " + cmd[1], true);
        }
    }

    void CommandExecutor::Impl::cmd_cluster_drop(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 2) throw std::runtime_error("Usage: .cluster-drop <name>");
        if (cmd[1] == "default") throw std::runtime_error(".cluster-drop: the default cluster cannot be dropped");

        // An unknown name is an error, exactly as in .cluster-merge -- and
        // it has to be checked here, because "removed 0 node(s)" is also the
        // honest report for a cluster that exists but is empty. Reporting a
        // typo as a successful rollback is how an experiment silently keeps
        // running against a cluster the user believes is gone.
        const auto clusters = _n->list_clusters();
        if (std::none_of(clusters.begin(), clusters.end(), [&](const auto& c)
                         { return c.first == cmd[1]; }))
            throw std::runtime_error(".cluster-drop: unknown cluster '" + cmd[1] + "'");

        const bool   was_active = _n->active_cluster_name() == cmd[1];
        const size_t removed    = _n->drop_cluster(cmd[1]);
        _n->out("Dropped cluster " + cmd[1] + ": removed " + std::to_string(removed) + " node(s).", true);
        if (was_active) _n->out("Active cluster: default", true);
    }

    void CommandExecutor::Impl::cmd_cluster_merge(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 3) throw std::runtime_error("Usage: .cluster-merge <from> <to>  (to may be 'default')");
        const std::string to = (cmd[2] == "default") ? "" : cmd[2];
        if (!_n->merge_cluster(cmd[1], to))
            throw std::runtime_error(".cluster-merge: unknown cluster '" + cmd[1] + "'");
        _n->out("Merged cluster " + cmd[1] + " into " + cmd[2] + ".", true);
    }

    void CommandExecutor::Impl::restore_cluster(const std::string& name) const
    {
        if (name.empty() || name == "default")
            _n->deactivate_cluster();
        else
            _n->set_active_cluster(name);
    }
}
