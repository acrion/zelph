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

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>

namespace zelph::network
{
#ifdef __EMSCRIPTEN__
    // wasm32: size_t is 32 bits. Node deliberately keeps the same width as
    // size_t (enforced below); the node/hash scheme runs as a faithful
    // 32-bit analog of the 64-bit design (see network.hpp).
    using Node = uint32_t;
#else
    using Node = uint64_t;
#endif
    using Variables = std::map<Node, Node>;

    static_assert(sizeof(Node) == 8 || sizeof(Node) == 4,
                  "Node must be exactly 64 bits (or 32 bits in the wasm32 build). This "
                  "implementation uses bit-shift operations and other bit-specific logic "
                  "tied to the exact width.");

    static_assert(sizeof(Node) == sizeof(std::size_t),
                  "Node and size_t must have the same size. "
                  "Compilation has been halted to prevent undefined behavior.");

    inline std::shared_ptr<Variables> join(const Variables& v1, const Variables& v2)
    {
        std::shared_ptr<Variables> result = std::make_shared<Variables>(v1);

        for (auto& var : v2)
        {
            auto it = result->find(var.first);

            if (it != result->end())
            {
                if (it->second != var.second)
                {
                    throw std::runtime_error("Variable sets to be merged do conflict");
                }
            }
            else
            {
                (*result)[var.first] = var.second;
            }
        }

        return result;
    }
}
