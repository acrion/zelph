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

#include "test_helpers.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

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

    std::string shard_file_name(const ChunkInfo& chunk)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "chunk-%06llu", static_cast<unsigned long long>(chunk.index));

        std::string name(buffer);
        if (!chunk.lang.empty()) name += "-" + chunk.lang;
        return name + ".capnp-packed";
    }

    // Build a complete artifact tree in `root`, mirroring what
    // tools/emit_zelph_hf_v2.py produces:
    //   net.bin, net.hf-v2.json, net.seek.json, shards/<section>/chunk-*.
    ArtifactPaths build_artifact(const fs::path& root)
    {
        fs::remove_all(root);
        fs::create_directories(root);

        ArtifactPaths paths;
        paths.root             = root;
        paths.bin              = root / "net.bin";
        paths.sharded_manifest = root / "net.hf-v2.json";
        paths.seek_manifest    = root / "net.seek.json";
        paths.shards_dir       = root / "shards";

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
            interactive.process(".save \"" + paths.bin.string() + "\"");
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

        // The artifact advertises a repository that does not exist: any
        // resolution that leaves the local tree must fail rather than
        // quietly succeed against something real.
        const std::string hf_root = "hf://datasets/acrion/zelph-partial-load-test/net";

        std::ostringstream sharded;
        std::ostringstream seek;
        sharded << "{\n  \"manifestVersion\": \"zelph-hf-layout/v2\",\n"
                << "  \"storageMode\": \"multi-object-shards\",\n"
                << "  \"source\": {\"binPath\": \"" << hf_root << "/net.bin\", \"headerLengthBytes\": " << header_length << "},\n"
                << "  \"sections\": {\n";
        seek << "{\n  \"source\": {\"binPath\": \"" << paths.bin.string()
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

        collector.clear();
        interactive.process(".lang de");
        interactive.process(".node beta_de");
        CHECK(any_event_contains(collector, "beta_de")); });

    fs::remove_all(artifact.root);
}

TEST_CASE("sharding: an out-of-range chunk selector is rejected")
{
    const auto artifact = build_artifact(artifact_root("range"));

    const size_t left_chunks = artifact.sections.at("left").size();

    with_fresh_session([&](auto&, auto& interactive)
                       { CHECK_THROWS(interactive.process(".load-partial \"" + artifact.sharded_manifest.string()
                                                          + "\" left=" + std::to_string(left_chunks))); });

    fs::remove_all(artifact.root);
}
