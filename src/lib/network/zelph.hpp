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

#include "answer.hpp"
#include "fact_structure_types.hpp"
#include "io/output.hpp"
#include "network.hpp"

#include <zelph_export.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    using name_of_node_map = ankerl::unordered_dense::map<Node, std::string_view>;
    using node_of_name_map = ankerl::unordered_dense::map<std::string_view, Node>;

    // --- Script-registered display schemes -------------------------------
    //
    // A scheme lets a script declare HOW its own notation is written, so
    // node_to_string can render terms the way the script's parser reads
    // them back. C++ knows the mechanism only; every value in here comes
    // from a script. Without a registration the tables stay empty and
    // nothing about the display changes.
    //
    // The wrapper (open/close) is emitted exclusively at locations where
    // the rendering genuinely differs from the output of the default
    // renderer -- omitted parentheses, an alternate numeral prefix, or call
    // notation, which the default renderer never produces. Both strings are
    // output exactly as written, meaning a scheme wanting padding registers
    // "$( " and " )".
    struct DisplayScheme
    {
        std::string name;
        std::string open;
        std::string close;
        std::string numeral_prefix{"&"}; // replaces the default "&" inside the scheme
        std::string name_first;          // characters a leaf name may START with
        std::string name_chars;          // characters a leaf name may consist of
    };

    struct OperatorDisplay
    {
        // How a fact (S P O) is written in the scheme.
        enum class Form
        {
            Infix,      // "S P O" -- parenthesized according to precedence
            Application // "S(O)"  -- self-delimiting; S must be a bare name
        };

        std::size_t scheme{0};
        Form        form{Form::Infix};
        int         precedence{0};
        int         assoc{-1}; // -1 left, 0 non-associative, +1 right
    };

    struct InfixEntry
    {
        Node predicate{0};
        int  precedence{0};
        int  assoc{-1};
    };

    struct DisplayTables
    {
        std::vector<DisplayScheme>                schemes;
        std::unordered_map<Node, OperatorDisplay> operators;
    };

    // The core semantic network engine. It manages the in-memory graph structure (nodes, edges),
    // provides low-level API for graph manipulation, and handles raw binary serialization (I/O)
    // of the network state via load_from_file/save_to_file. It is agnostic to the semantic meaning
    // or source format of the data.
    class ZELPH_EXPORT Zelph
    {
    public:
        struct BinChunkSelection
        {
            std::vector<uint32_t> left;
            std::vector<uint32_t> right;
            std::vector<uint32_t> nameOfNode;
            std::vector<uint32_t> nodeOfName;
            std::vector<uint64_t> route_nodes;
            std::string           route_name;
            std::string           route_lang;
            bool                  left_explicit         = false;
            bool                  right_explicit        = false;
            bool                  name_of_node_explicit = false;
            bool                  node_of_name_explicit = false;
            bool                  route_nodes_explicit  = false;
            bool                  route_name_explicit   = false;
        };

        explicit Zelph(const io::OutputHandler& output = io::default_output_handler);
        ~Zelph();

        struct FactComponents
        {
            Node          subject   = 0;
            Node          predicate = 0;
            adjacency_set objects;
        };

        class AllNodeView
        {
        private:
            const adjacency_map& _left_ref;

        public:
            explicit AllNodeView(const adjacency_map& left) : _left_ref(left) {}
            auto begin() const { return _left_ref.begin(); }
            auto end() const { return _left_ref.end(); }
            // Usage: for (auto it = view.begin(); it != view.end(); ++it) { Node nd = it->first; }
        };

        class LangNodeView
        {
        private:
            const node_of_name_map& _rev_map;

        public:
            explicit LangNodeView(const node_of_name_map& rev) : _rev_map(rev) {}
            auto begin() const { return _rev_map.begin(); }
            auto end() const { return _rev_map.end(); }
            // Usage: for (auto it = view.begin(); it != view.end(); ++it) { Node nd = it->second; }
        };

        // --- Implemented in zelph.cpp (core graph operations) ---

        static std::string   get_version();
        Node                 var() const;
        void                 set_lang(const std::string& lang);
        std::string          get_lang() const { return _lang; }
        std::string          lang() const { return _lang; }
        Node                 node(const std::string& name, std::string lang = "");
        bool                 exists(uint64_t nd) const;
        adjacency_set        get_sources(Node relationType, Node target, bool exclude_vars = false) const;
        adjacency_set        get_fact_objects(Node subject, Node predicate) const;
        adjacency_set        get_fact_subjects(Node predicate, Node object) const;
        adjacency_set        transitive_targets(Node start, Node predicate, bool include_start) const;
        adjacency_set        transitive_sources(Node target, Node predicate, bool include_target) const;
        adjacency_set        filter(const adjacency_set& source, Node target) const;
        adjacency_set        filter(Node fact, Node relationType, Node target) const;
        static adjacency_set filter(const adjacency_set& source, const std::function<bool(const Node nd)>& f);
        adjacency_set        get_left(const Node b) const;
        adjacency_set        get_right(const Node b) const;
        adjacency_set        get_facts_of_predicate(Node relation) const;
        bool                 has_left_edge(Node b, Node a) const;
        bool                 has_right_edge(Node a, Node b) const;
        static Node          create_hash(const adjacency_set& vec);
        static Node          create_hash(const Node predicate, const Node subject, const adjacency_set& objects);
        static bool          is_hash(Node a);
        static bool          is_var(Node a);
        static bool          is_recipe(Node a);
        static bool          is_value_recipe(Node a);
        static bool          is_template_id(Node a);
        static Node          bucket_term_id(Node bucket);
        Answer               check_fact(Node subject, Node predicate, const adjacency_set& objects) const;

        // Throws whatever node() throws when encountering a name that no
        // node can carry -- the empty one -- ensuring that a caller relying
        // solely on lookup reports it alike.
        static void check_node_name(const std::string& name);

        // get_left(b).size() and get_right(b).size(), without copying the set.
        std::size_t left_count(Node b) const;
        std::size_t right_count(Node b) const;

        // Whether a removal has touched a node since the nodes were named, for
        // a few nodes named ahead of time (see Network::watch_removals): in
        // the absence of such a removal, a node's edge count indicates whether
        // it underwent a change. `graph_epoch` counts the loads, each of which
        // replaced the graph.
        void          watch_removals(std::shared_ptr<const ankerl::unordered_dense::set<Node>> nodes) const;
        bool          touched_by_removal(Node n) const;
        std::uint64_t graph_epoch() const;

        // A shortest walk from `start` to `target` via `predicate`, as the
        // facts traversed during the journey, proceeding around the facts
        // listed in `unasserted` (pass unasserted_snapshot()) and those in
        // `without`; either can be null. Returns False if no such walk
        // exists, or if the `scan_budget` adjacency entries failed to resolve
        // it (in which case `exhausted`).
        // include_start: zero steps lead to `start`, resulting in an empty
        // walk. `scanned`: the total number of adjacency entries processed
        // during the operation.
        bool transitive_walk(Node start, Node target, Node predicate, bool include_start, const adjacency_set* unasserted, const adjacency_set* without, size_t scan_budget, std::vector<Node>& edges, bool& exhausted, size_t* scanned = nullptr) const;

        // Single-argument form for callers that already hold the relation
        // node itself (e.g. .explain, whose target comes from evaluating a
        // fact pattern). Derives the predicate via predicate_of(); a node
        // that is not a readable fact node is reported as unknown.
        Answer check_fact(Node relation) const;

        // The predicate associated with a pre-existing fact node. Favours
        // the genuine-structure store (exact, O(1), no heuristics); falls
        // back to parse_relation() when a node lacks an entry -- every node
        // once a bulk path has disarmed the store, and a node fact() did not
        // create. The fallback handles subject == predicate facts too.
        // Returns 0 if nd is not a fact node or its predicate cannot be
        // ascertained without ambiguity.
        Node predicate_of(Node nd) const;

        Node fact(Node subject, Node predicate, const adjacency_set& objects, long double probability = 1);
        Node fact_import_trusted_single_object(Node subject, Node predicate, Node object) const;
        Node list(const std::vector<Node>& elements);
        Node list(const std::vector<std::string>& elements);
        // `{...}`: identified by its members (extensionality), so two
        // occurrences are ONE node and it cannot be extended.
        Node set(const std::unordered_set<Node>& elements);
        // `@{...}`: a container possessing a distinct identity, where
        // membership is asserted and capable of expanding. Within the
        // template scope, a template that has been written (see
        // enter_template_scope), outside it a counter.
        Node collection(const std::unordered_set<Node>& elements);
        // The rule's condition set: a collection with a counter id, also
        // located within the template scope. The `~ conjunction` tag
        // indicates its nature at any position it occupies, hence the id has
        // no influence, and a typed rule keeps the ids it has always held.
        Node conjunction_collection(const std::unordered_set<Node>& elements);
        // The template scope. While it remains open, collection() creates a
        // written template (Network::create_written_template), as the
        // collection forms part of the text of a rule that is being written:
        // zelph/dedup-rule opens it around the statement of a typed rule,
        // zelph/build-rule around a rule that a Janet program builds, and
        // zelph/rule-text around a rule that a different statement refers
        // to. Scopes nest, and a scope stays open only on the thread that
        // opened it: a collection created by another thread during this time
        // becomes a value, just as one created outside every scope does. A
        // rule written by a generator acquires its collections from its
        // construction instead, which gives them recipe ids of the template
        // class (Network::recipe_id).
        void enter_template_scope();
        void leave_template_scope();
        // Holds the template scope in an open state throughout its
        // existence. If an error leaves the scope open, every collection
        // written afterwards, including data, would become a rule's own.
        class TemplateScope
        {
        public:
            explicit TemplateScope(Zelph& z)
                : _z(z)
            {
                _z.enter_template_scope();
            }
            ~TemplateScope() { _z.leave_template_scope(); }
            TemplateScope(const TemplateScope&)            = delete;
            TemplateScope& operator=(const TemplateScope&) = delete;

        private:
            Zelph& _z;
        };
        // A collection whose identifier is determined by its recipe (see
        // Network::recipe_id). Reuses the node if present -- avoiding any
        // comparison of members, which ensures that a collection that
        // another rule wrote into is not rebuilt -- and asserts each
        // membership fact that is absent, just as set() does. `created` says
        // whether this invocation generated the node; `asserted`, if not
        // null, receives the membership facts it wrote. `derived`: during a
        // firing, the collection is constructed, and a membership that
        // exists as a rule pattern is claimed, as a derived fact is.
        Node recipe_collection(Node id, const std::unordered_set<Node>& members, bool& created, std::vector<Node>* asserted, bool derived = false);
        // Whether a literal over `members` cannot determine its kind: a member
        // either is a variable or contains one within its fact closure. Since
        // extensionality requires knowledge of the members, the set() function
        // transforms such a literal into a collection.
        bool kind_unknowable(const std::unordered_set<Node>& members) const;
        bool is_set_constant(Node node) const;
        // Whether `n`, encountered within a rule's text, is a collection that
        // the rule's own statement wrote -- a Skolem function symbol, which
        // gets substituted by its term during firing -- as opposed to a value
        // or a constant. The id alone indicates this: a rule's own collection
        // is created in the template class (Network::is_template_id), and no
        // subsequent writing alters an id.
        bool is_rule_template(Node n) const;
        // Whether a membership fact having `container` as its object
        // constitutes a component of a rule's text: either a template or a
        // conjunction set.
        bool holds_rule_text(Node container) const;
        // Whether the `membership` belongs to a literal that an engine
        // before template ids wrote into a rule's text under a counter id,
        // with the collection being data within this engine, though the
        // rule states the member, which is a pattern; only a load of a
        // network that such an engine saved records any
        // (literals_written_before_template_ids).
        bool is_older_rule_literal(Node membership) const;

        // A bulk importer invokes these before its first triple and after
        // its final one. During the intervening period,
        // fact_import_trusted_single_object refrains from writing a triple if
        // its node is already taken by another statement: the first such
        // triple is reported at the moment it happens, and end_bulk_import()
        // reports the total count and returns that number. A second import
        // into the same engine counts its own occurrences.
        void     begin_bulk_import();
        uint64_t end_bulk_import();

        Node parse_fact(Node rule, adjacency_set& deductions, Node parent = 0) const;
        Node parse_relation(const Node rule) const;
        // Locked-scope read access (see Network::ReadScope). Constructed
        // here because only zelph.cpp sees the complete Impl type -- this
        // is the visibility-correct path (Cap'n-Proto layering).
        Network::ReadScope read_scope() const;
        // The role of the anchor in collect_anchored_facts regarding
        // the facts it keeps: any, their subject, or one of their objects.
        enum class AnchorRole
        {
            Any,
            Subject,
            Object
        };
        std::size_t collect_anchored_facts(Node anchor, Node relation, adjacency_set& out, AnchorRole role = AnchorRole::Any, Node after = 0) const;
        // The same collected into a list, following the sequence defined by
        // the anchor's adjacency, for a reader that processes facts according
        // to its own ordering (Unification::enumerate_by_id): a set exceeding
        // 128 facts builds a hash index, which such a reader does not
        // utilize.
        std::size_t collect_anchored_facts(Node anchor, Node relation, std::vector<Node>& out, AnchorRole role = AnchorRole::Any, Node after = 0) const;

        // parse_relation for code running under a live ReadScope: all
        // adjacency reads via scope references, predicate detection via
        // the caller-provided relation-type memo. The memo MUST be fetched
        // BEFORE the scope opens (its lazy build takes network locks).
        // `outgoing`, when given, is scope.right(rule) already fetched by the
        // caller -- the removal cascade has it and used to pay a second probe
        // for it here.
        Node parse_relation_scoped(const Network::ReadScope&                 scope,
                                   const ankerl::unordered_dense::set<Node>& rel_types,
                                   Node                                      rule,
                                   const adjacency_set*                      outgoing = nullptr) const;
        Node count() const;
        // Whether the graph contains precisely the initial elements a new
        // engine starts with: the core nodes and the facts that establish
        // the core relations, lacking any name in any language and devoid of
        // a weight. A module that solely specifies Janet code preserves it
        // in that state; a statement or query that names a node does not.
        bool         holds_only_core() const;
        AllNodeView  get_all_nodes_view() const;
        LangNodeView get_lang_nodes_view(const std::string& lang) const;
        bool         try_get_fact_structures_cached(Node fact, FactStructurePtr& out) const;

        /// While one of these lives, fact structures are computed and not
        /// remembered. For a bulk pass that visits every node once, where the
        /// cache is an exclusive lock and a map growing into the millions,
        /// bought with a reuse that never comes.
        class ZELPH_EXPORT SuspendFactStructureCache
        {
        public:
            explicit SuspendFactStructureCache(const Zelph& z)
                : _z(z)
                , _previous(z._fs_cache_suspended.exchange(true, std::memory_order_relaxed))
            {
            }
            ~SuspendFactStructureCache() { _z._fs_cache_suspended.store(_previous, std::memory_order_relaxed); }

            SuspendFactStructureCache(const SuspendFactStructureCache&)            = delete;
            SuspendFactStructureCache& operator=(const SuspendFactStructureCache&) = delete;

        private:
            const Zelph& _z;
            const bool   _previous;
        };

        void store_fact_structures_cached(Node fact, FactStructurePtr value) const;
        void invalidate_fact_structures_cache() const noexcept;

        /// Is `fact` the relation-type declaration of a predicate -- `p ~ ->`?
        /// Every path that REMOVES facts has to ask, because that one fact is
        /// what makes a node readable as a fact at all: fact-structure
        /// reconstruction rejects every predicate absent from the memoized
        /// relation-type set, so removing it and leaving the set alone made
        /// the session and a reload of its own `.save` disagree about whether
        /// a rule exists.
        bool is_relation_type_declaration(Node fact) const;

        /// Drop exactly these nodes from the fact-structure cache, leaving the
        /// rest of it alone. The targeted counterpart of the wholesale clear
        /// above, for a caller that can name what its change made stale --
        /// see invalidate_fact_structures_for (creation) and remove_node
        /// (removal), which are the two that can.
        void erase_fact_structures(const std::vector<Node>& nodes) const noexcept;
        void invalidate_fact_structures_for(Node subject, Node predicate, const adjacency_set& objects, Node relation) const;

        // Memoized set of declared relation types -- one shared_ptr read
        // per fact-structure reconstruction instead of a check_fact probe
        // per right neighbor (which cost a {->}-set temporary, its sorted
        // hash, three rwlock pairs and two adjacency copies EACH -- the
        // dominant per-miss machinery in the Jacobian profiles). Built
        // lazily; invalidated by new declarations and by every wholesale
        // cache clear (removals, merges, loads).
        std::shared_ptr<const ankerl::unordered_dense::set<Node>> relation_type_set() const;

        // Fact-structure cache statistics. Populated only while logging is
        // active (like all profiler counters); dumped by .prof, zeroed by
        // .prof reset. Arbitrates the two remaining cost hypotheses after
        // the per-node invalidation change: reconstruction frequency
        // (misses) vs per-hit overhead (lock + deep copy).
        struct FsCacheStats
        {
            uint64_t hits{0};
            uint64_t misses{0};
            uint64_t full_clears{0};
            uint64_t stale_erased{0};
        };
        FsCacheStats fs_cache_stats() const;
        void         reset_fs_cache_stats() const;

        // Whether var_in_closure reads the conditions associated with a `=>`
        // fact and the members contained within a rule's own collection (see
        // there).
        enum class VariableReading
        {
            Text,     // it does: the text of a rule that holds a variable is no data
            Instance, // it does not: what a firing substitutes
        };

        // --- Variable-closure flag (rule-template detection) ---
        // True iff nd is a variable or its GENUINE structural closure
        // (subject, predicate, objects at any depth) includes one, the
        // conditions of every `=>` fact within it included, and the members of
        // every collection of a rule's own text within it (is_rule_template),
        // including when a set constant holds that collection -- but not when
        // the collection is the object of a membership, which is its content
        // and holds a variable exactly when its member does; the rule
        // asserting the membership holds the collection's. These are attached
        // to a container, a collection, or a set constant, whose members are
        // not reachable through structure; the conditions are read as
        // Reasoning::evaluate reads them, meaning a rule whose variables
        // reside solely in its conditions holds them as its one-condition twin
        // does, and a rule whose variables appear within one of its own
        // collections also holds them. A value -- a collection not owned by
        // the rule -- is never entered: what other rules wrote into it is not
        // a variable in the text that holds it. When asked about a collection
        // directly, the response is no: a collection is not a fact, and
        // `{@{Y}}` is a set constant enclosing one collection
        // (kind_unknowable), while that set constant answers yes.
        // collect_variables, which asks what a firing substitutes, reads none
        // of these members. This is the criterion separating rule-template
        // nodes from data nodes (template rejection in extract_bindings,
        // anchor eligibility, bound-pattern grounding). O(1): maintained
        // eagerly by fact() from the actual triple arguments -- hash-consing
        // generates children before parents, so child flags are finalized by
        // the time the parent is created, and a node's ID is its triple hash,
        // making the flag immutable thereafter; a rule's conditions are read
        // at the time its `=>` fact is created, and a rule's own collection is
        // read when a fact or a set constant over it is created, and either
        // again when another rule writes a variable-containing member into it
        // (see Impl's _rule_text_vars).
        // Unlike the former reconstruction-based walk, this cannot be misled
        // by ambiguous adjacency readings. Paths that bypass triple
        // construction or destroy topology clear the authoritative bit (see
        // Impl); the query then falls back to the historical walk -- never
        // unsound, never inferior to the pre-flag behaviour.
        //
        // VariableReading::Instance leaves the conditions and those members
        // out, mirroring collect_variables: it returns whether a firing
        // possesses a variable for substitution. The verification of a prior
        // witness for a fresh variable reads its candidates in the same manner
        // (Reasoning::consequences_already_exist): it must locate what a
        // firing wrote, and a firing writes the conditions of a rule it
        // mentions exactly as they are.
        bool var_in_closure(Node nd, VariableReading reading = VariableReading::Text) const;

        // Whether the text of `n` holds a variable: `n` is one, or a fact or a
        // set constant var_in_closure answers for, or a rule's own collection
        // or a conjunction set containing such a member. A value, a constant,
        // and an atom hold none. reasoning.cpp determines by it the kind of a
        // rebuilt collection and which members the term of a bucket holds.
        bool holds_variable_in_text(Node n) const;

        // Did anybody CLAIM this statement? True for an asserted or derived
        // fact, false for the two kinds of node that exist as graph structure
        // without being claimed: a rule's ground patterns (marked by
        // mark_rule_patterns, revoked the moment the statement is asserted or
        // derived) and any fact carrying a variable, which is a pattern by
        // construction -- a rule condition, or the query just typed.
        //
        // This is the reading of the whole READ surface: the traversals
        // below, the Janet API they carry, and the usage listings. Unification
        // answers the same question through its own gates, and `.explain`
        // prints it as "[rule pattern; not asserted]". The structural
        // question -- does this node exist at all -- is check_fact/exists.
        bool is_asserted_fact(Node fact) const;

        // The same question for callers that must ask it while holding the
        // adjacency locks, which forbids asking it directly: both stores are
        // guarded by their own mutexes and their writers take those BEFORE
        // the adjacency locks, so a probe in the opposite order could
        // deadlock. Snapshot the nodes to skip first, then lock.
        //
        // Returns nullptr when there is nothing to skip -- the ordinary case,
        // and no allocation for it. The variable half is only as complete as
        // the template-var store: after a binary load or `.fact-stores off`
        // the store is disarmed, and a traversal then rejects a variable
        // through the is_var tests on the triple it reads, which covers every
        // pattern except one whose SUBJECT is itself composite.
        std::shared_ptr<const adjacency_set> unasserted_snapshot() const;

        struct VarClosureStats
        {
            uint64_t flag_queries{0};
            uint64_t walk_fallbacks{0};
        };
        VarClosureStats var_closure_stats() const;
        void            reset_var_closure_stats() const;

        // O(1) variable-set lookup for fact()-created nodes (see Impl's
        // _template_vars). Returns true while the store is authoritative;
        // out is then the node's variable set (nullptr = provably none).
        // Returns false after bulk paths disarmed the store -- callers
        // run the historical reconstruction walk then.
        bool try_get_template_vars(Node nd, std::shared_ptr<const std::unordered_set<Node>>& out) const;
        void count_template_vars_walk() const;

        struct TemplateVarsStats
        {
            uint64_t hits{0};
            uint64_t walks{0};
        };
        TemplateVarsStats template_vars_stats() const;
        void              reset_template_vars_stats() const;

        // --- Genuine-structure store (reconstruction bypass) ---
        // get_fact_structures checks this upon every fs_cache miss and
        // walks the adjacency solely for nodes lacking an entry: a hash
        // node that fact() did not create (a set constant), and each node
        // once the store has been disarmed. fact() stores every triple it
        // creates, including those where subject equals predicate. Atoms
        // and variables never get this far: the lock-free gate answers
        // them first. Ends the O(deg^2) re-reconstruction of hub nodes.
        bool try_get_genuine_structure(Node fact, FactStructurePtr& out) const;

        // Profiler hooks for get_fact_structures: an fs_cache miss is resolved
        // by either the store or the walk, with each resolution counted there,
        // not within the lookup -- predicate_of also queries the store, but
        // its lookups do not constitute answers to a miss.
        void count_genuine_hit() const;
        void count_genuine_walk() const;

        struct GenuineStats
        {
            uint64_t hits{0};
            uint64_t walks{0};
        };
        GenuineStats genuine_stats() const;
        void         reset_genuine_stats() const;

        // --- Fact-path store control (.fact-stores) ---
        // The genuine-structure and template-variable stores grow with
        // every fact() call (~130-180 bytes per fact for the genuine
        // store). Bulk paths (trusted imports, binary loads, removals,
        // merges) disarm them automatically via
        // invalidate_fact_structures_cache; this switch offers the same
        // for API-driven bulk building, e.g. a Janet mass importer.
        // One-way per engine instance: re-arming cannot be made sound
        // retroactively, because ABSENCE of an entry is meaningful while
        // a store is authoritative (.new creates a fresh engine with
        // stores enabled).
        bool fact_stores_enabled() const;
        void disable_fact_stores() const;

        FactComponents    extract_fact_components(Node relation) const;
        void              set_output_handler(io::OutputHandler output) const;
        io::OutputHandler get_output_handler() const;
        void              emit(io::OutputChannel channel, const std::string& text, bool newline = true, bool finding = false) const;
        void              out(const std::string&, bool newline = true) const;
        void              error(const std::string&, bool newline = true) const;
        void              diagnostic(const std::string&, bool newline = true) const;
        void              out_finding(const std::string&, bool newline = true) const;
        void              diagnostic_finding(const std::string&, bool newline = true) const;
        void              prompt(const std::string&, bool newline = false) const;
        io::OutputStream  out_stream() const;
        io::OutputStream  diagnostic_stream() const;
        io::OutputStream  error_stream() const;
        io::OutputStream  prompt_stream() const;
        void              set_logging(int max_depth) const;
        bool              should_log(int depth) const;
        bool              logging_active() const;
        void              log(int depth, const std::string& category, const std::string& message) const;
        bool              use_parallel() const { return _use_parallel; }
        void              toggle_parallel() { _use_parallel = !_use_parallel; }

        // Anchor-based candidate lookups of the unification engine
        // (subject/object-driven snapshots, grounded-subject anchoring,
        // partial-pattern anchoring). Semantically neutral index shortcuts,
        // active by default in BOTH parallel and single-core evaluation --
        // deliberately decoupled from .parallel, which only controls thread
        // pool usage. The off switch provides an anchor-free naive reference
        // for completeness tests and for diagnosing suspected anchor bugs.
        bool   use_anchors() const { return _use_anchors; }
        void   set_anchors(const bool on) { _use_anchors = on; }
        void   set_synapse(const Node from, const Node to, const double weight) const;
        bool   has_synapse(const Node from, const Node to) const;
        double edge_weight(Node from, Node to, double fallback = 1.0) const;
        void   set_edge_weight(Node from, Node to, double weight) const;

        // --- Number display (registered digit alphabet) ---
        // A script may register the digit alphabet of its number
        // representation, in ascending order of value. node_to_string then
        // renders cons lists consisting solely of these digit nodes as
        // decimal &-literals -- the exact inverse of the &-input syntax
        // (zelph/number). An empty vector disables the feature. Any other
        // list keeps the generic <...> display, so cons lists stay
        // general-purpose. See stdlib/decimal-arithmetic.zph.
        void                                                      set_number_digits(const std::vector<Node>& digits_ascending);
        std::shared_ptr<const std::unordered_map<Node, uint32_t>> number_digit_values() const;

        // Register or update a display scheme; returns its index, which
        // set_infix_display consumes. Schemes are matched by name.
        std::size_t register_display_scheme(const DisplayScheme& scheme);
        bool        find_display_scheme(const std::string& name, std::size_t& index) const;

        // Register infix operators into a scheme. Additive across calls; a
        // predicate already claimed by ANY scheme is rejected, because a
        // second claim would make a term's rendering depend on load order.
        // Registered operators are implicitly added to the verbose-self-fact
        // set: (X op X) must render "X op X", never ":op X", or the scheme's
        // own parser could not read it back.
        void set_infix_display(std::size_t scheme, const std::vector<InfixEntry>& operators);

        // Register application-form predicates: a fact (S P O) is written
        // "S(O)", and the predicate name does not appear at all. The head S
        // must render as a bare name matching the scheme's leaf grammar --
        // a composite head has no call notation, so such a term falls back
        // to the default rendering. Shares the one-scheme-per-predicate
        // namespace with set_infix_display.
        void set_application_display(std::size_t scheme, const std::vector<Node>& predicates);

        std::shared_ptr<const DisplayTables> display_tables() const;

        // Display control for self-fact sugar (":pred X"): predicates
        // registered here always render in the verbose "S P S" form.
        // Script-defined, like the digit alphabet: C++ makes no assumptions
        // about which predicates are term-forming operators -- the module
        // that defines an operator declares its display. Input sugar is
        // unaffected. Session state (cleared by .new and by a load that
        // replaces the graph, not persisted).
        void add_verbose_selffact_predicates(const std::vector<Node>& preds);
        bool selffact_sugar_suppressed(Node pred) const;

        // --- Fact-creation observer (semi-naive evaluation) ---
        // Invoked from fact() exactly when a NEW fact node is materialized
        // (never for pre-existing facts). Reasoning::run uses it to capture
        // the delta of facts created during a reasoning pass -- including
        // inner facts materialized as side effects of instantiate_fact,
        // which a deduce()-level hook would miss. Empty by default and
        // outside of runs. Deliberately NOT invoked by
        // fact_import_trusted_single_object (bulk import path).
        using FactCreationObserver = std::function<void(Node relation, Node predicate)>;
        void set_fact_creation_observer(FactCreationObserver observer);

        // --- Implemented in zelph_names.cpp (name management) ---

        void set_name(Node node, const std::string& name, std::string lang, bool merge_on_conflict);
        Node set_name(const std::string& name_in_current_lang, const std::string& name_in_given_lang, std::string lang);
        /// The importer's method for assigning a label to an entity. In
        /// contrast to set_name, it takes nothing from a different node: the
        /// node gets the label as its name, every other node that carries the
        /// label keeps it, and the name → node entry is directed to whichever
        /// carrier ranks first (see label_precedes). The outcome remains
        /// unaffected by the order in which labels are received, regardless
        /// of the threading within an import.
        void assign_label_ranked(Node node, const std::string& label, std::string lang);
        /// Among two carriers of a single label, the one that holds it is
        /// determined by: a property (`P…`) preceding an item (`Q…`) before
        /// any other node, followed by the smaller numeric value derived from
        /// the node's name in language `wikidata` (Q42 → 42; a node ID is
        /// assigned without a stable order during a threaded import), then
        /// the lower node ID. A node lacking a `wikidata` name comes last,
        /// and a variable comes after every other node.
        bool label_precedes(Node a, Node b) const;
        /// Each node that carries `name` in `lang` (the current language if
        /// empty), following the sequence defined by label_precedes: the
        /// node the name resolves to alone when no other node carries it,
        /// none if no node does; never a variable or a core node. Optional
        /// and empty if the name sections were partially loaded, since the
        /// carriers in the omitted chunks are not known. Creates nothing.
        std::optional<std::vector<Node>> nodes_named(const std::string& name, std::string lang = "") const;
        /// Whether the carriers kept for the labels that several nodes carry
        /// are precisely the ones the name maps hold, and each such label
        /// resolves to a node. Walks the forward map, serving as a check for
        /// tests, never called by the engine.
        bool label_index_consistent() const;
        /// The name, in language `zelph`, of the node a Wikidata import
        /// writes to say that every item kept its label and the lowest-ranked
        /// carrier holds it. A network without this node was imported
        /// previously.
        static const char* wikidata_labels_marker_name();
        std::string        get_name(const Node node, std::string lang = "", const bool fallback = false) const;
        std::string        get_formatted_name(Node node, const std::string& lang) const;
        bool               has_name(Node node, const std::string& lang) const;
        // Whether the node is a core node or possesses a name in any
        // language.
        bool        is_named_any(Node node) const;
        void        remove_name(Node node, std::string lang = "");
        void        unset_name(Node node, std::string lang = "");
        void        register_core_node(Node n, const std::string& name);
        Node        get_core_node(const std::string& name) const;
        std::string get_core_name(Node n) const;
        std::string get_name_hex(Node node, bool prepend_num, int max_neighbors) const;
        // In the language, the node corresponding to a name, otherwise the
        // core node sharing that spelling, otherwise 0. When atoms_only is
        // set, a variable carrying the name does not constitute an answer:
        // this is node()'s lookup before it creates a node, and the lookup
        // performed by a caller who must not write the graph.
        Node get_node(const std::string& name, const std::string& lang = "", bool atoms_only = false) const;
        // A node rendered for a HUMAN -- diagnostics, log lines, error
        // messages. The identifier markers node_to_string works with are
        // resolved here, so they stay an internal of the renderer instead
        // of leaking into whichever caller forgot to strip them.
        std::string              format(Node node) const;
        std::vector<std::string> get_languages() const;
        bool                     has_language(const std::string& language) const;
        name_of_node_map         get_nodes_in_language(const std::string& lang) const;
        std::vector<Node>        resolve_nodes_by_name(const std::string& name) const;
        size_t                   get_name_of_node_size(const std::string& lang) const;
        size_t                   get_node_of_name_size(const std::string& lang) const;
        size_t                   language_count() const;
        /// Whether each name -> node entry is matched by a corresponding
        /// node -> name entry, across all languages: this condition serves
        /// as the invariant that removal relies on to find a node's names
        /// without walking the reverse map. Walks both maps, so it is a
        /// check for tests, never called by the engine.
        bool name_maps_consistent() const;

        // --- Implemented in zelph_maintenance.cpp (cleanup, rules, persistence) ---

        void   cleanup_isolated(size_t& removed_count) const;
        size_t cleanup_names() const;
        /// Removes the node and everything it is a PART of, cascading
        /// upwards. Returns HOW MANY nodes went, which is more than one
        /// whenever the node took part in a fact -- the callers report it.
        /// `deferred_names` makes it affordable in BULK; see the definition.
        size_t        remove_node(Node node, adjacency_set* deferred_names = nullptr) const;
        void          collect_doomed(Node node, adjacency_set& out, const adjacency_set* keep = nullptr) const;
        void          remove_doomed(const adjacency_set& doomed, adjacency_set* deferred_names = nullptr) const;
        void          remove_names_of(const adjacency_set& dead) const;
        uint64_t      name_map_scans() const;
        adjacency_set get_rules() const;

        /// Whether the `=>` fact `node` conforms to the form of a rule --
        /// having a statement as its condition and a consequence capable of
        /// being asserted (see the definition). get_rules lists such a node
        /// unless a statement mentions it; regardless of whether it is
        /// mentioned or not, the form is the same.
        bool has_rule_form(Node node) const;

        /// Is this node a PART of some other fact -- its subject, its
        /// predicate or one of its objects? For a rule that is the
        /// difference between stating it and stating something ABOUT it.
        bool is_mentioned(Node node) const;

        // --- Rule patterns that are not data -------------------------------
        //
        // Writing a rule materializes its condition and consequence patterns
        // as real fact nodes, because the engine has nothing else to match
        // against. A pattern carrying a variable is recognisable as a
        // template and rejected as data everywhere. A GROUND one is not:
        // "(a p b) => (c q d)" made both `a p b` and `c q d` answer queries
        // and drive other rules, although nobody had claimed either.
        //
        // The node itself cannot say which happened -- asserting a statement
        // and building it as a pattern produce the same node with the same
        // edges. What CAN say it is the moment of construction: a rule is
        // built inside a scratch cluster (see zelph/dedup-rule), and a
        // cluster records exactly the nodes that did not exist before. Those
        // are pattern-only, and mark_rule_patterns records that as ordinary
        // graph structure -- a fact, so a .save/.load round trip keeps it and
        // nothing has to be remembered across sessions.
        //
        // Asserting the same statement later, or DERIVING it, revokes the
        // mark: it is then a claim like any other.

        // A rule's subject is either ONE condition or a container holding
        // them. The `~ conjunction` tag says HOW the members are combined --
        // it is not what makes the node a container, and other combinations
        // are conceivable. For a container of exactly ONE member no
        // combination can differ from any other, so the tag cannot change
        // its meaning and must not decide whether the rule works: writing a
        // single condition in set notation, without the comma sugar that
        // adds the tag, is the same rule.
        //
        // Every reader of a rule's conditions goes through this, so the two
        // spellings cannot drift apart again.

        /// The conditions `condition` stands for, if it is a container the
        /// engine reads as a set of them: tagged `~ conjunction`, or holding
        /// exactly one member. False -- and `out` untouched -- for a
        /// statement in its own right.
        bool condition_set_members(Node condition, adjacency_set& out) const;

        /// Whether `condition` is such a container at all.
        bool is_condition_set(Node condition) const;

        /// The pattern a negated condition negates, or 0 when `condition`
        /// is not one. `¬(P)` IS the fact `((P) ~ negation)`, and a rule
        /// names that fact as its condition, the way `⁺` and `≈` make their
        /// tag fact the condition. Thus, a pattern that one rule negates
        /// and another one uses positively is two nodes, and the polarity
        /// forms part of what the rule says. Only a fact concerning a
        /// statement or a conjunction set qualifies as a negation:
        /// `(X ~ negation)` with a variable subject stays an ordinary
        /// condition.
        Node negation_operand(Node condition) const;

        /// `¬(pattern)`: creates the fact `((pattern) ~ negation)` and
        /// returns it, the node a rule names as its condition. Rejected if
        /// the pattern is neither a statement nor a conjunction set,
        /// because negation_operand would not read it as negated, and, in
        /// fact(), for a negation: `¬¬(F)` would require a second level of
        /// negation.
        Node negate(Node pattern);

        /// The predicate that marks a pattern, or 0 when no graph has ever
        /// needed it (`create` false).
        Node rule_pattern_predicate(bool create) const;

        /// Mark the ground condition and consequence patterns of `rule` that
        /// appear in `created`. Everything else -- patterns with variables,
        /// nodes that already existed, the predicate declarations the
        /// construction emitted -- is left alone.
        void mark_rule_patterns(Node rule, const std::vector<Node>& created) const;

        /// The same applies to the parts of a rule: its condition
        /// and its consequences.
        void mark_rule_parts(Node condition, const adjacency_set& consequences, const std::vector<Node>& created) const;

        /// Whether `node` serves as the fact that a condition operator
        /// builds and that a condition is read as: a negation
        /// (negation_operand), a path condition (`closure`), or a
        /// neural one (`nn`).
        bool is_condition_tag(Node node) const;

        /// Designate as patterns whatever the query `statement` created
        /// under a condition operator -- the operator's fact, its operand,
        /// and all contained elements -- while excluding anything the query
        /// also reaches outside every operator: a positive ground part is
        /// created, and thereby asserted, just as previously. Only nodes
        /// within `created` are marked; a node that was present beforehand
        /// keeps its status. `statement` equals 0 when a query yields no
        /// node.
        void mark_query_operands(Node statement, const std::vector<Node>& created) const;

        /// The rule in force that specifies the same content as `rule`, up to
        /// a renaming of its variables (rule_identity.hpp), or 0. With
        /// `mentioned`, if not found, a rule that a statement merely mentions.
        /// Determined via the fingerprint index; refer to the definition.
        Node find_equivalent_rule(Node rule, bool mentioned) const;

        /// How many rule fingerprints the index has computed since the
        /// most recent reset, tallied only when logging is active
        /// (`.prof`).
        uint64_t rules_fingerprinted() const;
        void     reset_rules_fingerprinted() const;

        /// The rule in force that the construction of the rule `statement`
        /// under recipe key `key` (Reasoning::rebuild_rule) returned during its
        /// most recent execution -- either the rule it built or the one it
        /// claimed -- or 0. Without this, the construction would repeatedly
        /// assemble its components, locate the rule, and keep nothing new, or
        /// claim the rule and discard its components anew on every pass of
        /// every run; with this, it retrieves a single entry. The entry remains
        /// valid as long as the rule persists, remains in force, and no
        /// alteration has occurred in any rule's text under its fingerprint
        /// (refer to find_equivalent_rule). One entry per unique construction,
        /// thus rule-scale.
        Node claimed_rule(Node statement, Node key) const;
        void note_claim(Node statement, Node key, Node rule) const;

        /// Re-mark patterns whose marking a dropped cluster revoked. A node
        /// the drop removed is skipped, so this is safe to call with whatever
        /// the cluster recorded.
        void restore_rule_patterns(const std::vector<Node>& patterns) const;

        /// Was this fact node built as a rule pattern and never claimed?
        /// One hash probe, and a single empty() test in the graphs -- the
        /// overwhelming majority -- whose rules all carry variables.
        bool is_rule_pattern(Node node) const;

        /// Revoke the mark: the statement has been asserted or derived.
        /// Returns whether there was one.
        bool unmark_rule_pattern(Node node) const;

        /// What a claim does with a node nested in the statement
        /// (claim_nested): `Pass` neither claims nor enters it, `Claim`
        /// revokes its mark and enters it only if one was present, `Enter`
        /// revokes its mark and enters it. `into` is set for the object of a
        /// membership.
        enum class ClaimStep
        {
            Pass,
            Claim,
            Enter
        };
        using ClaimReach = std::function<ClaimStep(Node node, bool into)>;

        /// A statement claims itself and every fact nested in it outside a
        /// `=>`: revoke the marks of the facts nested in `statement`, just as
        /// unmark_rule_pattern revokes its own. A `=>` encountered along the
        /// way is claimed as a node, yet its parts are not; a rule's own
        /// collection and a conjunction set remain unchanged as rule text.
        /// `reach` determines the outcome for every other node. A network
        /// lacking a rule pattern pays one atomic load, and a statement whose
        /// immediate parts yield no work for the walk results in no
        /// allocation.
        void claim_nested(Node statement, const ClaimReach& reach) const;

        /// The nodes that claim_nested has taken up and the adjacency
        /// entries it has read in this process. Unaffected by hardware,
        /// enabling a test to bound what a claim costs per statement.
        static uint64_t claim_walk_steps();

        /// Rebuild the in-memory index from the graph. A binary load restores
        /// the marking facts without going through fact(), so the index has
        /// to be read back off the marker's extent afterwards.
        void rebuild_rule_pattern_index() const;

        /// Does the graph hold this fact as known-WRONG? That is what `¬(F)`
        /// says outside a rule condition, and the set is what keeps such a
        /// fact from answering a positive query: structurally it is an
        /// ordinary fact, because a fact's probability rides on its edge to
        /// its predicate. Same cost profile as is_rule_pattern -- one atomic
        /// load in every graph that refutes nothing.
        bool is_refuted_fact(Node node) const;

        /// Record the claim that this fact does not hold. The caller has
        /// already created it with a probability below 0.5; this adds the
        /// marking fact that survives a save and the index that makes the
        /// per-candidate test cheap.
        void mark_refuted_fact(Node node) const;

        /// The predicate a refutation is marked with. A NAMED node, because
        /// the criterion in CLAUDE.md under "What must be a CORE node" gives
        /// that answer: nothing has to reach it without a name lookup.
        static const char* refuted_fact_name();

        /// Put a node into the refuted index without writing the marking fact,
        /// for a caller that has just seen or written one.
        void note_refuted(Node node) const;

        /// Does the graph hold ANY refuted fact? One atomic load, so a caller
        /// on a bulk path can decide whether the question is worth asking at
        /// all before it starts asking it per node.
        bool has_refuted_facts() const;

        /// The refuted nodes as they stand, copied out under the index lock.
        /// A caller that has to ASK something about each of them needs the
        /// copy: every such question reads the graph, and a graph writer takes
        /// the index lock while holding the network (Zelph::fact ->
        /// note_refuted), so answering under the index lock inverts that order.
        std::vector<Node> refuted_facts_snapshot() const;

        /// Drop these nodes from the index, because they are being removed.
        /// A contradiction record is content-addressed, so a stale entry would
        /// survive the removal of the very facts it is about and silence the
        /// report when they come back.
        void forget_refuted(const adjacency_set& gone) const;

        /// Rebuild that index from the graph, for the same reason
        /// rebuild_rule_pattern_index exists.
        void rebuild_refuted_index() const;

        // A node IS the hash of what it is built from, so merging one node
        // away invalidates the identity of everything built on it: the fact
        // `a p b` whose subject was merged into `c` still carries the hash of
        // (p, a, {b}) while its edges now read (p, c, {b}). Re-entering the
        // line the renderer prints then creates a SECOND node, and a fact
        // whose subject and predicate became the same node stops being
        // readable at all -- the subject == predicate reading verifies the
        // hash by construction.
        //
        // Network::merge cannot repair that: it rewires edges, and knowing
        // what a triple is belongs above network.hpp. These two do it here.

        /// One hash-identified node's recipe, captured while its id is still
        /// valid. A set constant is identified by its members, everything
        /// else by its triple.
        struct HashRecipe
        {
            bool          is_set    = false;
            Node          subject   = 0;
            Node          predicate = 0;
            adjacency_set objects;
            adjacency_set members;
        };

        /// Every hash-identified node that `node` takes part in, transitively,
        /// with its recipe -- ordered so that a node comes after everything it
        /// is built from. Call BEFORE the merge, while the hashes still hold.
        std::vector<std::pair<Node, HashRecipe>> collect_hash_dependents(Node node) const;

        /// Re-create those nodes under the id their new components give them,
        /// folding each into an equal node that already exists. Call AFTER the
        /// merge, with what the call above returned.
        void rehash_dependents(const std::vector<std::pair<Node, HashRecipe>>& recipes, Node from, Node into) const;

        /// Which node keeps a contested name and which one disappears into
        /// it. Assumes both name locks are HELD; reads only, so set_name can
        /// derive it twice -- once to plan the repair, once to carry it out.
        /// False when there is no conflict; throws when the two cannot merge.
        bool resolve_name_conflict_locked(Node node, const std::string& name, const std::string& lang, Node& from, Node& into) const;

        void   remove_rules() const;
        size_t rule_count() const;
        void   save_to_file(const std::string& filename) const;

        /// Write a network containing only the facts of the given predicates,
        /// the nodes they connect and the names of those nodes. Returns the
        /// number of facts written. See the definition for what else has to
        /// travel with them for the result to be a usable network.
        // rules_kept, if given, receives how many rules the slice happens to
        // contain -- see the definition for why that is not a fixed answer.
        size_t save_predicate_slice(const std::string& filename, const std::vector<Node>& predicates, size_t* rules_kept = nullptr) const;
        // During a whole-file load, the graph gets substituted with the
        // contents of the file: each node, name, and cluster associated with
        // the session is discarded, indexes are regenerated based on the file,
        // and _on_graph_replaced lets the derived engine discard its prior
        // knowledge of the old graph. If a file fails following its header,
        // the graph retains a portion of its content.
        void load_from_file(const std::string& filename) const;
        void load_from_file(const std::string& filename, const BinChunkSelection& selection, bool skip_payload = false) const;
        void load_from_manifest(const std::string&       manifest_path,
                                const BinChunkSelection& selection,
                                const std::string&       shard_root        = "",
                                const std::string&       bin_path_override = "",
                                bool                     skip_payload      = false) const;

        void                                        set_active_cluster(const std::string& name) const;
        void                                        deactivate_cluster() const;
        std::string                                 active_cluster_name() const;
        std::vector<std::pair<std::string, size_t>> list_clusters() const;
        /// The nodes a cluster has recorded, i.e. which of them are NEW.
        std::vector<Node> cluster_nodes(const std::string& name) const;
        size_t            drop_cluster(const std::string& name) const;
        size_t            drop_scratch_cluster(const std::string& name) const;
        bool              merge_cluster(const std::string& from, const std::string& to) const;
        // The engine employs clusters whose names begin with a leading "__":
        // the scratch where a rule is constructed (__rule), a construction's
        // (__construction), and those used by .explain (__explain) and
        // .prune-facts (__prune). Should a user define a cluster with such a
        // name, it is treated as the scratch: the engine drops or merges that
        // cluster, and any work the user has done within it goes with it. The
        // entry points that allow a user to designate a cluster for activation
        // or merging explicitly reject such names: .cluster, .cluster-merge,
        // zelph/cluster, and zelph_cluster.
        static bool        is_scratch_cluster_name(const std::string& name) { return name.starts_with("__"); }
        static std::string scratch_cluster_name_refusal(const std::string& name)
        {
            return "the cluster name '" + name + "' begins with \"__\", which is reserved for the engine's own scratch clusters";
        }

        // --- Members ---

        class Impl;
        Impl* const _pImpl; // must stay at top of members list because of initialization order

        const struct PredefinedNode
        {
            const Node RelationTypeCategory;
            const Node Causes;
            const Node IsA;
            const Node Unequal;
            const Node Contradiction;
            const Node Cons;
            const Node Nil;
            const Node PartOf;
            const Node Conjunction;
            const Node Negation;
        } core;

    protected:
        std::string                                    _lang{"en"};
        std::unordered_map<network::Node, std::string> _core_names_by_node;
        std::unordered_map<std::string, network::Node> _core_names_by_name;
        bool                                           _use_parallel{true};
        bool                                           _use_anchors{true};
        mutable std::atomic<bool>                      _fs_cache_suspended{false};
        mutable std::atomic<uint64_t>                  _fs_cache_hits{0};
        mutable std::atomic<uint64_t>                  _fs_cache_misses{0};
        mutable std::atomic<uint64_t>                  _fs_cache_full_clears{0};
        mutable std::atomic<uint64_t>                  _fs_cache_stale_erased{0};
        mutable std::atomic<uint64_t>                  _var_flag_queries{0};
        mutable std::atomic<uint64_t>                  _var_flag_fallbacks{0};
        mutable std::atomic<uint64_t>                  _genuine_hits{0};
        mutable std::atomic<uint64_t>                  _genuine_walks{0};
        mutable std::atomic<uint64_t>                  _tvars_hits{0};
        mutable std::atomic<uint64_t>                  _tvars_walks{0};
        mutable std::atomic<uint64_t>                  _rules_fingerprinted{0};
        // The display registrations are mutable since a load, which is
        // declared as const, forgets them (forget_display_registrations).
        mutable std::shared_ptr<const std::unordered_map<Node, uint32_t>> _number_digits;
        mutable std::shared_mutex                                         _smtx_number_digits;
        mutable std::shared_ptr<const DisplayTables>                      _display_tables;
        mutable std::shared_mutex                                         _smtx_display_tables;
        mutable std::unordered_set<Node>                                  _verbose_selffact_preds;
        mutable std::shared_mutex                                         _smtx_verbose_selffact_preds;
        FactCreationObserver                                              _on_fact_created;

        // Triggered following a load operation that replaced the graph, as
        // well as after one that halted midway. Reasoning sets it to disregard
        // the information it retains about the old graph across commands.
        std::function<void()> _on_graph_replaced;

        // Drop the memoized relation-type set, so the next predicate test
        // rebuilds it from the graph. Protected rather than private: every
        // path that removes facts has to call it when one of them was a
        // relation-type declaration, and the prune paths live in Reasoning.
        void invalidate_relation_type_set() const;

    private:
        // After a load: once it has progressed the epoch past
        // `epoch_before`, rebuilds the indexes read from the graph, forgets
        // the display registrations, and calls _on_graph_replaced.
        void adopt_loaded_graph(std::uint64_t epoch_before) const;

        // Drop the scheme tables, the digit alphabet, and the
        // verbose self-fact predicates, all of which identify nodes
        // through their unique identifiers.
        void forget_display_registrations() const;

        // The ground facts a construction brought into existence, reached
        // from `pending`: the walk underlying mark_rule_parts and
        // mark_query_operands. `fresh` holds what the construction created
        // and gains the memberships of the containers it created; `stop`
        // ends the walk at a node, which is then neither collected nor
        // entered.
        std::vector<Node> constructed_patterns(std::vector<Node> pending, std::unordered_set<Node>& fresh, const std::function<bool(Node)>& stop) const;

        // Mark `patterns` as rule patterns (see is_rule_pattern).
        void mark_patterns(const std::vector<Node>& patterns) const;

        // Whether the conditions that a `=>` fact's container subject
        // represents hold a variable, each asked by var_in_closure: what
        // fact() reads upon creating such a fact (see Impl's
        // _rule_text_vars).
        bool conditions_hold_variable(Node conditions) const;

        // Whether the rule a `=>` fact over `subject` and `objects` states
        // holds, in either its conditions or its consequences, a
        // membership whose object is a rule's own collection with a
        // variable embedded in its text: what fact() reads in addition to
        // conditions_hold_variable.
        bool rule_memberships_hold_variable(Node subject, const adjacency_set& objects) const;

        // The text of a rule may have changed under its fingerprint: the
        // next lookup in the fingerprint index fingerprints every rule
        // again.
        void note_rule_text_changed() const;

        // collection() and conjunction_collection(): a written template when
        // `rule_text` indicates it, a counter otherwise.
        Node new_collection(const std::unordered_set<Node>& elements, bool rule_text);

        // Whether a component of a newly created fact or a member of a newly
        // defined set constant brings a variable of rule text into it that
        // the template-variable store lacks: a fact or set constant input
        // into Impl's _rule_text_vars, or a rule's own collection whose text
        // holds one.
        bool component_holds_text_variable(Node component) const;

        // The memberships of the collections that an engine before
        // template ids wrote as part of a rule's text; see the
        // definition.
        std::unordered_set<Node> literals_written_before_template_ids() const;

        zelph::io::OutputStream locked_stream(zelph::io::OutputChannel channel) const;
        void                    register_operator_display(std::size_t scheme, const std::vector<std::pair<Node, OperatorDisplay>>& entries);
    };
}
