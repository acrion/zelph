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
#include "script/script_engine.hpp"

#include <ankerl/unordered_dense.h>

#ifndef __EMSCRIPTEN__
    #include <bzlib.h>
    #include <capnp/common.h>
#endif

#include <atomic>
#include <sstream>

namespace
{
    // mimalloc is linked into the app and into nothing beyond -- neither
    // into this library, nor into the test executable, nor into a program
    // that interfaces with zelph via its C ABI. Thus, determining which one
    // runs is solely the application's responsibility, resolved during
    // startup. A header cannot: this line previously was compiled from
    // whichever <mimalloc.h> the compiler found, which was the SYSTEM's, and
    // it referred to an allocator that the binary lacks.
    std::atomic<int> linked_mimalloc{0};
}

namespace zelph
{
    void set_linked_mimalloc(const int version)
    {
        linked_mimalloc = version;
    }

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

#ifndef __EMSCRIPTEN__
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
#endif

        // mimalloc, encoded as major * 10000 + minor * 100 + patch
        if (const int mi = linked_mimalloc; mi != 0)
            oss << "mimalloc (v" << mi / 10000 << "." << mi / 100 % 100 << "." << mi % 100 << ") - MIT License\n";

        oss << "------------------------------------------------------\n";
        oss << "For full license texts and copyright notices, please refer to the\n";
        oss << "documentation or the source code repositories.";

        return oss.str();
    }
}