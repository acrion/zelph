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

#include <algorithm>
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

    // A part of a pattern as a binding fills it: the value of a bound
    // variable; otherwise, the part itself.
    Node bound_value(const Variables& variables, const Node part)
    {
        return Zelph::is_var(part) ? zelph::string::get(variables, part, part) : part;
    }

    // Whether a consequence holds a rule beneath its top level: a `=>` fact
    // present at any depth of its subject, relation, or objects, or as a
    // member of a set constant or of a collection of the rule's text. A firing
    // writes such a rule as a mention, and deduce() marks the ground parts
    // that firing creates for it. The walk terminates at a node preserved
    // unchanged by the firing, either at an atom or a value. It reads the
    // structure from the genuine-structure store and the adjacency, which the
    // profiler does not include in its count, and reconstructs a fact that has
    // no entry in the store, which the profiler does include. A fact is
    // missing the entry when the stores are off, following a load, or after a
    // removal that disarms the store, so on a loaded network the
    // reconstruction is the normal path: a walk that skipped such a fact would
    // let every firing there claim the parts of the rule it mentions. deduce()
    // asks once per consequence and run (Reasoning::_consequence_text), so a
    // consequence lacking a nested rule, which applies to every consequence in
    // the standard library, costs one lookup per firing.
    bool holds_nested_rule(const Zelph* const z, const Node source, const Node relation, const adjacency_set& targets)
    {
        std::vector<Node> pending(targets.begin(), targets.end());
        pending.push_back(source);
        pending.push_back(relation);

        std::unordered_set<Node> seen;
        while (!pending.empty())
        {
            const Node n = pending.back();
            pending.pop_back();
            if (n == 0 || Zelph::is_var(n) || !seen.insert(n).second) continue;
            if (!Zelph::is_hash(n) && !z->is_rule_template(n)) continue;

            if (Zelph::is_hash(n))
            {
                FactStructurePtr genuine;
                if (z->try_get_genuine_structure(n, genuine) && genuine && !genuine->empty())
                {
                    const FactStructure& fs = genuine->front();
                    if (fs.predicate == z->core.Causes) return true;
                    pending.push_back(fs.subject);
                    pending.push_back(fs.predicate);
                    pending.insert(pending.end(), fs.objects.begin(), fs.objects.end());
                    continue;
                }
            }

            const std::vector<Node> members = conjunction_members(z, n);
            if (members.empty() && Zelph::is_hash(n))
            {
                const FactStructure fs = get_preferred_structure(z, n, 1);
                if (fs.subject == 0) continue;
                if (fs.predicate == z->core.Causes) return true;
                pending.push_back(fs.subject);
                pending.push_back(fs.predicate);
                pending.insert(pending.end(), fs.objects.begin(), fs.objects.end());
                continue;
            }
            pending.insert(pending.end(), members.begin(), members.end());
        }
        return false;
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
    return instantiate_fact(this, pattern, variables, depth, history, recipe, Place{Position::Value, true, 0});
}

namespace
{
    // The cluster in which a construction performs its builds
    // (ConstructionScratch).
    const char* const kConstructionScratch = "__construction";

    // The cluster where a firing writes a consequence that nests a part
    // (Reasoning::deduce).
    const char* const kFiringScratch = "__firing";

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
    //
    // A firing that writes a consequence nesting a part runs in a scratch
    // cluster of its own, for the same reason, without the template scope:
    // the instantiation builds the nested parts prior to the fact that holds
    // them, and the claim of the instance revokes pattern marks and applies
    // refutations, so a refusal at any such stage left what preceded it as
    // data that no derivation line names, and other rules of the same run
    // fired on it. A drop takes all of it back (Network::note_unmarked,
    // Network::note_refutation), just as it does for a typed statement that
    // is refused (zelph/claim-text).
    class ConstructionScratch
    {
    public:
        explicit ConstructionScratch(Zelph& z, const char* const name = kConstructionScratch, const bool writes_rule = true)
            : _z(z)
            , _name(name)
            , _journal(std::string_view(name) == kFiringScratch)
            , _previous(_journal ? std::string() : z.active_cluster_name())
        {
            // The scratch from a firing represents the engine's journal
            // associated with that firing (Zelph::open_firing_journal), which
            // itself keeps a record of the cluster active at the time.
            if (_journal ? !_z.open_firing_journal(_name) : _previous == _name)
                throw std::logic_error(std::string("Reasoning: the cluster \"") + _name + "\" that a construction or a firing runs in is already active");
            if (_journal) return;
            if (writes_rule) _scope.emplace(z);
            _z.set_active_cluster(_name);
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
            if (_journal)
                _z.keep_firing_journal(_name);
            else
            {
                restore();
                _z.merge_cluster(_name, _previous);
            }
            _open = false;
        }

        // drop_scratch_cluster eliminates every node with the invalidation a
        // fact() of its shape performs, ensuring the fact stores remain armed:
        // nothing outside the construction can reference what it created yet
        // -- fact() operates without concurrent caller, and constructions do
        // not interleave.
        //
        // `witnesses` are the fresh nodes of a firing, created prior to its
        // consequences: every one that nothing outside the scratch holds goes
        // with the drop, whereas one that a consequence written earlier holds
        // remains. A firing that is refused again on every run otherwise left
        // a new witness behind each time.
        void drop(const std::vector<Node>& witnesses = {})
        {
            if (!witnesses.empty())
            {
                const std::vector<Node>        recorded = _journal ? _z.firing_journal_nodes() : _z.cluster_nodes(_name);
                const std::unordered_set<Node> ours(recorded.begin(), recorded.end());
                std::vector<Node>              taken;
                for (const Node w : witnesses)
                {
                    const adjacency_set left  = _z.get_left(w);
                    const adjacency_set right = _z.get_right(w);
                    if (std::all_of(left.begin(), left.end(), [&ours](const Node n)
                                    { return ours.count(n) != 0; })
                        && std::all_of(right.begin(), right.end(), [&ours](const Node n)
                                       { return ours.count(n) != 0; }))
                        taken.push_back(w);
                }
                if (!taken.empty())
                {
                    if (_journal)
                        _z.move_into_firing_journal(taken);
                    else
                        _z.move_into_cluster(_name, taken);
                }
            }
            restore();
            _open = false;
            _z.drop_scratch_cluster(_name);
        }

    private:
        // The journal assumes the identity of the named cluster that
        // drop_scratch_cluster takes.
        void restore() const
        {
            if (_journal)
                _z.close_firing_journal();
            else if (_previous.empty())
                _z.deactivate_cluster();
            else
                _z.set_active_cluster(_previous);
        }

        const Zelph&                        _z;
        const char* const                   _name;
        const bool                          _journal;
        const std::string                   _previous;
        std::optional<Zelph::TemplateScope> _scope;
        bool                                _open{true};
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
        && relation_types_of(deduction).size() == 1;
}

const Reasoning::ConsequenceText& Reasoning::consequence_text(const Node deduction, const Node source, const Node relation, const adjacency_set& targets, const Node parent, const int depth)
{
    if (const auto it = _consequence_text.find(deduction); it != _consequence_text.end()) return it->second;

    ConsequenceText text;
    text.mentions_rule = source != 0 && relation != 0 && holds_nested_rule(this, source, relation, targets);

    // A flat consequence (predicate_never_an_object): the predicate is an atom
    // that a firing writes exactly as it stands, and likewise each object that
    // is not a variable.
    if (relation != 0 && !Zelph::is_var(relation) && !Zelph::is_hash(relation) && !is_rule_template(relation) && relation != core.PartOf && !targets.empty())
    {
        std::vector<Node> variables;
        bool              flat = true;
        for (const Node t : targets)
        {
            if (Zelph::is_var(t))
                variables.push_back(t);
            else if (t == relation || Zelph::is_hash(t) || is_rule_template(t))
                flat = false;
        }
        if (flat)
        {
            text.flat_relation  = relation;
            text.flat_parent    = parent;
            text.flat_variables = std::move(variables);
        }
    }

    const auto claimable = [this, &text](const Node n)
    { return n != 0 && !Zelph::is_var(n) && (Zelph::is_hash(n) || is_rule_template(n) || enters_kept_list(text, n)); };
    text.claims_nested = claimable(source) || claimable(relation) || std::any_of(targets.begin(), targets.end(), claimable);

    // Only a consequence that mentions a rule may retain one of its
    // variables, and none of the consequences from the standard library
    // does: the instance walk, which the profiler tracks, remains off the
    // math stack.
    if (text.mentions_rule)
    {
        static const Variables unbound;
        const Recipe           recipe{deduction, &unbound, RecipeMode::Firing, parent, nullptr};
        text.keeps_rule_variable = keeps_rule_variable(this, recipe, depth, text.value_rule_variables);
    }

    // Whether the consequence writes a refuted marking within a statement,
    // outside a `=>` it mentions: the firing claims that marking, which
    // refutes its fact (refuted_consequence). Read once per rule, and
    // exclusively in a graph that includes the `refuted` node at all.
    if (_refuted_pred != 0)
    {
        std::unordered_set<Node> seen;
        std::vector<Node>        pending(targets.begin(), targets.end());
        pending.push_back(source);
        pending.push_back(relation);
        while (!pending.empty() && !text.nests_marking)
        {
            const Node n = pending.back();
            pending.pop_back();
            if (n == 0 || Zelph::is_var(n) || !seen.insert(n).second) continue;

            // A marking may function as a member within a container that the
            // consequence writes, a set constant, a collection, or a comma
            // list, just as it might in a statement.
            const FactStructure fs = Zelph::is_hash(n) ? get_preferred_structure(this, n, 1) : FactStructure{};
            if (fs.subject == 0)
            {
                if (Zelph::is_hash(n) || is_rule_template(n) || check_fact(n, core.IsA, {core.Conjunction}).is_known())
                {
                    const std::vector<Node> members = conjunction_members(this, n);
                    pending.insert(pending.end(), members.begin(), members.end());
                }
                continue;
            }
            if (fs.predicate == core.Causes) continue;

            // A binding may provide the `~` and the `refuted` associated with
            // a marking (refuted_consequence), thus a variable located in
            // either position counts.
            const auto may_be = [](const Node part, const Node node)
            { return part == node || Zelph::is_var(part); };
            text.nests_marking = fs.objects.size() == 1 && may_be(fs.predicate, core.IsA) && may_be(*fs.objects.begin(), _refuted_pred);
            pending.push_back(fs.subject);
            pending.push_back(fs.predicate);
            pending.insert(pending.end(), fs.objects.begin(), fs.objects.end());
        }
    }

    return _consequence_text.emplace(deduction, std::move(text)).first->second;
}

bool Reasoning::instance_keeps_variable(const Node deduction, const ConsequenceText& text, const Variables& binding, const std::unordered_set<Node>& fresh, const Node parent, const int depth) const
{
    if (!text.keeps_rule_variable && text.value_rule_variables.empty()) return false;

    // The text is read without a binding, meaning a relation that is a
    // variable counts both as one that generates values for its objects
    // and as a membership, which writes into those objects instead
    // (walk_instantiation). A binding that makes a variable `in` causes the
    // statement to be read again: when it makes the relation `in`, the
    // firing writes into the rule's collection, where the term excludes a
    // member holding a variable (bucket_term in reasoning.cpp), and builds
    // no value.
    bool                            keeps = text.keeps_rule_variable;
    const std::unordered_set<Node>* vars  = &text.value_rule_variables;
    std::unordered_set<Node>        in_values;
    if (std::any_of(binding.begin(), binding.end(), [this](const auto& b)
                    { return b.second == core.PartOf; }))
    {
        const Recipe recipe{deduction, &binding, RecipeMode::Firing, parent, nullptr};
        keeps = keeps_rule_variable(this, recipe, depth, in_values);
        vars  = &in_values;
    }
    if (keeps) return true;
    return std::any_of(vars->begin(), vars->end(), [&](const Node v)
                       {
                           if (fresh.count(v) != 0) return false;
                           const auto it = binding.find(v);
                           return it == binding.end() || Zelph::is_var(it->second); });
}

bool Reasoning::predicate_never_an_object(const ConsequenceText& text, const Node relation, const Variables& binding, const Node parent) const
{
    // predict_consequence keeps an atom that is not a collection of the rule's
    // text where it stands, and assigns a value to a variable, or returns the
    // variable itself when it remains unbound: at a place that is not written
    // into, a firing reads no value as a data term (stands_for_data_term). A
    // value that constitutes a collection of the rule's text is still passed
    // on to the prediction regardless, just as any value that serves as the
    // predicate. Whatever the graph has introduced in addition to the
    // consequence since the text was read -- a fact that uses the consequence
    // as its predicate is interpreted as one additional object -- predicts as
    // a statement, never as the atom that functions as the predicate.
    if (text.flat_relation == 0 || text.flat_relation != relation || text.flat_parent != parent) return false;
    for (const Node v : text.flat_variables)
    {
        const auto it = binding.find(v);
        if (it == binding.end()) continue;
        const Node value = it->second;
        if (value == relation || (!Zelph::is_hash(value) && is_rule_template(value))) return false;
    }
    return true;
}

const Reasoning::ConsequenceText& Reasoning::consequence_text(const Node deduction, const Node parent, const int depth)
{
    if (const auto it = _consequence_text.find(deduction); it != _consequence_text.end()) return it->second;

    const adjacency_set relations = relation_types_of(deduction);
    adjacency_set       targets;
    const Node          source = parse_fact(deduction, targets, parent);
    return consequence_text(deduction, source, relations.size() == 1 ? *relations.begin() : Node{0}, targets, parent, depth);
}

bool Reasoning::consequence_writes(const Node deduction, const Node source, const Node relation, const adjacency_set& targets, const Node node)
{
    ConsequenceText& text = _consequence_text.at(deduction);
    if (!text.written)
    {
        std::unordered_set<Node> written;
        std::vector<Node>        pending(targets.begin(), targets.end());
        pending.push_back(source);
        pending.push_back(relation);
        while (!pending.empty())
        {
            const Node n = pending.back();
            pending.pop_back();
            if (n == 0 || Zelph::is_var(n) || !written.insert(n).second) continue;

            if (Zelph::is_hash(n))
            {
                const FactStructure fs = get_preferred_structure(this, n, 1);
                if (fs.subject != 0)
                {
                    if (fs.predicate == core.Causes) continue;
                    pending.push_back(fs.subject);
                    pending.push_back(fs.predicate);
                    pending.insert(pending.end(), fs.objects.begin(), fs.objects.end());
                    continue;
                }
            }
            else if (!is_rule_template(n) && !enters_kept_list(text, n))
            {
                continue;
            }

            const std::vector<Node> members = conjunction_members(this, n);
            pending.insert(pending.end(), members.begin(), members.end());
        }
        text.written = std::move(written);
    }
    return text.written->count(node) != 0;
}

bool Reasoning::enters_kept_list(const ConsequenceText& text, const Node node) const
{
    if (node == 0 || Zelph::is_var(node) || Zelph::is_hash(node) || is_rule_template(node)) return false;
    if (const auto it = text.kept_lists.find(node); it != text.kept_lists.end()) return it->second;

    bool enters = check_fact(node, core.IsA, {core.Conjunction}).is_known();
    if (enters)
    {
        for (const Node rel : get_right(node))
        {
            if (parse_relation(rel) != core.Causes) continue;
            adjacency_set consequences;
            if (parse_fact(rel, consequences, 0) == node)
            {
                enters = false;
                break;
            }
        }
    }
    text.kept_lists.emplace(node, enters);
    return enters;
}

bool Reasoning::consequence_holds_opaque(const Node deduction, const Node source, const Node relation, const adjacency_set& targets, const Node rel, const Variables& binding)
{
    if (!has_opaque_predicates()) return false;
    if (is_opaque_predicate(rel)) return true;

    ConsequenceText& text = _consequence_text.at(deduction);
    if (!text.nested_predicates)
    {
        std::vector<Node>        predicates;
        std::unordered_set<Node> seen;
        std::vector<Node>        pending(targets.begin(), targets.end());
        pending.push_back(source);
        pending.push_back(relation);
        while (!pending.empty())
        {
            const Node n = pending.back();
            pending.pop_back();
            if (n == 0 || Zelph::is_var(n) || !seen.insert(n).second) continue;

            if (Zelph::is_hash(n))
            {
                const FactStructure fs = get_preferred_structure(this, n, 1);
                if (fs.subject != 0)
                {
                    if (fs.predicate == core.Causes) continue;
                    predicates.push_back(fs.predicate);
                    pending.push_back(fs.subject);
                    pending.push_back(fs.predicate);
                    pending.insert(pending.end(), fs.objects.begin(), fs.objects.end());
                    continue;
                }
            }
            else if (!is_rule_template(n))
            {
                continue;
            }

            const std::vector<Node> members = conjunction_members(this, n);
            pending.insert(pending.end(), members.begin(), members.end());
        }
        text.nested_predicates = std::move(predicates);
    }

    for (const Node p : *text.nested_predicates)
        if (is_opaque_predicate(Zelph::is_var(p) ? string::get(binding, p, Node{0}) : p)) return true;
    return false;
}

std::optional<Reasoning::ConsequenceParts> Reasoning::predict_consequence(const Node deduction, const Variables& variables, const int depth, const Node parent) const
{
    // Subject, predicate, and objects are read as deduce() interprets
    // them, according to the recipe of its firing. The deduction seeds the
    // history, as there, so that it is not confused with the subject of
    // its own subject.
    const adjacency_set relations = relation_types_of(deduction);
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

Reasoning::RefutedConsequence Reasoning::refuted_consequence(const Node deduction, const Node relation, const Node var_relation, const adjacency_set& var_targets, const Variables& variables, const int depth, const Node parent, const bool nests_marking, const bool opaque, Node& claimed, LookedUpFacts& looked_up) const
{
    claimed = 0;

    // Inquired about the pattern first: a consequence that is no marking
    // costs one comparison within a graph that refutes nothing.
    //
    // The `refuted` associated with a marking can be the value of a binding:
    // `(t p b) ~ Y` where Y is bound to `refuted` represents an identical
    // marking.
    const bool marking     = relation == core.IsA && var_targets.size() == 1 && _refuted_pred != 0 && bound_value(variables, *var_targets.begin()) == _refuted_pred;
    const bool any_refuted = has_refuted_facts();
    if (!marking && !any_refuted && !nests_marking) return RefutedConsequence::None;

    const auto parts = predict_consequence(deduction, variables, depth, parent);
    if (!parts) return RefutedConsequence::None;

    if (marking)
    {
        // A derived refutation of a fact that someone claimed constitutes
        // a contradiction between the two, with neither prevailing without
        // notice: the marking used to take the fact out of every query, NAF
        // then read the user's own assertion as absent, and whatever had been
        // derived from it remained intact. Nothing is written, and F keeps
        // answering.
        const Node f = parts->source;
        if (exists(f) && predicate_of(f) != 0 && !is_set_constant(f) && !var_in_closure(f))
        {
            const Answer answer = check_fact(f);
            if (answer.is_wrong()) return RefutedConsequence::Known;
            if (is_asserted_fact(f))
            {
                claimed = f;
                return RefutedConsequence::Contradiction;
            }
        }
    }

    if (!any_refuted && !nests_marking) return RefutedConsequence::None;

    // The fact a marking names is not claimed by it, and the parts of a
    // consequence whose predicate is opaque are regarded as mentions.
    const NestedPlace parts_at = marking                                 ? NestedPlace::Marked
                               : opaque && is_opaque_predicate(relation) ? NestedPlace::Mentioned
                                                                         : NestedPlace::Claimed;
    adjacency_set     unused;
    const Node        var_source = parse_fact(deduction, unused, parent);
    const NestedPlace others_at  = parts_at == NestedPlace::Mentioned ? parts_at : NestedPlace::Claimed;
    std::vector<Node> history{deduction};
    if (nests_refuted_fact(deduction, var_source, variables, depth, parent, history, nests_marking, opaque, parts_at, claimed, looked_up)) return RefutedConsequence::Contradiction;

    // A composite predicate is a statement that the consequence claims, just
    // as it claims its subject and its objects: after "¬(t p b)", the rule
    // "(X go Y) => (k (X p b) c)" ended its run with "known to be wrong",
    // while the typed "k (t p b) c" is refused as known to be wrong.
    history = {deduction};
    if (nests_refuted_fact(deduction, var_relation, variables, depth, parent, history, nests_marking, opaque, others_at, claimed, looked_up)) return RefutedConsequence::Contradiction;
    for (const Node var_t : var_targets)
    {
        history = {deduction};
        if (nests_refuted_fact(deduction, var_t, variables, depth, parent, history, nests_marking, opaque, others_at, claimed, looked_up, relation == core.PartOf)) return RefutedConsequence::Contradiction;
    }
    return RefutedConsequence::None;
}

bool Reasoning::nests_refuted_fact(const Node deduction, const Node pattern, const Variables& variables, const int depth, const Node parent, std::vector<Node>& history, const bool markings, const bool opaque, const NestedPlace place, Node& claimed, LookedUpFacts& looked_up, const bool into) const
{
    if (Zelph::Impl::is_var(pattern)) return false;
    if (std::find(history.begin(), history.end(), pattern) != history.end()) return false;

    const FactStructure fs = get_preferred_structure(this, pattern, depth);
    if (fs.subject == 0)
    {
        // An atom, or a container. The members of a container that the
        // firing writes are walked as the instantiation meets them, with
        // the container on the path, as the instantiation holds it there: a
        // statement claims what a set or a comma list it writes holds, just
        // as the typed statement does. After "t p b", the rule
        // "(X go Y) => (k says {((X p b) ~ refuted)})" refuted the claimed
        // `t p b` without a word, and after "¬(t p b)", the rule
        // "(X go Y) => (k says {(X p b)})" ended its run with "known to be
        // wrong", while both typed statements are refused. A member
        // standing as the rule wrote it is read without the binding.
        std::unordered_set<Node> members;
        bool                     rebuilt = false;
        if (into) return false;
        if (!container_members_met(this, pattern, Recipe{deduction, &variables, RecipeMode::Firing, parent, nullptr}, members, rebuilt)) return false;
        // A comma list that the claim does not enter contains nothing that
        // the statement claims (enters_kept_list).
        if (!rebuilt)
        {
            const auto text = _consequence_text.find(deduction);
            if (text == _consequence_text.end() || !enters_kept_list(text->second, pattern)) return false;
        }

        static const Variables as_written;
        const NestedPlace      members_at = place == NestedPlace::Mentioned ? place : NestedPlace::Claimed;
        history.push_back(pattern);
        bool found = false;
        for (const Node m : members)
            found = found || nests_refuted_fact(deduction, m, rebuilt ? variables : as_written, depth, parent, history, markings, opaque, members_at, claimed, looked_up);
        history.pop_back();
        return found;
    }

    // The predicate and the object as the firing writes them: a binding may
    // provide the `~` and the `refuted` associated with a marking.
    const Node predicate = bound_value(variables, fs.predicate);
    const bool marking   = predicate == core.IsA && fs.objects.size() == 1 && _refuted_pred != 0 && bound_value(variables, *fs.objects.begin()) == _refuted_pred;

    // A rule the consequence mentions is written as a mention, which claims
    // none of its components.
    //
    // So is an argument of an opaque predicate. No contradiction arises from
    // anything beneath a mentioned rule or an opaque argument, nor does any
    // marking there serve to refute, yet a refuted fact located there is still
    // looked up, since the instantiation would construct it at probability 1:
    // following "¬(bob sleeps nil)", the rule
    // "(X ~ person) => (X says ((X sleeps nil) => (bob ~ awake)))" terminated
    // its run due to "this fact is known to be wrong". The `=>` node itself is
    // not checked.
    const bool        rule     = predicate == core.Causes;
    const bool        mentions = place == NestedPlace::Mentioned || rule || (opaque && is_opaque_predicate(predicate));
    const NestedPlace parts_at = mentions ? NestedPlace::Mentioned : NestedPlace::Claimed;

    history.push_back(pattern);
    bool found = nests_refuted_fact(deduction, fs.subject, variables, depth, parent, history, markings, opaque, marking && !mentions ? NestedPlace::Marked : parts_at, claimed, looked_up)
              || nests_refuted_fact(deduction, fs.predicate, variables, depth, parent, history, markings, opaque, parts_at, claimed, looked_up);
    for (const Node o : fs.objects)
        found = found || nests_refuted_fact(deduction, o, variables, depth, parent, history, markings, opaque, parts_at, claimed, looked_up, predicate == core.PartOf);
    history.pop_back();
    if (found || rule) return found;

    // Predicted as predict_consequence predicts the consequence itself.
    const Recipe recipe{deduction, &variables, RecipeMode::Firing, parent, nullptr};

    // A marking the firing writes within a statement refutes its fact,
    // just as the derived marking does independently: following "a p b",
    // the rule "(X go Y) => (k says ((a p b) ~ refuted))" left `a p b`
    // un-claimed at its first firing and reported a contradiction at each
    // subsequent one.
    if (markings && marking && place != NestedPlace::Mentioned)
    {
        std::vector<Node> own{deduction};
        const Node        f = ground_instance(this, fs.subject, variables, depth, own, &recipe, Place{}, true);
        if (f != 0 && exists(f) && predicate_of(f) != 0 && !is_set_constant(f) && !var_in_closure(f) && check_fact(f).is_correct() && is_asserted_fact(f))
        {
            claimed = f;
            return true;
        }
    }

    std::vector<Node> own{deduction};
    const Node        instance = ground_instance(this, pattern, variables, depth, own, &recipe, Place{}, true);
    if (instance == 0 || !check_fact(instance).is_wrong()) return false;
    if (place == NestedPlace::Claimed) return true;

    // A refuted fact that the statement does not claim: the fact that a
    // marking names, which the marking merely reiterates, or a mention. After
    // "¬(t p b)", the rule "(X go Y) => (k says ((X p b) ~ refuted))" reported
    // a contradiction in each run, whereas the identical statement typed is
    // accepted.
    looked_up.push_back({history, instance});
    return false;
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

    // A consequence that would keep a variable from a rule it mentions is
    // not derived (below), thus no witness is made for its variables
    // either: such a witness would stand in no fact. This is checked only
    // for a rule that has fresh variables at all, before a node is created
    // for one. A rule contained within a value that a consequence
    // constructs keeps a fresh variable for which no derived consequence
    // makes a witness, hence the check is repeated as long as such a rule
    // exists and a witness drops out.
    if (!fresh_vars.empty())
    {
        std::lock_guard<std::mutex> lock(_mtx_network);
        for (bool again = true; again;)
        {
            std::unordered_set<Node> derived_vars;
            bool                     in_values = false;
            for (const Node deduction : ctx.rule_deductions)
            {
                if (deduction == core.Contradiction || deduction_is_rule(deduction)) continue;
                const ConsequenceText& text = consequence_text(deduction, parent, depth);
                if (instance_keeps_variable(deduction, text, variables, fresh_vars, parent, depth)) continue;
                in_values = in_values || !text.value_rule_variables.empty();
                std::vector<Node> history;
                collect_variables(this, deduction, derived_vars, depth, history);
            }
            const auto dropped = std::erase_if(fresh_vars, [&](const Node v)
                                               { return derived_vars.count(v) == 0; });
            again              = dropped != 0 && in_values;
        }
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

        if (consequences_already_exist(variables, fresh_vars, ctx.rule_deductions, parent, depth))
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

    // A fresh witness that no consequence wrote into stands in no fact, and
    // it goes when the firing ends, so a firing that writes nothing leaves
    // nothing. It was kept, and the next run made a new one, since no node
    // could ever satisfy the consequence: `(X p Y) => (Z @{Z} @{Z})`, whose
    // predicate is also its object, added a node on each run, and so did a
    // rule with a contradiction within its consequences, which ends the
    // firing before the others are written. A witness that a consequence
    // holds stays, even if another consequence of the same firing was
    // skipped.
    const auto release_unheld = [&]
    {
        if (fresh_vars.empty()) return;
        std::lock_guard<std::mutex> lock_network(_mtx_network);
        std::vector<Node>           unheld;
        for (const Node var : fresh_vars)
            if (const Node witness = augmented[var]; left_count(witness) == 0 && right_count(witness) == 0) unheld.push_back(witness);
        if (!unheld.empty()) ConstructionScratch(*this, kFiringScratch, false).drop(unheld);
    };

    // The consequences of this firing that were written or found to hold.
    // When each one was, each witness stands in a fact, and release_unheld
    // has nothing to find: a fresh variable is one that a consequence derived
    // here meets outside every container it keeps, and the instance that
    // consequence writes or finds is ground, so the variable's witness stands
    // within it.
    std::size_t held = 0;

    // --- Process Deductions ---
    for (const Node deduction : ctx.rule_deductions)
    {
        if (deduction == core.Contradiction)
        {
            release_unheld();
            throw contradiction_error(ctx.current_condition, augmented, parent, ctx.rule, core.Contradiction);
        }

        adjacency_set relations = relation_types_of(deduction);

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
        Node          d          = 0;
        bool          wrong      = false;
        bool          created    = false;
        bool          holds      = false; // the instance was there already, as a fact that holds
        Node          conclusion = 0;
        Node          claimed    = 0; // the claimed fact a derived marking met
        std::string   refusal;
        // A derived RULE is not a statement ABOUT anything, so the focus
        // filter has no subject to match it against -- and suppressing the
        // one deduction that changes what the engine will do next is the
        // wrong default. It prints whenever deductions print at all.
        const bool is_rule = rel == core.Causes;

        if (is_rule)
        {
            std::lock_guard<std::mutex> lock_network(_mtx_network);

            // fact() refuses a statement within the rule that a construction
            // builds -- for instance, a set constant extended in its
            // consequence -- just as it refuses one that a firing writes, and
            // the refusal is reported identically (the construction's
            // scratch has taken back what it had built). A scratch that
            // cannot open does not constitute a refusal and stops the run
            // (ConstructionScratch).
            try
            {
                d = rebuild_rule(deduction, augmented, depth, parent, created);
            }
            catch (const std::runtime_error& ex)
            {
                refusal                           = ex.what();
                constexpr std::string_view prefix = "fact(): ";
                if (refusal.starts_with(prefix)) refusal.erase(0, prefix.size());
                wrong   = true;
                d       = 0;
                created = false;
            }

            if (should_log(depth))
                log(depth, "deduce", "Derived rule: " + (d ? format(d) : "NULL") + (created ? " (new)" : " (already present)") + " (from pattern " + format(deduction) + ")");
        }
        else
        {
            std::lock_guard<std::mutex> lock_network(_mtx_network);

            // One instantiation of this consequence: a collection of the
            // rule's own text, contained within it, becomes its term under
            // this binding. A consequence that holds a rule writes a mention
            // of it, and a mention claims none of the rule's parts, as the
            // parser's does (zelph/rule-text): what the instantiation creates
            // is collected, and the ground parts of each rule within it are
            // marked once the instance comes into existence. The parser
            // collects what the mention itself created, the firing gathers
            // what the whole consequence created, and the claim of the
            // instance (below) revokes a mark where the part also stands
            // outside the mention: after `k p m`, the rule
            // `(X p Y) => ((X r Y) about ((X r Y) => (X q Y)))` answers
            // `k r m` and not `k q m`, just as the statement
            // `(k r m) about ((k r m) => (k q m))` does.
            const ConsequenceText& text = consequence_text(deduction, var_source, *relations.begin(), var_targets, parent, depth);

            // A consequence whose instance would keep a variable from a rule
            // it mentions is not derived: no write, no claim, no line, no
            // witness. That instance is rule text, which no query answers,
            // and where the firing would have substituted into the mentioned
            // rule's conditions, it constitutes half of a rule no one
            // authored. The determination comes before the instantiation,
            // which builds the nested `=>` prior to the fact that holds it: a
            // `=>` belonging to nothing is a rule in force.
            if (instance_keeps_variable(deduction, text, augmented, {}, parent, depth))
            {
                if (should_log(depth))
                    log(depth, "deduce", "SKIP: " + format(deduction) + " would keep a variable of a rule it mentions");
                continue;
            }

            // For each firing, read the predicates that this binding
            // writes, located at the top of the consequence and nested
            // within it: an earlier firing from the same run might have
            // declared one, and a variable predicate is opaque only where
            // its value is. A declaration written within the scope
            // beneath would be neither checked nor recorded.
            const bool opaque = consequence_holds_opaque(deduction, var_source, *relations.begin(), var_targets, rel, augmented);

            // Before any element of the consequence is built, it has been
            // decided; see refuted_consequence.
            LookedUpFacts            looked_up;
            const RefutedConsequence refuted = refuted_consequence(deduction, rel, *relations.begin(), var_targets, augmented, depth, parent, text.nests_marking, opaque, claimed, looked_up);
            if (refuted == RefutedConsequence::Known)
            {
                if (should_log(depth))
                    log(depth, "deduce", "SKIP: " + format(deduction) + " marks a fact that is refuted already");
                continue;
            }
            wrong = refuted == RefutedConsequence::Contradiction;
            if (wrong) conclusion = claimed;

            // A refutation claims its marking, not its fact, and what the
            // firing creates of the fact is written as a mention, exactly as
            // zelph/refute writes it (Zelph::refute): built as data, it
            // answered once the marking had been pruned, and the next run then
            // met a claimed fact with the refutation it had derived again.
            const bool        marks = _refuted_pred != 0 && rel == core.IsA && var_targets.size() == 1 && *var_targets.begin() == _refuted_pred;
            std::vector<Node> mention_parts;
            std::vector<Node> built_values;
            const Recipe      recipe{deduction, &augmented, RecipeMode::Firing, parent, text.mentions_rule || opaque || marks || text.nests_marking ? &mention_parts : nullptr, &_filled_terms, text.claims_nested ? &built_values : nullptr, looked_up.empty() ? nullptr : &looked_up};

            // A marking within a rule the consequence mentions is that
            // rule's pattern, which becomes known only after the instance is
            // established and its parts are marked (below): the markings
            // written by this firing are held back until then, while those
            // that are claimed are applied.
            std::vector<Node>                  deferred_markings;
            std::optional<Zelph::PatternScope> mention_scope;
            if (text.mentions_rule || opaque) mention_scope.emplace(*this, &deferred_markings);

            // A consequence that is refused does not alter the graph
            // (ConstructionScratch). Only a consequence whose instantiation
            // is capable of constructing or modifying something before the
            // refusal runs in the scratch: one that nests a part, writes a
            // refutation or a mention, or holds a fresh variable. A flat
            // consequence builds nothing before its own fact, which fact()
            // refuses whole.
            std::optional<ConstructionScratch> scratch;
            const size_t                       filled_before = _filled_terms.size();
            if (text.claims_nested || text.mentions_rule || opaque || marks || text.nests_marking || !fresh_vars.empty())
                scratch.emplace(*this, kFiringScratch, false);

            // Seed history with the deduction node so that get_preferred_structure()
            // skips it as a parent and does not mistake it for the subject of var_source.
            std::vector<Node> history{deduction};
            // fact() refuses a statement nested in the consequence, such
            // as a set constant extended inside `z says (X in {c d})`, just
            // as it refuses one at the top, and the refusal is reported
            // identically while the run continues. It escaped the run
            // before, and with auto-run every later input line failed with
            // it.
            const auto instantiate = [&](const Node pattern, const Place& place) -> Node
            {
                try
                {
                    return instantiate_fact(this, pattern, augmented, depth, history, &recipe, place);
                }
                catch (const std::exception& ex)
                {
                    refusal                           = ex.what();
                    constexpr std::string_view prefix = "fact(): ";
                    if (refusal.starts_with(prefix)) refusal.erase(0, prefix.size());
                    wrong = true;
                    return 0;
                }
            };
            if (!wrong)
            {
                source = instantiate(var_source, Place{});

                if (should_log(depth))
                    log(depth, "deduce", "Instantiated source: " + (source ? format(source) : "NULL") + " (from pattern " + format(var_source) + ")");

                // The predicate is a pattern like the rest of the consequence. The
                // substitution above it covers a predicate that IS a variable and
                // nothing else, so a COMPOSITE one kept the rule's own variables:
                // `(X p Y) => (X (Y r s) c)` derived `a (Y r s) c`, a fact carrying
                // a template variable that no query can ever match.
                const Node var_rel = rel;
                history            = {deduction};
                if (!wrong) rel = instantiate(rel, Place{});

                if (rel != var_rel && should_log(depth))
                    log(depth, "deduce", "Instantiated relation: " + (rel ? format(rel) : "NULL") + " (from pattern " + format(var_rel) + ")");
            }

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
                    Node t  = instantiate(var_t, targets_at);

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
                    holds         = answer.is_known() && !answer.is_wrong();

                    // The node may exist only because some rule was written
                    // with this very statement as a ground pattern. Deriving
                    // it is a claim, so the mark goes and the deduction
                    // counts as new -- "(A is bad) => (alarm is on)" has to
                    // start answering the moment something IS bad.
                    bool was_pattern = false;
                    if (answer.is_known() && !answer.is_wrong())
                    {
                        // A claim may be refused, just as a creation might be
                        // (the declaration of an opaque predicate, see
                        // refuse_opacity_change), and is reported in the same
                        // way.
                        try
                        {
                            was_pattern = unmark_rule_pattern(answer.relation());
                        }
                        catch (const std::exception& ex)
                        {
                            // The claim of a rule's marking pattern of a claimed fact is refused
                            // (unmark_rule_pattern). The prediction above reports that marking
                            // before anything is built, regardless of whether the consequence
                            // writes `refuted` or a binding supplies it; should a claim still meet
                            // one, it constitutes the same contradiction, recorded in the same
                            // way, and the refusal does not escape the run, which it formerly ended
                            // together with every later line.
                            if (const Node f = refuted_marking_subject(answer.relation()); f != 0)
                            {
                                claimed    = f;
                                conclusion = f;
                            }
                            else
                            {
                                refusal                           = ex.what();
                                constexpr std::string_view prefix = "fact(): ";
                                if (refusal.starts_with(prefix)) refusal.erase(0, prefix.size());
                            }
                            wrong = true;
                        }
                    }

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

                        wrong      = true;
                        conclusion = answer.relation();
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

                            // The collection constructed by the firing for the
                            // mentioned rule is a value, as determined by
                            // container_plan, with its memberships
                            // constituting its data, which every subsequent
                            // firing derives again: solely the facts created
                            // by the firing are parts of the rule.
                            if (!mention_parts.empty())
                            {
                                std::erase_if(mention_parts, [](const Node n)
                                              { return !Zelph::is_hash(n); });
                                for (const Node part : mention_parts)
                                    if (predicate_of(part) == core.Causes) mark_rule_patterns(part, mention_parts);

                                // A consequence whose predicate is opaque
                                // writes its arguments as mentions, just
                                // as one that holds a rule writes the
                                // rule.
                                if (is_opaque_predicate(rel))
                                {
                                    mark_mention(source, mention_parts);
                                    for (const Node t : targets)
                                        mark_mention(t, mention_parts);
                                }

                                // An opaque fact that is nested within
                                // the consequence writes its own
                                // arguments as mentions, just as the
                                // parser encloses them at any level of
                                // depth.
                                if (opaque)
                                {
                                    for (const Node part : mention_parts)
                                    {
                                        if (part == d || !is_opaque_predicate(predicate_of(part))) continue;
                                        adjacency_set objects;
                                        mark_mention(parse_fact(part, objects), mention_parts);
                                        for (const Node o : objects)
                                            mark_mention(o, mention_parts);
                                    }
                                }

                                // The fact of a refutation that the firing
                                // claims, as a mention: the consequence's own
                                // marking, and one nested within it.
                                if (marks) mark_mention(source, mention_parts);
                                if (text.nests_marking)
                                {
                                    for (const Node part : mention_parts)
                                        if (const Node f = refuted_marking_subject(part); f != 0) mark_mention(f, mention_parts);
                                }
                            }

                            // A statement that is derived claims the facts
                            // nested in it outside a `=>` that its
                            // consequence wrote, just as a typed statement
                            // claims what it holds: following
                            // `(a p b) => (c q d)`, a firing that derives
                            // `(a p b) q z` causes `a p b` to become data,
                            // and the marks just set for the mentioned
                            // rule's parts are revoked again where the
                            // consequence also writes a part beside the
                            // mention. A value the binding supplied is
                            // neither claimed nor entered, unless the
                            // consequence also writes it as it stands: the
                            // firing read it, and a rule over rules that
                            // binds the parts of a rule asserts none of
                            // them. The walk enters a value the firing
                            // built under this binding, at a value
                            // position, and no other: a term the
                            // consequence names as a constant, or one it
                            // writes into, holds what every firing wrote,
                            // and entering it made each firing cost as much
                            // as all firings before it.
                            //
                            // A comma list the consequence writes is entered
                            // in the same manner as a typed statement enters
                            // its own: following `(a p b) => (c q d)`, the
                            // rule `(X go Y) => (k says (a p b, j r s))` left
                            // `a p b` a pattern, whereas its set form made it
                            // data.
                            if (text.claims_nested)
                                claim_nested(d, [&](const Node n, const bool into)
                                             {
                                                 if (std::any_of(augmented.begin(), augmented.end(), [n](const auto& binding)
                                                                 { return binding.second == n; })
                                                     && (!is_rule_pattern(n) || !consequence_writes(deduction, var_source, *relations.begin(), var_targets, n)))
                                                     return ClaimStep::Pass;
                                                 if (Zelph::is_hash(n)) return ClaimStep::Enter;
                                                 if (!into && std::find(built_values.begin(), built_values.end(), n) != built_values.end()) return ClaimStep::Enter;
                                                 if (!into && enters_kept_list(text, n)) return ClaimStep::Enter;
                                                 return ClaimStep::Claim; });

                            if (keeps_negation_records())
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

            mention_scope.reset();
            for (const Node marking : deferred_markings)
                if (const Node refuted = refuted_marking_subject(marking); refuted != 0 && exists(marking) && !is_rule_pattern(marking) && !var_in_closure(refuted))
                    apply_refutation(refuted);

            // A consequence that writes nothing leaves nothing either: deduce
            // skips its fact if the binding makes the predicate one of its
            // objects, or if the instance keeps a variable, and whatever the
            // instantiation built for it then goes with the scratch,
            // silently, just as it would with a refusal. Kept, a part that
            // the consequence nests remained as data that no line had
            // derived: following `a p a`, `(X p Y) => ((X r Y) Y X)` answered
            // `a r a`, which `.explain` labelled an axiom. The fresh
            // witnesses stay until the firing concludes, because a later
            // consequence might still write them.
            if (scratch && !wrong && (created || holds))
                scratch->keep();
            else if (scratch)
            {
                std::vector<Node> witnesses;
                if (wrong)
                    for (const Node var : fresh_vars)
                        witnesses.push_back(augmented[var]);
                try
                {
                    scratch->drop(witnesses);
                }
                catch (const std::exception& ex)
                {
                    // A drop that refuses before eliminating anything keeps
                    // what the firing wrote, as the firing did before the
                    // scratch existed.
                    merge_cluster(kFiringScratch, active_cluster_name());
                    if (should_log(depth))
                        log(depth, "deduce", "the drop of a refused consequence failed: " + std::string(ex.what()));
                }

                // A bucket term that the refused firing filled first in this
                // run has lost what the drop took, causing each term to be
                // repopulated by the next firing that writes into it
                // (bucket_term).
                if (_filled_terms.size() != filled_before) _filled_terms.clear();
            }
        } // _mtx_network released

        if (wrong)
            throw contradiction_error(ctx.current_condition, augmented, parent, ctx.rule, conclusion, refusal, claimed);

        if (created || holds) ++held;

        if (created)
        {
            // The description is prepared before acquiring the output lock.
            // Since the other threads need that lock for their respective
            // lines, it is held for the push alone.
            if (_capture_derived)
            {
                DerivedFact                 derivation = describe_derivation(d, ctx.current_condition, augmented, ctx.rule);
                std::lock_guard<std::mutex> lock_derived(_mtx_output);
                _derived_facts.push_back(std::move(derivation));
            }

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
            if (!do_print && !_export_derivations && !_quiet) ++_skipped;

            if (do_print || _export_derivations)
            {
                std::string input, output;
                {
                    const string::PremiseRendering premises;
                    string::node_to_string(this, input, _lang, ctx.current_condition, 3, augmented, parent);
                }
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

    if (held != ctx.rule_deductions.size()) release_unheld();
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
    const Variables&                condition_bindings,
    const std::unordered_set<Node>& fresh_vars,
    const adjacency_set&            deductions,
    Node                            parent,
    const int                       depth)
{
    std::vector<Node> consequences;
    for (Node deduction : deductions)
    {
        // A rule deduction contributes no fresh variable (deduce() exempts
        // it) and carries its own exact duplicate check in rebuild_rule, so
        // it neither satisfies this guard nor may it block the others. Nor
        // does a deduction that deduce() skips for want of exactly one
        // predicate: it adds nothing that a witness could stand for. A
        // consequence that would keep a variable of a rule it mentions
        // does not either: no firing derives it, thus it is considered
        // present, and the check demands only the remaining consequences
        // -- otherwise, a rule with a fresh variable beside it would make
        // a new witness with each pass.
        // asserts_fact, with the predicate kept for the test below.
        if (deduction == core.Contradiction || deduction_is_rule(deduction)) continue;
        const adjacency_set relations = relation_types_of(deduction);
        if (relations.size() != 1) continue;
        bool predicted = true;
        {
            std::lock_guard<std::mutex> lock(_mtx_network);
            const ConsequenceText&      text = consequence_text(deduction, parent, depth);
            if (instance_keeps_variable(deduction, text, condition_bindings, fresh_vars, parent, depth)) continue;
            predicted = !predicate_never_an_object(text, *relations.begin(), condition_bindings, parent);
        }

        // A consequence whose prediction has its predicate listed among its
        // objects is never written: deduce skips it before fact(), which
        // refuses a typed statement of this shape. It is considered present
        // as well. While the check required it, no witness could satisfy it,
        // and a rule with a further consequence that can be written created a
        // new witness and a new fact on each pass:
        // `(X p Y) => (Z Y X) (Z r X)` applied to `a p a` never ended under
        // `.semi-naive off` or `check`.
        //
        // A flat consequence is determined by the binding
        // (predicate_never_an_object), and the prediction is generated
        // exclusively in cases where the binding fails to provide an answer.
        if (predicted)
            if (const auto parts = predict_consequence(deduction, condition_bindings, depth, parent); parts && parts->targets.count(parts->relation) != 0) continue;

        consequences.push_back(deduction);
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
        return answer.is_known() && !answer.is_wrong() && !is_rule_pattern(answer.relation());
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
