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

#include <memory>
#include <string>

namespace zelph::console
{
    enum class ScriptMode
    {
        Zelph,
        Janet
    };

    struct ReplState
    {
        bool        auto_run    = true;
        ScriptMode  script_mode = ScriptMode::Zelph;
        std::string janet_buffer;                      // Accumulates incomplete Janet expressions
        bool        accumulating_inline_janet = false; // True while a % expression spans multiple lines
    };

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
