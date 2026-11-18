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

#include "directory_rewrite.hpp"
#include "io/zelph.capnp.h"
#include "network/node_map_sizing.hpp"
#include "network/reasoning.hpp"
#include "test_helpers.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// The expense of the node map, and how a load builds it.
//
// The node map holds one entry for each node within a bucket array that
// expands by doubling when the number of entries exceeds its capacity. Two
// values determine the RAM usage of a large network: at 2^31 buckets, the
// array takes 16 GiB, and its last doubling adds an additional 16 GiB in a
// single step, occurring at entry 1,717,986,945 under ankerl's default load
// factor. The policy is a pure function (node_map_sizing.hpp) and is pinned
// here directly, encompassing the 2^31 and 2^32 regions: no test can build a
// map that large, only the arithmetic that decides it.
//
// Previously, a load inserted into a map of the size the session left, which
// doubled whenever the entries outgrew it and re-placed all entries inserted
// so far at each doubling. Now, it reserves the map once, for the nodes that
// the chunks it reads hold, as the chunk directory of the file records them,
// and the map records both what the load reserved and how many times it
// doubled during insertion (Network::NodeMapState): this counter is how the
// tests below see a growth.
// ---------------------------------------------------------------------------

namespace sizing = zelph::network::node_map_sizing;

namespace
{
    constexpr std::uint64_t operator""_buckets(const unsigned long long log2) { return std::uint64_t{1} << log2; }

    // A network composed of `facts` facts featuring fresh subjects and
    // objects, stored in `file`; returns its node count.
    std::size_t save_network(const std::filesystem::path& file, const std::size_t facts)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        std::string                 script;
        for (std::size_t i = 0; i < facts; ++i)
            script += "s" + std::to_string(i) + " p o" + std::to_string(i) + "\n";
        process_lines(interactive, script);
        interactive.process(".save \"" + file.string() + "\"");
        return node_count(collector, interactive);
    }

    // A network comprising two chunks per section, the second one short,
    // stored within `file`; returns its node count. A network must exceed
    // the capacity of a single chunk to achieve this, hence a program
    // using the library creates a million nodes, mirroring the import
    // process of a large network, in a fraction of the time a script
    // consisting of a million lines would require.
    std::size_t save_two_chunk_network(const std::filesystem::path& file)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        for (std::size_t i = 0; i < zelph::network::serialization::chunk_entries; ++i)
            interactive.graph()->var();
        interactive.process(".save \"" + file.string() + "\"");
        return interactive.graph()->node_map_state().entries;
    }
}

// The number of buckets is the smallest power of two capable of containing
// the entries at 0.8, ankerl's factor: the array of 2^30 buckets holds
// 858,993,472 entries, one additional entry requires 2^31, and that array
// holds 1,717,986,944 before its doubling to the last array of 2^32. The
// float arithmetic is ankerl's own: 0.8F multiplied by 2^31 yields
// 1,717,986,944, not 1,717,986,918.
TEST_CASE("node map: the bucket count is the smallest power of two that holds the entries")
{
    const float d = sizing::default_load_factor;
    CHECK(sizing::buckets_for(0, d) == 4);
    CHECK(sizing::buckets_for(3, d) == 4);
    CHECK(sizing::buckets_for(4, d) == 8);
    CHECK(sizing::buckets_for(25, d) == 32);
    CHECK(sizing::buckets_for(26, d) == 64);

    CHECK(sizing::capacity(30_buckets, d) == 858993472);
    CHECK(sizing::buckets_for(858993472, d) == 30_buckets);
    CHECK(sizing::buckets_for(858993473, d) == 31_buckets);

    CHECK(sizing::capacity(31_buckets, d) == 1717986944);
    CHECK(sizing::buckets_for(1717986944, d) == 31_buckets);
    CHECK(sizing::buckets_for(1717986945, d) == 32_buckets);

    // The last array does not double: it holds a single entry for each
    // bucket.
    CHECK(sizing::capacity(32_buckets, d) == 32_buckets);
    CHECK(sizing::buckets_for(32_buckets, d) == 32_buckets);
}

// The policy is what the map does: a map that grows one entry at a time, as
// every node created during a session makes it grow, has the bucket count
// that the policy names, on both sides of each doubling.
TEST_CASE("node map: a map that grows entry by entry has the bucket count the policy names")
{
    zelph::network::Network net;
    std::size_t             entries = 0;
    for (std::size_t buckets = 4; buckets <= 20_buckets; buckets *= 2)
    {
        const std::size_t full = sizing::capacity(buckets, sizing::default_load_factor);
        for (; entries < full; ++entries)
            net.create();
        CAPTURE(entries);
        CHECK(net.node_map_state().buckets == sizing::buckets_for(entries, sizing::default_load_factor));
        CHECK(net.node_map_state().buckets == buckets);

        net.create();
        ++entries;
        CHECK(net.node_map_state().buckets == sizing::buckets_for(entries, sizing::default_load_factor));
        CHECK(net.node_map_state().buckets == buckets * 2);
    }
}

// A higher factor defers the last doubling, the operation that adds 16 GiB
// in one step, and costs lookup speed solely in the range starting from
// 2^31 buckets. Below, the map keeps 0.8, ensuring that no network with
// fewer than 858,993,473 nodes changes. At 0.95, an array of 2^30 buckets
// would hold 1,020,054,720 entries; it still holds 858,993,472, and the
// subsequent entry requires 2^31.
TEST_CASE("node map: a factor above the default applies from 2^31 buckets on")
{
    for (const float large : {0.9F, 0.95F})
    {
        CAPTURE(large);
        for (const std::size_t entries : {std::size_t{0}, std::size_t{26}, std::size_t{1000000}, std::size_t{858993472}, std::size_t{858993473}})
            CHECK(sizing::buckets_for(entries, large) == sizing::buckets_for(entries, sizing::default_load_factor));
        CHECK(sizing::buckets_for(1717986945, large) == 31_buckets);
    }

    CHECK(sizing::capacity(31_buckets, 0.9F) == 1932735232);
    CHECK(sizing::buckets_for(1932735232, 0.9F) == 31_buckets);
    CHECK(sizing::buckets_for(1932735233, 0.9F) == 32_buckets);
    CHECK(sizing::capacity(31_buckets, 0.95F) == 2040109440);
    CHECK(sizing::buckets_for(2040109440, 0.95F) == 31_buckets);
    CHECK(sizing::buckets_for(2040109441, 0.95F) == 32_buckets);

    // A map with fewer than 2^31 buckets keeps the default while the
    // factor is set, and grows precisely as it would in its absence.
    zelph::network::Network net;
    net.set_large_load_factor(0.95F);
    for (std::size_t i = 0; i < 1000; ++i)
        net.create();
    CHECK(net.large_load_factor() == 0.95F);
    CHECK(net.node_map_state().load_factor == sizing::default_load_factor);
    CHECK(net.node_map_state().buckets == sizing::buckets_for(1000, sizing::default_load_factor));
}

// ankerl's reserve identifies the smallest array capable of containing the
// entries at one factor, and beyond the capacity of 2^32 buckets at that
// factor it computes the array size as 1 << 64, which is undefined: a
// reserve for 3,435,973,889 entries at the default is not a large map but a
// broken one. A load clamps its reserve to the limit, and the map grows
// from there.
TEST_CASE("node map: a reserve is clamped to what 2^32 buckets hold at its factor")
{
    CHECK(sizing::reserve_limit(sizing::default_load_factor) == 3435973888);
    CHECK(sizing::reserve_limit(0.95F) == 4080218880);
    for (const float factor : {sizing::default_load_factor, 0.9F, 0.95F})
    {
        CAPTURE(factor);
        CHECK(sizing::buckets_for(sizing::reserve_limit(factor), factor) == sizing::max_buckets);
    }
}

// The entries a load reserves, drawn from those the chunk directory records
// for each chunk. Chunk i in the left section and chunk i in the right
// section contain identical nodes, so loading chunk 0 on one side and chunk
// 1 on the other retrieves nodes from two distinct chunks, whereas loading
// chunk 0 on both sides accesses the nodes from a single chunk. Counting
// only the larger selection, one chunk in each scenario, underestimates the
// first case: chunk 0 on the left and chunk 1 on the right of the pruned
// P279 network read 2,000,000 nodes into a reserve of 1,000,000, with the
// map doubling during the insertion process.
TEST_CASE("node map: a load reserves the entries of the chunk numbers it reads")
{
    using selection                        = std::unordered_set<std::uint32_t>;
    const selection* const           whole = nullptr;
    const selection                  none, first{0}, second{1}, both{0, 1}, third{2};
    const std::vector<std::uint64_t> three{1000000, 1000000, 615};

    CHECK(sizing::load_reserve(three, three, whole, whole) == 2000615);

    CHECK(sizing::load_reserve(three, three, &first, &first) == 1000000);
    CHECK(sizing::load_reserve(three, three, &first, &second) == 2000000);
    CHECK(sizing::load_reserve(three, three, &both, &second) == 2000000);
    CHECK(sizing::load_reserve(three, three, &none, &second) == 1000000);
    CHECK(sizing::load_reserve(three, three, &none, &none) == 0);

    // A section read in its entirety and a selection of the other, which
    // adds the chunks missing from the whole section.
    CHECK(sizing::load_reserve(three, three, whole, &second) == 2000615);
    CHECK(sizing::load_reserve(three, three, &third, whole) == 2000615);

    // Sections of different lengths, as seen in a view generated by a
    // partial load and subsequently saved by the library.
    const std::vector<std::uint64_t> two{1000000, 1000000};
    CHECK(sizing::load_reserve(two, three, whole, whole) == 2000615);
    CHECK(sizing::load_reserve(two, three, whole, &third) == 2000615);
    CHECK(sizing::load_reserve({}, three, whole, &second) == 1000000);
}

// The final chunk in a section is short, and a load that reads it reserves
// the entries it holds rather than an entire chunk. Counted in chunk numbers,
// chunk 2 of the pruned P279 network, which holds 5,552 nodes, took 1,000,000
// entries, approximately 56 MB in memory at any one time.
TEST_CASE("node map: a load of the short last chunk reserves the entries it holds")
{
    using selection = std::unordered_set<std::uint32_t>;
    const selection                  none, last{2};
    const std::vector<std::uint64_t> p279{1000000, 1000000, 5552};

    CHECK(sizing::load_reserve(p279, p279, &last, &last) == 5552);
    CHECK(sizing::load_reserve(p279, p279, &last, &none) == 5552);
    CHECK(sizing::load_reserve(p279, p279, &none, &last) == 5552);
}

// The point of the reserve: a load places each node into a map that already
// has room for all of them. 200 facts generate 617 nodes, a count that a
// fresh engine's map -- initially with 32 buckets for its 15 nodes -- would
// only attain through five rounds of doubling.
TEST_CASE("load: a load reserves the node map once and does not grow it while it inserts")
{
    const auto        file  = std::filesystem::temp_directory_path() / "zelph_node_map_reserve.bin";
    const std::size_t nodes = save_network(file, 200);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    REQUIRE(nodes > sizing::capacity(interactive.graph()->node_map_state().buckets, sizing::default_load_factor));
    interactive.process(".load \"" + file.string() + "\"");
    REQUIRE(node_count(collector, interactive) == nodes);

    const auto state = interactive.graph()->node_map_state();
    CHECK(state.entries == nodes);
    CHECK(state.reserved == nodes);
    CHECK(state.growths == 0);
    CHECK(state.buckets == sizing::buckets_for(nodes, sizing::default_load_factor));

    std::filesystem::remove(file);
}

// Saving selected predicates writes the nodes it keeps, and its chunk
// directory records the entries of the chunks it writes, not the overall
// count of nodes in the network they were drawn from, so a load of the file
// reserves for what the file holds. One predicate out of two is saved here,
// each linked to 200 facts spread across nodes of its own.
TEST_CASE("load: a file of selected predicates records the nodes it holds")
{
    const auto  file         = std::filesystem::temp_directory_path() / "zelph_node_map_selected.bin";
    std::size_t source_nodes = 0;
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        std::string                 script;
        for (std::size_t i = 0; i < 200; ++i)
        {
            script += "s" + std::to_string(i) + " p o" + std::to_string(i) + "\n";
            script += "t" + std::to_string(i) + " q u" + std::to_string(i) + "\n";
        }
        process_lines(interactive, script);
        interactive.process(".save-predicates \"" + file.string() + "\" p");
        source_nodes = node_count(collector, interactive);
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");

    const auto state = interactive.graph()->node_map_state();
    REQUIRE(state.entries > 600);
    REQUIRE(state.entries < source_nodes);
    CHECK(state.reserved == state.entries);
    CHECK(state.growths == 0);

    std::filesystem::remove(file);
}

// A partial load reserves for the chunks it reads. Each load goes into a
// fresh engine, just as the whole load above does: clearing a map keeps its
// bucket array, so a map that a load of the file filled before has room for
// the next load with or without a reserve, and its growth counter would
// remain at 0 either way.
TEST_CASE("load: a partial load reserves for the chunks it reads")
{
    const auto        file  = std::filesystem::temp_directory_path() / "zelph_node_map_partial.bin";
    const std::size_t nodes = save_network(file, 200);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    for (const std::string sides : {"left=0 right=0", "left=0 right=none", "left=none right=0"})
    {
        CAPTURE(sides);
        interactive.process(".new");
        REQUIRE(nodes > sizing::capacity(interactive.graph()->node_map_state().buckets, sizing::default_load_factor));
        interactive.process(".load-partial \"" + file.string() + "\" " + sides);

        const auto state = interactive.graph()->node_map_state();
        CHECK(state.entries == nodes);
        CHECK(state.reserved == nodes);
        CHECK(state.growths == 0);
    }

    interactive.process(".load-partial \"" + file.string() + "\" left=none right=none");
    CHECK(interactive.graph()->node_map_state().reserved == 0);
    CHECK(interactive.graph()->node_map_state().entries == 0);

    std::filesystem::remove(file);
}

// The same where a user meets it: a network comprising more than one chunk,
// with the final chunk being short. A partial load of that chunk reserves the
// nodes it holds, on either side and on both, and a load that reads chunk 0 on
// one side and the last chunk on the other reserves the nodes of both.
TEST_CASE("load: a partial load of the short last chunk reserves the nodes it holds")
{
    constexpr std::size_t chunk = zelph::network::serialization::chunk_entries;
    const auto            file  = std::filesystem::temp_directory_path() / "zelph_node_map_last_chunk.bin";
    const std::size_t     nodes = save_two_chunk_network(file);
    REQUIRE(nodes > chunk);
    const std::size_t last = nodes - chunk;

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    for (const auto& [sides, read] : {std::pair<std::string, std::size_t>{"left=1 right=1", last},
                                      {"left=1 right=none", last},
                                      {"left=none right=1", last},
                                      {"left=0 right=1", nodes},
                                      {"left=0 right=0", chunk}})
    {
        CAPTURE(sides);
        interactive.process(".new");
        interactive.process(".load-partial \"" + file.string() + "\" " + sides);

        const auto state = interactive.graph()->node_map_state();
        CHECK(state.entries == read);
        CHECK(state.reserved == read);
        CHECK(state.growths == 0);
    }

    interactive.process(".new");
    interactive.process(".load \"" + file.string() + "\"");
    CHECK(interactive.graph()->node_map_state().entries == nodes);
    CHECK(interactive.graph()->node_map_state().reserved == nodes);

    std::filesystem::remove(file);
}

// A chunk directory could be damaged, or written by something other than the
// engine, and a load that reserved the values it records would rely on
// numbers that no other component bounds: a header indicating 40,000,000
// nodes for a network of 25,364 took 2.1 GB before the load read a chunk. A
// directory is trusted only when it matches its file, and one whose chunk in
// the left or right section claims more entries than a chunk can hold does
// not. The load refuses the file and states the reason, before reserving and
// before discarding the session's network.
TEST_CASE("load: a chunk directory that claims more entries than a chunk holds is refused")
{
    constexpr std::size_t chunk = zelph::network::serialization::chunk_entries;
    const auto            file  = std::filesystem::temp_directory_path() / "zelph_node_map_overstated.bin";
    save_network(file, 200);
    rewrite_directory(file, [](zelph::network::ChunkDirectory::Builder directory)
                      { directory.getSections()[1].getChunks()[0].setEntries(chunk + 1); });

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");
    for (const std::string load : {".load", ".load-partial"})
    {
        CAPTURE(load);
        CHECK_THROWS_WITH_AS(interactive.process(load + " \"" + file.string() + "\""),
                             doctest::Contains("right chunk 0 holds 1000001 entries, more than a chunk holds (1000000)"),
                             std::runtime_error);
    }
    CHECK(interactive.graph()->node_map_state().reserved == 0);
    collector.clear();
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    std::filesystem::remove(file);
}

// The other bound of a chunk: its bytes, which also bound the amount a load
// reserves for a file of a given size. An entry the engine writes in the
// left or right section takes at least four packed bytes, two for each of
// its words, the node and the pointer to its adjacency, neither of which is
// zero. A chunk therefore holds at most a quarter of its bytes in entries. A
// directory asserting one additional entry for a chunk of a few kilobytes,
// significantly below the chunk's capacity, is refused in the same way.
TEST_CASE("load: a chunk directory that claims more entries than a chunk's bytes can hold is refused")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_node_map_dense.bin";
    save_network(file, 200);
    std::uint64_t claimed = 0;
    rewrite_directory(file, [&claimed](zelph::network::ChunkDirectory::Builder directory)
                      {
                          auto left = directory.getSections()[0].getChunks()[0];
                          claimed   = left.getLength() / 4 + 1;
                          left.setEntries(claimed); });
    REQUIRE(claimed < zelph::network::serialization::chunk_entries);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process("a p b");
    for (const std::string load : {".load", ".load-partial"})
    {
        CAPTURE(load);
        CHECK_THROWS_WITH_AS(interactive.process(load + " \"" + file.string() + "\""),
                             doctest::Contains("left chunk 0 holds " + std::to_string(claimed) + " entries, more than its"),
                             std::runtime_error);
    }
    collector.clear();
    interactive.process("A p B");
    CHECK(answers_contain(collector, "a p b"));

    std::filesystem::remove(file);
}

namespace zelph::test
{
    // Binary files from older_networks/*.bin are integrated directly into
    // this compiled executable via embed_files.cmake.
    const std::map<std::string, std::vector<unsigned char>>& older_networks();
}

// A file from an older engine lacks a chunk directory, and no other part of
// it reliably indicates how many nodes its chunks hold: the load reserves
// nothing, regardless of its chunk counts, and the map grows as it did
// before the introduction of the reserve. Loading such a file is how a user
// reaches that path, and it shows the counter in action: the map that the
// fresh engine left doubles as the load inserts.
TEST_CASE("load: a file without a chunk directory lets the node map grow while it loads")
{
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("data_collections");
    REQUIRE(network != networks.end());

    const auto path = std::filesystem::temp_directory_path() / "zelph_node_map_older.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    const std::size_t           buckets_before = interactive.graph()->node_map_state().buckets;
    interactive.process(".load " + path.string());
    std::filesystem::remove(path);

    const auto state = interactive.graph()->node_map_state();
    REQUIRE(state.entries > sizing::capacity(buckets_before, sizing::default_load_factor));
    CHECK(state.reserved == 0);
    CHECK(state.growths > 0);
    CHECK(state.buckets == sizing::buckets_for(state.entries, sizing::default_load_factor));
}

// However many chunks a file lacking a chunk directory declares, the load
// reserves nothing: the counts in its header indicate the number of chunks,
// not the number of nodes they hold, and an estimate derived from them
// reserved a million entries for each complete chunk of a file whose header
// undergoes no verification. Here, a network with two chunks per section is
// written in the format of every network published before the directory, as
// an older engine wrote it, and loaded in full or in part.
TEST_CASE("load: a file without a chunk directory reserves nothing, however many chunks it has")
{
    const auto        saved = std::filesystem::temp_directory_path() / "zelph_node_map_two_chunks.bin";
    const auto        older = std::filesystem::temp_directory_path() / "zelph_node_map_two_chunks_older.bin";
    const std::size_t nodes = save_two_chunk_network(saved);
    REQUIRE(nodes > zelph::network::serialization::chunk_entries);
    write_without_directory(saved, older);
    std::filesystem::remove(saved);

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".stat-file \"" + older.string() + "\"");
    REQUIRE(any_output_contains(collector, "Left Chunks: 2"));
    REQUIRE(any_output_contains(collector, "Chunk Directory: none"));

    interactive.process(".load \"" + older.string() + "\"");
    CHECK(interactive.graph()->node_map_state().entries == nodes);
    CHECK(interactive.graph()->node_map_state().reserved == 0);
    CHECK(interactive.graph()->node_map_state().growths > 0);

    interactive.process(".new");
    interactive.process(".load-partial \"" + older.string() + "\" left=0,1 right=0,1");
    CHECK(interactive.graph()->node_map_state().entries == nodes);
    CHECK(interactive.graph()->node_map_state().reserved == 0);

    std::filesystem::remove(older);
}

// The setting consists of a single line, similar to the other engine
// settings, and says whether the factor is set to the default.
TEST_CASE("node-map-load-factor: without an argument it shows the factor, and whether it is the default")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".node-map-load-factor");
    CHECK(any_output_contains(collector, "Node map load factor from 2^31 buckets: 0.8 (default)"));
    CHECK(interactive.graph()->node_map_load_factor() == sizing::default_load_factor);

    collector.clear();
    interactive.process(".node-map-load-factor 0.95");
    CHECK(any_output_contains(collector, "Node map load factor from 2^31 buckets: 0.95"));
    CHECK_FALSE(any_output_contains(collector, "(default)"));
    CHECK(interactive.graph()->node_map_load_factor() == 0.95F);

    collector.clear();
    interactive.process(".node-map-load-factor default");
    CHECK(any_output_contains(collector, "Node map load factor from 2^31 buckets: 0.8 (default)"));
    CHECK(interactive.graph()->node_map_load_factor() == sizing::default_load_factor);
}

// A factor beneath the default only causes a large map to double sooner,
// while one above 0.95 results in a lookup within a map so densely
// populated that probing ever more buckets becomes necessary; a number that
// fails to read as a whole is not a factor. Each is refused, and a refusal
// preserves the factor in its original state.
TEST_CASE("node-map-load-factor: a factor out of range or malformed is refused")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".node-map-load-factor 0.9");
    for (const char* argument : {"0.5", "0.79", "0.96", "1", "-0.9", "abc", "0.9x", ",9", "0.9 0.9"})
    {
        CAPTURE(argument);
        CHECK_THROWS_WITH_AS(interactive.process(std::string(".node-map-load-factor ") + argument),
                             doctest::Contains("Usage: .node-map-load-factor [<factor>|default]"),
                             std::runtime_error);
    }
    CHECK(interactive.graph()->node_map_load_factor() == 0.9F);

    // The library's setter refuses the same factors, for a caller that does
    // not proceed via the command.
    for (const float factor : {0.5F, 0.96F, 1.0F, std::numeric_limits<float>::quiet_NaN()})
    {
        CAPTURE(factor);
        CHECK_THROWS_AS(interactive.graph()->set_node_map_load_factor(factor), std::invalid_argument);
    }
    CHECK(interactive.graph()->node_map_load_factor() == 0.9F);
}

// The factor is a setting of the engine, much like .explain-budget, and
// .new creates the engine anew.
TEST_CASE("node-map-load-factor: .new restores the default")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".node-map-load-factor 0.9");
    interactive.process(".new");
    CHECK(interactive.graph()->node_map_load_factor() == sizing::default_load_factor);
}

// .stat names the factor while it differs from the default, and only then:
// the statistics for every other session remain exactly as the
// documentation shows them.
TEST_CASE("node-map-load-factor: .stat shows the factor while it is not the default")
{
    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".stat");
    CHECK_FALSE(any_output_contains(collector, "Node map load factor"));

    interactive.process(".node-map-load-factor 0.9");
    collector.clear();
    interactive.process(".stat");
    CHECK(any_output_contains(collector, "Node map load factor from 2^31 buckets: 0.9"));
}
