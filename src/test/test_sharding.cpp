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
        fs::path route_index;

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

        collector.clear();
        interactive.process(".lang de");
        interactive.process(".node beta_de");
        CHECK(any_event_contains(collector, "beta_de")); });

    fs::remove_all(artifact.root);
}

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
