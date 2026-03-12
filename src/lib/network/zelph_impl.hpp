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

#include "network.hpp"
#include "zelph.hpp"

#include "io/zelph.capnp.h"

#include <ankerl/unordered_dense.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <kj/io.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
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

    class ZELPH_EXPORT Zelph::Impl : public Network
    {
        friend class Zelph;

        explicit Impl(const io::OutputHandler& output)
            : _output(output)
        {
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

        void loadLeftRightChunks(kj::BufferedInputStreamWrapper& bufferedInput, const ::capnp::ReaderOptions& options, uint32_t leftChunkCount, uint32_t rightChunkCount)
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
                for (auto pair : chunk.getPairs())
                {
                    adjacency_set adj;
                    for (auto n : pair.getAdj())
                    {
                        adj.insert(n);
                    }
                    _left[pair.getNode()] = std::move(adj);
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
                for (auto pair : chunk.getPairs())
                {
                    adjacency_set adj;
                    for (auto n : pair.getAdj())
                    {
                        adj.insert(n);
                    }
                    _right[pair.getNode()] = std::move(adj);
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
                    chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

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
                    chunk.setChunkIndex(static_cast<uint32_t>(chunkIdx));

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

            // Load _name_of_node (chunked, interning into string pool)
#ifdef CLEAR_ON_LOAD
            _name_of_node.clear();
            _string_pool.clear();
#endif
            for (uint32_t i = 0; i < nameOfNodeChunkCount; ++i)
            {
                ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
                auto                         chunk = chunkMessage.getRoot<NameChunk>();
                std::string                  lang  = chunk.getLang();
                auto&                        map   = _name_of_node[lang];
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
#ifndef NDEBUG
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded name_of_node chunk " << i + 1 << "/" << nameOfNodeChunkCount;
#else
                io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
#endif
            }
#ifdef NDEBUG
            io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
#endif

            // Load _node_of_name (chunked, interning into string pool)
#ifdef CLEAR_ON_LOAD
            _node_of_name.clear();
            // _string_pool already cleared above
#endif
            for (uint32_t i = 0; i < nodeOfNameChunkCount; ++i)
            {
                ::capnp::PackedMessageReader chunkMessage(bufferedInput, options);
                auto                         chunk = chunkMessage.getRoot<NodeNameChunk>();
                std::string                  lang  = chunk.getLang();
                auto&                        map   = _node_of_name[lang];
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
#ifndef NDEBUG
                io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "Loaded node_of_name chunk " << i + 1 << "/" << nodeOfNameChunkCount;
#else
                io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << "." << std::flush;
#endif
            }
#ifdef NDEBUG
            io::OutputStream(_output, io::OutputChannel::Diagnostic, false) << std::endl;
#endif

            io::OutputStream(_output, io::OutputChannel::Diagnostic, true) << "String pool size after load: " << _string_pool.size();

            fclose(file);
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

        int               _max_log_depth{0};
        bool              _logging{false};
        io::OutputHandler _output;
    };
}
