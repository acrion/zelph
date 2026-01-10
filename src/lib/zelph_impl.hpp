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

#include "zelph.capnp.h"

#include <ankerl/unordered_dense.h>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <kj/io.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace zelph
{
    namespace network
    {
        class ZELPH_EXPORT Zelph::Impl : public Network
        {
            friend class Zelph;
            Impl() = default;

            void saveToFile(const std::string& filename)
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
                impl.setNodeCount(_node_count.load(std::memory_order_relaxed));

                // Serialize _name_of_node
                auto nameOfNode = impl.initNameOfNode(_name_of_node.size());
                idx             = 0;
                for (const auto& langMap : _name_of_node)
                {
                    nameOfNode[idx].setLang(langMap.first);
                    std::vector<std::pair<Node, std::wstring>> sorted(langMap.second.begin(), langMap.second.end());
                    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                              { return a.first < b.first; });
                    auto   pairs = nameOfNode[idx].initPairs(sorted.size());
                    size_t pIdx  = 0;
                    for (const auto& pair : sorted)
                    {
                        pairs[pIdx].setKey(pair.first);
                        pairs[pIdx].setValue(zelph::string::unicode::to_utf8(pair.second));
                        ++pIdx;
                    }
                    ++idx;
                }

                // Serialize _node_of_name
                auto nodeOfName = impl.initNodeOfName(_node_of_name.size());
                idx             = 0;
                for (const auto& langMap : _node_of_name)
                {
                    nodeOfName[idx].setLang(langMap.first);
                    std::vector<std::pair<std::wstring, Node>> sorted(langMap.second.begin(), langMap.second.end());
                    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
                              { return a.first < b.first; });
                    auto   pairs = nodeOfName[idx].initPairs(sorted.size());
                    size_t pIdx  = 0;
                    for (const auto& pair : sorted)
                    {
                        pairs[pIdx].setKey(zelph::string::unicode::to_utf8(pair.first));
                        pairs[pIdx].setValue(pair.second);
                        ++pIdx;
                    }
                    ++idx;
                }

                impl.setFormatFactLevel(_format_fact_level);

                // Calculate and set chunk counts
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
                kj::FdInputStream              rawInput(fileno(file));
                kj::BufferedInputStreamWrapper bufferedInput(rawInput);

                // Set ReaderOptions for large data (adjust traversalLimit based on RAM; 1ULL << 30 = ~8GB)
                ::capnp::ReaderOptions options;
                options.traversalLimitInWords = 1ULL << 30; // Increase to handle huge lists
                options.nestingLimit          = 128;        // Increase if deep nesting

                // Read main message
                ::capnp::PackedMessageReader mainMessage(bufferedInput, options);
                auto                         impl = mainMessage.getRoot<ZelphImpl>();

                // Load small data from main
                _probabilities.clear();
                for (auto p : impl.getProbabilities())
                {
                    _probabilities[p.getHash()] = static_cast<long double>(p.getProb());
                }
                _last     = impl.getLast();
                _last_var = impl.getLastVar();
                _node_count.store(impl.getNodeCount(), std::memory_order_relaxed);

                // Load _name_of_node
                _name_of_node.clear();
                for (auto langMap : impl.getNameOfNode())
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

                // Load _node_of_name
                _node_of_name.clear();
                for (auto langMap : impl.getNodeOfName())
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

                _format_fact_level = impl.getFormatFactLevel();

                // Debug: Log chunk counts
                uint32_t leftChunkCount  = impl.getLeftChunkCount();
                uint32_t rightChunkCount = impl.getRightChunkCount();
                std::cerr << "Loading: left chunks=" << leftChunkCount << ", right chunks=" << rightChunkCount << std::endl;

                // Load left chunks
                _left.clear();
                for (uint32_t chunkIdx = 0; chunkIdx < leftChunkCount; ++chunkIdx)
                {
                    ::capnp::PackedMessageReader chunkMessage(bufferedInput, options); // Use options for each chunk
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
                    // Debug: Log after each chunk to track progress/memory
                    std::cerr << "Loaded left chunk " << chunkIdx + 1 << "/" << leftChunkCount << ", current _left size=" << _left.size() << std::endl;
                }

                // Load right chunks similarly
                _right.clear();
                for (uint32_t chunkIdx = 0; chunkIdx < rightChunkCount; ++chunkIdx)
                {
                    ::capnp::PackedMessageReader chunkMessage(bufferedInput, options); // Use options for each chunk
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
                    // Debug: Log after each chunk
                    std::cerr << "Loaded right chunk " << chunkIdx + 1 << "/" << rightChunkCount << ", current _right size=" << _right.size() << std::endl;
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