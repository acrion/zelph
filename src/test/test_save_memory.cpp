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

#include <doctest/doctest.h>

#include <filesystem>

#include "network/serialization_layout.hpp"
#include "test_helpers.hpp"

using namespace zelph::test;

// ---------------------------------------------------------------------------
// What a .save costs in RAM.
//
// Cap'n Proto allocates the first segment of a message up front, so the size
// handed to MallocMessageBuilder is a floor on the memory a save needs, not a
// hint. It used to be a flat 1u << 26 WORDS -- 512 MiB per message, whatever
// the message held; measured against the shipped binary, saving an 11 kB
// network moved process RSS from 0.0 to 0.5 GiB and it stayed there.
//
// The size is asserted here rather than the memory: RSS is not a property a
// test can pin honestly. It depends on the allocator (the zelph binary links
// mimalloc, this test binary does not, and glibc hands out untouched pages
// for a large calloc), and on whatever the 300-odd test cases before this one
// left behind. What CAN be pinned is the policy that produces it -- that the
// first segment follows the data instead of being a constant, which is the
// thing a future edit might undo.
// ---------------------------------------------------------------------------

namespace layout = zelph::network::serialization;

TEST_CASE("save: the first message segment is sized from the data")
{
    constexpr std::size_t words_per_mib = (1024 * 1024) / 8;

    // Small message, small segment. The bound is two orders of magnitude
    // below the 512 MiB (64 Mi words) this replaced.
    CHECK(layout::first_segment_words(0) < words_per_mib);
    CHECK(layout::first_segment_words(10) < words_per_mib);

    // ...but never zero: Cap'n Proto would then allocate per object.
    CHECK(layout::first_segment_words(0) > 0);

    // It follows the entry count.
    CHECK(layout::first_segment_words(1000000) > layout::first_segment_words(1000));

    // A full chunk starts in the tens of MiB -- the order of magnitude a real
    // Wikidata chunk measures (~27 MB packed for the 2017 pruned network),
    // so the common case still fits in the first segment and the file stays
    // single-segment. Well clear of both a per-object trickle and the flat
    // half gigabyte.
    const std::size_t full_chunk = layout::first_segment_words(layout::chunk_entries);
    CHECK(full_chunk > 8 * words_per_mib);
    CHECK(full_chunk < 128 * words_per_mib);
}

TEST_CASE("save: a network survives the round trip through a sized message")
{
    namespace fs = std::filesystem;

    // Whatever segmentation the sizing produces, the file has to read back.
    // Cap'n Proto appends segments when the first one runs out, and both the
    // writer and every reader of a .bin have to agree about that.
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());

    process_lines(interactive, R"(
alice knows bob
bob knows charlie
(A knows B) => (B "is known by" A)
)");
    interactive.process(".name alice de Alice");

    const auto path = fs::temp_directory_path() / "zelph_save_memory.bin";
    interactive.process(".save \"" + path.string() + "\"");
    interactive.process(".new");
    interactive.process(".load \"" + path.string() + "\"");

    collector.clear();
    interactive.process("A knows B");
    CHECK(answers_contain(collector, "alice knows bob"));
    CHECK(answers_contain(collector, "bob knows charlie"));

    collector.clear();
    interactive.process("A \"is known by\" B");
    CHECK(answers_contain(collector, "bob \"is known by\" alice"));

    // The name chunks are messages of their own, one per language.
    collector.clear();
    interactive.process(".node alice");
    CHECK(any_output_contains(collector, "Alice"));

    fs::remove(path);
}
