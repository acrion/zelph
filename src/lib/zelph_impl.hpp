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
#include "string_utils.hpp"
#include "zelph.hpp"

#include "zelph.capnp.h"

#include <ankerl/unordered_dense.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <kj/io.h>

#include <cstdint>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// #define CLEAR_ON_LOAD

namespace zelph
{
    namespace network
    {
        class ZELPH_EXPORT Zelph::Impl : public Network
        {
            friend class Zelph;
            Impl() = default;

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

                _format_fact_level = impl.getFormatFactLevel();
            }

            void loadSmallData(const ZelphImplOld::Reader& impl_old)
            {
#ifdef CLEAR_ON_LOAD
                _probabilities.clear();
#endif
                for (auto p : impl_old.getProbabilities())
                {
                    _probabilities[p.getHash()] = static_cast<long double>(p.getProb());
                }
                _last     = impl_old.getLast();
                _last_var = impl_old.getLastVar();

                _format_fact_level = impl_old.getFormatFactLevel();
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
#ifdef _DEBUG
                    std::cerr << "Loaded left chunk " << chunkIdx + 1 << "/" << leftChunkCount << ", current _left size=" << _left.size() << std::endl;
#else
                    std::cerr << "." << std::flush;
#endif
                }
#ifndef _DEBUG
                std::cerr << std::endl;
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
#ifdef _DEBUG
                    std::cerr << "Loaded right chunk " << chunkIdx + 1 << "/" << rightChunkCount << ", current _right size=" << _right.size() << std::endl;
#else
                    std::cerr << "." << std::flush;
#endif
                }
#ifndef _DEBUG
                std::cerr << std::endl;
#endif
            }

            void saveToFile(const std::string& filename) const
            {
                const size_t chunkSize = 1000000; // 1M entries per chunk; adjust based on RAM/testing

                // Debug: Log sizes before serializing large structures
                std::cerr << "Saving: probabilities size=" << _probabilities.size() << ", left size=" << _left.size() << ", right size=" << _right.size() << std::endl;
                std::cerr << "Saving: name_of_node outer size=" << _name_of_node.size() << ", node_of_name outer size=" << _node_of_name.size() << std::endl;

// Open file
#ifdef _WIN32
    #define fileno _fileno
#endif
                FILE* file = fopen(filename.c_str(), "wb");
                if (!file)
                {
                    throw std::runtime_error("Failed to open file for writing: " + filename);
                }
                kj::FdOutputStream output(fileno(file));

                // Main message (small data)
                ::capnp::MallocMessageBuilder mainMessage(1u << 26); // Large initial for safety
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

                impl.setFormatFactLevel(_format_fact_level);

                // Calculate and set chunk counts for left/right
                size_t leftChunkCount  = (_left.size() + chunkSize - 1) / chunkSize;
                size_t rightChunkCount = (_right.size() + chunkSize - 1) / chunkSize;
                impl.setLeftChunkCount(static_cast<uint32_t>(leftChunkCount));
                impl.setRightChunkCount(static_cast<uint32_t>(rightChunkCount));

                // Write main message
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

                // Chunk _right similarly
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
                for (const auto& langMap : _name_of_node)
                {
                    std::string                                lang = langMap.first;
                    const auto&                                map  = langMap.second;
                    std::vector<std::pair<Node, std::wstring>> sorted(map.begin(), map.end());
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
                            pairs[i].setValue(zelph::string::unicode::to_utf8(it->second));
                        }
                        ::capnp::writePackedMessage(output, chunkMessage);
                    }
                }

                // Chunk _node_of_name
                for (const auto& langMap : _node_of_name)
                {
                    std::string                                lang = langMap.first;
                    const auto&                                map  = langMap.second;
                    std::vector<std::pair<std::wstring, Node>> sorted(map.begin(), map.end());
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
                            pairs[i].setKey(zelph::string::unicode::to_utf8(it->first));
                            pairs[i].setValue(it->second);
                        }
                        ::capnp::writePackedMessage(output, chunkMessage);
                    }
                }
            }

            void loadFromFile(const std::string& filename)
            {
// Open file
#ifdef _WIN32
    #define fileno _fileno
#endif
                FILE* file = fopen(filename.c_str(), "rb");
                if (!file)
                {
                    throw std::runtime_error("Failed to open file for reading: " + filename);
                }

                // Set ReaderOptions for large data
                ::capnp::ReaderOptions options;
                options.traversalLimitInWords = 1ULL << 32; // 32 GB - TODO after stopping support for old format this can be reduced
                options.nestingLimit          = 128;

                // Neuen Format-Instanz
                kj::FdInputStream              rawInput(fileno(file));
                kj::BufferedInputStreamWrapper bufferedInput(rawInput);

                bool is_old_format = false;
                try
                {
                    ::capnp::PackedMessageReader mainMessage(bufferedInput, options);
                    auto                         impl = mainMessage.getRoot<ZelphImpl>();

                    loadSmallData(impl);

                    // Debug: Log chunk counts
                    uint32_t leftChunkCount       = impl.getLeftChunkCount();
                    uint32_t rightChunkCount      = impl.getRightChunkCount();
                    uint32_t nameOfNodeChunkCount = impl.getNameOfNodeChunkCount();
                    uint32_t nodeOfNameChunkCount = impl.getNodeOfNameChunkCount();
                    std::cerr << "Loading new format: left chunks=" << leftChunkCount << ", right chunks=" << rightChunkCount
                              << ", nameOfNode chunks=" << nameOfNodeChunkCount << ", nodeOfName chunks=" << nodeOfNameChunkCount << std::endl;

                    loadLeftRightChunks(bufferedInput, options, leftChunkCount, rightChunkCount);

// Load _name_of_node (chunked)
#ifdef CLEAR_ON_LOAD
                    _name_of_node.clear();
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
                                map[pair.getKey()] = zelph::string::unicode::from_utf8(pair.getValue());
                            }
                            catch (...)
                            {
                                map[pair.getKey()] = L"?";
                                std::cerr << "Error converting UTF-8 to wstring for name_of_node key " << pair.getKey() << std::endl;
                            }
                        }
#ifdef _DEBUG
                        std::cerr << "Loaded name_of_node chunk " << i + 1 << "/" << nameOfNodeChunkCount << std::endl;
#else
                        std::cerr << "." << std::flush;
#endif
                    }
#ifndef _DEBUG
                    std::cerr << std::endl;
#endif

// Load _node_of_name (chunked)
#ifdef CLEAR_ON_LOAD
                    _node_of_name.clear();
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
                                map[zelph::string::unicode::from_utf8(pair.getKey())] = pair.getValue();
                            }
                            catch (...)
                            {
                                map[L"?"] = pair.getValue();
                                std::cerr << "Error converting UTF-8 to wstring for node_of_name value " << pair.getValue() << std::endl;
                            }
                        }
#ifdef _DEBUG
                        std::cerr << "Loaded node_of_name chunk " << i + 1 << "/" << nodeOfNameChunkCount << std::endl;
#else
                        std::cerr << "." << std::flush;
#endif
                    }
#ifndef _DEBUG
                    std::cerr << std::endl;
#endif
                }
                catch (std::exception& ex)
                {
                    is_old_format = true;
                    // Schliesse das File und öffne neu für separate Instanzen
                    fclose(file);
                    file = fopen(filename.c_str(), "rb");
                    if (!file)
                    {
                        throw std::runtime_error("Failed to reopen file for old format: " + filename);
                    }

                    // Alte Format-Instanz (getrennt)
                    kj::FdInputStream              oldRawInput(fileno(file));
                    kj::BufferedInputStreamWrapper oldBufferedInput(oldRawInput);

                    // Load as old schema
                    ::capnp::PackedMessageReader mainMessage(oldBufferedInput, options);
                    auto                         impl_old = mainMessage.getRoot<ZelphImplOld>();

                    loadSmallData(impl_old);

// Load _name_of_node from old
#ifdef CLEAR_ON_LOAD
                    _name_of_node.clear();
#endif
                    for (auto langMap : impl_old.getNameOfNode())
                    {
                        name_of_node_map inner;
                        for (auto pair : langMap.getPairs())
                        {
                            try
                            {
                                inner[pair.getKey()] = zelph::string::unicode::from_utf8(pair.getValue());
                            }
                            catch (...)
                            {
                                inner[pair.getKey()] = L"?";
                                std::cerr << "Error converting UTF-8 to wstring for name_of_node key " << pair.getKey() << std::endl;
                            }
                        }
                        _name_of_node[langMap.getLang()] = std::move(inner);
                    }

// Load _node_of_name from old
#ifdef CLEAR_ON_LOAD
                    _node_of_name.clear();
#endif
                    for (auto langMap : impl_old.getNodeOfName())
                    {
                        node_of_name_map inner;
                        for (auto pair : langMap.getPairs())
                        {
                            try
                            {
                                inner[zelph::string::unicode::from_utf8(pair.getKey())] = pair.getValue();
                            }
                            catch (...)
                            {
                                inner[L"?"] = pair.getValue();
                                std::cerr << "Error converting UTF-8 to wstring for node_of_name value " << pair.getValue() << std::endl;
                            }
                        }
                        _node_of_name[langMap.getLang()] = std::move(inner);
                    }

                    // Debug: Log chunk counts (für old: Chunk-Counts für Names = 0)
                    uint32_t leftChunkCount       = impl_old.getLeftChunkCount();
                    uint32_t rightChunkCount      = impl_old.getRightChunkCount();
                    uint32_t nameOfNodeChunkCount = 0;
                    uint32_t nodeOfNameChunkCount = 0;
                    std::cerr << "Loading old format: left chunks=" << leftChunkCount << ", right chunks=" << rightChunkCount
                              << ", nameOfNode chunks=" << nameOfNodeChunkCount << ", nodeOfName chunks=" << nodeOfNameChunkCount << std::endl;

                    loadLeftRightChunks(oldBufferedInput, options, leftChunkCount, rightChunkCount);
                }

                fclose(file);
            }

            void transfer_names(const Node from, const Node into)
            {
                if (from == into) return;

                std::lock_guard lock1(_mtx_name_of_node);
                std::lock_guard lock2(_mtx_node_of_name);

                // Transfer forward mappings: node -> name
                for (auto& outer : _name_of_node)
                {
                    std::string lang = outer.first;
                    auto&       map  = outer.second;
                    auto        it   = map.find(from);
                    if (it != map.end())
                    {
                        std::wstring name = it->second;
                        map.erase(it);

                        auto it2 = map.find(into);
                        if (it2 != map.end())
                        {
                            if (it2->second != name)
                            {
                                std::wclog << L"Warning: Name conflict in language '" << string::unicode::from_utf8(lang) << L"': '" << name << L"' (from merged node) vs '" << it2->second << L"'. Keeping existing name '" << it2->second << L"'." << std::endl;
                            }
                            // No need to set, keep existing
                        }
                        else
                        {
                            map[into] = name;
                        }
                    }
                }

                // Update reverse mappings: name -> node
                for (auto& outer : _node_of_name)
                {
                    std::string lang = outer.first;
                    auto&       map  = outer.second;
                    for (auto it = map.begin(); it != map.end();)
                    {
                        if (it->second == from)
                        {
                            std::wstring name = it->first;
                            it                = map.erase(it);

                            // Check if name already maps to something else
                            auto it_existing = map.find(name);
                            if (it_existing == map.end())
                            {
                                // Safe to set to into
                                map[name] = into;
                            }
                            else
                            {
                                // Conflict, already maps to another node (likely into or other)
                                if (it_existing->second != into)
                                {
                                    std::wclog << L"Warning: Skipping reverse mapping update for name '" << name << L"' in language '" << string::unicode::from_utf8(lang) << L"' due to existing conflicting mapping." << std::endl;
                                }
                                // Else already points to into, ok
                            }
                        }
                        else
                        {
                            ++it;
                        }
                    }
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
                    std::lock_guard lock(_mtx_name_of_node);
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
                    std::lock_guard lock(_mtx_node_of_name);
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

                return removed_count;
            }

            void remove_node_names(Node nd)
            {
                std::lock_guard lock1(_mtx_name_of_node);
                std::lock_guard lock2(_mtx_node_of_name);

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

            using name_of_node_map = ankerl::unordered_dense::map<Node, std::wstring>;
            using node_of_name_map = ankerl::unordered_dense::map<std::wstring, Node>;

            ankerl::unordered_dense::map<std::string, name_of_node_map> _name_of_node; // key is language identifier
            ankerl::unordered_dense::map<std::string, node_of_name_map> _node_of_name; // key is language identifier

            mutable std::recursive_mutex _mtx_node_of_name;
            mutable std::recursive_mutex _mtx_name_of_node;
            mutable std::recursive_mutex _mtx_print;

            int _format_fact_level{0}; // recursion level of method format_fact
        };
    }
}
