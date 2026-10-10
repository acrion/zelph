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

#ifndef __EMSCRIPTEN__
    #include "io/zelph.capnp.h"
    #include "manifest_loader.hpp"

    #include <capnp/message.h>
    #include <capnp/serialize-packed.h>
    #include <kj/io.h>
#endif

#include "network.hpp"
#include "zelph.hpp"

#include <ankerl/unordered_dense.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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

    using IndexPair = std::pair<Node, Node>; // (subject, object)
    static_assert(sizeof(IndexPair) == 2 * sizeof(Node), "IndexPair must be tightly packed");

    class ZELPH_EXPORT Zelph::Impl : public Network
    {
        friend class Zelph;

        explicit Impl(const io::OutputHandler& output)
            : _output(output)
        {
        }

        // --- Implemented in zelph_persistence.cpp ---

#ifndef __EMSCRIPTEN__
        std::string        pidx_path(const Node predicate) const;
        bool               try_load_pidx(const Node predicate, std::vector<IndexPair>& out) const;
        void               try_save_pidx(const Node predicate, const std::vector<IndexPair>& pairs) const;
        static void        validate_chunk_selector(const detail::chunk_selector& selection, uint32_t chunkCount, const char* label);
        void               clear_loaded_state();
        void               loadSmallData(const ZelphImpl::Reader& impl);
        void               loadLeftRightChunks(kj::BufferedInputStreamWrapper& bufferedInput, const ::capnp::ReaderOptions& options, uint32_t leftChunkCount, uint32_t rightChunkCount, const detail::chunk_selector* leftSelection = nullptr, const detail::chunk_selector* rightSelection = nullptr);
        void               loadNameOfNodeChunks(kj::BufferedInputStreamWrapper& bufferedInput, const ::capnp::ReaderOptions& options, uint32_t nameOfNodeChunkCount, const detail::chunk_selector* selection = nullptr);
        void               loadNodeOfNameChunks(kj::BufferedInputStreamWrapper& bufferedInput, const ::capnp::ReaderOptions& options, uint32_t nodeOfNameChunkCount, const detail::chunk_selector* selection = nullptr);
        void               loadLeftRightChunkFromPath(const std::string& source_path, uint64_t source_offset, const detail::chunk_selector* selection, const char* which_name, uint32_t section_count);
        void               loadNameOfNodeChunkFromPath(const std::string& source_path, uint64_t source_offset, const detail::chunk_selector* selection);
        void               loadNodeOfNameChunkFromPath(const std::string& source_path, uint64_t source_offset, const detail::chunk_selector* selection);
        void               loadFromManifest(const std::string& manifest_path, const Zelph::BinChunkSelection& selection, const std::string& shard_root, const std::string& bin_path_hint, const bool skip_payload);
        void               saveToFile(const std::string& filename, const ankerl::unordered_dense::set<Node>* const keep = nullptr) const;
        static std::string read_error_text(const std::string& filename, const kj::Exception& e, const bool state_discarded);
        bool               loadFromFile(const std::string& filename); // if the file was written using template ids
        void               loadFromFile(const std::string& filename, const Zelph::BinChunkSelection& selection, const bool skip_payload);
#endif // __EMSCRIPTEN__

        // --- Implemented in zelph_names.cpp ---

        void   transfer_names_locked(const Node from, const Node into);
        void   transfer_names(const Node from, const Node into);
        void   assign_name_locked(const Node node, const std::string& name, const std::string& lang);
        void   remove_name_locked(const Node node, const std::string& lang);
        size_t cleanup_dangling_names();
        void   remove_names_of(const adjacency_set& dead);
        void   remove_node_names(Node nd);

        // --- Implemented in zelph_index.cpp ---

        static adjacency_set                         fact_objects_of(const adjacency_set& rel_left, const adjacency_set& rel_right);
        static bool                                  is_fact_subject(const Node rel, const Node predicate, const Node cand, const adjacency_set& rel_left, const adjacency_set& rel_right, const adjacency_set& objects);
        static unsigned int                          index_build_threads();
        std::vector<IndexPair>                       extract_predicate_pairs(const Node predicate, const adjacency_set* skip) const;
        static std::shared_ptr<const PredicateIndex> index_from_pairs(std::vector<IndexPair>& fw);
        std::shared_ptr<const PredicateIndex>        predicate_index(const Node predicate, const adjacency_set* skip) const;
        bool                                         try_indexed_fact_lookup(Node predicate, Node node, bool forward, adjacency_set& out) const;
        void                                         invalidate_predicate_index() const noexcept;
        void                                         invalidate_relation_type_set() const;
        // `stop_at`: end the walk upon arriving at this node. `via`: for each
        // node that is reached, the fact it was reached by and the node it
        // came from. `skip2`: further facts to bypass during traversal.
        // `scanned`: the count of adjacency entries the walk has read.
        bool try_transitive_direct(Node start, Node predicate, bool include_start, bool forward, size_t scan_budget, const adjacency_set* skip, adjacency_set& result, Node stop_at = 0, ankerl::unordered_dense::map<Node, std::pair<Node, Node>>* via = nullptr, const adjacency_set* skip2 = nullptr, size_t* scanned = nullptr) const;

        void emit(io::OutputChannel channel, const std::string& text, bool newline = true, bool finding = false) const
        {
            if (_output)
                _output(io::OutputEvent{channel, text, newline, finding});
        }

        using name_of_node_map = ankerl::unordered_dense::map<Node, std::string_view>;
        using node_of_name_map = ankerl::unordered_dense::map<std::string_view, Node>;

        StringPool _string_pool;

        ankerl::unordered_dense::map<std::string, name_of_node_map> _name_of_node; // key is language identifier
        ankerl::unordered_dense::map<std::string, node_of_name_map> _node_of_name; // key is language identifier

        mutable std::shared_mutex _mtx_node_of_name;
        mutable std::shared_mutex _mtx_name_of_node;

        // How often `_node_of_name` was walked end to end. It is the only way
        // to find the reverse entries of a node, and doing it PER NODE is what
        // made a full-dump prune take two months; a bulk removal owes one walk
        // per batch. Hardware-independent, so a test can hold the shape
        // without a quiet machine -- see Impl::remove_names_of.
        mutable std::atomic<uint64_t> _name_map_scans{0};

        // Bulk-import triples where the node was already present but
        // contained a distinct statement (refer to
        // Zelph::fact_import_trusted_single_object), counted throughout the
        // engine's lifetime, and that count at the moment the current import
        // began (Zelph::begin_bulk_import). Each engine maintains two
        // counters; these do not grow with the size of the graph.
        mutable std::atomic<uint64_t> _import_conflicts{0};
        mutable std::atomic<uint64_t> _import_conflicts_at_begin{0};

        mutable std::recursive_mutex _mtx_print;

        mutable std::shared_mutex                                    _fs_cache_mtx;
        mutable ankerl::unordered_dense::map<Node, FactStructurePtr> _fs_cache;
        mutable std::atomic<bool>                                    _fs_cache_has_entries{false};

        // Memoized set of declared relation types: the exact node set for
        // which check_fact(p, IsA, {RelationTypeCategory}) is_known().
        // nullptr = not built. _rel_types_gen detects invalidations racing
        // a concurrent build (built outside the lock, stored only if the
        // generation is unchanged).
        mutable std::shared_mutex                                         _rel_types_mtx;
        mutable std::shared_ptr<const ankerl::unordered_dense::set<Node>> _rel_types;
        mutable uint64_t                                                  _rel_types_gen{0};

        // Template-variable store: for each node materialized via
        // triple-level construction, the precise set of variables within its
        // genuine structural closure. Entries are present exclusively for
        // nodes possessing a non-empty set (rule-template nodes), thus,
        // while authoritative, their absence signifies
        // "provably no variables". Absorbs the former _var_closure flag set:
        // an entry var-flags its node; both var_in_closure and
        // collect_variables read this, and var_in_closure reads
        // _rule_text_vars beside it.
        // Maintained eagerly by Zelph::fact in a bottom-up manner (children
        // are established before parents; a node ID is its triple hash,
        // making sets definitive upon creation). Covers every node fact()
        // creates, subject == predicate facts included, and aligns with the
        // reconstruction walk, which also consults these facts. Disarmed AND
        // cleared solely by the wholesale funnel
        // (invalidate_fact_structures_cache).
        mutable std::shared_mutex                                                           _template_vars_mtx;
        ankerl::unordered_dense::map<Node, std::shared_ptr<const std::unordered_set<Node>>> _template_vars;
        std::atomic<bool>                                                                   _template_vars_authoritative{true};

        // Facts whose rule text holds a variable invisible to the store above:
        // a `=>` fact where the conditions hold one while the consequence
        // holds none -- the conditions are attached to a container, a
        // collection, or a set constant, and the structural closure does not
        // enter its members --, a `=>` fact whose text states a membership
        // within a rule's own collection where the members hold one, a fact or
        // a set constant over such a collection, and any fact constructed upon
        // such a node (a mention, a membership). var_in_closure examines this
        // beside the store; collect_variables does not, nor does
        // var_in_closure when requested with VariableReading::Instance: a
        // firing keeps a rule's condition set and performs no substitution
        // within it, replacing a rule's own collection with a collection of
        // its own. Zelph::fact and Zelph::set enter a node upon its creation.
        // No member is written into a rule's own collection or conjunction set
        // after its rule has been written (refer to the refusal in
        // Zelph::fact); a tag applied to a container afterwards, and a member
        // written into another rule's collection while the template scope
        // remains open, are observed solely by the reconstruction walk. One
        // entry per such rule and per fact written over one, including entries
        // from a firing, under the protection of _template_vars_mtx, and
        // cleared alongside the store.
        ankerl::unordered_dense::set<Node> _rule_text_vars;
        std::atomic<bool>                  _rule_text_vars_any{false}; // read before acquiring the mutex on the fact path

        // Ground rule patterns, i.e. fact nodes that exist only because a
        // rule was written and that nobody ever claimed. See the block
        // around Zelph::is_rule_pattern. This is an INDEX, not the record:
        // the record is a fact in the graph, so a load can rebuild it and a
        // save carries it. One entry per marked pattern, which is bounded by
        // the number of rules with ground patterns -- it does not grow with
        // the graph, so the memory budget does not apply and there is
        // nothing to switch off.
        mutable std::shared_mutex _rule_patterns_mtx;
        std::unordered_set<Node>  _rule_patterns;
        // Read before the mutex on both hot paths -- the per-candidate test in
        // unification and the per-known-fact revocation in deduce. A graph
        // whose rules all carry variables, which includes every bulk import,
        // never marks anything and therefore never takes a lock at all.
        std::atomic<bool> _has_rule_patterns{false};

        // The template scope (see Zelph::enter_template_scope), for every
        // thread holding it open on this network: its depth, and the
        // collections written during its open state, which zelph/rule-text
        // reads. The writers of a rule's text open it: zelph/dedup-rule,
        // zelph/build-rule, and zelph/rule-text on the script thread, along
        // with a construction (Reasoning::rebuild_rule) on the thread
        // executing it. A collection created by another thread in parallel --
        // for instance, a Janet thread writing data while the script thread
        // writes a rule -- does not belong to that rule, so the scope remains
        // tied to the opening thread and the network: two engines within a
        // single process do not share it. An entry persists as long as its
        // depth exceeds zero; the count of entries is read before acquiring
        // the mutex, so a fact written when no rule is being written takes no
        // lock.
        struct TemplateScopeState
        {
            int               depth{0};
            std::vector<Node> collections;
        };
        mutable std::mutex                                      _template_scopes_mtx;
        std::unordered_map<std::thread::id, TemplateScopeState> _template_scopes;
        std::atomic<int>                                        _template_scopes_open{0};

        // Whether the thread that initiated the call maintains the
        // template scope as open.
        bool template_scope_open() const
        {
            if (_template_scopes_open.load(std::memory_order_acquire) == 0) return false;
            std::lock_guard lock(_template_scopes_mtx);
            return _template_scopes.count(std::this_thread::get_id()) != 0;
        }

        // The fingerprint index of the rules (see Zelph::find_equivalent_rule):
        // fingerprint -> rule, and rule -> fingerprint. One entry per `=>`
        // fact, thus rule-scale. The queue holds the `=>` facts that were
        // written since the last lookup, which fingerprints them first. Stale:
        // the next lookup clears the index and re-fingerprints each rule.
        std::mutex                                 _rule_index_mtx;
        std::unordered_multimap<std::size_t, Node> _rule_index;
        std::unordered_map<Node, std::size_t>      _rule_fingerprints;
        std::vector<Node>                          _rule_index_queue;
        std::atomic<bool>                          _rule_index_stale{true};

        // The constructions remembered (see Zelph::claimed_rule): the
        // construction's statement and recipe key -> the rule it returned,
        // built, or claimed. One entry for each unique construction, thus
        // rule-scale. Cleared upon detecting the fingerprint index as
        // stale; guarded by _rule_index_mtx.
        std::map<std::pair<Node, Node>, Node> _claims;

        // Facts the graph holds as known-WRONG, which is what `¬(F)` says
        // outside a rule condition. Structurally they are ordinary facts --
        // they have to be, since a fact's probability rides on its edge to its
        // predicate -- so nothing but this set keeps them from answering a
        // positive query as though they held.
        //
        // Same shape and the same reasoning as _rule_patterns beside it: the
        // atomic is read first on the per-candidate path, so a graph that
        // refutes nothing, which is every graph built before this existed and
        // every bulk import, never takes the lock.
        mutable std::shared_mutex _refuted_facts_mtx;
        std::unordered_set<Node>  _refuted_facts;
        std::atomic<bool>         _has_refuted_facts{false};

        // Genuine-structure store: the exact (subject, predicate, objects)
        // triple of every node materialized through triple-level
        // construction (Zelph::fact), stored at creation as an immutable
        // single-structure list. A node's ID is its triple hash, so an
        // entry can never go stale through graph GROWTH -- unlike walked
        // reconstructions, whose candidate readings evolve with the
        // neighborhood (the reason the fs_cache needs invalidation at
        // all). Disarmed and cleared ONLY by the wholesale funnel
        // (invalidate_fact_structures_cache): trusted imports, binary
        // loads, removals, merges. Growth-only clears never touch it.
        mutable std::shared_mutex                                    _genuine_mtx;
        mutable ankerl::unordered_dense::map<Node, FactStructurePtr> _genuine;
        std::atomic<bool>                                            _genuine_authoritative{true};

        mutable std::shared_mutex                                                         _pred_idx_mtx;
        mutable ankerl::unordered_dense::map<Node, std::shared_ptr<const PredicateIndex>> _pred_idx_cache;
        mutable std::atomic<bool>                                                         _pred_idx_has_entries{false};

        std::string               _pidx_base;
        Node                      _pidx_node_count{0};
        Node                      _pidx_last{0};
        Node                      _pidx_last_var{0};
        mutable std::atomic<bool> _pidx_io_enabled{false};

        // Set during a whole-file load that detected rules previously
        // stored by an older engine
        // (Zelph::rules_written_before_template_ids). A save keeps their
        // counter collections, thereby maintaining the template-id field
        // as false, causing the next load of the file to re-read the
        // rules. Cleared by clear_loaded_state, which discards the network
        // where they were discovered; the view a partial or manifest load
        // puts in its place is not subjected to scanning.
        // Zelph::remove_rules prompts a re-scan; a rule eliminated through
        // alternative means leaves it set until the next load of the saved
        // file reads the rules again.
        bool _older_rules{false};

        int               _max_log_depth{0};
        bool              _logging{false};
        io::OutputHandler _output;
    };
}
