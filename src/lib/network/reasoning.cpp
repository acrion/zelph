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
#include <unordered_set>
#include <vector>

using namespace zelph::network;

Reasoning::Reasoning(const io::OutputHandler& output)
    : Zelph{output}
    , _pool{std::make_unique<concurrency::ThreadPool>(std::thread::hardware_concurrency())}
    , _prof{this}
    , _seminaive_check{default_seminaive_check()}
{
}

void Reasoning::set_export_file(const std::string& path)
{
    _export_file = path;
}

void Reasoning::set_query_collector(std::vector<std::shared_ptr<Variables>>* collector)
{
    _query_results = collector;
}

bool Reasoning::record_contradiction(const contradiction_error& error)
{
    if (!_record_contradictions) return false;

    const Node       condition = error.get_fact();
    const Variables& variables = error.get_variables();

    // The members of the condition set, found the way render_premises finds
    // them -- and the way node_to_string finds them when it prints "{...}".
    std::unordered_set<Node> matched;
    for (const Node rel : get_right(condition))
    {
        if (parse_relation(rel) != core.PartOf) continue;
        adjacency_set objs;
        const Node    element = parse_fact(rel, objs, 0);
        if (element == 0 || objs.count(condition) != 1) continue;

        // A condition that matched no FACT contributes nothing: a guard
        // filtered, a negation succeeded on absence. Instantiating one would
        // assert it -- see the note on this function.
        if (is_negated_condition(element, 0)) continue;
        const Node element_predicate = parse_relation(element);
        if (element_predicate == core.Unequal
            || (_nn_pred != 0 && element_predicate == _nn_pred)
            || (_closure_pred != 0 && element_predicate == _closure_pred))
            continue;

        // No recipe: every container remains unchanged. A condition matched a
        // fact through its nodes, and Unification compares a container via
        // its node, thus the condition with its containers preserved
        // constitutes the fact that was matched; a rebuilt container would
        // make the record name a fact that nobody derived. A condition's
        // container is matched exclusively by a fact that names that precise
        // container, which nothing writes, hence no input reaches the
        // difference.
        std::vector<Node> history;
        const Node        instance = instantiate_fact(this, element, variables, 0, history);
        if (instance == 0) return false; // cannot name this one; report it as new
        matched.insert(instance);
    }

    // A rule whose conditions are ALL guards or negations leaves nothing to
    // point at. Rare, and the honest answer is to keep reporting it rather
    // than to invent a record.
    if (matched.empty()) return false;

    const Node record = set(matched);
    if (record == 0) return false;

    if (is_refuted_fact(record)) return true;

    mark_refuted_fact(record);
    return false;
}

void Reasoning::report_contradiction(const contradiction_error& error)
{
    // Before the output lock, never under it: this takes the network locks,
    // and deduce establishes network-then-output as the order.
    const bool already_known = record_contradiction(error);

    std::lock_guard<std::mutex> lock(_mtx_output);

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

    const bool print = _print_deductions || _contradictions_printed;
    if (!print && !_export_derivations) return;

    std::string output;
    string::node_to_string(this, output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());

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

std::vector<std::string> Reasoning::render_premises(const Node condition, const Variables& variables, const Node parent) const
{
    std::vector<std::string> out;

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

void Reasoning::run(const bool print_deductions, const bool export_derivations, const bool suppress_repetition, const bool silent, const bool incremental)
{
    // Input capture must not extend into evaluation: in classic mode the
    // observer would otherwise collect every deduced fact into the focus
    // set, neutralizing the filter (semi-naive mode replaces the observer
    // anyway, but the boundary belongs here, not to the evaluation mode).
    end_input_capture();
    _filled_terms.clear();

    // Take the facts created since the last run before anything else can add
    // to them. A classic run does not need them, but it must not leave them
    // behind either: they are covered by its own first pass, and carrying
    // them into a later incremental run would seed work twice.
    std::vector<std::pair<Node, Node>> carried;
    bool                               delta_valid = false;
    {
        std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
        carried.swap(_delta_since_run);
        delta_valid = _delta_valid;
    }

    // Delta seeding replaces the classic pass that would otherwise establish
    // what the rules derive from the graph as a whole. That is only equivalent
    // if the graph already was a fixpoint of these rules, so a previous run
    // and the semi-naive strategy are required -- and the rules must not have
    // changed since, because a new rule has to see facts that are older than
    // itself, which is precisely what the skipped pass would have shown it.
    //
    // A new rule announces itself in the delta as a fact with the Causes
    // predicate. The rule COUNT cannot carry that alone: get_left returns the
    // distinct condition nodes, so two rules sharing a condition collapse into
    // one entry. Both checks are kept -- the delta catches additions, the
    // count catches wholesale replacement (e.g. .load).
    bool rules_changed = false;
    for (const auto& [f, p] : carried)
    {
        (void)f;
        if (p == core.Causes)
        {
            rules_changed = true;
            break;
        }
    }

    const size_t rule_count = _pImpl->get_left(core.Causes).size();
    const bool   seed_only  = incremental
                           && _seminaive
                           && !suppress_repetition
                           && delta_valid
                           && !rules_changed
                           && rule_count == _rules_at_last_run;

    if (incremental && !seed_only && !silent)
        diagnostic("Incremental run not applicable (no previous run, bulk load, changed rule set, or non-semi-naive strategy) - running a full pass.");

    chrono::StopWatch watch;
    watch.start();

    _print_deductions     = print_deductions;
    _export_derivations   = export_derivations;
    _skipped              = 0;
    _contradiction        = false;
    _total_matches        = 0;
    _total_contradictions = 0;
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
        // Check mode re-tests the negations whose predicates have acquired
        // new facts -- the ones generated directly by the run are collected
        // by the delta observer, while those created since the previous run
        // are these.
        if (_seminaive_check)
        {
            if (!delta_valid) _check_touched_all = true;
            for (const auto& [f, p] : carried)
            {
                (void)f;
                _check_touched.insert(p);
            }
        }

        seminaive_violations = run_fixpoint_seminaive(silent, seed_only ? &carried : nullptr);

        if (_seminaive_check) negations_lost = recheck_negations();
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
    }

    // Check mode re-reads a negation record solely when a predicate its rule
    // negates has acquired new facts since the last check, and only a
    // check-mode semi-naive execution logs those predicates. Every other run
    // -- a .run-once, or a run conducted while check mode is inactive --
    // accepts incoming facts and those derived during the run without logging
    // them, causing the subsequent check run to re-test every record. Records
    // are present exclusively where check mode created them, so this
    // behaviour incurs no cost outside check mode.
    if (!_seminaive || suppress_repetition || !_seminaive_check) _check_touched_all = true;

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

    if (_contradiction)
    {
        diagnostic_finding("Found one or more contradictions!", true);
    }

    if (_done && suppress_repetition)
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
    _rules_at_last_run = _pImpl->get_left(core.Causes).size();
    {
        std::lock_guard<std::mutex> lock(_mtx_delta_since_run);
        _delta_since_run.clear();
        _delta_valid = true;
    }
    arm_delta_recorder();

    watch.stop();

    if (!silent)
        diagnostic_stream() << "Reasoning complete in " << watch.format() << " – "
                            << _total_matches << " matches processed, "
                            << _total_contradictions << " contradictions found"
                            << known_contradiction_note() << "." << std::endl;

    if (!negations_lost.empty())
    {
        out_finding("Negation check: " + std::to_string(negations_lost.size())
                        + " fact(s) no longer justified -- a negated premise they were derived under now holds:",
                    true);
        for (const NegationRecord& record : negations_lost)
        {
            std::string fact_text;
            std::string rule_text;
            string::node_to_string(this, fact_text, _lang, record.fact, 3);
            string::node_to_string(this, rule_text, _lang, record.rule, 3);
            out_finding("  " + string::unmark_identifiers(fact_text) + "  (by " + string::unmark_identifiers(rule_text) + ")", true);
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
    if (!negations_lost.empty())
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
                   "derives them (listed above). Facts only accumulate, so they stay. This happens when "
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
    }

    if (condition && condition != core.Causes)
    {
        ctx.current_condition = condition;
        ctx.next.clear();

        // Create initial vector with single condition
        auto conditions = std::make_shared<std::vector<Node>>();
        conditions->push_back(condition);

        try
        {
            evaluate(RulePos({rule, conditions, 0}), ctx, 1);
        }
        catch (const contradiction_error& error)
        {
            report_contradiction(error);
        }

        _pool->wait();
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
            _delta_valid = false;
            _delta_since_run.clear();
            _delta_since_run.shrink_to_fit();
            return;
        }

        _delta_since_run.emplace_back(f, p); });
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

    // Survivors ACCUMULATE into the focus set: focus mode answers "what
    // follows from what I entered", and "what I entered" grows with the
    // session. Imported scripts never reach this path (imports suspend
    // input capture), and nodes materialized DURING reasoning are never
    // captured at all -- so accumulation cannot degenerate into
    // everything-is-focused. The set is cleared by .reset and on mode
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
        if (!Zelph::Impl::is_hash(nd)) return true; // plain atom

        std::unordered_set<Node> vars;
        std::vector<Node>        history;
        collect_variables(this, nd, vars, depth, history);
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
            Node          cond  = *it;
            double        score = 0;
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
                std::vector<Node>        history;
                collect_variables(this, cond, cond_vars, depth, history);

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
            if (is_negated_condition(cond, depth))
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
                std::vector<Node>        history;
                collect_variables(this, best_cond, cond_vars, depth, history);
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

    bool is_negation(const Zelph* const z, const Node node)
    {
        return z->check_fact(node, z->core.IsA, {z->core.Negation}).is_known();
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
        // Every container is kept without a recipe: the record of a
        // contradiction names the facts that were matched, and a
        // condition's container that is matched by its node
        // (Reasoning::record_contradiction).
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
        on_container(n, plan, members);
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
            const adjacency_set relations = z->filter(recipe.statement, z->core.IsA, z->core.RelationTypeCategory);
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
            if (im != m)
            {
                changed = true;
                // The negation tag is a fact concerning a condition, thus
                // requiring the instance to be told again.
                if (plan == Plan::ConjunctionSet && is_negation(z, m)) z->fact(im, z->core.IsA, {z->core.Negation});
            }
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
}

std::unordered_set<Node> zelph::network::statement_variables(const Zelph* const z, const Recipe& recipe, const int depth)
{
    std::unordered_set<Node> vars;
    walk_statement(z, recipe, depth, [&](const Node v)
                   { vars.insert(v); },
                   [](Node, Plan, const std::unordered_set<Node>&) {});
    return vars;
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

    // A rule within the pattern -- the consequence of a generator, located
    // one or more levels beneath the rule being derived. Its single
    // condition can be negated, and the tag indicating this negation is a
    // fact ABOUT the pattern, thus it must be restated on the instance, just
    // as rebuild_condition does for the outermost rule and
    // instantiate_container does for the members of a conjunction. Omitting
    // it caused `¬(X blocks H)` to become `(X blocks k)`, a rule saying the
    // opposite of what was originally stated.
    if (inst_relation == z->core.Causes && inst_subject != fs.subject && is_negation(z, fs.subject))
        z->fact(inst_subject, z->core.IsA, {z->core.Negation});

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

    // While a construction builds a rule, a fact emerging in this context is
    // a part of that rule, and the construction marks it as a pattern once it
    // keeps the rule (Reasoning::build_rule). A fact that existed prior,
    // whether asserted or derived, stays what it was, just as it does under a
    // typed rule. No fact is recorded that keeps a variable a firing would
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
    // returned via an empty get_preferred_structure probe.
    if (!Zelph::Impl::is_hash(pattern)) return;

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
        collect_variables(z, o, vars, depth, history);
    }

    history.pop_back();
}
