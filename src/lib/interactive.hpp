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

#include "io/output.hpp"

#include <zelph_export.h>

#include <string>
#include <vector>

namespace zelph::console
{
    // The command-line interface (REPL). It manages user input, translates commands into operations
    // on the DataManager or Zelph instance, and visualizes results. It holds the current state of
    // how the data was loaded via the DataManager.
    class ZELPH_EXPORT Interactive
    {
    public:
        explicit Interactive(io::OutputHandler output = io::default_output_handler);
        ~Interactive();

        void               import_file(const std::string& file) const;
        void               process(std::string line) const;
        void               run(const bool print_deductions, const bool generate_markdown, const bool suppress_repetition) const;
        std::string        get_lang() const;
        static std::string get_version();
        bool               is_auto_run_active() const;
        bool               is_accumulating() const;
        void               process_file(const std::string& file, const std::vector<std::string>& args = {}) const;

        void set_output_handler(io::OutputHandler output) const;
        void out(const std::string& text, bool newline = true) const;
        void err(const std::string& text, bool newline = true) const;
        void log(const std::string& text, bool newline = true) const;
        void prompt(const std::string& text, bool newline = false) const;

        Interactive(const Interactive&)            = delete;
        Interactive& operator=(const Interactive&) = delete;

    private:
        class Impl;
        Impl* const _pImpl;
    };
}
