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

#include "reasoning.hpp"

#include "chrono/stopwatch.hpp"
#include "contradiction_error.hpp"
#include "fact_structure.hpp"
#include "rule_identity.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <functional>
#include <set>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

using namespace zelph::network;

Reasoning::Reasoning(const io::OutputHandler& output)
    : Zelph{output}
    , _pool{std::make_unique<concurrency::ThreadPool>(std::thread::hardware_concurrency())}
    , _prof{this}
    , _seminaive_check{default_seminaive_check()}
{
    _on_graph_replaced = [this]
    { forget_graph_state(); };
    _on_removal = [this](const adjacency_set& removed, const bool derived)
    { note_removal(removed, derived); };
    _on_cluster_closed = [this](const std::string& name, const std::string* into)
    { close_cluster_negations(name, into); };
    observe_merges([this](const std::unordered_map<Node, Node>& moved)
                   { follow_merge(moved); });
}

// All data the engine retains between commands concerning graph node
// identifiers. Once a load has replaced the graph, those identifiers either
// refer to the file's nodes or denote nothing. The input focus would serve as
// the basis for deductions regarding whichever node took an ID the session had
// typed; the delta record would seed a run using facts no longer present, so
// it is marked as invalid, and the next .run-delta is a full pass; the
// negation records, including those from check mode, would report the defeat
// of a conclusion the loaded graph had never drawn; and the list
// zelph/defeated returns, the compiled nets, the nets and conditions already
// reported, and the rules explain() read would all point to the old graph. The
// settings remain unchanged, with .negation-records included. _nn_pred and its
// siblings are re-consulted at the start of each run and need nothing here.
void Reasoning::forget_graph_state()
{
    _input_captured.clear();
    _input_focus.clear();
    {
        std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
        drop_delta_record_locked(false);
    }
    {
        std::lock_guard<std::mutex> lock(_mtx_negation_records);
        std::vector<NegationRecord>().swap(_negation_records);
        _reported_in_cluster.clear();
        _any_reported_in_cluster.store(false, std::memory_order_release);
    }
    _negating_rules.clear();
    _check_touched.clear();
    _check_touched_all = false;
    _defeated_negations.clear();
    _nn_cache.clear();
    _nn_reported.clear();
    _refused_conditions.clear();
    _explain_rules.reset();
    // The noted `=>` facts would refer to the nodes from the loaded file,
    // which the caller did not produce (note_fact_rule).
    _fact_rules.clear();
}

void Reasoning::note_fact_rule(const Node rule, const char* const writer, const bool claimed)
{
    _fact_rules.push_back({rule, writer, claimed});
}

// The `=>` fact departs in the same manner as query text does
// (drop_query_text): removed node by node, it would disarm the fact stores for
// the rest of the session (drop_cluster). Its components persist, just as
// .remove-rules leaves them too: they consist of the claims or rule text
// originating from the calls that wrote them. A rule removed here might have
// been the sole fact holding another noted rule, which is then in force; thus,
// the pass continues iteratively until no further elements are removed.
//
// A claimed fact existed before the call: zelph/mention or zelph_mention wrote
// it, and the claim revoked the mention's mark. It is not a fact that the call
// created to take back, and the action the call performed on it is the claim
// alone, thus the claim is undone and the fact stays a mention, exactly as it
// had been before the call. Claimed unchecked, it was a rule in force whose
// consequence predicate is not bound by any condition.
std::string Reasoning::settle_fact_rules()
{
    if (_fact_rules.empty()) return {};

    std::vector<NotedFactRule> pending;
    pending.swap(_fact_rules);

    std::string refused;
    for (bool removed = true; removed;)
    {
        removed = false;
        for (auto it = pending.begin(); it != pending.end();)
        {
            const Node rule = it->rule;
            if (!_pImpl->exists(rule))
            {
                it = pending.erase(it);
                continue;
            }
            if (!is_rule_in_force(rule))
            {
                ++it;
                continue;
            }

            adjacency_set     consequences;
            const Node        condition = parse_fact(rule, consequences);
            const std::string why       = unbound_predicate(this, {condition}, consequences);
            if (!why.empty())
            {
                refused += (refused.empty() ? "" : "\n") + std::string(it->writer) + ": " + why;
                if (it->claimed)
                    restore_mention(rule);
                else
                {
                    const std::string scratch = "__fact-rule";
                    move_into_cluster(scratch, {rule});
                    drop_scratch_cluster(scratch);
                    removed = true;
                }
            }
            it = pending.erase(it);
        }
    }
    return refused;
}

// A run within this cluster reported the defeats of these records, and
// reporting a record removes it. A drop takes back what the cluster wrote,
// potentially the fact that caused the defeat, and the report from a dry run
// accompanies the dry run. With the records gone, the run in the caller's
// cluster that meets the same defeat had nothing left to report. A merge
// assigns the cluster's work to the target.
void Reasoning::close_cluster_negations(const std::string& name, const std::string* into)
{
    if (!_any_reported_in_cluster.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(_mtx_negation_records);
        if (_reported_in_cluster.empty()) return;
        const auto it = _reported_in_cluster.find(name);
        if (it == _reported_in_cluster.end()) return;

        std::vector<NegationRecord> reported = std::move(it->second);
        _reported_in_cluster.erase(it);

        if (into != nullptr)
        {
            if (!into->empty())
            {
                auto& target = _reported_in_cluster[*into];
                target.insert(target.end(), std::make_move_iterator(reported.begin()), std::make_move_iterator(reported.end()));
            }
            _any_reported_in_cluster.store(!_reported_in_cluster.empty(), std::memory_order_release);
            return;
        }
        _any_reported_in_cluster.store(!_reported_in_cluster.empty(), std::memory_order_release);

        if (!keeps_negation_records()) return;
        _negation_records.insert(_negation_records.end(), std::make_move_iterator(reported.begin()), std::make_move_iterator(reported.end()));

        // The predicates that gained facts since the previous re-test convey no
        // insight into those: what defeated them might have existed all along.
        _check_touched_all = true;
    }

    // The records given back remain pending a re-test that no run has
    // performed since, hence the graph no longer reflects the state the last
    // run left, and the host's run following a keyword handler must not be
    // skipped (unchanged_since_last_run).
    std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
    _fixpoint_at_last_run = false;
}

void Reasoning::set_export_file(const std::string& path)
{
    _export_file = path;
}

void Reasoning::set_query_collector(std::vector<std::shared_ptr<Variables>>* collector)
{
    _query_results = collector;
}

namespace
{
    // The fact that Unification matches `condition` with under `variables`,
    // or 0. The bindings encompass all variables within the condition,
    // making each match an instance the run met; the first in the order of
    // ids is selected, ensuring the same contradiction is recorded with
    // identical members across every run.
    Node matched_fact(Zelph* const z, const Node condition, const Variables& variables, const Node parent)
    {
        Unification unification(z, condition, parent, std::make_shared<Variables>(variables), std::make_shared<Variables>(), /*pool*/ nullptr, /*log_depth*/ 1, /*profiler*/ nullptr);
        unification.enumerate_by_id();
        while (unification.Next())
            if (const Node fact = unification.matched_fact(); fact != 0) return fact;
        return 0;
    }
}

Reasoning::ContradictionRecord Reasoning::record_contradiction(const contradiction_error& error, std::vector<Node>* conditions)
{
    if (!_record_contradictions && conditions == nullptr) return ContradictionRecord::None;

    const Node       condition = error.get_fact();
    const Variables& variables = error.get_variables();

    std::vector<Node> members = condition_elements(condition);

    // A rule featuring a solitary condition situated outside a set contains
    // no members, and the condition joins as itself: the resulting record
    // is the one a condition set holding that condition alone produces. The
    // two formulations represent a single rule (rule_identity.cpp keeps
    // whichever was entered first), thus they must record alike.
    if (members.empty()) members.push_back(condition);

    // A refutation achieved via derivation that met a claimed fact: the
    // record comprises the conditions along with that fact, distinguishing
    // it from a denial based solely on the conditions, hence a marking
    // rule featuring a single condition results in a record identical to
    // what the equivalent denial `(X p Y, X q Y) => !` would leave.
    const Node claimed = error.get_claimed();

    std::unordered_set<Node> matched;
    bool                     nameable = true;
    for (const Node element : members)
    {
        // A condition that matched no FACT contributes nothing: a guard
        // filtered, a negation succeeded on absence. When looked up, it would
        // find nothing and leave the record unnamed -- see the note on this
        // function.
        if (!matches_a_fact(element)) continue;

        // Looked up, never built: a record asserts nothing. The condition
        // matched a fact, and the record names that fact as it stands: one
        // that carries more objects than the condition names (`X p Y`
        // matched `a p b c`), the set constant that a set in the condition
        // matched via its members, the marking of a refuted fact. Built
        // through fact(), the condition asserted `a p b`, which nobody had
        // said, and the fact a marking names was refused as known to be
        // wrong, which ended the run and the input following it.
        //
        // The look-up finds the fact that the condition denotes, and this
        // does not necessarily correspond to the fact it was matched
        // against: a rule's pattern or a refuted fact, which no condition
        // matches, may stand beside a wider fact that it did match, and a
        // held rule matches a stored rule through the members of its
        // conditions. Unification is subsequently requested again for the
        // fact that was matched (matched_fact). A condition for which
        // neither finds a fact leaves the record unnamed.
        std::vector<Node> history;
        Node              instance = 0;
        if (resolve_pattern(this, element, variables, instance, history, /*containing*/ true) != Resolve::Ok || is_rule_pattern(instance) || is_refuted_fact(instance))
            instance = matched_fact(this, element, variables, error.get_parent());
        if (instance == 0)
        {
            if (conditions == nullptr) return ContradictionRecord::None; // cannot name this one; report it as new
            nameable = false;
            continue;
        }
        matched.insert(instance);
    }

    if (conditions != nullptr)
    {
        conditions->assign(matched.begin(), matched.end());
        std::sort(conditions->begin(), conditions->end());
    }

    if (!_record_contradictions || !nameable) return ContradictionRecord::None;
    if (claimed != 0) matched.insert(claimed);

    // A rule whose conditions are ALL guards or negations leaves nothing to
    // point at. Rare, and the honest answer is to keep reporting it rather
    // than to invent a record.
    if (matched.empty()) return ContradictionRecord::None;

    const Node record = set(matched);
    if (record == 0) return ContradictionRecord::None;

    if (is_refuted_fact(record)) return ContradictionRecord::Existing;

    mark_refuted_fact(record);
    return ContradictionRecord::New;
}

void Reasoning::report_contradiction(const contradiction_error& error)
{
    // Before the output lock, never under it: this takes the network locks,
    // and deduce establishes network-then-output as the order.
    std::vector<Node>         conditions;
    const ContradictionRecord record        = record_contradiction(error, _quiet ? &conditions : nullptr);
    const bool                already_known = record == ContradictionRecord::Existing;

    std::lock_guard<std::mutex> lock(_mtx_output);

    // A quiet run keeps what it met, regardless of whether a record
    // already stored it, and lists each instantiation just once, even if
    // both the seeded and the classic passes reach it multiple times.
    if (_quiet && _met_keys.emplace(error.get_rule(), error.get_variables()).second)
        _met_contradictions.push_back({error.get_rule(), std::move(conditions), error.get_conclusion(), record, error.get_variables(), string::unmark_identifiers(error.get_reason())});

    // A contradiction the graph already holds is not a new finding, and
    // saying so again on every later input line is what this replaces. The
    // export is the exception: it is a record of the run, so a second run
    // must not hand back an empty file.
    if (already_known)
    {
        if (_export_derivations)
            _export->add("contradiction", contradiction_symbol(), render_premises(error.get_fact(), error.get_variables(), error.get_parent()), string::unmark_identifiers(error.get_reason()));
        return;
    }

    _contradiction = true;
    ++_total_contradictions;

    const bool print = !_quiet && (_print_deductions || _contradictions_printed);
    if (!print && !_export_derivations) return;

    std::string output;
    {
        const string::PremiseRendering premises;
        string::node_to_string(this, output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());
    }

    if (print)
    {
        print_pending_verification_marker(true);
        out_finding(string::unmark_identifiers(contradiction_symbol() + " ⇐ " + output), true);

        // A refusal is not a contradiction in the data, and the `!` line alone
        // named neither the shape nor what to write instead.
        if (!error.get_reason().empty())
            out_finding("   └─ refused: " + string::unmark_identifiers(error.get_reason()), true);
    }

    if (_export_derivations)
        _export->add("contradiction", contradiction_symbol(), render_premises(error.get_fact(), error.get_variables(), error.get_parent()), string::unmark_identifiers(error.get_reason()));
}

std::size_t Reasoning::count_contradiction_records() const
{
    std::size_t count = 0;

    for (const Node nd : refuted_facts_snapshot())
        if (is_set_constant(nd)) ++count;

    return count;
}

// What the graph already held when this run began. Empty when there is nothing
// to say, so the ordinary summary is unchanged; a network loaded from a file
// that was saved after a run is the case this exists for, because there every
// contradiction is already recorded, none of them is announced again, and the
// bare count reads as "clean".
std::string Reasoning::known_contradiction_note() const
{
    if (_records_at_run_start == 0) return {};
    return " (" + std::to_string(_records_at_run_start) + " already recorded in this network)";
}

std::string Reasoning::contradiction_symbol() const
{
    return string::mark_identifier(get_formatted_name(core.Contradiction, _lang));
}

// The members of the condition set, found the way render_premises finds
// them -- and the way node_to_string finds them when it prints "{...}".
std::vector<Node> Reasoning::condition_elements(const Node condition) const
{
    std::vector<Node> members;
    for (const Node rel : get_right(condition))
    {
        if (parse_relation(rel) != core.PartOf) continue;
        adjacency_set objs;
        const Node    element = parse_fact(rel, objs, 0);
        if (element == 0 || objs.count(condition) != 1) continue;
        members.push_back(element);
    }
    return members;
}

bool Reasoning::matches_a_fact(const Node element)
{
    if (is_negated_condition(element, 0)) return false;
    const Node element_predicate = parse_relation(element);
    return element_predicate != core.Unequal
        && (_nn_pred == 0 || element_predicate != _nn_pred)
        && (_closure_pred == 0 || element_predicate != _closure_pred);
}

// Describes a fact that a firing derived, during a quiet run initiated via
// `derived`. Each premise is retrieved without being instantiated:
// instantiation generates the fact upon a lookup failure, such as when
// unification matched a fact through its members, and a description must
// never perform a write operation; at this stage, the export also reads the
// graph. Such a premise is omitted. A premise that carries more objects than
// the condition names is found (`containing`).
Reasoning::DerivedFact Reasoning::describe_derivation(const Node fact, const Node condition, const Variables& bindings, const Node rule)
{
    DerivedFact derived{fact, rule, {}, {}, bindings};

    std::vector<Node> elements = condition_elements(condition);
    if (elements.empty()) elements.push_back(condition);
    for (const Node element : elements)
    {
        if (const Node operand = negation_operand(element); operand != 0)
        {
            derived.absent.push_back(operand);
            continue;
        }
        if (!matches_a_fact(element)) continue;

        std::vector<Node> history;
        Node              premise = 0;
        if (resolve_pattern(this, element, bindings, premise, history, /*containing*/ true) == Resolve::Ok && premise != 0)
            derived.premises.push_back(premise);
    }
    std::sort(derived.premises.begin(), derived.premises.end());
    derived.premises.erase(std::unique(derived.premises.begin(), derived.premises.end()), derived.premises.end());
    return derived;
}

std::vector<Reasoning::DerivedFact> Reasoning::take_derived_facts()
{
    std::lock_guard<std::mutex> lock(_mtx_output);
    std::vector<DerivedFact>    taken;
    taken.swap(_derived_facts);
    return taken;
}

std::vector<std::string> Reasoning::render_premises(const Node condition, const Variables& variables, const Node parent) const
{
    std::vector<std::string>       out;
    const string::PremiseRendering premises;

    // Elements of the conjunction set, found the same way node_to_string
    // finds them when it prints "{...}".
    for (const Node rel : get_right(condition))
    {
        if (parse_relation(rel) != core.PartOf) continue;
        adjacency_set objs;
        const Node    element = parse_fact(rel, objs, 0);
        if (element == 0 || objs.count(condition) != 1) continue;

        std::string rendered;
        string::node_to_string(this, rendered, _lang, element, 3, variables, condition);
        out.push_back(std::move(rendered));
    }

    if (out.empty())
    {
        std::string rendered;
        string::node_to_string(this, rendered, _lang, condition, 3, variables, parent);
        out.push_back(std::move(rendered));
    }

    return out;
}

void Reasoning::run(const bool print_deductions, const bool export_derivations, const bool suppress_repetition, const bool silent_requested, const bool incremental, const bool quiet, const bool derived)
{
    // Before anything can fire it, a rule authored by the assertion API
    // undergoes verification (settle_fact_rules). A refusal halts the run
    // before it has changed anything, and the rule is gone.
    if (const std::string refused = settle_fact_rules(); !refused.empty()) throw std::runtime_error(refused);

    // Input capture must not extend into evaluation: in classic mode the
    // observer would otherwise collect every deduced fact into the focus
    // set, neutralizing the filter (semi-naive mode replaces the observer
    // anyway, but the boundary belongs here, not to the evaluation mode).
    //
    // When a capture was previously open, it resumes once the run
    // concludes, along every exit route. Any statement written by a `%`
    // block or a keyword handler after its own run is treated as
    // input, just like earlier statements, yet it did not serve as an
    // anchor: only the start of the next input line began the capture
    // again. A run may trigger a throw at its termination (such as in
    // check mode, due to a negation check or a completeness violation),
    // and a handler that catches this continues execution.
    struct ResumeCapture
    {
        Reasoning* r;
        bool       resume;
        ~ResumeCapture()
        {
            if (resume) r->begin_input_capture();
        }
    } resume_capture{this, _capturing};
    end_input_capture();
    _filled_terms.clear();
    _consequence_text.clear();

    // A quiet run is silent too. The flag is cleared regardless of how the
    // run concludes, a throw included: deduce and report_contradiction read
    // it on the worker threads, and an engine left quiet would hide what any
    // subsequent run finds.
    const bool silent = silent_requested || quiet;
    struct QuietScope
    {
        Reasoning& r;
        ~QuietScope()
        {
            r._quiet = false;
            r._met_keys.clear();
            r._capture_derived = false;
        }
    } quiet_scope{*this};
    _quiet = quiet;
    _met_contradictions.clear();
    _met_keys.clear();
    _capture_derived = derived;
    _derived_facts.clear();

    // Take the facts created since the last run before anything else can add
    // to them. A classic run does not need them, but it must not leave them
    // behind either: they are covered by its own first pass, and carrying
    // them into a later incremental run would seed work twice.
    std::vector<std::pair<Node, Node>> carried;
    std::unordered_set<Node>           removed_preds;
    bool                               delta_valid     = false;
    bool                               lost_to_removal = false;
    {
        std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
        carried.swap(_delta_since_run);
        removed_preds   = _removed_preds_since_run;
        delta_valid     = _delta_valid;
        lost_to_removal = _delta_lost_to_removal;
        // From this point onward until the end of the current run, where the
        // graph is declared a fixpoint again, the record remains empty and
        // signifies nothing. A run interrupted by an error before this stage
        // -- such as a .run-export whose file fails to open -- left it valid
        // yet empty, and the next .run-delta seeded nothing from the facts
        // this run had taken over.
        _delta_valid = false;
        if (_delta_erased != 0)
            std::erase_if(carried, [](const std::pair<Node, Node>& entry)
                          { return entry.first == 0; });
        _fixpoint_at_last_run = false;
        _delta_index.clear();
        _delta_repeated.clear();
        _delta_revived.clear();
        _delta_indexed = 0;
        _delta_erased  = 0;
    }

    // What a run creates is recorded in the active cluster as derived (see
    // Network::cluster_derived), and a removal made by the run itself leaves
    // the record alone: the run ends at a fixpoint that triggers a reset.
    struct Deriving
    {
        Network& network;
        explicit Deriving(Network& n)
            : network(n) { network.enter_derivation(); }
        ~Deriving() { network.leave_derivation(); }
        Deriving(const Deriving&)            = delete;
        Deriving& operator=(const Deriving&) = delete;
    } const deriving(*_pImpl);

    // Delta seeding replaces the classic pass that would otherwise establish
    // what the rules derive from the graph as a whole. That is only equivalent
    // if the graph already was a fixpoint of these rules, so a previous run
    // and the semi-naive strategy are required.
    //
    // A new rule must observe facts that predate it, exactly the kind of
    // insight a skipped pass would have provided, thus the seeded run gives
    // each new rule that same pass for itself alone and continues seeding the
    // remaining rules as previously. A new rule announces itself in the record
    // as a fact using the Causes predicate; likewise, the `=>` fact of a rule
    // that a statement merely mentions -- though not a rule in force and
    // therefore disregarded here -- also appears in this manner. The full pass
    // is kept for a rule that has been removed since the last run (as it might
    // have been the sole mention of another rule, now in force and having
    // observed nothing), for a `=>` fact the record fails to track (the
    // count), and for a new rule when any rule in force holds a negated
    // condition, since a new rule has the potential to alter negation levels.
    std::unordered_set<Node> new_rules;
    bool                     rules_accounted = false;
    bool                     negating_rule   = false;
    if (incremental && _seminaive && !suppress_repetition && delta_valid)
    {
        size_t                   recorded_rules = 0;
        std::unordered_set<Node> seen;
        for (const auto& [f, p] : carried)
        {
            if (p != core.Causes || !_pImpl->exists(f) || !seen.insert(f).second) continue;
            ++recorded_rules;
            if (!is_mentioned(f) && has_rule_form(f)) new_rules.insert(f);
        }

        const size_t rule_count = _pImpl->get_left(core.Causes).size();
        rules_accounted         = removed_preds.count(core.Causes) == 0
                               && rule_count >= _rules_at_last_run
                               && rule_count - _rules_at_last_run <= recorded_rules;

        if (rules_accounted && !new_rules.empty())
        {
            for (const Node rule : _pImpl->get_left(core.Causes))
            {
                if (is_mentioned(rule)) continue;
                adjacency_set deductions;
                const Node    condition = parse_fact(rule, deductions);
                if (condition && condition != core.Causes && condition_contains_negation(condition, 1))
                {
                    negating_rule = true;
                    break;
                }
            }
        }
    }

    const bool seed_only = incremental
                        && _seminaive
                        && !suppress_repetition
                        && delta_valid
                        && rules_accounted
                        && !negating_rule;

    if (incremental && !seed_only && !silent)
    {
        if (lost_to_removal)
            diagnostic("Incremental run not applicable (a removal took facts a run may have derived) - running a full pass.");
        else
            diagnostic("Incremental run not applicable (no previous run, bulk load, changed rule set, or non-semi-naive strategy) - running a full pass.");
    }

    chrono::StopWatch watch;
    watch.start();

    _print_deductions     = print_deductions && !quiet;
    _export_derivations   = export_derivations;
    _skipped              = 0;
    _contradiction        = false;
    _total_matches        = 0;
    _total_contradictions = 0;
    _defeated_negations.clear();
    // Only the summary reads this, and only a non-silent run prints one --
    // which matters, because auto-run calls this per input line and the count
    // is a pass over the whole refuted index.
    _records_at_run_start = silent ? 0 : count_contradiction_records();
    // Start the banner clock here, so the first one is due a second in.
    _progress_last = std::chrono::steady_clock::now();

    if (_export_derivations)
    {
        if (_export_file.empty())
        {
            throw std::runtime_error("No export file set for .run-export");
        }
        _export = std::make_unique<io::DerivationExport>(std::filesystem::path(_export_file), this);
    }

    if (!silent)
        diagnostic("Starting reasoning with " + std::to_string(_pool->count()) + " worker threads.");

    uint64_t                    seminaive_violations = 0;
    std::vector<NegationRecord> negations_lost;

    if (_seminaive && !suppress_repetition)
    {
        // A run that keeps negation records re-tests the negations whose
        // predicates have acquired new facts -- the ones generated directly
        // by the run are collected by the delta observer, whereas those
        // created since the previous run are these.
        if (keeps_negation_records())
        {
            if (!delta_valid) _check_touched_all = true;
            for (const auto& [f, p] : carried)
            {
                (void)f;
                _check_touched.insert(p);
            }
        }

        seminaive_violations = run_fixpoint_seminaive(silent, seed_only ? &carried : nullptr, &removed_preds, &new_rules);

        if (keeps_negation_records()) negations_lost = recheck_negations();
    }
    else if (suppress_repetition)
    {
        // Single-pass mode never reaches a fixpoint, so the stratified
        // two-phase schedule does not apply; keep the historic behaviour
        // (one classic pass over all rules). The "suppressed" warning
        // below still reports pending work via _done.
        _done = false;
        if (!silent)
            diagnostic_stream() << "--- Reasoning iteration 1 (single pass) ---" << std::endl;
        for (Node rule : _pImpl->get_left(core.Causes))
            if (!is_mentioned(rule)) apply_rule(rule, 0);
        _pool->wait();
    }
    else
    {
        // Classic (naive) evaluation utilizing stratified negation. Rules
        // whose conditions contain a negation are deferred, and organized
        // into levels of negation (see negation_levels):
        //   Phase 1 saturates the positive rules (fixpoint);
        //   Phase 2 evaluates the deferred rules of the lowest level that is
        //   not yet final against that state.
        // A negation that succeeds in phase 2 is final, because
        // evaluation is inflationary -- facts are never removed, so a newly
        // derived fact can cause a negation to fail but can never cause it to
        // succeed -- provided no element operating LATER can expand the
        // domain it inspected. This is what levels provide: a deferred rule
        // runs only after every rule whose facts it tests under a negation --
        // either directly or via positive rules in between -- has exhausted
        // its derivations, whereas a rule whose facts it reads solely in a
        // positive manner does not run at a lower level, potentially
        // operating at the same level (see negation_levels). Phase-2
        // consequences can activate positive rules, thus maintaining the
        // alternation between the two phases at a single level until no
        // further facts are derived, at which point the next level starts.
        //
        // At any given level, the classical restriction remains in force:
        // rules that negate the very outcomes they contribute to deriving
        // cannot be assigned a stratified reading and are assessed within the
        // alternation above. This approach is sound only when each such
        // negation is settled by facts that the same positive saturation
        // derives, a principle explicitly justified in symbolic-core's header
        // regarding the identity fallback. Otherwise, the outcome risks
        // depending on the order in which the rule's instances are evaluated,
        // and under .parallel, this sequence can differ between runs; a
        // program that is locally stratified likewise receives no such
        // guarantee. Check mode reports a fact once its negated condition has
        // become true; it does not see a choice between two outcomes that are
        // both logically consistent.
        int iteration = 0;

        // A rule can DERIVE another rule, and the schedule outlined below is
        // built from the current rule set -- thus, any rule introduced during
        // a run appears in neither list. The rule count is therefore
        // compared after every positive saturation, prior to the execution of
        // the next negation level: a rule that is derived might produce a fact
        // that a negation examines, which is why the negation must remain
        // unresolved until the derived rule has been processed (level analysis
        // orders such a negation after the rule that derives the rule,
        // based on the consequence of the rule it writes). Upon the
        // expansion of the set, the rules are collected again, the levels are
        // recalculated, and the schedule restarts at level 0; this repeated
        // positive saturation also enables a derived rule to access facts
        // created before itself. Rules only accumulate, and an
        // identical rule is never generated more than once (rebuild_rule),
        // ensuring termination. When no rule derives another, a single
        // inspection of the rule count is required before each negation level
        // runs.
        size_t rules_before;
        do
        {
            rules_before = _pImpl->get_left(core.Causes).size();

            std::vector<Node> all_rules;
            std::vector<bool> negates;
            std::vector<Node> positive_rules;
            std::vector<Node> deferred_rules;
            if (keeps_negation_records()) _negating_rules.clear();
            for (Node rule : _pImpl->get_left(core.Causes))
            {
                // A rule that some other statement merely MENTIONS was never
                // claimed -- see Zelph::is_mentioned. Filtered where the rules
                // are COLLECTED, so the neighbour scan is paid once per rule per
                // run rather than once per iteration; a graph without rules
                // never reaches it at all.
                if (is_mentioned(rule)) continue;

                adjacency_set deductions;
                Node          condition = parse_fact(rule, deductions);
                const bool    deferred  = condition && condition != core.Causes
                                       && condition_contains_negation(condition, 1);
                (deferred ? deferred_rules : positive_rules).push_back(rule);
                all_rules.push_back(rule);
                negates.push_back(deferred);

                // The entries identify the rule based on the parent that
                // deduce() takes in, as in run_fixpoint_seminaive.
                if (deferred && keeps_negation_records())
                {
                    _negating_rules[rule]      = rule;
                    _negating_rules[condition] = rule;
                }
            }

            const NegationLevels           stratified = negation_levels(all_rules);
            const std::size_t              levels     = stratified.levels;
            std::vector<std::vector<Node>> by_level(levels);
            for (std::size_t i = 0; i < all_rules.size(); ++i)
                if (negates[i]) by_level[stratified.level[i]].push_back(all_rules[i]);

            if (!silent && !deferred_rules.empty())
                diagnostic_stream() << "Stratified schedule: " << deferred_rules.size()
                                    << " rule(s) with negated conditions deferred until positive quiescence"
                                    << (levels > 1 ? ", in " + std::to_string(levels) + " negation levels" : std::string())
                                    << "." << std::endl;

            // Positive saturation must be established initially, and once
            // more following each deferred pass that produced a derivation; a
            // pass yielding nothing maintains saturation, and the next level
            // begins immediately.
            std::size_t level          = 0;
            bool        saturate_first = true;
            while (true)
            {
                if (saturate_first)
                {
                    do
                    {
                        _done = false;
                        ++iteration;
                        if (!silent && progress_due())
                            diagnostic_stream() << "--- Reasoning iteration " << iteration << " ---" << std::endl;
                        for (Node rule : positive_rules)
                            apply_rule(rule, 0);
                        _pool->wait();
                    } while (_done);
                }

                // A rule derived during that saturation is not present in any
                // of the lists yet; the outer loop collects it before any
                // negation is tested again (see above).
                if (level >= levels || _pImpl->get_left(core.Causes).size() != rules_before) break;

                _done = false;
                if (!silent && progress_due())
                    diagnostic_stream() << "--- Deferred stratum (negation level " << level + 1 << " of " << levels << ") ---" << std::endl;
                for (Node rule : by_level[level])
                    apply_rule(rule, 0);
                _pool->wait();

                saturate_first = _done;
                if (!_done) ++level;
            }
        } while (_pImpl->get_left(core.Causes).size() != rules_before);

        _done = false;

        // Nothing here notes which predicates acquired new facts, thus
        // each record undergoes re-testing.
        if (keeps_negation_records())
        {
            _check_touched_all = true;
            negations_lost     = recheck_negations();
        }
    }

    // A negation record is re-read solely when a predicate its rule negates
    // has gained fresh facts since the last check, and only a semi-naive
    // execution that keeps records logs those predicates. Every other run --
    // a .run-once, or a run conducted while records are not being kept --
    // accepts incoming facts and those derived during the run without
    // recording them, resulting in the subsequent run that re-tests having to
    // re-test every record. Records exist solely where check mode or
    // .negation-records created them, hence this behaviour incurs no overhead
    // elsewhere.
    if (!_seminaive || suppress_repetition || !keeps_negation_records()) _check_touched_all = true;

    if (!silent)
        diagnostic_stream() << "Reasoning complete. Total unification matches processed: " << _total_matches
                            << ". Total contradictions found: " << _total_contradictions
                            << known_contradiction_note() << "." << std::endl;

    // Once per run, with the run's total, on the same channel as the deduction
    // lines it accounts for. Both of those repair something that misled a
    // reader: the count used to be flushed before every printed deduction, so
    // a run reported several REMAINDERS and none of them was the total, and it
    // used to go to the Diagnostic channel while the deductions go to Out, so
    // `zelph ... > log.txt` kept the incomplete content and dropped the notice
    // saying it was incomplete.
    //
    // What it does NOT do is explain the mode. A run only reaches this line in
    // a mode the reader chose -- the default says it with a mark on the prompt
    // instead -- and explaining the active mode after every answer is telling
    // somebody how to arrive where they already are, in the middle of the
    // result they asked for. The explanation belongs where the choice is made,
    // and .deductions gives it there.
    //
    // Kept out of the `!silent` block deliberately: auto-run is silent, and a
    // filtered auto-run is exactly where a reader loses lines without asking.
    if (_skipped > 0 && _deduction_notice)
    {
        const std::string count = std::to_string(_skipped.load());
        out("Note: " + count + (_skipped == 1 ? " deduction was hidden." : " deductions were hidden."), true);
    }

    if (_contradiction && !_quiet)
    {
        diagnostic_finding("Found one or more contradictions!", true);
    }

    if (_done && suppress_repetition && !_quiet)
    {
        out("Warning: Additional reasoning iterations are required, but have been suppressed.", true);
    }

    if (!silent)
        diagnostic_stream() << "Reasoning summary: " << _total_matches << " matches processed, "
                            << _total_contradictions << " contradictions found"
                            << known_contradiction_note() << "." << std::endl;
    if (_pool && !silent)
    {
        diagnostic_stream() << "Parallel unifications activated for " << _prof.parallel_relation_count()
                            << " distinct fixed relations." << std::endl;
    }

    _prof.clear_parallel_relations();

    // Close the export here rather than at the next run: the worker threads
    // are joined, so nothing more will be written, and a caller that keeps
    // the engine alive (a library, a test) must still find a complete file
    // the moment run() returns.
    _export.reset();
    _export_derivations = false;

    // The graph is a fixpoint of these rules now, which is the precondition a
    // later incremental run relies on. Facts created from here on are new to
    // it, so start recording again.
    // A single pass that produced a result failed to reach the fixpoint --
    // the warning above indicates this -- and a seeded run would never
    // revisit what remained incomplete. The record thus remains invalid, and
    // the next incremental request takes a full pass.
    const bool fixpoint = !(suppress_repetition && _done);
    _rules_at_last_run  = _pImpl->get_left(core.Causes).size();
    {
        std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
        drop_delta_record_locked(false);
        _delta_valid = fixpoint;
        ++_completed_runs;
        // A single pass that derived something was unable to determine if
        // additional passes would derive more (the warning above), and a
        // single pass never re-tests the negation records, which the next
        // full run performs.
        _fixpoint_at_last_run = !(suppress_repetition && (_done || keeps_negation_records()));
    }
    arm_delta_recorder();

    watch.stop();

    if (!silent)
        diagnostic_stream() << "Reasoning complete in " << watch.format() << " – "
                            << _total_matches << " matches processed, "
                            << _total_contradictions << " contradictions found"
                            << known_contradiction_note() << "." << std::endl;

    // A quiet run does not output a list. It keeps the facts within
    // _defeated_negations, which the caller reads, and only generates the
    // list when a message needs to carry it (below).
    std::string lost_list;
    if (!negations_lost.empty())
    {
        // A defeat reported inside a cluster is given back when the cluster
        // is dropped (close_cluster_negations).
        if (const std::string cluster = active_cluster_name(); !cluster.empty())
        {
            std::lock_guard<std::mutex> lock(_mtx_negation_records);
            auto&                       reported = _reported_in_cluster[cluster];
            reported.insert(reported.end(), negations_lost.begin(), negations_lost.end());
            _any_reported_in_cluster.store(true, std::memory_order_release);
        }

        // Outside check mode, the identical list triggers a warning: the
        // facts remain, and the run completes successfully.
        const std::string count = std::to_string(negations_lost.size());
        if (!_quiet && _seminaive_check)
            out_finding("Negation check: " + count + " fact(s) no longer justified -- a negated premise they were derived under now holds:", true);
        else if (!_quiet)
            out_finding("Warning: " + count + " fact(s) no longer justified -- a negated premise they were derived under now holds, "
                                              "and nothing else derives them; they stay:",
                        true);
        for (const NegationRecord& record : negations_lost)
        {
            _defeated_negations.emplace_back(record.fact, record.rule);

            std::string fact_text;
            std::string rule_text;
            string::node_to_string(this, fact_text, _lang, record.fact, 3);
            string::node_to_string(this, rule_text, _lang, record.rule, 3);
            const std::string line = "  " + string::unmark_identifiers(fact_text) + "  (by " + string::unmark_identifiers(rule_text) + ")";
            if (_quiet)
                lost_list += "\n" + line;
            else
                out_finding(line, true);
        }
    }

    std::string failure;
    if (seminaive_violations > 0)
    {
        // The graph itself is complete at this point: the safety net kept
        // re-applying classic evaluation until quiescence. The throw turns
        // the incompleteness of delta seeding into a hard failure for tests
        // and a visible error in the REPL.
        failure = "Semi-naive completeness violation: the classic verification pass derived new facts in "
                + std::to_string(seminaive_violations)
                + " extra pass(es) after the delta drained. The final graph is complete, but delta "
                  "seeding missed at least one derivation. Please report this rule set at https://github.com/acrion/zelph/issues.";
    }
    // A quiet run yields the findings of the negation check rather than
    // raising for them. Only the completeness violation still raises an
    // error, and as no output was printed, its message carries the list
    // directly.
    if (!negations_lost.empty() && _seminaive_check && (!_quiet || !failure.empty()))
    {
        // There is no issue with the evaluation strategies presented here --
        // both produce identical facts. What check mode reports is that the
        // result does not constitute a supported model according to the
        // rules: a fact rests on a negation that no longer holds, and no
        // rule derives it in any other way. Every rule remains satisfied; it
        // is the support that is lost.
        if (!failure.empty()) failure += " ";
        failure += "Negation check: " + std::to_string(negations_lost.size())
                 + " fact(s) derived under a negated premise that has come to hold, and nothing else "
                   "derives them"
                 + (_quiet ? ":" + lost_list + "\n" : std::string(" (listed above). "))
                 + "Facts only accumulate, so they stay. This happens when "
                   "a fact arrives after a negation was tested against its absence: a statement made "
                   "later, or a rule set that negates what it derives itself.";
    }
    if (!failure.empty()) throw std::runtime_error(failure);
}

void Reasoning::apply_rule(const Node& rule, Node condition)
{
    _prof.note_rule_applied(rule ? rule : condition);

    _nn_pred           = get_node("nn", "zelph");
    _nn_layers_pred    = get_node("nn-layers", "zelph");
    _closure_pred      = get_node("closure", "zelph");
    _refuted_pred      = get_node(refuted_fact_name(), "zelph");
    _closure_one_plus  = get_node("one-or-more", "zelph");
    _closure_zero_plus = get_node("zero-or-more", "zelph");

    if (should_log(1))
    {
        std::string formatted_rule;
        string::node_to_string(this, formatted_rule, _lang, rule, 3);
        log(0, "rule", "=== Applying rule " + formatted_rule + " ===");
    }
    ReasoningContext ctx;

    if (rule == 0)
    {
        assert(condition != 0);
    }
    else
    {
        condition = parse_fact(rule, ctx.rule_deductions);
        ctx.rule  = rule;
    }

    if (condition && condition != core.Causes)
    {
        ctx.current_condition = condition;
        ctx.next.clear();

        // Create initial vector with single condition
        auto conditions = std::make_shared<std::vector<Node>>();
        conditions->push_back(condition);

        // Each binding receives a single answer per query
        // (Reasoning::answer), and the bindings answered are reset for
        // every query.
        if (rule == 0) _answered.clear();
        try
        {
            evaluate(RulePos({rule, conditions, 0}), ctx, 1);
        }
        catch (const contradiction_error& error)
        {
            report_contradiction(error);
        }

        _pool->wait();
        // Released instead of cleared: a cleared set keeps the buckets
        // associated with the largest query, and every subsequent query would
        // clear them once more.
        if (rule == 0) decltype(_answered)().swap(_answered);
    }
}

void Reasoning::profiler_dump(const bool reset_after)
{
    if (!logging_active())
    {
        out("Profiler counters are inactive. Enable them with \".log -1\" "
            "(counters only, no log lines) or \".log <depth>\".",
            true);
        return;
    }

    auto shorten = [](std::string s)
    {
        constexpr size_t max_len = 120;
        for (char& c : s)
            if (c == '\n' || c == '\t') c = ' ';
        if (s.size() > max_len)
        {
            s.resize(max_len);
            s += "...";
        }
        return s;
    };

    // Rules are rendered readably (truncated) so a dump identifies the
    // dominating rule without manual .node lookups; relations by name.
    auto rule_str = [&](const Node r) -> std::string
    {
        if (!r) return "0";
        std::string rendered;
        string::node_to_string(this, rendered, _lang, r, 3);
        rendered = string::unmark_identifiers(rendered);
        if (rendered.empty()) return format(r);
        return shorten(rendered);
    };

    auto rel_str = [&](const Node r) -> std::string
    {
        if (!r) return "0";
        const std::string name = get_name(r, _lang, true);
        return name.empty() ? format(r) : name;
    };

    std::ostringstream oss;
    {
        const double sec = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - _prof.epoch_start)
                               .count();
        oss << "[prof] epoch=" << _prof.epoch_id.load(std::memory_order_relaxed)
            << " elapsed=" << sec << "s\n";
    }

    oss << _prof.core_block();

    constexpr size_t top_n      = 10;
    const auto       append_top = [&](const char*                               title,
                                      const std::unordered_map<Node, uint64_t>& source,
                                      const std::function<std::string(Node)>&   namer)
    {
        std::vector<std::pair<Node, uint64_t>> entries;
        {
            std::lock_guard<std::mutex> lk(_prof._mtx);
            entries.assign(source.begin(), source.end());
        }
        if (entries.empty()) return;
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b)
                  { return a.second > b.second; });
        if (entries.size() > top_n) entries.resize(top_n);
        oss << "  " << title << ":\n";
        for (const auto& [node, count] : entries)
            oss << "    " << count << "  " << namer(node) << "\n";
    };

    append_top("top_relations_by_scan (candidate facts scanned)", _prof.rel_scanned_facts, rel_str);
    append_top("top_relations_by_match (extract_success)", _prof.rel_matches, rel_str);
    append_top("top_rules_by_applied", _prof.rule_applied, rule_str);
    append_top("top_rules_by_facts_created", _prof.rule_facts_created, rule_str);

    diagnostic_stream() << oss.str() << std::flush;

    if (reset_after) _prof.reset_epoch(/*force=*/true);
}

// One observer serves both consumers, because Zelph holds only one slot and
// the two are not mutually exclusive: the input focus wants the facts of the
// current input line, the cross-run delta wants every fact created since the
// last run. Which of the two is fed is decided per call by _capturing, so
// arming is idempotent and safe to repeat.
void Reasoning::arm_delta_recorder()
{
    set_fact_creation_observer([this](Node f, Node p)
                               {
        if (_capturing) _input_captured.insert(f);

        std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
        if (!_delta_valid) return; // already given up on this record

        // Give up on the record rather than let it grow without bound; the
        // next incremental request then simply runs a classic pass. Note that
        // this deliberately does NOT key on input capture: facts created by a
        // script or an imported module are ordinary additions, and excluding
        // them would rule out the very case this exists for -- a program that
        // drives zelph as a library and never enters a statement.
        if (_delta_since_run.size() >= _max_delta_entries)
        {
            drop_delta_record_locked(false);
            _delta_since_run.shrink_to_fit();
            return;
        }

        _delta_since_run.emplace_back(f, p); });
}

void Reasoning::drop_delta_record_locked(const bool by_removal)
{
    _delta_valid = false;
    _delta_since_run.clear();
    _delta_index.clear();
    _delta_repeated.clear();
    _delta_revived.clear();
    _delta_indexed = 0;
    _delta_erased  = 0;
    _removed_preds_since_run.clear();
    _delta_lost_to_removal = by_removal;
}

// The record says "these facts are new since the last run, and the graph was
// a fixpoint before them", thus every removal must preserve this truth. A
// removed fact that the record lists had not been encountered by any run: its
// entries vanish, ensuring that a rule constructed within a dropped cluster
// does not trigger a full pass and that no seeding occurs from a fact that
// has disappeared. A removed fact that was present at the last run leaves its
// predicate in _removed_preds_since_run, which a seeded run must consider when
// evaluating whether a negation could now be true. Neither mechanism covers a
// derived fact whose premises still exist, which no seeded run will derive
// again: such a removal gives the record up (`derived`, note_user_removal).
void Reasoning::note_removal(const adjacency_set& removed, const bool derived)
{
    if (_pImpl->deriving()) return;

    forget_negation_records(removed);

    std::vector<Node> older;
    {
        std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
        if (!_delta_valid) return;
        if (derived)
        {
            drop_delta_record_locked(true);
            return;
        }

        for (; _delta_indexed < _delta_since_run.size(); ++_delta_indexed)
        {
            const Node f = _delta_since_run[_delta_indexed].first;
            if (!_delta_index.try_emplace(f, _delta_indexed).second) _delta_repeated.insert(f);
        }

        for (const Node n : removed)
        {
            const auto it = _delta_index.find(n);
            if (it == _delta_index.end())
            {
                older.push_back(n);
                continue;
            }

            // A rule that the hook below entered because a removal brought it
            // into force was present at the last run: removing it constitutes
            // the removal of an older rule, which could have been the last
            // mention of another, and merely erasing its entry would hide that
            // fact.
            if (_delta_revived.erase(n) != 0) older.push_back(n);

            if (_delta_repeated.erase(n) != 0)
            {
                for (size_t i = it->second; i < _delta_since_run.size(); ++i)
                {
                    if (_delta_since_run[i].first != n) continue;
                    _delta_since_run[i] = {0, 0};
                    ++_delta_erased;
                }
            }
            else
            {
                _delta_since_run[it->second] = {0, 0};
                ++_delta_erased;
            }
            _delta_index.erase(it);
        }
    }

    // The predicates are read outside the lock, which the fact-creation
    // observer takes, and while the edges remain present. Only a statement
    // that held counts, along with a rule: the removal of a rule pattern, a
    // fact involving a variable, a pattern marking, or a conjunction or
    // negation tag takes nothing from the data -- and in the core profile `~`
    // serves as the predicate for every class fact, so counting the engine's
    // own rule text would indicate a reduction in class facts whenever a rule
    // is removed.
    const Node        marking = rule_pattern_predicate(false);
    std::vector<Node> predicates;
    std::vector<Node> held_rules;
    for (const Node n : older)
    {
        const Node p = predicate_of(n);
        if (p == 0) continue;

        // A `=>` fact that a removed statement held as one of its components
        // was mentioned by that statement, and a mentioned rule is not in
        // force: if the statement was present during the most recent run,
        // the rule encountered none of the facts. Once the statement is
        // eliminated, the rule may be in force again, and for a seeded run it
        // becomes a new rule, so its `=>` enters the record just as the `=>`
        // of a created rule does. The run decides, as it does for every
        // recorded `=>`, whether the rule is in force: a mention that remains
        // keeps it out. Without this mechanism, a dropped mention would bring
        // the rule back into force without the classic pass that a new rule
        // takes, and the rule would never meet the older facts.
        FactStructurePtr structures;
        if (!try_get_genuine_structure(n, structures) || !structures) structures = get_fact_structures(this, n, 3);
        for (const FactStructure& fs : *structures)
        {
            const auto held = [&](const Node part)
            {
                if (part != n && removed.count(part) == 0 && predicate_of(part) == core.Causes) held_rules.push_back(part);
            };
            held(fs.subject);
            held(fs.predicate);
            for (const Node o : fs.objects)
                held(o);
        }

        if (p != core.Causes)
        {
            if (p == core.IsA
                && ((marking != 0 && _pImpl->has_left_edge(n, marking))
                    || _pImpl->has_left_edge(n, core.Conjunction)
                    || _pImpl->has_left_edge(n, core.Negation)))
                continue;
            if (!is_asserted_fact(n)) continue;
        }
        predicates.push_back(p);
    }
    if (predicates.empty() && held_rules.empty()) return;

    std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
    if (!_delta_valid) return;
    for (const Node p : predicates)
        _removed_preds_since_run.insert(p);
    for (const Node rule : held_rules)
    {
        if (_delta_since_run.size() >= _max_delta_entries)
        {
            drop_delta_record_locked(false);
            return;
        }
        _delta_since_run.emplace_back(rule, core.Causes);
        _delta_revived.insert(rule);
    }
}

// When a rule is eliminated, its associated negation records are removed
// alongside it. A record undergoes re-testing via its rule, but since a
// removed rule lacks any remaining condition to evaluate, the record remained
// in place for the rest of the session and was never reported. Every NAF
// derivation from a dry run whose cluster held the rule left behind such a
// record, even if the real run later re-derived the same fact using a rule
// written afterwards. A list maintained by a cluster for its drop
// (_reported_in_cluster) also holds records. Only a node with an edge to `=>`
// can be a rule, hence a removal that does not eliminate a rule costs one
// probe for each removed node, but only as long as records exist.
void Reasoning::forget_negation_records(const adjacency_set& removed)
{
    {
        std::lock_guard<std::mutex> lock(_mtx_negation_records);
        if (_negation_records.empty() && _reported_in_cluster.empty()) return;
    }

    std::unordered_set<Node> rules;
    for (const Node n : removed)
        if (Zelph::Impl::is_hash(n) && _pImpl->has_right_edge(n, core.Causes)) rules.insert(n);
    if (rules.empty()) return;

    const auto of_removed_rule = [&rules](const NegationRecord& record)
    { return rules.count(record.rule) != 0; };

    std::lock_guard<std::mutex> lock(_mtx_negation_records);
    std::erase_if(_negation_records, of_removed_rule);
    for (auto& [name, reported] : _reported_in_cluster)
        std::erase_if(reported, of_removed_rule);
}

// A name merge re-creates the facts and rules that were constructed on the
// node it absorbed, assigning them fresh identifiers, and a record names its
// fact, its rule, the condition part, and the bindings through their
// respective IDs. Originally left unchanged, the record of `jim ~ impatient`
// named a fact that no longer existed and bound X to a node that had been
// removed, causing the re-test to discard it, while a later `jim waits nil`
// defeated the conclusion without issuing a report. Each ID is now replaced
// by the node to which the merge moved it. The next re-test covers every
// record, since a merged predicate may enable a negated pattern to be
// satisfied by facts it previously held.
void Reasoning::follow_merge(const std::unordered_map<Node, Node>& moved)
{
    const auto moved_to = [&moved](Node n)
    {
        for (size_t hops = 0; hops < moved.size(); ++hops)
        {
            const auto it = moved.find(n);
            if (it == moved.end()) break;
            n = it->second;
        }
        return n;
    };
    const auto follow = [&moved_to](NegationRecord& record)
    {
        record.fact   = moved_to(record.fact);
        record.rule   = moved_to(record.rule);
        record.parent = moved_to(record.parent);
        Variables bindings;
        for (const auto& [variable, value] : record.bindings)
            bindings[moved_to(variable)] = moved_to(value);
        record.bindings = std::move(bindings);
    };

    // The list of what the most recent run reported, as returned by
    // `zelph/defeated`, is carried forward through the merge. It kept the
    // prior identifiers and named nodes that the merge had removed until the
    // next run, and it was not accessed when no record existed, since a
    // reported record departs alongside its report. Two pairs that the merge
    // combines into a single entity are named once.
    std::vector<std::pair<Node, Node>> defeated;
    std::set<std::pair<Node, Node>>    named;
    for (const auto& [fact, rule] : _defeated_negations)
    {
        const std::pair<Node, Node> pair{moved_to(fact), moved_to(rule)};
        if (named.insert(pair).second) defeated.push_back(pair);
    }
    _defeated_negations = std::move(defeated);

    std::lock_guard<std::mutex> lock(_mtx_negation_records);
    if (_negation_records.empty() && _reported_in_cluster.empty()) return;
    for (NegationRecord& record : _negation_records)
        follow(record);
    for (auto& [name, reported] : _reported_in_cluster)
        for (NegationRecord& record : reported)
            follow(record);
    _check_touched_all = true;
}

void Reasoning::note_user_removal()
{
    std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
    if (_delta_valid) drop_delta_record_locked(true);
}

std::vector<Node> Reasoning::removed_predicates_since_run() const
{
    std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
    return {_removed_preds_since_run.begin(), _removed_preds_since_run.end()};
}

size_t Reasoning::delta_entries() const
{
    std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
    return _delta_since_run.size() - _delta_erased;
}

size_t Reasoning::completed_runs() const
{
    std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
    return _completed_runs;
}

bool Reasoning::unchanged_since_last_run() const
{
    {
        std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
        if (!_fixpoint_at_last_run || !_delta_valid || _delta_since_run.size() != _delta_erased || !_removed_preds_since_run.empty()) return false;
    }
    return left_count(core.Causes) == _rules_at_last_run;
}

void Reasoning::begin_input_capture()
{
    if (_capture_suppress_depth > 0) return;
    if (_capturing) return;
    _capturing = true;
    // Collect RAW fact nodes only. Parsing a statement materializes its
    // subterms bottom-up (every cons cell of a numeral is a fact), so the
    // capture necessarily contains far more than the statement itself;
    // end_input_capture reduces it to the top-level inputs. Runs on the
    // REPL thread only -- run() ends the capture before evaluation starts.
    arm_delta_recorder();
}

void Reasoning::end_input_capture()
{
    if (!_capturing) return;
    _capturing = false;
    // The observer stays: the input focus is done with this line, the
    // cross-run delta is not. run() installs its own observer for the
    // duration of evaluation and re-arms this one when it is finished.
    arm_delta_recorder();
    settle_input_capture();
}

void Reasoning::settle_input_capture()
{
    // Survivors ACCUMULATE into the focus set: focus mode answers "what
    // follows from what I entered", and "what I entered" grows with the
    // session. Imported scripts never reach this path (imports suspend
    // input capture), and nodes materialized DURING reasoning are never
    // captured at all -- so accumulation cannot degenerate into
    // everything-is-focused. The set is cleared by .new and on mode
    // changes, not per run.
    std::unordered_set<Node> covered;
    for (const Node f : _input_captured)
    {
        adjacency_set objs;
        if (const Node subj = parse_fact(f, objs))
        {
            covered.insert(subj);
            for (const Node o : objs)
                covered.insert(o);
        }
    }

    for (const Node f : _input_captured)
    {
        if (covered.count(f)) continue;
        _input_focus.insert(f);
        adjacency_set objs;
        if (const Node subj = parse_fact(f, objs))
        {
            _input_focus.insert(subj);
            for (const Node o : objs)
                _input_focus.insert(o);
        }
    }

    _input_captured.clear();
}

void Reasoning::set_deduction_filter(const bool on)
{
    _deduction_filter = on;
}

void Reasoning::set_deduction_notice(const bool on)
{
    _deduction_notice = on;
}

void Reasoning::suppress_input_capture(const bool on)
{
    if (on)
    {
        ++_capture_suppress_depth;
        if (_capturing)
        {
            // Discard, do not reduce: whatever was captured of the triggering
            // .import line itself is command syntax, not knowledge input.
            // The observer itself stays armed -- facts an import creates are
            // still facts created since the last run.
            _capturing = false;
            _input_captured.clear();
        }
    }
    else if (_capture_suppress_depth > 0)
    {
        --_capture_suppress_depth;
    }
}

void Reasoning::clear_input_focus()
{
    _input_focus.clear();
}

// Greedy Sort to optimize execution order based on variable bindings
std::shared_ptr<std::vector<Node>> Reasoning::optimize_order(const adjacency_set& conditions, const Variables& current_vars, int depth)
{
    if (logging_active())
        _prof.optimize_order_calls.fetch_add(1, std::memory_order_relaxed);

    auto sorted = std::make_shared<std::vector<Node>>();
    sorted->reserve(conditions.size());

    // Copy to a temporary list we can destructively consume
    std::vector<Node> pending(conditions.begin(), conditions.end());
    Variables         simulated_vars = current_vars;

    // The variables a term holds: collect_variables', along with those found
    // in the members of a set pattern inside it, which unification binds by
    // pairing the members and which collect_variables does not read in a
    // rule's text. A condition whose variables all resided within a set
    // pattern was scored as a bound atom, trivially connected, that binds
    // nothing. The walk runs only where a set pattern may exist
    // (Zelph::may_hold_set_patterns), once for each term.
    const bool                                         set_patterns = may_hold_set_patterns();
    std::unordered_map<Node, std::unordered_set<Node>> set_pattern_vars;
    const auto                                         variables_of = [&](const Node nd, std::unordered_set<Node>& vars)
    {
        std::vector<Node> history;
        collect_variables(this, nd, vars, depth, history);
        if (!set_patterns) return;
        const auto [it, fresh] = set_pattern_vars.try_emplace(nd);
        if (fresh) collect_set_pattern_variables(this, nd, it->second);
        vars.insert(it->second.begin(), it->second.end());
    };

    // A term counts as "bound" for scoring purposes if the unification engine
    // can resolve it to a concrete node without scanning: atoms, bound
    // variables, and structured patterns whose variables are all bound in
    // vars_in_scope -- the latter thanks to bound-pattern grounding in
    // Unification. A pattern with unbound inner variables is as expensive as
    // an unbound variable and must not receive the bonus.
    auto is_bound_term = [&](Node nd, const Variables& vars_in_scope) -> bool
    {
        if (nd == 0) return false;
        if (Zelph::Impl::is_var(nd)) return vars_in_scope.count(nd) != 0;
        if (!Zelph::Impl::is_hash(nd) && !set_patterns) return true; // plain atom

        std::unordered_set<Node> vars;
        variables_of(nd, vars);
        for (Node v : vars)
            if (vars_in_scope.count(v) == 0) return false;
        return true;
    };

    while (!pending.empty())
    {
        auto   best_it   = pending.end();
        double max_score = -999999;

        for (auto it = pending.begin(); it != pending.end(); ++it)
        {
            // A condition that is negated receives its score from the
            // pattern it negates, just as every other condition is scored
            // by its own pattern.
            const Node    negated = negation_operand(*it);
            Node          cond    = negated != 0 ? negated : *it;
            double        score   = 0;
            adjacency_set objects;
            Node          subject = parse_fact(cond, objects); // Relation is ignored for scoring for now, could be added

            // Subjects and objects treat SIMULATED bindings (variables that
            // earlier conditions of this planned order will bind, as opposed
            // to constants and pre-existing current_vars entries)
            // asymmetrically:
            //
            //  - A subject whose variables are all simulated-bound still
            //    scores +100: bound-pattern grounding resolves it to one
            //    concrete fact node in O(1) at evaluation time, no matter
            //    WHICH nodes the variables end up bound to.
            //
            //  - An object variable that is only simulated-bound scores like
            //    an unbound one. The planner cannot know which node it will
            //    be bound to, and in table-driven rules it is systematically
            //    a small value node (digit, carry) shared by a large fraction
            //    of the relation's facts; an object-driven anchor on such a
            //    hub scans hundreds of candidates. This exact miscoring made
            //    rule PA1 order its mco condition (object E = carry digit;
            //    the E=0 anchor covers 271 of 900 dx-table facts) before the
            //    small mci state condition, letting mco dominate every
            //    profile. Only constants and really-bound variables, whose
            //    anchor node is known at planning time, earn the +50.
            if (is_bound_term(subject, simulated_vars))
                score += 100; // subject resolvable without scanning (atom, bound var, groundable pattern)
            else
                score -= 10; // subject requires scanning

            for (Node obj : objects)
            {
                if (is_bound_term(obj, current_vars))
                    score += 50; // anchor node known at planning time (constant or pre-bound variable)
                else
                    score -= 10; // unbound or simulated-only: anchor unknown, potentially a hub
            }

            // Among otherwise comparable conditions, prefer the one that binds
            // more still-unbound variables: every variable it binds can turn a
            // later condition's pattern into a direct lookup (bound-pattern
            // grounding) instead of a scan. Weight 2 keeps this a tie-breaker
            // relative to the +/-100, +/-50 and log2 terms.
            {
                std::unordered_set<Node> cond_vars;
                variables_of(cond, cond_vars);

                size_t new_vars  = 0;
                bool   connected = cond_vars.empty(); // ground condition: trivially connected
                for (Node v : cond_vars)
                {
                    if (simulated_vars.count(v) == 0)
                        ++new_vars;
                    else
                        connected = true;
                }
                score += 2.0 * static_cast<double>(new_vars);

                // Join connectivity: a condition that shares NO variable with
                // the (simulated) bindings starts an unconstrained scan of its
                // relation -- a cross product. It must lose against every
                // connected condition, whatever their cardinalities. The check
                // is deliberately based on collect_variables, i.e. variables at
                // ANY structural depth count as connecting: the connected
                // condition of the SC congruence rules, ((U + V) needssimp
                // (U + V)), carries its bound V only inside the pattern, which
                // is invisible to the subject/object boundness scores above.
                // Without this term, the new-vars bonus (+4 for the fully
                // unbound (U simp P) vs +2 here) ordered the full simp scan
                // FIRST for every seeded congruence match -- 47.6M scanned
                // simp candidates in the diffby phase alone. The penalty is
                // uniform when nothing is bound yet (classic first condition),
                // so relative order there is unchanged; it stays above the
                // guard tiers (!= -500, neural -800, negation -1000), which
                // must remain last regardless of connectivity.
                if (!connected) score -= 200;
            }

            // Negated conditions must be evaluated last to ensure
            // maximum variable binding before the existence check.
            if (negated != 0)
                score -= 1000;

            adjacency_set rels_for_score = filter(cond, core.IsA, core.RelationTypeCategory);

            // Inequality guards must be evaluated after the involved
            // variables are bound — similar to negation, but higher priority
            // (negation at -1000 is always last, != at -500 is second-to-last).
            if (rels_for_score.size() == 1 && *rels_for_score.begin() == core.Unequal)
                score -= 500;

            // Neural conditions want maximal bindings and are comparatively
            // expensive: evaluate after != (-500), before negation (-1000).
            if (_nn_pred != 0 && rels_for_score.size() == 1 && *rels_for_score.begin() == _nn_pred)
                score -= 800;

            // A path condition REFUSES both ends free, so it must not be
            // scheduled before something binds one of them -- but only then.
            // The penalty is what keeps it from refusing itself; it is not a
            // cost estimate, and reading it as one had it scheduled last even
            // where an end was a CONSTANT.
            //
            // With an end bound it is the cheap side of the join: an INDEXED
            // closure yielding one binding per node reached, against the full
            // relation scan of the ordinary condition it was queued behind.
            // Measured on `(A P31 C, C P279∗ Qx)`: last means scanning every
            // P31 fact and reachability-testing each -- 16.9 s over 200 000
            // instances, linear, where walking the closure from Qx first
            // touches the classes and nothing else.
            //
            // "Bound" is the planner's own reading: an atom, or a variable an
            // already-chosen condition of this plan binds. Both ends still
            // free -- or a shape that does not decompose -- keeps -700, which
            // is exactly the case the refusal exists for.
            if (_closure_pred != 0 && rels_for_score.size() == 1 && *rels_for_score.begin() == _closure_pred)
            {
                // `subject` is the one-step pattern of the tag fact, already
                // parsed above; parent=cond keeps the tag fact itself out of
                // its subject candidates, as in evaluate_closure.
                adjacency_set path_ends;
                const Node    from = subject == 0 ? 0 : parse_fact(subject, path_ends, cond);
                const Node    to   = path_ends.size() == 1 ? *path_ends.begin() : 0;

                const bool executable = from != 0 && to != 0
                                     && (is_bound_term(from, simulated_vars) || is_bound_term(to, simulated_vars));

                if (!executable) score -= 700;
            }

            // Prefer conditions whose predicate has fewer matching facts
            if (rels_for_score.size() == 1)
            {
                Node rel = *rels_for_score.begin();
                if (!Zelph::Impl::is_var(rel))
                {
                    // Size-only lookup: snapshot_left_of copied the entire
                    // relation extent per scoring iteration just to read its
                    // size -- catastrophic for high-cardinality relations
                    // such as P31 on a full Wikidata load.
                    const size_t n = _pImpl->left_count_of(rel);
                    if (n > 0)
                    {
                        // Subtract a small penalty proportional to log(fact_count)
                        // so that high-cardinality relations are tried last
                        score -= std::log2(static_cast<double>(n));
                    }
                }
            }

            if (score > max_score)
            {
                max_score = score;
                best_it   = it;
            }
        }

        if (best_it != pending.end())
        {
            Node best_cond = *best_it;
            if (should_log(depth + 1))
            // Log the selected condition with its score
            {
                adjacency_set rels     = filter(best_cond, core.IsA, core.RelationTypeCategory);
                std::string   rel_name = "?";
                if (rels.size() == 1) rel_name = get_name(*rels.begin(), _lang, true);
                log(depth + 1, "optorder", "Selected condition=" + format(best_cond) + " rel=" + rel_name + " score=" + std::to_string(static_cast<int>(max_score)));
            }

            sorted->push_back(best_cond);

            // Bind variables for next iteration.
            // Simulate the bindings this condition will produce: matching a
            // condition binds ALL variables occurring anywhere in it, at any
            // structural depth -- the previous shallow parse_fact() binding
            // never marked variables inside nested patterns as bound, so the
            // planner could not see that later conditions become groundable.
            {
                std::unordered_set<Node> cond_vars;
                variables_of(best_cond, cond_vars);
                for (Node v : cond_vars)
                    simulated_vars[v] = 1; // dummy bind
            }

            pending.erase(best_it);
        }
        else
        {
            // Should not happen unless empty
            break;
        }
    }

    if (should_log(depth) && !sorted->empty())
    {
        std::string order_str;
        for (size_t i = 0; i < sorted->size(); ++i)
        {
            adjacency_set rels     = filter((*sorted)[i], core.IsA, core.RelationTypeCategory);
            std::string   rel_name = "?";
            if (rels.size() == 1) rel_name = get_name(*rels.begin(), _lang, true);
            order_str += " [" + std::to_string(i) + "]=" + rel_name;
        }
        log(depth, "optorder", "Final order:" + order_str);
    }

    return sorted;
}

// Substitute the current bindings into one side of an inequality guard.
// Returns false when the side does not denote a concrete node (yet, or at
// all), in which case this check cannot decide anything and the constraint
// is left for a later one.
//
// The third case is the one that used to be missing at BOTH call sites. An
// operand may be a plain variable, a ground node -- or a STRUCTURED PATTERN
// such as (A cons R) in
//
//     ((A cons R) probe M, (A cons R) != &0) => ...
//
// Neither is_var nor "ground" describes that, and it used to be treated as
// ground: the comparison then ran against the PATTERN node, which is never
// identical to a concrete argument, so the guard silently permitted
// everything. A guard that looks right and does nothing is worse than one
// that is rejected, and this shape is the natural way to write "this
// numeral is not zero".
bool Reasoning::progress_due()
{
    // Logging turns the banners into part of the trace being read, so keep
    // all of them. Otherwise one line per second is plenty for a progress
    // indicator -- including the FIRST, so short runs stay silent instead
    // of emitting a banner nobody had time to read.
    if (logging_active()) return true;

    const auto now = std::chrono::steady_clock::now();
    if (now - _progress_last < std::chrono::seconds(1)) return false;
    _progress_last = now;
    return true;
}

bool Reasoning::resolve_guard_side(const Node item, const Variables& variables, Node& out) const
{
    if (Zelph::Impl::is_var(item))
    {
        const auto it = variables.find(item);
        if (it == variables.end() || Zelph::Impl::is_var(it->second)) return false;
        out = it->second;
        return true;
    }

    // Atoms, and variable-free structures: hash-consing makes such a
    // pattern identical to the node it denotes, so no lookup is needed.
    // Keeping this ahead of resolve_pattern also keeps the common case free
    // of graph access -- both call sites are on the join path.
    if (!Zelph::Impl::is_hash(item) || !var_in_closure(item))
    {
        out = item;
        return true;
    }

    // Resolve::Missing means the denoted fact does not exist; it can then
    // not be the node the other side is bound to, so "undecided" is the
    // right answer there too.
    std::vector<Node> history;
    return resolve_pattern(this, item, variables, out, history) == Resolve::Ok;
}

// Does any recorded `!=` guard still have an unresolved side?
//
// logic.md states the contract in as many words: `!=` is a guard
// constraint, NOT a fact lookup, and it filters variable bindings AFTER
// the involved variables are bound by positive conditions. Deferring an
// undecidable guard is therefore right while conditions are still being
// joined -- contradicts() skips it, and the binding usually arrives from a
// later condition. At the TERMINAL point no binding can arrive any more,
// so a guard that is still unresolved never filtered anything, and the
// match it would license rests on a condition that said nothing.
//
// Without this, `S != O` answered `Answer: S != O` -- on an empty network
// too -- claiming something with its variables unbound, which is exactly
// what the page says `!=` does not do.
// Was this side never bound at all?
//
// NOT the same as "resolve_guard_side answered no". That answers no for two
// different reasons, and only one of them means the guard was never
// applicable: an UNBOUND variable, versus a structured operand whose
// variables are all bound but whose denoted fact is absent from the graph.
// The absent one cannot be the node the other side holds, so the guard
// passes -- the Resolve::Missing note in resolve_guard_side, and the test
// "a structured operand denoting no existing fact does not block".
bool Reasoning::guard_side_unbound(const Node item, const Variables& variables) const
{
    if (Zelph::Impl::is_var(item))
    {
        const auto it = variables.find(item);
        return it == variables.end() || Zelph::Impl::is_var(it->second);
    }

    if (!Zelph::Impl::is_hash(item) || !var_in_closure(item)) return false;

    Node              out = 0;
    std::vector<Node> history;
    return resolve_pattern(this, item, variables, out, history) == Resolve::Unbound;
}

bool Reasoning::guards_unresolved(const Variables& variables, const Variables& unequals) const
{
    for (const auto& var : unequals)
    {
        if (guard_side_unbound(var.first, variables)) return true;
        if (guard_side_unbound(var.second, variables)) return true;
    }

    return false;
}

bool Reasoning::contradicts(const Variables& variables, const Variables& unequals) const
{
    for (const auto& var : unequals)
    {
        Node item1 = 0;
        Node item2 = 0;
        if (!resolve_guard_side(var.first, variables, item1)) continue;
        if (!resolve_guard_side(var.second, variables, item2)) continue;

        if (item1 == item2)
            return true; // contradiction, because item1 and item2 must be unequal
    }

    return false; // no contradiction
}

namespace
{
    // A container has no fact structure of its own: its members hang off it as
    // separate PartOf facts, which is the same reconstruction node_to_string
    // performs when it prints "{...}".
    bool collect_container_members(const Zelph* const z, const Node node, std::unordered_set<Node>& members)
    {
        // predicate_of before parse_fact, and predicate_of rather than
        // parse_relation: this runs over the adjacency of every atom a
        // deduction instantiates, so a node that is merely a busy constant --
        // the object of ten thousand facts -- must be rejected by an O(1)
        // store lookup per neighbour, not by reconstructing each one.
        for (const Node rel : z->get_right(node))
        {
            if (z->predicate_of(rel) != z->core.PartOf) continue;

            adjacency_set objs;
            const Node    member = z->parse_fact(rel, objs, 0);

            if (member != 0 && objs.count(node) == 1) members.insert(member);
        }

        return !members.empty();
    }

    bool is_conjunction(const Zelph* const z, const Node node)
    {
        return z->check_fact(node, z->core.IsA, {z->core.Conjunction}).is_known();
    }

    // The identifier for the set constant over `members`
    // (Zelph::set).
    Node set_constant_id(const std::unordered_set<Node>& members)
    {
        adjacency_set hashed;
        for (const Node m : members)
            hashed.insert(m);
        return Zelph::Impl::create_hash(hashed);
    }

    // When an instantiation processes a node lacking a fact structure: an
    // atom, a value, or a constant remains unchanged (Keep); a set constant
    // of the rule's text is rebuilt whenever one of its members undergoes a
    // change; a collection originating from the rule's own text --
    // specifically, a Skolem function symbol -- is substituted with its
    // corresponding term; a conjunction set is rebuilt through a
    // construction; and during a firing, the collection that a consequence
    // writes into -- the rule's bucket -- becomes the bucket's term
    // (bucket_term below), while a conjunction set that is written into
    // becomes its data term (data_term below).
    enum class Plan
    {
        Keep,
        SetConstantTemplate,
        CollectionTemplate,
        ConjunctionSet,
        BucketTerm,
        DataTerm
    };

    // The sole location that determines how an instantiation interacts with a
    // container. instantiate_container, ground_instance, and the instance
    // walk all ask it, ensuring that the instantiation, its prediction, and
    // the key of a term remain aligned. `members` receives the members it has
    // read; it reads none for a node it keeps via its id or its tag.
    //
    // Whether a collection belongs to the rule is determined by its identifier
    // (Zelph::is_rule_template): a single-bit examination. No name, no claim, and
    // no membership participates, so no subsequent writing can turn a rule's text
    // into data or transform a value into rule text, and a template that received
    // a name remains a template.
    Plan container_plan(const Zelph* const z, const Node node, const Recipe* const recipe, const Place& place, std::unordered_set<Node>& members)
    {
        // Each container is kept without a recipe: the ground corner of a
        // construction builds the rule a generator mentions, exactly as it
        // was written (Reasoning::rebuild_rule).
        if (recipe == nullptr) return Plan::Keep;

        const bool firing = recipe->mode == RecipeMode::Firing;

        // A node that is neither a hash nor a rule's own collection -- an
        // atom, a value, a term a firing built, a typed rule's conjunction
        // set -- stays itself, unless a construction meets a conjunction set
        // or a firing writes into one. Thus, the id decides before a member
        // is read: an atom is a member of every term built over a literal
        // that holds it, and reading its adjacency during each firing made a
        // run quadratic in the number of firings.
        if (!Zelph::is_hash(node) && !z->is_rule_template(node))
        {
            if (firing && place.position == Position::Value) return Plan::Keep;
            if (!is_conjunction(z, node)) return Plan::Keep;
            if (firing) return Plan::DataTerm;
            return collect_container_members(z, node, members) ? Plan::ConjunctionSet : Plan::Keep;
        }

        // A firing writes into what it names, meaning that among all nodes
        // located at that position, only the rule text undergoes
        // substitution: the rule's own bucket is replaced by its term, and
        // a conjunction set -- the conditions associated with a rule, which
        // a rule over rules can hold when the construction could not tell
        // that a membership has been written, as in `(X R C)` where R is
        // bound solely by the firing -- is replaced with its data term,
        // since a rule's text is immutable after being written. A term that
        // a firing in this run has filled is found without reading the
        // bucket's members or its tag.
        if (firing && place.position != Position::Value)
        {
            if (!Zelph::is_hash(node) && recipe->filled != nullptr && recipe->filled->count(Zelph::bucket_term_id(node)) != 0) return Plan::BucketTerm;
            if (is_conjunction(z, node)) return Plan::DataTerm;
            if (Zelph::is_hash(node)) return Plan::Keep;
            if (z->exists(Zelph::bucket_term_id(node))) return Plan::BucketTerm;
            if (!collect_container_members(z, node, members)) return Plan::Keep;
            return Plan::BucketTerm;
        }

        // A conjunction set -- either a generated rule's or a set constant
        // tagged as such -- is a rule structure: a firing keeps it,
        // a construction rebuilds it.
        if (is_conjunction(z, node))
        {
            if (firing) return Plan::Keep;
            return collect_container_members(z, node, members) ? Plan::ConjunctionSet : Plan::Keep;
        }
        if (!collect_container_members(z, node, members)) return Plan::Keep;

        // In a construction, a variable is not considered a member of a set
        // constant, just as Zelph::set_constant_members does not include it
        // in its count: the membership `X in {c d}` appearing in the
        // generator's text is rule text that names the set, regardless of
        // whether the rule being constructed treats it as a consequence or a
        // condition, or whether the generator writes it beside that rule.
        // Counted as a member, it rebuilt the set as `{a c d}` under the
        // binding X = a, a set that no one authored, which a subsequent
        // condition then matched: `(X p Y) => ((X in {c d}) => (X ok Y))`
        // derived `a ok b`. The built rule now refers to `{c d}` exactly as
        // originally written, and fact() refuses the membership
        // `a in {c d}`, just as it would in a standalone rule.
        //
        // A firing reads the set in the same way. It met the set as a
        // value beside its rule's own consequence `X in {c d}`, and
        // `(X p Y) => (z has {c d}) (X in {c d})` derived `z has {a c d}`
        // before refusing that consequence.
        //
        // A statement that holds a variable is interpreted identically
        // (Zelph::is_pattern_member): next to a rule's condition `(X p z) in
        // {(x p z) c}`, `(X q Y) => (z has {(x p z) c})` derived `z has @{(X p
        // z)}`.
        if (Zelph::is_hash(node))
            std::erase_if(members, [z](const Node m)
                          { return z->is_pattern_member(m); });

        // A set constant is its members, thus it is rebuilt whenever any of
        // them undergoes a change; a named one is a constant. What remains
        // is a collection of the rule's own text.
        if (Zelph::is_hash(node)) return z->is_named_any(node) ? Plan::Keep : Plan::SetConstantTemplate;
        return Plan::CollectionTemplate;
    }

    // Where the parts of a fact are located, beneath the fact itself at
    // `place`: the subject of a `=>` fact is a condition, and its
    // consequences are none; the objects of a membership are written
    // into, unless a construction meets the membership within a condition,
    // which writes nothing.
    Place subject_place(const FactStructure& fs, const Zelph* const z, const Place& place)
    {
        return Place{Position::Value, fs.predicate == z->core.Causes || place.condition, 0};
    }

    Place object_place(const FactStructure& fs, const Node relation, const Zelph* const z, const Recipe* const recipe, const Place& place)
    {
        const bool condition = fs.predicate != z->core.Causes && place.condition;
        if (relation != z->core.PartOf) return Place{Position::Value, condition, 0};
        if (condition && recipe != nullptr && recipe->mode == RecipeMode::Construction) return Place{Position::Member, true, fs.subject};
        return Place{Position::Into, condition, 0};
    }

    // The instance walk: what the instantiation of `n` at `place` meets,
    // traversed as instantiate_fact walks it without building anything --
    // `on_variable` for each variable, `on_container` for each node that
    // container_plan determines, using the plan and the members it
    // accesses. It proceeds into what container_plan rebuilds and stops at
    // what it keeps: a value, a constant, an atom, and during a firing a
    // bucket, whose term lacks any variable keys, and a conjunction set,
    // in which a firing introduces no substitutions. The key of a term and
    // the check for a prior witness both read this walk, so the prediction
    // names the term that the firing builds.
    //
    // A relation that remains unbound -- a consequence read without the
    // binding of a firing, as .explain interprets it -- might actually
    // represent a membership, meaning that in a firing, the objects of
    // such a relation are met both where they are kept and where they are
    // written into.
    //
    // An `on_container` returning a bool decides whether the walk proceeds
    // into what the plan rebuilds; one returning nothing allows it to
    // proceed.
    template <class OnVariable, class OnContainer>
    void walk_instantiation(const Zelph* const z, const Node n, const Recipe& recipe, const Place& place, const int depth, std::set<std::pair<Node, Position>>& seen, const OnVariable& on_variable, const OnContainer& on_container)
    {
        if (n == 0) return;
        if (Zelph::is_var(n))
        {
            on_variable(n);
            return;
        }
        if (!seen.insert({n, place.position}).second) return;

        if (Zelph::is_hash(n))
        {
            const FactStructure fs = get_preferred_structure(const_cast<Zelph*>(z), n, depth);
            if (fs.subject != 0)
            {
                walk_instantiation(z, fs.subject, recipe, subject_place(fs, z, place), depth, seen, on_variable, on_container);
                walk_instantiation(z, fs.predicate, recipe, Place{Position::Value, place.condition, 0}, depth, seen, on_variable, on_container);
                const Node relation = Zelph::is_var(fs.predicate) ? zelph::string::get(*recipe.binding, fs.predicate, fs.predicate) : fs.predicate;
                const bool open     = recipe.mode == RecipeMode::Firing && Zelph::is_var(relation);
                for (const Node o : fs.objects)
                {
                    walk_instantiation(z, o, recipe, object_place(fs, relation, z, &recipe, place), depth, seen, on_variable, on_container);
                    if (open) walk_instantiation(z, o, recipe, object_place(fs, z->core.PartOf, z, &recipe, place), depth, seen, on_variable, on_container);
                }
                return;
            }
        }

        std::unordered_set<Node> members;
        const Plan               plan = container_plan(z, n, &recipe, place, members);
        if constexpr (std::is_same_v<std::invoke_result_t<const OnContainer&, Node, Plan, const std::unordered_set<Node>&>, bool>)
        {
            if (!on_container(n, plan, members)) return;
        }
        else
        {
            on_container(n, plan, members);
        }
        if (plan == Plan::Keep || plan == Plan::BucketTerm || plan == Plan::DataTerm) return;

        const Place member_place{Position::Value, plan == Plan::ConjunctionSet, 0};
        for (const Node m : members)
            walk_instantiation(z, m, recipe, member_place, depth, seen, on_variable, on_container);
    }

    // The statement read as its instantiation reads it: during a firing,
    // the consequence as deduce takes it apart (its subject, its
    // relation, and its targets), whereas in a construction, the rule
    // being written, its consequences and its condition.
    template <class OnVariable, class OnContainer>
    void walk_statement(const Zelph* const z, const Recipe& recipe, const int depth, const OnVariable& on_variable, const OnContainer& on_container)
    {
        std::set<std::pair<Node, Position>> seen;
        adjacency_set                       parts;
        const Node                          head = z->parse_fact(recipe.statement, parts, recipe.parent);
        if (head == 0) return;

        if (recipe.mode == RecipeMode::Firing)
        {
            const adjacency_set relations = z->relation_types_of(recipe.statement);
            const Node          rel       = relations.size() == 1 ? *relations.begin() : Node{0};
            const Node          relation  = Zelph::is_var(rel) ? zelph::string::get(*recipe.binding, rel, rel) : rel;
            walk_instantiation(z, head, recipe, Place{}, depth, seen, on_variable, on_container);
            walk_instantiation(z, rel, recipe, Place{}, depth, seen, on_variable, on_container);
            for (const Node t : parts)
            {
                walk_instantiation(z, t, recipe, Place{relation == z->core.PartOf ? Position::Into : Position::Value, false, 0}, depth, seen, on_variable, on_container);
                if (Zelph::is_var(relation)) walk_instantiation(z, t, recipe, Place{Position::Into, false, 0}, depth, seen, on_variable, on_container);
            }
            return;
        }

        for (const Node c : parts)
            walk_instantiation(z, c, recipe, Place{}, depth, seen, on_variable, on_container);
        walk_instantiation(z, head, recipe, Place{Position::Value, true, 0}, depth, seen, on_variable, on_container);
    }

    // The members of a collection of the rule's text that its instance holds,
    // where the object in a condition's membership excludes the subject of
    // that membership: `X` qualifies as a member of the collection in
    // `(X in {H})` solely because the condition asserts it, and the condition
    // reasserts it of the instance.
    void without_member_of_condition(const Place& place, std::unordered_set<Node>& members)
    {
        if (place.position == Position::Member && place.member != 0 && members.size() > 1) members.erase(place.member);
    }

    // Whether a collection of the rule's text becomes a set constant: when
    // the literal is unable to determine its kind -- a member is a variable
    // or holds one as rule text, the exact test Zelph::set falls back on --
    // and each member of its instance is ground within the rule's text. A
    // firing always builds such a set (`@{Y}` turns into `{k}`, just as
    // `{Y}` does); a collection a rule writes to stays one.
    bool becomes_set_constant(const Zelph* const z, const Recipe& recipe, const Place& place, const std::unordered_set<Node>& members, const std::unordered_set<Node>& instances)
    {
        if (recipe.mode == RecipeMode::Construction && place.position == Position::Into) return false;
        if (!z->kind_unknowable(members)) return false;
        return std::none_of(instances.begin(), instances.end(), [z](const Node i)
                            { return z->holds_variable_in_text(i); });
    }

    Node instantiate_container(Zelph* z, Node node, const Variables& variables, int depth, std::vector<Node>& history, const Recipe* recipe, const Place& place);

    // The `bucket`'s term: one collection for every firing of its rule, keyed
    // by no variable, into which the firing writes rather than into the
    // rule's own collection. The data names the term, ensuring that only the
    // rule's statement ever writes into the rule's text, and the rule keeps
    // saying what it says. A firing asserts the bucket's members that hold no
    // variable within the rule's text, each instantiated as any part of a
    // consequence is, so a collection of the rule's text among them becomes a
    // term as well, and the term holds no rule text. The term can exist
    // before its rule fires, initialized as empty by a statement that binds
    // the bucket (data_term), and its members arrive with the first firing
    // all the same; subsequent firings during the run find the term as
    // complete (Recipe::filled) and read none of the bucket's members.
    Node bucket_term(Zelph* z, const Node bucket, std::unordered_set<Node>& members, const int depth, std::vector<Node>& history, const Recipe& recipe)
    {
        const Node id = Zelph::bucket_term_id(bucket);
        if (recipe.filled != nullptr && recipe.filled->count(id) != 0) return id;
        if (members.empty()) collect_container_members(z, bucket, members);

        const Variables none;
        Recipe          own{bucket, &none, RecipeMode::Firing, 0, nullptr};
        own.key = Zelph::Impl::recipe_key({});

        std::unordered_set<Node> ground;
        for (const Node m : members)
        {
            if (z->holds_variable_in_text(m)) continue;
            const Node im = instantiate_fact(z, m, none, depth, history, &own, Place{});
            ground.insert(im != 0 ? im : m);
        }

        bool created = false;
        z->recipe_collection(id, ground, created, nullptr, true);
        if (recipe.filled != nullptr) recipe.filled->insert(id);
        return id;
    }

    // Whether a variable bound to `value` refers to the data term of that
    // collection instead of the collection itself. A collection of another
    // rule's text that a statement binds -- via rule structure, as in
    // `(G => (S q C)) => ((X r Y) => (X in C))` binding the collection of a
    // ground rule, or via a marking fact -- is included within that rule's
    // text, which remains fixed after being written; what the statement
    // contributes to it is directed toward the data the collection
    // represents, its data term (Zelph::bucket_term_id), which is also what a
    // firing of a ground rule writes for it. A rule generated by a
    // construction names the data term wherever the collection appears within
    // it, so the rule it produces concerns the data. A firing replaces it
    // solely where it writes a membership into it: elsewhere, a firing
    // asserts something regarding the node it bound, as
    // `(G => (S in C)) => (C noted yes)` does concerning the collection of
    // the rule it matched. A conjunction set -- the conditions of a rule,
    // which a rule over rules binds to state something about them -- refers
    // to its data term only where a membership is written into it, in a
    // construction as well, and there a condition `X in C` in the rule it
    // generates constitutes such membership: that fact exists for the
    // condition to match, and within the set it would make X a condition of
    // the bound rule. The explicit form `(*{...} ~ conjunction)` with ground
    // conditions builds such a set as a set constant. A conjunction set a
    // firing writes into without a binding -- the rule holds it where the
    // relation was not bound at the time the rule was written -- is replaced
    // by container_plan.
    //
    // Two outcomes arise from replacing the binding, not the statement. The
    // data term is the term that remains unbound, thus for a collection
    // whose rule involves variables -- which only a marking fact can bind --
    // it is a collection that none of the rule's firings writes.
    // Furthermore, a rule restated from its bound parts,
    // `(G => (S q C)) => (G => (S q C))`, constitutes a second rule
    // concerning the data term in addition to the first, and under a switch,
    // the first remains in force.
    bool stands_for_data_term(const Zelph* const z, const Recipe* const recipe, const Place& place, const Node value)
    {
        if (recipe == nullptr || Zelph::is_var(value)) return false;
        const bool written_into = place.position != Position::Value;
        const bool own          = !Zelph::is_hash(value) && z->is_rule_template(value);
        if (!own && !written_into) return false;
        if (is_conjunction(z, value)) return written_into;
        return own && (written_into || recipe->mode == RecipeMode::Construction);
    }

    // The `value` collection's data term, for a statement that binds the
    // collection or writes into a conjunction set. Before a firing of the
    // rule that wrote the collection has built the term, no derivation exists
    // for what it contains, thus it is built empty: the members of the rule's
    // literal, and the single entity its statement writes, emerge at that
    // firing (bucket_term, instantiate_container). A conjunction set lacks
    // such a firing: its term holds what is written into it, never the
    // conditions.
    Node data_term(Zelph* const z, const Node value)
    {
        bool created = false;
        return z->recipe_collection(Zelph::bucket_term_id(value), {}, created, nullptr);
    }

    // A container within a statement represents the content that the
    // binding inserts, thus the instantiation determines it according to
    // what container_plan says. The term of a collection of the rule's
    // text is a collection whose identifier is its recipe: re-deriving
    // under identical binding results in the same node, enabling the run
    // to converge, unless the rule's own output supplies its binding (the
    // Skolem chase, akin to a fresh witness). Nothing compares members to
    // find it.
    Node instantiate_container(Zelph* z, const Node node, const Variables& variables, const int depth, std::vector<Node>& history, const Recipe* const recipe, const Place& place)
    {
        std::unordered_set<Node> members;
        const Plan               plan = container_plan(z, node, recipe, place, members);
        if (plan == Plan::Keep) return node;
        if (plan == Plan::BucketTerm) return bucket_term(z, node, members, depth, history, *recipe);
        if (plan == Plan::DataTerm) return data_term(z, node);

        const bool construction = recipe->mode == RecipeMode::Construction;
        const auto record       = [&](const Node n)
        {
            if (recipe->created != nullptr) recipe->created->push_back(n);
        };

        if (plan == Plan::CollectionTemplate) without_member_of_condition(place, members);

        // A conjunction set holds conditions; every other container holds
        // terms.
        const Place member_place{Position::Value, plan == Plan::ConjunctionSet, 0};

        std::unordered_set<Node> instances;
        bool                     changed = false;
        for (const Node m : members)
        {
            const Node im = instantiate_fact(z, m, variables, depth, history, recipe, member_place);
            if (im == 0) return node;
            if (im != m) changed = true;
            instances.insert(im);
        }

        if (plan == Plan::ConjunctionSet)
        {
            // A pre-existing set with precisely these conditions, including the
            // template's own when no modifications occurred, is the one the
            // rule holds.
            if (const Node found = find_conjunction_set(z, instances); found != 0) return found;

            bool       created = false;
            const Node id      = z->recipe_collection(Zelph::Impl::recipe_id(node, recipe_key(z, *recipe, depth), true), instances, created, recipe->created);
            if (!z->check_fact(id, z->core.IsA, {z->core.Conjunction}).is_known()) z->fact(id, z->core.IsA, {z->core.Conjunction});
            if (created) record(id);
            return id;
        }

        if (plan == Plan::SetConstantTemplate)
        {
            if (!changed) return node;
            const bool fresh = !z->exists(set_constant_id(instances));
            const Node built = z->set(instances);
            if (fresh) record(built);
            return built;
        }

        if (becomes_set_constant(z, *recipe, place, members, instances))
        {
            const bool fresh = !z->exists(set_constant_id(instances));
            const Node built = z->set(instances);
            if (fresh) record(built);
            return built;
        }

        bool       created = false;
        const Node id      = z->recipe_collection(Zelph::Impl::recipe_id(node, recipe_key(z, *recipe, depth), construction), instances, created, recipe->created, !construction);
        if (created) record(id);
        if (!construction && recipe->values != nullptr) recipe->values->push_back(id);
        return id;
    }

    // The outcome of instantiate_container, calculated without
    // generating any new entities.
    Node predict_container(const Zelph* const z, const Node node, const Variables& variables, const int depth, std::vector<Node>& history, const Recipe* const recipe, const Place& place, const bool keep_variables)
    {
        std::unordered_set<Node> members;
        const Plan               plan = container_plan(z, node, recipe, place, members);
        if (plan == Plan::Keep) return z->var_in_closure(node) && !keep_variables ? 0 : node;
        if (plan == Plan::BucketTerm || plan == Plan::DataTerm) return Zelph::bucket_term_id(node);

        if (plan == Plan::CollectionTemplate) without_member_of_condition(place, members);
        const Place member_place{Position::Value, plan == Plan::ConjunctionSet, 0};

        std::unordered_set<Node> instances;
        bool                     changed = false;
        for (const Node m : members)
        {
            const Node im = ground_instance(z, m, variables, depth, history, recipe, member_place, keep_variables);
            if (im == 0) return node;
            if (im != m) changed = true;
            instances.insert(im);
        }

        if (plan == Plan::ConjunctionSet)
        {
            if (const Node found = find_conjunction_set(z, instances); found != 0) return found;
            return Zelph::Impl::recipe_id(node, recipe_key(z, *recipe, depth), true);
        }

        if (plan == Plan::SetConstantTemplate && !changed) return node;
        if (plan == Plan::SetConstantTemplate || becomes_set_constant(z, *recipe, place, members, instances))
            return set_constant_id(instances);

        return Zelph::Impl::recipe_id(node, recipe_key(z, *recipe, depth), recipe->mode == RecipeMode::Construction);
    }

    // The variables encountered during the instance walk through a rule
    // that `n`, a member of a value a firing builds, holds: those from
    // each `=>` fact within its parts. `read` holds the nodes that have
    // been read up to this point.
    void rule_variables_within(const Zelph* const z, const Node n, const Recipe& recipe, const int depth, std::unordered_set<Node>& read, std::unordered_set<Node>& vars)
    {
        if (!Zelph::is_hash(n) || !read.insert(n).second) return;
        const FactStructure fs = get_preferred_structure(const_cast<Zelph*>(z), n, depth);
        if (fs.subject == 0) return;
        if (fs.predicate != z->core.Causes)
        {
            rule_variables_within(z, fs.subject, recipe, depth, read, vars);
            rule_variables_within(z, fs.predicate, recipe, depth, read, vars);
            for (const Node o : fs.objects)
                rule_variables_within(z, o, recipe, depth, read, vars);
            return;
        }
        std::set<std::pair<Node, Position>> seen;
        walk_instantiation(z, n, recipe, Place{}, depth, seen, [&](const Node v)
                           { vars.insert(v); },
                           [](Node, Plan, const std::unordered_set<Node>&) {});
    }
}

std::unordered_set<Node> zelph::network::statement_variables(const Zelph* const z, const Recipe& recipe, const int depth)
{
    std::unordered_set<Node> vars;
    walk_statement(z, recipe, depth, [&](const Node v)
                   { vars.insert(v); },
                   [](Node, Plan, const std::unordered_set<Node>&) {});
    return vars;
}

bool zelph::network::container_members_met(const Zelph* const z, const Node node, const Recipe& recipe, std::unordered_set<Node>& members, bool& rebuilt)
{
    rebuilt         = false;
    const Plan plan = container_plan(z, node, &recipe, Place{}, members);
    if (plan == Plan::SetConstantTemplate || plan == Plan::CollectionTemplate || plan == Plan::ConjunctionSet)
    {
        rebuilt = true;
        return !members.empty();
    }

    // A conjunction set that the firing keeps as written, a comma list among
    // them: each member appears in the instance exactly as it appears in the
    // rule's text. Every other node preserved by the firing is an atom, a
    // value, or a constant, and the firing writes none of its members. The
    // tag decides before a member is read.
    if (plan != Plan::Keep || !is_conjunction(z, node)) return false;
    members.clear();
    return collect_container_members(z, node, members);
}

bool zelph::network::keeps_rule_variable(const Zelph* const z, const Recipe& recipe, const int depth, std::unordered_set<Node>& in_values)
{
    // A firing keeps a conjunction set as written, ensuring that any
    // variable within it survives the substitution: the outer rule's own
    // variable as well as the mentioned rule's. Any other container holding
    // a variable is rebuilt, while a variable located outside a container is
    // either substituted or made fresh. The walk additionally enters the
    // value a firing builds for a collection of the rule's text. A value
    // constitutes data regardless of its content, and Zelph::var_in_closure
    // does not enter it, thus the instance around it reads as a statement:
    // if a rule inside the value were to keep a variable, that statement
    // would be derived and answered with half of a rule that nobody wrote.
    // Inside a value, a firing also keeps a variable it does not bind;
    // `in_values` receives those the walk encounters within a rule there,
    // and whether any of them stays unbound depends on the binding
    // (Reasoning::instance_keeps_variable).
    bool                     keeps = false;
    std::unordered_set<Node> read;
    walk_statement(z, recipe, depth, [](Node) {}, [&](const Node n, const Plan plan, const std::unordered_set<Node>& members)
                   {
                       if (keeps) return false;
                       if (plan == Plan::Keep && !Zelph::is_hash(n)) keeps = is_conjunction(z, n) && z->holds_variable_in_text(n);
                       if (plan == Plan::CollectionTemplate)
                           for (const Node m : members)
                               rule_variables_within(z, m, recipe, depth, read, in_values);
                       return true; });
    return keeps;
}

Node zelph::network::recipe_key(const Zelph* const z, const Recipe& recipe, const int depth)
{
    if (recipe.key) return *recipe.key;

    std::vector<std::pair<Node, Node>> pairs;
    for (const Node v : statement_variables(z, recipe, depth))
    {
        const auto it = recipe.binding->find(v);
        if (it != recipe.binding->end() && it->second != v) pairs.emplace_back(v, it->second);
    }
    std::sort(pairs.begin(), pairs.end());
    recipe.key = Zelph::Impl::recipe_key(pairs);
    return *recipe.key;
}

void zelph::network::firing_stand_ins(const Zelph* const z, const Node consequence, const Node parent, const Variables& binding, StandIns& out)
{
    // A node that the walk encounters both at the location where the
    // firing keeps it and at the point where it is substituted by a
    // term -- the objects associated with a relation that the binding
    // leaves open -- represents one of two possibilities: the stand-in
    // for any term, which also matches the node itself, and the prediction
    // under the binding of the whole rule decides.
    std::unordered_set<Node> kept;
    std::unordered_set<Node> replaced;

    const Recipe recipe{consequence, &binding, RecipeMode::Firing, parent, nullptr};
    walk_statement(z, recipe, 3, [](Node) {}, [&](const Node n, const Plan plan, const std::unordered_set<Node>& members)
                   {
                       switch (plan)
                       {
                       case Plan::Keep:
                           kept.insert(n);
                           if (replaced.count(n) != 0) out[n].term = 0;
                           break;
                       case Plan::BucketTerm:
                       case Plan::DataTerm:
                           // Met where a collection of the rule's text is
                           // rebuilt as well, the node keeps the stand-in for
                           // any term, including this one.
                           replaced.insert(n);
                           if (kept.count(n) != 0)
                               out[n].term = 0;
                           else
                               out.try_emplace(n, StandIn{false, Zelph::bucket_term_id(n)});
                           break;
                       case Plan::CollectionTemplate:
                           out[n] = StandIn{z->kind_unknowable(members), 0};
                           break;
                       case Plan::SetConstantTemplate:
                           if (rule_text_below(z, n)) out[n] = StandIn{true, 0};
                           break;
                       default:
                           break;
                       } });
}

Node zelph::network::find_conjunction_set(const Zelph* const z, const std::unordered_set<Node>& members)
{
    if (members.empty()) return 0;

    // Every set a node belongs to is reachable from it through its PartOf
    // facts, and a rule condition belongs to very few sets -- so one member
    // is enough to enumerate all candidates.
    const Node probe = *members.begin();

    for (const Node rel : z->get_right(probe))
    {
        if (z->parse_relation(rel) != z->core.PartOf) continue;

        adjacency_set objs;
        if (z->parse_fact(rel, objs, 0) != probe) continue;

        for (const Node candidate : objs)
        {
            if (candidate == probe) continue;
            if (!is_conjunction(z, candidate)) continue;

            std::unordered_set<Node> have;
            collect_container_members(z, candidate, have);
            if (have == members) return candidate;
        }
    }

    return 0;
}

Node zelph::network::instantiate_fact(Zelph* z, Node pattern, const Variables& variables, const int depth, std::vector<Node>& history, const Recipe* const recipe, const Place& place)
{
    // 1. Variable substitution, a collection of rule text bound to the
    //    variable read as its data term, as indicated by the statement
    if (Zelph::Impl::is_var(pattern))
    {
        const Node value = zelph::string::get(variables, pattern, pattern);
        if (value != pattern && stands_for_data_term(z, recipe, place, value)) return data_term(z, value);
        return value;
    }

    // 2. Cycle Check (Safety net)
    for (Node visited : history)
    {
        if (visited == pattern) return pattern; // Should be caught by get_preferred_structure, but safe is safe
    }
    history.push_back(pattern);

    // 3. Structural recursion
    FactStructure fs = get_preferred_structure(z, pattern, depth);

    if (z->should_log(depth))
        z->log(depth, "instantiate", "fact=" + z->format(pattern) + " subj=" + z->format(fs.subject) + " pred=" + z->format(fs.predicate) + " objs=" + std::to_string(fs.objects.size()));

    if (fs.subject == 0)
    {
        // Atomic, or a container -- the latter has no fact structure but does
        // have members to substitute. `pattern` stays on the history while
        // they are rebuilt, so a container that reaches itself terminates.
        const Node atom = instantiate_container(z, pattern, variables, depth, history, recipe, place);
        history.pop_back();
        return atom;
    }

    Node inst_subject  = instantiate_fact(z, fs.subject, variables, depth, history, recipe, subject_place(fs, z, place));
    Node inst_relation = instantiate_fact(z, fs.predicate, variables, depth, history, recipe, Place{Position::Value, place.condition, 0});

    adjacency_set inst_objects;
    bool          changed = (inst_subject != fs.subject) || (inst_relation != fs.predicate);

    const Place objects_at = object_place(fs, inst_relation, z, recipe, place);
    for (Node o : fs.objects)
    {
        Node io = instantiate_fact(z, o, variables, depth, history, recipe, objects_at);
        inst_objects.insert(io);
        if (io != o) changed = true;
    }

    history.pop_back();

    if (!changed)
    {
        return pattern;
    }

    // A refuted fact that a firing's consequence holds where it claims nothing
    // -- specifically, the fact that a refutation marking names, an argument
    // of an opaque predicate, or a part of a rule it mentions -- is accepted
    // in its current form (Reasoning::refuted_consequence found it).
    // Constructed via fact() at probability 1, it was rejected due to being
    // known to be wrong, and this rejection ended the run.
    //
    // Only at the place where the prediction met it: the prediction walks
    // neither the members of a container nor a composite predicate of the
    // consequence, and at the point where the instantiation meets the
    // identical fact there, the statement claims it. Taken wherever it
    // appeared, the look-up made the rule
    // "(X go Y) => (k says ((X p b) ~ refuted) {(X p b)})" produce a set
    // holding the refuted fact following "¬(t p b)", without a word, whereas
    // the typed statement is refused as known to be wrong.
    if (recipe != nullptr && recipe->refuted != nullptr)
    {
        const Node existing = Zelph::Impl::create_hash(inst_relation, inst_subject, inst_objects);
        if (std::any_of(recipe->refuted->begin(), recipe->refuted->end(), [&](const LookedUpFact& l)
                        { return l.fact == existing && l.path == history; }))
            return existing;
    }

    // A construction looks up a refuted fact that the rule it writes holds,
    // just as the parser looks one up for a typed rule: after "¬(t p b)",
    // the rule "(X go Y) => ((X p b) => (j r s))" ended its run with "known
    // to be wrong", while the typed "(t p b) => (j r s)" is accepted. A
    // condition based on the fact never matches, a marking of it within a
    // condition reads its refutation, and a consequence tied to it
    // constitutes a contradiction once the rule fires.
    if (recipe != nullptr && recipe->mode == RecipeMode::Construction)
    {
        const Answer known = z->check_fact(inst_subject, inst_relation, inst_objects);
        if (known.is_wrong()) return known.relation();
    }

    // While a construction builds a rule, a fact emerging in this context is a
    // part of that rule, and the construction marks it as a pattern once it
    // keeps the rule (Reasoning::build_rule). A firing whose consequence
    // mentions a rule records the same, and deduce() marks the parts of that
    // rule once the instance is present. A fact that existed prior, whether
    // asserted or derived, stays what it was, just as it does under a typed
    // rule. No fact is recorded that keeps a variable a firing would
    // substitute: it is a template and is never marked, and the marking reads
    // the neighbours of each recorded node -- those of `(X p k)` include X,
    // the same node in every rule a generator writes.
    const bool fresh = recipe != nullptr && recipe->created != nullptr && !z->exists(Zelph::Impl::create_hash(inst_relation, inst_subject, inst_objects));
    const Node built = z->fact(inst_subject, inst_relation, inst_objects);
    if (fresh && !z->var_in_closure(built, Zelph::VariableReading::Instance)) recipe->created->push_back(built);
    return built;
}

Node zelph::network::ground_instance(const Zelph* z, const Node pattern, const Variables& variables, const int depth, std::vector<Node>& history, const Recipe* const recipe, const Place& place, const bool keep_variables)
{
    if (Zelph::Impl::is_var(pattern))
    {
        const Node v = zelph::string::get(variables, pattern, pattern);
        if (v != pattern && stands_for_data_term(z, recipe, place, v)) return Zelph::bucket_term_id(v);
        return Zelph::Impl::is_var(v) && !keep_variables ? 0 : v;
    }

    // On a cycle, the pattern is its own instance, as the
    // `instantiate_fact` function states.
    for (const Node visited : history)
        if (visited == pattern) return pattern;

    // A fact devoid of any variable constitutes an instance of itself,
    // unless a rule's own collection located beneath it is replaced by
    // its term.
    if (Zelph::Impl::is_hash(pattern) && !z->var_in_closure(pattern) && (recipe == nullptr || !rule_text_below(z, pattern))) return pattern;

    history.push_back(pattern);
    const FactStructure fs = get_preferred_structure(const_cast<Zelph*>(z), pattern, depth);

    if (fs.subject == 0)
    {
        const Node result = predict_container(z, pattern, variables, depth, history, recipe, place, keep_variables);
        history.pop_back();
        return result;
    }

    const Node subject   = ground_instance(z, fs.subject, variables, depth, history, recipe, subject_place(fs, z, place), keep_variables);
    const Node predicate = ground_instance(z, fs.predicate, variables, depth, history, recipe, Place{Position::Value, place.condition, 0}, keep_variables);

    adjacency_set objects;
    bool          ground     = subject != 0 && predicate != 0;
    bool          changed    = subject != fs.subject || predicate != fs.predicate;
    const Place   objects_at = object_place(fs, predicate, z, recipe, place);
    for (const Node o : fs.objects)
    {
        if (!ground) break;
        const Node go = ground_instance(z, o, variables, depth, history, recipe, objects_at, keep_variables);
        if (go == 0) ground = false;
        if (go != o) changed = true;
        objects.insert(go);
    }
    history.pop_back();
    if (!ground) return 0;
    if (keep_variables && !changed) return pattern; // what instantiate_fact returns unaltered

    return Zelph::Impl::create_hash(predicate, subject, objects);
}

// Recursively collect all variable nodes from a fact pattern.
// Used to detect "fresh variables" — variables that appear only in rule
// consequences and need to be bound to newly created nodes.
void zelph::network::collect_variables(Zelph* z, Node pattern, std::unordered_set<Node>& vars, const int depth, std::vector<Node>& history)
{
    if (pattern == 0) return;

    if (Zelph::Impl::is_var(pattern))
    {
        vars.insert(pattern);
        return;
    }

    // Atoms carry no variables and no structure (same classification as
    // the lock-free gate in get_fact_structures) -- historically this
    // returned via an empty get_preferred_structure probe. The sole
    // exception is a set pattern that a query or a program typed, whose
    // variables a statement over it holds (Zelph::set_pattern_variables).
    if (!Zelph::Impl::is_hash(pattern))
    {
        if (const auto stored = z->set_pattern_variables(pattern)) vars.insert(stored->begin(), stored->end());
        return;
    }

    // O(1) store path: the exact variable set recorded at creation. On
    // authoritative stores this is IDENTICAL to the walk below: the walk
    // recurses over genuine preferred structures, and hash-consed DAGs
    // are acyclic, so both compute the same union.
    {
        std::shared_ptr<const std::unordered_set<Node>> stored;
        if (z->try_get_template_vars(pattern, stored))
        {
            if (stored) vars.insert(stored->begin(), stored->end());
            return;
        }
    }

    z->count_template_vars_walk();

    // Historical reconstruction walk (fallback after bulk paths):
    // Cycle check
    for (Node visited : history)
    {
        if (visited == pattern) return;
    }
    history.push_back(pattern);

    FactStructure fs = get_preferred_structure(z, pattern, depth);
    if (fs.subject == 0)
    {
        history.pop_back();
        return; // Atomic, non-variable node
    }

    collect_variables(z, fs.subject, vars, depth, history);
    collect_variables(z, fs.predicate, vars, depth, history);
    for (Node o : fs.objects)
    {
        // The membership's object is not entered, as in Zelph::fact.
        if (fs.predicate == z->core.PartOf && !Zelph::Impl::is_hash(o) && !Zelph::Impl::is_var(o)) continue;
        collect_variables(z, o, vars, depth, history);
    }

    history.pop_back();
}

namespace
{
    void set_pattern_variables_below(Zelph* const z, const Node node, std::unordered_set<Node>& vars, std::unordered_set<Node>& seen)
    {
        if (node == 0 || Zelph::Impl::is_var(node) || !seen.insert(node).second) return;

        const auto members_of = [&]
        {
            adjacency_set members;
            if (!z->set_pattern_members(node, members)) return;
            for (const Node m : members)
            {
                std::vector<Node> history;
                collect_variables(z, m, vars, 1, history);
                set_pattern_variables_below(z, m, vars, seen);
            }
        };

        if (!Zelph::Impl::is_hash(node)) return members_of();
        const auto structures = get_fact_structures(z, node, 1); // held: see Zelph::get_fact_objects
        if (structures->empty()) return members_of();
        for (const auto& fs : *structures)
        {
            set_pattern_variables_below(z, fs.subject, vars, seen);
            set_pattern_variables_below(z, fs.predicate, vars, seen);
            for (const Node o : fs.objects)
                set_pattern_variables_below(z, o, vars, seen);
        }
    }
}

void zelph::network::collect_set_pattern_variables(Zelph* z, const Node pattern, std::unordered_set<Node>& vars)
{
    std::unordered_set<Node> seen;
    set_pattern_variables_below(z, pattern, vars, seen);
}
