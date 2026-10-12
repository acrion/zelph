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

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace zelph::console
{
    enum class ScriptMode
    {
        Zelph,
        Janet
    };

    enum class DeductionMode
    {
        All,   // print every deduction (historic behavior)
        Focus, // print only deductions anchored in the current input
        Quiet, // as Focus, but the prompt carries what a notice would say
        Off    // print no deductions
    };

    // When the REPL prints the time an input took (`.timer`).
    enum class TimerMode
    {
        Auto, // only following an input that consumed a discernible duration
        On,   // following each input
        Off   // never
    };

    // Whether the mode removes deductions from the output. Quiet differs from
    // Focus in what it SAYS, not in what it shows, so every decision about
    // filtering has to treat the two alike.
    inline bool filters_deductions(const DeductionMode mode)
    {
        return mode == DeductionMode::Focus || mode == DeductionMode::Quiet;
    }

    // Whether the prompt carries the mark. Only where deductions are being
    // withheld WITHOUT a notice saying so, plus `off`, where the user asked
    // for silence and the prompt is the only place left to say that something
    // is being kept back -- the same job the "-" of a disabled auto-run does.
    inline bool marks_withheld_in_prompt(const DeductionMode mode)
    {
        return mode == DeductionMode::Quiet || mode == DeductionMode::Off;
    }

    // What a script file IS to the engine, which decides how it is run.
    //
    // A MODULE is a library load: `.import`, `zelph/import`, and every nested
    // import below them. It defines things for the session around it, so its
    // own lines are not echoed, its statements are not focus anchors, and
    // auto-run waits until the whole file has been read.
    //
    // A SESSION is a file named on the command line -- `zelph script.zph`, and
    // the shebang `#!/usr/bin/env zelph` that points at it. It is a typed
    // session that happens to arrive from a file rather than from a keyboard,
    // so it runs exactly as `zelph < script.zph` does.
    // What a session keeps from a module is everything about FINDING and
    // preparing the file: standard-library path resolution, the `.janet`
    // whole-program runner, script arguments, and the import-once module guard.
    enum class ScriptRole
    {
        Module,
        Session
    };

    struct ReplState
    {
        bool auto_run{true};

        // Quiet by default: the filtering is the same as Focus, and what
        // differs is that the run does not write three lines of prose after
        // the answer somebody asked for. Whoever wants those lines chooses
        // Focus, and choosing it is also what tells them what it means.
        DeductionMode deduction_mode{DeductionMode::Quiet};

        // As with deduction mode, this setting belongs to the session
        // rather than the engine, so `.new` keeps it: it decides what the
        // person at the REPL sees, and the network remains silent on this
        // matter. Read by Interactive::report_timing, which both front ends
        // call.
        TimerMode timer_mode{TimerMode::Auto};

        // Whether a prompt will be shown after each run. False for
        // `zelph script.zph`, which prints none -- and there the mark has
        // nowhere to appear, so Quiet falls back to the notice (see
        // announces_withheld). Set once by the binary before it runs.
        bool prompt_available{true};
#ifndef __EMSCRIPTEN__
        bool        partial_load_mode{false};
        std::string partial_load_source;
#endif
        ScriptMode  script_mode{ScriptMode::Zelph};
        std::string janet_buffer;                     // Accumulates incomplete Janet expressions
        bool        accumulating_inline_janet{false}; // True while a % expression spans multiple lines
        std::string zelph_buffer;
        bool        accumulating_zelph{false};
        bool        reset_requested{false};
        bool        accumulating_keyword = false;
        std::string active_keyword;
        std::string keyword_buffer;
        bool        keyword_prev_blank = false;

        // Absolute path of the most recently generated Mermaid HTML file
        // (graph of an output node). Consumed via Interactive::take_last_graph_html();
        // the wasm playground polls it after every command batch instead of
        // parsing file:// links out of the terminal output.
        std::string last_graph_html_path;

        // Depth of nested MODULE processing. 0 = the session: interactive
        // input, piped input, or the lines of a script named on the command
        // line. Managed strictly by RAII in CommandExecutor::import_file, and
        // raised only for ScriptRole::Module; deliberately NOT reset by .new
        // (the guards on the stack restore it correctly even when .new is
        // issued from inside a script).
        int import_depth{0};

        // Whether the lines of a SESSION script are currently being read. The
        // loop that reads them is part of the command executor, and `.new`
        // replaces both the executor and the engine: carried out at the point
        // of reading, it destroyed the loop that was still running, causing
        // `zelph script.zph` containing a `.new` to crash with a segmentation
        // fault after its last line. Thus, while this state is set, `.new`
        // merely records the request; the loop stops after that line, and
        // Interactive::process_file clears the network and proceeds to read
        // from the subsequent line.
        bool reading_session_script{false};

        // Import guard ("import once"): associates each registered module
        // ID with the default ID (the lowercase filename stem) of the
        // script that registered it. A script registers its default ID
        // along with every ID it specifies through .provides. Cleared by
        // .new, and by a load operation that replaces the graph except for
        // the scripts listed in modules_importing.
        std::map<std::string, std::string> imported_module_ids;

        // The default IDs of the .zph scripts currently being imported,
        // with the innermost one listed last. Managed strictly by RAII
        // within the CommandExecutor::import_file function.
        std::vector<std::string> modules_importing;

        // Depth of quiet processing scopes (the '?' result-query pre-pass):
        // while > 0, the input echo is suppressed like inside imports.
        // Managed strictly by RAII in Interactive::process.
        int quiet_depth{0};

        // Something went wrong that the caller has to be able to notice
        // without reading the log. main() turns this into the process's exit
        // status, which used to be 0 whatever happened -- so a script whose
        // `.import` did not resolve reported success, and the session it
        // described was one nobody had asked for.
        //
        // Set at the places that MEAN failure, deliberately not by counting
        // Error-channel events: that channel also carries the partial-view
        // warning, and mkdocs/docs/sharding.md documents the operation it
        // accompanies as legitimate ("Nothing is undone"). A count would
        // report the documented sharding workflow as a failed run.
        //
        // Never cleared, and `.new` does not clear it either: `.new` resets
        // the graph, not what the session has already got wrong.
        bool failed{false};

        // `.quit` arrives at the command table, where its handler merely
        // records it, leaving the decision to the loop that owns the input.
        // The REPL loop within main() intercepts a plain `.quit` before
        // process(), which explains why typing one inside a keyword block
        // still ends the session; a `.quit` that the check does not recognize,
        // like `.quit now`, is directed here, and main() respects it following
        // process() via Interactive::quit_requested(). Similarly, the loop
        // that reads a SESSION script (refer to ScriptRole) honours it,
        // enabling a script to conclude before its final line, just as a piped
        // session can. The loop that reads a module clears it and continues,
        // as a library does not end the session that loads it -- and this is
        // precisely where a `.quit` reached from Janet ends up, since
        // `zelph/import` loads a module.
        bool quit_requested{false};
    };

    // Whether a run writes the notice accounting for what it withheld.
    //
    // The mark and the notice are two ways of saying one thing, and which one
    // fits depends on the medium. Quiet asks for the prompt rather than the
    // sentence -- but `zelph script.zph` prints no prompt, so there the
    // "rather than" has nothing to point at, and a quiet run would be a SILENT
    // one: a log that does not say it is incomplete, which is the failure the
    // notice exists to prevent.
    inline bool announces_withheld(const ReplState& state)
    {
        return state.deduction_mode != DeductionMode::Quiet || !state.prompt_available;
    }

    // Helper RAII struct to temporarily suspend auto-run
    struct AutoRunSuspender
    {
        std::shared_ptr<ReplState> state;
        bool                       previous_val;

        explicit AutoRunSuspender(std::shared_ptr<ReplState> s)
            : state(std::move(s))
        {
            if (state)
            {
                previous_val    = state->auto_run;
                state->auto_run = false;
            }
        }

        ~AutoRunSuspender()
        {
            if (state)
            {
                state->auto_run = previous_val;
            }
        }

        bool was_active() const
        {
            return previous_val;
        }
    };
}
