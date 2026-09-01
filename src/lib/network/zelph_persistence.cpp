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

#include "zelph.hpp"

// Everything this file does is behind the guard, includes as well: under
// emscripten there is no Cap'n Proto and no file to read, and the translation
// unit is then empty on purpose.
#ifndef __EMSCRIPTEN__
    #include "io/zelph.capnp.h"
    #include "manifest_loader.hpp"
    #include "network.hpp"
    #include "serialization_layout.hpp"
    #include "zelph_impl.hpp"

    #include <ankerl/unordered_dense.h>
    #include <capnp/message.h>
    #include <capnp/serialize-packed.h>
    #include <kj/io.h>

    #include <algorithm>
    #include <atomic>
    #include <cstdint>
    #include <cstdio>
    #include <cstring>
    #include <exception>
    #include <filesystem>
    #include <memory>
    #include <mutex>
    #include <ostream>
    #include <shared_mutex>
    #include <stdexcept>
    #include <string>
    #include <string_view>
    #include <unordered_map>
    #include <utility>
    #include <vector>
#endif

using namespace zelph::network;

#ifndef __EMSCRIPTEN__
std::string Zelph::Impl::pidx_path(const Node predicate) const
{
    return _pidx_base + ".pidx." + std::to_string(predicate);
}
#endif

#ifndef __EMSCRIPTEN__
bool Zelph::Impl::try_load_pidx(const Node predicate, std::vector<IndexPair>& out) const
{
    if (!_pidx_io_enabled.load(std::memory_order_acquire) || _pidx_base.empty())
        return false;

    const std::string path = pidx_path(predicate);
    FILE*             file = fopen(path.c_str(), "rb");
    if (!file) return false;

    bool ok = false;
    try
    {
        detail::PidxHeader h{};
        if (fread(&h, sizeof(h), 1, file) == 1
            && std::memcmp(h.magic, "ZPIX", 4) == 0
            && h.version == 2
            && h.predicate == predicate
            && h.node_count == _pidx_node_count
            && h.last == _pidx_last
            && h.last_var == _pidx_last_var)
        {
            out.resize(h.pair_count);
            ok = h.pair_count == 0
              || fread(out.data(), sizeof(IndexPair), h.pair_count, file) == h.pair_count;
        }
    }
    catch (...)
    {
        ok = false;
    }
    fclose(file);

    if (ok)
    {
        emit(io::OutputChannel::Diagnostic,
             "Loaded adjacency index from " + path + " (" + std::to_string(out.size()) + " edges).");
    }
    else
    {
        out.clear();
    }
    return ok;
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::try_save_pidx(const Node predicate, const std::vector<IndexPair>& pairs) const
{
    if (!_pidx_io_enabled.load(std::memory_order_acquire) || _pidx_base.empty())
        return;

    const std::string path = pidx_path(predicate);
    FILE*             file = fopen(path.c_str(), "wb");
    if (!file)
    {
        emit(io::OutputChannel::Diagnostic, "Could not write adjacency index sidecar: " + path);
        return;
    }

    detail::PidxHeader h{};
    std::memcpy(h.magic, "ZPIX", 4);
    h.version    = 2;
    h.predicate  = predicate;
    h.node_count = _pidx_node_count;
    h.last       = _pidx_last;
    h.last_var   = _pidx_last_var;
    h.pair_count = pairs.size();

    const bool ok = fwrite(&h, sizeof(h), 1, file) == 1
                 && (pairs.empty()
                     || fwrite(pairs.data(), sizeof(IndexPair), pairs.size(), file) == pairs.size());
    fclose(file);

    if (ok)
    {
        emit(io::OutputChannel::Diagnostic,
             "Saved adjacency index to " + path + " (" + std::to_string(pairs.size()) + " edges).");
    }
    else
    {
        emit(io::OutputChannel::Diagnostic, "Failed to write adjacency index sidecar: " + path);
        std::remove(path.c_str());
    }
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::validate_chunk_selector(const detail::chunk_selector& selection, uint32_t chunkCount, const char* label)
{
    for (uint32_t index : selection)
    {
        if (index >= chunkCount)
        {
            throw std::runtime_error(std::string("Requested ")
                                     + label
                                     + " chunk "
                                     + std::to_string(index)
                                     + " but the file only has "
                                     + std::to_string(chunkCount)
                                     + " chunks");
        }
    }
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::clear_loaded_state()
{
    invalidate_predicate_index();

    std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
    std::unique_lock<std::shared_mutex> lock_right(_smtx_right);
    std::unique_lock<std::shared_mutex> lock_weights(_mtx_weights);
    std::unique_lock<std::shared_mutex> lock_node_name(_mtx_node_of_name);
    std::unique_lock<std::shared_mutex> lock_name_node(_mtx_name_of_node);

    _left.clear();
    _right.clear();
    _weights.clear();
    _name_of_node.clear();
    _node_of_name.clear();
    _string_pool.clear();
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadSmallData(const ZelphImpl::Reader& impl)
{
    #ifdef CLEAR_ON_LOAD
    _weights.clear();
    #endif
    for (auto p : impl.getProbabilities())
    {
        _weights[p.getHash()] = static_cast<long double>(p.getProb());
    }
    _last     = impl.getLast();
    _last_var = impl.getLastVar();
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadLeftRightChunks(kj::BufferedInputStreamWrapper& bufferedInput, const ::capnp::ReaderOptions& options, uint32_t leftChunkCount, uint32_t rightChunkCount, const detail::chunk_selector* leftSelection, const detail::chunk_selector* rightSelection)
{
    #ifdef CLEAR_ON_LOAD
    _left.clear();
    #endif
    for (uint32_t chunkIdx = 0; chunkIdx < leftChunkCount; ++chunkIdx)
    {
        ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
        auto                         chunk = chunkMessage.getRoot<AdjChunk>();
        if (chunk.getWhich() != "left" || chunk.getChunkIndex() != chunkIdx)
        {
            throw std::runtime_error("Invalid left chunk order");
        }
        const bool shouldLoad = leftSelection == nullptr || leftSelection->count(chunkIdx) == 1;
        if (shouldLoad)
        {
            for (auto pair : chunk.getPairs())
            {
                adjacency_set adj;
                for (auto n : pair.getAdj())
                {
                    adj.insert(n);
                }
                _left[pair.getNode()] = std::move(adj);
            }
        }
    #ifndef NDEBUG
        io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded left chunk " << chunkIdx + 1 << "/" << leftChunkCount << ", current _left size=" << _left.size();
    #else
        io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
    #endif
    }
    #ifdef NDEBUG
    io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
    #endif

    #ifdef CLEAR_ON_LOAD
    _right.clear();
    #endif
    for (uint32_t chunkIdx = 0; chunkIdx < rightChunkCount; ++chunkIdx)
    {
        ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
        auto                         chunk = chunkMessage.getRoot<AdjChunk>();
        if (chunk.getWhich() != "right" || chunk.getChunkIndex() != chunkIdx)
        {
            throw std::runtime_error("Invalid right chunk order");
        }
        const bool shouldLoad = rightSelection == nullptr || rightSelection->count(chunkIdx) == 1;
        if (shouldLoad)
        {
            for (auto pair : chunk.getPairs())
            {
                adjacency_set adj;
                for (auto n : pair.getAdj())
                {
                    adj.insert(n);
                }
                _right[pair.getNode()] = std::move(adj);
            }
        }
    #ifndef NDEBUG
        io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded right chunk " << chunkIdx + 1 << "/" << rightChunkCount << ", current _right size=" << _right.size();
    #else
        io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
    #endif
    }
    #ifdef NDEBUG
    io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
    #endif
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadNameOfNodeChunks(kj::BufferedInputStreamWrapper& bufferedInput, const ::capnp::ReaderOptions& options, uint32_t nameOfNodeChunkCount, const detail::chunk_selector* selection)
{
    #ifdef CLEAR_ON_LOAD
    _name_of_node.clear();
    _string_pool.clear();
    #endif
    for (uint32_t i = 0; i < nameOfNodeChunkCount; ++i)
    {
        ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
        auto                         chunk      = chunkMessage.getRoot<NameChunk>();
        const bool                   shouldLoad = selection == nullptr || selection->count(i) == 1;
        if (shouldLoad)
        {
            std::string lang = chunk.getLang();
            auto&       map  = _name_of_node[lang];
            for (auto pair : chunk.getPairs())
            {
                try
                {
                    std::string_view sv = _string_pool.intern(pair.getValue());
                    map[pair.getKey()]  = sv;
                }
                catch (...)
                {
                    std::string_view sv = _string_pool.intern("?");
                    map[pair.getKey()]  = sv;
                    io::OutputStream(_output, io::OutputChannel::Error, true) << "Error converting UTF-8 to string for name_of_node key " << pair.getKey();
                }
            }
        }
    #ifndef NDEBUG
        io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded name_of_node chunk " << i + 1 << "/" << nameOfNodeChunkCount;
    #else
        io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
    #endif
    }
    #ifdef NDEBUG
    io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
    #endif
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadNodeOfNameChunks(kj::BufferedInputStreamWrapper& bufferedInput, const ::capnp::ReaderOptions& options, uint32_t nodeOfNameChunkCount, const detail::chunk_selector* selection)
{
    #ifdef CLEAR_ON_LOAD
    _node_of_name.clear();
    #endif
    for (uint32_t i = 0; i < nodeOfNameChunkCount; ++i)
    {
        ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
        auto                         chunk      = chunkMessage.getRoot<NodeNameChunk>();
        const bool                   shouldLoad = selection == nullptr || selection->count(i) == 1;
        if (shouldLoad)
        {
            std::string lang = chunk.getLang();
            auto&       map  = _node_of_name[lang];
            for (auto pair : chunk.getPairs())
            {
                try
                {
                    std::string_view sv = _string_pool.intern(pair.getKey());
                    map[sv]             = pair.getValue();
                }
                catch (...)
                {
                    std::string_view sv = _string_pool.intern("?");
                    map[sv]             = pair.getValue();
                    io::OutputStream(_output, io::OutputChannel::Error, true) << "Error converting UTF-8 to string for node_of_name value " << pair.getValue();
                }
            }
        }
    #ifndef NDEBUG
        io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded node_of_name chunk " << i + 1 << "/" << nodeOfNameChunkCount;
    #else
        io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
    #endif
    }
    #ifdef NDEBUG
    io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
    #endif
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadLeftRightChunkFromPath(const std::string& source_path, uint64_t source_offset, const detail::chunk_selector* selection, const char* which_name, uint32_t section_count)
{
    FILE* file = detail::open_file_or_throw(source_path);
    try
    {
        detail::seek_offset_or_throw(file, source_offset);

        ::capnp::ReaderOptions options;
        options.traversalLimitInWords = 1ULL << 32;
        options.nestingLimit          = 128;

        kj::FdInputStream              raw_input(fileno(file));
        kj::BufferedInputStreamWrapper buffered_input(raw_input);
        ::capnp::PackedMessageReader   chunk_message(buffered_input, options);
        auto                           chunk = chunk_message.getRoot<AdjChunk>();

        if (chunk.getWhich() != which_name)
        {
            throw std::runtime_error("Expected chunk type " + std::string(which_name)
                                     + " but found " + chunk.getWhich().cStr());
        }

        const uint32_t chunk_index = chunk.getChunkIndex();
        const bool     should_load = (selection == nullptr) || selection->count(chunk_index) == 1;

        if (should_load)
        {
            for (auto pair : chunk.getPairs())
            {
                adjacency_set adj;
                for (auto n : pair.getAdj())
                {
                    adj.insert(n);
                }

                if (std::string_view(which_name) == "left")
                {
                    _left[pair.getNode()] = std::move(adj);
                }
                else
                {
                    _right[pair.getNode()] = std::move(adj);
                }
            }
        }

    #ifndef NDEBUG
        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
            << "Loaded " << which_name << " chunk " << chunk_index + 1 << "/" << section_count
            << ", current size=" << (std::string_view(which_name) == "left" ? _left.size() : _right.size());
    #else
        io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
    #endif

        fclose(file);
    }
    catch (...)
    {
        fclose(file);
        throw;
    }
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadNameOfNodeChunkFromPath(const std::string& source_path, uint64_t source_offset, const detail::chunk_selector* selection)
{
    FILE* file = detail::open_file_or_throw(source_path);
    try
    {
        detail::seek_offset_or_throw(file, source_offset);

        ::capnp::ReaderOptions options;
        options.traversalLimitInWords = 1ULL << 32;
        options.nestingLimit          = 128;

        kj::FdInputStream              raw_input(fileno(file));
        kj::BufferedInputStreamWrapper buffered_input(raw_input);
        ::capnp::PackedMessageReader   chunk_message(buffered_input, options);
        auto                           chunk = chunk_message.getRoot<NameChunk>();

        const uint32_t chunk_index = chunk.getChunkIndex();
        const bool     should_load = (selection == nullptr) || selection->count(chunk_index) == 1;

        if (!should_load)
        {
            fclose(file);
            return;
        }

        const std::string lang = chunk.getLang();
        auto&             map  = _name_of_node[lang];
        for (auto pair : chunk.getPairs())
        {
            try
            {
                std::string_view sv = _string_pool.intern(pair.getValue());
                map[pair.getKey()]  = sv;
            }
            catch (...)
            {
                std::string_view sv = _string_pool.intern("?");
                map[pair.getKey()]  = sv;
                io::OutputStream(_output, io::OutputChannel::Error, true)
                    << "Error converting UTF-8 to string for name_of_node key " << pair.getKey();
            }
        }

        fclose(file);
    }
    catch (...)
    {
        fclose(file);
        throw;
    }
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadNodeOfNameChunkFromPath(const std::string& source_path, uint64_t source_offset, const detail::chunk_selector* selection)
{
    FILE* file = detail::open_file_or_throw(source_path);
    try
    {
        detail::seek_offset_or_throw(file, source_offset);

        ::capnp::ReaderOptions options;
        options.traversalLimitInWords = 1ULL << 32;
        options.nestingLimit          = 128;

        kj::FdInputStream              raw_input(fileno(file));
        kj::BufferedInputStreamWrapper buffered_input(raw_input);
        ::capnp::PackedMessageReader   chunk_message(buffered_input, options);
        auto                           chunk = chunk_message.getRoot<NodeNameChunk>();

        const uint32_t chunk_index = chunk.getChunkIndex();
        const bool     should_load = (selection == nullptr) || selection->count(chunk_index) == 1;
        if (!should_load)
        {
            fclose(file);
            return;
        }

        const std::string lang = chunk.getLang();
        auto&             map  = _node_of_name[lang];
        for (auto pair : chunk.getPairs())
        {
            try
            {
                std::string_view sv = _string_pool.intern(pair.getKey());
                map[sv]             = pair.getValue();
            }
            catch (...)
            {
                std::string_view sv = _string_pool.intern("?");
                map[sv]             = pair.getValue();
                io::OutputStream(_output, io::OutputChannel::Error, true)
                    << "Error converting UTF-8 to string for node_of_name value " << pair.getValue();
            }
        }

        fclose(file);
    }
    catch (...)
    {
        fclose(file);
        throw;
    }
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadFromManifest(const std::string& manifest_path, const Zelph::BinChunkSelection& selection, const std::string& shard_root, const std::string& bin_path_hint, const bool skip_payload)
{
    std::string local_manifest_path = manifest_path;
    if (detail::is_hf_uri(manifest_path))
    {
        local_manifest_path = detail::fetch_chunk_to_cache(manifest_path, 0, 0, "manifest").string();
    }

    const detail::ManifestDescription manifest_description = detail::parse_manifest_file(local_manifest_path);
    const std::string                 header_source        = bin_path_hint.empty() ? manifest_description.source_bin_path : bin_path_hint;

    if (header_source.empty())
    {
        throw std::runtime_error("Manifest names no .bin: pass source-bin=<file> or add source.binPath to the manifest");
    }

    const uint32_t leftChunkCount       = static_cast<uint32_t>(manifest_description.left.chunks.size());
    const uint32_t rightChunkCount      = static_cast<uint32_t>(manifest_description.right.chunks.size());
    const uint32_t nameOfNodeChunkCount = static_cast<uint32_t>(manifest_description.name_of_node.chunks.size());
    const uint32_t nodeOfNameChunkCount = static_cast<uint32_t>(manifest_description.node_of_name.chunks.size());

    detail::RouteSelectionResolution routed_selection;
    const bool                       route_requested = selection.route_nodes_explicit || selection.route_name_explicit;
    if (route_requested)
    {
        routed_selection = resolve_route_selection(local_manifest_path, manifest_description, selection, shard_root);
    }

    const bool left_explicit         = selection.left_explicit || route_requested;
    const bool right_explicit        = selection.right_explicit || route_requested;
    const bool name_of_node_explicit = selection.name_of_node_explicit || route_requested;
    const bool node_of_name_explicit = selection.node_of_name_explicit || route_requested;

    auto leftSelection       = detail::normalize_chunk_selector(selection.left, leftChunkCount, left_explicit);
    auto rightSelection      = detail::normalize_chunk_selector(selection.right, rightChunkCount, right_explicit);
    auto nameOfNodeSelection = detail::normalize_chunk_selector(selection.nameOfNode, nameOfNodeChunkCount, name_of_node_explicit);
    auto nodeOfNameSelection = detail::normalize_chunk_selector(selection.nodeOfName, nodeOfNameChunkCount, node_of_name_explicit);

    leftSelection.insert(routed_selection.left.begin(), routed_selection.left.end());
    rightSelection.insert(routed_selection.right.begin(), routed_selection.right.end());
    nameOfNodeSelection.insert(routed_selection.name_of_node.begin(), routed_selection.name_of_node.end());
    nodeOfNameSelection.insert(routed_selection.node_of_name.begin(), routed_selection.node_of_name.end());

    const detail::chunk_selector* leftSelectionPtr       = left_explicit ? &leftSelection : nullptr;
    const detail::chunk_selector* rightSelectionPtr      = right_explicit ? &rightSelection : nullptr;
    const detail::chunk_selector* nameOfNodeSelectionPtr = name_of_node_explicit ? &nameOfNodeSelection : nullptr;
    const detail::chunk_selector* nodeOfNameSelectionPtr = node_of_name_explicit ? &nodeOfNameSelection : nullptr;

    const size_t requestedLeftChunks       = left_explicit ? leftSelection.size() : leftChunkCount;
    const size_t requestedRightChunks      = right_explicit ? rightSelection.size() : rightChunkCount;
    const size_t requestedNameOfNodeChunks = name_of_node_explicit ? nameOfNodeSelection.size() : nameOfNodeChunkCount;
    const size_t requestedNodeOfNameChunks = node_of_name_explicit ? nodeOfNameSelection.size() : nodeOfNameChunkCount;

    validate_chunk_selector(leftSelection, leftChunkCount, "left");
    validate_chunk_selector(rightSelection, rightChunkCount, "right");
    validate_chunk_selector(nameOfNodeSelection,
                            nameOfNodeChunkCount,
                            "nameOfNode");
    validate_chunk_selector(nodeOfNameSelection,
                            nodeOfNameChunkCount,
                            "nodeOfName");

    bool        header_is_remote   = detail::is_hf_uri(header_source);
    std::string header_source_path = header_source;
    if (header_is_remote)
    {
        // Same rule as for the shards: a local copy of the named .bin
        // beats fetching it. Finding it also turns a non-sharded
        // manifest into plain seeks in that file instead of one
        // ranged request per chunk. Costs nothing and fetches
        // nothing, so it belongs before the graph is discarded.
        if (const auto local_bin = detail::resolve_local_source_bin(local_manifest_path, header_source, shard_root);
            !local_bin.empty())
        {
            header_source_path = local_bin.string();
            header_is_remote   = false;
        }
    }

    // Also before the graph is discarded. A manifest naming a .bin
    // that is not there left a network without even its core nodes,
    // and said only "Failed to open file for reading": every
    // following statement then failed with "requested left node 1
    // does not exist" and nothing pointed at .new.
    if (!header_is_remote && !std::filesystem::exists(header_source_path))
    {
        throw std::runtime_error("Manifest names a source .bin that is not there: " + header_source_path);
    }

    clear_loaded_state();

    if (header_is_remote)
    {
        if (manifest_description.source_header_length_bytes == 0)
        {
            throw std::runtime_error("Manifest headerLengthBytes required for remote source-bin loading");
        }
        header_source_path =
            detail::fetch_chunk_to_cache(header_source, 0, manifest_description.source_header_length_bytes, "header").string();
    }

    FILE* file = detail::open_file_or_throw(header_source_path);
    try
    {
        ::capnp::ReaderOptions options;
        options.traversalLimitInWords = 1ULL << 32;
        options.nestingLimit          = 128;

        kj::FdInputStream              raw_input(fileno(file));
        kj::BufferedInputStreamWrapper buffered_input(raw_input);
        ::capnp::PackedMessageReader   main_message(buffered_input, options);
        auto                           impl = main_message.getRoot<ZelphImpl>();
        loadSmallData(impl);
        fclose(file);
    }
    catch (...)
    {
        fclose(file);
        throw;
    }

    io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
        << "Partial loading from manifest: left chunks=" << requestedLeftChunks << "/"
        << leftChunkCount << ", right chunks=" << requestedRightChunks << "/"
        << rightChunkCount << ", nameOfNode chunks=" << requestedNameOfNodeChunks << "/"
        << nameOfNodeChunkCount << ", nodeOfName chunks=" << requestedNodeOfNameChunks << "/"
        << nodeOfNameChunkCount << ", route_requested=" << (route_requested ? "true" : "false")
        << ", skip_payload=" << (skip_payload ? "true" : "false");

    if (skip_payload)
    {
        io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Header-only manifest load complete.";
        return;
    }

    // Where one chunk is read from. A sharded reference is looked up
    // locally FIRST -- next to the manifest, then below shard-root --
    // and only fetched from the remote object when no local copy
    // exists. The published artifact tree contains manifest and
    // shards together, so the previous "no shard-root means remote"
    // rule downloaded files that were already on disk: 232 MB over
    // the network for one chunk of the pruned network, 50x slower
    // than reading it, with nothing in the output saying so.
    auto chunk_source_path = [&](const detail::ManifestChunkRef& ref,
                                 const bool                      is_sharded_ref,
                                 const uint64_t                  source_offset,
                                 const std::string&              cache_label) -> std::string
    {
        if (is_sharded_ref)
        {
            try
            {
                return detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
            }
            catch (const std::exception&)
            {
                // A path that is not remote cannot be recovered from.
                if (!detail::is_hf_uri(ref.object_path)) throw;
            }

            io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                << "Shard " << cache_label << " has no local copy; fetching " << ref.object_path;

            return detail::fetch_chunk_to_cache(ref.object_path, 0, ref.length, cache_label).string();
        }

        if (header_is_remote)
        {
            return detail::fetch_chunk_to_cache(header_source, source_offset, ref.length, cache_label).string();
        }

        return header_source_path;
    };

    if (leftSelectionPtr == nullptr || !leftSelection.empty())
    {
        for (const auto& ref : manifest_description.left.chunks)
        {
            if (leftSelectionPtr != nullptr && leftSelection.count(ref.chunk_index) != 1) continue;

            const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
            const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
            const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);

            const std::string source_file =
                chunk_source_path(ref, is_sharded_ref, source_offset, "left-" + std::to_string(ref.chunk_index));

            try
            {
                loadLeftRightChunkFromPath(source_file, read_chunk_start, leftSelectionPtr, "left", manifest_description.left.chunks.size());
            }
            catch (const std::exception& ex)
            {
                if (!ref.has_source_offset)
                {
                    throw;
                }
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Shard chunk left/" << ref.chunk_index << " failed (" << ex.what()
                    << "); falling back to sequential bin load";
                loadFromFile(header_source_path, selection, skip_payload);
                return;
            }
        }
    }

    if (rightSelectionPtr == nullptr || !rightSelection.empty())
    {
        for (const auto& ref : manifest_description.right.chunks)
        {
            if (rightSelectionPtr != nullptr && rightSelection.count(ref.chunk_index) != 1) continue;

            const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
            const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
            const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);

            const std::string source_file =
                chunk_source_path(ref, is_sharded_ref, source_offset, "right-" + std::to_string(ref.chunk_index));

            try
            {
                loadLeftRightChunkFromPath(source_file,
                                           read_chunk_start,
                                           rightSelectionPtr,
                                           "right",
                                           manifest_description.right.chunks.size());
            }
            catch (const std::exception& ex)
            {
                if (!ref.has_source_offset)
                {
                    throw;
                }
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Shard chunk right/" << ref.chunk_index << " failed (" << ex.what()
                    << "); falling back to sequential bin load";
                loadFromFile(header_source_path, selection, skip_payload);
                return;
            }
        }
    }

    if (nameOfNodeSelectionPtr == nullptr || !nameOfNodeSelection.empty())
    {
        for (const auto& ref : manifest_description.name_of_node.chunks)
        {
            if (nameOfNodeSelectionPtr != nullptr && nameOfNodeSelection.count(ref.chunk_index) != 1) continue;

            const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
            const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
            const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);
            const std::string source_file =
                chunk_source_path(ref, is_sharded_ref, source_offset, "nameOfNode-" + std::to_string(ref.chunk_index));

            try
            {
                loadNameOfNodeChunkFromPath(source_file, read_chunk_start, nameOfNodeSelectionPtr);
            }
            catch (const std::exception& ex)
            {
                if (!ref.has_source_offset)
                {
                    throw;
                }
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Shard chunk nameOfNode/" << ref.chunk_index << " failed (" << ex.what()
                    << "); falling back to sequential bin load";
                loadFromFile(header_source_path, selection, skip_payload);
                return;
            }
        }
    }

    if (nodeOfNameSelectionPtr == nullptr || !nodeOfNameSelection.empty())
    {
        for (const auto& ref : manifest_description.node_of_name.chunks)
        {
            if (nodeOfNameSelectionPtr != nullptr && nodeOfNameSelection.count(ref.chunk_index) != 1) continue;

            const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
            const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
            const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);
            const std::string source_file =
                chunk_source_path(ref, is_sharded_ref, source_offset, "nodeOfName-" + std::to_string(ref.chunk_index));

            try
            {
                loadNodeOfNameChunkFromPath(source_file, read_chunk_start, nodeOfNameSelectionPtr);
            }
            catch (const std::exception& ex)
            {
                if (!ref.has_source_offset)
                {
                    throw;
                }
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Shard chunk nodeOfName/" << ref.chunk_index << " failed (" << ex.what()
                    << "); falling back to sequential bin load";
                loadFromFile(header_source_path, selection, skip_payload);
                return;
            }
        }
    }

    io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "String pool size after partial load: " << _string_pool.size();

    // The route index is a sidecar written by the emitter, and the
    // loader trusts it: it says which chunks hold a node, and those
    // chunks are what gets read. An index that points somewhere else
    // therefore produced a perfectly ordinary load of the wrong
    // pieces -- the requested node simply was not in the result, with
    // nothing said. Checking afterwards costs one lookup per
    // requested node and turns that into a sentence.
    if (route_requested && !skip_payload)
    {
        const auto has_a_name = [this](const Node nd)
        {
            for (const auto& lang : _name_of_node)
            {
                if (lang.second.find(nd) != lang.second.end()) return true;
            }
            return false;
        };

        for (const Node nd : selection.route_nodes)
        {
            if (!exists(nd))
            {
                io::OutputStream(_output, io::OutputChannel::Error, true)
                    << "route-node=" << nd
                    << ": the chunks the route index named do not contain that node"
                    << " -- the index and the shards disagree";
            }
            else if (!routed_selection.name_of_node.empty() && !has_a_name(nd))
            {
                // The index routed a nameOfNode chunk for this node,
                // so it claims to know where the name is.
                io::OutputStream(_output, io::OutputChannel::Error, true)
                    << "route-node=" << nd
                    << ": the nameOfNode chunk the route index named carries no name for it"
                    << " -- the index and the shards disagree";
            }
        }

        if (selection.route_name_explicit && !selection.route_name.empty())
        {
            const auto lang_it = _node_of_name.find(selection.route_lang);
            if (lang_it == _node_of_name.end()
                || lang_it->second.find(selection.route_name) == lang_it->second.end())
            {
                io::OutputStream(_output, io::OutputChannel::Error, true)
                    << "route-name=" << selection.route_name
                    << ": the chunk the route index named does not contain that name in '"
                    << selection.route_lang << "' -- the index and the shards disagree";
            }
        }
    }
}
#endif

#ifndef __EMSCRIPTEN__
// Write the network, or the part of it that `keep` selects.
//
// A filtered save produces a smaller network that is complete in
// itself: the caller is responsible for handing in a node set that is
// structurally closed (see Zelph::save_predicate_slice), this function
// only drops everything else -- including the edges POINTING at
// dropped nodes, which is what keeps the result loadable.
void Zelph::Impl::saveToFile(const std::string& filename, const ankerl::unordered_dense::set<Node>* const keep) const
{
    const size_t chunkSize = serialization::chunk_entries;

    // See serialization_layout.hpp for why this is sized from the data
    // rather than fixed.
    const auto firstSegmentWords = [](const size_t entries) -> ::capnp::uint
    { return static_cast<::capnp::uint>(serialization::first_segment_words(entries)); };

    const auto kept = [keep](const Node nd)
    { return keep == nullptr || keep->find(nd) != keep->end(); };

    // Number of entries a section contributes under the filter.
    const auto count_kept = [&kept](const auto& map, const size_t unfiltered, const bool filtering)
    {
        if (!filtering) return unfiltered;
        size_t n = 0;
        for (const auto& entry : map)
        {
            if (kept(entry.first)) ++n;
        }
        return n;
    };

    // Adjacency of one node, without the neighbours that were dropped.
    const auto kept_neighbours = [&kept](const adjacency_set& adj)
    {
        std::vector<Node> sorted;
        sorted.reserve(adj.size());
        for (const Node nd : adj)
        {
            if (kept(nd)) sorted.push_back(nd);
        }
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    };

    io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Saving: probabilities size=" << _weights.size() << ", left size=" << _left.size() << ", right size=" << _right.size();
    io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Saving: name_of_node outer size=" << _name_of_node.size() << ", node_of_name outer size=" << _node_of_name.size();
    io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Saving: string pool size=" << _string_pool.size();

    #ifdef _WIN32
        #define fileno _fileno
    #endif
    FILE* file = fopen(filename.c_str(), "wb");
    if (!file)
    {
        throw std::runtime_error("Failed to open file for writing: " + filename);
    }
    auto               fileGuard = std::unique_ptr<FILE, decltype(&fclose)>(file, &fclose);
    kj::FdOutputStream output(fileno(file));

    // Main message (small data)
    ::capnp::MallocMessageBuilder mainMessage(firstSegmentWords(_weights.size()));
    auto                          impl = mainMessage.initRoot<ZelphImpl>();

    // Serialize probabilities
    auto   probs = impl.initProbabilities(_weights.size());
    size_t idx   = 0;
    for (const auto& p : _weights)
    {
        probs[idx].setHash(p.first);
        probs[idx].setProb(static_cast<double>(p.second));
        ++idx;
    }
    impl.setLast(_last);
    impl.setLastVar(_last_var);

    const bool filtering = keep != nullptr;

    // Per-language entry counts under the filter; needed twice (chunk
    // counts in the header, chunk sizes below), so computed once.
    std::unordered_map<std::string, size_t> nameOfNodeKept;
    std::unordered_map<std::string, size_t> nodeOfNameKept;

    size_t nameOfNodeChunkTotal = 0;
    for (const auto& langMap : _name_of_node)
    {
        size_t mapSize                = count_kept(langMap.second, langMap.second.size(), filtering);
        nameOfNodeKept[langMap.first] = mapSize;
        nameOfNodeChunkTotal += (mapSize + chunkSize - 1) / chunkSize;
    }
    impl.setNameOfNodeChunkCount(static_cast<uint32_t>(nameOfNodeChunkTotal));

    size_t nodeOfNameChunkTotal = 0;
    for (const auto& langMap : _node_of_name)
    {
        // node_of_name is keyed by NAME; the node is the value, so the
        // filter has to look at the other side of the pair.
        size_t mapSize = langMap.second.size();
        if (filtering)
        {
            mapSize = 0;
            for (const auto& entry : langMap.second)
            {
                if (kept(entry.second)) ++mapSize;
            }
        }
        nodeOfNameKept[langMap.first] = mapSize;
        nodeOfNameChunkTotal += (mapSize + chunkSize - 1) / chunkSize;
    }
    impl.setNodeOfNameChunkCount(static_cast<uint32_t>(nodeOfNameChunkTotal));

    const size_t leftKept  = count_kept(_left, _left.size(), filtering);
    const size_t rightKept = count_kept(_right, _right.size(), filtering);

    size_t leftChunkCount  = (leftKept + chunkSize - 1) / chunkSize;
    size_t rightChunkCount = (rightKept + chunkSize - 1) / chunkSize;
    impl.setLeftChunkCount(static_cast<uint32_t>(leftChunkCount));
    impl.setRightChunkCount(static_cast<uint32_t>(rightChunkCount));

    ::capnp::writePackedMessage(output, mainMessage);

    // Chunk _left
    auto leftIt = _left.begin();
    for (size_t chunkIdx = 0; chunkIdx < leftChunkCount; ++chunkIdx)
    {
        const size_t thisChunkSize = std::min(chunkSize, leftKept - chunkIdx * chunkSize);

        ::capnp::MallocMessageBuilder chunkMessage(firstSegmentWords(thisChunkSize));
        auto                          chunk = chunkMessage.initRoot<AdjChunk>();
        chunk.setWhich("left");
        chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

        auto   pairList = chunk.initPairs(thisChunkSize);
        size_t pIdx     = 0;
        for (size_t i = 0; i < thisChunkSize; ++i, ++leftIt)
        {
            while (leftIt != _left.end() && !kept(leftIt->first))
                ++leftIt;

            pairList[pIdx].setNode(leftIt->first);
            const std::vector<Node> sorted = kept_neighbours(leftIt->second);
            auto                    adj    = pairList[pIdx].initAdj(sorted.size());
            for (size_t j = 0; j < sorted.size(); ++j)
            {
                adj.set(j, sorted[j]);
            }
            ++pIdx;
        }
        ::capnp::writePackedMessage(output, chunkMessage);
    }

    // Chunk _right
    auto rightIt = _right.begin();
    for (size_t chunkIdx = 0; chunkIdx < rightChunkCount; ++chunkIdx)
    {
        const size_t thisChunkSize = std::min(chunkSize, rightKept - chunkIdx * chunkSize);

        ::capnp::MallocMessageBuilder chunkMessage(firstSegmentWords(thisChunkSize));
        auto                          chunk = chunkMessage.initRoot<AdjChunk>();
        chunk.setWhich("right");
        chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

        auto   pairList = chunk.initPairs(thisChunkSize);
        size_t pIdx     = 0;
        for (size_t i = 0; i < thisChunkSize; ++i, ++rightIt)
        {
            while (rightIt != _right.end() && !kept(rightIt->first))
                ++rightIt;

            pairList[pIdx].setNode(rightIt->first);
            const std::vector<Node> sorted = kept_neighbours(rightIt->second);
            auto                    adj    = pairList[pIdx].initAdj(sorted.size());
            for (size_t j = 0; j < sorted.size(); ++j)
            {
                adj.set(j, sorted[j]);
            }
            ++pIdx;
        }
        ::capnp::writePackedMessage(output, chunkMessage);
    }

    // Chunk _name_of_node
    // Note: string_view::data() is null-terminated here because the pool
    // stores std::string objects and string_view points into them.
    //
    // chunkIndex is assigned section-globally (it keeps counting across
    // language boundaries) so that it is unique within the section and
    // equals the chunk's sequential position in the stream. This keeps
    // the stream-position-based loader (loadNameOfNodeChunks, selects by
    // `i`) and the chunkIndex-based loader (loadNameOfNodeChunkFromPath,
    // selects by getChunkIndex()) in agreement. A per-language restart
    // would make several chunks share chunkIndex 0, 1, ...
    uint32_t nameOfNodeChunkIndex = 0;
    for (const auto& langMap : _name_of_node)
    {
        std::string                                    lang = langMap.first;
        const auto&                                    map  = langMap.second;
        std::vector<std::pair<Node, std::string_view>> sorted;
        sorted.reserve(nameOfNodeKept[lang]);
        for (const auto& entry : map)
        {
            if (kept(entry.first)) sorted.emplace_back(entry.first, entry.second);
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                  { return a.first < b.first; });

        auto   it        = sorted.begin();
        size_t numChunks = (sorted.size() + chunkSize - 1) / chunkSize;
        for (size_t chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx)
        {
            const size_t thisSize = std::min(chunkSize, sorted.size() - chunkIdx * chunkSize);

            ::capnp::MallocMessageBuilder chunkMessage(firstSegmentWords(thisSize));
            auto                          chunk = chunkMessage.initRoot<NameChunk>();
            chunk.setLang(lang);
            chunk.setChunkIndex(nameOfNodeChunkIndex++);

            auto pairs = chunk.initPairs(thisSize);
            for (size_t i = 0; i < thisSize; ++i, ++it)
            {
                pairs[i].setKey(it->first);
                // Pool-backed string_view: data() is null-terminated
                pairs[i].setValue(it->second.data());
            }
            ::capnp::writePackedMessage(output, chunkMessage);
        }
    }

    // Chunk _node_of_name
    // chunkIndex is assigned section-globally; see _name_of_node above.
    uint32_t nodeOfNameChunkIndex = 0;
    for (const auto& langMap : _node_of_name)
    {
        std::string                                    lang = langMap.first;
        const auto&                                    map  = langMap.second;
        std::vector<std::pair<std::string_view, Node>> sorted;
        sorted.reserve(nodeOfNameKept[lang]);
        for (const auto& entry : map)
        {
            if (kept(entry.second)) sorted.emplace_back(entry.first, entry.second);
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                  { return a.first < b.first; });

        auto   it        = sorted.begin();
        size_t numChunks = (sorted.size() + chunkSize - 1) / chunkSize;
        for (size_t chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx)
        {
            const size_t thisSize = std::min(chunkSize, sorted.size() - chunkIdx * chunkSize);

            ::capnp::MallocMessageBuilder chunkMessage(firstSegmentWords(thisSize));
            auto                          chunk = chunkMessage.initRoot<NodeNameChunk>();
            chunk.setLang(lang);
            chunk.setChunkIndex(nodeOfNameChunkIndex++);

            auto pairs = chunk.initPairs(thisSize);
            for (size_t i = 0; i < thisSize; ++i, ++it)
            {
                // Pool-backed string_view: data() is null-terminated
                pairs[i].setKey(it->first.data());
                pairs[i].setValue(it->second);
            }
            ::capnp::writePackedMessage(output, chunkMessage);
        }
    }
}
#endif

#ifndef __EMSCRIPTEN__
// kj reports through kj::Exception, whose what() carries a C++ source
// location and a hex stack trace. Neither belongs in front of someone
// who typed a file name; what does belong there is whether the graph
// they had is still the graph they have.
std::string Zelph::Impl::read_error_text(const std::string& filename, const kj::Exception& e, const bool state_discarded)
{
    std::string text = "Failed to read '" + filename
                     + "' as a zelph .bin file (truncated, or not a .bin at all): "
                     + std::string(e.getDescription().cStr());
    if (state_discarded)
    {
        text += ". The previous network was already discarded -- use .new to start over";
    }
    return text;
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadFromFile(const std::string& filename)
{
    #ifdef _WIN32
        #define fileno _fileno
    #endif
    FILE* file = fopen(filename.c_str(), "rb");
    if (!file)
    {
        throw std::runtime_error("Failed to open file for reading: " + filename);
    }

    try
    {
        ::capnp::ReaderOptions options;
        options.traversalLimitInWords = 1ULL << 32;
        options.nestingLimit          = 128;

        kj::FdInputStream              rawInput(fileno(file));
        kj::BufferedInputStreamWrapper bufferedInput(rawInput);

        ::capnp::PackedMessageReader mainMessage(bufferedInput, options);
        auto                         impl = mainMessage.getRoot<ZelphImpl>();

        loadSmallData(impl);

        uint32_t leftChunkCount       = impl.getLeftChunkCount();
        uint32_t rightChunkCount      = impl.getRightChunkCount();
        uint32_t nameOfNodeChunkCount = impl.getNameOfNodeChunkCount();
        uint32_t nodeOfNameChunkCount = impl.getNodeOfNameChunkCount();
        io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loading: left chunks=" << leftChunkCount << ", right chunks=" << rightChunkCount
                                                                       << ", nameOfNode chunks=" << nameOfNodeChunkCount << ", nodeOfName chunks=" << nodeOfNameChunkCount;

        loadLeftRightChunks(bufferedInput, options, leftChunkCount, rightChunkCount);
        loadNameOfNodeChunks(bufferedInput, options, nameOfNodeChunkCount);
        loadNodeOfNameChunks(bufferedInput, options, nodeOfNameChunkCount);

        io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "String pool size after load: " << _string_pool.size();

        fclose(file);

        // Enable predicate-index sidecar I/O for this file and take the
        // validation snapshot of the freshly loaded, unmodified graph.
        _pidx_base       = filename;
        _pidx_node_count = _left.size();
        _pidx_last       = _last;
        _pidx_last_var   = _last_var;
        _pidx_io_enabled.store(true, std::memory_order_release);
    }
    catch (const kj::Exception& e)
    {
        // A load merges into whatever is already there, so a failure
        // partway leaves a graph that is neither the old one nor the
        // file. Say so; the handle was leaked on every throw before.
        fclose(file);
        throw std::runtime_error(read_error_text(filename, e, true));
    }
    catch (...)
    {
        fclose(file);
        throw;
    }
}
#endif

#ifndef __EMSCRIPTEN__
void Zelph::Impl::loadFromFile(const std::string& filename, const Zelph::BinChunkSelection& selection, const bool skip_payload)
{
    #ifdef _WIN32
        #define fileno _fileno
    #endif
    FILE* file = fopen(filename.c_str(), "rb");
    if (!file)
    {
        throw std::runtime_error("Failed to open file for reading: " + filename);
    }

    bool state_cleared = false;

    try
    {
        ::capnp::ReaderOptions options;
        options.traversalLimitInWords = 1ULL << 32;
        options.nestingLimit          = 128;

        kj::FdInputStream              rawInput(fileno(file));
        kj::BufferedInputStreamWrapper bufferedInput(rawInput);

        ::capnp::PackedMessageReader mainMessage(bufferedInput, options);
        auto                         impl = mainMessage.getRoot<ZelphImpl>();

        uint32_t leftChunkCount       = impl.getLeftChunkCount();
        uint32_t rightChunkCount      = impl.getRightChunkCount();
        uint32_t nameOfNodeChunkCount = impl.getNameOfNodeChunkCount();
        uint32_t nodeOfNameChunkCount = impl.getNodeOfNameChunkCount();
        auto     leftSelector         = detail::normalize_chunk_selector(selection.left, leftChunkCount, selection.left_explicit);
        auto     rightSelector        = detail::normalize_chunk_selector(selection.right, rightChunkCount, selection.right_explicit);
        auto     nameOfNodeSelector   = detail::normalize_chunk_selector(selection.nameOfNode, nameOfNodeChunkCount, selection.name_of_node_explicit);
        auto     nodeOfNameSelector   = detail::normalize_chunk_selector(selection.nodeOfName, nodeOfNameChunkCount, selection.node_of_name_explicit);

        const detail::chunk_selector* leftSelectorPtr       = selection.left_explicit ? &leftSelector : nullptr;
        const detail::chunk_selector* rightSelectorPtr      = selection.right_explicit ? &rightSelector : nullptr;
        const detail::chunk_selector* nameOfNodeSelectorPtr = selection.name_of_node_explicit ? &nameOfNodeSelector : nullptr;
        const detail::chunk_selector* nodeOfNameSelectorPtr = selection.node_of_name_explicit ? &nodeOfNameSelector : nullptr;

        const size_t requestedLeftChunks       = selection.left_explicit ? leftSelector.size() : leftChunkCount;
        const size_t requestedRightChunks      = selection.right_explicit ? rightSelector.size() : rightChunkCount;
        const size_t requestedNameOfNodeChunks = selection.name_of_node_explicit ? nameOfNodeSelector.size() : nameOfNodeChunkCount;
        const size_t requestedNodeOfNameChunks = selection.node_of_name_explicit ? nodeOfNameSelector.size() : nodeOfNameChunkCount;

        // BEFORE the graph is touched. A selector naming a chunk the
        // file does not have is the most ordinary way to get this
        // command wrong -- a typo -- and it used to arrive after
        // clear_loaded_state(), which leaves a network without even
        // its core nodes: every following statement then failed with
        // "requested left node 1 does not exist" and only .new got
        // the session back.
        validate_chunk_selector(leftSelector, leftChunkCount, "left");
        validate_chunk_selector(rightSelector, rightChunkCount, "right");
        validate_chunk_selector(nameOfNodeSelector, nameOfNodeChunkCount, "nameOfNode");
        validate_chunk_selector(nodeOfNameSelector, nodeOfNameChunkCount, "nodeOfName");

        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
            << "Partial loading: left chunks=" << requestedLeftChunks << "/" << leftChunkCount
            << ", right chunks=" << requestedRightChunks << "/" << rightChunkCount
            << ", nameOfNode chunks=" << requestedNameOfNodeChunks << "/"
            << nameOfNodeChunkCount
            << ", nodeOfName chunks=" << requestedNodeOfNameChunks << "/"
            << nodeOfNameChunkCount
            << ", skip_payload=" << (skip_payload ? "true" : "false");

        clear_loaded_state();
        state_cleared = true;
        loadSmallData(impl);

        if (skip_payload)
        {
            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Header-only file load complete.";
            fclose(file);
            return;
        }

        // What each section is asked for. A selector that is present
        // but empty is `=none`: the section is wanted for nothing.
        const bool want_left_right   = leftSelectorPtr == nullptr || !leftSelector.empty()
                                    || rightSelectorPtr == nullptr || !rightSelector.empty();
        const bool want_name_of_node = nameOfNodeSelectorPtr == nullptr || !nameOfNodeSelector.empty();
        const bool want_node_of_name = nodeOfNameSelectorPtr == nullptr || !nodeOfNameSelector.empty();

        // THIS STREAM IS SEQUENTIAL. A section that is not read is not
        // consumed either, so everything after it would be parsed from
        // the wrong offset -- and capnp does not necessarily complain:
        // `left=none right=none` made the name section read adjacency
        // chunks, which surfaced as "Error converting UTF-8 to string
        // for name_of_node key 1" for every core node and left the
        // network without names. A section may therefore be skipped
        // only when nothing AFTER it is wanted; the loaders themselves
        // already consume a chunk they do not keep, which is what makes
        // a partial selection inside a section work.
        //
        // The manifest path is unaffected: it seeks to each chunk's
        // offset instead of streaming past the others.
        if (want_left_right || want_name_of_node || want_node_of_name)
        {
            loadLeftRightChunks(bufferedInput,
                                options,
                                leftChunkCount,
                                rightChunkCount,
                                leftSelectorPtr,
                                rightSelectorPtr);
        }
        if (want_name_of_node || want_node_of_name)
        {
            loadNameOfNodeChunks(bufferedInput, options, nameOfNodeChunkCount, nameOfNodeSelectorPtr);
        }
        if (want_node_of_name)
        {
            loadNodeOfNameChunks(bufferedInput, options, nodeOfNameChunkCount, nodeOfNameSelectorPtr);
        }

        io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "String pool size after partial load: " << _string_pool.size();

        fclose(file);
    }
    catch (const kj::Exception& e)
    {
        fclose(file);
        throw std::runtime_error(read_error_text(filename, e, state_cleared));
    }
    catch (...)
    {
        fclose(file);
        throw;
    }
}
#endif
