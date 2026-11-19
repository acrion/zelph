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

#include "curl_stand_in.hpp"
#include "directory_rewrite.hpp"
#include "network/reasoning.hpp"
#include "network/serialization_layout.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
    #include <csignal>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Sharded artifacts and partial loading.
//
// The published artifacts live on Hugging Face and are gigabytes large, so
// nothing here may depend on them: every test builds its own artifact from a
// four-node network -- .save, .index-file, one shard file per chunk, a
// manifest that advertises hf:// object paths exactly as the emitter does.
// That is small enough to be hermetic and faithful enough to pin the rules
// that matter:
//
//  * A manifest whose shards lie next to it resolves them locally. The
//    advertised paths are remote, so a regression turns this into a download
//    attempt -- which fails in CI, loudly, instead of silently costing
//    hundreds of megabytes (the state before: 232 MB fetched for one chunk
//    that was already on disk).
//  * The same holds for the .bin the manifest names: a local copy is used
//    for the header instead of a ranged request, so a downloaded artifact
//    works offline without source-bin=.
//  * chunkIndex is section-global and continues across languages, so
//    nameOfNode=<n> selects exactly one language's chunk on both loaders.
// ---------------------------------------------------------------------------

namespace
{
    namespace fs = std::filesystem;

    struct ChunkInfo
    {
        uint64_t    index  = 0;
        uint64_t    offset = 0;
        uint64_t    length = 0;
        std::string lang; // name sections only
    };

    struct ArtifactPaths
    {
        fs::path root;
        fs::path bin;
        fs::path sharded_manifest;
        fs::path seek_manifest;
        fs::path shards_dir;
        fs::path route_index;

        // The range of the .bin that the index designates as its header: the
        // header message and the chunk directory located after it.
        uint64_t header_length = 0;

        // Node ID of "alpha", read from the network rather than assumed: the
        // route index addresses nodes by ID, which is what makes it a route
        // index rather than a chunk selector.
        uint64_t alpha = 0;

        std::map<std::string, std::vector<ChunkInfo>> sections;

        // chunkIndex of the name chunks of one language, looked up rather
        // than assumed: which language is written first depends on map order.
        uint64_t name_of_node_chunk(const std::string& lang) const
        {
            for (const auto& chunk : sections.at("nameOfNode"))
            {
                if (chunk.lang == lang) return chunk.index;
            }
            return static_cast<uint64_t>(-1);
        }

        uint64_t node_of_name_chunk(const std::string& lang) const
        {
            for (const auto& chunk : sections.at("nodeOfName"))
            {
                if (chunk.lang == lang) return chunk.index;
            }
            return static_cast<uint64_t>(-1);
        }
    };

    std::string read_file(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // Pull "<key>":<number> out of one line of the .index-file output. The
    // format is written by zelph itself and has one chunk object per line,
    // so a full JSON parser would be ceremony without benefit here.
    uint64_t number_field(const std::string& line, const std::string& key)
    {
        const std::string marker = "\"" + key + "\":";
        const size_t      pos    = line.find(marker);
        REQUIRE(pos != std::string::npos);
        return std::stoull(line.substr(pos + marker.size()));
    }

    std::string string_field(const std::string& line, const std::string& key)
    {
        const std::string marker = "\"" + key + "\":\"";
        const size_t      pos    = line.find(marker);
        if (pos == std::string::npos) return {};
        const size_t start = pos + marker.size();
        return line.substr(start, line.find('"', start) - start);
    }

    // Copy [offset, offset+length) of the .bin into its own file.
    void write_shard(const fs::path& bin, const fs::path& target, uint64_t offset, uint64_t length)
    {
        fs::create_directories(target.parent_path());

        std::ifstream in(bin, std::ios::binary);
        REQUIRE(in.is_open());
        in.seekg(static_cast<std::streamoff>(offset));

        std::vector<char> buffer(static_cast<size_t>(length));
        in.read(buffer.data(), static_cast<std::streamsize>(length));
        REQUIRE(in.gcount() == static_cast<std::streamsize>(length));

        std::ofstream out(target, std::ios::binary);
        out.write(buffer.data(), static_cast<std::streamsize>(length));
    }

    // ".node <name>" prints "Node ID: <n>"; nothing else exposes the ID.
    uint64_t node_id_of(const zelph::console::Interactive& interactive,
                        zelph::io::OutputCollector&        collector,
                        const std::string&                 name)
    {
        collector.clear();
        interactive.process(".node " + name);

        const std::string marker = "Node ID:";
        for (const auto& event : collector.events())
        {
            const size_t pos = event.text.find(marker);
            if (pos != std::string::npos)
            {
                return std::stoull(event.text.substr(pos + marker.size()));
            }
        }

        REQUIRE_MESSAGE(false, "no node ID in .node output");
        return 0;
    }

    std::string shard_file_name(const ChunkInfo& chunk)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "chunk-%06llu", static_cast<unsigned long long>(chunk.index));

        std::string name(buffer);
        if (!chunk.lang.empty()) name += "-" + chunk.lang;
        return name + ".capnp-packed";
    }

    // What a case might request from the artifact's network.
    struct ArtifactShape
    {
        // False for a .bin in the format of the networks published before
        // the chunk directory: no directory present, and a header that
        // announces none.
        bool with_directory = true;

        // Nodes beyond the named ones. A quantity exceeding what a chunk
        // holds assigns a second chunk to both the left and the right
        // section.
        std::size_t further_nodes = 0;
    };

    // Build a complete artifact tree in `root`, mirroring what
    // tools/emit_zelph_hf_v2.py produces:
    //   net.bin, net.hf-v2.json, net.seek.json, shards/<section>/chunk-*.
    ArtifactPaths build_artifact(const fs::path& root, const ArtifactShape& shape = {})
    {
        fs::remove_all(root);
        fs::create_directories(root);

        ArtifactPaths paths;
        paths.root             = root;
        paths.bin              = root / "net.bin";
        paths.sharded_manifest = root / "net.hf-v2.json";
        paths.seek_manifest    = root / "net.seek.json";
        paths.shards_dir       = root / "shards";
        paths.route_index      = root / "net.route.json";

        const fs::path index_json = root / "index.json";

        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            process_lines(interactive, R"(
alpha ~ beta
gamma ~ beta
)");
            interactive.process(".name alpha de alpha_de");
            interactive.process(".name beta de beta_de");
            paths.alpha = node_id_of(interactive, collector, "alpha");
            for (std::size_t i = 0; i < shape.further_nodes; ++i)
                interactive.graph()->var();
            interactive.process(".save \"" + paths.bin.string() + "\"");

            // Index, shards, and manifests are made from the file as it
            // currently stands, thus aligning with the .bin regardless of
            // format.
            if (!shape.with_directory) write_without_directory(paths.bin, paths.bin);
            interactive.process(".index-file \"" + paths.bin.string() + "\" \"" + index_json.string() + "\"");
        }

        REQUIRE(fs::exists(paths.bin));
        REQUIRE(fs::exists(index_json));

        uint64_t           header_length = 0;
        std::string        section;
        std::istringstream index_stream(read_file(index_json));

        for (std::string line; std::getline(index_stream, line);)
        {
            if (line.find("\"header\"") != std::string::npos)
            {
                header_length = number_field(line, "length");
                continue;
            }

            for (const char* candidate : {"left", "right", "nameOfNode", "nodeOfName"})
            {
                if (line.find(std::string("\"") + candidate + "\": [") != std::string::npos) section = candidate;
            }

            if (line.find("\"chunkIndex\":") == std::string::npos) continue;

            ChunkInfo chunk;
            chunk.index  = number_field(line, "chunkIndex");
            chunk.offset = number_field(line, "offset");
            chunk.length = number_field(line, "length");
            chunk.lang   = string_field(line, "lang");
            paths.sections[section].push_back(chunk);
        }

        REQUIRE(header_length > 0);
        REQUIRE(paths.sections.size() == 4);
        paths.header_length = header_length;

        // The artifact advertises a repository that does not exist: any
        // resolution that leaves the local tree must fail rather than
        // quietly succeed against something real.
        const std::string hf_root = "hf://datasets/acrion/zelph-partial-load-test/net";

        std::ostringstream sharded;
        std::ostringstream seek;
        sharded << "{\n  \"manifestVersion\": \"zelph-hf-layout/v2\",\n"
                << "  \"storageMode\": \"multi-object-shards\",\n"
                // Advertised the way a published manifest does it: capability
                // plus an hf:// path for the sidecar, which the loader has to
                // resolve against the local tree.
                << "  \"selectorModel\": {\"supportedOperations\": [\"header-probe\", \"selected-chunk-read\", \"node-route\"]},\n"
                << "  \"hfObjects\": {\"nodeRouteIndex\": {\"path\": \"" << hf_root << "/net.route.json\"}},\n"
                << "  \"source\": {\"binPath\": \"" << hf_root << "/net.bin\", \"headerLengthBytes\": " << header_length << "},\n"
                << "  \"sections\": {\n";

        // The generic form on purpose: a native Windows path written into a
        // JSON string is not one -- every separator starts an escape, and
        // "\\net.bin" arrives as a newline. Forward slashes open the same file.
        seek << "{\n  \"source\": {\"binPath\": \"" << paths.bin.generic_string()
             << "\", \"headerLengthBytes\": " << header_length << "},\n"
             << "  \"sections\": {\n";

        bool first_section = true;
        for (const char* name : {"left", "right", "nameOfNode", "nodeOfName"})
        {
            if (!first_section)
            {
                sharded << ",\n";
                seek << ",\n";
            }
            first_section = false;

            sharded << "    \"" << name << "\": {\"chunks\": [";
            seek << "    \"" << name << "\": {\"chunks\": [";

            bool first_chunk = true;
            for (const auto& chunk : paths.sections[name])
            {
                const std::string object = std::string(hf_root) + "/shards/" + name + "/" + shard_file_name(chunk);
                write_shard(paths.bin, paths.shards_dir / name / shard_file_name(chunk), chunk.offset, chunk.length);

                if (!first_chunk)
                {
                    sharded << ", ";
                    seek << ", ";
                }
                first_chunk = false;

                sharded << "{\"chunkIndex\": " << chunk.index
                        << ", \"length\": " << chunk.length
                        << ", \"sourceOffset\": " << chunk.offset
                        << ", \"objectPath\": \"" << object << "\"}";
                seek << "{\"chunkIndex\": " << chunk.index
                     << ", \"length\": " << chunk.length
                     << ", \"offset\": " << chunk.offset << "}";
            }

            sharded << "]}";
            seek << "]}";
        }

        sharded << "\n  }\n}\n";
        seek << "\n  }\n}\n";

        std::ofstream(paths.sharded_manifest) << sharded.str();
        std::ofstream(paths.seek_manifest) << seek.str();

        // The node route index: which chunk holds a given node, and which
        // holds a given name. Written for "alpha" and its two names only --
        // enough to tell a routed selection from a wrong one, because the
        // name sections have one chunk per language.
        std::ostringstream route;
        route << "{\n  \"routing\": {\n"
              << "    \"left\":  [{\"chunkIndex\": 0, \"nodes\": [" << paths.alpha << "]}],\n"
              << "    \"right\": [{\"chunkIndex\": 0, \"nodes\": [" << paths.alpha << "]}],\n"
              << "    \"nameOfNode\": [{\"chunkIndex\": " << paths.name_of_node_chunk("zelph")
              << ", \"nodes\": [" << paths.alpha << "]}],\n"
              << "    \"nodeOfName\": [{\"chunkIndex\": " << paths.node_of_name_chunk("de")
              << ", \"lang\": \"de\", \"names\": [\"alpha_de\"]},\n"
              << "                    {\"chunkIndex\": " << paths.node_of_name_chunk("zelph")
              << ", \"lang\": \"zelph\", \"names\": [\"alpha\"]}]\n"
              << "  }\n}\n";
        std::ofstream(paths.route_index) << route.str();

        return paths;
    }

    // A load scenario always starts from an empty network.
    template <typename F>
    void with_fresh_session(F&& body)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        body(collector, interactive);
    }

    fs::path artifact_root(const std::string& name)
    {
        return fs::temp_directory_path() / ("zelph_shard_test_" + name);
    }
}

TEST_CASE("sharding: a local artifact tree loads without shard-root or source-bin")
{
    const auto artifact = build_artifact(artifact_root("local"));

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.sharded_manifest.string() + "\"");

        // Both would still produce a correct network -- through a download
        // and through the sequential whole-file fallback respectively -- so
        // without these two checks the test could not tell a local shard
        // read from the expensive paths it is meant to pin.
        CHECK_FALSE(any_event_contains(collector, "has no local copy"));
        CHECK_FALSE(any_event_contains(collector, "falling back to sequential"));

        collector.clear();
        interactive.process(".stat");
        // Both languages of the source network survive the round trip.
        CHECK(any_output_contains(collector, "Languages: 2"));

        collector.clear();
        interactive.process(".lang de");
        interactive.process(".node alpha_de");
        CHECK(any_event_contains(collector, "alpha_de"));
        CHECK(any_event_contains(collector, "alpha")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: chunk selection addresses one language's name chunk")
{
    const auto artifact = build_artifact(artifact_root("select"));

    const uint64_t de_chunk = artifact.name_of_node_chunk("de");
    REQUIRE(de_chunk != static_cast<uint64_t>(-1));

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                            + "\" left=none right=none nodeOfName=none nameOfNode=" + std::to_string(de_chunk));

        collector.clear();
        interactive.process(".stat");

        // Two names were given in 'de'; the zelph names live in another
        // chunk of the same section and must not come along.
        CHECK(any_output_contains(collector, "de: 2"));
        CHECK(any_output_contains(collector, "Languages: 1"));
        CHECK_FALSE(any_output_contains(collector, "zelph:")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: deselecting a section keeps the sequential stream aligned")
{
    // A .bin is read SEQUENTIALLY. A section that is not read is not consumed
    // either, so everything after it was parsed from the wrong offset:
    // `left=none right=none` made the name loader read ADJACENCY chunks, which
    // capnp reported as "Schema mismatch: Message contains list pointer of
    // non-bytes where text was expected" -- surfacing as "Error converting
    // UTF-8 to string for name_of_node key 1" for every core node, and leaving
    // the network without any names at all.
    //
    // The manifest path never had this: it seeks to each chunk's offset
    // instead of streaming past the others, which is why the selector test
    // above passed with the very same selectors.
    const auto artifact = build_artifact(artifact_root("align"));

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        collector.clear();
        interactive.process(".load-partial \"" + artifact.bin.string() + "\" left=none right=none");

        CHECK_FALSE(any_event_contains(collector, "Error converting UTF-8"));
        CHECK_FALSE(any_event_contains(collector, "Schema mismatch"));

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "de: 2"));
        CHECK(any_output_contains(collector, "Languages: 2"));

        // The names are usable, not merely counted: a name still resolves to
        // the node it was given to.
        collector.clear();
        interactive.process(".node alpha");
        CHECK_FALSE(any_output_contains(collector, "No node found")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: shard-root serves shards from outside the artifact tree")
{
    const auto artifact = build_artifact(artifact_root("shardroot"));

    // Move the shards away, so only shard-root can satisfy the manifest.
    const fs::path relocated = artifact.root / "elsewhere";
    fs::rename(artifact.shards_dir, relocated);

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                            + "\" shard-root=\"" + relocated.string() + "\" left=0 right=none nameOfNode=none nodeOfName=none");

        CHECK_FALSE(any_event_contains(collector, "has no local copy"));
        CHECK_FALSE(any_event_contains(collector, "falling back to sequential"));

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Nodes:")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: a seek-only manifest reads chunks from the .bin")
{
    const auto artifact = build_artifact(artifact_root("seek"));

    // No objectPath anywhere: every chunk is a byte range of the .bin.
    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.seek_manifest.string() + "\"");

        // A chunk that cannot be read at its offset makes the load fall back
        // to reading the whole .bin sequentially, constructing an identical
        // network, so only this line tells the two apart. On macOS, a load of
        // this type fell back at the first chunk that does not start at a
        // block boundary: the chunk was sought through the stream and read
        // through the stream's descriptor, and the C library of BSD leaves
        // the descriptor positioned after the buffer that a seek fills, not
        // at the target of the seek.
        CHECK_FALSE(any_event_contains(collector, "falling back to sequential"));

        collector.clear();
        interactive.process(".lang de");
        interactive.process(".node beta_de");
        CHECK(any_event_contains(collector, "beta_de")); });

    fs::remove_all(artifact.root);
}

// A load using a manifest reserves the node map in the same way a .bin load
// does: for the nodes that the chunks being accessed contain, as the chunk
// directory of the source .bin records them. The artifact's network is too
// small to make the map of a fresh engine grow, so the reserve shows in the
// entries it was made for: every node of the file for either manifest,
// regardless of whether a chunk comes from a shard or from a byte range
// within the .bin, and none where the load reads neither section.
TEST_CASE("sharding: a load through a manifest reserves the node map for the nodes it reads")
{
    const auto artifact = build_artifact(artifact_root("reserve"));

    for (const auto& manifest : {artifact.sharded_manifest, artifact.seek_manifest})
    {
        CAPTURE(manifest.string());
        with_fresh_session([&](auto&, auto& interactive)
                           {
            interactive.process(".load-partial \"" + manifest.string() + "\"");
            const auto whole = interactive.graph()->node_map_state();
            CHECK(whole.entries > 0);
            CHECK(whole.reserved == whole.entries);

            interactive.process(".load-partial \"" + manifest.string() + "\" left=none right=none");
            const auto neither = interactive.graph()->node_map_state();
            CHECK(neither.entries == 0);
            CHECK(neither.reserved == 0); });
    }

    fs::remove_all(artifact.root);
}

// A manifest can enumerate fewer chunks than its .bin contains, down to none
// for a section, and a load reads only the chunks it lists. What it reserves
// corresponds to the contents of those chunks, not every chunk in the
// directory: here the manifest lists the name chunks alone, and the load
// inserts no node and reserves for none.
TEST_CASE("sharding: a load through a manifest reserves for the chunks it lists")
{
    const auto artifact = build_artifact(artifact_root("listed"));
    const auto manifest = artifact.root / "net.names.json";
    {
        std::ostringstream text;
        text << "{\"source\": {\"binPath\": \"" << artifact.bin.generic_string() << "\"}, \"sections\": {"
             << "\"left\": {\"chunks\": []}, \"right\": {\"chunks\": []}";
        for (const char* name : {"nameOfNode", "nodeOfName"})
        {
            text << ", \"" << name << "\": {\"chunks\": [";
            const auto& list = artifact.sections.at(name);
            for (size_t i = 0; i < list.size(); ++i)
                text << (i == 0 ? "" : ", ") << "{\"chunkIndex\": " << list[i].index << ", \"length\": " << list[i].length << ", \"offset\": " << list[i].offset << "}";
            text << "]}";
        }
        text << "}}\n";
        std::ofstream(manifest) << text.str();
    }

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + manifest.string() + "\"");
        CHECK(interactive.graph()->node_map_state().entries == 0);
        CHECK(interactive.graph()->node_map_state().reserved == 0);

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Languages: 2")); });

    fs::remove_all(artifact.root);
}

// A manifest is made from the index of a single .bin and applies solely to that
// file: the chunk lengths it gives are those of the chunks the .bin's directory
// contains, and likewise the offsets at which it reads a chunk from the .bin. A
// manifest that specifies a different length or offset, or lists a chunk not
// present in the directory, pertains to a different file or has been corrupted,
// and the load rejects it before discarding the session's network, explicitly
// naming the discrepancy. Without verification of the offsets, the load would
// discard the network and encounter failure when reading a chunk at an
// incorrect location. The manifest is written here as a manually crafted one
// might be.
TEST_CASE("sharding: a manifest that does not match the chunk directory of its .bin is refused")
{
    const auto artifact = build_artifact(artifact_root("mismatch"));
    const auto manifest = artifact.root / "net.mismatch.json";

    // What the manifest modifies in relation to the index of the artifact:
    // the left chunks, lengthened by `longer` bytes, placed `later` bytes
    // farther along, or given no offset at all, and a nodeOfName chunk
    // beyond what the file contains if `one_more`: a copy of the last one,
    // placed under the next number.
    struct Changes
    {
        uint64_t longer         = 0;
        uint64_t later          = 0;
        bool     without_offset = false;
        bool     one_more       = false;
    };
    const auto write_manifest = [&](const Changes& changes)
    {
        std::ostringstream text;
        text << "{\n  \"source\": {\"binPath\": \"" << artifact.bin.generic_string() << "\", \"headerLengthBytes\": " << artifact.header_length
             << "},\n  \"sections\": {\n";
        bool first_section = true;
        for (const char* name : {"left", "right", "nameOfNode", "nodeOfName"})
        {
            text << (first_section ? "" : ",\n") << "    \"" << name << "\": {\"chunks\": [";
            first_section    = false;
            const auto& list = artifact.sections.at(name);
            const bool  left = std::string(name) == "left";
            for (size_t i = 0; i < list.size(); ++i)
            {
                text << (i == 0 ? "" : ", ") << "{\"chunkIndex\": " << list[i].index << ", \"length\": " << list[i].length + (left ? changes.longer : 0);
                if (!left || !changes.without_offset) text << ", \"offset\": " << list[i].offset + (left ? changes.later : 0);
                text << "}";
            }
            if (changes.one_more && std::string(name) == "nodeOfName")
            {
                text << ", {\"chunkIndex\": " << list.size() << ", \"length\": " << list.back().length << ", \"offset\": " << list.back().offset << "}";
            }
            text << "]}";
        }
        text << "\n  }\n}\n";
        std::ofstream(manifest) << text.str();
    };

    const auto refused = [&](const std::string& difference)
    {
        with_fresh_session([&](auto& collector, auto& interactive)
                           {
            interactive.process("a p b");
            CHECK_THROWS_WITH_AS(interactive.process(".load-partial \"" + manifest.string() + "\""), doctest::Contains(difference), std::runtime_error);

            collector.clear();
            interactive.process("A p B");
            CHECK(answers_contain(collector, "a p b")); });
    };

    const uint64_t left_length = artifact.sections.at("left").front().length;
    const uint64_t left_offset = artifact.sections.at("left").front().offset;
    SUBCASE("a chunk of another length")
    {
        write_manifest({.longer = 1});
        refused("the manifest gives left chunk 0 " + std::to_string(left_length + 1) + " bytes, the directory " + std::to_string(left_length));
    }
    SUBCASE("a chunk at another offset")
    {
        write_manifest({.later = 8});
        refused("the manifest places left chunk 0 at byte " + std::to_string(left_offset + 8) + ", the directory at byte " + std::to_string(left_offset));
    }
    SUBCASE("a chunk without an offset")
    {
        // A chunk that the load reads from the .bin, which it would read at the
        // start of the file, where the header stands.
        write_manifest({.without_offset = true});
        refused("the manifest gives left chunk 0 no offset, the directory places it at byte " + std::to_string(left_offset));
    }
    SUBCASE("a chunk the file does not have")
    {
        write_manifest({.one_more = true});
        refused("the manifest lists nodeOfName chunk 2, which the directory does not");
    }

    fs::remove_all(artifact.root);
}

// A load through a manifest reads the header of the .bin specified by the
// manifest and holds the chunk directory against the file's size, whether the
// .bin is local or remote, before discarding the session's network. A local
// .bin missing its last bytes, as a copy that broke off would leave it, is
// refused at that point. The case asks the graph and not merely the message:
// without this comparison, the load discards the graph and fails at the chunk
// that is truncated, and when the shards are located alongside the manifest,
// it even succeeds.
TEST_CASE("sharding: a manifest over a local .bin that lost bytes is refused before the session's network is discarded")
{
    const auto        artifact = build_artifact(artifact_root("cut-bin"));
    const std::string bin      = read_file(artifact.bin);
    const uint64_t    size     = bin.size();
    std::ofstream(artifact.bin, std::ios::binary | std::ios::trunc).write(bin.data(), static_cast<std::streamsize>(size - 8));

    for (const auto& manifest : {artifact.sharded_manifest, artifact.seek_manifest})
    {
        CAPTURE(manifest.string());
        with_fresh_session([&](auto& collector, auto& interactive)
                           {
            interactive.process("a p b");
            CHECK_THROWS_WITH_AS(interactive.process(".load-partial \"" + manifest.string() + "\""),
                                 doctest::Contains("nodeOfName chunk 1 ends at byte " + std::to_string(size) + ", but the file ends at byte " + std::to_string(size - 8)),
                                 std::runtime_error);

            collector.clear();
            interactive.process("A p B");
            CHECK(answers_contain(collector, "a p b")); });
    }

    fs::remove_all(artifact.root);
}

// A chunk that the load reads from the .bin stands at the offset specified by
// the chunk directory, which the load has compared with the manifest's before
// any reading. A chunk that cannot be read at that offset is either damaged,
// or the read operation itself is at fault, and the load ends on it, naming
// the chunk and its offset. Previously, it would fall back to reading the
// whole .bin from its start, which reaches the same bytes: for a damaged
// chunk, this only added a second, less precise failure, and for a faulty
// read, it built the right network and thus concealed the fault. Here, the
// bytes of the first left chunk are overwritten in place, which leaves the
// directory matching the file.
TEST_CASE("sharding: a chunk that cannot be read at the offset its chunk directory gives ends the load")
{
    const auto  artifact = build_artifact(artifact_root("damaged-chunk"));
    const auto& chunk    = artifact.sections.at("left").front();
    {
        std::fstream bin(artifact.bin, std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(bin.is_open());
        bin.seekp(static_cast<std::streamoff>(chunk.offset));
        bin << std::string(static_cast<size_t>(chunk.length), '\xff');
    }

    // The same manifest, with its .bin named by a URI as a published one names
    // it: the load finds the copy beside the manifest and reads it, and the
    // message names the copy, which is the file that is damaged.
    const auto  by_uri = artifact.root / "net.seek-uri.json";
    std::string text   = read_file(artifact.seek_manifest);
    const auto  named  = text.find(artifact.bin.generic_string());
    REQUIRE(named != std::string::npos);
    text.replace(named, artifact.bin.generic_string().size(), "hf://datasets/acrion/zelph-partial-load-test/net/net.bin");
    std::ofstream(by_uri) << text;

    for (const auto& manifest : {artifact.seek_manifest, by_uri})
    {
        CAPTURE(manifest.string());
        with_fresh_session([&](auto& collector, auto& interactive)
                           {
            interactive.process(".stat-file \"" + artifact.bin.string() + "\"");
            REQUIRE(any_output_contains(collector, "Chunk Directory: matches the file"));

            interactive.process("a p b");
            try
            {
                interactive.process(".load-partial \"" + manifest.string() + "\"");
                FAIL("expected the load to fail");
            }
            catch (const std::runtime_error& e)
            {
                const std::string message(e.what());
                CAPTURE(message);
                CHECK(message.find("Failed to read left chunk 0 of '") != std::string::npos);
                CHECK(message.find("net.bin' at byte " + std::to_string(chunk.offset) + ": ") != std::string::npos);
                CHECK(message.find("hf://") == std::string::npos);
                CHECK(message.find("The previous network was already discarded -- use .new to start over") != std::string::npos);
                // The content that kj adds to its own message stays out of this message.
                CHECK(message.find("stack:") == std::string::npos);
                CHECK(message.find(".c++") == std::string::npos);
            }
            CHECK_FALSE(any_event_contains(collector, "falling back to sequential"));
            CHECK(node_count(collector, interactive) == 0); });
    }

    fs::remove_all(artifact.root);
}

// A .bin lacking a chunk directory, which every network published so far is,
// contains nothing against which a manifest could be held, so a chunk that
// cannot be read at the location the manifest specifies may instead reside
// elsewhere. For such a file the load continues to fall back to the whole
// .bin, reading it from the beginning, and reports this fact. Here the
// manifest places the first left chunk eight bytes beyond where it should be,
// as one made from the index of a different file would: the load reports the
// fallback, and the network arrives as a whole.
TEST_CASE("sharding: a chunk that a .bin without a chunk directory does not have where the manifest says is read from the whole .bin")
{
    const auto artifact = build_artifact(artifact_root("stale-manifest"), {.with_directory = false});
    const auto manifest = artifact.root / "net.stale.json";
    {
        std::ostringstream text;
        text << "{\n  \"source\": {\"binPath\": \"" << artifact.bin.generic_string() << "\", \"headerLengthBytes\": " << artifact.header_length
             << "},\n  \"sections\": {\n";
        bool first_section = true;
        for (const char* name : {"left", "right", "nameOfNode", "nodeOfName"})
        {
            text << (first_section ? "" : ",\n") << "    \"" << name << "\": {\"chunks\": [";
            first_section    = false;
            const auto& list = artifact.sections.at(name);
            for (size_t i = 0; i < list.size(); ++i)
            {
                const uint64_t offset = list[i].offset + (std::string(name) == "left" && i == 0 ? 8 : 0);
                text << (i == 0 ? "" : ", ") << "{\"chunkIndex\": " << list[i].index << ", \"length\": " << list[i].length << ", \"offset\": " << offset << "}";
            }
            text << "]}";
        }
        text << "\n  }\n}\n";
        std::ofstream(manifest) << text.str();
    }

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + manifest.string() + "\"");
        CHECK(any_event_contains(collector, "Shard chunk left/0 failed"));
        CHECK(any_event_contains(collector, "falling back to sequential"));
        CHECK(interactive.graph()->node_map_state().entries > 0);

        collector.clear();
        interactive.process(".lang de");
        interactive.process(".node beta_de");
        CHECK(any_event_contains(collector, "beta_de")); });

    fs::remove_all(artifact.root);
}

// A pipe named as the .bin of a manifest. A chunk that the load reads from the
// .bin is read at its offset, and since a pipe cannot be sought, such a load
// is refused prior to opening the pipe and while the session's network remains
// in place. It previously read the header from the pipe, discarded the
// network, and opened the pipe once more for the first chunk, waiting for a
// writer that had already concluded; with a writer that serves every reader,
// it could not seek and instead fell back to reading the whole .bin from the
// pipe. In this case, the writer offers the file to each reader that opens the
// pipe, ensuring that a load which does open it ends as well, and shows in
// what it leaves behind rather than in a case that never returns. The .bin
// lacks a chunk directory: one possessing a directory is refused from a pipe
// anyway, due to its unknown size. A load that reads no chunk from the .bin
// needs no seek and is not refused: the header alone, a selection with no
// chunk, and a manifest where all chunks originate from shards.
#ifndef _WIN32
namespace
{
    // A named pipe in which a writer offers `content` to a reader that opens
    // the pipe, mirroring the behaviour of `cat file > pipe`, and re-offers it
    // solely after the reader has closed the pipe. The writer never blocks
    // during a call: it asks every millisecond whether a reader holds the pipe
    // open, guaranteeing termination on every exit path, even in the case of
    // an exception. A reader that opens the pipe within that millisecond
    // following another's closure is treated as the same reader and offered
    // nothing, so a case ensures each load has its own pipe.
    class OfferedPipe
    {
    public:
        OfferedPipe(fs::path path, std::string content)
            : _path(std::move(path))
            , _content(std::move(content))
        {
            REQUIRE(mkfifo(_path.c_str(), 0600) == 0);
            _writer = std::thread([this]
                                  { offer(); });
        }
        ~OfferedPipe()
        {
            _stop = true;
            _writer.join();
            std::error_code ignored;
            fs::remove(_path, ignored);
        }
        OfferedPipe(const OfferedPipe&)            = delete;
        OfferedPipe& operator=(const OfferedPipe&) = delete;

        // How many readers have been presented with the content.
        int offered() const { return _offered.load(); }

    private:
        // Opening a pipe for writing, without waiting, succeeds precisely when
        // a reader holds it open.
        bool has_reader(int& descriptor) const
        {
            descriptor = open(_path.c_str(), O_WRONLY | O_NONBLOCK);
            return descriptor >= 0;
        }

        void offer()
        {
            // A reader that closes the pipe early must not end the process.
            sigset_t pipe_signal;
            sigemptyset(&pipe_signal);
            sigaddset(&pipe_signal, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr);

            const auto pause = []
            { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
            while (!_stop)
            {
                int out = -1;
                if (!has_reader(out))
                {
                    pause();
                    continue;
                }
                ++_offered;
                const auto written = write(out, _content.data(), _content.size());
                (void)written;
                close(out);

                // The subsequent offer is intended for the next reader, thus
                // this one must close the pipe first.
                while (!_stop && has_reader(out))
                {
                    close(out);
                    pause();
                }
            }
        }

        fs::path          _path;
        std::string       _content;
        std::atomic<bool> _stop{false};
        std::atomic<int>  _offered{0};
        std::thread       _writer;
    };
}

TEST_CASE("sharding: a manifest that reads chunks from a .bin that is no regular file is refused before the pipe is opened")
{
    const auto        artifact = build_artifact(artifact_root("pipe-bin"), {.with_directory = false});
    const std::string bin      = read_file(artifact.bin);
    const fs::path    path     = artifact.root / "net.pipe";

    std::string manifest_text = read_file(artifact.seek_manifest);
    const auto  named         = manifest_text.find(artifact.bin.generic_string());
    REQUIRE(named != std::string::npos);
    manifest_text.replace(named, artifact.bin.generic_string().size(), path.generic_string());
    const fs::path manifest = artifact.root / "net.pipe.json";
    std::ofstream(manifest) << manifest_text;

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process("a p b");
        {
            const OfferedPipe pipe(path, bin);
            CHECK_THROWS_WITH_AS(interactive.process(".load-partial \"" + manifest.string() + "\""),
                                 doctest::Contains("it reads chunks at their offsets in '" + path.generic_string() + "', which is not a regular file"),
                                 std::runtime_error);
            CHECK(pipe.offered() == 0);
            CHECK_FALSE(any_event_contains(collector, "falling back to sequential"));
        }
        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        // The header by itself requires no seek, and it is read from the pipe.
        {
            const OfferedPipe pipe(path, bin);
            collector.clear();
            interactive.process(".load-partial \"" + manifest.string() + "\" meta-only");
            CHECK(any_event_contains(collector, "Header-only manifest load complete."));
            CHECK(pipe.offered() == 1);
        }

        // Neither does a selection that reads no chunk.
        {
            const OfferedPipe pipe(path, bin);
            collector.clear();
            interactive.process(".load-partial \"" + manifest.string() + "\" left=none right=none nameOfNode=none nodeOfName=none");
            CHECK(any_event_contains(collector, "left chunks=0/1"));
            CHECK(pipe.offered() == 1);
        }

        // Nor a manifest whose chunks all come from shards: only its header is
        // read from the pipe, which is named as its .bin in this context.
        {
            const OfferedPipe pipe(path, bin);
            collector.clear();
            interactive.process(".load-partial \"" + artifact.sharded_manifest.string() + "\" source-bin=\"" + path.string() + "\"");
            CHECK_FALSE(any_event_contains(collector, "falling back to sequential"));
            CHECK(pipe.offered() == 1);
            collector.clear();
            interactive.process(".stat");
            CHECK(any_output_contains(collector, "Languages: 2"));
        } });

    fs::remove_all(artifact.root);
}
#endif

// No network published before the chunk directory possesses one, and a load
// via a manifest is the method by which such a network is loaded in part.
// Nothing within such a file reliably indicates how many nodes its chunks
// hold, so the load reads it using the reader it has always used and reserves
// nothing: it neither refuses the file due to the absence of the directory,
// nor sizes the node map from the chunk counts in the header, an estimate
// that reserved a million entries for every chunk except the last. For a
// single chunk, that approximation yields zero as well, hence the second
// subcase, a network with two chunks in the left section and two chunks in
// the right section.
TEST_CASE("sharding: a .bin without a chunk directory loads through a manifest and reserves nothing" * doctest::test_suite("slow"))
{
    SUBCASE("one chunk per section, from shards and from byte ranges of the .bin")
    {
        const auto artifact = build_artifact(artifact_root("no-directory"), {.with_directory = false});
        for (const auto& manifest : {artifact.sharded_manifest, artifact.seek_manifest})
        {
            CAPTURE(manifest.string());
            with_fresh_session([&](auto& collector, auto& interactive)
                               {
                interactive.process(".stat-file \"" + artifact.bin.string() + "\"");
                REQUIRE(any_output_contains(collector, "Chunk Directory: none"));

                interactive.process(".load-partial \"" + manifest.string() + "\"");
                CHECK(interactive.graph()->node_map_state().entries > 0);
                CHECK(interactive.graph()->node_map_state().reserved == 0);

                collector.clear();
                interactive.process(".stat");
                CHECK(any_output_contains(collector, "Languages: 2")); });
        }
        fs::remove_all(artifact.root);
    }
    SUBCASE("two chunks per section")
    {
        constexpr std::size_t chunk    = zelph::network::serialization::chunk_entries;
        const auto            artifact = build_artifact(artifact_root("no-directory-two-chunks"), {.with_directory = false, .further_nodes = chunk});
        REQUIRE(artifact.sections.at("left").size() == 2);
        REQUIRE(artifact.sections.at("right").size() == 2);

        with_fresh_session([&](auto&, auto& interactive)
                           {
            interactive.process(".load-partial \"" + artifact.seek_manifest.string() + "\"");
            const auto state = interactive.graph()->node_map_state();
            CHECK(state.entries > chunk);
            CHECK(state.reserved == 0);
            CHECK(state.growths > 0); });
        fs::remove_all(artifact.root);
    }
}

// The cases that load straight from the repository are omitted on Windows. The
// engine composes the command that starts curl for a POSIX shell, with its
// arguments enclosed in single quotes, and hands it to std::system; on Windows
// this is cmd.exe, which does not read single quotes as quote markers, so the
// stand-in for curl could not be initiated via that command. The cases also
// set their environment using setenv and unsetenv.
#ifndef _WIN32
namespace
{
    // Sets environment variables for the scope of a single test and reinstates
    // their prior values afterwards.
    class ScopedEnvironment
    {
    public:
        explicit ScopedEnvironment(const std::vector<std::pair<std::string, std::string>>& values)
        {
            for (const auto& [name, value] : values)
            {
                const char* before = std::getenv(name.c_str());
                _before.emplace_back(name, before == nullptr ? std::nullopt : std::optional<std::string>(before));
                setenv(name.c_str(), value.c_str(), 1);
            }
        }
        ~ScopedEnvironment()
        {
            for (const auto& [name, before] : _before)
            {
                if (before)
                    setenv(name.c_str(), before->c_str(), 1);
                else
                    unsetenv(name.c_str());
            }
        }
        ScopedEnvironment(const ScopedEnvironment&)            = delete;
        ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    private:
        std::vector<std::pair<std::string, std::optional<std::string>>> _before;
    };

    // The environment for a load straight from the repository: the cache is a
    // directory specific to the test, and the program started in the place of
    // curl is this test binary, which serves the files located under
    // `repository` via the URLs of
    // hf://datasets/acrion/zelph-partial-load-test/ (curl_stand_in.hpp).
    // `further` adds what a single case needs.
    std::vector<std::pair<std::string, std::string>> repository_environment(const fs::path&                                  repository,
                                                                            const fs::path&                                  cache,
                                                                            std::vector<std::pair<std::string, std::string>> further = {})
    {
        REQUIRE(fs::exists(zelph::test::curl_stand_in::program()));
        further.emplace_back("ZELPH_HF_CACHE_DIR", cache.string());
        further.emplace_back("ZELPH_HF_CURL_BIN", zelph::test::curl_stand_in::program().string());
        further.emplace_back(zelph::test::curl_stand_in::repository_variable, repository.string());
        return further;
    }
}

// What the chunk directory was made for: a partial load straight from the
// repository. It fetches the .bin's header via the range specified in the
// manifest and acquires the chunks individually, with no local file having a
// size to check the directory against: the directory arrives alongside the
// header range, and it is checked against the size of the remote object. The
// repository is a local directory served by a stand-in for curl, as no test
// can access the network, and the cache is a directory belonging to the test
// itself. A remote .bin that has lost bytes no longer matches its directory,
// and a manifest whose header range concludes within the directory cannot
// have it read: either case is rejected before the session's network is
// discarded.
TEST_CASE("sharding: a load straight from the repository checks the directory against the remote object")
{
    const fs::path repository = artifact_root("remote");
    const fs::path cache      = artifact_root("remote-cache");
    fs::remove_all(repository);
    fs::remove_all(cache);
    fs::create_directories(repository);
    const auto artifact = build_artifact(repository / "net");

    const ScopedEnvironment environment(repository_environment(repository, cache));
    const std::string       manifest = "hf://datasets/acrion/zelph-partial-load-test/net/net.hf-v2.json";

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial " + manifest);
        CHECK(any_event_contains(collector, "has no local copy"));
        const auto state = interactive.graph()->node_map_state();
        CHECK(state.entries > 0);
        CHECK(state.reserved == state.entries); });

    const auto refused = [&](const std::string& reason)
    {
        with_fresh_session([&](auto& collector, auto& interactive)
                           {
            interactive.process("a p b");
            CHECK_THROWS_WITH_AS(interactive.process(".load-partial " + manifest), doctest::Contains(reason), std::runtime_error);

            collector.clear();
            interactive.process("A p B");
            CHECK(answers_contain(collector, "a p b")); });
    };

    const std::string bin  = read_file(artifact.bin);
    const uint64_t    size = bin.size();
    SUBCASE("a remote .bin that lost bytes")
    {
        std::ofstream(artifact.bin, std::ios::binary | std::ios::trunc).write(bin.data(), static_cast<std::streamsize>(size - 8));
        refused("nodeOfName chunk 1 ends at byte " + std::to_string(size) + ", but the file ends at byte " + std::to_string(size - 8));
    }
    SUBCASE("a header range that ends inside the directory")
    {
        std::string text   = read_file(artifact.sharded_manifest);
        const auto  needle = "\"headerLengthBytes\": " + std::to_string(artifact.header_length);
        const auto  at     = text.find(needle);
        REQUIRE(at != std::string::npos);
        text.replace(at, needle.size(), "\"headerLengthBytes\": " + std::to_string(artifact.header_length - 8));
        std::ofstream(artifact.sharded_manifest, std::ios::trunc) << text;
        refused("its headerLengthBytes (" + std::to_string(artifact.header_length - 8) + ") do not cover the chunk directory");
    }

    fs::remove_all(repository);
    fs::remove_all(cache);
}

// A load via a manifest discards the session's network before reading the
// first chunk, causing a shard that fails to parse to do so after the discard,
// and the message indicates this, as every message following the discard does.
// For a remote .bin, only the header range is accessible, leaving nothing that
// can stand in for the shard, and the load ends there, naming the chunk and
// the file it read. Previously, it would fall back regardless: it read the
// header range from the cache as though it were the .bin and reported that
// file's chunk directory as not matching it, which conveyed no information
// about the shard. The shard is impaired at the location where the load
// retrieves it, within the repository, and retains its length.
TEST_CASE("sharding: a load straight from the repository that fails after discarding the session's network says so")
{
    const fs::path repository = artifact_root("remote-fallback");
    const fs::path cache      = artifact_root("remote-fallback-cache");
    fs::remove_all(repository);
    fs::remove_all(cache);
    fs::create_directories(repository);
    const auto artifact = build_artifact(repository / "net");

    const auto& chunk = artifact.sections.at("left").front();
    std::ofstream(artifact.shards_dir / "left" / shard_file_name(chunk), std::ios::binary | std::ios::trunc)
        << std::string(static_cast<size_t>(chunk.length), '\xff');

    const ScopedEnvironment environment(repository_environment(repository, cache));

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process("a p b");
        REQUIRE(node_count(collector, interactive) > 0);
        try
        {
            interactive.process(".load-partial hf://datasets/acrion/zelph-partial-load-test/net/net.hf-v2.json");
            FAIL("expected the load to fail");
        }
        catch (const std::runtime_error& e)
        {
            const std::string message(e.what());
            CAPTURE(message);
            CHECK(message.find("Failed to read left chunk 0 from its shard '") != std::string::npos);
            CHECK(message.find("its chunk directory does not match the file") == std::string::npos);
            CHECK(message.find("The previous network was already discarded -- use .new to start over") != std::string::npos);
        }
        CHECK_FALSE(any_event_contains(collector, "falling back to sequential"));
        CHECK(node_count(collector, interactive) == 0); });

    fs::remove_all(repository);
    fs::remove_all(cache);
}

// The size against which the directory of a remote .bin is held is the size of
// the object to which the URL resolves. The repository responds to a request
// for a large file by issuing a redirect to the file's actual location, and
// curl logs the headers from each response in the chain: the relevant size
// resides in the last block, while the redirect's block contains the length of
// its own, brief body. In this case, the redirect claims a size that the .bin
// lacks, so a load that relied on the first block's length would hold the
// directory against that size and refuse a file that is sound.
TEST_CASE("sharding: the size of a remote .bin is that of the last response of a redirect chain")
{
    const fs::path repository = artifact_root("redirect");
    const fs::path cache      = artifact_root("redirect-cache");
    fs::remove_all(repository);
    fs::remove_all(cache);
    fs::create_directories(repository);
    const auto artifact = build_artifact(repository / "net");

    const ScopedEnvironment environment(repository_environment(repository, cache, {{zelph::test::curl_stand_in::redirect_variable, "1340"}}));
    REQUIRE(fs::file_size(artifact.bin) != 1340);

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial hf://datasets/acrion/zelph-partial-load-test/net/net.hf-v2.json");
        CHECK(any_event_contains(collector, "has no local copy"));
        const auto state = interactive.graph()->node_map_state();
        CHECK(state.entries > 0);
        CHECK(state.reserved == state.entries); });

    fs::remove_all(repository);
    fs::remove_all(cache);
}

// A chunk directory is believed once it matches the size of its file, and for a
// remote .bin that size is the one the server reports for the object. A server
// that reports none leaves the directory without anything to be held against,
// and the load refuses the .bin for that, while the session's network is still
// there, instead of reserving what an unchecked directory says.
TEST_CASE("sharding: a remote .bin whose size the server does not report is refused")
{
    const fs::path repository = artifact_root("no-length");
    const fs::path cache      = artifact_root("no-length-cache");
    fs::remove_all(repository);
    fs::remove_all(cache);
    fs::create_directories(repository);
    build_artifact(repository / "net");

    const ScopedEnvironment environment(repository_environment(repository, cache, {{zelph::test::curl_stand_in::no_length_variable, "1"}}));
    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process("a p b");
        CHECK_THROWS_WITH_AS(interactive.process(".load-partial hf://datasets/acrion/zelph-partial-load-test/net/net.hf-v2.json"),
                             doctest::Contains("the size of the remote .bin is unknown, so its chunk directory cannot be checked"),
                             std::runtime_error);

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b")); });

    fs::remove_all(repository);
    fs::remove_all(cache);
}

// A .bin lacking a chunk directory, straight from the repository -- this is how
// networks published before the directory are loaded from there: the header
// range brings no directory, nothing is held against the object's size, and the
// load reads the chunks while reserving nothing.
TEST_CASE("sharding: a .bin without a chunk directory loads straight from the repository and reserves nothing")
{
    const fs::path repository = artifact_root("remote-no-directory");
    const fs::path cache      = artifact_root("remote-no-directory-cache");
    fs::remove_all(repository);
    fs::remove_all(cache);
    fs::create_directories(repository);
    build_artifact(repository / "net", {.with_directory = false});

    const ScopedEnvironment environment(repository_environment(repository, cache));
    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial hf://datasets/acrion/zelph-partial-load-test/net/net.hf-v2.json");
        CHECK(any_event_contains(collector, "has no local copy"));
        CHECK(interactive.graph()->node_map_state().entries > 0);
        CHECK(interactive.graph()->node_map_state().reserved == 0);

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Languages: 2")); });

    fs::remove_all(repository);
    fs::remove_all(cache);
}
#endif

TEST_CASE("sharding: an out-of-range chunk selector is rejected without collateral")
{
    const auto artifact = build_artifact(artifact_root("range"));

    const size_t left_chunks = artifact.sections.at("left").size();

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        CHECK_THROWS(interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                                         + "\" left=" + std::to_string(left_chunks)));

        // A typo in a selector is the ordinary way to get this command wrong,
        // so the refusal must cost nothing. The manifest loader always
        // checked before touching the graph; the plain-.bin loader did not,
        // and left a network without even its core nodes (see
        // test_bin_inspection.cpp). Pinned on this side too, so the two
        // loaders cannot drift apart again.
        collector.clear();
        interactive.process("a p b");
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: route-name selects the chunk of one language")
{
    const auto artifact = build_artifact(artifact_root("routename"));

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                            + "\" route-name=alpha_de route-lang=de");

        // Exactly the de chunk of nodeOfName, and nothing of the other
        // sections: the sidecar names no chunk for them under a name route.
        CHECK(any_event_contains(collector, "nodeOfName chunks=1/2"));
        CHECK(any_event_contains(collector, "route_requested=true"));
        CHECK_FALSE(any_event_contains(collector, "has no local copy"));

        // The name resolves -- but the node itself is not in this view: the
        // adjacency sections were not part of a name route, so .node reports
        // the name as unknown. That asymmetry is inherent to selecting the
        // two name directions independently, and the point of the route is to
        // narrow WHICH chunk has to be read, not to complete the graph.
        collector.clear();
        interactive.process(".lang de");
        interactive.process(".node alpha_de");
        CHECK(any_event_contains(collector, "Node ID:"));
        CHECK(any_event_contains(collector, "No names in any language"));

        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "de: 2"));
        CHECK_FALSE(any_output_contains(collector, "zelph:")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: route-node selects the chunks that hold one node")
{
    const auto artifact = build_artifact(artifact_root("routenode"));

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                            + "\" route-node=" + std::to_string(artifact.alpha));

        // Adjacency and the node's own name travel together; the reverse
        // direction (nodeOfName) is a name route, not a node route.
        CHECK(any_event_contains(collector, "left chunks=1/1"));
        CHECK(any_event_contains(collector, "right chunks=1/1"));
        CHECK(any_event_contains(collector, "nameOfNode chunks=1/2"));
        CHECK(any_event_contains(collector, "nodeOfName chunks=0/2"));

        collector.clear();
        interactive.process(".stat");
        // The zelph names came along, the de names did not.
        CHECK(any_output_contains(collector, "zelph: 3"));
        CHECK_FALSE(any_output_contains(collector, "de:")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: a routed and an explicit selector combine")
{
    const auto artifact = build_artifact(artifact_root("routeboth"));

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                            + "\" route-node=" + std::to_string(artifact.alpha)
                            + " route-name=alpha_de route-lang=de nodeOfName="
                            + std::to_string(artifact.node_of_name_chunk("zelph")));

        // The de chunk from the name route, the zelph chunk from the explicit
        // selector: both, not one of them.
        CHECK(any_event_contains(collector, "nodeOfName chunks=2/2"));

        collector.clear();
        interactive.process(".node alpha");
        CHECK(any_event_contains(collector, "alpha")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: route selectors are rejected without what they need")
{
    const auto artifact = build_artifact(artifact_root("routebad"));

    with_fresh_session([&](auto&, auto& interactive)
                       {
        // A name route without its language cannot be resolved.
        CHECK_THROWS(interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                                         + "\" route-name=alpha_de"));

        // A node the sidecar does not mention resolves no chunk at all, which
        // is an error rather than an empty network.
        CHECK_THROWS(interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                                         + "\" route-node=999999"));

        // The seek-only manifest advertises no route index.
        CHECK_THROWS(interactive.process(".load-partial \"" + artifact.seek_manifest.string()
                                         + "\" route-node=" + std::to_string(artifact.alpha))); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: a manifest naming a .bin that is not there leaves the session alone")
{
    const auto artifact = build_artifact(artifact_root("missing-bin"));

    // The seek manifest names the .bin by a local path, so nothing here can
    // reach for the network. The manifest loader used to discard the graph
    // before it ever tried to open that file: what was left had not even its
    // core nodes, and the message was "Failed to open file for reading".
    fs::rename(artifact.bin, artifact.root / "moved.bin");

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        try
        {
            interactive.process(".load-partial \"" + artifact.seek_manifest.string() + "\"");
            FAIL("expected the load to be refused");
        }
        catch (const std::runtime_error& e)
        {
            const std::string message(e.what());
            CHECK(message.find("net.bin") != std::string::npos);
            CHECK(message.find("not there") != std::string::npos);
        }

        collector.clear();
        interactive.process("a p b");
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b")); });

    fs::remove_all(artifact.root);
}

// The header of the .bin specified by a manifest is read before the session's
// network is discarded, so a .bin that is no network at all results in the
// session remaining unchanged: the load fails at the header, and the graph
// still answers. While the header was read after the discard, such a manifest
// left a network devoid even of its core nodes. The message says what the file
// is not, using the words a .load of that file employs for it. It once was the
// exception of the reader as it stood, with the place in its source and its
// stack trace.
TEST_CASE("sharding: a manifest naming a .bin that is not a network leaves the session alone")
{
    const auto artifact = build_artifact(artifact_root("no-network-bin"));
    std::ofstream(artifact.bin, std::ios::binary | std::ios::trunc) << "hello, this is not a serialized network";

    for (const auto& manifest : {artifact.sharded_manifest, artifact.seek_manifest})
    {
        CAPTURE(manifest.string());
        with_fresh_session([&](auto& collector, auto& interactive)
                           {
            interactive.process("a p b");
            try
            {
                interactive.process(".load-partial \"" + manifest.string() + "\"");
                FAIL("expected the load to fail");
            }
            catch (const std::runtime_error& e)
            {
                const std::string message(e.what());
                CAPTURE(message);
                CHECK(message.find("net.bin' as a zelph .bin file (truncated, or not a .bin at all): ") != std::string::npos);
                // The file that was read is named: the sharded manifest names
                // its .bin via a URI, and the copy located alongside it is the
                // one that failed.
                CHECK(message.find("hf://") == std::string::npos);
                CHECK(message.find("stack:") == std::string::npos);
                CHECK(message.find(".c++") == std::string::npos);
                CHECK(message.find("already discarded") == std::string::npos);
            }

            collector.clear();
            interactive.process("A p B");
            CHECK(answers_contain(collector, "a p b")); });
    }

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: an unreadable shard falls back to the .bin and says so")
{
    const auto artifact = build_artifact(artifact_root("bad-shard"));

    // Emptied, not deleted: a shard that is ABSENT is fetched, which no test
    // may trigger. One that is present and unreadable exercises the fallback
    // without leaving the machine.
    const auto& chunk = artifact.sections.at("left").front();
    const auto  shard = artifact.shards_dir / "left" / shard_file_name(chunk);
    REQUIRE(fs::exists(shard));
    std::ofstream(shard, std::ios::binary | std::ios::trunc);

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.sharded_manifest.string() + "\"");

        // The substitution is announced. Every other test in this file
        // asserts the ABSENCE of this line to prove it read the shards, which
        // is only worth anything if the line appears when it should.
        CHECK(any_event_contains(collector, "falling back to sequential"));
        CHECK_FALSE(any_event_contains(collector, "has no local copy"));

        // The line gives the reason in words, excluding the stack trace that
        // the reader's exception carries.
        CHECK_FALSE(any_event_contains(collector, "stack:"));

        // And the network is complete, because the .bin next to the manifest
        // can serve what the shard could not.
        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Languages: 2")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: a route selector that resolves to nothing is refused")
{
    const auto        artifact = build_artifact(artifact_root("routebad"));
    const std::string m        = artifact.sharded_manifest.string();

    // Every one of these is a question the route index cannot answer. None
    // of them may cost the session: the selectors are resolved and checked
    // before the graph is discarded.
    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        for (const std::string& tail : {std::string(" route-node=999999999"),
                                        std::string(" route-node=abc"),
                                        std::string(" route-name=nosuchname route-lang=zelph"),
                                        std::string(" route-name=alpha route-lang=fr")})
        {
            CHECK_THROWS_AS(interactive.process(".load-partial \"" + m + "\"" + tail), std::runtime_error);
        }

        collector.clear();
        interactive.process("a p b");
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b")); });

    // A routing entry naming a chunk the section does not have is caught by
    // the same selector validation as a hand-typed one.
    {
        std::string text = read_file(artifact.route_index);
        const auto  from = std::string("\"left\":  [{\"chunkIndex\": 0");
        const auto  pos  = text.find(from);
        REQUIRE(pos != std::string::npos);
        text.replace(pos, from.size(), "\"left\":  [{\"chunkIndex\": 99");
        std::ofstream(artifact.route_index, std::ios::trunc) << text;
    }

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        CHECK_THROWS_AS(interactive.process(".load-partial \"" + m + "\" route-node="
                                            + std::to_string(artifact.alpha)),
                        std::runtime_error);

        collector.clear();
        interactive.process("a p b");
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: a route index that points at the wrong chunk says so")
{
    const auto artifact = build_artifact(artifact_root("routewrong"));

    // The route index is a sidecar the emitter writes and the loader trusts:
    // it says which chunk holds a name, and that chunk is what gets read. An
    // index pointing elsewhere therefore produced an ordinary, successful
    // load of the wrong pieces -- the requested name simply was not in the
    // result, with nothing said. Here alpha_de is routed to the zelph chunk.
    {
        const auto  wrong  = artifact.node_of_name_chunk("zelph");
        const auto  needle = "{\"chunkIndex\": " + std::to_string(artifact.node_of_name_chunk("de"))
                           + ", \"lang\": \"de\"";
        std::string text   = read_file(artifact.route_index);
        const auto  pos    = text.find(needle);
        REQUIRE(pos != std::string::npos);
        text.replace(pos, needle.size(), "{\"chunkIndex\": " + std::to_string(wrong) + ", \"lang\": \"de\"");
        std::ofstream(artifact.route_index, std::ios::trunc) << text;
    }

    with_fresh_session([&](auto& collector, auto& interactive)
                       {
        interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                            + "\" route-name=alpha_de route-lang=de");
        CHECK(any_event_contains(collector, "the index and the shards disagree"));
        CHECK(any_event_contains(collector, "alpha_de")); });

    fs::remove_all(artifact.root);
}
