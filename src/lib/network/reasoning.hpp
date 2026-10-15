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

#include "concurrency/thread_pool.hpp"
#include "contradiction_error.hpp"
#include "io/derivation_export.hpp"
#include "io/output.hpp"
#include "network_types.hpp"
#include "neural.hpp"
#include "reasoning_profiler.hpp"
#include "unification.hpp"
#include "zelph.hpp"

#include <ankerl/unordered_dense.h>
#include <zelph_export.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
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
        Node                 rule{0}; // the `=>` node a contradiction names; 0 for a query
    };

    // --- Free helper functions (implemented in reasoning.cpp) ---

    // Who instantiates a statement: a firing writes the rule's consequence
    // into the data, a construction writes the rule that a rule generator
    // derives.
    enum class RecipeMode
    {
        Firing,
        Construction
    };

    // A refuted fact that the prediction of a firing met where the
    // consequence claims nothing (Reasoning::nests_refuted_fact), with the
    // path of its walk: the patterns beginning at the top of the consequence
    // and proceeding downward to the one that holds the fact. The
    // instantiation takes the fact exactly as it stands at that place and at
    // no other.
    struct LookedUpFact
    {
        std::vector<Node> path;
        Node              fact{0};
    };
    using LookedUpFacts = std::vector<LookedUpFact>;

    // An instantiation of a single statement -- a consequence triggered during
    // a firing, the rule a construction writes. A collection of the rule's own
    // text (Zelph::is_rule_template) is a Skolem function symbol: the
    // instantiation substitutes it with its term, a collection whose
    // identifier is computed from the template and the key
    // (Network::recipe_id), ensuring that the same statement, under identical
    // binding, always resolves to the same node regardless of how frequently
    // it is instantiated. The key corresponds to the binding of the variables
    // the instantiation meets (statement_variables in reasoning.cpp), computed
    // at the first template met; a statement that holds none incurs no cost
    // for it.
    struct Recipe
    {
        Node                      statement{0};
        const Variables*          binding{nullptr};
        RecipeMode                mode{RecipeMode::Firing};
        Node                      parent{0};        // the rule's condition part, to read the statement as deduce reads it
        std::vector<Node>*        created{nullptr}; // what came into existence, for marking: a construction's, or a firing's that writes a rule its consequence mentions
        std::unordered_set<Node>* filled{nullptr};  // Firing: the bucket terms the run has filled (bucket_term)
        std::vector<Node>*        values{nullptr};  // Firing: the value built for every collection of the rule's text, under this binding (what the claim of the instance enters)
        const LookedUpFacts*      refuted{nullptr}; // Firing: the refuted facts the consequence holds where it claims nothing, each in its specific place (Reasoning::refuted_consequence); instantiate_fact takes each one exactly as it is, at that precise place

        mutable std::optional<Node> key;
    };

    // Where an instantiation meets a node within the statement. `Into` is the
    // object of a membership the statement asserts -- what it writes into.
    // Within a construction, the object of a membership within a condition of
    // the written rule is `Member` instead -- a condition performs no writing
    // -- and `member` serves as the subject of that membership.
    enum class Position
    {
        Value,
        Into,
        Member
    };

    struct Place
    {
        Position position{Position::Value};
        bool     condition{false}; // under a condition of a rule the statement holds
        Node     member{0};
    };

    // Replace variables within a fact pattern in a recursive manner to produce a
    // concrete fact. Used by deduce and during the construction of a derived
    // rule. A container is instantiated as `recipe` and `place` decide (via
    // container_plan in reasoning.cpp); if no recipe exists, every container
    // is kept.
    Node instantiate_fact(Zelph* z, Node pattern, const Variables& variables, int depth, std::vector<Node>& history, const Recipe* recipe = nullptr, const Place& place = {});

    // The node instantiate_fact would return for `pattern` under `variables`,
    // calculated without any construction: because node ids are content
    // hashes and a collection built by a rule has an id equal to its recipe,
    // the identifier is established before its creation. Returns 0 when the
    // result would not be ground. It follows exactly the same route as
    // instantiate_fact and must stay in sync with it: both read
    // container_plan.
    //
    // Using `keep_variables`, a variable lacking a binding remains
    // unchanged, as instantiate_fact preserves it, instead of making the
    // result 0: this allows the prediction to encompass patterns that retain
    // their own variables, such as the parts of a rule constructed by a rule
    // generator, or the variable that stays a member of a collection created
    // during a firing.
    Node ground_instance(const Zelph* z, Node pattern, const Variables& variables, int depth, std::vector<Node>& history, const Recipe* recipe = nullptr, const Place& place = {}, bool keep_variables = false);

    // The variables that an instantiation of the recipe's statement meets:
    // the ones it substitutes, outside every container it keeps (the
    // instance walk, reasoning.cpp).
    std::unordered_set<Node> statement_variables(const Zelph* z, const Recipe& recipe, int depth);

    // The members of the container `node` that a firing's instantiation
    // meets at a value position, read as container_plan reads them.
    // `rebuilt`: the instantiation rebuilds them under the binding;
    // otherwise, they stand in the instance exactly as written, in a
    // conjunction set that the firing preserves. False in the case of a node
    // whose members the firing does not write: an atom, a value, a
    // constant, a term.
    bool container_members_met(const Zelph* z, Node node, const Recipe& recipe, std::unordered_set<Node>& members, bool& rebuilt);

    // Whether the instance a firing writes for the recipe's statement keeps a
    // variable: a container the firing keeps as written -- the condition set
    // of a rule the statement mentions -- contains one within its text,
    // making the instance rule text. Read off the statement alone, via the
    // instance walk, before anything is built. `in_values` receives the
    // variables the walk encounters within a rule inside a value the firing
    // builds, which the instance keeps where the firing does not bind them.
    bool keeps_rule_variable(const Zelph* z, const Recipe& recipe, int depth, std::unordered_set<Node>& in_values);

    // The key for the recipe's terms: the binding of statement_variables,
    // presented as sorted (variable, value) pairs (Network::recipe_key).
    // Calculated a single time per recipe.
    Node recipe_key(const Zelph* z, const Recipe& recipe, int depth);

    // The stand-ins a firing of `consequence` has to be matched with
    // (Unification): a bucket represents its term, while a collection of the
    // rule's text that the firing replaces with a term of its own
    // represents any such term -- the specific one becomes clear only within
    // the whole binding, hence a match via it must be confirmed by the
    // prediction. `binding` solely indicates what a variable predicate
    // corresponds to.
    void firing_stand_ins(const Zelph* z, Node consequence, Node parent, const Variables& binding, StandIns& out);

    // The conjunction set whose members are exactly `members`, or 0.
    // Deriving a rule that already exists must not build a second set node:
    // the set node is created, not hash-consed, thus no merging would occur,
    // the rule would be re-derived in each run, and the fixpoint would never
    // be reached.
    Node find_conjunction_set(const Zelph* z, const std::unordered_set<Node>& members);

    // Recursively collect all variable nodes from a fact pattern.
    // Used to detect "fresh variables" that appear only in rule consequences.
    ZELPH_EXPORT void collect_variables(Zelph* z, Node pattern, std::unordered_set<Node>& vars, int depth, std::vector<Node>& history);

    // At any level of nesting, the variables contained within the members of
    // every set pattern that `pattern` holds (Zelph::set_pattern_members):
    // those unify_nodes binds when it pairs the members with a set
    // constant's. collect_variables ignores them within a rule's text, as a
    // firing substitutes none.
    void collect_set_pattern_variables(Zelph* z, Node pattern, std::unordered_set<Node>& vars);

    // --- Proof reconstruction (reasoning_explain.cpp) --------------------------
    // Rebuild a justification for an asserted fact from the saturated graph.
    // Nothing is tracked during inference: a derived fact keeps a rule
    // instantiation whose consequence unifies with it and whose conditions
    // hold as long as nothing was taken away since -- a premise pruned, the
    // rule removed, a negated premise that came to hold later -- and this
    // backward search finds one, using the same unification machinery that
    // runs forward. Read-only: no graph structure is created. Nothing is
    // recorded so that the forward pass costs nothing; the search pays
    // instead, once per fact it enters (see reasoning_explain.cpp).
    struct ProofNode
    {
        enum class Status
        {
            Derived,       // justified by `rule` via `premises` and `absent`
            Axiom,         // asserted, and no rule consequence unifies: an input fact
            Unfounded,     // asserted, rule consequences unify, yet no
                           // instantiation satisfies the CURRENT graph (for
                           // instance, a NAF premise that has subsequently
                           // turned true), or each one of the search results
                           // traces back through the fact's own cycle (the
                           // SCC rule, reasoning_explain.cpp)
            RulePattern,   // the node exists only because a rule was written
                           // with this statement as a ground pattern -- nobody
                           // claimed it, so there is nothing to justify
            RuleMentioned, // the rule being queried, which a statement
                           // mentions: not in force, and maybe asserted as
                           // well (leaf_status)
            Truncated,     // not expanded: depth limit reached
            Mentioned      // the mark of RulePattern where the node stands as
                           // a mention (Zelph::is_mention): a statement wrote
                           // it down without claiming it
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

        // Per path condition in `walked`, the facts its walk steps over that a
        // rule establishes, along with their proofs. These serve as premises
        // for the step, just as the others do, and the tree outputs one
        // beneath its [closure] line when the proof ends at a leaf situated on
        // a cycle (`cycle_leaf`): such a leaf behind a walk was never
        // displayed, and the step was interpreted as being inferred from
        // axioms and a walk whose edge is anchored to the step itself.
        std::vector<std::vector<std::shared_ptr<ProofNode>>> walk_edges;

        // An Unfounded leaf, generated by the SCC rule within a cycle: the
        // fact is one of several in its component, or
        // an instantiation found for it relies on the fact itself, and
        // each derivation the search found for it runs back through its
        // own cycle (reasoning_explain.cpp). A leaf of a fact not part of
        // any cycle -- an axiom, or an asserted fact no instantiation of
        // which holds -- does not bear it.
        bool cycle_leaf = false;

        // Whether the search found a SECOND justification for this fact: an
        // additional instantiation where the premises hold independently of
        // the fact. Only one is displayed -- pursuing each one would multiply
        // the effort at every level and answer a query that no one has posed.
        // The flag enables the displayed one to no longer be considered the
        // sole available one, distinguishing between "the evidence" and "some
        // evidence". Set at the root only; see reasoning_explain.cpp.
        bool more_justifications = false;

        // The instantiation that justifies this step (Derived only). The
        // positive premises are already ground nodes, but a NAF condition
        // usually has no node -- absence is why it holds -- so `absent`
        // carries the rule's PATTERN and needs these bindings to be
        // rendered as the concrete premise that was checked. Variables
        // occurring only inside the negation stay unbound: that is what
        // "for no D" means, and it must remain visible as such.
        Variables bindings;

        ProofNode()                            = default;
        ProofNode(const ProofNode&)            = default;
        ProofNode(ProofNode&&)                 = default;
        ProofNode& operator=(const ProofNode&) = default;
        ProofNode& operator=(ProofNode&&)      = default;

        // Releasing a premise released its premises in turn, one call frame
        // per level, and relinquishing a proof as deep as a chain of 100 000
        // facts overflowed the stack. Premises that nothing else holds are
        // dismantled in a loop instead.
        ~ProofNode()
        {
            std::vector<std::shared_ptr<ProofNode>> pending = std::move(premises);
            for (auto& edges : walk_edges)
                for (auto& q : edges)
                    pending.push_back(std::move(q));
            while (!pending.empty())
            {
                std::shared_ptr<ProofNode> p = std::move(pending.back());
                pending.pop_back();
                if (p && p.use_count() == 1)
                {
                    for (auto& q : p->premises)
                        pending.push_back(std::move(q));
                    p->premises.clear();
                    for (auto& edges : p->walk_edges)
                        for (auto& q : edges)
                            pending.push_back(std::move(q));
                    p->walk_edges.clear();
                }
            }
        }
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
        //
        // quiet: during the run, no output is produced, and what it met is
        // kept as data: met_contradictions, and defeated_negations for the
        // facts that lost the negation they were derived under. Check mode's
        // negation check returns its results rather than triggering an
        // exception; however, the semi-naive completeness violation continues
        // to raise, and its message carries that list. A program retrieves
        // both via (zelph/run :quiet), and printing them as well would put
        // zelph syntax into the program's own output.
        //
        // derived: each fact that the run derives, along with its rule and
        // premises, is retained by the run for take_derived_facts.
        void run(const bool print_deductions, const bool export_derivations, const bool suppress_repetition, const bool silent = false, const bool incremental = false, const bool quiet = false, const bool derived = false);
        void apply_rule(const network::Node& rule, network::Node condition);
        void profiler_reset_epoch()
        {
            _prof.reset_epoch();
            _nn_cache.clear();
        }

        // A user removal took a fact that could have been derived, with no
        // record indicating which: the graph is no longer known to be a
        // fixpoint, and the next .run-delta constitutes a full pass. Called
        // by the removal commands (.remove, .prune-facts, .prune-nodes,
        // .cleanup) when they remove or withdraw at least one fact.
        void note_user_removal();
        // The predicates of the facts that were present at the end of the last
        // run and have since been removed, or whose claim has been revoked by
        // a drop, excluding the rule text the engine writes (pattern markings,
        // conjunction and negation tags). Cleared by every run, empty while the
        // record of additions is not valid.
        std::vector<Node> removed_predicates_since_run() const;
        // The facts recorded as created since the last run; the entries of a
        // fact removed since then have vanished.
        size_t delta_entries() const;
        // The number of runs that have concluded since the engine was
        // constructed. A caller compares two readings to tell whether the
        // code it ran (a keyword handler) ran the engine itself.
        size_t completed_runs() const;
        // Whether the graph continues to be the fixpoint from the prior run:
        // that run reached it (a .run-once has not when it derived
        // something, nor while negation records are kept, as it never
        // re-tests them), the record of additions is valid and holds no
        // entry, no fact that was present at its end has been removed, no
        // drop has given a reported negation record back since, and the rule
        // count matches. A replacing load discards the record, and a cluster
        // that was created and then dropped since leaves no entry within it.
        bool unchanged_since_last_run() const;

        // A `=>` fact generated by the assertion API -- zelph/fact in Janet,
        // zelph_fact in the C ABI -- where the consequence features a
        // predicate that no condition binds (unbound_predicate). The call is
        // unable to reject it directly: a generator's inner rule and a rule
        // that a statement holds exhibit this shape until the call that holds
        // them has run, and that call occurs at a later time. `writer` names
        // the call in the refusal; it must persist beyond the lifetime of the
        // record (a string literal). `claimed` says that the fact was present
        // before the call, out of force as a mention, and that the call's
        // claim brought it into force.
        void note_fact_rule(Node rule, const char* writer, bool claimed = false);
        // Refuses every noted `=>` fact that is still a rule in force
        // according to get_rules, and whose predicate remains unbound, then
        // returns the refusals, each on its own line and prefixed by the
        // writer; returns an empty result if no such facts exist. The fact
        // generated by the call is eliminated; the one that was claimed
        // maintains its position and reverts to being a mention
        // (Zelph::restore_mention). A noted node that something holds passes,
        // while one that has been removed is ignored. Afterwards, the record
        // becomes empty. Invoked when the outermost evaluation of Janet code
        // ends, and also called by run() before any other action, which
        // subsequently raises the refusals instead of running. The `.save` and
        // `.save-predicates` commands also trigger it before writing to the
        // file, and raise the refusals instead of carrying out the save.
        std::string settle_fact_rules();

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

        // What record_contradiction did: wrote a new record, found the record
        // already present within the graph, or kept none (see the reasons
        // below).
        enum class ContradictionRecord
        {
            New,
            Existing,
            None
        };

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
        // a path condition -- contribute nothing. Each remaining member is
        // looked up, never built: built through Zelph::fact, a `!=` condition
        // would enter `(bright != dark)` as a claim of the core `!=` predicate
        // that nobody made, and a member that names a refuted fact is refused.
        //
        // Called BEFORE the output lock is taken: Zelph::fact takes the
        // network locks, and deduce establishes network-then-output as the
        // order (see "// _mtx_network released" in reasoning_deduce.cpp).
        //
        // Using `conditions`, it also gathers the instances of the positive
        // conditions, sorted by node id, regardless of whether records are
        // kept: a quiet run returns them for every contradiction it meets.
        // A single condition outside a set is likewise gathered, even though
        // it is recorded only alongside a claimed fact (see
        // count_contradiction_records).
        ContradictionRecord record_contradiction(const contradiction_error& error, std::vector<Node>* conditions = nullptr);

        // A single contradiction encountered during a quiet run appears
        // once, even if the run met it repeatedly (keyed by rule and
        // bindings), ordered by the time it was first met. `conclusion`
        // corresponds to the result of
        // contradiction_error::get_conclusion.
        struct MetContradiction
        {
            Node                rule{0};
            std::vector<Node>   conditions; // instances of its positive conditions, by node id
            Node                conclusion{0};
            ContradictionRecord record{ContradictionRecord::None};
            Variables           bindings;
            std::string         refusal; // `contradiction_error::get_reason`
        };
        const std::vector<MetContradiction>& met_contradictions() const { return _met_contradictions; }

        // A fact derived by a run started with `derived`, following the
        // sequence in which the run derived them. The premises consist of
        // the facts that its positive conditions matched, ordered by node id,
        // as `conditions` holds them for a contradiction, yet looked up,
        // never created; `absent` holds the patterns of its negated
        // conditions, which the firing found absent, and `bindings` turns
        // them into what was tested.
        struct DerivedFact
        {
            Node              fact{0};
            Node              rule{0}; // the => fact
            std::vector<Node> premises;
            std::vector<Node> absent;
            Variables         bindings;
        };
        // Hands over what the last run collected and forgets it. That
        // memory expands according to what the run derived, and it is freed
        // after the caller has read it.
        std::vector<DerivedFact> take_derived_facts();

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
        // The sole exception arises when a derived refutation meets a
        // claimed fact: that fact becomes a second member, and the record is
        // written. Written as a condition set with one constituent -- the
        // tagged spelling, or any rule zelph/rule builds -- the identical
        // condition has a member to point at and is recorded in the same
        // manner as any other.
        std::size_t count_contradiction_records() const;

        // The premises of one rule instantiation, rendered individually.
        // The printed line shows the condition SET -- "{(a p b) (b p c)}" --
        // because that is what the rule's subject IS; a consumer of the
        // export should not have to take those braces apart again, so the
        // export asks for the elements. A single-condition rule has no set,
        // and then this is that one condition.
        std::vector<std::string> render_premises(Node condition, const Variables& variables, Node parent) const;
        // The elements of a condition set, located in the manner that
        // render_premises locates them; empty for a single condition
        // outside a set.
        std::vector<Node> condition_elements(Node condition) const;
        // Whether the condition of a rule qualifies as one that matches a
        // fact: not a negation, a `!=` guard, an `≈`, or a path condition.
        bool matches_a_fact(Node element);

        // --- Deduction focus (implemented in reasoning.cpp) ---

        // Capture nodes materialized by user input (fact nodes plus their
        // subjects and objects) into the input-focus set, via the fact
        // creation observer. Idempotent; run() ends the capture itself, so
        // the observer never overlaps with the semi-naive delta observer.
        void begin_input_capture();
        // Reduce what the capture holds so far to just its top-level inputs,
        // and incorporate those into the focus, mirroring how the end of a
        // capture operates, then continue the act of capturing. A query
        // removes its own pattern immediately after being answered, and the
        // reduction must examine that pattern while it remains within the
        // graph.
        void settle_input_capture();
        // When on, deduction printing is restricted to deductions whose
        // subject or rule is in the input-focus set ("focus mode").
        void set_deduction_filter(bool on);
        // When off, a run does not write the notice accounting for what it
        // withheld. The count is still kept and still readable below: what is
        // switched off is the sentence, not the bookkeeping, so a caller that
        // reports it some other way -- a mark on the prompt -- can.
        void set_deduction_notice(bool on);
        // When active, a run that prints no deductions still prints the
        // contradictions it finds. The '?' prefix performs inference
        // during such a run, ensuring the derivation remains excluded from
        // the answer, and a contradiction was dropped along with it:
        // counted, recorded, but never shown.
        void set_contradictions_printed(bool on) { _contradictions_printed = on; }
        // Deductions the most recent run derived and did not show.
        std::size_t deductions_withheld() const { return _skipped.load(); }
        // Temporarily suppress input capture (modules): begin_input_capture
        // becomes a no-op while suppressed, and an active capture is closed
        // WITHOUT contributing to the focus set -- a library's own definitions
        // are not what the user is looking at. Only `.import` suppresses; the
        // lines of a script named on the command line are the session's own
        // and do anchor the filter (see ScriptRole in repl_state.hpp).
        void suppress_input_capture(bool on);
        // Reset the accumulated focus anchors (mode switch; .new gets a fresh Reasoning instance anyway).
        void clear_input_focus();

        // --- Implemented in reasoning_pruning.cpp ---

        /// Is `fact` the relation-type declaration of a CORE predicate?
        bool is_core_declaration(Node fact) const;

        void prune_facts(Node pattern, size_t& removed_count);

        /// Each condition and consequence in a rule -- the statement itself,
        /// a member of the rule's condition set, a statement embedded within
        /// one of those, or a member of a collection in the rule's text.
        /// Pruning such a fact withdraws the claim while preserving the
        /// statement as the rule's pattern; withdraw_claims performs this
        /// action on multiple facts simultaneously, given what this returned
        /// for all of them.
        adjacency_set rule_parts() const;
        void          withdraw_claims(const std::vector<Node>& facts, const adjacency_set& parts) const;
        /// What removing a fact that is not a component of a rule leaves standing
        /// in `parts`; see the definition.
        adjacency_set kept_by_removal(const adjacency_set& parts) const;
        /// The ground form's removal of that fact, along with the statements that
        /// hold it; it yields the count of nodes removed, just as remove_node does.
        size_t remove_fact_and_holders(Node fact, const adjacency_set& parts) const;

        // One fact that was claimed has been withdrawn, as the ground form
        // of .prune-facts manages it: `removed` if the node is absent along
        // with the statements that held it, `unclaimed` if it stays as a
        // rule's pattern or a mention (withdraw_claims), `none` if no claim
        // was made on it. `count` represents the number reported by
        // .prune-facts.
        enum class FactPrune
        {
            none,
            removed,
            unclaimed
        };
        struct PrunedFact
        {
            FactPrune outcome = FactPrune::none;
            size_t    count   = 0;
        };
        PrunedFact prune_fact(Node fact);

        // The statements that claim `fact` through nesting, ordered by
        // node: what prune_fact takes along first, read without changing
        // anything.
        std::vector<Node> holders(Node fact) const;

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
        // Whether an engine built from this point forward begins in check mode.
        // A setting of the PROCESS, read exclusively by the constructor; the
        // test binary activates it so that no test can leave check mode through
        // the manner in which it assembles the engine (see test_seminaive.cpp).
        static void set_default_seminaive_check(bool on);
        // Records outside check mode (.negation-records): each fact that a
        // rule obtains through a negated condition is logged and re-evaluated
        // following every execution, just as in check mode, yet if the negated
        // premise begins to be satisfied, the fact is named in a warning,
        // and the execution proceeds successfully. Disabled by default, since
        // the records expand alongside the facts generated by such rules;
        // disabling it discards them, except when check mode is active and
        // keeps them.
        void record_negations(bool on);
        bool record_negations() const;
        // The facts identified in the most recent run as no longer
        // justified, each accompanied by the rule that derived it.
        // These are the facts that the run's warning, or check mode's error,
        // explicitly names.
        const std::vector<std::pair<Node, Node>>& defeated_negations() const;

        // When max_depth is set to 0, the limit becomes unlimited (the process
        // ends because each fact is examined just once, and cycles among facts
        // are resolved as components; hash-consing ensures that shared subterms
        // become shared subproofs, thus producing a DAG where identical facts
        // reuse a single ProofNode instance). With a defined max_depth, the
        // result is the full proof provided the search stays within the explain
        // budget, and the caller cuts it at max_depth; once the budget is
        // exceeded, the result is the proof discovered by a search confined to
        // max_depth (ExplainCounts::limited).
        std::shared_ptr<ProofNode> explain(Node fact, std::size_t max_depth) const;

        // The maximum depth used by .explain when no value is given, and that
        // of zelph/explain.
        static constexpr std::size_t default_explain_depth = 4;

        // How much work the search to find the complete proof may
        // expend before a depth-limited explain opts to restrict itself
        // to the search within its limit (reasoning_explain.cpp,
        // `charge`); 0 reverts to the default (`default_explain_budget`).
        // No search does as much work as the largest value, thus with it a
        // depth-limited explain always outputs the top of the complete proof
        // (`.explain-budget unlimited`).
        static constexpr std::size_t default_explain_budget = 80000;
        void                         set_explain_budget(std::size_t work) const;
        std::size_t                  explain_budget() const;

        // What the last explain() did: the frequency with which the search
        // entered a fact, and the number of unique facts it entered. The
        // search aiming to achieve the complete proof enters each fact
        // exactly once, making the two values identical unless the budget was
        // exhausted beneath a depth limit (`limited`). In such a case, the
        // search operating within the limit also proceeded, and within that
        // scope, a result that was truncated by the limit is re-examined when
        // a less deep position arrives at the same fact
        // (reasoning_explain.cpp, `prepare`). The counts cover both searches.
        struct ExplainCounts
        {
            std::size_t searches{0};
            std::size_t facts{0};
            std::size_t most{0};        // the most searches for one fact
            bool        limited{false}; // the proof is the one found inside the depth limit
            std::size_t work{0};        // the work conducted by both searches in the budget's units
        };
        ExplainCounts last_explain_counts() const;

        // The rules in force, read for explain() and defined
        // where it is.
        struct ExplainRules;

        // The facts a rule derives from `fact` in a single step: the
        // explanation search run forward, traversing identical rules and
        // operating under the same budget. `truncated` says that the budget
        // was exhausted before examining every rule, hence the resulting list
        // might be incomplete.
        std::vector<Node> dependents(Node fact, bool& truncated) const;

        // --- Implemented in reasoning_strata.cpp ---

        // What `negation_levels` identifies within a rule set: for each rule,
        // parallel to `rules`, and for each strongly connected component in
        // the graph where every rule directs to the rules that read what it
        // generates. A component marked with `negation_inside` negates a
        // predicate it produces and lacks a stratified reading; its negating
        // rules operate at a single level and alternate there with the
        // positive rules. One entry for each rule and each component, with no
        // growth tied to the facts.
        struct NegationLevels
        {
            std::vector<Node>        rules;
            std::vector<std::size_t> level;           // its level if the rule negates, 0 otherwise
            std::vector<bool>        negates;         // the rule has a negated condition
            std::vector<std::size_t> component;       // the rule's component
            std::vector<bool>        negation_inside; // indexed by component
            std::vector<bool>        transparent;     // a rule over predicates (RuleFootprint::transparent)
            std::size_t              levels{0};       // levels in use, numbered from 0 without gaps
        };

        // The negation levels of the rule set in its current form: the
        // rules a run gathers, examined according to how the run
        // interprets them (.strata).
        NegationLevels strata();

        // The rules in force, as enumerated by .list-rules, sorted by node.
        // For each rule, its conditions sorted by kind as the negation levels
        // read them (read_footprint) -- the members of a condition set
        // individually, a nested set read through -- followed by its
        // consequences. Each list is sorted by node, and nothing is created.
        struct ListedRule
        {
            Node              rule{0};
            std::vector<Node> conditions; // plain positive conditions
            std::vector<Node> negated;    // the patterns the negated conditions negate
            std::vector<Node> guards;     // `!=` conditions
            std::vector<Node> paths;      // path conditions, as the tag fact that carries the closure
            std::vector<Node> neural;     // `≈` conditions
            std::vector<Node> heads;      // the consequences, core `!` for a denial
        };
        std::vector<ListedRule> rules_in_force();

    private:
        // --- Implemented in reasoning.cpp (orchestration) ---

        // Loses track of what this engine keeps across commands regarding the
        // nodes of a graph that a load has replaced (see the definition).
        // Installed as Zelph's _on_graph_replaced.
        void forget_graph_state();

        // The hook for removal (Zelph's _on_removal; see the definition).
        void note_removal(const adjacency_set& removed, bool derived);
        // Erases the negation records of each rule among `removed` (see the
        // definition).
        void forget_negation_records(const adjacency_set& removed);
        // The hook for a name merge (Zelph::observe_merges): the negation
        // records follow the nodes that were relocated (see the definition).
        void follow_merge(const std::unordered_map<Node, Node>& moved);
        // Give the record up: the graph is no longer considered a
        // fixpoint. Presumes _mtx_delta_since_run is held.
        void drop_delta_record_locked(bool by_removal);

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
        // An answer to the query being evaluated, collected
        // (set_query_collector) or printed, once per binding. A narrower fact
        // alongside a wider one states the same binding a second time --
        // `a p c` and `a p b c` both answer `X p Y` with Y = c -- and the
        // second row repeated the first.
        void answer(Node condition, const std::shared_ptr<Variables>& bindings, Node rule);
        // The bindings that the current query has answered (see answer): the
        // maps handed to answer() directly, compared by content. Nothing
        // writes to such a map after it reaches a terminal: each is
        // constructed via join() or copied from a prior binding before its
        // single write, and thereafter accessed solely for reading.
        //
        // A binding is retained with its hash, which the set reads as it
        // expands rather than traversing the map again.
        struct AnsweredBinding
        {
            std::shared_ptr<Variables> binding;
            std::uint64_t              hash;
        };
        struct AnsweredHash
        {
            std::uint64_t operator()(const AnsweredBinding& a) const noexcept { return a.hash; }
        };
        struct AnsweredEqual
        {
            bool operator()(const AnsweredBinding& a, const AnsweredBinding& b) const { return a.hash == b.hash && *a.binding == *b.binding; }
        };
        using AnsweredSet = ankerl::unordered_dense::segmented_set<AnsweredBinding, AnsweredHash, AnsweredEqual>;

        // --- Implemented in reasoning_deduce.cpp ---

        void deduce(const Variables& variables, Node parent, const int depth, ReasoningContext& ctx, double confidence);
        bool consequences_already_exist(const Variables&                condition_bindings,
                                        const std::unordered_set<Node>& fresh_vars,
                                        const adjacency_set&            deductions,
                                        Node                            parent,
                                        const int                       depth);

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

        // The construction process, as `recipe` instantiates the rule's
        // collections (none: every container is kept). `kept` reports whether
        // the rule returned is the one over the parts this construction built
        // -- created at this moment, or found in force or merely mentioned
        // beneath them -- rather than a different rule in force that says the
        // same.
        Node build_rule(Node pattern, const Variables& variables, int depth, Node parent, bool& created, const Recipe* recipe, bool& kept);

    public:
        // The rule build_rule returns for `pattern` under `variables`, found
        // without any construction, or 0 if it is absent from the graph or
        // merely mentioned there. The read-only twin of build_rule,
        // analogous to how ground_instance relates to instantiate_fact: it
        // walks the same way and must remain synchronized with it. It finds
        // the rule based on the parts a construction keeps, not a rule a
        // construction claimed. .explain uses it to justify a rule that a
        // rule generator derived.
        Node existing_rule(Node pattern, const Variables& variables, int depth, Node parent) const;

        // The fact that a firing of the rule, where the condition part is
        // `parent`, derives from its consequence `deduction` under
        // `variables`, found without any instantiation, or 0 if the graph
        // does not hold it. The read-only twin of what deduce asserts, just
        // as existing_rule corresponds to build_rule. .explain uses it to
        // justify a fact over a collection that the firing produced, which
        // would otherwise be overlooked by identity matching with the
        // consequence.
        Node existing_consequence(Node deduction, const Variables& variables, int depth, Node parent) const;

    private:
        // Whether deduce asserts a fact for the consequence `deduction`: it
        // is not a contradiction, not a rule, and it possesses precisely
        // one predicate.
        bool asserts_fact(Node deduction) const;

        // The subject, the predicate, and the objects that deduce
        // instantiates for the consequence `deduction` under `variables`, as
        // predicted by ground_instance; a variable lacking a binding remains
        // unchanged. nullopt if any component cannot be predicted.
        struct ConsequenceParts
        {
            Node          source{0};
            Node          relation{0};
            adjacency_set targets;
        };
        std::optional<ConsequenceParts> predict_consequence(Node deduction, const Variables& variables, int depth, Node parent) const;

        // What a consequence would do to a refuted fact, determined from its
        // prediction before any part of it is built. Instantiation builds a
        // nested fact at probability 1, so a consequence that nested a fact
        // refuted by `¬` aborted the entire run with "this fact is known to
        // be wrong", even if it merely reiterated that the fact is refuted.
        // `Known`: a marking `(F) ~ refuted` of an F already refuted, which
        // changes nothing. `Contradiction`: a consequence that nests a
        // refuted fact (a statement claims what it nests), or a marking of an
        // F someone claimed, which hands that F back in `claimed` for the
        // record. Zero cost as long as the graph refutes nothing and the
        // consequence is not a marking. `nests_marking` says that the
        // consequence writes a marking inside a statement
        // (ConsequenceText::nests_marking): one of an F somebody claimed is
        // the same contradiction.
        //
        // A binding can provide the `~` and the `refuted` associated with a
        // marking: after processing "t p b" and "t go refuted", the rule
        // "(X go Y) => ((t p b) ~ Y)" removed `t p b` from every query without
        // comment. `opaque` says that the consequence holds an opaque
        // predicate (consequence_holds_opaque). A refuted fact that the
        // consequence holds where it claims nothing (see NestedPlace) does not
        // contradict anything; it is included in `looked_up`, and the
        // instantiation adopts it as-is rather than reasserting it.
        //
        // The `var_relation` predicate, as the rule writes it, is read by the
        // walk as it reads the consequence's subject and objects.
        enum class RefutedConsequence
        {
            None,
            Known,
            Contradiction
        };
        RefutedConsequence refuted_consequence(Node deduction, Node relation, Node var_relation, const adjacency_set& var_targets, const Variables& variables, int depth, Node parent, bool nests_marking, bool opaque, Node& claimed, LookedUpFacts& looked_up) const;

        // How the claim of a derived statement engages with a fact embedded
        // within it. `Claimed`: the statement claims it. `Marked`: it is the
        // fact that a refutation marking names; the marking says that it does
        // not hold and claims nothing of it, while the fact's constituent
        // elements are claimed as usual. `Mentioned`: it appears within the
        // argument of an opaque predicate or within a rule the consequence
        // mentions, where nothing is claimed.
        enum class NestedPlace
        {
            Claimed,
            Marked,
            Mentioned
        };

        // Whether instantiating `pattern` under `variables` builds a fact
        // that the graph holds as known to be wrong, at any depth of the
        // pattern's own structure, or, with `markings`, a marking `(F) ~
        // refuted` of an F somebody claimed, which it hands back in
        // `claimed`. A value provided by a variable is not entered: a firing
        // reads it and claims none of it.
        //
        // The `place` indicates how the statement holds `pattern`: a refuted
        // fact located outside the `Claimed` place does not conflict with
        // anything and is included in `looked_up`, while a marking within a
        // `Mentioned` place constitutes a mention, which refutes nothing.
        //
        // A container is walked through the members the instantiation meets
        // (container_members_met); `into` says that the statement writes into
        // `pattern`, which then holds nothing the firing writes.
        bool nests_refuted_fact(Node deduction, Node pattern, const Variables& variables, int depth, Node parent, std::vector<Node>& history, bool markings, bool opaque, NestedPlace place, Node& claimed, LookedUpFacts& looked_up, bool into = false) const;

        // Every variable of a rule, conditions and consequences alike. Unlike
        // collect_variables this descends through the conjunction SET node,
        // which carries no structure of its own -- so the variables of a
        // multi-condition rule are reachable at all.
        std::unordered_set<Node> rule_variables(Node rule, Node parent, int depth) const;

        // A condition inside a derived rule, with the specified bindings:
        // its instance as instantiate_fact builds it within a condition,
        // including a negated condition. A nested conjunction within the
        // condition is also instantiated using the recipe (container_plan);
        // without a recipe, it is kept.
        Node rebuild_condition(Node pattern, const Variables& variables, int depth, const Recipe* recipe);

        // --- Implemented in reasoning_neural.cpp ---
        const NeuralNet* compiled_net(Node net_node, int depth);
        void             report_unusable_net(Node net_node, const std::string& why);
        void             evaluate_neural(Node condition, const RulePos& rule, ReasoningContext& ctx, int depth, bool negated);
        void             evaluate_closure(Node condition, const RulePos& rule, ReasoningContext& ctx, int depth, bool negated);
        void             proceed_after_condition(const RulePos& rule, ReasoningContext& ctx, int depth, std::shared_ptr<Variables> vars, std::shared_ptr<Variables> uneqs, double confidence);

        // --- Implemented in reasoning_strata.cpp ---

        // What a single rule generates and what it examines, in terms of
        // predicates: every result at every depth level, each dependency, and
        // separately the conditions it tests under a negation, with the
        // outcomes of a rule that generates another rule being what the derived
        // rule will create. `*_keys` pair each of these predicates with the
        // object the levels assess it against, 0 indicating any object
        // (collect_keys in reasoning_strata.cpp); check mode notes the
        // predicates alone. `*_any` represents a variable predicate, or a
        // condition that is not retrieved through facts. `checkable` is false
        // when check mode cannot re-test the rule's negations, because a
        // condition can only be verified during the rule's runtime, not later
        // through reconstruction: a neural condition, a negated path condition,
        // or a path condition whose endpoints are bound by nothing else.
        // `transparent` marks a rule that solely expands the predicates it
        // reads, such as a transitivity rule applied to a predicate variable;
        // its variable consequence then is not considered `produces_any` (see
        // footprint in reasoning_strata.cpp).
        using PatternKey = std::pair<Node, Node>;
        struct RuleFootprint
        {
            std::unordered_set<Node> produces;
            std::unordered_set<Node> reads;
            std::unordered_set<Node> negates;
            std::set<PatternKey>     produces_keys;
            std::set<PatternKey>     reads_keys;
            std::set<PatternKey>     negates_keys;
            std::unordered_set<Node> produces_vars; // variables appearing within a predicate position of a consequence
            std::unordered_set<Node> reads_vars;    // the same under a positive condition, at any depth
            std::unordered_set<Node> binds_vars;    // the predicate of a plain positive condition
            std::unordered_set<Node> joined_vars;   // each variable within a plain positive condition or a consequence
            std::vector<Node>        paths;         // the path patterns associated with positive path conditions
            std::vector<Node>        plain_conds;   // the conditions read through facts, by node
            std::vector<Node>        guard_conds;   // `!=` guards
            std::vector<Node>        path_conds;    // parallel to paths: the tag facts themselves
            std::vector<Node>        neural_conds;  // `≈` conditions
            bool                     produces_any{false};
            bool                     reads_any{false};
            bool                     reads_unnamed{false}; // a neural condition: no predicate says what it reads
            bool                     negates_any{false};
            bool                     has_negation{false};
            bool                     checkable{true};
            bool                     transparent{false};
            std::vector<Node>        negated_conditions; // the patterns the negated conditions negate
        };
        RuleFootprint footprint(Node rule);
        void          read_footprint(Node condition, RuleFootprint& f);

        // The level at which each `rules` entry featuring a negated condition
        // may be evaluated, 0 for the others, the overall count of levels
        // currently active, and the components they were derived from. A
        // level runs solely when every lower level fails to generate further
        // results; consult the file for further information on what this
        // ensures and where it does not.
        NegationLevels negation_levels(const std::vector<Node>& rules);

        // --- Implemented in reasoning_seminaive.cpp ---

        static bool default_seminaive_check();

        // Delta-driven fixpoint loop (semi-naive evaluation). Returns the
        // number of safety-net violations found (always 0 unless
        // _seminaive_check is active and delta seeding missed a derivation).
        // seed: when non-null, the facts to start from instead of the classic
        // first pass (see the incremental parameter of run()).
        // removed_preds: during a seeded run, _removed_preds_since_run as the
        // run found it. new_rules: during a seeded run, the rules currently
        // in force that have been newly introduced since the last run.
        uint64_t run_fixpoint_seminaive(bool silent, const std::vector<std::pair<Node, Node>>* seed = nullptr, const std::unordered_set<Node>* removed_preds = nullptr, const std::unordered_set<Node>* new_rules = nullptr);

        // --- Members ---

        std::atomic<bool>                     _done{false};
        std::unique_ptr<io::DerivationExport> _export;
        std::atomic<uint64_t>                 _running{0};
        bool                                  _print_deductions{true};
        bool                                  _export_derivations{false};
        std::atomic<bool>                     _contradiction{false};
        // Deductions this run derived but did not show. Reset per run and
        // reported once at the end of run(), so what the reader gets is the
        // run's total rather than the remainder since the last printed line.
        // Read after the run by deductions_withheld().
        std::atomic<size_t> _skipped{0};
        bool                _deduction_notice{true};
        bool                _contradictions_printed{false};
        std::mutex          _mtx_output;
        std::mutex          _mtx_network;
        // The bucket terms that a firing of this run has given their bucket's
        // ground members (bucket_term in reasoning.cpp), ensuring subsequent
        // firings of the run into a bucket skip re-reading that bucket. One
        // entry per bucket, thus rule-scale; cleared at the start of a run,
        // written under _mtx_network.
        std::unordered_set<Node> _filled_terms;
        // Per consequence pattern, whether it holds a rule beneath its top
        // level (holds_nested_rule in reasoning_deduce.cpp), and whether its
        // instance would keep a variable from such a rule
        // (keeps_rule_variable, with the variables of a rule within a value
        // stored in `value_rule_variables`), ensuring that deduce() walks a
        // consequence only once per run, excluding a firing that binds a
        // variable to `in` (instance_keeps_variable). One entry per
        // consequence, thus rule-scale; cleared at the start of a run, read
        // and written under _mtx_network (consequence_text).
        //
        // `claims_nested` determines if the instance can hold a node whose
        // mark its claim revokes or which its claim enters
        // (Zelph::claim_nested): a fact or a set constant within the
        // consequence's parts, a collection of the rule's text that the
        // firing builds a value for, or a comma list that the claim enters
        // (enters_kept_list, populating `kept_lists`). A part represented by
        // a variable is a value provided by the binding, and a derivation
        // claims none of these. `written` lists the facts and set constants
        // the consequence writes exactly as they stand outside a `=>`,
        // including the members of such a comma list, read only when a value
        // supplied by the binding bears a mark.
        //
        // `nested_predicates` lists the predicates from facts nested within
        // the consequence, outside a `=>`, including variables; they are read
        // solely after a predicate has been declared opaque
        // (consequence_holds_opaque).
        //
        // `flat_relation` serves as the predicate in a flat consequence,
        // interpreted via `flat_parent` as the rule: an atom distinct from
        // `in`, with objects that are atoms other than this predicate, or
        // variables, the latter listed in `flat_variables`. 0 for any other
        // consequence (predicate_never_an_object).
        struct ConsequenceText
        {
            bool                                    mentions_rule{false};
            bool                                    keeps_rule_variable{false};
            std::unordered_set<Node>                value_rule_variables;
            bool                                    claims_nested{false};
            bool                                    nests_marking{false};
            std::optional<std::unordered_set<Node>> written;
            std::optional<std::vector<Node>>        nested_predicates;
            mutable std::unordered_map<Node, bool>  kept_lists;
            Node                                    flat_relation{0};
            Node                                    flat_parent{0};
            std::vector<Node>                       flat_variables;
        };
        std::unordered_map<Node, ConsequenceText> _consequence_text;
        // Whether a `deduction` firing that binds the same thing as `binding`
        // does, and makes a witness for each of `fresh`, keeps a variable of a
        // rule the consequence mentions: one located within a condition set
        // that the firing keeps as written, or one among the
        // `value_rule_variables` it leaves unbound. The text answers this
        // without performing an additional walk, except when a binding binds a
        // variable to `in`: a relation bound to `in` writes into the objects
        // that the text also read as values.
        bool instance_keeps_variable(Node deduction, const ConsequenceText& text, const Variables& binding, const std::unordered_set<Node>& fresh, Node parent, int depth) const;
        // Whether the prediction of a flat consequence, under `binding`, is
        // certain not to list its predicate `relation` among its objects,
        // determined solely by the binding. False whenever only
        // `predict_consequence` can tell, which then makes the decision.
        bool predicate_never_an_object(const ConsequenceText& text, Node relation, const Variables& binding, Node parent) const;
        // The `deduction` entry, obtained by reading the components that the
        // deduce() function has disassembled, or disassembled here. The
        // reference stays valid until the next run clears the cache.
        const ConsequenceText& consequence_text(Node deduction, Node source, Node relation, const adjacency_set& targets, Node parent, int depth);
        const ConsequenceText& consequence_text(Node deduction, Node parent, int depth);
        // Whether the consequence writes `node` in its original form, outside
        // a `=>` (ConsequenceText::written).
        bool consequence_writes(Node deduction, Node source, Node relation, const adjacency_set& targets, Node node);
        // Whether the claim of a firing's instance enters `node`, a
        // conjunction set the firing keeps as written: a comma list of the
        // consequence's text, with members the instance holds as the rule
        // wrote them, and which the claim asserts as a typed statement
        // asserts its own. The claim and the prediction both ask it
        // (nests_refuted_fact), so what the prediction reads as claimed
        // matches what the firing claims. A rule's condition set that the
        // text names by node is not entered, because a typed statement does
        // not enter a comma list it holds by node: if it were entered, it
        // would turn that rule's conditions into data. The answer is kept
        // per node in `text`, since the adjacency of a comma list grows with
        // each fact that holds it.
        bool enters_kept_list(const ConsequenceText& text, Node node) const;
        // Whether this firing writes a fact with an opaque predicate: `rel`,
        // the predicate it binds at the top, or any of the
        // `nested_predicates` under `binding`. One atomic load when no
        // predicate has been declared.
        bool consequence_holds_opaque(Node deduction, Node source, Node relation, const adjacency_set& targets, Node rel, const Variables& binding);
        // The bindings of the unification search for positive fact conditions,
        // including the seed for each semi-naive iteration (rules.md, "What the
        // run summary counts"). As wide as a size, because a run over a bulk
        // import counts more than an int holds.
        std::atomic<std::size_t> _total_matches{0};
        std::atomic<int>         _total_contradictions{0};
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
        std::unordered_map<Node, bool>           _prune_by_members; // per prune: holds_member_matched_set for each condition read
        std::vector<std::shared_ptr<Variables>>* _query_results{nullptr};
        AnsweredSet                              _answered; // the bindings the current query has answered (see answer)
        ReasoningProfiler                        _prof;
        std::unordered_set<Node>                 _input_captured; // raw fact nodes materialized while parsing input
        std::unordered_set<Node>                 _input_focus;    // reduced focus set: top-level inputs + their components
        bool                                     _deduction_filter{false};
        bool                                     _capturing{false};
        int                                      _capture_suppress_depth{0}; // > 0: input capture suppressed (nested imports)

        // --- Cross-run delta (implemented in reasoning.cpp) ---
        //
        // Facts created after the last run(), recorded by the same observer
        // responsible for supplying the input focus. A normal run rebuilds
        // its knowledge of the graph from the beginning (its first iteration
        // constitutes a classic pass), so it consumes and discards this
        // data; .run-delta seeds from it instead, transforming
        // "run again after adding a little" from a cost in the size of the
        // graph into a cost in the size of the addition --
        // applicable solely to rules that support seeding. A rule that is
        // delta-unsafe still takes a classic pass in every iteration, a rule
        // featuring a path condition in each iteration that extends the
        // predicate it walks, a rule that is new since the last run just
        // once, and a rule with a negated condition at its negation level
        // when it cannot be seeded there (see run_fixpoint_seminaive).
        void                               arm_delta_recorder();
        std::vector<std::pair<Node, Node>> _delta_since_run;
        mutable std::mutex                 _mtx_delta_since_run;
        // Where each recorded fact stands in _delta_since_run, for the removal
        // hook: upon removal, the fact's entries are set to {0, 0} and ignored
        // by the next run. Constructed on demand, up to _delta_indexed, the
        // first time a removal meets the record, ensuring that sessions
        // removing nothing incur no cost; limited by the record's cap. A fact
        // that is recorded twice (initially created as a pattern, later
        // claimed) resides in _delta_repeated and has all its entries found by
        // a scan.
        ankerl::unordered_dense::map<Node, size_t> _delta_index;
        ankerl::unordered_dense::set<Node>         _delta_repeated;
        size_t                                     _delta_indexed{0};
        size_t                                     _delta_erased{0};
        // The rules that note_removal incorporated into the record because a
        // removal brought them into force. Each had been present during the
        // last run, meaning that its subsequent removal constitutes the removal
        // of an older rule rather than of a recorded fact, even though the
        // record lists it.
        ankerl::unordered_dense::set<Node> _delta_revived;
        // See removed_predicates_since_run.
        std::unordered_set<Node> _removed_preds_since_run;
        // The record was relinquished because a removal took facts that a run
        // might have derived; the fallback note in the next .run-delta
        // indicates this.
        bool _delta_lost_to_removal{false};
        // False whenever the record is not a faithful account of everything
        // added since the last run: before the first run, while an import is
        // in progress (those facts are bulk knowledge, not an increment), and
        // once the record has outgrown its cap -- and also whenever the graph
        // is no longer known to be a fixpoint, following a removal that might
        // have taken a derived fact (note_removal, note_user_removal). Seeding
        // then falls back to a classic pass, so an invalid record costs time,
        // never correctness.
        // A run clears it when it takes the record over and sets it at its
        // end, so a run that an error halts leaves it false.
        bool _delta_valid{false};
        // A recorded entry is 16 bytes, so this caps the record at ~16 MB.
        // Anything that adds a million facts between two runs is a bulk load,
        // for which a classic pass is the right answer anyway.
        static constexpr size_t _max_delta_entries{1'000'000};
        // Rule count observed at the end of the last run. A count that the
        // `=>` facts within the record of additions fail to explain
        // indicates a change to the rule set the record is unable to
        // represent, causing an incremental request to revert to a classic
        // pass.
        size_t _rules_at_last_run{0};
        // See completed_runs and unchanged_since_last_run; both are guarded
        // by _mtx_delta_since_run. The flag is cleared at the beginning of a
        // run, so a run that throws before completion leaves it as false,
        // and a drop that gives reported negation records back clears it as
        // well (close_cluster_negations).
        size_t _completed_runs{0};
        bool   _fixpoint_at_last_run{false};

        // See note_fact_rule. Restricted to the main thread, just like
        // zelph/fact and zelph_fact.
        struct NotedFactRule
        {
            Node        rule;
            const char* writer;
            bool        claimed;
        };
        std::vector<NotedFactRule> _fact_rules;

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
        Node                                       _refuted_pred{0};      // "refuted"
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
        bool _record_negations{false};

        // --- Check mode: a negation that has come to hold ---
        //
        // A negation is evaluated just once, and its result is never revisited:
        // facts only increase over time, implying that a deduction made under
        // ¬P stays in the graph even if P becomes true later. That is sound
        // exactly when no subsequent development can make P true -- precisely
        // the purpose of negation levels within a single run, and precisely
        // what nothing guarantees across different runs (a statement made after
        // the question) or inside a program that negates its own derived
        // conclusions. Classic and semi-naive evaluation produce identical
        // deductions in this case, so comparing them yields no variation; the
        // sole difference emerges when questioning whether the negated premise
        // continues to fail.
        //
        // In check mode, and when operating outside of it with
        // `.negation-records` enabled, each fact generated by a negating
        // rule is logged alongside its associated rule and bindings. Once
        // every such execution finishes, any records whose rule negates a
        // predicate that has gained fresh facts since the last check are
        // re-evaluated -- all entries undergo re-testing after a run that
        // did not note those predicates (refer to Reasoning::run) -- unless
        // the rule is labelled as not `checkable` (RuleFootprint). A fact is
        // reported if its negated condition currently holds AND no
        // alternative justification is present (Reasoning::explain): the run
        // fails in check mode, otherwise it issues a warning. Once reported,
        // a record is removed, ensuring each loss is communicated only once.
        // The cost involves a single record per such fact, and this happens
        // solely when records are kept.
        struct NegationRecord
        {
            Node      fact;
            Node      rule;   // the => fact
            Node      parent; // what the rule was evaluated under: its condition set, if it has one
            Variables bindings;
        };
        std::vector<NegationRecord>    _negation_records;
        std::mutex                     _mtx_negation_records;
        std::unordered_map<Node, Node> _negating_rules;           // the deferred rules of the current schedule, by rule and by condition part
        std::unordered_set<Node>       _check_touched;            // predicates that gained facts since the last check
        bool                           _check_touched_all{false}; // ... or everything: following a bulk load, or a run that did not note them
        std::vector<NegationRecord>    recheck_negations();
        bool                           keeps_negation_records() const { return _seminaive_check || _record_negations; }

        std::vector<std::pair<Node, Node>> _defeated_negations; // (fact, rule): what was reported by the most recent run

        // The records a run reported, and thus discarded, while a named
        // cluster was active, by that cluster, under _mtx_negation_records.
        // A drop of the cluster restores them, as the fact that defeated them
        // might have been lost alongside it, and a later run then reports the
        // defeat again where it holds; a merge hands them to the target, and
        // a merge into the default cluster keeps the report. Memory: a single
        // record for each defeat reported within a cluster, until that
        // cluster is dropped or merged.
        std::map<std::string, std::vector<NegationRecord>> _reported_in_cluster;
        void                                               close_cluster_negations(const std::string& name, const std::string* into);
        // Whether _reported_in_cluster contains any data: before acquiring the
        // mutex, every merge and drop of a cluster, the engine's scratch
        // clusters included, examines it.
        std::atomic<bool> _any_reported_in_cluster{false};

        // Set for the duration of a quiet run (see run). What it met is kept
        // until the next run begins; its memory expands with the
        // contradictions that the run meets, and a full pass meets each
        // recorded one again. The keys that eliminate duplicates are
        // discarded once the run ends.
        bool                                 _quiet{false};
        std::vector<MetContradiction>        _met_contradictions;
        std::set<std::pair<Node, Variables>> _met_keys;

        // Set for the duration of a run started with `derived`; what it
        // collects remains stored, under _mtx_output, until either
        // take_derived_facts is invoked or a new run begins.
        bool                     _capture_derived{false};
        std::vector<DerivedFact> _derived_facts;
        DerivedFact              describe_derivation(Node fact, Node condition, const Variables& bindings, Node rule);

        // Set during the execution of check mode's classic verification
        // pass; the initial line output by that pass is preceded by the
        // marker. Controlled by _mtx_output, just as the printing itself
        // is. The marker heads the line that follows it, making it a
        // finding if that line qualifies as one.
        bool _verification_marker_pending{false};
        void print_pending_verification_marker(bool finding = false);

        // Written by explain(), which is const: a read of the graph that
        // reports its actions.
        mutable ExplainCounts _explain_counts;

        // The rules in force as explain() last read them are preserved for
        // the subsequent call, provided the rules remain unchanged
        // (reasoning_explain.cpp).
        mutable std::shared_ptr<const ExplainRules> _explain_rules;
        mutable std::size_t                         _explain_budget{0}; // 0: the default
        std::shared_ptr<const ExplainRules>         explain_rules_in_force() const;
    };
}