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

#include "chrono/stopwatch.hpp"
#include "concurrency/thread_pool.hpp"
#include "contradiction_error.hpp"
#include "io/derivation_export.hpp"
#include "io/output.hpp"
#include "network_types.hpp"
#include "neural.hpp"
#include "reasoning_profiler.hpp"
#include "zelph.hpp"

#include <zelph_export.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    struct RulePos
    {
        Node                                      node;
        std::shared_ptr<std::vector<Node>>        conditions;
        size_t                                    index;
        std::shared_ptr<Variables>                variables{std::make_shared<Variables>()};
        std::shared_ptr<Variables>                unequals{std::make_shared<Variables>()};
        std::shared_ptr<std::unordered_set<Node>> excluded{std::make_shared<std::unordered_set<Node>>()};

        // Accumulated confidence of ≈ conditions along this binding path;
        // stays 1.0 when no neural condition fired. Propagated into deduce()
        // and stored as the deduced fact's probability.
        double confidence{1.0};
    };

    struct ReasoningContext
    {
        Node                 current_condition{0};
        std::vector<RulePos> next;
        adjacency_set        rule_deductions;
    };

    // --- Free helper functions (implemented in reasoning.cpp) ---

    // Recursively substitute variables in a fact pattern to produce a concrete fact.
    // Used by evaluate and deduce to instantiate rule patterns with current bindings.
    // rebuild_container = false keeps a container's identity instead of rebuilding
    // it from the substituted members; the object of a PartOf fact is written INTO
    // and must stay the same container across bindings.
    Node instantiate_fact(Zelph* z, Node pattern, const Variables& variables, int depth, std::vector<Node>& history, bool rebuild_container = true);

    // Recursively collect all variable nodes from a fact pattern.
    // Used to detect "fresh variables" that appear only in rule consequences.
    ZELPH_EXPORT void collect_variables(Zelph* z, Node pattern, std::unordered_set<Node>& vars, int depth, std::vector<Node>& history);

    // --- Proof reconstruction (reasoning_explain.cpp) --------------------------
    // Rebuild a justification for an asserted fact from the saturated graph.
    // Nothing is tracked during inference: after quiescence, every derived
    // fact has at least one rule instantiation whose consequence unifies
    // with it and whose conditions are all present -- this backward search
    // finds one, using the same unification machinery that runs forward.
    // Read-only: no graph structure is created. Cost is irrelevant here by
    // design; it buys a zero-overhead forward pass.
    struct ProofNode
    {
        enum class Status
        {
            Derived,     // justified by `rule` via `premises` and `absent`
            Axiom,       // asserted, and no rule consequence unifies: an input fact
            Unfounded,   // asserted, rule consequences unify, but no acyclic
                         // instantiation verifies against the CURRENT graph
                         // (e.g. a NAF premise that has since become true)
            RulePattern, // the node exists only because a rule was written
                         // with this statement as a ground pattern -- nobody
                         // claimed it, so there is nothing to justify
            Truncated    // not expanded: depth limit reached
        };

        Node                                    fact   = 0;
        Node                                    rule   = 0; // the => fact, Derived only
        Status                                  status = Status::Axiom;
        std::vector<std::shared_ptr<ProofNode>> premises; // positive conditions
        std::vector<Node>                       absent;   // NAF conditions, verified absent NOW

        // Transitive path conditions, verified NOW by walking the closure.
        // Like `absent` these carry the rule's PATTERN rather than a fact
        // node, and for the same reason: what makes the premise hold is a
        // walk, not a statement anybody claimed, so there is no node to
        // point at. `bindings` turns the pattern into the path that was
        // actually tested.
        std::vector<Node> walked;

        // Whether the search found a SECOND verified instantiation for this
        // fact. Only the first is reconstructed -- following every one of them
        // would multiply the work at each level and answer a question nobody
        // asked. What the flag buys is that the one shown no longer passes for
        // the only one there is, which is the difference between "the evidence"
        // and "some evidence". Set at the root only; see reconstruct().
        bool more_justifications = false;

        // The instantiation that justifies this step (Derived only). The
        // positive premises are already ground nodes, but a NAF condition
        // usually has no node -- absence is why it holds -- so `absent`
        // carries the rule's PATTERN and needs these bindings to be
        // rendered as the concrete premise that was checked. Variables
        // occurring only inside the negation stay unbound: that is what
        // "for no D" means, and it must remain visible as such.
        Variables bindings;
    };

    class ZELPH_EXPORT Reasoning : public Zelph
    {
    public:
        // --- Implemented in reasoning.cpp (orchestration) ---

        explicit Reasoning(const io::OutputHandler& output = io::default_output_handler);
        // Path of the JSON Lines file the next run(export=true) writes.
        void set_export_file(const std::string& path);
        void set_query_collector(std::vector<std::shared_ptr<Variables>>* collector);
        // incremental: skip the classic first pass and seed the fixpoint from
        // the facts created since the previous run (see .run-delta). Only
        // sound when the graph was already saturated under the current rule
        // set; run() falls back to a classic pass when it cannot establish
        // that, so the flag is a request, not an override.
        void run(const bool print_deductions, const bool export_derivations, const bool suppress_repetition, const bool silent = false, const bool incremental = false);
        void apply_rule(const network::Node& rule, network::Node condition);
        void profiler_reset_epoch()
        {
            _prof.reset_epoch();
            _nn_cache.clear();
        }

        // On-demand profiler summary (command .prof): the counter block
        // plus top-N sections (relations by scan/match, rules by
        // application/created facts). reset_after additionally zeroes the
        // counters, starting a fresh measurement window (.prof reset).
        void profiler_dump(bool reset_after = false);

        // --- Rendering helpers shared by the console and the export ---

        // Count and report one detected contradiction. Every catch site in
        // the engine calls this and nothing else, so what a contradiction
        // costs and how it is presented is decided in one place.
        //
        // The SAME instantiation arrives several times: semi-naive
        // evaluation seeds a rule once per newly derived premise, and a
        // contradiction has no result node that hash-consing could
        // collapse the way it collapses a repeated deduction. Reporting
        // each arrival made the number of violations depend on the
        // evaluation strategy -- 10 semi-naive, 6 classic, 3 real -- and
        // that number is the headline of the Wikidata work.
        //
        // Repeats used to be dropped by a per-run hash set, which is why a
        // contradiction came back on every later input line: the set was
        // cleared at the start of each run, and unlike a derived fact there
        // was nothing in the GRAPH to make the second run quiet. There is
        // now -- see record_contradiction.
        void report_contradiction(const contradiction_error& error);

        // Write the contradiction into the graph, and answer whether it was
        // already there. The record is the refuted set of the facts that
        // MATCHED: "these statements do not hold together". Nothing is
        // retracted -- each of them stays asserted and keeps answering
        // queries -- and nothing is created but the set node, because every
        // member is a fact the unification just matched.
        //
        // A set constant is content-addressed and order-independent, so the
        // same contradiction yields the same node however it was reached.
        // That is where the quiet second run comes from, and it is why the
        // record is keyed on the FACTS rather than on (rule, bindings): two
        // rules contradicting on the same statements make the same claim, and
        // they report once between them.
        //
        // Members that matched no fact -- a `!=` guard, a negation, an `≈` or
        // a path condition -- contribute nothing. Instantiating them would
        // ASSERT them (instantiate_fact ends in Zelph::fact), so a `!=`
        // condition would enter `(bright != dark)` as a claim of the core
        // `!=` predicate that nobody made.
        //
        // Called BEFORE the output lock is taken: Zelph::fact takes the
        // network locks, and deduce establishes network-then-output as the
        // order (see "// _mtx_network released" in reasoning_deduce.cpp).
        bool record_contradiction(const contradiction_error& error);

        // One set node per DISTINCT contradiction, which is a memory cost that
        // grows with the data: the P361/P527 asymmetry rule finds 355 073 of
        // them on the medium Wikidata artifact. Switchable, like every other
        // acceleration that trades memory away -- see `.fact-stores`.
        void record_contradictions(bool on) { _record_contradictions = on; }
        bool record_contradictions() const { return _record_contradictions; }

        // Prune mode: record what the matched CONDITION denotes under these
        // bindings. Called from every terminal site of evaluate(), which is
        // why it is a function rather than the three copies it replaces.
        //
        // With a target variable set (`.prune-nodes A (...)`) the condition is
        // not read at all: the victims are that variable's BINDING, and the
        // conditions are the filter that selected it. See prune_nodes.
        void collect_prune_targets(Node condition, const Variables& bindings, Node parent);

        // The rendered "!" as a MARKED identifier -- the conclusion of a
        // contradiction, in the same form node_to_string produces for any
        // other name, so console and export read it the same way.
        std::string contradiction_symbol() const;
        std::string known_contradiction_note() const;

        // The contradiction records the graph holds. They are the refuted
        // nodes that are SET constants: record_contradiction marks exactly
        // those -- set(matched) over the facts that matched -- while an
        // ordinary refutation, `¬(a p b)`, marks the relation node of a fact,
        // which never hashes back to its own members. A rule with a SINGLE
        // condition has no condition set to point at and is therefore not
        // recorded at all; it is announced on every run and counted there.
        std::size_t count_contradiction_records() const;

        // The premises of one rule instantiation, rendered individually.
        // The printed line shows the condition SET -- "{(a p b) (b p c)}" --
        // because that is what the rule's subject IS; a consumer of the
        // export should not have to take those braces apart again, so the
        // export asks for the elements. A single-condition rule has no set,
        // and then this is that one condition.
        std::vector<std::string> render_premises(Node condition, const Variables& variables, Node parent) const;

        // --- Deduction focus (implemented in reasoning.cpp) ---

        // Capture nodes materialized by user input (fact nodes plus their
        // subjects and objects) into the input-focus set, via the fact
        // creation observer. Idempotent; run() ends the capture itself, so
        // the observer never overlaps with the semi-naive delta observer.
        void begin_input_capture();
        // When on, deduction printing is restricted to deductions whose
        // subject or rule is in the input-focus set ("focus mode").
        void set_deduction_filter(bool on);
        // Temporarily suppress input capture (imports): begin_input_capture
        // becomes a no-op while suppressed, and an active capture is closed
        // WITHOUT contributing to the focus set -- imported statements are
        // not "entered by the user".
        void suppress_input_capture(bool on);
        // Reset the accumulated focus anchors (mode switch; .reset gets a fresh Reasoning instance anyway).
        void clear_input_focus();

        // --- Implemented in reasoning_pruning.cpp ---

        /// Is `fact` the relation-type declaration of a CORE predicate?
        bool is_core_declaration(Node fact) const;

        void prune_facts(Node pattern, size_t& removed_count);

        // `target_var` names whose bindings die, and 0 keeps the single-fact
        // reading in which the pattern's one variable does. It is the VARIABLE
        // NODE of this very pattern, not a name: a variable is quantified per
        // statement and many nodes may display one letter (`be16650`), so the
        // command resolves the letter against the pattern it just built and
        // hands the node over. That is also what makes a conjunction usable --
        // it has one variable per condition, and this says which is meant.
        void prune_nodes(Node pattern, Node target_var, size_t& removed_facts, size_t& removed_nodes);
        void purge_unused_predicates(size_t& removed_facts, size_t& removed_predicates);

        // --- Implemented in reasoning_seminaive.cpp ---

        void set_seminaive(bool on);
        bool seminaive() const;
        void set_seminaive_check(bool on);
        bool seminaive_check() const;

        // max_depth 0 = unlimited (terminates via path exclusion and memoization;
        // hash-consing makes shared subterms shared subproofs, so the result is
        // a DAG: identical facts share one ProofNode instance).
        std::shared_ptr<ProofNode> explain(Node fact, std::size_t max_depth) const;

    private:
        // --- Implemented in reasoning.cpp (orchestration) ---

        std::shared_ptr<std::vector<Node>> optimize_order(const adjacency_set& conditions, const Variables& current_vars, int depth);
        // True when an iteration banner may be printed. The banners are a
        // progress indicator, not data: a saturating run can execute
        // thousands of iterations per second, and printing one line each
        // buries whatever the user actually asked to see. With logging on
        // they ARE the data, so every one is kept.
        bool progress_due();

        bool resolve_guard_side(Node item, const Variables& variables, Node& out) const;
        bool contradicts(const Variables& variables, const Variables& unequals) const;
        bool guards_unresolved(const Variables& variables, const Variables& unequals) const;
        bool guard_side_unbound(Node item, const Variables& variables) const;
        void end_input_capture();

        // --- Implemented in reasoning_evaluate.cpp ---

        void evaluate(RulePos rule, ReasoningContext& ctx, int depth);
        bool is_negated_condition(Node condition, int depth);
        void refuse_condition(Node condition, const std::string& message);
        bool condition_contains_negation(Node condition, int depth);

        // --- Implemented in reasoning_deduce.cpp ---

        void deduce(const Variables& variables, Node parent, const int depth, ReasoningContext& ctx, double confidence);
        bool consequences_already_exist(const Variables&     condition_bindings,
                                        const adjacency_set& deductions,
                                        Node                 parent,
                                        const int            depth);

        // Does this node reach the input focus -- as itself, or, when the
        // deduction CONSTRUCTED it, through what it was constructed of?
        // "((x f y) q c)" is a statement about x and y, which the user
        // entered; the composed subject node itself never was and never can
        // be, so the direct test alone hid every rule whose consequence has a
        // composed subject -- with no unbound variable anywhere in sight.
        //
        // The depth bound is what keeps focus a filter: an anchor is a
        // component of an ENTERED statement and therefore sits shallow, while
        // the terms a computation builds nest arbitrarily deep and are exactly
        // what focus exists to suppress.
        bool in_input_focus(Node node, int depth_left) const;

        // How far in_input_focus descends into a constructed subject. One
        // level covers the shape that motivated it, "((x f y) q c)"; the
        // value is a measured trade-off, see the tests.
        static constexpr int _focus_subject_depth{1};

        // Is this deduction a RULE -- a statement whose predicate is `=>`?
        // It decides two things a fact deduction settles differently: the
        // variables inside it are quantified by that INNER rule and must
        // survive instantiation as variables instead of becoming fresh nodes,
        // and rebuilding it needs rebuild_rule, not instantiate_fact.
        bool deduction_is_rule(Node deduction) const;

        // Rebuild the RULE `pattern` under `variables` and return the `=>`
        // fact. `created` reports whether anything new was added; false means
        // the identical rule was already in the graph. 0 is returned when the
        // pattern does not decompose into a rule at all.
        //
        // A rule is not just a fact: its subject is either one condition
        // pattern or a conjunction SET node, and that set node is created
        // rather than hash-consed, its members hang off it as separate PartOf
        // facts, and the tags that make the engine read it as a conjunction --
        // or a member as a negation -- are facts of their own. instantiate_fact
        // reproduces none of that, which is why deriving a rule needs its own
        // construction. Call it under _mtx_network.
        Node rebuild_rule(Node pattern, const Variables& variables, int depth, Node parent, bool& created);

        // Every variable of a rule, conditions and consequences alike. Unlike
        // collect_variables this descends through the conjunction SET node,
        // which carries no structure of its own -- so the variables of a
        // multi-condition rule are reachable at all.
        std::unordered_set<Node> rule_variables(Node rule, Node parent, int depth);

        // One condition of a derived rule, under the bindings: the
        // instantiated pattern plus the tags that describe how the engine has
        // to read it (negation, and a nested conjunction rebuilt as a set).
        Node rebuild_condition(Node pattern, const Variables& variables, int depth);

        // The conjunction set node whose members are exactly `members`, or 0.
        // Deriving a rule that is already there must not build a second set
        // node for it: the set node is created, not hash-consed, so nothing
        // would collapse the two, the rule would be re-derived on every run,
        // and the fixpoint would never be reached.
        Node find_conjunction_set(const std::unordered_set<Node>& members) const;

        // --- Implemented in reasoning_neural.cpp ---
        const NeuralNet* compiled_net(Node net_node, int depth);
        void             report_unusable_net(Node net_node, const std::string& why);
        void             evaluate_neural(Node condition, const RulePos& rule, ReasoningContext& ctx, int depth, bool negated);
        void             evaluate_closure(Node condition, const RulePos& rule, ReasoningContext& ctx, int depth, bool negated);
        void             proceed_after_condition(const RulePos& rule, ReasoningContext& ctx, int depth, std::shared_ptr<Variables> vars, std::shared_ptr<Variables> uneqs, double confidence);

        // --- Implemented in reasoning_seminaive.cpp ---

        // Delta-driven fixpoint loop (semi-naive evaluation). Returns the
        // number of safety-net violations found (always 0 unless
        // _seminaive_check is active and delta seeding missed a derivation).
        // seed: when non-null, the facts to start from instead of the classic
        // first pass (see the incremental parameter of run()).
        uint64_t run_fixpoint_seminaive(bool silent, const std::vector<std::pair<Node, Node>>* seed = nullptr);

        // --- Members ---

        std::atomic<bool>                     _done{false};
        std::unique_ptr<io::DerivationExport> _export;
        std::atomic<uint64_t>                 _running{0};
        bool                                  _print_deductions{true};
        bool                                  _export_derivations{false};
        std::atomic<bool>                     _contradiction{false};
        chrono::StopWatch                     _stop_watch;
        std::atomic<size_t>                   _skipped{0};
        std::mutex                            _mtx_output;
        std::mutex                            _mtx_network;
        std::atomic<int>                      _total_matches{0};
        std::atomic<int>                      _total_contradictions{0};
        // How many contradiction records the graph HELD when this run began.
        // A run that reports nothing and says nothing else is indistinguishable
        // from a clean graph -- and after a .load of a network that was saved
        // after a run, that is every contradiction in it, because the record is
        // a fact and travels with the file.
        //
        // Read from the graph rather than counted as the run meets them: a
        // record is content-addressed, so ONE contradiction is one record
        // however many rule instantiations reach it. A symmetric rule such as
        // (A p B, B p A) => ! matches twice over the same pair and would
        // otherwise report two. Read at the START, so a contradiction this run
        // announces is not also counted as one that was already there.
        std::size_t _records_at_run_start{0};
        // Whether a contradiction is written into the graph. See
        // record_contradiction for what it costs and why it can be switched
        // off; the per-run hash set that used to sit here is gone with it.
        bool                                     _record_contradictions{true};
        std::unique_ptr<concurrency::ThreadPool> _pool;
        std::string                              _export_file;
        bool                                     _prune_mode{false};
        bool                                     _prune_nodes_mode{false};
        Node                                     _prune_target_var{0};
        std::unordered_set<Node>                 _facts_to_prune;
        std::unordered_set<Node>                 _nodes_to_prune;
        std::vector<std::shared_ptr<Variables>>* _query_results{nullptr};
        ReasoningProfiler                        _prof;
        std::unordered_set<Node>                 _input_captured; // raw fact nodes materialized while parsing input
        std::unordered_set<Node>                 _input_focus;    // reduced focus set: top-level inputs + their components
        bool                                     _deduction_filter{false};
        bool                                     _capturing{false};
        int                                      _capture_suppress_depth{0}; // > 0: input capture suppressed (nested imports)

        // --- Cross-run delta (implemented in reasoning.cpp) ---
        //
        // Facts created since the last run(), recorded by the same observer
        // that feeds the input focus. A normal run rebuilds its knowledge of
        // the graph from scratch (its first iteration is a classic pass), so
        // it consumes and discards this; .run-delta seeds from it instead,
        // which is what turns "run again after adding a little" from a cost
        // in the size of the graph into a cost in the size of the addition.
        void                               arm_delta_recorder();
        std::vector<std::pair<Node, Node>> _delta_since_run;
        std::mutex                         _mtx_delta_since_run;
        // False whenever the record is not a faithful account of everything
        // added since the last run: before the first run, while an import is
        // in progress (those facts are bulk knowledge, not an increment), and
        // once the record has outgrown its cap. Seeding then falls back to a
        // classic pass, so an invalid record costs time, never correctness.
        bool _delta_valid{false};
        // A recorded entry is 16 bytes, so this caps the record at ~16 MB.
        // Anything that adds a million facts between two runs is a bulk load,
        // for which a classic pass is the right answer anyway.
        static constexpr size_t _max_delta_entries{1'000'000};
        // Rule count observed at the end of the last run. A changed rule set
        // invalidates delta seeding -- a new rule has to see the old facts --
        // so an incremental request falls back to a classic pass.
        size_t _rules_at_last_run{0};

        // --- Neural (≈) support ---
        // Rate limit for the iteration banners; see progress_due().
        std::chrono::steady_clock::time_point _progress_last{};

        Node _nn_pred{0};        // node named "nn" in lang "zelph", 0 = feature inactive
        Node _nn_layers_pred{0}; // node named "nn-layers" in lang "zelph"

        // --- Transitive path conditions (P⁺ / P∗) ---
        // Named nodes in language "zelph", cached exactly like _nn_pred: a
        // path condition IS the tag fact (pattern closure mode), so the
        // predicate identifies it and the object says which closure. None of
        // them may be a core node -- core ids are positional and frozen by
        // every .bin ever written; see CLAUDE.md, "What must be a CORE node".
        Node                                       _closure_pred{0};      // "closure"
        Node                                       _closure_one_plus{0};  // "one-or-more"  (P⁺)
        Node                                       _closure_zero_plus{0}; // "zero-or-more" (P∗)
        std::map<Node, std::unique_ptr<NeuralNet>> _nn_cache;             // compiled nets, cleared per epoch

        // Which nets have already been reported as unusable. NOT cleared with
        // the cache: that happens once per input line, and a rule consulting
        // a misspelled net would then repeat its warning for every line of an
        // import. Once per session is what makes it readable.
        std::set<Node> _nn_reported;

        // Which conditions have already been refused. Same lifetime and same
        // reason as _nn_reported: a condition is refused for a property of its
        // own shape, which does not change between two candidate bindings, so
        // reporting per evaluation buries the message under its own repeats --
        // ten identical lines for a three-fact network, and one per binding on
        // anything real.
        std::set<Node> _refused_conditions;

        bool _seminaive{true};
        bool _seminaive_check{false};
    };
}