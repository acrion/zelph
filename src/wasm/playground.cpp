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

// zelph WebAssembly playground - C ABI shim around the interactive REPL.
//
// Wraps console::Interactive (the full REPL brain: commands, multi-line
// accumulation, inline Janet, auto-run) behind a few extern "C" entry
// points called from JavaScript via ccall/cwrap. Output currently goes
// through the default output handler (stdout/stderr), which Emscripten
// forwards to console.log/console.error - sufficient for the Node smoke
// test (M1). M2 adds a registered JS callback based on io::OutputHandler.

// zelph WebAssembly playground - C ABI shim around the interactive REPL.
//
// Wraps console::Interactive (the full REPL brain: commands, multi-line
// accumulation, inline Janet, auto-run) behind a few extern "C" entry
// points called from JavaScript via ccall/cwrap.
//
// Output: an io::OutputHandler forwards every OutputEvent (channel, text,
// newline) to an optional JS callback Module["zelphOutput"]. If no callback
// is registered (e.g. the Node smoke tests), it falls back to the default
// stdout/stderr handler. Note that raw stdio (e.g. Janet's (print ...))
// bypasses this bridge; embedders should also hook Module.print/printErr.

#include "interactive.hpp"
#include "io/output.hpp"

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

// Returns 1 if a JS callback consumed the event, 0 otherwise.
// Must live at global scope (EM_JS emits a C-linkage symbol).
// clang-format off
EM_JS(int, zelph_try_js_output, (int channel, const char* text, int newline), {
    const cb = Module["zelphOutput"];
    if (!cb) return 0;
    cb(channel, UTF8ToString(text), newline !== 0);
    return 1;
});
// clang-format on
//
namespace
{
    void output_bridge(const zelph::io::OutputEvent& e)
    {
        if (!zelph_try_js_output(static_cast<int>(e.channel), e.text.c_str(), e.newline ? 1 : 0))
        {
            zelph::io::default_output_handler(e); // Node smoke tests, debugging
        }
    }

    std::unique_ptr<zelph::console::Interactive> g_interactive;

    zelph::console::Interactive& instance()
    {
        // Lazy construction keeps heavy engine setup out of static
        // initialization during wasm instantiation and enables zelph_reset().
        if (!g_interactive)
        {
            // The zelph stdlib is embedded into MEMFS at /stdlib (see the
            // --embed-file link option). Point script resolution there;
            // binary-relative lookup is meaningless inside the wasm module.
            setenv("ZELPH_STDLIB", "/stdlib", 1);

            g_interactive = std::make_unique<zelph::console::Interactive>(&output_bridge);
        }
        return *g_interactive;
    }
}

extern "C"
{
    // Process one REPL input line (UTF-8, no trailing newline). Mirrors the
    // native REPL loop: exceptions are reported on the error channel and the
    // session stays alive.
    EMSCRIPTEN_KEEPALIVE void zelph_process(const char* line)
    {
        if (line == nullptr)
            return;

        try
        {
            instance().process(line);
        }
        catch (const std::exception& e)
        {
            instance().err(e.what());
        }
    }

    // Trigger reasoning explicitly (like .run); normally redundant because
    // auto-run is active by default.
    EMSCRIPTEN_KEEPALIVE void zelph_run()
    {
        try
        {
            instance().run(true, false, false);
        }
        catch (const std::exception& e)
        {
            instance().err(e.what());
        }
    }

    // Non-zero while a multi-line statement or Janet block is being
    // accumulated (UI: continuation prompt).
    EMSCRIPTEN_KEEPALIVE int zelph_is_accumulating()
    {
        return instance().is_accumulating() ? 1 : 0;
    }

    // Current REPL prompt; mirrors make_prompt in src/app/main.cpp
    // (empty while a multi-line statement is being accumulated).
    EMSCRIPTEN_KEEPALIVE const char* zelph_prompt()
    {
        static std::string prompt;

        auto& i = instance();
        if (i.is_accumulating())
            prompt = "";
        else
            prompt = i.get_lang() + (i.is_auto_run_active() ? "> " : "-> ");

        return prompt.c_str();
    }

    // Discard the current engine and start from scratch (Reset button).
    // IMPORTANT: arithmetic and binary-arithmetic must never be imported
    // into the same network - always reset in between.
    EMSCRIPTEN_KEEPALIVE void zelph_reset()
    {
        g_interactive.reset();
    }

    EMSCRIPTEN_KEEPALIVE const char* zelph_version()
    {
        static const std::string version = zelph::console::Interactive::get_version();
        return version.c_str();
    }
}
