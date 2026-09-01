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

#ifndef __EMSCRIPTEN__
    #include "wikidata/wikidata.hpp"
#endif

#include <filesystem>
#include <string>
#include <vector>

using namespace zelph;

namespace zelph::console
{
#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_wikidata_constraints(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 3) throw std::runtime_error("Command .wikidata-constraints: Missing json file name or directory name");
        if (cmd.size() > 3) throw std::runtime_error("Command .wikidata-constraints: Unknown argument after directory name");

        chrono::StopWatch watch;
        watch.start();

        const std::string&    dir        = cmd[2];
        std::filesystem::path input_path = cmd[1];

        // Up front, and with the whole path: the export runs one entity at a
        // time on worker threads, where a filesystem_error is not a message
        // but a std::terminate. A nested target directory used to abort the
        // process the moment the first property entity arrived.
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec && !std::filesystem::is_directory(dir))
        {
            throw std::runtime_error("Command .wikidata-constraints: cannot create output directory '"
                                     + dir + "': " + ec.message());
        }

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
#endif

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_wikidata_qualifiers(const std::vector<std::string>& cmd)
    {
        if (cmd.size() < 2) throw std::runtime_error("Command .wikidata-qualifiers: Missing json file name");

        if (_repl_state->partial_load_mode)
        {
            throw std::runtime_error("Command .wikidata-qualifiers: blocked while a partial/incomplete graph is loaded");
        }

        std::vector<std::string> qualifier_properties(cmd.begin() + 2, cmd.end());
        for (const auto& p : qualifier_properties)
        {
            bool valid = p.size() >= 2 && p[0] == 'P';
            for (size_t i = 1; valid && i < p.size(); ++i)
            {
                valid = p[i] >= '0' && p[i] <= '9';
            }
            if (!valid)
            {
                throw std::runtime_error("Command .wikidata-qualifiers: '" + p
                                         + "' is not a Wikidata property ID (expected P<number>)");
            }
        }

        if (_repl_state->auto_run)
        {
            _repl_state->auto_run = false;
            _n->out("Auto-run has been disabled due to importing a large dataset.", true);
        }

        chrono::StopWatch watch;
        watch.start();

        // Local manager on purpose: _data_manager stays associated with the
        // network loaded via .load.
        auto data_manager = io::DataManager::create(_n, cmd[1]);
        auto wikidata_mgr = std::dynamic_pointer_cast<wikidata::Wikidata>(data_manager);
        if (!wikidata_mgr)
        {
            throw std::runtime_error("Command .wikidata-qualifiers: '" + cmd[1]
                                     + "' is not a Wikidata JSON dump (.json or .json.bz2)");
        }

        wikidata_mgr->import_qualifiers(qualifier_properties);

        watch.stop();
        _n->diagnostic(" Time needed for qualifier import: " + watch.format(), true);
    }
#endif

#ifndef __EMSCRIPTEN__
    void CommandExecutor::Impl::cmd_export_wikidata(const std::vector<std::string>& cmd)
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
#endif
}
