/*
Copyright (c) 2025 acrion innovations GmbH
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

#include "platform_utils.hpp"

#ifdef __linux__
    #include <fstream>
    #include <sstream>
    #include <string>
#endif

namespace zelph
{
    namespace platform
    {
        size_t get_process_memory_usage()
        {
#ifdef __linux__
            std::ifstream status("/proc/self/status");
            if (!status.is_open())
            {
                return 0; // Fallback if file not accessible
            }
            std::string line;
            while (std::getline(status, line))
            {
                if (line.compare(0, 6, "VmRSS:") == 0)
                {
                    std::istringstream iss(line.substr(6));
                    size_t             rss_kb;
                    iss >> rss_kb;
                    return rss_kb * 1024; // Convert kB to Bytes
                }
            }
#endif
            return 0; // Non-Linux
        }
    }
}
