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

#pragma once

#include <cstdint>
#include <map>

namespace zelph
{
    namespace network
    {
        using Node      = uint64_t;
        using Variables = std::map<Node, Node>;

        static_assert(sizeof(Node) == 8,
                      "Node must be exactly 64 bits (8 bytes) in size. This implementation uses "
                      "bit-shift operations and other bit-specific logic that requires exactly "
                      "64 bits. Please modify the implementation for different bit sizes.");

        static_assert(sizeof(Node) == sizeof(std::size_t),
                      "Node and size_t must have the same size. "
                      "This implementation requires a 64-bit architecture where size_t is also 64 bits. "
                      "Compilation has been halted to prevent undefined behavior.");

    }
}
