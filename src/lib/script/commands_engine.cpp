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
#include "script/script_engine.hpp"

#include <string>
#include <vector>

using namespace zelph;

namespace zelph::console
{
    void CommandExecutor::Impl::cmd_log(const std::vector<std::string>& cmd)
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

    void CommandExecutor::Impl::cmd_log_janet(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .log-janet takes no arguments");

        _script_engine->toggle_janet_logging();
        _n->out("Janet function logging is now " + _script_engine->get_janet_logging_status() + ".", true);
    }

    void CommandExecutor::Impl::cmd_prof(const std::vector<std::string>& cmd)
    {
        if (cmd.size() > 2 || (cmd.size() == 2 && cmd[1] != "reset"))
            throw std::runtime_error("Usage: .prof [reset]");
        _n->profiler_dump(cmd.size() == 2);
    }

    void CommandExecutor::Impl::cmd_parallel(const std::vector<std::string>& cmd)
    {
        if (cmd.size() != 1)
            throw std::runtime_error("Command .parallel takes no arguments");

        _n->toggle_parallel();
        _n->out("Parallel processing is now " + std::string(_n->use_parallel() ? "enabled" : "disabled") + ".", true);
    }

    void CommandExecutor::Impl::cmd_anchors(const std::vector<std::string>& cmd)
    {
        if (cmd.size() == 1)
        {
            _n->out(std::string("Anchor-based lookups: ") + (_n->use_anchors() ? "on" : "off"), true);
            return;
        }
        if (cmd.size() != 2 || (cmd[1] != "on" && cmd[1] != "off"))
            throw std::runtime_error("Usage: .anchors [on|off]");

        _n->set_anchors(cmd[1] == "on");
        _n->out(std::string("Anchor-based lookups: ") + (_n->use_anchors() ? "on" : "off"), true);
    }

    void CommandExecutor::Impl::cmd_semi_naive(const std::vector<std::string>& cmd)
    {
        auto status = [this]() -> std::string
        {
            if (!_n->seminaive()) return "off";
            return _n->seminaive_check() ? "check" : "on";
        };

        if (cmd.size() == 1)
        {
            _n->out("Semi-naive evaluation: " + status(), true);
            return;
        }

        if (cmd.size() != 2)
            throw std::runtime_error("Usage: .semi-naive [on|off|check]");

        if (cmd[1] == "on")
        {
            _n->set_seminaive(true);
            _n->set_seminaive_check(false);
        }
        else if (cmd[1] == "off")
        {
            _n->set_seminaive(false);
            _n->set_seminaive_check(false);
        }
        else if (cmd[1] == "check")
        {
            _n->set_seminaive(true);
            _n->set_seminaive_check(true);
        }
        else
        {
            throw std::runtime_error("Usage: .semi-naive [on|off|check]");
        }

        _n->out("Semi-naive evaluation: " + status(), true);
    }

    void CommandExecutor::Impl::cmd_fact_stores(const std::vector<std::string>& cmd)
    {
        if (cmd.size() == 1)
        {
            _n->out(std::string("Fact-path stores: ") + (_n->fact_stores_enabled() ? "on" : "off"), true);
            return;
        }
        if (cmd.size() != 2 || (cmd[1] != "on" && cmd[1] != "off"))
            throw std::runtime_error("Usage: .fact-stores [on|off]");

        if (cmd[1] == "off")
        {
            _n->disable_fact_stores();
            _n->out("Fact-path stores: off", true);
            return;
        }

        if (_n->fact_stores_enabled())
            _n->out("Fact-path stores: on", true);
        else
            throw std::runtime_error(".fact-stores on: the stores cannot be re-armed once disabled, "
                                     "because absence of an entry is meaningful while a store is "
                                     "authoritative. Use .new to start with a fresh engine and stores enabled again.");
    }

    void CommandExecutor::Impl::cmd_contradiction_records(const std::vector<std::string>& cmd)
    {
        if (cmd.size() == 1)
        {
            _n->out(std::string("Contradiction records: ") + (_n->record_contradictions() ? "on" : "off"), true);
            return;
        }
        if (cmd.size() != 2 || (cmd[1] != "on" && cmd[1] != "off"))
            throw std::runtime_error("Usage: .contradiction-records [on|off]");

        // Re-armable, unlike .fact-stores: absence of a record is not
        // meaningful here. Switching back on simply means the contradictions
        // found from now on are written down, and the ones found while it was
        // off are reported again the next time they are found.
        _n->record_contradictions(cmd[1] == "on");
        _n->out(std::string("Contradiction records: ") + cmd[1], true);
    }
}
