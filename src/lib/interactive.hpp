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

namespace zelph::network
{
    class Reasoning;
}

namespace zelph::console
{
    // The command-line interface (REPL). It manages user input, translates commands into operations
    // on the DataManager or zelph instance, and visualizes results. It holds the current state of
    // how the data was loaded via the DataManager.
    class ZELPH_EXPORT Interactive
    {
    public:
        explicit Interactive(io::OutputHandler output = io::default_output_handler);
        ~Interactive();

        void        import_file(const std::string& file) const;
        void        process(std::string line) const;
        void        run(const bool print_deductions, const bool export_derivations, const bool suppress_repetition) const;
        std::string get_lang() const;

        // The REPL prompt, empty while a multi-line statement, keyword block or
        // Janet block is still being read. It lives here rather than in the
        // binary because there are two front ends -- the terminal and the wasm
        // playground -- and a prompt built twice is a prompt that drifts.
        //
        // What it carries besides the language: "-" before the ">" when
        // auto-run is off, and "+" when deductions are being withheld without
        // a notice saying so. Both mark a state the user set and would
        // otherwise have to remember.
        std::string prompt_text() const;

        // Tell the library whether a prompt will follow each run. False for a
        // caller that shows none -- `zelph script.zph` -- where the mark of
        // the default deduction mode would have nowhere to appear, so a run
        // states in words what it hid instead of leaving a log that does not
        // say it is incomplete.
        void               set_prompt_available(bool available) const;
        static std::string get_version();
        bool               is_auto_run_active() const;
        bool               is_accumulating() const;

        // Dispatch whatever process() is still accumulating. Call this when
        // the input ENDS: a piped session or a script that stops inside a
        // keyword block, a multi-line statement or a Janet block would
        // otherwise discard it without a word -- and "printf ... | zelph" is
        // how zelph gets verified.
        void finish_input() const;

        // Run a script named on the command line. It is a SESSION, not a
        // library load: its lines are echoed, its statements anchor the
        // deduction focus, and auto-run fires after each of them -- so
        // `zelph script.zph` and `zelph < script.zph` produce the same
        // reasoning and the same output. What it keeps from `.import` is the
        // path resolution (including the standard library), the `.janet`
        // whole-program runner, the script arguments and the module guard.
        void process_file(const std::string& file, const std::vector<std::string>& args = {}) const;

        // Whether anything has FAILED in this session: a command that threw
        // and was reported, or an import that was declined. The binary turns
        // it into its exit status. Not cleared by `.new`, and deliberately not
        // derived from the Error output channel, which also carries warnings
        // about operations the documentation calls legitimate.
        bool had_failure() const;

        // Record a failure the library cannot see -- what a caller caught and
        // reported itself, which for the binary is every error the REPL loop
        // survives.
        void note_failure() const;

        // The graph this REPL drives, for a caller that embeds zelph rather
        // than typing at it. Non-owning, and NOT stable across .new, which
        // rebuilds the network.
        network::Reasoning* graph() const;

        // Execute an already tokenized dot-command, e.g. {".load", "a b.bin"}.
        // Same path as process(), minus the tokenizer, so a file name
        // containing spaces stays one argument. This is what zelph/save and
        // zelph/load use from Janet, and what the C ABI uses.
        void execute_command(const std::vector<std::string>& cmd) const;

        void set_output_handler(io::OutputHandler output) const;
        void out(const std::string& text, bool newline = true) const;
        void err(const std::string& text, bool newline = true) const;
        void log(const std::string& text, bool newline = true) const;
        void prompt(const std::string& text, bool newline = false) const;

        // Returns the path of the most recently generated Mermaid HTML file
        // and clears it (take semantics), or an empty string if none was
        // generated since the last call. Used by the wasm playground.
        std::string take_last_graph_html() const;

        Interactive(const Interactive&)            = delete;
        Interactive& operator=(const Interactive&) = delete;

    private:
        class Impl;
        Impl* const _pImpl;
    };
}
