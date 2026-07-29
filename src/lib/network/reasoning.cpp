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

#include "contradiction_error.hpp"
#include "fact_structure.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <functional>
#include <sstream>
#include <vector>

using namespace zelph::network;

Reasoning::Reasoning(const io::OutputHandler& output)
    : Zelph{output}
    , _pool{std::make_unique<concurrency::ThreadPool>(std::thread::hardware_concurrency())}
    , _prof{this}
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

void Reasoning::report_contradiction(const contradiction_error& error)
{
    std::lock_guard<std::mutex> lock(_mtx_output);

    // The instantiation IS the condition pattern plus the bindings that
    // satisfied it. Variables is an ordered map, so the fold below is
    // deterministic without sorting anything, and it reuses the engine's
    // own mixing function rather than inventing a second one.
    uint64_t h = error.get_fact();
    for (const auto& [var, value] : error.get_variables())
        h = Impl::create_hash(Impl::create_hash(h, var), value);

    if (!_reported_contradictions.insert(h).second) return;

    _contradiction = true;
    ++_total_contradictions;

    if (!_print_deductions && !_export_derivations) return;

    std::string output;
    string::node_to_string(this, output, _lang, error.get_fact(), 3, error.get_variables(), error.get_parent());

    if (_print_deductions)
        out(string::unmark_identifiers(contradiction_symbol() + " ⇐ " + output), true);

    if (_export_derivations)
        _export->add("contradiction", contradiction_symbol(),
                     render_premises(error.get_fact(), error.get_variables(), error.get_parent()));
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
    _reported_contradictions.clear();
    // Start the banner clock here, so the first one is due a second in.
    _progress_last        = std::chrono::steady_clock::now();

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

    uint64_t seminaive_violations = 0;

    if (_seminaive && !suppress_repetition)
    {
        seminaive_violations = run_fixpoint_seminaive(silent, seed_only ? &carried : nullptr);
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
            apply_rule(rule, 0);
        _pool->wait();
    }
    else
    {
        // Classic (naive) evaluation with stratified negation. Rules whose
        // conditions contain a negation form a deferred stratum:
        //   Phase 1 saturates the positive rules (fixpoint);
        //   Phase 2 evaluates the deferred rules once against that state.
        // A negation succeeding in phase 2 is final (monotonicity: new
        // facts can make a negation fail, never newly succeed). Phase-2
        // consequences may feed positive rules, so the cycle repeats until
        // neither phase derives anything.
        //
        // NOTE: this implements ONE negation stratum. If a deferred rule's
        // consequences can (transitively) grow the extension of a pattern
        // negated by another deferred rule, results within phase 2 depend
        // on rule order -- the classic limitation of non-stratifiable
        // programs. Contradiction-only deferred rules (consequence !) are
        // always safe: they produce no facts.
        std::vector<Node> positive_rules;
        std::vector<Node> deferred_rules;
        for (Node rule : _pImpl->get_left(core.Causes))
        {
            adjacency_set deductions;
            Node          condition = parse_fact(rule, deductions);
            const bool    deferred  = condition && condition != core.Causes
                                   && condition_contains_negation(condition, 1);
            (deferred ? deferred_rules : positive_rules).push_back(rule);
        }

        if (!silent && !deferred_rules.empty())
            diagnostic_stream() << "Stratified schedule: " << deferred_rules.size()
                                << " rule(s) with negated conditions deferred until positive quiescence."
                                << std::endl;

        int  iteration = 0;
        bool deferred_derived;
        do
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

            deferred_derived = false;
            if (!deferred_rules.empty())
            {
                _done = false;
                if (!silent && progress_due())
                    diagnostic_stream() << "--- Deferred stratum (negation) ---" << std::endl;
                for (Node rule : deferred_rules)
                    apply_rule(rule, 0);
                _pool->wait();
                deferred_derived = _done;
            }
        } while (deferred_derived);
        _done = false;
    }

    if (!silent)
        diagnostic_stream() << "Reasoning complete. Total unification matches processed: " << _total_matches
                            << ". Total contradictions found: " << _total_contradictions << "." << std::endl;

    if (_skipped > 0) diagnostic(" (skipped " + std::to_string(_skipped) + " deductions)", true);

    if (_contradiction)
    {
        diagnostic("Found one or more contradictions!", true);
    }

    if (_done && suppress_repetition)
    {
        out("Warning: Additional reasoning iterations are required, but have been suppressed.", true);
    }

    if (!silent)
        diagnostic_stream() << "Reasoning summary: " << _total_matches << " matches processed, "
                            << _total_contradictions << " contradictions found." << std::endl;
    static std::unordered_set<Node> logged_relations;

    if (_pool && !silent)
    {
        diagnostic_stream() << "Parallel unifications activated for " << logged_relations.size()
                            << " distinct fixed relations." << std::endl;
    }

    logged_relations.clear();

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
                            << _total_contradictions << " contradictions found." << std::endl;

    if (seminaive_violations > 0)
    {
        // The graph itself is complete at this point: the safety net kept
        // re-applying classic evaluation until quiescence. The throw turns
        // the incompleteness of delta seeding into a hard failure for tests
        // and a visible error in the REPL.
        throw std::runtime_error(
            "Semi-naive completeness violation: the classic verification pass derived new facts in "
            + std::to_string(seminaive_violations)
            + " extra pass(es) after the delta drained. The final graph is complete, but delta "
              "seeding missed at least one derivation. Please report this rule set at https://github.com/acrion/zelph/issues.");
    }
}

void Reasoning::apply_rule(const Node& rule, Node condition)
{
    _prof.note_rule_applied(rule ? rule : condition);

    _nn_pred        = get_node("nn", "zelph");
    _nn_layers_pred = get_node("nn-layers", "zelph");

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

Node zelph::network::instantiate_fact(Zelph* z, Node pattern, const Variables& variables, const int depth, std::vector<Node>& history)
{
    // 1. Variable substitution
    if (Zelph::Impl::is_var(pattern))
    {
        return zelph::string::get(variables, pattern, pattern);
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
        history.pop_back();
        return pattern; // Atomic / No structure found
    }

    Node inst_subject  = instantiate_fact(z, fs.subject, variables, depth, history);
    Node inst_relation = instantiate_fact(z, fs.predicate, variables, depth, history);

    adjacency_set inst_objects;
    bool          changed = (inst_subject != fs.subject) || (inst_relation != fs.predicate);

    for (Node o : fs.objects)
    {
        Node io = instantiate_fact(z, o, variables, depth, history);
        inst_objects.insert(io);
        if (io != o) changed = true;
    }

    history.pop_back();

    if (!changed)
    {
        return pattern;
    }

    return z->fact(inst_subject, inst_relation, inst_objects);
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
