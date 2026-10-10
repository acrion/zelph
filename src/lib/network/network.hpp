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
#include <functional>
#include <limits>
#include <memory>
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

        // The `foreign` flag is set when the node associated with the triple
        // is already present in the system yet does not hold the triple;
        // refer to the comment accompanying the check.
        Node insert_fact_single_object_trusted(Node subject, Node predicate, Node object, bool& foreign)
        {
            foreign = false;

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
                // The node is present. Typically, it is precisely this same
                // triple once more: the Wikidata importer types a property
                // once per thread, and a dump might duplicate a claim. Any
                // other case is a node that merely shares the triple's
                // identifier -- a partially loaded view, a damaged file, or a
                // collision arising from the 62-bit hash -- and modifying it
                // would merge two statements. It is not altered, and the
                // caller is informed, enabling a prolonged import to tally
                // such nodes rather than terminate upon encountering one.
                //
                // The test functions as a superset test, similar to
                // check_fact: the node's adjacency also holds the facts that
                // mention it, meaning the subject and predicate edges, along
                // with the object edge, are looked up individually, rather
                // than being compared as whole sets.
                const auto to = _right.find(relation);
                foreign       = rel_left_it->second.count(subject) == 0
                             || rel_left_it->second.count(predicate) == 0
                             || to == _right.end()
                             || to->second.count(object) == 0;
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
            if (_watched)
            {
                note_removal(a);
                note_removal(b);
            }

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

        // ONE exclusive lock triple for the whole removal, and no copies.
        //
        // This called disconnect() per edge, and disconnect takes both
        // adjacency locks plus the weight lock every time -- so a node of
        // degree D cost 2D+2 exclusive acquisitions where two suffice. It also
        // COPIED both adjacency sets first, and had to: disconnect erases from
        // the very set the loop walks. On a graph that lives in swap a copy is
        // the worst thing there is, since it touches every byte of the source
        // and then writes a second one.
        //
        // Walking the live sets is safe because neither loop writes the map it
        // reads: erasing `node` from a NEIGHBOUR's entry touches the OTHER
        // map, and no entry is erased from `_left`/`_right` themselves until
        // both loops are done. A self-loop needs no special case for the same
        // reason -- `_left[node]` and `_right[node]` go wholesale at the end.
        //
        // The lock order is disconnect's own (left, right, weights), so the
        // two remain interchangeable for any caller holding neither.
        void remove(Node node)
        {
            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);
            std::unique_lock                    lock_weights(_mtx_weights);
            const bool                          watching = _watched != nullptr;
            if (watching) note_removal(node);

            Node hash = 0;

            const auto drop_weight = [&](const Node a, const Node b)
            {
                const auto it = find_weight(a, b, hash);
                if (it != _weights.end()) _weights.erase(it);
            };

            // Incoming: every `from` that has `node` among its outgoing edges.
            const auto incoming = _right.find(node);
            if (incoming != _right.end())
            {
                for (const Node from : incoming->second)
                {
                    const auto it = _left.find(from);
                    if (it != _left.end()) it->second.erase(node);
                    drop_weight(from, node);
                    if (watching) note_removal(from);
                }
            }

            // Outgoing: every `to` that has `node` among its incoming edges.
            const auto outgoing = _left.find(node);
            if (outgoing != _left.end())
            {
                for (const Node to : outgoing->second)
                {
                    const auto it = _right.find(to);
                    if (it != _right.end()) it->second.erase(node);
                    drop_weight(node, to);
                    if (watching) note_removal(to);
                }
            }

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

        // `is_protected` marks nodes that stay even when nothing points at
        // them. The engine's own core nodes are such nodes: several of them
        // (the contradiction marker, nil, the conjunction and negation tags)
        // carry no edges in a fresh network, so a cleanup deleted them and
        // the next rule using "!" failed with "requested node does not
        // exist". Network itself does not know which nodes those are -- that
        // is a Zelph concept, hence the predicate.
        void remove_isolated_nodes(size_t& removed_count, const std::function<bool(Node)>& is_protected = {})
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

                if (outgoing.empty() && incoming.empty()
                    && !(is_protected && is_protected(n)))
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

        // The set of a few nodes touched by a removal. An addition never
        // reduces the edge count of a node, so a node maintaining its prior
        // edge count and untouched by any removal since remains unchanged;
        // following a removal, an addition may reestablish the edge count of a
        // node that was altered. A reader names the nodes it preserves
        // information about (watch_removals), which also discards prior touch
        // records; a removal records only those of them it touches, and
        // nothing more. Absent a reader, a removal pays one pointer test.
        // `graph_epoch` tracks the number of loads, which touch nodes without
        // noting a removal (note_load).
        using WatchedNodes = ankerl::unordered_dense::set<Node>;
        void watch_removals(std::shared_ptr<const WatchedNodes> nodes)
        {
            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);
            _watched = std::move(nodes);
            _touched.clear();
        }

        // Called by every load before it writes. A load that replaces the
        // graph clears it, while a load that merges a file into the graph
        // assigns to each node the file holds the edges contained within the
        // file, possibly reducing the number of edges it previously held:
        // either way, edges vanish without a record of removal, and every
        // reader must re-read the data it keeps. Only a load that cleared the
        // graph advanced the epoch, and a rule read for an explanation before
        // a merging load was adopted afterwards whenever the edge counts it
        // had read from matched.
        void note_load()
        {
            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);
            _touched.clear();
            _graph_epoch.fetch_add(1, std::memory_order_acq_rel);
        }
        bool touched_by_removal(const Node n) const
        {
            std::shared_lock<std::shared_mutex> lock(_smtx_left);
            return _touched.count(n) != 0;
        }
        std::uint64_t graph_epoch() const { return _graph_epoch.load(std::memory_order_acquire); }

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

            // The identifiers from recipe_floor up are part of rule-built
            // collections (see is_recipe); a counter there would be read as
            // one. In practice, 2^61 creates are unattainable.
            if (_last >= recipe_floor)
            {
                throw std::logic_error("Network::create: the node counter reached the ids reserved for rule-built collections");
            }

            if (is_var(_last))
            {
                throw std::logic_error("Network::var: Exceeded maximum number of " + std::to_string(_last - 1) + " nodes.");
            }

            _left[_last]  = adjacency_set{};
            _right[_last] = adjacency_set{};
            note_created(_last);
            return _last;
        }

        // A collection written while a rule is being written: the next counter
        // value serves as its payload, encapsulated within the
        // written-template class (see is_written_template). The identifier is
        // distinct due to the uniqueness of the counter; the counter slot
        // remains unutilized, and no assumption is made about the counters
        // being consecutive. A load writes identifiers exactly as they appear
        // and takes the counter from the file, meaning that after a load
        // operation merging a file into a session, the counter may fall below
        // a template currently held by the session: a payload whose template
        // id exists is skipped.
        Node create_written_template()
        {
            std::unique_lock<std::shared_mutex> lock_left(_smtx_left);
            std::unique_lock<std::shared_mutex> lock_right(_smtx_right);

            Node id = 0;
            do
            {
                while (_left.find(++_last) != _left.end())
                    ;

                // The payload contains the 59 bits below construction_bit,
                // which would make the id a construction's recipe. As with
                // the guard in create(), 2^59 creates are out of reach.
                if (_last >= construction_bit)
                {
                    throw std::logic_error("Network::create_written_template: the node counter outgrew the payload of a written template");
                }

                id = recipe_floor | template_class | _last;
            } while (_left.find(id) != _left.end());

            _left[id]  = adjacency_set{};
            _right[id] = adjacency_set{};
            note_created(id);
            return id;
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
            note_created(_last_var);

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

        // The classes associated with a node id, determined by
        // extracting bits 63..60:
        //   000x  a counter (create): atoms, witnesses, the core nodes, and
        //         every collection that is in no class below
        //   0010  a value recipe: the term a firing builds for a template
        //   0011  the template class, a rule's own collection: bit 59 set a
        //         construction's recipe, bit 59 clear a written template
        //         (create_written_template)
        //   01xx  a hash: facts and set constants
        //   1xxx  a variable
        // A recipe is not a hash, so every reader that takes a non-hash as
        // an atom or a collection interprets it as a collection, and
        // neither a fact nor a set constant can land on one.
        static bool is_recipe(Node a)
        {
            return (a & 0xE000000000000000ull) == recipe_floor;
        }

        static bool is_value_recipe(Node a)
        {
            return (a & 0xF000000000000000ull) == recipe_floor;
        }

        static bool is_template_id(Node a)
        {
            return (a & 0xF000000000000000ull) == (recipe_floor | template_class);
        }

        static bool is_written_template(Node a)
        {
            return (a & 0xF800000000000000ull) == (recipe_floor | template_class);
        }

        // The key of a recipe: the binding of the variables the instantiated
        // statement meets, as (variable, value) pairs arranged in ascending
        // order by variable. An absence of pairs results in the fixed empty
        // key.
        static Node recipe_key(const std::vector<std::pair<Node, Node>>& sorted_pairs)
        {
            Node h = mix_bits(recipe_key_seed, sorted_pairs.size());
            for (const auto& [v, x] : sorted_pairs)
            {
                h = mix_bits(h, mod(v));
                h = mix_bits(h, mod(x));
            }
            return h;
        }

        // The identifier for what an instantiation produces for the template
        // collection `tmpl` at `key`: either a construction's recipe (59 hash
        // bits) or, in the case of a firing, a value recipe (60 hash bits).
        // When two recipes collide, they share a single node; no comparison
        // occurs between their members.
        static Node recipe_id(const Node tmpl, const Node key, const bool construction)
        {
            const Node h = mix_bits(mix_bits(recipe_id_seed, mod(tmpl)), key);
            return construction ? (h & construct_payload) | recipe_floor | template_class | construction_bit
                                : (h & value_payload) | recipe_floor;
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
            Node h = 0;
            h      = mix_bits(h, vec.size());

            if (vec.iterates_sorted())
            {
                // Empty/Single/Vector storage already iterates ascending --
                // exactly the sequence the sorted copy below would produce.
                // Object sets are almost always tiny, so this path removes
                // a heap allocation plus a sort from nearly every hash.
                for (const Node node : vec)
                    h = mix_bits(h, mod(node));
                return (h & mask_node) | mark_hash;
            }

            // Set storage (>64 elements): unspecified iteration order --
            // keep the order-normalizing copy+sort so the hash stays a pure
            // function of the element SET.
            std::vector<Node> sorted_vec(vec.begin(), vec.end());
            std::sort(sorted_vec.begin(), sorted_vec.end());
            for (const Node node : sorted_vec)
                h = mix_bits(h, mod(node));
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

        // Exact-triple edge probe for a fact node: all membership checks of
        // check_fact under ONE lock scope, on references. The former
        // implementation bound const& to the BY-VALUE returns of
        // get_right/get_left -- two full adjacency copies (allocation +
        // element copy) and several rwlock pairs per call, on one of the
        // hottest engine paths.
        bool fact_edges_hold(const Node relation, const Node subject, const adjacency_set& objects) const
        {
            // Same lock order as writers (connect): left before right.
            std::shared_lock<std::shared_mutex> lock_left(_smtx_left);
            std::shared_lock<std::shared_mutex> lock_right(_smtx_right);

            const auto lr = _left.find(relation);
            if (lr == _left.end()) return false; // relation node does not exist
            const auto rr = _right.find(relation);
            if (rr == _right.end()) return false;

            // Naming follows check_fact: "from" = edges out of the relation
            // node (_left[relation]), "to" = edges into it (_right[relation]).
            const adjacency_set& from = lr->second;
            const adjacency_set& to   = rr->second;

            if (from.count(subject) != 1 || to.count(subject) != 1) return false; // subject must be bidirectional

            for (const Node t : objects)
            {
                if (to.count(t) == 0) return false;                   // object must point to the relation
                if (t != subject && from.count(t) != 0) return false; // and must not be pointed AT (that marks subjects/predicates)
            }
            return true;
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

        // The same into a list, containing nodes whose identifiers exceed
        // `after` exclusively, arranged according to adjacency order; returns
        // how many incoming edges b has. For a reader that processes nodes in
        // a sequence of its own choosing (Unification::enumerate_by_id): a
        // copy of a set comprising over 128 items replicates its hash index,
        // which such a reader has no use for.
        size_t list_left_of(Node b, std::vector<Node>& out, Node after = 0) const
        {
            out.clear();
            std::shared_lock<std::shared_mutex> lock(_smtx_right);
            auto                                it = _right.find(b);
            if (it == _right.end()) return 0;
            for (const Node n : it->second)
                if (n > after) out.push_back(n);
            return it->second.size();
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

        // --- ReadScope: one shared lock pair for a whole read-only region ---
        //
        // Acquires shared locks on BOTH adjacency maps (left before right --
        // the writer order of connect()) and hands out REFERENCES into the
        // maps for its lifetime. Replaces sequences of get_right/get_left
        // calls that each paid a rwlock pair plus a full adjacency_set copy.
        //
        // HARD RULES for code running under a live scope:
        //  - never write to the network,
        //  - never take another network lock (no nested ReadScope),
        //  - never call the locking API: get_right/get_left/exists/
        //    check_fact/parse_relation/format/log or any output stream.
        //    std::shared_mutex shared-locking is not guaranteed reentrant,
        //    and a writer queued between two shared acquisitions deadlocks
        //    the process.
        // Pure hash arithmetic (create_hash, is_hash, is_var) and reads
        // through the scope itself are the only permitted operations.
        class ReadScope
        {
        public:
            explicit ReadScope(const Network& n)
                : _n(&n)
                , _lock_left(n._smtx_left)
                , _lock_right(n._smtx_right)
            {
            }

            ReadScope(ReadScope&&) noexcept        = default;
            ReadScope(const ReadScope&)            = delete;
            ReadScope& operator=(const ReadScope&) = delete;
            ReadScope& operator=(ReadScope&&)      = delete;

            // Successors / outgoing edges of b -- the reference counterpart
            // of Network::get_right (which reads _left; see there).
            const adjacency_set& right(const Node b) const
            {
                const auto it = _n->_left.find(b);
                return it == _n->_left.end() ? empty_set() : it->second;
            }

            // right() and exists() in ONE probe, for callers that ask both of
            // the same node -- nullptr IS "does not exist", since create()
            // gives every node a _left entry and only removal takes it away.
            // The removal cascade asked three times per candidate (exists,
            // right, and right again inside parse_relation_scoped) while
            // do_find on these maps was 54 % of its profile.
            const adjacency_set* try_right(const Node b) const
            {
                const auto it = _n->_left.find(b);
                return it == _n->_left.end() ? nullptr : &it->second;
            }

            // Predecessors / incoming edges of b (counterpart of get_left).
            const adjacency_set& left(const Node b) const
            {
                const auto it = _n->_right.find(b);
                return it == _n->_right.end() ? empty_set() : it->second;
            }

            bool exists(const Node a) const
            {
                return _n->_left.find(a) != _n->_left.end();
            }

        private:
            static const adjacency_set& empty_set()
            {
                static const adjacency_set empty;
                return empty;
            }

            // Pointer, not reference: keeps the defaulted move constructor.
            const Network*                      _n;
            std::shared_lock<std::shared_mutex> _lock_left; // declaration order IS lock order
            std::shared_lock<std::shared_mutex> _lock_right;
        };

        // --- Synapses (neural substrate) ---
        // A synapse is an entry in the weight store for a directed node
        // pair -- and nothing else. Creating one inserts NOTHING into the
        // adjacency maps. This makes synapses invisible to the reasoning
        // engine by construction rather than by filtering: the adjacency
        // of relation nodes (fact nodes, including every cons cell of a
        // structural number) IS their fact structure -- an edge into a
        // relation node carries the signature of an additional object, an
        // edge out of it the signature of a predicate link -- so an
        // extraneous adjacency entry on such a node corrupts its structure
        // and breaks the hash-consing identity guarantee (check_fact).
        // With synapses confined to the weight store, any node is a safe
        // neuron, including fact nodes and structural numbers.
        //
        // Shared-store caveat (deliberate): a synapse on a node pair that
        // also carries a real edge occupies the same store entry as that
        // edge's probability. The case that matters is a synapse from a
        // fact node to that fact's own predicate node, which aliases the
        // fact's probability.

        // True if a synapse (or an explicitly stored edge probability)
        // exists for the directed pair a -> b.
        bool has_synapse(Node a, Node b) const
        {
            std::shared_lock lock(_mtx_weights);
            return _weights.find(create_hash(a, b)) != _weights.end();
        }

        // Create or overwrite the synapse a -> b. Both nodes must exist.
        void set_synapse(Node a, Node b, double w)
        {
            {
                std::shared_lock<std::shared_mutex> lock_left(_smtx_left);
                if (_left.find(a) == _left.end())
                {
                    throw std::runtime_error("Network::set_synapse: node " + std::to_string(a) + " does not exist");
                }
            }
            {
                std::shared_lock<std::shared_mutex> lock_right(_smtx_right);
                if (_right.find(b) == _right.end())
                {
                    throw std::runtime_error("Network::set_synapse: node " + std::to_string(b) + " does not exist");
                }
            }

            std::unique_lock lock(_mtx_weights);
            _weights[create_hash(a, b)] = w;
        }

        // --- Raw edge weights (neural substrate) ---

        // Set the weight of the directed pair a -> b. Accepts existing
        // synapse entries (weight store) as well as real adjacency edges
        // (fact probabilities); rejects pairs that are neither.
        void set_edge_weight(Node a, Node b, double w)
        {
            const Node hash = create_hash(a, b);

            {
                std::unique_lock lock(_mtx_weights);
                auto             it = _weights.find(hash);
                if (it != _weights.end())
                {
                    it->second = w;
                    return;
                }
            }

            {
                std::shared_lock<std::shared_mutex> lock(_smtx_left);
                auto                                it = _left.find(a);
                if (it == _left.end() || it->second.count(b) == 0)
                {
                    throw std::runtime_error("Network::set_edge_weight: no synapse and no edge " + std::to_string(a) + " -> " + std::to_string(b));
                }
            }

            // Benign race between the two lock scopes: a concurrent
            // disconnect could remove the edge here; the write below then
            // creates a value-only entry, which nothing ever reads.
            std::unique_lock lock(_mtx_weights);
            _weights[hash] = w;
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
        // fact() (create(Node)), trusted-import relations, and variables
        // (var). Facts that already existed are NOT recorded, so dropping a
        // cluster can never destroy pre-existing knowledge. Node IDs are
        // never altered; membership is a side table, so nodes outside any
        // cluster cost nothing. Clusters are not yet persisted by
        // save_to_file.
        //
        // Variables are tracked because a rule built inside a cluster is
        // otherwise only PARTLY rolled back: its patterns disappear while
        // the variables they were made of stay behind as isolated,
        // still-named nodes. They are exactly as "created while active" as
        // any other node.

        void set_active_cluster(const std::string& name)
        {
            std::lock_guard lock(_mtx_clusters);
            _active_cluster.store(&_clusters[name], std::memory_order_release);
            _active_unmarked.store(&_cluster_unmarked[name], std::memory_order_release);
            _active_cluster_name = name;
        }

        void deactivate_cluster()
        {
            std::lock_guard lock(_mtx_clusters);
            _active_cluster.store(nullptr, std::memory_order_release);
            _active_unmarked.store(nullptr, std::memory_order_release);
            _active_cluster_name.clear();
        }

        // A cluster records what it CREATED, which is what lets a drop promise
        // never to destroy pre-existing knowledge. One change it makes to a
        // PRE-EXISTING node has to be undone all the same: asserting a
        // statement that was only a rule's ground pattern revokes that
        // marking, and the node existed, so nothing was recorded and the drop
        // undid nothing. An experiment could turn a rule's patterns into data
        // permanently -- both of them, since the ground consequence is
        // materialized with the rule.
        //
        // The line the contract draws: a marking is the ENGINE's own
        // bookkeeping about a node, not a claim anybody made. Names, merges
        // and other statements about pre-existing nodes stay outside, as
        // before.
        void note_unmarked(Node n)
        {
            if (_active_unmarked.load(std::memory_order_acquire) == nullptr) return; // fast path
            std::lock_guard lock(_mtx_clusters);
            if (auto* u = _active_unmarked.load(std::memory_order_acquire)) u->insert(n);
        }

        // The pattern markings revoked while `name` was active, without
        // touching the bookkeeping -- cluster_nodes' sibling. Read it BEFORE
        // take_cluster, which drops both sets.
        std::vector<Node> cluster_unmarked(const std::string& name) const
        {
            std::lock_guard lock(_mtx_clusters);
            const auto      it = _cluster_unmarked.find(name);
            if (it == _cluster_unmarked.end()) return {};
            return {it->second.begin(), it->second.end()};
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

        // The nodes a cluster has recorded so far, WITHOUT touching the
        // bookkeeping -- take_cluster's read-only sibling. What it answers is
        // "which of these nodes are new", because a cluster records exactly
        // what was created while it was active.
        std::vector<Node> cluster_nodes(const std::string& name) const
        {
            std::lock_guard lock(_mtx_clusters);
            const auto      it = _clusters.find(name);
            if (it == _clusters.end()) return {};
            return {it->second.begin(), it->second.end()};
        }

        // Bookkeeping only: wherever `from` was recorded, record `into`
        // instead -- or nothing at all when `into` is 0. A node that is
        // RE-CREATED under a different id is the same knowledge under a new
        // name, so it has to inherit the answer to "was this new?"; recording
        // it afresh instead would put a repaired PRE-EXISTING fact into the
        // active cluster, and dropping that cluster would then destroy it.
        void retag_cluster_member(const Node from, const Node into)
        {
            std::lock_guard lock(_mtx_clusters);
            for (auto& [name, nodes] : _clusters)
            {
                if (nodes.erase(from) == 0) continue;
                if (into != 0) nodes.insert(into);
            }
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
                _active_unmarked.store(nullptr, std::memory_order_release);
                _active_cluster_name.clear();
            }
            _clusters.erase(it);
            _cluster_unmarked.erase(name);
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

                // A merge commits the bookkeeping, so the revocations travel
                // with the nodes: merging into `default` (to == "") drops both
                // and turns everything into ordinary state, which is what
                // committing an experiment means.
                if (const auto u = _cluster_unmarked.find(from); u != _cluster_unmarked.end())
                {
                    auto& target_unmarked = _cluster_unmarked[to];
                    for (Node n : u->second)
                        target_unmarked.insert(n);
                }
            }
            if (_active_cluster.load(std::memory_order_acquire) == &it->second)
            {
                _active_cluster.store(nullptr, std::memory_order_release);
                _active_unmarked.store(nullptr, std::memory_order_release);
                _active_cluster_name.clear();
            }
            _clusters.erase(it);
            _cluster_unmarked.erase(from);
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
        std::map<std::string, ankerl::unordered_dense::set<Node>> _cluster_unmarked;
        std::atomic<ankerl::unordered_dense::set<Node>*>          _active_cluster{nullptr};
        std::atomic<ankerl::unordered_dense::set<Node>*>          _active_unmarked{nullptr};
        std::string                                               _active_cluster_name;

        mutable std::mutex        _mtx_clusters;
        mutable std::shared_mutex _mtx_weights;
        mutable std::shared_mutex _smtx_left;
        mutable std::shared_mutex _smtx_right;

        // See watch_removals(). Written while exclusively holding the
        // adjacency locks; `_touched` contains only watched nodes, so
        // neither grows with the graph.
        std::shared_ptr<const WatchedNodes> _watched;
        WatchedNodes                        _touched;
        std::atomic<std::uint64_t>          _graph_epoch{0};
        void                                note_removal(const Node n)
        {
            if (_watched->count(n) != 0) _touched.insert(n);
        }

#ifdef NDEBUG
    private:
#endif
        static constexpr Node shift_inc           = 5;
        static constexpr Node mark_hash           = 0x4000000000000000ull;
        static constexpr Node mask_node           = 0x7FFFFFFFFFFFFFFFull; // mask highest bit
        static constexpr Node mask_highest_2_bits = 0x3fffffffffffffffull;

        // The identifier classes of collections built by rules, the top of
        // the counter range (see is_recipe).
        static constexpr Node recipe_floor      = 0x2000000000000000ull; // bit 61: the recipe classes
        static constexpr Node template_class    = 0x1000000000000000ull; // bit 60: a rule's own collection
        static constexpr Node construction_bit  = 0x0800000000000000ull; // bit 59: created by a construction
        static constexpr Node value_payload     = 0x0FFFFFFFFFFFFFFFull; // the 60 hash bits of a value recipe
        static constexpr Node construct_payload = 0x07FFFFFFFFFFFFFFull; // the 59 hash bits of a construction's recipe
        static constexpr Node recipe_key_seed   = 0x6b65793a72656369ull;
        static constexpr Node recipe_id_seed    = 0x7265636970653a31ull;

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
            // bit 61.
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
            // Rotate by half the node width, i.e. by 32.
            return n > mask_node ? ror(n, 4 * sizeof(Node)) : n;
        }
    };
}
