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

#include "serialization_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

// How the node map (Network::node_map) is sized: the number of buckets, which
// determines the RAM cost of a large network, and the reserve a load makes.
// Excluded from network.hpp, just as serialization_layout.hpp is excluded
// from zelph_impl.hpp, enabling the policy to be tested directly. The numbers
// are 64 bits wide across every target, including wasm32 with its 32-bit
// size_t.
namespace zelph::network::node_map_sizing
{
    // The map's buckets are ankerl's standard ones: 8 bytes with a 32-bit
    // index of the entry, meaning there are at most 2^32 of them. The
    // array doubles when the entries surpass the capacity it holds at the
    // load factor.
    inline constexpr std::uint64_t max_buckets = std::uint64_t{1} << 32;

    // An array of this scale consumes 16 GiB, and its single remaining
    // doubling introduces an additional 16 GiB instantly. At the default
    // factor, a map has it beyond 858,993,472 entries and doubles it at
    // entry 1,717,986,945.
    inline constexpr std::uint64_t large_buckets = std::uint64_t{1} << 31;

    // ankerl's default, and the factor below large_buckets.
    inline constexpr float default_load_factor = 0.8F;

    // The factors a map may use from large_buckets on. Below the default it
    // would only double sooner; above 0.95 a lookup probes ever more buckets
    // of a map that full.
    inline constexpr float min_large_load_factor = default_load_factor;
    inline constexpr float max_large_load_factor = 0.95F;

    // The entries that `buckets` buckets hold at `factor` before the array
    // doubles, according to ankerl's own float arithmetic. An array of
    // max_buckets no longer doubles and uses every bucket.
    constexpr std::uint64_t capacity(const std::uint64_t buckets, const float factor)
    {
        return buckets >= max_buckets ? max_buckets : static_cast<std::uint64_t>(static_cast<float>(buckets) * factor);
    }

    // The factor that the map uses at `buckets` buckets, given the factor
    // from large_buckets on.
    constexpr float factor_at(const std::uint64_t buckets, const float large_factor)
    {
        return buckets >= large_buckets ? large_factor : default_load_factor;
    }

    // The number of buckets in a map holding `entries` entries: the smallest
    // power of two that holds them at the factor it uses there, ranging from
    // ankerl's smallest array of 4 up to max_buckets.
    constexpr std::uint64_t buckets_for(const std::uint64_t entries, const float large_factor)
    {
        std::uint64_t buckets = 4;
        while (buckets < max_buckets && capacity(buckets, factor_at(buckets, large_factor)) < entries)
            buckets *= 2;
        return buckets;
    }

    // The maximum number of entries a reserve may request. ankerl's reserve
    // seeks the smallest array capable of containing them at a single
    // factor; once surpassing what max_buckets can hold at that factor
    // (3,435,973,888 at the default), it computes the array size as 1 << 64,
    // which is undefined. A larger reserve is clamped to this limit, and the
    // map grows from that point onward.
    constexpr std::uint64_t reserve_limit(const float large_factor)
    {
        return static_cast<std::uint64_t>(static_cast<float>(max_buckets) * large_factor);
    }

    // The chunk numbers a load reads in either section, for a load that
    // reads each section in its entirety or solely the chunks specified by
    // a selection. A selection holds distinct numbers beneath its
    // section's chunk count, as loads verify before reserving.
    constexpr std::uint64_t chunks_read(const std::uint64_t left_chunks, const std::uint64_t right_chunks)
    {
        return (std::max)(left_chunks, right_chunks);
    }
    template <typename Selection>
    std::uint64_t chunks_read(const std::uint64_t left_chunks, const std::uint64_t right_chunks, const Selection* const left, const Selection* const right)
    {
        if (left == nullptr && right == nullptr) return chunks_read(left_chunks, right_chunks);
        if (left == nullptr || right == nullptr)
        {
            const std::uint64_t whole    = left == nullptr ? left_chunks : right_chunks;
            const Selection&    selected = left == nullptr ? *right : *left;
            std::uint64_t       chunks   = whole;
            for (const auto index : selected)
                if (index >= whole) ++chunks;
            return chunks;
        }
        std::uint64_t chunks = left->size();
        for (const auto index : *right)
            if (left->count(index) == 0) ++chunks;
        return chunks;
    }

    // The entries a load reserves before inserting any, for a load that reads
    // `chunks` chunk numbers (chunks_read). Both sections of a file a session
    // saved are written according to the order of the one node map, so chunk
    // i of the left section and chunk i of the right section hold the same
    // nodes, up to at most serialization::chunk_entries of them. A load reads
    // no more nodes than its chunks can accommodate and none beyond the node
    // count recorded in the header, and reserves the smaller of the two. That
    // is also what keeps a damaged header from sizing the map by a single
    // number: a node count beyond what the chunks can hold is not reserved,
    // nor are chunks beyond the node count. A header from an older engine
    // records no node count (0); every chunk of a section except its last one
    // is full, so a load reserves that amount for all its chunks except one
    // and lets the map grow for the rest.
    constexpr std::uint64_t load_reserve(const std::uint64_t node_count, const std::uint64_t chunks)
    {
        const std::uint64_t chunk = serialization::chunk_entries;
        if (node_count == 0) return chunks == 0 ? 0 : (chunks - 1) * chunk;
        return (std::min)(node_count, chunks * chunk);
    }
}
