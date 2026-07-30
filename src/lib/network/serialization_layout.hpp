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

#include <algorithm>
#include <cstddef>

// How a saved network is laid out in the file, in the two numbers that decide
// what the write costs in RAM. Kept out of zelph_impl.hpp, which only
// zelph.cpp may include, so the policy can be tested directly.
namespace zelph::network::serialization
{
    // Entries per chunk. Each chunk is one Cap'n Proto message, so this is
    // also the granularity at which a partial load can pick pieces out of a
    // file (see manifest_loader.hpp).
    inline constexpr std::size_t chunk_entries = 1000000;

    // First segment of a message, in WORDS.
    //
    // This is a floor on the memory a save needs, not a hint: Cap'n Proto
    // allocates the first segment up front, and under the allocator the zelph
    // binary ships with (mimalloc, linked in src/app) the pages are resident
    // immediately. A flat 1u << 26 -- 512 MiB, whatever the message held --
    // therefore showed up as half a gigabyte of RSS for saving an 11 kB
    // network, and was paid again on top of a Wikidata-sized graph, where
    // memory rather than time is what the operation runs out of.
    //
    // Eight words per entry covers a node plus a list header plus a typical
    // adjacency, or a key plus a short label. Where the estimate falls short
    // Cap'n Proto APPENDS a segment instead of copying, each new one as large
    // as all previous together, so a miss costs at most a factor of two in
    // memory and never a memcpy -- it does grow the FILE slightly, because
    // every additional segment brings a segment table entry and turns
    // cross-segment references into far pointers.
    inline constexpr std::size_t first_segment_words(const std::size_t entries)
    {
        constexpr std::size_t words_per_entry = 8;
        constexpr std::size_t floor_words     = 1024;
        return std::max(floor_words, entries * words_per_entry);
    }
}
