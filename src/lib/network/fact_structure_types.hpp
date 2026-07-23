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

#include "adjacency_set.hpp"
#include "network_types.hpp"

#include <memory>
#include <vector>

namespace zelph::network
{
    struct FactStructure
    {
        Node          subject{};
        Node          predicate{};
        adjacency_set objects;
    };

    // Cached fact-structure lists are IMMUTABLE and shared: a cache hit
    // copies one shared_ptr (a single atomic increment) instead of
    // deep-copying the structures. A holder's pointer stays valid across
    // invalidations -- it then references a consistent snapshot, exactly
    // the staleness window the former private copies had.
    using FactStructureList = std::vector<FactStructure>;
    using FactStructurePtr  = std::shared_ptr<const FactStructureList>;
}
