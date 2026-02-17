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

#include "network_types.hpp" // For network::Node

#include <string>
#include <vector>

namespace zelph
{
    namespace network
    {
        class Reasoning;
    }

    class ScriptEngine
    {
    public:
        // Pass the reasoning engine so the script can operate on the graph
        explicit ScriptEngine(network::Reasoning* reasoning);
        ~ScriptEngine();

        // Setup the Janet environment and PEG grammar
        void initialize();

        // Parse Zelph syntax to Janet AST
        std::string parse_zelph_to_janet(const std::string& input) const;

        // Execute Janet code (either raw or transformed Zelph AST)
        // is_zelph_ast determines how the output is handled/printed
        void process_janet(const std::string& code, bool is_zelph_ast);

        // Evaluate an expression and return a single Node (used for patterns/pruning)
        network::Node evaluate_expression(const std::string& janet_code);

        // Inject arguments into the script environment (for script files with args)
        void set_script_args(const std::vector<std::string>& args);

        // Check whether a Janet code fragment has balanced delimiters
        // (parentheses, brackets, braces), respecting strings and comments.
        // Returns true when the expression is syntactically complete.
        static bool is_expression_complete(const std::string& code);

        static bool is_var(std::wstring token);

        ScriptEngine(const ScriptEngine&)            = delete;
        ScriptEngine& operator=(const ScriptEngine&) = delete;

    private:
        class Impl;
        Impl* const _pImpl;
    };
}
