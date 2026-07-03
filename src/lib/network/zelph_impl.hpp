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

#include "manifest_loader.hpp"
#include "network.hpp"
#include "zelph.hpp"

#include "io/zelph.capnp.h"

#include <ankerl/unordered_dense.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <kj/io.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zelph::network
{
    // Append-only string pool providing pointer-stable storage.
    // All name strings are interned here exactly once; both name maps
    // store std::string_view pointing into this pool.
    //
    // Thread safety: callers must hold at least one of the name mutexes
    // (_mtx_name_of_node or _mtx_node_of_name) before calling intern().
    // In practice every code path that interns already holds both.
    class StringPool
    {
        // std::unordered_set is node-based => pointers/references to
        // elements are never invalidated by insert or rehash.
        std::unordered_set<std::string> _pool;

    public:
        // Returns a view whose lifetime equals the pool's lifetime
        // (or until clear() is called).
        std::string_view intern(const std::string& s)
        {
            auto [it, _] = _pool.insert(s);
            return std::string_view(*it);
        }

        std::string_view intern(std::string&& s)
        {
            auto [it, _] = _pool.insert(std::move(s));
            return std::string_view(*it);
        }

        void   clear() { _pool.clear(); }
        size_t size() const { return _pool.size(); }
    };

    // Per-predicate adjacency index for transitive closures.
    //
    // The generic traversal (get_fact_objects / get_fact_subjects) scans
    // every relation node touching a visited node, regardless of predicate.
    // At hub nodes (popular Wikidata classes) this means scanning millions
    // of unrelated relations (e.g. all P31 instance facts of a class) just
    // to find its few P279 edges. The index is built in one pass over the
    // relations of a single predicate (_right[predicate]) and maps
    // subject -> objects and object -> subjects for exactly that predicate;
    // a BFS over the index touches only true edges.
    struct PredicateIndex
    {
        using adjacency = ankerl::unordered_dense::map<Node, std::vector<Node>>;

        adjacency forward;  // subject -> objects
        adjacency backward; // object  -> subjects
    };

    class ZELPH_EXPORT Zelph::Impl : public Network
    {
        friend class Zelph;

        explicit Impl(const io::OutputHandler& output)
            : _output(output)
        {
        }

        std::string pidx_path(const Node predicate) const
        {
            return _pidx_base + ".pidx." + std::to_string(predicate);
        }

        bool try_load_pidx(const Node predicate, std::vector<detail::IndexPair>& out) const
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
                    && h.version == 1
                    && h.predicate == predicate
                    && h.node_count == _pidx_node_count
                    && h.last == _pidx_last
                    && h.last_var == _pidx_last_var)
                {
                    out.resize(h.pair_count);
                    ok = h.pair_count == 0
                      || fread(out.data(), sizeof(detail::IndexPair), h.pair_count, file) == h.pair_count;
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

        void try_save_pidx(const Node predicate, const std::vector<detail::IndexPair>& pairs) const
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
            h.version    = 1;
            h.predicate  = predicate;
            h.node_count = _pidx_node_count;
            h.last       = _pidx_last;
            h.last_var   = _pidx_last_var;
            h.pair_count = pairs.size();

            const bool ok = fwrite(&h, sizeof(h), 1, file) == 1
                         && (pairs.empty()
                             || fwrite(pairs.data(), sizeof(detail::IndexPair), pairs.size(), file) == pairs.size());
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

        static void validate_chunk_selector(const detail::chunk_selector& selection, uint32_t chunkCount, const char* label)
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

        void clear_loaded_state()
        {
            invalidate_predicate_index();

            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);
            std::unique_lock<std::shared_mutex> lock_prob(_mtx_prob);
            std::unique_lock<std::shared_mutex> lock_node_name(_mtx_node_of_name);
            std::unique_lock<std::shared_mutex> lock_name_node(_mtx_name_of_node);

            _left.clear();
            _right.clear();
            _probabilities.clear();
            _name_of_node.clear();
            _node_of_name.clear();
            _string_pool.clear();
        }

        void loadSmallData(const ZelphImpl::Reader& impl)
        {
#ifdef CLEAR_ON_LOAD
            _probabilities.clear();
#endif
            for (auto p : impl.getProbabilities())
            {
                _probabilities[p.getHash()] = static_cast<long double>(p.getProb());
            }
            _last     = impl.getLast();
            _last_var = impl.getLastVar();
        }

        void loadLeftRightChunks(kj::BufferedInputStreamWrapper& bufferedInput,
                                 const ::capnp::ReaderOptions&   options,
                                 uint32_t                        leftChunkCount,
                                 uint32_t                        rightChunkCount,
                                 const detail::chunk_selector*   leftSelection  = nullptr,
                                 const detail::chunk_selector*   rightSelection = nullptr)
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

        void loadNameOfNodeChunks(kj::BufferedInputStreamWrapper& bufferedInput,
                                  const ::capnp::ReaderOptions&   options,
                                  uint32_t                        nameOfNodeChunkCount,
                                  const detail::chunk_selector*   selection = nullptr)
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

        void loadNodeOfNameChunks(kj::BufferedInputStreamWrapper& bufferedInput,
                                  const ::capnp::ReaderOptions&   options,
                                  uint32_t                        nodeOfNameChunkCount,
                                  const detail::chunk_selector*   selection = nullptr)
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

        void loadLeftRightChunkFromPath(const std::string&            source_path,
                                        uint64_t                      source_offset,
                                        const detail::chunk_selector* selection,
                                        const char*                   which_name,
                                        uint32_t                      section_count)
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

        void loadNameOfNodeChunkFromPath(const std::string&            source_path,
                                         uint64_t                      source_offset,
                                         const detail::chunk_selector* selection)
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

        void loadNodeOfNameChunkFromPath(const std::string&            source_path,
                                         uint64_t                      source_offset,
                                         const detail::chunk_selector* selection)
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

        void loadFromManifest(const std::string&              manifest_path,
                              const Zelph::BinChunkSelection& selection,
                              const std::string&              shard_root,
                              const std::string&              bin_path_hint,
                              const bool                      skip_payload)
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
                throw std::runtime_error("Manifest requires --source-bin (or source.binPath in manifest)");
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

            clear_loaded_state();

            const bool  header_is_remote   = detail::is_hf_uri(header_source);
            std::string header_source_path = header_source;
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

            if (leftSelectionPtr == nullptr || !leftSelection.empty())
            {
                for (const auto& ref : manifest_description.left.chunks)
                {
                    if (leftSelectionPtr != nullptr && leftSelection.count(ref.chunk_index) != 1) continue;

                    const bool     is_sharded_ref   = (manifest_description.is_v2 || manifest_description.is_v3) && !ref.object_path.empty();
                    const uint64_t source_offset    = is_sharded_ref ? 0 : (ref.has_source_offset ? ref.source_offset : 0);
                    const uint64_t read_chunk_start = is_sharded_ref ? 0 : (header_is_remote ? 0 : source_offset);
                    const uint64_t source_length    = ref.length;
                    std::string    source_file;

                    if (is_sharded_ref)
                    {
                        if (detail::is_hf_uri(ref.object_path))
                        {
                            try
                            {
                                if (!shard_root.empty())
                                {
                                    source_file = detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                                }
                                else
                                {
                                    source_file =
                                        detail::fetch_chunk_to_cache(ref.object_path, 0, source_length, "left-" + std::to_string(ref.chunk_index)).string();
                                }
                            }
                            catch (...)
                            {
                                source_file =
                                    detail::fetch_chunk_to_cache(ref.object_path, 0, source_length, "left-" + std::to_string(ref.chunk_index)).string();
                            }
                        }
                        else
                        {
                            source_file = detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                        }
                    }
                    else if (header_is_remote)
                    {
                        source_file =
                            detail::fetch_chunk_to_cache(header_source, source_offset, source_length, "left-" + std::to_string(ref.chunk_index))
                                .string();
                    }
                    else
                    {
                        source_file = header_source_path;
                    }

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
                    const uint64_t source_length    = ref.length;
                    std::string    source_file;

                    if (is_sharded_ref)
                    {
                        if (detail::is_hf_uri(ref.object_path))
                        {
                            try
                            {
                                if (!shard_root.empty())
                                {
                                    source_file = detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                                }
                                else
                                {
                                    source_file = detail::fetch_chunk_to_cache(ref.object_path,
                                                                               0,
                                                                               source_length,
                                                                               "right-" + std::to_string(ref.chunk_index))
                                                      .string();
                                }
                            }
                            catch (...)
                            {
                                source_file = detail::fetch_chunk_to_cache(ref.object_path,
                                                                           0,
                                                                           source_length,
                                                                           "right-" + std::to_string(ref.chunk_index))
                                                  .string();
                            }
                        }
                        else
                        {
                            source_file = detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                        }
                    }
                    else if (header_is_remote)
                    {
                        source_file =
                            detail::fetch_chunk_to_cache(header_source, source_offset, source_length, "right-" + std::to_string(ref.chunk_index))
                                .string();
                    }
                    else
                    {
                        source_file = header_source_path;
                    }

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
                    const uint64_t source_length    = ref.length;
                    std::string    source_file;

                    if (is_sharded_ref)
                    {
                        if (detail::is_hf_uri(ref.object_path))
                        {
                            try
                            {
                                if (!shard_root.empty())
                                {
                                    source_file = detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                                }
                                else
                                {
                                    source_file = detail::fetch_chunk_to_cache(ref.object_path,
                                                                               0,
                                                                               source_length,
                                                                               "nameOfNode-" + std::to_string(ref.chunk_index))
                                                      .string();
                                }
                            }
                            catch (...)
                            {
                                source_file =
                                    detail::fetch_chunk_to_cache(ref.object_path,
                                                                 0,
                                                                 source_length,
                                                                 "nameOfNode-" + std::to_string(ref.chunk_index))
                                        .string();
                            }
                        }
                        else
                        {
                            source_file = detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                        }
                    }
                    else if (header_is_remote)
                    {
                        source_file =
                            detail::fetch_chunk_to_cache(header_source, source_offset, source_length, "nameOfNode-" + std::to_string(ref.chunk_index))
                                .string();
                    }
                    else
                    {
                        source_file = header_source_path;
                    }
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
                    const uint64_t source_length    = ref.length;
                    std::string    source_file;

                    if (is_sharded_ref)
                    {
                        if (detail::is_hf_uri(ref.object_path))
                        {
                            try
                            {
                                if (!shard_root.empty())
                                {
                                    source_file = detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                                }
                                else
                                {
                                    source_file = detail::fetch_chunk_to_cache(ref.object_path,
                                                                               0,
                                                                               source_length,
                                                                               "nodeOfName-" + std::to_string(ref.chunk_index))
                                                      .string();
                                }
                            }
                            catch (...)
                            {
                                source_file =
                                    detail::fetch_chunk_to_cache(ref.object_path,
                                                                 0,
                                                                 source_length,
                                                                 "nodeOfName-" + std::to_string(ref.chunk_index))
                                        .string();
                            }
                        }
                        else
                        {
                            source_file = detail::resolve_manifest_chunk_path(local_manifest_path, ref.object_path, shard_root).string();
                        }
                    }
                    else if (header_is_remote)
                    {
                        source_file =
                            detail::fetch_chunk_to_cache(header_source, source_offset, source_length, "nodeOfName-" + std::to_string(ref.chunk_index))
                                .string();
                    }
                    else
                    {
                        source_file = header_source_path;
                    }
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
        }
        void saveToFile(const std::string& filename) const
        {
            const size_t chunkSize = 1000000; // 1M entries per chunk

            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Saving: probabilities size=" << _probabilities.size() << ", left size=" << _left.size() << ", right size=" << _right.size();
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
            ::capnp::MallocMessageBuilder mainMessage(1u << 26);
            auto                          impl = mainMessage.initRoot<ZelphImpl>();

            // Serialize probabilities
            auto   probs = impl.initProbabilities(_probabilities.size());
            size_t idx   = 0;
            for (const auto& p : _probabilities)
            {
                probs[idx].setHash(p.first);
                probs[idx].setProb(static_cast<double>(p.second));
                ++idx;
            }
            impl.setLast(_last);
            impl.setLastVar(_last_var);

            size_t nameOfNodeChunkTotal = 0;
            for (const auto& langMap : _name_of_node)
            {
                size_t mapSize = langMap.second.size();
                nameOfNodeChunkTotal += (mapSize + chunkSize - 1) / chunkSize;
            }
            impl.setNameOfNodeChunkCount(static_cast<uint32_t>(nameOfNodeChunkTotal));

            size_t nodeOfNameChunkTotal = 0;
            for (const auto& langMap : _node_of_name)
            {
                size_t mapSize = langMap.second.size();
                nodeOfNameChunkTotal += (mapSize + chunkSize - 1) / chunkSize;
            }
            impl.setNodeOfNameChunkCount(static_cast<uint32_t>(nodeOfNameChunkTotal));

            size_t leftChunkCount  = (_left.size() + chunkSize - 1) / chunkSize;
            size_t rightChunkCount = (_right.size() + chunkSize - 1) / chunkSize;
            impl.setLeftChunkCount(static_cast<uint32_t>(leftChunkCount));
            impl.setRightChunkCount(static_cast<uint32_t>(rightChunkCount));

            ::capnp::writePackedMessage(output, mainMessage);

            // Chunk _left
            auto leftIt = _left.begin();
            for (size_t chunkIdx = 0; chunkIdx < leftChunkCount; ++chunkIdx)
            {
                ::capnp::MallocMessageBuilder chunkMessage(1u << 26);
                auto                          chunk = chunkMessage.initRoot<AdjChunk>();
                chunk.setWhich("left");
                chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

                size_t thisChunkSize = std::min(chunkSize, _left.size() - chunkIdx * chunkSize);
                auto   pairList      = chunk.initPairs(thisChunkSize);
                size_t pIdx          = 0;
                for (size_t i = 0; i < thisChunkSize; ++i, ++leftIt)
                {
                    pairList[pIdx].setNode(leftIt->first);
                    std::vector<Node> sorted(leftIt->second.begin(), leftIt->second.end());
                    std::sort(sorted.begin(), sorted.end());
                    auto adj = pairList[pIdx].initAdj(sorted.size());
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
                ::capnp::MallocMessageBuilder chunkMessage(1u << 26);
                auto                          chunk = chunkMessage.initRoot<AdjChunk>();
                chunk.setWhich("right");
                chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

                size_t thisChunkSize = std::min(chunkSize, _right.size() - chunkIdx * chunkSize);
                auto   pairList      = chunk.initPairs(thisChunkSize);
                size_t pIdx          = 0;
                for (size_t i = 0; i < thisChunkSize; ++i, ++rightIt)
                {
                    pairList[pIdx].setNode(rightIt->first);
                    std::vector<Node> sorted(rightIt->second.begin(), rightIt->second.end());
                    std::sort(sorted.begin(), sorted.end());
                    auto adj = pairList[pIdx].initAdj(sorted.size());
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
                std::vector<std::pair<Node, std::string_view>> sorted(map.begin(), map.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                          { return a.first < b.first; });

                auto   it        = sorted.begin();
                size_t numChunks = (sorted.size() + chunkSize - 1) / chunkSize;
                for (size_t chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx)
                {
                    ::capnp::MallocMessageBuilder chunkMessage(1u << 26);
                    auto                          chunk = chunkMessage.initRoot<NameChunk>();
                    chunk.setLang(lang);
                    chunk.setChunkIndex(nameOfNodeChunkIndex++);

                    size_t thisSize = std::min(chunkSize, sorted.size() - chunkIdx * chunkSize);
                    auto   pairs    = chunk.initPairs(thisSize);
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
                std::vector<std::pair<std::string_view, Node>> sorted(map.begin(), map.end());
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                          { return a.first < b.first; });

                auto   it        = sorted.begin();
                size_t numChunks = (sorted.size() + chunkSize - 1) / chunkSize;
                for (size_t chunkIdx = 0; chunkIdx < numChunks; ++chunkIdx)
                {
                    ::capnp::MallocMessageBuilder chunkMessage(1u << 26);
                    auto                          chunk = chunkMessage.initRoot<NodeNameChunk>();
                    chunk.setLang(lang);
                    chunk.setChunkIndex(nodeOfNameChunkIndex++);

                    size_t thisSize = std::min(chunkSize, sorted.size() - chunkIdx * chunkSize);
                    auto   pairs    = chunk.initPairs(thisSize);
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

        void loadFromFile(const std::string& filename)
        {
#ifdef _WIN32
    #define fileno _fileno
#endif
            FILE* file = fopen(filename.c_str(), "rb");
            if (!file)
            {
                throw std::runtime_error("Failed to open file for reading: " + filename);
            }

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

        void loadFromFile(const std::string&              filename,
                          const Zelph::BinChunkSelection& selection,
                          const bool                      skip_payload)
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

                clear_loaded_state();
                loadSmallData(impl);

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

                io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                    << "Partial loading: left chunks=" << requestedLeftChunks << "/" << leftChunkCount
                    << ", right chunks=" << requestedRightChunks << "/" << rightChunkCount
                    << ", nameOfNode chunks=" << requestedNameOfNodeChunks << "/"
                    << nameOfNodeChunkCount
                    << ", nodeOfName chunks=" << requestedNodeOfNameChunks << "/"
                    << nodeOfNameChunkCount
                    << ", skip_payload=" << (skip_payload ? "true" : "false");

                validate_chunk_selector(leftSelector, leftChunkCount, "left");
                validate_chunk_selector(rightSelector, rightChunkCount, "right");
                validate_chunk_selector(nameOfNodeSelector, nameOfNodeChunkCount, "nameOfNode");
                validate_chunk_selector(nodeOfNameSelector, nodeOfNameChunkCount, "nodeOfName");

                if (skip_payload)
                {
                    io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Header-only file load complete.";
                    fclose(file);
                    return;
                }

                if (leftSelectorPtr == nullptr || !leftSelector.empty() || rightSelectorPtr == nullptr || !rightSelector.empty())
                {
                    loadLeftRightChunks(bufferedInput,
                                        options,
                                        leftChunkCount,
                                        rightChunkCount,
                                        leftSelectorPtr,
                                        rightSelectorPtr);
                }
                if (nameOfNodeSelectorPtr == nullptr || !nameOfNodeSelector.empty())
                {
                    loadNameOfNodeChunks(bufferedInput, options, nameOfNodeChunkCount, nameOfNodeSelectorPtr);
                }
                if (nodeOfNameSelectorPtr == nullptr || !nodeOfNameSelector.empty())
                {
                    loadNodeOfNameChunks(bufferedInput, options, nodeOfNameChunkCount, nodeOfNameSelectorPtr);
                }

                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "String pool size after partial load: " << _string_pool.size();

                fclose(file);
            }
            catch (...)
            {
                fclose(file);
                throw;
            }
        }

        void transfer_names_locked(const Node from, const Node into)
        {
            if (from == into) return;

            // PRECONDITION:
            // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively,
            // always in this order: _mtx_node_of_name -> _mtx_name_of_node

            for (auto& [lang, forward] : _name_of_node)
            {
                auto it_from = forward.find(from);
                if (it_from == forward.end())
                {
                    continue;
                }

                const std::string_view from_name = it_from->second;

                auto [reverse_outer_it, inserted_reverse_outer] = _node_of_name.try_emplace(lang);
                (void)inserted_reverse_outer;
                auto& reverse = reverse_outer_it->second;

                // Remove reverse entry for the disappearing pair (from_name -> from), if it exists.
                auto rev_from_it = reverse.find(from_name);
                if (rev_from_it != reverse.end() && rev_from_it->second == from)
                {
                    reverse.erase(rev_from_it);
                }

                auto it_into = forward.find(into);
                if (it_into != forward.end())
                {
                    const std::string_view into_name = it_into->second;

                    if (into_name != from_name)
                    {
                        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                            << "Warning: Name conflict in language '" << lang << "': '"
                            << from_name << "' (from merged node) vs '" << into_name
                            << "'. Keeping existing name '" << into_name << "'.";
                    }

                    // Drop the old forward mapping for "from".
                    forward.erase(it_from);

                    // Repair reverse mapping for the kept name.
                    auto rev_into_it = reverse.find(into_name);
                    if (rev_into_it == reverse.end())
                    {
                        reverse.emplace(into_name, into);
                    }
                    else if (rev_into_it->second == from)
                    {
                        rev_into_it->second = into;
                    }
                }
                else
                {
                    // Move forward mapping from -> into
                    forward.erase(it_from);
                    auto [new_into_it, inserted_into] = forward.emplace(into, from_name);
                    if (!inserted_into)
                    {
                        new_into_it->second = from_name;
                    }

                    // Move reverse mapping from_name -> into
                    auto rev_it = reverse.find(from_name);
                    if (rev_it == reverse.end())
                    {
                        reverse.emplace(from_name, into);
                    }
                    else if (rev_it->second == from)
                    {
                        rev_it->second = into;
                    }
                    else if (rev_it->second != into)
                    {
                        io::OutputStream(_output, io::OutputChannel::Diagnostic, true)
                            << "Warning: Skipping reverse mapping update for name '" << from_name
                            << "' in language '" << lang
                            << "' due to existing conflicting mapping.";
                    }
                }
            }
        }

        void transfer_names(const Node from, const Node into)
        {
            if (from == into) return;

            // Global lock order must be consistent everywhere:
            // _mtx_node_of_name -> _mtx_name_of_node
            std::unique_lock lock_node(_mtx_node_of_name);
            std::unique_lock lock_name(_mtx_name_of_node);

            transfer_names_locked(from, into);
        }

        void assign_name_locked(const Node node, const std::string& name, const std::string& lang)
        {
            // PRECONDITION:
            // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively
            // in this order: _mtx_node_of_name -> _mtx_name_of_node

            auto [rev_outer_it, rev_outer_inserted] = _node_of_name.try_emplace(lang);
            auto [fwd_outer_it, fwd_outer_inserted] = _name_of_node.try_emplace(lang);
            (void)rev_outer_inserted;
            (void)fwd_outer_inserted;

            auto& rev = rev_outer_it->second; // name -> node
            auto& fwd = fwd_outer_it->second; // node -> name

            // 1. Remove old name of this node, if different
            auto fwd_it = fwd.find(node);
            if (fwd_it != fwd.end() && fwd_it->second != name)
            {
                auto old_rev_it = rev.find(fwd_it->second);
                if (old_rev_it != rev.end() && old_rev_it->second == node)
                {
                    rev.erase(old_rev_it);
                }
                fwd.erase(fwd_it);
            }

            // 2. Intern new name
            std::string_view sv = _string_pool.intern(name);

            // 3. If another node currently owns that name, detach its forward mapping
            auto rev_it = rev.find(sv);
            if (rev_it != rev.end() && rev_it->second != node)
            {
                const Node previous_owner = rev_it->second;

                auto prev_fwd_it = fwd.find(previous_owner);
                if (prev_fwd_it != fwd.end() && prev_fwd_it->second == sv)
                {
                    fwd.erase(prev_fwd_it);
                }

                rev_it->second = node;
            }
            else if (rev_it == rev.end())
            {
                rev.emplace(sv, node);
            }

            // 4. Write/repair forward mapping
            auto [new_fwd_it, inserted] = fwd.emplace(node, sv);
            if (!inserted)
            {
                new_fwd_it->second = sv;
            }
        }

        void remove_name_locked(const Node node, const std::string& lang)
        {
            // PRECONDITION:
            // caller holds _mtx_node_of_name and _mtx_name_of_node exclusively
            // in this order: _mtx_node_of_name -> _mtx_name_of_node

            auto fwd_outer_it = _name_of_node.find(lang);
            if (fwd_outer_it == _name_of_node.end())
            {
                return;
            }

            auto& fwd    = fwd_outer_it->second;
            auto  fwd_it = fwd.find(node);
            if (fwd_it == fwd.end())
            {
                return;
            }

            const std::string_view old_name = fwd_it->second;
            fwd.erase(fwd_it);

            auto rev_outer_it = _node_of_name.find(lang);
            if (rev_outer_it == _node_of_name.end())
            {
                return;
            }

            auto& rev    = rev_outer_it->second;
            auto  rev_it = rev.find(old_name);
            if (rev_it != rev.end() && rev_it->second == node)
            {
                rev.erase(rev_it);
            }
        }

        size_t cleanup_dangling_names()
        {
            size_t removed_count = 0;

            ankerl::unordered_dense::set<Node> valid_nodes;
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_left);
                valid_nodes.reserve(_left.size());
                for (const auto& pair : _left)
                {
                    valid_nodes.insert(pair.first);
                }
            }

            {
                std::unique_lock lock(_mtx_name_of_node);
                for (auto& lang_pair : _name_of_node)
                {
                    auto& map = lang_pair.second;
                    for (auto it = map.begin(); it != map.end();)
                    {
                        if (valid_nodes.count(it->first) == 0)
                        {
                            it = map.erase(it);
                            removed_count++;
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
            }

            {
                std::unique_lock lock(_mtx_node_of_name);
                for (auto& lang_pair : _node_of_name)
                {
                    auto& map = lang_pair.second;
                    for (auto it = map.begin(); it != map.end();)
                    {
                        if (valid_nodes.count(it->second) == 0)
                        {
                            it = map.erase(it);
                            removed_count++;
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
            }

            // Note: removed strings remain in _string_pool (append-only).

            return removed_count;
        }

        void remove_node_names(Node nd)
        {
            std::unique_lock lock1(_mtx_node_of_name);
            std::unique_lock lock2(_mtx_name_of_node);

            // Remove forward mappings (node → name) in all languages
            for (auto& lang_map : _name_of_node)
            {
                lang_map.second.erase(nd);
            }

            // Remove reverse mappings (name → node) for this node in all languages
            for (auto& lang_map : _node_of_name)
            {
                auto& map = lang_map.second;
                for (auto it = map.begin(); it != map.end();)
                {
                    if (it->second == nd)
                    {
                        it = map.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
        }

        static unsigned int index_build_threads()
        {
            const unsigned int hw = std::thread::hardware_concurrency();
            return hw == 0 ? 4u : hw;
        }

        // Phase 1: extract (subject, object) pairs from the predicate's
        // relation nodes (parallel; see previous comments on locking and
        // swap-bound random access). Returns the unsorted forward pairs.
        std::vector<detail::IndexPair> extract_predicate_pairs(const Node predicate) const
        {
            // Same lock order as writers (connect): left before right.
            std::shared_lock<std::shared_mutex> lock_left(_smtx_left);
            std::shared_lock<std::shared_mutex> lock_right(_smtx_right);

            std::vector<detail::IndexPair> fw;

            const auto rels_it = _right.find(predicate);
            if (rels_it == _right.end()) return fw;

            std::vector<Node> rels;
            rels.reserve(rels_it->second.size());
            for (const Node rel : rels_it->second)
                rels.push_back(rel);

            const size_t n_threads =
                rels.size() >= (size_t(1) << 15) ? index_build_threads() : 1;

            emit(io::OutputChannel::Diagnostic,
                 "Building adjacency index over " + std::to_string(rels.size())
                     + " relation nodes (" + std::to_string(n_threads) + " thread(s))...");

            std::vector<std::vector<detail::IndexPair>> partial(n_threads);
            std::atomic<bool>                           failed{false};

            auto extract_chunk = [&](const size_t begin, const size_t end, std::vector<detail::IndexPair>& out_pairs)
            {
                try
                {
                    std::vector<Node> subjects;
                    for (size_t i = begin; i < end; ++i)
                    {
                        const Node rel = rels[i];

                        const auto rl_it = _left.find(rel);
                        const auto rr_it = _right.find(rel);
                        if (rl_it == _left.end() || rr_it == _right.end()) continue;

                        const adjacency_set& rel_left  = rl_it->second;
                        const adjacency_set& rel_right = rr_it->second;

                        // rel may be in _right[predicate] because the predicate
                        // is the *subject* of rel (e.g. (P ~ ->)); such relations
                        // contribute nothing below.
                        if (rel_left.count(predicate) == 0) continue;

                        subjects.clear();
                        for (const Node cand : rel_left)
                        {
                            if (cand == predicate || is_var(cand)) continue;
                            if (rel_right.count(cand) == 1) subjects.push_back(cand);
                        }
                        if (subjects.empty()) continue;

                        for (const Node obj : rel_right)
                        {
                            if (is_var(obj)) continue;
                            if (rel_left.count(obj) == 1) continue; // not a pure object
                            for (const Node subj : subjects)
                            {
                                out_pairs.emplace_back(subj, obj);
                            }
                        }
                    }
                }
                catch (...)
                {
                    failed.store(true, std::memory_order_relaxed);
                }
            };

            if (n_threads == 1)
            {
                extract_chunk(0, rels.size(), partial[0]);
            }
            else
            {
                std::vector<std::thread> workers;
                workers.reserve(n_threads);
                const size_t chunk = (rels.size() + n_threads - 1) / n_threads;
                for (size_t t = 0; t < n_threads; ++t)
                {
                    const size_t begin = t * chunk;
                    const size_t end   = std::min(rels.size(), begin + chunk);
                    if (begin >= end) break;
                    workers.emplace_back([&extract_chunk, &partial, begin, end, t]
                                         { extract_chunk(begin, end, partial[t]); });
                }
                for (auto& w : workers)
                    w.join();
            }

            if (failed.load(std::memory_order_relaxed))
            {
                throw std::runtime_error("Predicate index build failed (worker exception)");
            }

            size_t total = 0;
            for (const auto& p : partial)
                total += p.size();

            fw.reserve(total);
            for (auto& p : partial)
            {
                fw.insert(fw.end(), p.begin(), p.end());
                p.clear();
                p.shrink_to_fit();
            }

            return fw;
        }

        // Phase 2: sort each direction and fill the maps with exact-size
        // vectors. fw is sorted in place (and stays valid for persisting).
        static std::shared_ptr<const PredicateIndex> index_from_pairs(std::vector<detail::IndexPair>& fw)
        {
            auto idx = std::make_shared<PredicateIndex>();

            std::vector<detail::IndexPair> bw;
            bw.reserve(fw.size());
            for (const auto& [s, o] : fw)
                bw.emplace_back(o, s);

            std::atomic<bool> failed{false};

            auto fill = [&failed](std::vector<detail::IndexPair>& pairs, PredicateIndex::adjacency& out)
            {
                try
                {
                    std::sort(pairs.begin(), pairs.end());
                    out.reserve(pairs.size());
                    size_t i = 0;
                    while (i < pairs.size())
                    {
                        size_t j = i;
                        while (j < pairs.size() && pairs[j].first == pairs[i].first)
                            ++j;

                        auto& vec = out[pairs[i].first];
                        vec.reserve(j - i);
                        for (size_t k = i; k < j; ++k)
                        {
                            if (k > i && pairs[k] == pairs[k - 1]) continue;
                            vec.push_back(pairs[k].second);
                        }
                        i = j;
                    }
                }
                catch (...)
                {
                    failed.store(true, std::memory_order_relaxed);
                }
            };

            std::thread bw_thread([&]
                                  { fill(bw, idx->backward); });
            fill(fw, idx->forward);
            bw_thread.join();

            if (failed.load(std::memory_order_relaxed))
            {
                throw std::runtime_error("Predicate index build failed (fill exception)");
            }

            return idx;
        }

        std::shared_ptr<const PredicateIndex> predicate_index(const Node predicate) const
        {
            {
                std::shared_lock lock(_pred_idx_mtx);
                const auto       it = _pred_idx_cache.find(predicate);
                if (it != _pred_idx_cache.end()) return it->second;
            }

            std::vector<detail::IndexPair> fw;
            bool                           fresh = false;

            if (!try_load_pidx(predicate, fw))
            {
                fw    = extract_predicate_pairs(predicate);
                fresh = true;
            }

            auto idx = index_from_pairs(fw); // sorts fw in place

            emit(io::OutputChannel::Diagnostic,
                 "Adjacency index ready: " + std::to_string(fw.size()) + " edges.");

            if (fresh)
            {
                try_save_pidx(predicate, fw);
            }

            std::unique_lock lock(_pred_idx_mtx);
            const auto [it, inserted] = _pred_idx_cache.try_emplace(predicate, std::move(idx));
            if (inserted) _pred_idx_has_entries.store(true, std::memory_order_release);
            return it->second;
        }

        // Lookup that only consumes an already-built index; never triggers a
        // build. Returns true if an index for the predicate exists (out then
        // holds the complete answer, possibly empty).
        bool try_indexed_fact_lookup(Node predicate, Node node, bool forward, adjacency_set& out) const
        {
            if (!_pred_idx_has_entries.load(std::memory_order_acquire)) return false;

            std::shared_ptr<const PredicateIndex> idx;
            {
                std::shared_lock lock(_pred_idx_mtx);
                const auto       it = _pred_idx_cache.find(predicate);
                if (it == _pred_idx_cache.end()) return false;
                idx = it->second;
            }

            const auto& map = forward ? idx->forward : idx->backward;
            const auto  it  = map.find(node);
            if (it != map.end())
            {
                for (const Node n : it->second)
                    out.insert(n);
            }
            return true;
        }

        void invalidate_predicate_index() const noexcept
        {
            _pidx_io_enabled.store(false, std::memory_order_release);

            if (!_pred_idx_has_entries.exchange(false, std::memory_order_acq_rel)) return;
            std::unique_lock lock(_pred_idx_mtx);
            _pred_idx_cache.clear();
        }

        // Lock-once transitive traversal working directly on _left/_right
        // references: no adjacency_set copies, no per-edge lock acquisitions.
        // Aborts and returns false once `scan_budget` relation entries have
        // been scanned - hub nodes blow the budget immediately, signalling
        // the caller to switch to the predicate index. On false, `result`
        // is partial and must be discarded.
        bool try_transitive_direct(Node           start,
                                   Node           predicate,
                                   bool           include_start,
                                   bool           forward,
                                   size_t         scan_budget,
                                   adjacency_set& result) const
        {
            // Same lock order as writers (connect): left before right.
            std::shared_lock<std::shared_mutex> lock_left(_smtx_left);
            std::shared_lock<std::shared_mutex> lock_right(_smtx_right);

            ankerl::unordered_dense::set<Node> seen;
            std::vector<Node>                  frontier{start};
            size_t                             scanned = 0;

            if (include_start)
            {
                seen.insert(start);
                result.insert(start);
            }

            auto expand = [&](const Node n, std::vector<Node>& next) -> bool
            {
                // Outgoing edges of n: relations where n is subject or object.
                const auto edges_it = _left.find(n);
                if (edges_it == _left.end()) return true;

                scanned += edges_it->second.size();
                if (scanned > scan_budget) return false;

                for (const Node rel : edges_it->second)
                {
                    const auto rl_it = _left.find(rel);
                    if (rl_it == _left.end()) continue;
                    const adjacency_set& rel_left = rl_it->second;
                    if (rel_left.count(predicate) == 0) continue;

                    const auto rr_it = _right.find(rel);
                    if (rr_it == _right.end()) continue;
                    const adjacency_set& rel_right = rr_it->second;

                    if (forward)
                    {
                        // n must be the subject (bidirectional with rel).
                        if (rel_left.count(n) == 0 || rel_right.count(n) == 0) continue;
                        for (const Node obj : rel_right)
                        {
                            if (obj == n || is_var(obj)) continue;
                            if (rel_left.count(obj) == 1) continue; // not a pure object
                            if (seen.insert(obj).second)
                            {
                                result.insert(obj);
                                next.push_back(obj);
                            }
                        }
                    }
                    else
                    {
                        // n must be in the pure object role.
                        if (rel_right.count(n) == 0 || rel_left.count(n) == 1) continue;
                        for (const Node subj : rel_right)
                        {
                            if (subj == n || subj == predicate || is_var(subj)) continue;
                            if (rel_left.count(subj) == 0) continue; // subjects are bidirectional
                            if (seen.insert(subj).second)
                            {
                                result.insert(subj);
                                next.push_back(subj);
                            }
                        }
                    }
                }
                return true;
            };

            while (!frontier.empty())
            {
                std::vector<Node> next;
                for (const Node n : frontier)
                {
                    if (!expand(n, next)) return false;
                }
                frontier = std::move(next);
            }
            return true;
        }

        void emit(io::OutputChannel channel, const std::string& text, bool newline = true) const
        {
            if (_output)
                _output(io::OutputEvent{channel, text, newline});
        }

        using name_of_node_map = ankerl::unordered_dense::map<Node, std::string_view>;
        using node_of_name_map = ankerl::unordered_dense::map<std::string_view, Node>;

        StringPool _string_pool;

        ankerl::unordered_dense::map<std::string, name_of_node_map> _name_of_node; // key is language identifier
        ankerl::unordered_dense::map<std::string, node_of_name_map> _node_of_name; // key is language identifier

        mutable std::shared_mutex    _mtx_node_of_name;
        mutable std::shared_mutex    _mtx_name_of_node;
        mutable std::recursive_mutex _mtx_print;

        mutable std::shared_mutex                                              _fs_cache_mtx;
        mutable ankerl::unordered_dense::map<Node, std::vector<FactStructure>> _fs_cache;
        mutable std::atomic<bool>                                              _fs_cache_has_entries{false};

        mutable std::shared_mutex                                                         _pred_idx_mtx;
        mutable ankerl::unordered_dense::map<Node, std::shared_ptr<const PredicateIndex>> _pred_idx_cache;
        mutable std::atomic<bool>                                                         _pred_idx_has_entries{false};

        std::string               _pidx_base;
        Node                      _pidx_node_count{0};
        Node                      _pidx_last{0};
        Node                      _pidx_last_var{0};
        mutable std::atomic<bool> _pidx_io_enabled{false};

        int               _max_log_depth{0};
        bool              _logging{false};
        io::OutputHandler _output;
    };
}
