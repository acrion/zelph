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

#include "versions.hpp"

#include "interactive.hpp"
#include "script_engine.hpp"

#include <ankerl/unordered_dense.h>
#include <bzlib.h>
#include <capnp/common.h>

#if __has_include(<mimalloc.h>)
    #include <mimalloc.h>
#endif

#include <sstream>

namespace zelph
{
    std::string get_version_description()
    {
        std::ostringstream oss;

        // 1. zelph Version
        oss << "zelph " << console::Interactive::get_version() << "\n\n";

        // 2. Third-party software
        oss << "zelph incorporates the following third-party software:\n";
        oss << "------------------------------------------------------\n";

        // Janet
        oss << "Janet (v" << ScriptEngine::get_janet_version() << ") - MIT License\n";

        // unordered_dense
        oss << "unordered_dense (v"
            << ANKERL_UNORDERED_DENSE_VERSION_MAJOR << "."
            << ANKERL_UNORDERED_DENSE_VERSION_MINOR << "."
            << ANKERL_UNORDERED_DENSE_VERSION_PATCH << ") - MIT License\n";

        // Cap'n Proto
        oss << "Cap'n Proto (v"
            << CAPNP_VERSION_MAJOR << "."
            << CAPNP_VERSION_MINOR << "."
            << CAPNP_VERSION_MICRO << ") - MIT License\n";

        // bzip2
        std::string bz2_full      = BZ2_bzlibVersion();
        size_t      bz2_comma_pos = bz2_full.find(',');
        std::string bz2_version   = (bz2_comma_pos != std::string::npos) ? bz2_full.substr(0, bz2_comma_pos) : bz2_full;
        oss << "bzip2 (v" << bz2_version << ") - bzip2 License (BSD-style)\n";

        // mimalloc
#if defined(MI_MALLOC_VERSION)
        int mi_major = MI_MALLOC_VERSION / 1000;
        int mi_minor = (MI_MALLOC_VERSION / 100) % 10;
        int mi_patch = MI_MALLOC_VERSION % 100;
        oss << "mimalloc (v" << mi_major << "." << mi_minor << "." << mi_patch << ") - MIT License\n";
#endif

        oss << "------------------------------------------------------\n";
        oss << "For full license texts and copyright notices, please refer to the\n";
        oss << "documentation or the source code repositories.";

        return oss.str();
    }
}