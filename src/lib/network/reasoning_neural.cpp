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
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

#include <cmath>

using namespace zelph::network;

namespace
{
    // Calibration lives here, not in the net: the reasoning engine's
    // >0.5 semantics (Answer::is_correct) expects [0,1] confidences.
    //
    // clamp, not sigmoid: nn.zph trains with one-hot MSE targets in
    // {0,1}, so raw scores concentrate near 0 (negatives) and 1
    // (positives). A sigmoid would place the >0.5 threshold at raw > 0,
    // which nearly every score passes after MSE training; clamping puts
    // the decision boundary at raw > 0.5, exactly between the targets.
    double calibrate(const double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
}

// Resolve the compiled network for a net node, (re)compiling on demand.
// The cache lives for one epoch (cleared in profiler_reset_epoch, i.e.
// on every new REPL input), so weights trained between inputs are
// picked up. Compile failures are cached as nullptr to avoid retrying
// per match.
void Reasoning::report_unusable_net(const Node net_node, const std::string& why)
{
    if (!_nn_reported.insert(net_node).second) return;
    diagnostic("Neural condition: net '" + format(net_node) + "' " + why + ".", true);
}

const NeuralNet* Reasoning::compiled_net(const Node net_node, const int depth)
{
    auto it = _nn_cache.find(net_node);
    if (it != _nn_cache.end()) return it->second.get();

    // Net definition: (net_node nn-layers <L1 ... Ln>), input layer first.
    std::vector<Node> layers;
    bool              has_definition = false;
    if (_nn_layers_pred != 0)
    {
        adjacency_set defs = get_fact_objects(net_node, _nn_layers_pred);
        has_definition     = !defs.empty();
        if (!defs.empty())
        {
            Node cell = *defs.begin();
            while (cell != 0 && cell != core.Nil && exists(cell) && parse_relation(cell) == core.Cons)
            {
                adjacency_set objs;
                const Node    car = parse_fact(cell, objs, 0);
                if (car == 0) break;
                layers.push_back(car);
                cell = objs.empty() ? core.Nil : *objs.begin();
            }
        }
    }

    std::unique_ptr<NeuralNet> net;
    if (layers.size() >= 2)
    {
        try
        {
            net = NeuralNet::compile(*this, layers);
        }
        catch (const std::exception& ex)
        {
            if (should_log(depth)) log(depth, "neural", std::string("compile failed: ") + ex.what());
            report_unusable_net(net_node, std::string("cannot be compiled: ") + ex.what());
        }
    }
    else
    {
        if (should_log(depth)) log(depth, "neural", "net " + format(net_node) + " has no valid nn-layers definition");

        // A net that is not there is a configuration error -- a misspelled
        // name, or a definition that comes after the rule -- and it makes
        // every rule consulting it permanently inert. Silence was only broken
        // by `.log`, so the rule looked fine, .list-rules showed it, and
        // nothing was ever derived. Reported once per net, on the diagnostic
        // channel, like every other "the engine could not do what was asked".
        //
        // The two cases are told apart: telling a user who wrote
        // `net nn-layers something` that there is no definition would be
        // false, and it would send them looking in the wrong place.
        report_unusable_net(net_node,
                            has_definition
                                ? "has an nn-layers definition that is not a list of at least two layers"
                                  " -- write `net nn-layers < input hidden output >`"
                                : "has no nn-layers definition, so every rule consulting it stays silent");
    }

    const NeuralNet* raw = net.get();
    _nn_cache.emplace(net_node, std::move(net));
    return raw;
}

// Evaluate a condition tagged (condition nn net_node).
//
// v1 semantics:
//   - pattern must be S P O with exactly one object
//   - subject and predicate must be bound (order ≈ after binding
//     conditions; optimize_order does this automatically)
//   - input activation: every bound component that is a member of the
//     input layer (structure-blind role assignment via membership)
//   - object bound   -> guard mode:    succeed iff sigmoid(score) > 0.5
//   - object unbound -> generator mode: one binding per output node
//     with sigmoid(score) > 0.5
//   - the confidence multiplies into rule.confidence and ends up as
//     the deduced fact's probability
//
// `negated` is the condition's `¬` tag, which the caller reads for us because
// the dispatch in Reasoning::evaluate returns here before the leaf branch that
// otherwise handles negation. Under `¬` the condition is a guard and nothing
// else: it succeeds when the net does NOT confirm the fact, it contributes no
// confidence (there is no confidence in an absence to multiply in), and
// generator mode is unavailable because a negation binds nothing.
void Reasoning::evaluate_neural(const Node condition, const RulePos& rule, ReasoningContext& ctx, const int depth, const bool negated)
{
    // The condition IS the tag fact (pattern nn net): extract both.
    adjacency_set nets;
    const Node    pattern = parse_fact(condition, nets, rule.node);
    if (pattern == 0 || nets.size() != 1)
    {
        if (should_log(depth)) log(depth, "neural", "malformed ≈ condition (expected (pattern nn net))");
        return;
    }

    // A net that cannot be consulted makes the condition fail in BOTH
    // readings, and that is deliberate. "The net does not confirm this" and
    // "there is no net" are different statements, and letting the second one
    // satisfy `¬≈` would turn a misspelled net name into a derivation engine:
    // every rule consulting it would start firing on everything. The
    // configuration error is reported by compiled_net; the rule stays inert.
    const NeuralNet* net = compiled_net(*nets.begin(), depth);
    if (!net) return; // condition fails; reason already logged

    // Decompose the inner pattern. parent=condition excludes the tag
    // fact itself from the pattern's subject candidates.
    adjacency_set pattern_objects;
    const Node    pattern_subject  = parse_fact(pattern, pattern_objects, condition);
    const Node    pattern_relation = parse_relation(pattern);

    if (pattern_subject == 0 || pattern_relation == 0 || pattern_objects.size() != 1)
    {
        if (should_log(depth)) log(depth, "neural", "unsupported ≈ pattern shape (need S P O with exactly one object)");
        return;
    }
    const Node pattern_object = *pattern_objects.begin();

    auto resolved = [&](Node n) -> Node
    { return Zelph::Impl::is_var(n) ? string::get(*rule.variables, n, n) : n; };

    const Node s = resolved(pattern_subject);
    const Node p = resolved(pattern_relation);
    const Node o = resolved(pattern_object);

    if (Zelph::Impl::is_var(s) || Zelph::Impl::is_var(p))
    {
        // v1: the subject must be bound by a preceding positive condition
        // (optimize_order schedules ≈ late, so this is the normal flow).
        if (should_log(depth)) log(depth, "neural", "≈ requires bound subject and predicate (v1)");
        return;
    }

    // Input: bound components that are input-layer members. The object is
    // never fed -- it is the query, in both modes.
    std::vector<std::pair<Node, double>> input;
    if (net->has_node(0, s)) input.emplace_back(s, 1.0);
    if (net->has_node(0, p)) input.emplace_back(p, 1.0);

    if (input.empty())
    {
        if (should_log(depth)) log(depth, "neural", "no pattern component is a member of the input layer");
        return;
    }

    std::vector<std::pair<Node, double>> scored;
    try
    {
        scored = net->eval_nodes(input);
    }
    catch (const std::exception& ex)
    {
        if (should_log(depth)) log(depth, "neural", std::string("forward pass failed: ") + ex.what());
        return;
    }

    const size_t out_layer = net->layer_count() - 1;

    if (!Zelph::Impl::is_var(o))
    {
        // --- Guard mode ---
        // Same reasoning as for an unusable net: an object the output layer
        // does not contain is one the net has no opinion about, not one it
        // rejects, so `¬≈` does not succeed on it either.
        if (!net->has_node(out_layer, o))
        {
            if (should_log(depth)) log(depth, "neural", "bound object " + format(o) + " is not a member of the output layer");
            return;
        }

        double raw = 0.0;
        for (const auto& [node, score] : scored)
        {
            if (node == o)
            {
                raw = score;
                break;
            }
        }

        const double conf = calibrate(raw);
        if (should_log(depth))
            log(depth, "neural", std::string(negated ? "¬≈ guard " : "≈ guard ") + format(s) + " " + format(p) + " " + format(o) + " => confidence " + std::to_string(conf));

        if ((conf > 0.5) != negated)
        {
            // The confidence of the branch is left alone under `¬`. What the
            // net reports is how strongly it believes the fact; one minus that
            // is not a measure of anything the deduction can carry, so a
            // negated guard filters and contributes nothing.
            proceed_after_condition(rule, ctx, depth, rule.variables, rule.unequals, negated ? rule.confidence : rule.confidence * conf);
        }
    }
    else if (negated)
    {
        // Generator mode under `¬` would have to bind the open object, and a
        // negation binds nothing -- the same wall the path condition meets.
        // "The net proposes nothing at all for this subject" is a different
        // question, and it is not this one.
        if (should_log(depth)) log(depth, "neural", "a negated ≈ condition needs a bound object");
        refuse_condition(condition,
                         "a negated ≈ condition needs a bound object -- \"≈\" under ¬ is a test "
                         "(\"the net does not confirm this\"), and a negation binds nothing. "
                         "Bind the object with a preceding condition.");
    }
    else
    {
        // --- Generator mode ---
        for (const auto& [node, score] : scored)
        {
            const double conf = calibrate(score);
            if (conf <= 0.5) continue;

            auto bindings               = std::make_shared<Variables>(*rule.variables);
            (*bindings)[pattern_object] = node;

            if (contradicts(*bindings, *rule.unequals)) continue;

            if (should_log(depth))
                log(depth, "neural", "≈ generated " + format(node) + " with confidence " + std::to_string(conf));

            proceed_after_condition(rule, ctx, depth, bindings, rule.unequals, rule.confidence * conf);
        }
    }
}

// Shared continuation for procedurally evaluated conditions (currently
// only ≈): advance to the next condition, pop a stacked conjunction
// branch, or handle the terminal (deduce / prune / answer). Mirrors the
// structure of the != guard and negation paths in reasoning_evaluate.cpp.
void Reasoning::proceed_after_condition(const RulePos&             rule,
                                        ReasoningContext&          ctx,
                                        const int                  depth,
                                        std::shared_ptr<Variables> vars,
                                        std::shared_ptr<Variables> uneqs,
                                        const double               confidence)
{
    const size_t next_index = rule.index + 1;

    if (next_index < rule.conditions->size())
    {
        RulePos next              = rule;
        next.variables            = vars;
        next.unequals             = uneqs;
        next.confidence           = confidence;
        next.index                = next_index;
        ReasoningContext ctx_copy = ctx;
        evaluate(next, ctx_copy, depth + 1);
        return;
    }

    if (!ctx.next.empty())
    {
        RulePos next    = ctx.next.back();
        next.variables  = vars;
        next.unequals   = uneqs;
        next.confidence = confidence;
        ctx.next.pop_back();
        ReasoningContext ctx_copy = ctx;
        evaluate(next, ctx_copy, depth + 1);
        return;
    }

    // Terminal: all conditions satisfied.
    ReasoningContext ctx_copy = ctx;

    if (!ctx_copy.rule_deductions.empty())
    {
        try
        {
            deduce(*vars, rule.node, depth, ctx_copy, confidence);
        }
        catch (const contradiction_error& error)
        {
            report_contradiction(error);
        }
        return;
    }

    if (_prune_mode)
    {
        // A NAMED target variable is what the deletion stands on, and it is
        // bound by the conditions, not by this one -- so a terminal reached
        // through a path condition is as good as any other. Without one the
        // ≈ v1 restriction stands: pruning removes concrete matched facts,
        // while a terminal reached through ≈ carries a confidence judgement,
        // not a matched fact instance, and the same holds for the path
        // condition, whose "match" is a walk rather than a fact.
        if (_prune_target_var != 0)
        {
            collect_prune_targets(ctx_copy.current_condition, *vars, rule.node);
        }
        return;
    }

    // Normal query output / collection
    std::lock_guard<std::mutex> lock(_mtx_output);
    if (_query_results)
    {
        _query_results->push_back(vars);
    }
    else
    {
        std::string output;
        string::node_to_string(this, output, _lang, ctx_copy.current_condition, 3, *vars, rule.node);
        out("Answer: " + string::unmark_identifiers(output), true);
    }
}