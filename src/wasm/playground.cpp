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

#include "interactive.hpp"

#include <emscripten/emscripten.h>

#include <exception>
#include <memory>
#include <string>

namespace
{
    std::unique_ptr<zelph::console::Interactive> g_interactive;

    zelph::console::Interactive& instance()
    {
        // Lazy construction keeps heavy engine setup out of static
        // initialization during wasm instantiation and enables zelph_reset().
        if (!g_interactive)
        {
            // The zelph stdlib is packaged into MEMFS at /stdlib (see the
            // --preload-file link option). Point script resolution there;
            // binary-relative lookup is meaningless inside the wasm module.
            setenv("ZELPH_STDLIB", "/stdlib", 1);

            g_interactive = std::make_unique<zelph::console::Interactive>();
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
