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
#include <vector>

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

    // The entries a load reserves before inserting any: the nodes held by
    // the chunks it reads, as the chunk directory of its file records them,
    // `left` and `right` holding the entries of each chunk within the two
    // sections. A null selection reads its section in full; any other holds
    // distinct numbers beneath its section's chunk count, as loads verify
    // before reserving. Both sections of a file a session saved are written
    // following the order of the one node map, so chunk i of the left
    // section and chunk i of the right section contain identical nodes: a
    // chunk number is counted once, using the larger of its two entry
    // counts, regardless of whether the load reads it in one section or in
    // both. A directory is trusted only after it matches its file, which
    // bounds the entries of each chunk via serialization::chunk_entries and
    // through its byte size.
    template <typename Selection>
    std::uint64_t load_reserve(const std::vector<std::uint64_t>& left, const std::vector<std::uint64_t>& right, const Selection* const left_selected, const Selection* const right_selected)
    {
        const auto read = [](const std::vector<std::uint64_t>& entries, const Selection* const selected, const std::size_t chunk) -> std::uint64_t
        { return chunk < entries.size() && (selected == nullptr || selected->count(static_cast<std::uint32_t>(chunk)) != 0) ? entries[chunk] : 0; };

        std::uint64_t nodes = 0;
        for (std::size_t chunk = 0; chunk < (std::max)(left.size(), right.size()); ++chunk)
            nodes += (std::max)(read(left, left_selected, chunk), read(right, right_selected, chunk));
        return nodes;
    }
}
