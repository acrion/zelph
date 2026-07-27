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
    // The wrapper (open/close) is emitted ONLY where the rendering actually
    // deviates from what the default renderer would produce -- elided
    // parentheses or a different numeral prefix. Both strings are emitted
    // verbatim, so a scheme wanting padding registers "$( " and " )".
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
        bool                 has_left_edge(Node b, Node a) const;
        bool                 has_right_edge(Node a, Node b) const;
        static Node          create_hash(const adjacency_set& vec);
        static Node          create_hash(const Node predicate, const Node subject, const adjacency_set& objects);
        static bool          is_hash(Node a);
        static bool          is_var(Node a);
        Answer               check_fact(Node subject, Node predicate, const adjacency_set& objects) const;

        // Single-argument form for callers that already hold the relation
        // node itself (e.g. .explain, whose target comes from evaluating a
        // fact pattern). Derives the predicate via predicate_of(); a node
        // that is not a readable fact node is reported as unknown.
        Answer check_fact(Node relation) const;

        // Predicate of an existing fact node. Prefers the genuine-structure
        // store (exact, O(1), no heuristics); falls back to parse_relation()
        // for nodes without an entry -- subject == predicate facts, which the
        // store deliberately omits, and everything after a bulk path disarmed
        // it. Returns 0 if nd is not a fact node or its predicate cannot be
        // determined unambiguously.
        Node predicate_of(Node nd) const;

        Node fact(Node subject, Node predicate, const adjacency_set& objects, long double probability = 1);
        Node fact_import_trusted_single_object(Node subject, Node predicate, Node object) const;
        Node list(const std::vector<Node>& elements);
        Node list(const std::vector<std::string>& elements);
        Node set(const std::unordered_set<Node>& elements);
        Node parse_fact(Node rule, adjacency_set& deductions, Node parent = 0) const;
        Node parse_relation(const Node rule) const;
        // Locked-scope read access (see Network::ReadScope). Constructed
        // here because only zelph.cpp sees the complete Impl type -- this
        // is the visibility-correct path (Cap'n-Proto layering).
        Network::ReadScope read_scope() const;
        void               collect_anchored_facts(Node anchor, Node relation, adjacency_set& out) const;

        // parse_relation for code running under a live ReadScope: all
        // adjacency reads via scope references, predicate detection via
        // the caller-provided relation-type memo. The memo MUST be fetched
        // BEFORE the scope opens (its lazy build takes network locks).
        Node         parse_relation_scoped(const Network::ReadScope&                 scope,
                                           const ankerl::unordered_dense::set<Node>& rel_types,
                                           Node                                      rule) const;
        Node         count() const;
        AllNodeView  get_all_nodes_view() const;
        LangNodeView get_lang_nodes_view(const std::string& lang) const;
        bool         try_get_fact_structures_cached(Node fact, FactStructurePtr& out) const;
        void         store_fact_structures_cached(Node fact, FactStructurePtr value) const;
        void         invalidate_fact_structures_cache() const noexcept;
        void         invalidate_fact_structures_for(Node subject, Node predicate, const adjacency_set& objects, Node relation) const;

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

        // --- Variable-closure flag (rule-template detection) ---
        // True iff nd is a variable or its GENUINE structural closure
        // (subject, predicate, objects at any depth) contains one -- the
        // criterion separating rule-template nodes from data nodes
        // (template rejection in extract_bindings, anchor eligibility,
        // bound-pattern grounding). O(1): maintained eagerly by fact()
        // from the actual triple arguments -- hash-consing materializes
        // children before parents, so child flags are final when the
        // parent is created, and a node's ID is its triple hash, so the
        // flag can never change afterwards. Unlike the former
        // reconstruction-based walk this cannot be misled by ambiguous
        // adjacency readings. Paths that bypass triple construction or
        // destroy topology clear the authoritative bit (see Impl); the
        // query then falls back to the historical walk -- never unsound,
        // never worse than the pre-flag behaviour.
        bool var_in_closure(Node nd) const;

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
        // get_fact_structures consults this on every fs_cache miss and
        // walks the adjacency only for nodes without an entry: atoms
        // (negative entries), subject == predicate facts (deliberately
        // not stored -- the walk reconstructs those as EMPTY and
        // unification's atom treatment of them must not change), and
        // everything after the store is disarmed. Ends the O(deg^2)
        // re-reconstruction of hub nodes.
        bool try_get_genuine_structure(Node fact, FactStructurePtr& out) const;
        void count_genuine_walk() const; // profiler hook for get_fact_structures

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
        void              emit(io::OutputChannel channel, const std::string& text, bool newline = true) const;
        void              out(const std::string&, bool newline = true) const;
        void              error(const std::string&, bool newline = true) const;
        void              diagnostic(const std::string&, bool newline = true) const;
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
        // general-purpose. See stdlib/arithmetic.zph.
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
        // unaffected. Session state (cleared by .reset, not persisted).
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

        void                     set_name(Node node, const std::string& name, std::string lang, bool merge_on_conflict);
        Node                     set_name(const std::string& name_in_current_lang, const std::string& name_in_given_lang, std::string lang);
        std::string              get_name(const Node node, std::string lang = "", const bool fallback = false) const;
        std::string              get_formatted_name(Node node, const std::string& lang) const;
        bool                     has_name(Node node, const std::string& lang) const;
        void                     remove_name(Node node, std::string lang = "");
        void                     unset_name(Node node, std::string lang = "");
        Node                     get_node(const std::string& name, std::string lang = "") const;
        void                     register_core_node(Node n, const std::string& name);
        Node                     get_core_node(const std::string& name) const;
        std::string              get_core_name(Node n) const;
        std::string              get_name_hex(Node node, bool prepend_num, int max_neighbors) const;
        std::string              format(Node node) const;
        std::vector<std::string> get_languages() const;
        bool                     has_language(const std::string& language) const;
        name_of_node_map         get_nodes_in_language(const std::string& lang) const;
        std::vector<Node>        resolve_nodes_by_name(const std::string& name) const;
        size_t                   get_name_of_node_size(const std::string& lang) const;
        size_t                   get_node_of_name_size(const std::string& lang) const;
        size_t                   language_count() const;

        // --- Implemented in zelph_maintenance.cpp (cleanup, rules, persistence) ---

        void          cleanup_isolated(size_t& removed_count) const;
        size_t        cleanup_names() const;
        void          remove_node(Node node) const;
        adjacency_set get_rules() const;
        void          remove_rules() const;
        size_t        rule_count() const;
        void          save_to_file(const std::string& filename) const;
        void          load_from_file(const std::string& filename) const;
        void          load_from_file(const std::string& filename, const BinChunkSelection& selection, bool skip_payload = false) const;
        void          load_from_manifest(const std::string&       manifest_path,
                                         const BinChunkSelection& selection,
                                         const std::string&       shard_root        = "",
                                         const std::string&       bin_path_override = "",
                                         bool                     skip_payload      = false) const;

        void                                        set_active_cluster(const std::string& name) const;
        void                                        deactivate_cluster() const;
        std::string                                 active_cluster_name() const;
        std::vector<std::pair<std::string, size_t>> list_clusters() const;
        size_t                                      drop_cluster(const std::string& name) const;
        bool                                        merge_cluster(const std::string& from, const std::string& to) const;

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
        std::string                                               _lang{"en"};
        std::unordered_map<network::Node, std::string>            _core_names_by_node;
        std::unordered_map<std::string, network::Node>            _core_names_by_name;
        bool                                                      _use_parallel{true};
        bool                                                      _use_anchors{true};
        mutable std::atomic<uint64_t>                             _fs_cache_hits{0};
        mutable std::atomic<uint64_t>                             _fs_cache_misses{0};
        mutable std::atomic<uint64_t>                             _fs_cache_full_clears{0};
        mutable std::atomic<uint64_t>                             _fs_cache_stale_erased{0};
        mutable std::atomic<uint64_t>                             _var_flag_queries{0};
        mutable std::atomic<uint64_t>                             _var_flag_fallbacks{0};
        mutable std::atomic<uint64_t>                             _genuine_hits{0};
        mutable std::atomic<uint64_t>                             _genuine_walks{0};
        mutable std::atomic<uint64_t>                             _tvars_hits{0};
        mutable std::atomic<uint64_t>                             _tvars_walks{0};
        std::shared_ptr<const std::unordered_map<Node, uint32_t>> _number_digits;
        mutable std::shared_mutex                                 _smtx_number_digits;
        std::shared_ptr<const DisplayTables>                      _display_tables;
        mutable std::shared_mutex                                 _smtx_display_tables;
        std::unordered_set<Node>                                  _verbose_selffact_preds;
        mutable std::shared_mutex                                 _smtx_verbose_selffact_preds;
        FactCreationObserver                                      _on_fact_created;

    private:
        zelph::io::OutputStream locked_stream(zelph::io::OutputChannel channel) const;
        void                    invalidate_relation_type_set() const;
        void                    register_operator_display(std::size_t scheme, const std::vector<std::pair<Node, OperatorDisplay>>& entries);
    };
}
