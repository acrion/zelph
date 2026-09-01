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

#include <string>
#include <vector>

using namespace zelph;

namespace zelph::console
{
    void CommandExecutor::Impl::cmd_lang(const std::vector<std::string>& cmd)
    {
        if (cmd.size() > 2) throw std::runtime_error("Usage: .lang [code]");

        if (cmd.size() < 2)
        {
            _n->out_stream() << "The current language is '" << _n->get_lang() << "'" << std::endl;
        }
        else
        {
            _n->set_lang(cmd[1]);
        }
    }

    void CommandExecutor::Impl::cmd_name(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".name");
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
            else if (node_in_target_lang == node_in_current_lang)
            {
                // Renaming a node to the name it already carries. Refusing
                // it complained about the node itself -- "Name 'a' is
                // already in use by node 11" where 11 IS 'a'.
                _n->out("Node '" + name_in_current_lang + "' already has this name in language '" + target_lang + "'.", true);
            }
            else if (node_in_target_lang != 0
                     && network::Zelph::is_var(node_in_target_lang)
                     && !network::Zelph::is_var(node_in_current_lang))
            {
                // A rule's VARIABLE holds the name. Refusing here told the
                // user something the engine does not believe -- names are
                // not unique per language among variables, every statement
                // makes a fresh one -- and the advice ("remove the other
                // node") named a node whose removal damages the rule.
                //
                // What the parser does with the same collision is the
                // answer: the atom takes the lookup over, the variable keeps
                // its own display name, and nothing merges (a variable and a
                // non-variable cannot). merge_on_conflict is therefore off;
                // with it on, the merge machinery would refuse the merge it
                // is not being asked for.
                _n->set_name(node_in_current_lang, name_in_target_lang, target_lang, false);
                _n->out("Name '" + name_in_target_lang + "' was the display name of a variable. Node '"
                            + name_in_current_lang + "' carries it now; the variable keeps rendering under it.",
                        true);
            }
            else if (node_in_target_lang != 0)
            {
                throw std::runtime_error("Name '" + name_in_target_lang + "' is already in use by node " + std::to_string(node_in_target_lang)
                                         + ". Names are unique per language; remove the other node or use a different name.");
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
        else if (node_in_current_lang == node_in_target_lang)
        {
            // The alias is already in place, which is what running the same
            // script a second time looks like: a .name line that established
            // the alias once resolves to the SAME node from both sides now.
            // The merge branch below then announced "are different nodes =>
            // Merging them" about one node merging with itself -- a script
            // re-imported into a live session reported a structural repair
            // that never happened. The same mistake the same-language branch
            // above already answers for a plain rename.
            _n->out("Node '" + name_in_current_lang + "' ('" + current_lang + "') is already the node named '"
                        + name_in_target_lang + "' in '" + target_lang + "'.",
                    true);
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

    void CommandExecutor::Impl::cmd_delname(const std::vector<std::string>& cmd)
    {
        require_full_graph_mode(".delname");
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
}
