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
#include "rule_identity.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "unification.hpp"
#include "zelph_impl.hpp"

#include <functional>
#include <optional>
#include <string_view>

using namespace zelph::network;

namespace
{
    // Held by a firing of a rule using a fresh variable, starting from the
    // witness check and progressing to its final consequence; refer to
    // deduce(). A single mutex per process: only these rules take it, and no
    // rule in the standard library employs a fresh variable.
    std::mutex fresh_witness_mutex;

    // The elements of a conjunction set: the subjects of the PartOf facts
    // pointing at it. The same reconstruction Reasoning::evaluate performs
    // when it takes a rule condition apart, and node_to_string when it
    // prints "{...}".
    std::vector<Node> conjunction_members(const Zelph* const z, const Node set_node)
    {
        std::vector<Node> members;
        for (const Node rel : z->get_right(set_node))
        {
            if (z->parse_relation(rel) != z->core.PartOf) continue;
            adjacency_set objs;
            const Node    s = z->parse_fact(rel, objs, 0);
            if (s != 0 && objs.count(set_node) == 1) members.push_back(s);
        }
        return members;
    }

    // Whether the instance a firing of `deduction` writes is the
    // consequence's own pattern node containing a variable: a firing keeps
    // the condition set of a rule that the consequence mentions, including
    // all variables, and when no other substitutions occur, the instance is
    // the pattern itself. That node is rule text, and a firing does not
    // derive it: since the pattern already exists, deduce() writes nothing,
    // makes no claim, and prints nothing about it, and
    // consequences_already_exist counts it as present -- otherwise, a rule
    // with a fresh variable next to it would make a new witness on each
    // pass. A ground consequence is also its own pattern, and deriving that
    // one constitutes a claim.
    bool instance_is_rule_text(const Zelph* const z, const Node instance, const Node deduction)
    {
        return instance == deduction && z->var_in_closure(deduction);
    }
}

bool Reasoning::in_input_focus(const Node node, const int depth_left) const
{
    if (node == 0) return false;
    if (_input_focus.count(node) != 0) return true;
    if (depth_left <= 0 || !Zelph::Impl::is_hash(node)) return false;

    const FactStructure fs = get_preferred_structure(this, node, 3);
    if (fs.subject == 0) return false;

    if (in_input_focus(fs.subject, depth_left - 1)) return true;
    for (const Node o : fs.objects)
        if (in_input_focus(o, depth_left - 1)) return true;

    return false;
}

bool Reasoning::deduction_is_rule(const Node deduction) const
{
    // The edge probe in front is what keeps this off the hot path: `=>` is a
    // neighbour of a fact node only when it is that fact's predicate or its
    // subject, which no ordinary deduction has -- so the exact reading is
    // reconstructed for candidates only.
    return has_right_edge(deduction, core.Causes) && parse_relation(deduction) == core.Causes;
}

std::unordered_set<Node> Reasoning::rule_variables(const Node rule, const Node parent, const int depth) const
{
    std::unordered_set<Node> vars;
    Zelph* const             self = const_cast<Reasoning*>(this);

    adjacency_set consequences;
    const Node    condition = parse_fact(rule, consequences, parent);
    if (condition == 0) return vars;

    for (const Node c : consequences)
    {
        std::vector<Node> history;
        collect_variables(self, c, vars, depth, history);
    }

    // A conjunction set node carries no structure a variable walk could
    // follow -- Zelph::set creates it, and its members hang off it as
    // separate PartOf facts. Without this step the variables of a
    // multi-condition rule would be invisible.
    std::vector<Node> pending{condition};
    while (!pending.empty())
    {
        const Node cond = pending.back();
        pending.pop_back();

        if (check_fact(cond, core.IsA, {core.Conjunction}).is_known())
        {
            for (const Node m : conjunction_members(this, cond))
                pending.push_back(m);
            continue;
        }

        std::vector<Node> history;
        collect_variables(self, cond, vars, depth, history);
    }

    return vars;
}

// NOTE on the tag checks in this file: rebuilding a rule REPRODUCES what was
// written, so the `~ conjunction` tag is the right discriminator here and
// Zelph::is_condition_set is not. That helper answers "does the engine read
// this container as a set of conditions", which is deliberately wider -- a
// single untagged member counts -- and applying it here turned a one-element
// container in a generated CONSEQUENCE into a conjunction set.
Node Reasoning::rebuild_condition(const Node pattern, const Variables& variables, const int depth, const Recipe* const recipe)
{
    // A conjunction nested inside the condition forms a distinct set, which
    // evaluate() processes by traversing it recursively; instantiate_fact
    // reconstructs it as one (container_plan), ensuring that a rule nested
    // within a generated rule takes the outer binding in its conditions as
    // well.
    std::vector<Node> history;
    const Node        inst = instantiate_fact(this, pattern, variables, depth, history, recipe, Place{Position::Value, true, 0});
    if (inst == 0) return 0;

    // The negation tag is a fact ABOUT the pattern, not a part of it, so
    // instantiation cannot carry it along -- it has to be restated. When the
    // pattern came back unchanged the tag is already on the node.
    if (inst != pattern && check_fact(pattern, core.IsA, {core.Negation}).is_known())
        fact(inst, core.IsA, {core.Negation});

    return inst;
}

namespace
{
    // The cluster in which a construction performs its builds
    // (ConstructionScratch).
    const char* const kConstructionScratch = "__construction";

    // A construction generates the components of a rule before determining
    // whether to retain them: the rule it produces might mirror what a rule in
    // force already states, and in such a case, the construction returns that
    // existing rule. A part that is itself a rule -- the nested rule within a
    // switch -- would become a rule in force once the rule that was to mention
    // it is not written, and no mark can neutralize a `=>` fact. Thus, the
    // construction operates within a scratch cluster of its own, much like
    // zelph/dedup-rule runs a typed rule, which tracks exactly the nodes it
    // creates: keep() passes those nodes to the cluster that was active,
    // ensuring that .cluster-drop still takes a generated rule back with the
    // rest of an experiment, and drop() removes them.
    //
    // Every alternative exit drops them too, including a thrown exception,
    // before the exception leaves the construction. A scratch left active
    // would record each node created afterwards, the user's own statements
    // included, and the next construction returning another rule would
    // remove them all. For the same reason, a construction never starts
    // while a cluster bearing the scratch's name is active: constructions do
    // not nest, meaning the cluster is a scratch that was not closed or one
    // the user activated under that name, and the drop would take whatever
    // it holds.
    //
    // Throughout its entire span, the construction holds the template
    // scope open, on the thread that runs it: it writes a rule, and the
    // memberships it writes into that rule's own collections and
    // conjunction set constitute the rule's text, which Zelph::fact
    // refuses whenever no rule is being written.
    class ConstructionScratch
    {
    public:
        explicit ConstructionScratch(Zelph& z)
            : _z(z)
            , _previous(z.active_cluster_name())
            , _scope(z)
        {
            if (_previous == kConstructionScratch)
                throw std::runtime_error(std::string("Reasoning::rebuild_rule: the cluster \"") + kConstructionScratch + "\" that a construction runs in is already active");
            _z.set_active_cluster(kConstructionScratch);
        }

        ~ConstructionScratch()
        {
            if (!_open) return;
            try
            {
                drop();
            }
            catch (...)
            {
                // When an exception is being unwound, a destructor that runs
                // must not throw; the cluster was restored before the drop
                // began.
            }
        }

        ConstructionScratch(const ConstructionScratch&)            = delete;
        ConstructionScratch& operator=(const ConstructionScratch&) = delete;

        void keep()
        {
            restore();
            _z.merge_cluster(kConstructionScratch, _previous);
            _open = false;
        }

        // drop_scratch_cluster eliminates every node with the invalidation a
        // fact() of its shape performs, ensuring the fact stores remain armed:
        // nothing outside the construction can reference what it created yet
        // -- fact() operates without concurrent caller, and constructions do
        // not interleave.
        void drop()
        {
            restore();
            _open = false;
            _z.drop_scratch_cluster(kConstructionScratch);
        }

    private:
        void restore() const
        {
            if (_previous.empty())
                _z.deactivate_cluster();
            else
                _z.set_active_cluster(_previous);
        }

        const Zelph&               _z;
        const std::string          _previous;
        const Zelph::TemplateScope _scope;
        bool                       _open{true};
    };
}

Node Reasoning::rebuild_rule(const Node pattern, const Variables& variables, const int depth, const Node parent, bool& created)
{
    created = false;

    // What the construction creates: the components marked as rule patterns
    // once it keeps them (build_rule).
    std::vector<Node> made;
    const Recipe      recipe{pattern, &variables, RecipeMode::Construction, parent, &made};

    // A rule lacking any variable outside its containers, under a binding
    // that substitutes nothing, is the rule the generator mentions: the
    // ground corner Zelph::is_mentioned names. It is built as the
    // generator wrote it, each container preserved, and settles upon the
    // mention.
    const Node key    = recipe_key(this, recipe, depth);
    const bool corner = key == Zelph::Impl::recipe_key({}) && rule_variables(pattern, parent, depth).empty();

    if (!corner)
        if (const Node claimed = claimed_rule(pattern, key); claimed != 0)
        {
            _prof.constructions_remembered.fetch_add(1, std::memory_order_relaxed);
            return claimed;
        }

    _prof.constructions_built.fetch_add(1, std::memory_order_relaxed);
    ConstructionScratch scratch(*this);
    bool                kept = false;
    const Node          rule = build_rule(pattern, variables, depth, parent, created, corner ? nullptr : &recipe, kept);

    if (rule != 0 && kept)
        scratch.keep();
    else
    {
        scratch.drop();
        created = false;
    }

    // Built or claimed, this is the rule that every subsequent construction
    // under the same statement and key returns, which a classic pass and
    // each statement under auto-run would otherwise build anew.
    if (rule != 0 && !corner) note_claim(pattern, key, rule);
    return rule;
}

Node Reasoning::build_rule(const Node pattern, const Variables& variables, const int depth, const Node parent, bool& created, const Recipe* const recipe, bool& kept)
{
    created = false;
    kept    = false;

    adjacency_set var_consequences;
    const Node    var_condition = parse_fact(pattern, var_consequences, parent);
    if (var_condition == 0 || var_consequences.empty()) return 0;

    adjacency_set consequences;
    for (const Node c : var_consequences)
    {
        std::vector<Node> history;
        const Node        inst = instantiate_fact(this, c, variables, depth, history, recipe);
        if (inst == 0) return 0;
        consequences.insert(inst);
    }

    Node condition = 0;

    if (check_fact(var_condition, core.IsA, {core.Conjunction}).is_known())
    {
        std::unordered_set<Node> members;
        for (const Node m : conjunction_members(this, var_condition))
        {
            const Node inst = rebuild_condition(m, variables, depth, recipe);
            if (inst == 0) return 0;
            members.insert(inst);
        }
        if (members.empty()) return 0;

        // The rule's own conjunction set: an already present one containing
        // precisely these conditions -- the rule's own as originally written,
        // provided no modifications occurred -- or a collection built by the
        // construction under its recipe, tagged as conditions. Never a set
        // constant: such a thing might represent a data set sharing the same
        // members, which would then be interpreted as a rule's conditions.
        condition = find_conjunction_set(this, members);
        if (condition == 0)
        {
            bool created_set = false;
            condition        = recipe != nullptr ? recipe_collection(Zelph::Impl::recipe_id(var_condition, recipe_key(this, *recipe, depth), true), members, created_set, recipe->created)
                                                 : set(members);
            if (!check_fact(condition, core.IsA, {core.Conjunction}).is_known()) fact(condition, core.IsA, {core.Conjunction});
            if (created_set && recipe->created != nullptr) recipe->created->push_back(condition);
        }
    }
    else
    {
        // A single-condition rule has no set node at all -- its `=>` subject
        // is the condition itself, and that one is hash-consed, so the
        // check_fact below deduplicates it without any help.
        condition = rebuild_condition(var_condition, variables, depth, recipe);
        if (condition == 0) return 0;
    }

    // The construction produces a rule structure, which is marked only
    // after being kept -- and exclusively at that moment: a construction
    // that returns a different rule leaves no part behind (rebuild_rule).
    // No identity test beneath examines a mark.
    const auto keep = [&](const Node rule)
    {
        kept = true;
        if (recipe != nullptr && recipe->created != nullptr && !recipe->created->empty())
            mark_rule_parts(condition, consequences, *recipe->created);
        return rule;
    };

    // A rule that is already ASSERTED is not news.
    const Answer existing = check_fact(condition, core.Causes, consequences);
    if (existing.is_known())
    {
        if (!is_mentioned(existing.relation())) return keep(existing.relation());

        // It exists, but only as a MENTION -- and a mention was never
        // claimed, so nothing fires (see Zelph::is_mentioned). That is not a
        // corner case here: whenever the outer rule substitutes nothing into
        // the inner one, hash-consing lands the rebuild on the very node the
        // outer rule mentions. It is exactly the shape a switchable rule has,
        //
        //     (K is on) => ((X p Y) => (X q Y))
        //
        // which would otherwise turn the feature on and derive nothing.
        //
        // Claiming it needs a node of its own, and alpha-renaming gives one:
        // each statement names its own variables anyway, so a renamed copy
        // says exactly the same thing while being a statement rather than a
        // reference to one.
        for (const Node candidate : get_left(core.Causes))
        {
            if (candidate == existing.relation()) continue;
            if (is_mentioned(candidate)) continue;
            if (rules_alpha_equivalent(this, candidate, existing.relation())) return candidate;
        }

        Variables renamed = variables;
        for (const Node v : rule_variables(pattern, parent, depth))
        {
            if (renamed.find(v) != renamed.end()) continue;

            const Node fresh = var();
            // The name travels along, or the rule prints as "(?? p ??)".
            // Reusing it is what the parser does too -- every statement
            // creates its own node for the variable it calls X.
            const std::string name = get_name(v, _lang, true);
            if (!name.empty()) set_name(fresh, name, _lang, false);
            renamed[v] = fresh;
        }

        // No variable to rename: a GROUND rule that is asserted and mentioned
        // at once is one node, and the graph cannot tell the two apart. The
        // same corner Zelph::is_mentioned names.
        if (renamed.size() == variables.size()) return keep(existing.relation());

        if (recipe == nullptr) return build_rule(pattern, renamed, depth, parent, created, nullptr, kept);
        const Recipe renaming{pattern, &renamed, RecipeMode::Construction, parent, recipe->created};
        return build_rule(pattern, renamed, depth, parent, created, &renaming, kept);
    }

    const Node built = fact(condition, core.Causes, consequences);

    // A rule in force that says the same is that rule: a typed twin, a rule
    // a different generator wrote, the rule a name merge rebuilt. The rule
    // freshly constructed goes with the rest of the construction
    // (rebuild_rule).
    if (const Node twin = find_equivalent_rule(built, false); twin != 0) return twin;

    created = true;
    return keep(built);
}

Node Reasoning::existing_rule(const Node pattern, const Variables& variables, const int depth, const Node parent) const
{
    adjacency_set var_consequences;
    const Node    var_condition = parse_fact(pattern, var_consequences, parent);
    if (var_condition == 0 || var_consequences.empty()) return 0;

    // The recipe rebuild_rule instantiates with, and its ground corner.
    const Recipe  recipe{pattern, &variables, RecipeMode::Construction, parent, nullptr};
    const bool    corner = recipe_key(this, recipe, depth) == Zelph::Impl::recipe_key({}) && rule_variables(pattern, parent, depth).empty();
    const Recipe* used   = corner ? nullptr : &recipe;

    // Inner variables stay variables, exactly as build_rule
    // leaves them.
    adjacency_set consequences;
    for (const Node c : var_consequences)
    {
        std::vector<Node> history;
        const Node        inst = ground_instance(this, c, variables, depth, history, used, Place{}, true);
        if (inst == 0) return 0;
        consequences.insert(inst);
    }

    Node condition = 0;
    if (check_fact(var_condition, core.IsA, {core.Conjunction}).is_known())
    {
        std::unordered_set<Node> members;
        for (const Node m : conjunction_members(this, var_condition))
        {
            std::vector<Node> history;
            const Node        inst = ground_instance(this, m, variables, depth, history, used, Place{Position::Value, true, 0}, true);
            if (inst == 0) return 0;
            members.insert(inst);
        }
        condition = find_conjunction_set(this, members);
        if (condition == 0 && used != nullptr)
        {
            const Node id = Zelph::Impl::recipe_id(var_condition, recipe_key(this, recipe, depth), true);
            if (exists(id)) condition = id;
        }
    }
    else
    {
        std::vector<Node> history;
        condition = ground_instance(this, var_condition, variables, depth, history, used, Place{Position::Value, true, 0}, true);
    }
    if (condition == 0) return 0;

    const Answer existing = check_fact(condition, core.Causes, consequences);
    if (!existing.is_known()) return 0;
    if (!is_mentioned(existing.relation())) return existing.relation();

    // Only mentioned: build_rule claimed an alpha-renamed duplicate
    // instead.
    for (const Node candidate : get_left(core.Causes))
    {
        if (candidate == existing.relation()) continue;
        if (is_mentioned(candidate)) continue;
        if (rules_alpha_equivalent(this, candidate, existing.relation())) return candidate;
    }
    return 0;
}

bool Reasoning::asserts_fact(const Node deduction) const
{
    // A rule deduction is constructed through rebuild_rule, and deduce
    // bypasses any deduction that does not have precisely one predicate.
    return deduction != core.Contradiction
        && !deduction_is_rule(deduction)
        && filter(deduction, core.IsA, core.RelationTypeCategory).size() == 1;
}

std::optional<Reasoning::ConsequenceParts> Reasoning::predict_consequence(const Node deduction, const Variables& variables, const int depth, const Node parent) const
{
    // Subject, predicate, and objects are read as deduce() interprets
    // them, according to the recipe of its firing. The deduction seeds the
    // history, as there, so that it is not confused with the subject of
    // its own subject.
    const adjacency_set relations = filter(deduction, core.IsA, core.RelationTypeCategory);
    if (relations.size() != 1) return std::nullopt;

    const Recipe     recipe{deduction, &variables, RecipeMode::Firing, parent, nullptr};
    ConsequenceParts parts;
    adjacency_set    var_targets;
    const Node       var_source = parse_fact(deduction, var_targets, parent);

    std::vector<Node> history{deduction};
    parts.source = ground_instance(this, var_source, variables, depth, history, &recipe, Place{}, true);

    history        = {deduction};
    parts.relation = ground_instance(this, *relations.begin(), variables, depth, history, &recipe, Place{}, true);

    if (parts.source == 0 || parts.relation == 0 || var_targets.empty()) return std::nullopt;

    const Place targets_at{parts.relation == core.PartOf ? Position::Into : Position::Value, false, 0};
    for (const Node var_t : var_targets)
    {
        history      = {deduction};
        const Node t = ground_instance(this, var_t, variables, depth, history, &recipe, targets_at, true);
        if (t == 0) return std::nullopt;
        parts.targets.insert(t);
    }
    return parts;
}

Node Reasoning::existing_consequence(const Node deduction, const Variables& variables, const int depth, const Node parent) const
{
    const auto parts = predict_consequence(deduction, variables, depth, parent);
    if (!parts) return 0;
    const Answer answer = check_fact(parts->source, parts->relation, parts->targets);
    return answer.is_known() ? answer.relation() : 0;
}

void Reasoning::deduce(const Variables& variables, const Node parent, const int depth, ReasoningContext& ctx, const double confidence)
{
    if (logging_active())
        _prof.deduce_calls.fetch_add(1, std::memory_order_relaxed);

    if (should_log(depth))
    {
        std::string vars_str;
        for (const auto& [k, v] : variables)
            vars_str += " " + format(k) + "=" + format(v);
        log(depth, "deduce", "BEGIN with bindings:" + vars_str);
    }

    // --- Fresh Variable Detection ---
    // Variables appearing in consequences yet unbound by conditions are
    // classified as "fresh variables": upon a rule's firing, a new node is
    // created for each such variable, provided that no pre-existing nodes
    // already make every consequence a fact (refer to
    // consequences_already_exist). This mechanism enables rules to
    // construct new graph topology (existential witnesses).

    // A deduction that is itself a RULE is exempt: its variables are
    // quantified by that inner rule, not by the outer one, and turning them
    // into fresh nodes would derive a rule that says nothing -- conditions
    // still carrying the unbound pattern variables, a conclusion over nodes
    // no condition can ever bind. They stay variables; see rebuild_rule.
    std::unordered_set<Node> deduction_vars;
    for (const Node deduction : ctx.rule_deductions)
    {
        if (deduction == core.Contradiction) continue;
        if (deduction_is_rule(deduction)) continue;
        std::vector<Node> history;
        collect_variables(this, deduction, deduction_vars, depth, history);
    }

    std::unordered_set<Node> fresh_vars;
    for (Node var : deduction_vars)
    {
        if (variables.find(var) == variables.end())
            fresh_vars.insert(var);
    }

    // --- Termination Check ---
    // The check and the creation of the witness must not be interleaved
    // with any other firing, as otherwise both would fail to locate a
    // witness and both proceed to create one. Currently, this cannot
    // happen: .parallel splits the fact scanning phase of a Unification,
    // and the thread that evaluates the rule drains its matches and calls
    // deduce() alone. The lock ensures that check and creation remain
    // together should firings ever run in parallel.
    std::unique_lock<std::mutex> witness_lock;
    if (!fresh_vars.empty())
    {
        witness_lock = std::unique_lock<std::mutex>(fresh_witness_mutex);

        if (logging_active())
        {
            _prof.fresh_vars_total.fetch_add(fresh_vars.size(), std::memory_order_relaxed);

            if (should_log(depth))
            {
                std::string fresh_str;
                for (Node fv : fresh_vars)
                    fresh_str += " " + format(fv);
                log(depth, "deduce", "Fresh variables:" + fresh_str);
            }
        }

        if (consequences_already_exist(variables, ctx.rule_deductions, parent, depth))
        {
            if (logging_active())
            {
                _prof.termination_guard_checks.fetch_add(1, std::memory_order_relaxed);
                _prof.termination_guard_skips.fetch_add(1, std::memory_order_relaxed);

                if (should_log(depth))
                    log(depth, "deduce", "consequences_already_exist => SKIP (termination guard)");
            }
            return;
        }
        if (logging_active())
        {
            _prof.termination_guard_checks.fetch_add(1, std::memory_order_relaxed);

            if (should_log(depth))
                log(depth, "deduce", "consequences_already_exist => false, proceeding");
        }
    }
    else if (should_log(depth))
    {
        log(depth, "deduce", "No fresh variables");
    }

    // --- Create Fresh Nodes ---
    Variables augmented = variables;
    for (Node var : fresh_vars)
    {
        Node fresh;
        {
            std::lock_guard<std::mutex> lock(_mtx_network);
            fresh = _pImpl->create();
        }
        augmented[var] = fresh;

        if (logging_active())
            _prof.fresh_nodes_created.fetch_add(1, std::memory_order_relaxed);

        if (should_log(depth))
            log(depth, "deduce", "Created fresh node " + std::to_string(fresh) + " for " + format(var));
    }

    // --- Process Deductions ---
    for (const Node deduction : ctx.rule_deductions)
    {
        if (deduction == core.Contradiction)
        {
            throw contradiction_error(ctx.current_condition, augmented, parent);
        }

        adjacency_set relations = filter(deduction, core.IsA, core.RelationTypeCategory);

        if (relations.size() != 1)
        {
            if (should_log(depth))
                log(depth, "deduce", "Deduction " + format(deduction) + " has " + std::to_string(relations.size()) + " relations, skipping");
            continue;
        }

        Node rel = Zelph::Impl::is_var(*relations.begin())
                     ? string::get(augmented, *relations.begin(), Node{0})
                     : *relations.begin();

        if (!rel || Zelph::Impl::is_var(rel))
        {
            if (should_log(depth))
                log(depth, "deduce", "Deduction " + format(deduction) + ": relation resolved to null or to a variable, skipping");
            continue;
        }

        adjacency_set var_targets;
        Node          var_source = parse_fact(deduction, var_targets, parent);

        if (var_targets.empty())
        {
            if (should_log(depth))
                log(depth, "deduce", "Deduction " + format(deduction) + ": no targets found, skipping");
            continue;
        }

        // All instantiation and fact creation happens under one lock to
        // prevent races where parallel threads create the same node.
        Node          source = 0;
        adjacency_set targets;
        Node          d       = 0;
        bool          wrong   = false;
        bool          created = false;
        std::string   refusal;
        // A derived RULE is not a statement ABOUT anything, so the focus
        // filter has no subject to match it against -- and suppressing the
        // one deduction that changes what the engine will do next is the
        // wrong default. It prints whenever deductions print at all.
        const bool is_rule = rel == core.Causes;

        if (is_rule)
        {
            std::lock_guard<std::mutex> lock_network(_mtx_network);
            d = rebuild_rule(deduction, augmented, depth, parent, created);

            if (should_log(depth))
                log(depth, "deduce", "Derived rule: " + (d ? format(d) : "NULL") + (created ? " (new)" : " (already present)") + " (from pattern " + format(deduction) + ")");
        }
        else
        {
            std::lock_guard<std::mutex> lock_network(_mtx_network);

            // One instantiation of this consequence: a collection of the rule's
            // own text, contained within it, becomes its term under this
            // binding.
            const Recipe recipe{deduction, &augmented, RecipeMode::Firing, parent, nullptr, &_filled_terms};

            // Seed history with the deduction node so that get_preferred_structure()
            // skips it as a parent and does not mistake it for the subject of var_source.
            std::vector<Node> history{deduction};
            source = instantiate_fact(this, var_source, augmented, depth, history, &recipe);

            if (should_log(depth))
                log(depth, "deduce", "Instantiated source: " + (source ? format(source) : "NULL") + " (from pattern " + format(var_source) + ")");

            // The predicate is a pattern like the rest of the consequence. The
            // substitution above it covers a predicate that IS a variable and
            // nothing else, so a COMPOSITE one kept the rule's own variables:
            // `(X p Y) => (X (Y r s) c)` derived `a (Y r s) c`, a fact carrying
            // a template variable that no query can ever match.
            const Node var_rel = rel;
            history            = {deduction};
            rel                = instantiate_fact(this, rel, augmented, depth, history, &recipe);

            if (rel != var_rel && should_log(depth))
                log(depth, "deduce", "Instantiated relation: " + (rel ? format(rel) : "NULL") + " (from pattern " + format(var_rel) + ")");

            if (!rel || Zelph::Impl::is_var(rel)) source = 0;

            if (source)
            {
                bool done = true;

                // A PartOf object is written INTO: a value remains unchanged,
                // the rule's own bucket transforms into its term, one node
                // for every firing -- the accumulator idiom -- and a
                // conjunction set its data term. In any other location, a
                // container of the rule's text is a value of this binding.
                const Place targets_at{rel == core.PartOf ? Position::Into : Position::Value, false, 0};
                for (Node var_t : var_targets)
                {
                    history = {deduction};
                    Node t  = instantiate_fact(this, var_t, augmented, depth, history, &recipe, targets_at);

                    if (should_log(depth))
                        log(depth, "deduce", "Instantiated target: " + (t ? format(t) : "NULL") + " (from pattern " + format(var_t) + ")");

                    if (t)
                    {
                        targets.insert(t);
                    }
                    else
                    {
                        done = false;
                        break;
                    }
                }

                if (done)
                {
                    // Ground guard: after instantiation, the deduced fact
                    // must not contain variables at any depth. A residual
                    // variable means a rule variable was bound to a graph
                    // variable (template leak); asserting it would
                    // materialize partially instantiated junk facts.
                    // Defense in depth -- the primary fix is the deep
                    // template reject in extract_bindings.
                    std::unordered_set<Node> residual;
                    std::vector<Node>        ground_history;
                    collect_variables(this, source, residual, depth, ground_history);
                    // The predicate too: it was left out, which is how a
                    // composite predicate carrying a rule variable reached the
                    // graph before the instantiation above was added.
                    ground_history.clear();
                    collect_variables(this, rel, residual, depth, ground_history);
                    for (Node t : targets)
                    {
                        ground_history.clear();
                        collect_variables(this, t, residual, depth, ground_history);
                    }
                    if (!residual.empty())
                    {
                        done = false;
                        if (should_log(depth))
                            log(depth, "deduce", "SKIP: instantiated deduction is not ground (template-leak guard)");
                    }
                }

                if (done)
                {
                    Answer answer = check_fact(source, rel, targets);

                    // The node may exist only because some rule was written
                    // with this very statement as a ground pattern. Deriving
                    // it is a claim, so the mark goes and the deduction
                    // counts as new -- "(A is bad) => (alarm is on)" has to
                    // start answering the moment something IS bad.
                    const bool was_pattern = answer.is_known() && !answer.is_wrong()
                                          && !instance_is_rule_text(this, answer.relation(), deduction)
                                          && unmark_rule_pattern(answer.relation());

                    if (should_log(depth))
                    {
                        std::string targets_str;
                        for (Node t : targets)
                            targets_str += " " + format(t);
                        log(depth, "deduce", "check_fact(" + format(source) + ", " + format(rel) + "," + targets_str + ") => " + (answer.is_known() ? (answer.is_wrong() ? "WRONG" : "KNOWN/exists") : "UNKNOWN/new") + (targets.count(rel) ? " [target==rel, skip]" : ""));
                    }

                    if (answer.is_wrong())
                    {
                        if (logging_active())
                            _prof.check_fact_wrong.fetch_add(1, std::memory_order_relaxed);

                        wrong = true;
                    }
                    else if ((was_pattern || !answer.is_known()) && targets.count(rel) == 0)
                    {
                        if (logging_active())
                            _prof.check_fact_new.fetch_add(1, std::memory_order_relaxed);

                        try
                        {
                            // Confidence < 1 (from ≈ conditions) becomes the fact's probability in
                            // the shared weight store. Note: if the fact already exists (known
                            // correct), the existing probability is NOT upgraded or touched.
                            d       = fact(source, rel, targets, confidence);
                            created = true;

                            if (_seminaive_check)
                                if (const auto negating = _negating_rules.find(parent); negating != _negating_rules.end())
                                {
                                    std::lock_guard<std::mutex> lock_records(_mtx_negation_records);
                                    _negation_records.push_back({d, negating->second, parent, variables});
                                }

                            if (logging_active())
                            {
                                _prof.note_rule_created_fact(parent);
                                _prof.facts_created.fetch_add(1, std::memory_order_relaxed);
                                _prof.log_after_deduction(parent, d, depth);
                            }
                        }
                        catch (const std::exception& ex)
                        {
                            if (should_log(depth))
                                log(depth, "deduce", "fact() threw: " + std::string(ex.what()));

                            // fact() refuses a shape it cannot represent -- a
                            // set constant being extended, a subject that is
                            // also one of several objects. The rule stops here
                            // either way, but reporting it as a bare `!` said
                            // the knowledge base contradicts itself, which is
                            // not what happened and gives the user nothing to
                            // act on. Carry the reason to the report.
                            refusal = ex.what();

                            constexpr std::string_view prefix = "fact(): ";
                            if (refusal.starts_with(prefix)) refusal.erase(0, prefix.size());

                            wrong = true;
                        }
                    }
                    else if (logging_active() && answer.is_known())
                        _prof.check_fact_known.fetch_add(1, std::memory_order_relaxed);
                }
                else if (should_log(depth))
                {
                    log(depth, "deduce", "Target instantiation incomplete, skipping deduction");
                }
            }
        } // _mtx_network released

        if (wrong)
            throw contradiction_error(ctx.current_condition, augmented, parent, refusal);

        if (created)
        {
            std::lock_guard<std::mutex> lock(_mtx_output);

            // Focus mode: only deductions ABOUT a subject of the SESSION are
            // printed -- typed, piped, or a line of a script named on the
            // command line (see ScriptRole in repl_state.hpp); a module
            // loaded with .import contributes no anchor. The applied rule is
            // deliberately NOT an anchor either: with session-wide
            // accumulation, rule anchors would make focus degenerate to "all"
            // for any entered (or pasted) rule set. A subject the deduction
            // CONSTRUCTED is reached through what it was constructed of --
            // see in_input_focus.
            const bool focus_reject = _print_deductions && _deduction_filter && !is_rule
                                   && !in_input_focus(source, _focus_subject_depth);

            const bool do_print = _print_deductions && !focus_reject;

            // What the count MEANS is "derived, and you did not get to see
            // it", so a deduction that reaches the derivation export is not
            // one of them: the caller of .run-export asked for a file rather
            // than for lines. Counting it made .run-export report "(skipped
            // 1 deductions)" once per deduction, for deductions all of which
            // were in the JSON it had just written.
            if (!do_print && !_export_derivations) ++_skipped;

            if (do_print || _export_derivations)
            {
                std::string input, output;
                string::node_to_string(this, input, _lang, ctx.current_condition, 3, augmented, parent);
                string::node_to_string(this, output, _lang, d, 3, {}, parent);

                if (do_print)
                {
                    print_pending_verification_marker();
                    out(string::unmark_identifiers(output + " ⇐ " + input), true);
                }

                if (_export_derivations)
                {
                    // The export keeps the premises apart, which the printed
                    // line cannot: it renders the condition SET, and a
                    // consumer would have to take the braces back apart.
                    _export->add("deduction", output, render_premises(ctx.current_condition, augmented, parent));
                }
            }

            _done = true;

            if (should_log(depth))
                log(depth, "deduce", "CREATED fact " + format(d));
        }
        else if (should_log(depth) && !wrong)
        {
            log(depth, "deduce", "No new fact created (already exists or skipped)");
        }
    }
}

// Whether this firing would add nothing: true if certain nodes for the
// fresh variables already ensure that every consequence is an existing
// fact. The rule then possesses a witness and produces nothing -- the
// restricted chase.
//
// The check performs a join across the consequences, where condition
// variables are bound and fresh ones remain free, with the Unification that
// matches rule conditions handling the matching process. It explores each
// possible candidate for a fresh variable shared between two consequences,
// identifies a self-fact, a fresh variable appearing in any position and one
// nested within a consequence, and never takes a rule's own consequence
// pattern for a fact, since a pattern inherently carries variables. The
// manually coded walk it supersedes accomplished none of these tasks. It kept
// the initial candidate encountered, skipped self-facts, abandoned the
// process when both subject and object were fresh, missed nested variables,
// and matched rule patterns -- thus causing a rule to create a new witness
// for every input line, or to never fire at all.
//
// A candidate is read as a firing writes it, not as a query reads it
// (VariableReading::Instance): a firing keeps the conditions of a rule its
// consequence mentions, variables included, and a query bypasses such a
// fact as rule text. Read the query's way, the fact a firing wrote would
// never be found, and the rule would make a witness on each pass.
//
// A consequence that is ground according to the bindings is searched for as the
// exact node deduce() would assert, anticipated by ground_instance using the
// firing's recipe so that the check creates nothing: a fact containing
// additional objects constitutes a different fact, and a collection of the
// rule's text serves as the term the firing builds -- for a bucket, the
// bucket's term. A consequence where the instance walk meets a fresh variable
// left free by the bindings currently lacks a prediction, because that variable
// keys the term of a template it resides within; the join binds it first.
// Unification matches a bucket as its term and a collection of the rule's text
// as any term a firing builds (firing_stand_ins), and a join that matched via
// the latter is confirmed by the prediction once every witness is bound.
//
// The check determines solely whether a witness can be found, so the
// particular candidate encountered first is irrelevant. What matters instead
// is what other firings have created by then: given (A is p) => (A q N) and
// (A is p) => (A q m), the expression `a is p` produces only `a q m` when
// the second rule fires first, and `a q ??` beside it if the first rule
// fires first. Each outcome maps into the other, meaning the model is
// uniquely determined only up to homomorphism, not up to renaming of the
// nodes that were generated.
bool Reasoning::consequences_already_exist(
    const Variables&     condition_bindings,
    const adjacency_set& deductions,
    Node                 parent,
    const int            depth)
{
    std::vector<Node> consequences;
    for (Node deduction : deductions)
    {
        // A rule deduction contributes no fresh variable (deduce() exempts
        // it) and carries its own exact duplicate check in rebuild_rule, so
        // it neither satisfies this guard nor may it block the others. Nor
        // does a deduction that deduce() skips for want of exactly one
        // predicate: it adds nothing that a witness could stand for.
        if (asserts_fact(deduction)) consequences.push_back(deduction);
    }

    // The fresh variables, as deduce() finds them: located beyond any
    // container, and not bound by the conditions.
    std::unordered_set<Node> fresh;
    for (const Node c : consequences)
    {
        std::vector<Node> history;
        collect_variables(this, c, fresh, depth, history);
    }
    for (auto it = fresh.begin(); it != fresh.end();)
        it = condition_bindings.count(*it) != 0 ? fresh.erase(it) : std::next(it);

    // Whether a consequence that is ground under `bindings` holds, or nullopt
    // when a fresh variable it meets is free.
    const auto ground_holds = [&](const Node deduction, const Variables& bindings) -> std::optional<bool>
    {
        if (!fresh.empty())
        {
            const Recipe recipe{deduction, &bindings, RecipeMode::Firing, parent, nullptr};
            for (const Node v : statement_variables(this, recipe, depth))
            {
                if (fresh.count(v) == 0) continue;
                const auto it = bindings.find(v);
                if (it == bindings.end() || Zelph::is_var(it->second)) return std::nullopt;
            }
        }

        const auto parts = predict_consequence(deduction, bindings, depth, parent);
        if (!parts) return std::nullopt;

        // A node that only a rule mentions, or one considered incorrect,
        // does not constitute a fact -- Unification passes over both alike.
        const Answer answer = check_fact(parts->source, parts->relation, parts->targets);
        return answer.is_known() && !answer.is_wrong()
            && (!is_rule_pattern(answer.relation()) || instance_is_rule_text(this, answer.relation(), deduction));
    };

    // Set once a consequence was matched through a stand-in for any given
    // term.
    bool                    confirm = false;
    const std::vector<Node> all     = consequences;

    const auto join = [&](const auto& self, std::vector<Node> rest, const Variables& bindings) -> bool
    {
        if (rest.empty())
        {
            if (!confirm) return true;
            return std::all_of(all.begin(), all.end(), [&](const Node c)
                               { return ground_holds(c, bindings).value_or(false); });
        }

        // A ground consequence first: a single lookup occurs, and it
        // establishes no binding, thus when it is missing, no choice of
        // witness for the others can provide assistance.
        for (auto it = rest.begin(); it != rest.end(); ++it)
        {
            const std::optional<bool> holds = ground_holds(*it, bindings);
            if (!holds) continue;
            if (!*holds) return false;
            rest.erase(it);
            return self(self, std::move(rest), bindings);
        }

        // Unification does not bind any variable within a container: a
        // consequence whose free variables are all located inside containers
        // fails to match anything until another consequence has bound them.
        // The first consequence possessing a free variable outside all
        // containers thus takes precedence. When evaluated in the order of
        // node IDs instead, `(X p Y) => (X likes @{(W s Y)}) (W in @{k})`
        // only discovered its witness when the membership came first, and
        // otherwise made a new one on each pass.
        auto lead = rest.begin();
        if (rest.size() > 1)
        {
            const auto binds = [&](const Node c)
            {
                std::unordered_set<Node> vars;
                std::vector<Node>        history;
                collect_variables(this, c, vars, depth, history);
                return std::any_of(vars.begin(), vars.end(), [&](const Node v)
                                   {
                                       const auto it = bindings.find(v);
                                       return it == bindings.end() || Zelph::is_var(it->second); });
            };
            if (const auto it = std::find_if(rest.begin(), rest.end(), binds); it != rest.end()) lead = it;
        }
        const Node consequence = *lead;
        rest.erase(lead);

        StandIns stand_ins;
        firing_stand_ins(this, consequence, parent, bindings, stand_ins);
        if (std::any_of(stand_ins.begin(), stand_ins.end(), [](const auto& entry)
                        { return entry.second.term == 0; }))
            confirm = true;

        Unification u(this, consequence, parent, std::make_shared<Variables>(bindings), std::make_shared<Variables>(), /*pool*/ nullptr, depth + 1, &_prof, 0, 0, VariableReading::Instance, stand_ins.empty() ? nullptr : &stand_ins);

        bool found = false;
        while (const std::shared_ptr<Variables> match = u.Next())
        {
            Variables joined = bindings;
            for (const auto& [k, v] : *match)
                joined[k] = v;

            if (self(self, rest, joined))
            {
                found = true;
                break;
            }
        }
        u.wait_for_completion();
        return found;
    };

    return join(join, std::move(consequences), condition_bindings);
}
