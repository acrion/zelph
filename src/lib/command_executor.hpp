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

#pragma once

#include "repl_state.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declarations to avoid heavy includes in the header
namespace zelph
{
    namespace console
    {
        class DataManager;
    }
    class ScriptEngine;
    namespace network
    {
        class Reasoning;
    }
}

namespace zelph::console
{
    /**
     * @brief Handles the execution of dot-commands (e.g., .help, .load, .run).
     *
     * This class encapsulates the logic for all interactive commands to keep
     * the main Interactive class clean. It uses the Pimpl idiom.
     */
    class CommandExecutor
    {
    public:
        using LineProcessor = std::function<void(const std::wstring&)>;

        /**
         * @brief Constructs the executor with references to the system components.
         *
         * @param reasoning Pointer to the reasoning network (must remain valid).
         * @param script_engine Pointer to the script engine (must remain valid).
         * @param data_manager Reference to the shared_ptr holding the data manager (allows replacement).
         * @param line_processor Callback to process a raw line (used for .import recursion).
         */
        CommandExecutor(zelph::network::Reasoning*    reasoning,
                        zelph::ScriptEngine*          script_engine,
                        std::shared_ptr<DataManager>& data_manager,
                        std::shared_ptr<ReplState>    repl_state,
                        LineProcessor                 line_processor);

        ~CommandExecutor();

        /**
         * @brief Executes a command.
         * @param cmd The tokenized command arguments (e.g., {".help", "load"}).
         */
        void execute(const std::vector<std::wstring>& cmd);

        // Non-copyable due to internal state references
        CommandExecutor(const CommandExecutor&)            = delete;
        CommandExecutor& operator=(const CommandExecutor&) = delete;

    private:
        class Impl;
        std::unique_ptr<Impl> _pImpl;
    };
}
