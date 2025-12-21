/*
Copyright (c) 2025 acrion innovations GmbH
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

#include "network_types.hpp"
#include "string_utils.hpp"

#include <ankerl/unordered_dense.h>

#include <boost/serialization/map.hpp>
#include <boost/serialization/vector.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <vector>

namespace zelph
{
    namespace network
    {
        using adjacency_map = ankerl::unordered_dense::map<Node, adjacency_set>;

        inline std::shared_ptr<Variables> join(const Variables& v1, const Variables& v2)
        {
            std::shared_ptr<Variables> result = std::make_shared<Variables>(v1);

            for (auto& var : v2)
            {
                auto it = result->find(var.first);

                if (it != result->end())
                {
                    if (it->second != var.second)
                    {
                        throw std::runtime_error("Variable sets to be merged do conflict");
                    }
                }
                else
                {
                    (*result)[var.first] = var.second;
                }
            }

            return result;
        }

        class Network
        {
        public:
            friend class boost::serialization::access;

            template <class Archive>
            void serialize(Archive& ar, const unsigned int /*version*/)
            {
                ar & _probabilities;
                ar & _last;
                ar & _last_var;

                std::size_t left_size = _left.size();
                ar & left_size;
                if constexpr (Archive::is_loading::value)
                {
                    _left.clear();
                    for (std::size_t i = 0; i < left_size; ++i)
                    {
                        Node key;
                        ar & key;
                        adjacency_set value;
                        std::size_t   value_size;
                        ar & value_size;
                        value.reserve(value_size);
                        for (std::size_t j = 0; j < value_size; ++j)
                        {
                            Node n;
                            ar & n;
                            value.insert(n);
                        }
                        _left[key] = std::move(value);
                    }
                }
                else
                {
                    for (const auto& p : _left)
                    {
                        ar & p.first;
                        std::size_t value_size = p.second.size();
                        ar & value_size;
                        for (const Node n : p.second)
                        {
                            ar & n;
                        }
                    }
                }

                std::size_t right_size = _right.size();
                ar & right_size;
                if constexpr (Archive::is_loading::value)
                {
                    _right.clear();
                    for (std::size_t i = 0; i < right_size; ++i)
                    {
                        Node key;
                        ar & key;
                        adjacency_set value;
                        std::size_t   value_size;
                        ar & value_size;
                        value.reserve(value_size);
                        for (std::size_t j = 0; j < value_size; ++j)
                        {
                            Node n;
                            ar & n;
                            value.insert(n);
                        }
                        _right[key] = std::move(value);
                    }
                }
                else
                {
                    for (const auto& p : _right)
                    {
                        ar & p.first;
                        std::size_t value_size = p.second.size();
                        ar & value_size;
                        for (const Node n : p.second)
                        {
                            ar & n;
                        }
                    }
                }
            }

            void connect(Node a, Node b, long double probability = 1)
            {
                std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
                std::unique_lock<std::shared_mutex> lock_right(_smtx_right);
                auto                                leftIt  = _left.find(a);
                auto                                rightIt = _right.find(b);

                if (leftIt == _left.end())
                {
                    throw std::runtime_error("Network::connect: requested left node " + std::to_string(a) + " does not exist");
                }

                if (rightIt == _right.end())
                {
                    throw std::runtime_error("Network::connect: requested right node " + std::to_string(b) + " does not exist");
                }

                if (probability < 1)
                {
                    if (is_var(a | b))
                    {
                        throw std::runtime_error("Network::connect: setting probabilities for connection that include variables");
                    }

                    Node                        hash;
                    std::lock_guard<std::mutex> lock3(_mtx_prob);
                    auto                        it = find_probability(a, b, hash);

                    if (it == _probabilities.end())
                    {
                        _probabilities[hash] = probability;
                    }
                    else if (it->second >= 0.5L && probability >= 0.5L)
                    {
                        it->second = std::max(it->second, probability);
                    }
                    else if (it->second <= 0.5L && probability <= 0.5L)
                    {
                        it->second = std::min(it->second, probability);
                    }
                    else
                    {
                        throw std::runtime_error("Network::connect: nodes have contradicting probabilities");
                    }
                }

                leftIt->second.insert(b);
                rightIt->second.insert(a);
            }

            bool exists(Node a)
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_left);
                return _left.find(a) != _left.end();
            }

            long double probability(Node a, Node b)
            {
                if (is_var(a | b))
                {
                    return 1;
                }

                std::shared_lock<std::shared_mutex> lock(_smtx_left);
                auto                                itLeft = _left.find(a);
                if (itLeft != _left.end() && itLeft->second.count(b) == 1)
                {
                    Node                        hash;
                    std::lock_guard<std::mutex> lock3(_mtx_prob);
                    auto                        it = find_probability(a, b, hash);
                    return it == _probabilities.end() ? 1 : it->second;
                }
                else
                {
                    return 0;
                }
            }

            Node create()
            {
                std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
                std::unique_lock<std::shared_mutex> lock_right(_smtx_right);

                while (_left.find(++_last) != _left.end())
                    ;

                if (is_var(_last))
                {
                    throw std::logic_error("Network::var: Exceeded maximum number of " + std::to_string(_last - 1) + " nodes.");
                }

                _left[_last]  = adjacency_set{};
                _right[_last] = adjacency_set{};
                _node_count.fetch_add(1, std::memory_order_relaxed);
                return _last;
            }

            Node count() const
            {
                return _node_count.load(std::memory_order_relaxed);
            }

            Node var()
            {
                std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
                std::unique_lock<std::shared_mutex> lock_right(_smtx_right);

                if (_left.find(--_last_var) != _left.end())
                {
                    throw std::runtime_error("Network::var: Node " + std::to_string(_last_var) + " already in use");
                }

                if (!is_var(_last_var))
                {
                    throw std::logic_error("Network::var: Exceeded maximum number of " + std::to_string(std::numeric_limits<Node>::max() - _last_var) + " variables.");
                }

                _left[_last_var]  = adjacency_set{};
                _right[_last_var] = adjacency_set{};

                return _last_var;
            }

            static bool is_var(Node a)
            {
                return a > mask_node;
            }

            static bool is_hash(Node a)
            {
                return (a & mark_hash) == mark_hash;
            }

            void create(const Node a)
            {
                std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
                std::unique_lock<std::shared_mutex> lock_right(_smtx_right);

                if (_left.find(a) != _left.end())
                {
                    throw std::runtime_error("Network::create: requested node " + std::to_string(a) + " already in use");
                }

                if (is_var(a))
                {
                    throw std::runtime_error("Network::create: requested node " + std::to_string(a) + " conflicts with variable values");
                }

                _left[a]  = adjacency_set{};
                _right[a] = adjacency_set{};
            }

            static inline uint64_t mix_bits(uint64_t seed, uint64_t value)
            {
                seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 12) + (seed >> 4);
                return seed;
            }

            static Node create_hash(const Node a, const Node b)
            {
                uint64_t h = 0;
                h          = mix_bits(h, mod(a));
                h          = mix_bits(h, mod(b));

                return (h & mask_node) | mark_hash;
            }

            static Node create_hash(const Node a, const Node b, const Node c)
            {
                uint64_t h = 0;
                h          = mix_bits(h, mod(a));
                h          = mix_bits(h, mod(b));
                h          = mix_bits(h, mod(c));
                return (h & mask_node) | mark_hash;
            }

            static Node create_hash(const adjacency_set& vec)
            {
                std::vector<Node> sorted_vec(vec.begin(), vec.end());
                std::sort(sorted_vec.begin(), sorted_vec.end());

                uint64_t h = 0;
                h          = mix_bits(h, sorted_vec.size());

                for (Node node : sorted_vec)
                {
                    h = mix_bits(h, mod(node));
                }
                return (h & mask_node) | mark_hash;
            }

            static Node create_hash(const Node head, const adjacency_set& vec)
            {
                Node     vec_hash = create_hash(vec);
                uint64_t h        = mix_bits(vec_hash, mod(head));

                return (h & mask_node) | mark_hash;
            }

            static Node create_hash(const Node head1, const Node head2, const adjacency_set& vec)
            {
                Node current_hash = create_hash(vec);
                current_hash      = mix_bits(current_hash, mod(head1));
                current_hash      = mix_bits(current_hash, mod(head2));
                return (current_hash & mask_node) | mark_hash;
            }

            bool has_left_edge(Node b, Node a) const
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_right);
                auto                                it = _right.find(b);
                return it != _right.end() && it->second.count(a) == 1;
            }

            bool has_right_edge(Node a, Node b) const
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_left);
                auto                                it = _left.find(a);
                return it != _left.end() && it->second.count(b) == 1;
            }

            bool snapshot_left_of(Node b, adjacency_set& out) const
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_right);
                auto                                it = _right.find(b);
                if (it == _right.end()) return false;
                out = it->second;
                return true;
            }

            // get predecessors / incoming edges
            adjacency_set get_left(const Node b)
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_right);
                auto                                it = _right.find(b);
                if (it == _right.end())
                {
                    return {};
                }
                return it->second;
            }

            // get successors / outgoing edges
            adjacency_set get_right(const Node b)
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_left);
                auto                                it = _left.find(b);
                if (it == _left.end())
                {
                    return {};
                }
                return it->second;
            }

#ifndef _DEBUG
        private:
#endif
            static constexpr Node shift_inc           = 5;
            static constexpr Node mark_hash           = 0x4000000000000000ull;
            static constexpr Node mask_node           = 0x7FFFFFFFFFFFFFFFull; // mask highest bit
            static constexpr Node mask_highest_2_bits = 0x3fffffffffffffffull;

            adjacency_map _left;
            adjacency_map _right;

            std::map<Node, long double> _probabilities;
            Node                        _last{Node()};
            Node                        _last_var{Node()};
            std::atomic<Node>           _node_count{0};
            mutable std::mutex          _mtx_prob;
            mutable std::shared_mutex   _smtx_left;
            mutable std::shared_mutex   _smtx_right;

            typename decltype(_probabilities)::iterator find_probability(Node a, Node b, Node& hash)
            {
                hash = create_hash(a, b);
                return _probabilities.find(hash);
            }

#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 4146)
#endif
            static inline uint64_t rol(const uint64_t n, uint64_t c = 1)
            {
                constexpr uint64_t mask = 8 * sizeof(n) - 1;
                c &= mask;
                // uint64_t wrapped = n >> ((-c)&mask);
                uint64_t wrapped = (n & mask_highest_2_bits) >> (((-c) & mask) - 2); // we want to wrap already at bit 61 (not 63), since mask_node clears bit 63 (which marks vars) and we use bit 62 to denote hashes (mark_hash)
                return ((n << c) | wrapped) & mask_highest_2_bits;
            }

            static inline uint64_t ror(const uint64_t n, uint64_t c = 1)
            {
                constexpr uint64_t mask = 8 * sizeof(n) - 1;
                c &= mask;
                return (n >> c) | (n << ((-c) & mask));
            }
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

            static uint64_t mod(uint64_t n)
            {
                // We generate nodes both by counting up and down (for vars) from 0, which increases probability of hash collisions.
                // So, make a clear difference between those two categories.
                return n > mask_node ? ror(n, 32) : n;
            }
        };
    }
}