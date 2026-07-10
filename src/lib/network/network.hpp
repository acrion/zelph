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

#include "adjacency_set.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <vector>

namespace zelph::network
{
    using adjacency_map = ankerl::unordered_dense::segmented_map<Node, adjacency_set>;

    class Network
    {
    public:
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

                // Probability semantics (constrained view of the weight store):
                // range checks and contradiction handling apply only on this path.
                Node             hash;
                std::unique_lock lock3(_mtx_weights);
                auto             it = find_weight(a, b, hash);

                if (it == _weights.end())
                {
                    _weights[hash] = static_cast<double>(probability);
                }
                else if (it->second >= 0.5 && probability >= 0.5L)
                {
                    it->second = std::max(it->second, static_cast<double>(probability));
                }
                else if (it->second <= 0.5 && probability <= 0.5L)
                {
                    it->second = std::min(it->second, static_cast<double>(probability));
                }
                else
                {
                    throw std::runtime_error("Network::connect: nodes have contradicting probabilities");
                }
            }

            leftIt->second.insert(b);
            rightIt->second.insert(a);
        }

        Node insert_fact_single_object_trusted(Node subject, Node predicate, Node object)
        {
            if (subject == 0 || predicate == 0 || object == 0)
            {
                throw std::invalid_argument("Network::insert_fact_single_object_trusted: subject/predicate/object must be non-zero");
            }

            const Node relation = create_hash(predicate, subject, object);

            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);

            if (_left.find(subject) == _left.end())
                throw std::runtime_error("Network::insert_fact_single_object_trusted: subject does not exist");
            if (_left.find(object) == _left.end())
                throw std::runtime_error("Network::insert_fact_single_object_trusted: object does not exist");
            if (_right.find(subject) == _right.end())
                throw std::runtime_error("Network::insert_fact_single_object_trusted: subject right-side entry does not exist");
            if (_right.find(predicate) == _right.end())
                throw std::runtime_error("Network::insert_fact_single_object_trusted: predicate right-side entry does not exist");

            auto [rel_left_it, inserted_left] =
                _left.try_emplace(relation, adjacency_set{subject, predicate});

            if (!inserted_left)
            {
                return relation;
            }

            note_created(relation);

            auto [rel_right_it, inserted_right] =
                (subject == object)
                    ? _right.try_emplace(relation, adjacency_set{subject})
                    : _right.try_emplace(relation, adjacency_set{subject, object});

            if (!inserted_right)
            {
                throw std::runtime_error("Network::insert_fact_single_object_trusted: inconsistent state, relation exists only on right side");
            }

            // Re-fetch after insertion
            auto subj_left  = _left.find(subject);
            auto subj_right = _right.find(subject);
            auto pred_right = _right.find(predicate);

            subj_left->second.insert(relation);
            subj_right->second.insert(relation);
            pred_right->second.insert(relation);

            if (subject != object)
            {
                auto obj_left = _left.find(object);
                obj_left->second.insert(relation);
            }

            return relation;
        }

        void disconnect(Node a, Node b)
        {
            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);

            auto leftIt = _left.find(a);
            if (leftIt != _left.end())
            {
                leftIt->second.erase(b);
            }

            auto rightIt = _right.find(b);
            if (rightIt != _right.end())
            {
                rightIt->second.erase(a);
            }

            // Remove probability if exists
            {
                std::unique_lock lock_weights(_mtx_weights);
                Node             hash;
                auto             it = find_weight(a, b, hash);
                if (it != _weights.end())
                {
                    _weights.erase(it);
                }
            }
        }

        void remove(Node node)
        {
            // Disconnect all incoming and outgoing edges
            {
                adjacency_set incoming = get_left(node);
                for (Node from : incoming)
                {
                    disconnect(from, node);
                }

                adjacency_set outgoing = get_right(node);
                for (Node to : outgoing)
                {
                    disconnect(node, to);
                }
            }

            // Remove the node itself
            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);
            _left.erase(node);
            _right.erase(node);
        }

        void merge(Node from, Node into)
        {
            if (from == into)
            {
                return; // Nothing to do if merging a node into itself
            }

            if (!exists(from) || !exists(into))
            {
                throw std::runtime_error("Network::merge: One or both nodes do not exist");
            }

            // Transfer outgoing connections from 'from' to 'into'
            adjacency_set outgoing = get_right(from);
            for (Node to : outgoing)
            {
                long double prob = probability(from, to);
                disconnect(from, to);
                // Connect only if not already connected to avoid duplicates
                if (!has_right_edge(into, to))
                {
                    connect(into, to, prob);
                }
                else
                {
                    // If already connected, update probability if necessary
                    long double existing_prob = probability(into, to);
                    if (existing_prob != prob)
                    {
                        // Resolve conflicting probabilities; here we take the max/min based on sign
                        if (existing_prob >= 0.5L && prob >= 0.5L)
                        {
                            connect(into, to, std::max(existing_prob, prob));
                        }
                        else if (existing_prob <= 0.5L && prob <= 0.5L)
                        {
                            connect(into, to, std::min(existing_prob, prob));
                        }
                        else
                        {
                            throw std::runtime_error("Network::merge: Conflicting probabilities between existing and transferred connection");
                        }
                    }
                }
            }

            // Transfer incoming connections from 'from' to 'into'
            adjacency_set incoming = get_left(from);
            for (Node fr : incoming)
            {
                long double prob = probability(fr, from);
                disconnect(fr, from);
                // Connect only if not already connected to avoid duplicates
                if (!has_left_edge(into, fr))
                {
                    connect(fr, into, prob);
                }
                else
                {
                    // If already connected, update probability if necessary
                    long double existing_prob = probability(fr, into);
                    if (existing_prob != prob)
                    {
                        // Resolve conflicting probabilities; here we take the max/min based on sign
                        if (existing_prob >= 0.5L && prob >= 0.5L)
                        {
                            connect(fr, into, std::max(existing_prob, prob));
                        }
                        else if (existing_prob <= 0.5L && prob <= 0.5L)
                        {
                            connect(fr, into, std::min(existing_prob, prob));
                        }
                        else
                        {
                            throw std::runtime_error("Network::merge: Conflicting probabilities between existing and transferred connection");
                        }
                    }
                }
            }

            // Remove the 'from' node after transferring connections
            remove(from);
        }

        void remove_isolated_nodes(size_t& removed_count)
        {
            removed_count = 0;

            std::vector<Node> all_nodes;
            {
                std::shared_lock<std::shared_mutex> lock_left(_smtx_left);
                all_nodes.reserve(_left.size());
                for (const auto& p : _left)
                {
                    all_nodes.push_back(p.first);
                }
            }

            std::vector<Node> isolated;
            isolated.reserve(all_nodes.size() / 10); // grobe Schätzung

            for (Node n : all_nodes)
            {
                adjacency_set outgoing = get_right(n);
                adjacency_set incoming = get_left(n);

                if (outgoing.empty() && incoming.empty())
                {
                    isolated.push_back(n);
                }
            }

            for (Node n : isolated)
            {
                remove(n);
                ++removed_count;
            }
        }

        bool exists(Node a) const
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
                Node             hash;
                std::shared_lock lock3(_mtx_weights);
                auto             it = find_weight(a, b, hash);
                return it == _weights.end() ? 1 : static_cast<long double>(it->second);
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
            note_created(_last);
            return _last;
        }

        Node count() const
        {
            std::shared_lock<std::shared_mutex> lock_left(_smtx_left);
            return _left.size();
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
            note_created(a);
        }

        static inline Node mix_bits(Node seed, Node value)
        {
#ifdef __EMSCRIPTEN__
            // 32-bit analog of the 64-bit path below.
            // Scramble value to avoid collisions of sequential IDs
            // (MurmurHash3 32-bit finalizer, "fmix32")
            value ^= value >> 16;
            value *= 0x85ebca6bu;
            value ^= value >> 13;
            value *= 0xc2b2ae35u;
            value ^= value >> 16;

            // Boost hash_combine 32-bit (using standard shifts 6 and 2)
            seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            return seed;
#else
            // Scramble value to avoid collisions of sequential IDs
            // (MurmurHash3 64-bit finalizer)
            value ^= value >> 33;
            value *= 0xff51afd7ed558ccdULL;
            value ^= value >> 33;
            value *= 0xc4ceb9fe1a85ec53ULL;
            value ^= value >> 33;

            // Boost hash_combine 64-bit (using standard shifts 6 and 2)
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
            return seed;
#endif
        }

        static Node create_hash(const Node a, const Node b)
        {
            Node h = 0;
            h      = mix_bits(h, mod(a));
            h      = mix_bits(h, mod(b));

            return (h & mask_node) | mark_hash;
        }

        static Node create_hash(const Node predicate, const Node subject, const Node object)
        {
            Node h = 0;
            h      = mix_bits(h, 1);              // size of object set
            h      = mix_bits(h, mod(object));    // only object
            h      = (h & mask_node) | mark_hash; // match create_hash(adjacency_set) return value
            h      = mix_bits(h, mod(predicate)); // head1
            h      = mix_bits(h, mod(subject));   // head2
            return (h & mask_node) | mark_hash;
        }

        static Node create_hash(const adjacency_set& vec)
        {
            std::vector<Node> sorted_vec(vec.begin(), vec.end());
            std::sort(sorted_vec.begin(), sorted_vec.end());

            Node h = 0;
            h      = mix_bits(h, sorted_vec.size());

            for (Node node : sorted_vec)
            {
                h = mix_bits(h, mod(node));
            }
            return (h & mask_node) | mark_hash;
        }

        static Node create_hash(const Node head, const adjacency_set& vec)
        {
            Node vec_hash = create_hash(vec);
            Node h        = mix_bits(vec_hash, mod(head));

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

        // Size-only counterpart of snapshot_left_of: the number of incoming
        // edges of b (for a predicate node: the number of facts using it as
        // relation type) WITHOUT copying the adjacency set. Used for
        // cardinality heuristics such as condition ordering.
        size_t left_count_of(Node b) const
        {
            std::shared_lock<std::shared_mutex> lock(_smtx_right);
            auto                                it = _right.find(b);
            return it == _right.end() ? 0 : it->second.size();
        }

        size_t right_count_of(Node b) const
        {
            std::shared_lock<std::shared_mutex> lock(_smtx_left);
            auto                                it = _left.find(b);
            return it == _left.end() ? 0 : it->second.size();
        }

        // get predecessors / incoming edges
        adjacency_set get_left(const Node b) const
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
        adjacency_set get_right(const Node b) const
        {
            std::shared_lock<std::shared_mutex> lock(_smtx_left);
            auto                                it = _left.find(b);
            if (it == _left.end())
            {
                return {};
            }
            return it->second;
        }

        // --- Raw edge weights (neural substrate) ---

        // Set the weight of the directed edge a -> b. The edge must already exist.
        // No range constraints and no contradiction checks apply here; fact
        // probabilities are a constrained view of this same store, this is the
        // raw access path.
        void set_edge_weight(Node a, Node b, double w)
        {
            {
                std::shared_lock<std::shared_mutex> lock(_smtx_left);
                auto                                it = _left.find(a);
                if (it == _left.end() || it->second.count(b) == 0)
                {
                    throw std::runtime_error("Network::set_edge_weight: edge " + std::to_string(a) + " -> " + std::to_string(b) + " does not exist");
                }
            }

            std::unique_lock lock(_mtx_weights);
            _weights[create_hash(a, b)] = w;
        }

        // Weight of the directed edge a -> b; `fallback` if no entry exists.
        // The canonical fallback is 1 (mirroring probability semantics).
        double edge_weight(Node a, Node b, double fallback = 1.0) const
        {
            std::shared_lock lock(_mtx_weights);
            auto             it = _weights.find(create_hash(a, b));
            return it == _weights.end() ? fallback : it->second;
        }

        // --- Node clusters (named workspaces) ---
        //
        // A cluster records the IDs of nodes created while it is active:
        // sequential nodes (create), relation/hash nodes materialized by
        // fact() (create(Node)), and trusted-import relations. Facts that
        // already existed are NOT recorded, so dropping a cluster can never
        // destroy pre-existing knowledge. Node IDs are never altered;
        // membership is a side table, so nodes outside any cluster cost
        // nothing. Variables are never tracked. Clusters are not yet
        // persisted by save_to_file.

        void set_active_cluster(const std::string& name)
        {
            std::lock_guard lock(_mtx_clusters);
            _active_cluster.store(&_clusters[name], std::memory_order_release);
            _active_cluster_name = name;
        }

        void deactivate_cluster()
        {
            std::lock_guard lock(_mtx_clusters);
            _active_cluster.store(nullptr, std::memory_order_release);
            _active_cluster_name.clear();
        }

        std::string active_cluster_name() const
        {
            std::lock_guard lock(_mtx_clusters);
            return _active_cluster_name;
        }

        std::vector<std::pair<std::string, size_t>> list_clusters() const
        {
            std::lock_guard                             lock(_mtx_clusters);
            std::vector<std::pair<std::string, size_t>> out;
            out.reserve(_clusters.size());
            for (const auto& [name, nodes] : _clusters)
                out.emplace_back(name, nodes.size());
            return out;
        }

        // Removes the bookkeeping and hands the node list to the caller
        // (Zelph::drop_cluster removes the nodes themselves). Deactivates
        // the cluster if it was active. Empty result if the name is unknown.
        std::vector<Node> take_cluster(const std::string& name)
        {
            std::lock_guard lock(_mtx_clusters);
            auto            it = _clusters.find(name);
            if (it == _clusters.end()) return {};
            std::vector<Node> nodes(it->second.begin(), it->second.end());
            if (_active_cluster.load(std::memory_order_acquire) == &it->second)
            {
                _active_cluster.store(nullptr, std::memory_order_release);
                _active_cluster_name.clear();
            }
            _clusters.erase(it);
            return nodes;
        }

        // Set union, then erases `from`. to == "" merges into the default
        // cluster: the bookkeeping is dropped, the nodes become ordinary
        // nodes. No edges are touched in either case.
        bool merge_cluster(const std::string& from, const std::string& to)
        {
            std::lock_guard lock(_mtx_clusters);
            auto            it = _clusters.find(from);
            if (it == _clusters.end() || from == to) return it != _clusters.end() && from == to;
            if (!to.empty())
            {
                auto& target = _clusters[to];
                for (Node n : it->second)
                    target.insert(n);
            }
            if (_active_cluster.load(std::memory_order_acquire) == &it->second)
            {
                _active_cluster.store(nullptr, std::memory_order_release);
                _active_cluster_name.clear();
            }
            _clusters.erase(it);
            return true;
        }

#ifdef NDEBUG
    protected:
#endif
        adjacency_map _left;
        adjacency_map _right;

        // Sparse edge-weight store, keyed by create_hash(a, b) of a directed edge.
        // Two interpretations share this store:
        //   - fact probabilities (edge: relation -> predicate), range [0, 1],
        //     absent entry == 1. This is the historical "probability" semantics;
        //     it is enforced by connect() / probability(), not by the store itself.
        //   - neural synapse weights (edge: neuron -> neuron), unconstrained,
        //     absent entry == 1. Accessed via set_edge_weight() / edge_weight().
        // Nodes and edges that carry no weight cost nothing here.
        ankerl::unordered_dense::map<Node, double> _weights;

        Node                                                      _last{Node()};
        Node                                                      _last_var{Node()};
        std::map<std::string, ankerl::unordered_dense::set<Node>> _clusters;
        std::atomic<ankerl::unordered_dense::set<Node>*>          _active_cluster{nullptr};
        std::string                                               _active_cluster_name;

        mutable std::mutex        _mtx_clusters;
        mutable std::shared_mutex _mtx_weights;
        mutable std::shared_mutex _smtx_left;
        mutable std::shared_mutex _smtx_right;

#ifdef NDEBUG
    private:
#endif
        static constexpr Node shift_inc = 5;
#ifdef __EMSCRIPTEN__
        static constexpr Node mark_hash           = 0x40000000u;
        static constexpr Node mask_node           = 0x7FFFFFFFu; // mask highest bit
        static constexpr Node mask_highest_2_bits = 0x3fffffffu;
#else
        static constexpr Node mark_hash           = 0x4000000000000000ull;
        static constexpr Node mask_node           = 0x7FFFFFFFFFFFFFFFull; // mask highest bit
        static constexpr Node mask_highest_2_bits = 0x3fffffffffffffffull;
#endif

        // Called from all three node-materialization paths. Lock order is
        // always adjacency locks -> _mtx_clusters, never the reverse.
        void note_created(Node n)
        {
            if (_active_cluster.load(std::memory_order_acquire) == nullptr) return; // fast path
            std::lock_guard lock(_mtx_clusters);
            if (auto* c = _active_cluster.load(std::memory_order_acquire)) c->insert(n);
        }

        typename decltype(_weights)::iterator find_weight(Node a, Node b, Node& hash)
        {
            hash = create_hash(a, b);
            return _weights.find(hash);
        }

#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 4146)
#endif
        static inline Node rol(const Node n, Node c = 1)
        {
            constexpr Node mask = 8 * sizeof(n) - 1;
            c &= mask;
            // Node wrapped = n >> ((-c)&mask);
            // Wrap below the two reserved top bits (mask_node clears the var
            // bit, mark_hash occupies the bit below): the rotation wraps at
            // bit 61 on 64-bit and at bit 29 on the 32-bit wasm build.
            Node wrapped = (n & mask_highest_2_bits) >> (((-c) & mask) - 2);
            return ((n << c) | wrapped) & mask_highest_2_bits;
        }

        static inline Node ror(const Node n, Node c = 1)
        {
            constexpr Node mask = 8 * sizeof(n) - 1;
            c &= mask;
            return (n >> c) | (n << ((-c) & mask));
        }
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

        static Node mod(Node n)
        {
            // We generate nodes both by counting up and down (for vars) from 0, which increases probability of hash collisions.
            // So, make a clear difference between those two categories.
            // Rotate by half the node width: 32 on 64-bit, 16 on the wasm32 build.
            return n > mask_node ? ror(n, 4 * sizeof(Node)) : n;
        }
    };
}
