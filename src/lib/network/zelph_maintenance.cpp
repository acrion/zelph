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

#include "zelph.hpp"

#include "fact_structure.hpp"

#include "zelph_impl.hpp"

#include <algorithm>

using namespace zelph::network;

void Zelph::cleanup_isolated(size_t& removed_count) const
{
    removed_count = 0;

    invalidate_fact_structures_cache();

    // The core nodes are exempt. Four of them carry no edges in a fresh
    // network -- the contradiction marker, nil, and the conjunction and
    // negation tags -- so a plain .cleanup used to delete them, and the next
    // rule concluding "!" then failed with "requested left node does not
    // exist" until .new. They are the engine's own vocabulary, not data.
    _pImpl->remove_isolated_nodes(removed_count,
                                  [this](const Node nd)
                                  { return !get_core_name(nd).empty(); });
}

size_t Zelph::cleanup_names() const
{
    return _pImpl->cleanup_dangling_names();
}

size_t Zelph::remove_node(Node node) const
{
    // Removing a core node leaves a network in which the next negation, list
    // or contradiction fails deep inside the engine ("requested node does not
    // exist"), with nothing pointing back at the command that caused it. The
    // merge path has always protected them; this is the same rule for the
    // removal path, which .remove reaches now that core spellings resolve.
    if (const std::string core = get_core_name(node); !core.empty())
    {
        throw std::runtime_error("Node '" + core + "' is part of the engine's core vocabulary and cannot be removed");
    }

    if (!_pImpl->exists(node))
    {
        throw std::runtime_error("Cannot remove non-existent node " + std::to_string(node));
    }

    invalidate_fact_structures_cache();

    // A fact minus one of its parts is not a fact -- and the graph cannot
    // say which one it is missing. With the OBJECT gone, the subject is the
    // only neighbour left, and a subject's link to its fact is
    // bidirectional: exactly the shape of a self-fact. `outside rel d` was
    // therefore indistinguishable from `outside rel outside`, answered
    // `outside rel X` as that, and went into the .bin on the next .save --
    // the engine asserting something nobody stated. So whatever the node is
    // a PART of goes with it.
    //
    // Strictly upwards: a doomed fact takes the facts it occurs in, never
    // its own subject, predicate or objects. Those exist in their own right
    // -- a nested fact used elsewhere, a condition two rules share -- and
    // walking down into them would delete knowledge that has nothing to do
    // with the node being removed.
    const auto is_part_of = [this](const Node whole, const Node part)
    {
        adjacency_set objects;
        if (parse_fact(whole, objects, 0) == part) return true;
        if (parse_relation(whole) == part) return true;
        return objects.count(part) != 0;
    };

    std::vector<Node>                  pending{node};
    ankerl::unordered_dense::set<Node> doomed{node};

    const auto doom = [&](const Node candidate)
    {
        if (doomed.count(candidate) != 0) return;
        // The engine's vocabulary is not data, as in prune_nodes.
        if (!get_core_name(candidate).empty()) return;

        doomed.insert(candidate);
        pending.push_back(candidate);
    };

    while (!pending.empty())
    {
        const Node current = pending.back();
        pending.pop_back();

        // Subject and object both point AT their fact, the predicate is
        // pointed at BY it, so both directions have to be looked at; which
        // of the two nodes is the WHOLE is then decided by is_part_of.
        adjacency_set neighbours = _pImpl->get_right(current);
        for (const Node from : _pImpl->get_left(current))
        {
            neighbours.insert(from);
        }

        for (const Node candidate : neighbours)
        {
            if (parse_relation(candidate) == 0) continue; // ordinary data
            if (!is_part_of(candidate, current)) continue;

            doom(candidate);
        }

        // A set IS its elements, so a container that loses one is no longer
        // the set anybody built -- and a rule's condition list is such a
        // set. Without this the rule kept firing on the conditions that
        // remained: removing `yellow` turned
        // `(X is yellow, X has petals) => (X is flower)` into
        // `(X has petals) => (X is flower)`, which then concluded that a
        // rose is a flower. Nobody wrote that rule. Membership is itself a
        // fact, so the container is reached from the doomed PartOf fact --
        // one more step upwards, not a second direction.
        if (parse_relation(current) == core.PartOf)
        {
            adjacency_set containers;
            const Node    member = parse_fact(current, containers, 0);

            // ... but a VARIABLE was never an element. `X in {a b}` is how a
            // rule quantifies over the members, not a claim about them, and
            // is_set_constant and the renderer both skip such a member for
            // exactly that reason. Dooming the container for it destroyed a
            // set constant that OTHER rules were written against -- reachable
            // from the outside, because the parse-time duplicate check builds
            // every rule in a scratch cluster and rolls it back: entering
            // `(A in {a b}) => (A flagged yes)` a second time took the
            // original rule with it and left `No rules found`.
            if (!Zelph::is_var(member))
            {
                for (const Node container : containers)
                {
                    doom(container);
                }
            }
        }
    }

    for (const Node dead : doomed)
    {
        _pImpl->remove(dead);            // Disconnects edges and removes from adjacency maps
        _pImpl->remove_node_names(dead); // Separate method for name cleanup
    }

    return doomed.size();
}

// Returns all nodes that are subjects of a core.Causes relation
// Building a fact in order to TALK about it cannot be told from asserting
// it: a fact node exists exactly when its edges exist, and the edges of
// `((X p Y) => (X q Y)) is nice` include those of the rule it mentions. So
// the rule fired, and `a p b` derived `a q b` although nobody had claimed
// the rule -- only that it is nice.
//
// A rule is the one construct where the difference is decidable from the
// graph alone, and cheaply: a rule that was ASSERTED is a part of nothing,
// while a mentioned one is the subject, the predicate or an object of the
// statement that mentions it. Ordinary facts and mathematical terms are not
// affected, and must not be: `(x + y) ≡ (y + x)` needs `x + y` to be a fact,
// and `(a p b) q c` needs `a p b`.
//
// The corner this cannot reach is a fully GROUND rule that is both asserted
// and mentioned -- hash-consing makes those one node. A rule with variables
// is two, because each statement names its own variables.
bool Zelph::is_mentioned(const Node node) const
{
    adjacency_set neighbours = _pImpl->get_right(node);
    for (const Node from : _pImpl->get_left(node))
    {
        neighbours.insert(from);
    }

    for (const Node whole : neighbours)
    {
        if (whole == node) continue;
        if (parse_relation(whole) == 0) continue; // not a fact

        adjacency_set objects;
        if (parse_fact(whole, objects, 0) == node) return true;
        if (parse_relation(whole) == node) return true;
        if (objects.count(node) != 0) return true;
    }

    return false;
}

// --- Rule patterns that are not data ---------------------------------------
// The rationale is in zelph.hpp; here is only how it is stored. The mark is
// the fact `(pattern ~ "rule pattern")`, i.e. ordinary graph structure: it
// survives a save, it prints and re-enters like anything else, and the
// engine keeps an index of it purely so that the per-candidate test in
// unification is a hash probe instead of a fact lookup.
//
// The predicate is a NAMED node rather than a core one on purpose. Core
// nodes take the first IDs at construction, so adding an eleventh would
// collide with node 11 of every .bin ever written.

namespace
{
    constexpr const char* rule_pattern_name = "rule pattern";
}

Node Zelph::rule_pattern_predicate(const bool create) const
{
    auto* self = const_cast<Zelph*>(this);
    if (!create)
    {
        const Node existing = self->get_node(rule_pattern_name, "zelph");
        return existing;
    }
    return self->node(rule_pattern_name, "zelph");
}

bool Zelph::is_rule_pattern(const Node node) const
{
    if (!_pImpl->_has_rule_patterns.load(std::memory_order_acquire)) return false;

    std::shared_lock lock(_pImpl->_rule_patterns_mtx);
    return _pImpl->_rule_patterns.count(node) != 0;
}

bool Zelph::unmark_rule_pattern(const Node node) const
{
    if (!_pImpl->_has_rule_patterns.load(std::memory_order_acquire)) return false;

    {
        std::unique_lock lock(_pImpl->_rule_patterns_mtx);
        if (_pImpl->_rule_patterns.erase(node) == 0) return false;
        _pImpl->_has_rule_patterns.store(!_pImpl->_rule_patterns.empty(), std::memory_order_release);
    }

    const Node pred = rule_pattern_predicate(false);
    if (pred != 0)
    {
        // Remove the marking FACT, not the pattern: the statement is a claim
        // from now on, and everything else about the node stays.
        const Answer mark = check_fact(node, core.IsA, {pred});
        if (mark.is_known())
        {
            const Node marker = mark.relation();

            // NOT through remove_node. That path invalidates through the
            // WHOLESALE funnel, which DISARMS the genuine-structure store for
            // the rest of the session -- and that store is what keeps
            // get_fact_structures off the adjacency walk. Eight patterns
            // unmarked during the Jacobian import were enough to turn 456k
            // store hits into 667k reconstruction walks and to triple the
            // runtime of the whole workload.
            //
            // The marking fact is the engine's own leaf: this function
            // created it, and nothing is ever built on top of it, so the
            // upward cascade of remove_node has nothing to find. What the
            // removal does need is exactly the invalidation a fact() of the
            // same shape performs, in the other direction -- the node's own
            // entry, its components' and one bidirectional level around
            // them. A marking somebody wrote ABOUT, on the other hand, is no
            // longer a leaf and takes the general path, disarm included.
            const bool is_leaf = _pImpl->get_right(marker).size() <= 2 // subject, predicate
                              && _pImpl->get_left(marker).size() <= 2; // subject, object

            if (is_leaf)
            {
                invalidate_fact_structures_for(node, core.IsA, {pred}, marker);
                _pImpl->remove(marker);
                _pImpl->remove_node_names(marker);
            }
            else
            {
                const_cast<Zelph*>(this)->remove_node(marker);
            }
        }
    }

    // This is the moment the statement BECOMES data, and everything that
    // reacts to a new fact has to hear about it -- semi-naive seeding above
    // all, which would otherwise never offer it to the rules whose conditions
    // it now satisfies. The node itself is old, so nothing else announces it.
    if (_on_fact_created)
    {
        if (const Node predicate = parse_relation(node); predicate != 0)
            _on_fact_created(node, predicate);
    }

    return true;
}

void Zelph::rebuild_rule_pattern_index() const
{
    std::unique_lock lock(_pImpl->_rule_patterns_mtx);
    _pImpl->_rule_patterns.clear();
    _pImpl->_has_rule_patterns.store(false, std::memory_order_release);

    const Node pred = rule_pattern_predicate(false);
    if (pred == 0) return;

    for (const Node subject : get_sources(core.IsA, pred, true))
        _pImpl->_rule_patterns.insert(subject);

    _pImpl->_has_rule_patterns.store(!_pImpl->_rule_patterns.empty(), std::memory_order_release);
}

void Zelph::mark_rule_patterns(const Node rule, const std::vector<Node>& created) const
{
    if (created.empty()) return;

    const std::unordered_set<Node> fresh(created.begin(), created.end());

    // The conditions and the consequences, and inside them every ground fact
    // node at any depth: "((a p b) g c)" leaks `a p b` just as readily as
    // itself. The `=>` fact is deliberately not walked as a pattern -- a rule
    // is not data, and whether it was asserted or merely mentioned is
    // decided by is_mentioned.
    adjacency_set consequences;
    const Node    condition = parse_fact(rule, consequences);
    if (condition == 0) return;

    std::vector<Node>        pending(consequences.begin(), consequences.end());
    std::unordered_set<Node> seen;
    std::vector<Node>        patterns;

    // A conjunction set carries no structure of its own; its members hang off
    // it as PartOf facts.
    if (check_fact(condition, core.IsA, {core.Conjunction}).is_known())
    {
        for (const Node rel : get_right(condition))
        {
            if (parse_relation(rel) != core.PartOf) continue;
            adjacency_set objs;
            const Node    member = parse_fact(rel, objs, 0);
            if (member != 0 && objs.count(condition) == 1) pending.push_back(member);
        }
    }
    else
    {
        pending.push_back(condition);
    }

    while (!pending.empty())
    {
        const Node nd = pending.back();
        pending.pop_back();
        if (nd == 0 || !seen.insert(nd).second) continue;
        if (Impl::is_var(nd)) continue;

        // A SET node carries no structure of its own -- its members hang off
        // it as PartOf facts, exactly as for the conjunction set above -- and
        // Zelph::set builds it with create(), so its ID is a COUNTER, not a
        // triple hash. It therefore failed the is_hash gate below before the
        // structural descent could even stop at it, and the membership facts
        // a rule's own set literal created were never marked. They then read
        // as data: `(X in {a b}) => (X flagged yes)` derived `a flagged yes`
        // and `b flagged yes`, and `.explain` called `a in {a b}` an axiom,
        // although the only reason that fact exists is that the rule was
        // written. Same leak afc0f3e closed for the other shapes.
        //
        // Only a container THIS construction created is walked -- a set the
        // rule merely refers to keeps its members and its own facts -- so the
        // adjacency read stays inside what the rule brought into being.
        // A SET CONSTANT is excluded: its membership is definitional, not
        // asserted -- `a in {a b}` holds because that is what the set IS --
        // so a rule quantifying over it with `(X in {a b})` legitimately
        // binds a and b. Only a COLLECTION the rule itself built carries
        // members nobody claimed.
        if (fresh.count(nd) != 0 && !is_set_constant(nd))
        {
            for (const Node rel : get_right(nd))
            {
                if (parse_relation(rel) != core.PartOf) continue;
                adjacency_set objs;
                const Node    member = parse_fact(rel, objs, 0);
                if (member == 0 || objs.count(nd) != 1) continue;
                pending.push_back(rel); // the membership fact is the pattern
                pending.push_back(member);
            }
        }

        if (!Impl::is_hash(nd)) continue; // an atom has no fact structure

        const FactStructure fs = get_preferred_structure(this, nd, 3);
        if (fs.predicate == 0 || fs.subject == 0) continue;

        // Only what this construction brought into being, and only where
        // there is no variable to give it away as a template.
        if (fresh.count(nd) != 0 && !var_in_closure(nd)) patterns.push_back(nd);

        pending.push_back(fs.subject);
        // The PREDICATE too. It is a named atom for every ordinary rule, so
        // the is_hash gate above drops it again at no cost -- but a COMPOSITE
        // predicate is a ground fact node like any other, and writing
        // `(a (b r s) c) => (d q e)` brought `b r s` into being. Left out of
        // the descent, it read as data: `(X r Y) => (X leaked Y)` derived
        // `b leaked s` from a fact nobody had claimed. The same leak afc0f3e
        // closed for the subject and the objects.
        pending.push_back(fs.predicate);
        for (const Node o : fs.objects)
            pending.push_back(o);
    }

    if (patterns.empty()) return;

    const Node pred = rule_pattern_predicate(true);
    auto*      self = const_cast<Zelph*>(this);

    std::unique_lock lock(_pImpl->_rule_patterns_mtx);
    for (const Node p : patterns)
    {
        self->fact(p, core.IsA, {pred});
        _pImpl->_rule_patterns.insert(p);
    }
    _pImpl->_has_rule_patterns.store(true, std::memory_order_release);
}

adjacency_set Zelph::get_rules() const
{
    const adjacency_set& rule_candidates = _pImpl->get_left(core.Causes);

    adjacency_set rules;

    for (Node rule_candidate : rule_candidates)
    {
        // We filter the rule candidates in the same way as Reasoning::apply_rule() does it.
        // Note that a rule candidate with empty deductions is interpreted as a question, see Reasoning::evaluate()
        if (rule_candidate)
        {
            adjacency_set deductions;
            Node          condition = parse_fact(rule_candidate, deductions);
            if (condition && condition != core.Causes && !deductions.empty()
                && !is_mentioned(rule_candidate))
            {
                rules.insert(rule_candidate);
            }
        }
    }

    return rules;
}

void Zelph::remove_rules() const
{
    adjacency_set rules = get_rules();
    for (Node rule : rules)
    {
        invalidate_fact_structures_cache();

        _pImpl->remove(rule);
        // Clean up names
        for (auto& lang_map : _pImpl->_name_of_node)
        {
            lang_map.second.erase(rule);
        }
        for (auto& lang_map : _pImpl->_node_of_name)
        {
            for (auto it = lang_map.second.begin(); it != lang_map.second.end();)
            {
                if (it->second == rule)
                {
                    it = lang_map.second.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }
    }
}

size_t Zelph::rule_count() const
{
    return get_rules().size();
}

#ifndef __EMSCRIPTEN__
void Zelph::save_to_file(const std::string& filename) const
{
    _pImpl->saveToFile(filename);
}

// A slice is a network in its own right that contains the facts of the given
// predicates, the nodes those facts connect, every name of those nodes, and
// nothing else. It exists because the interesting questions about a large
// graph rarely need all of it: the class hierarchy of Wikidata is 5.2 million
// P279 facts inside 88 GB, and once it stands alone it fits in a fraction of
// the memory while answering the same queries with the same engine.
//
// Two things have to travel with the facts or the result is a network that
// looks complete and answers nothing:
//
//   * the relation-type declaration of each predicate -- fact structures are
//     only reconstructed for declared predicates, so without it every query
//     over the slice returns empty. It is addressed by its content hash
//     rather than searched: core.IsA's adjacency holds every instance-of fact
//     of a mapped Wikidata network, and scanning that to find one edge would
//     cost a full pass over the graph.
//   * the structural closure of everything retained -- a fact whose subject
//     is itself a fact (a rule, a qualified statement) drags in the nodes it
//     is built from, or the loaded slice has a node whose structure cannot be
//     read back.
size_t Zelph::save_predicate_slice(const std::string& filename, const std::vector<Node>& predicates, size_t* rules_kept) const
{
    // Every rule of the network travels with the slice. Which ones happened
    // to be reachable from the retained facts was not an answer anyone could
    // predict, and it silently excluded the class that matters most: a
    // contradiction rule's consequence is the core `!` node, a fact of no
    // predicate, so nothing retained ever reached it and a slice stopped
    // reporting contradictions its source reports. Carrying all of them
    // makes the slice reason exactly as its source does over the predicates
    // it kept; a rule whose conditions name a predicate that stayed behind
    // is complete and simply never matches.
    //
    // Read before the locks below: get_rules() takes its own.
    const adjacency_set rules = get_rules();

    if (rules_kept) *rules_kept = rules.size();

    ankerl::unordered_dense::set<Node> keep{
        core.RelationTypeCategory, core.Causes, core.IsA, core.Unequal, core.Contradiction,
        core.Cons, core.Nil, core.PartOf, core.Conjunction, core.Negation};

    size_t facts = 0;

    {
        // Same lock order as the writers: left before right.
        std::shared_lock<std::shared_mutex> lock_left(_pImpl->_smtx_left);
        std::shared_lock<std::shared_mutex> lock_right(_pImpl->_smtx_right);

        const auto exists_unlocked = [this](const Node nd)
        { return _pImpl->_left.find(nd) != _pImpl->_left.end(); };

        std::vector<Node> pending;

        const auto retain = [&keep, &pending](const Node nd)
        {
            if (keep.insert(nd).second) pending.push_back(nd);
        };

        for (const Node p : predicates) retain(p);
        for (const Node rule : rules) retain(rule);

        // The declarations that make the predicates usable, including the
        // core ones (a freshly constructed network re-creates those, but a
        // load replaces the whole state, so they have to be in the file).
        std::vector<Node> declared(predicates);
        declared.insert(declared.end(), {core.IsA, core.Unequal, core.Causes, core.Cons, core.PartOf});
        for (const Node nd : declared)
        {
            const Node declaration = create_hash(core.IsA, nd, {core.RelationTypeCategory});
            if (exists_unlocked(declaration)) retain(declaration);
        }

        for (const Node p : predicates)
        {
            const auto rels_it = _pImpl->_right.find(p);
            if (rels_it == _pImpl->_right.end()) continue;

            for (const Node rel : rels_it->second)
            {
                const auto rel_left  = _pImpl->_left.find(rel);
                const auto rel_right = _pImpl->_right.find(rel);
                if (rel_left == _pImpl->_left.end() || rel_right == _pImpl->_right.end()) continue;

                if (rel_left->second.count(p) == 0) continue;

                // rel can also be a fact ABOUT p -- the declaration above, or
                // "P279 is a transitive relation". Those carry p as their
                // SUBJECT, which puts it on both sides of rel just like the
                // predicate position does, so the two are told apart the same
                // way the index builder does it: a fact OF p has a subject
                // that is not p and stands on both sides of rel.
                const bool is_fact_of_p =
                    std::any_of(rel_left->second.begin(), rel_left->second.end(),
                                [&](const Node candidate)
                                { return candidate != p
                                      && !Impl::is_var(candidate)
                                      && rel_right->second.count(candidate) == 1; });
                if (!is_fact_of_p) continue;

                retain(rel);
                ++facts;
                for (const Node nd : rel_left->second) retain(nd);
                for (const Node nd : rel_right->second) retain(nd);
            }
        }

        // Structural closure: expand retained fact nodes until nothing new
        // appears. Plain nodes are not expanded -- their adjacency is the
        // rest of the graph, which is exactly what the slice leaves behind.
        while (!pending.empty())
        {
            const Node nd = pending.back();
            pending.pop_back();

            // The tags that give a node its meaning to the reasoner. A rule's
            // condition set is a FRESH node, not a content-addressed one (two
            // literal sets are never the same node), so the loop below skips
            // it and everything reachable only through it stayed behind --
            // including "<set> ~ conjunction". Without that tag the reasoner
            // reads the whole set as a single condition: the sliced network
            // still REPORTED the rule, printed it in full and counted it, and
            // could not fire it, while .explain called the facts it had once
            // derived axioms. Addressed by content hash rather than searched,
            // like the relation-type declaration above: core.IsA's adjacency
            // is most of the graph.
            for (const Node tag : {core.Conjunction, core.Negation})
            {
                const Node tagged = create_hash(core.IsA, nd, {tag});
                if (exists_unlocked(tagged)) retain(tagged);
            }

            if (!Impl::is_hash(nd)) continue;

            const auto left_it = _pImpl->_left.find(nd);
            if (left_it != _pImpl->_left.end())
            {
                for (const Node participant : left_it->second) retain(participant);
            }

            const auto right_it = _pImpl->_right.find(nd);
            if (right_it != _pImpl->_right.end())
            {
                for (const Node participant : right_it->second) retain(participant);
            }
        }
    }

    _pImpl->saveToFile(filename, &keep);

    return facts;
}

void Zelph::load_from_file(const std::string& filename) const
{
    invalidate_fact_structures_cache();

    _pImpl->loadFromFile(filename);
    rebuild_rule_pattern_index();
}

void Zelph::load_from_file(const std::string& filename, const BinChunkSelection& selection, const bool skip_payload) const
{
    invalidate_fact_structures_cache();

    _pImpl->loadFromFile(filename, selection, skip_payload);
    rebuild_rule_pattern_index();
}

void Zelph::load_from_manifest(const std::string&       manifest_path,
                               const BinChunkSelection& selection,
                               const std::string&       shard_root,
                               const std::string&       bin_path_override,
                               const bool               skip_payload) const
{
    invalidate_fact_structures_cache();

    _pImpl->loadFromManifest(manifest_path, selection, shard_root, bin_path_override, skip_payload);
    rebuild_rule_pattern_index();
}
#endif

void        Zelph::set_active_cluster(const std::string& name) const { _pImpl->set_active_cluster(name); }
void        Zelph::deactivate_cluster() const { _pImpl->deactivate_cluster(); }
std::string Zelph::active_cluster_name() const { return _pImpl->active_cluster_name(); }

std::vector<std::pair<std::string, size_t>> Zelph::list_clusters() const { return _pImpl->list_clusters(); }

std::vector<Node> Zelph::cluster_nodes(const std::string& name) const { return _pImpl->cluster_nodes(name); }

bool Zelph::merge_cluster(const std::string& from, const std::string& to) const
{
    return _pImpl->merge_cluster(from, to);
}

// Destructive: removes every node recorded in the cluster, including all
// of their edges and names. Nodes that no longer exist (e.g. merged away
// by set_name) are skipped silently.
size_t Zelph::drop_cluster(const std::string& name) const
{
    const std::vector<Node> nodes = _pImpl->take_cluster(name);
    if (nodes.empty()) return 0;

    invalidate_fact_structures_cache();

    // Counting the calls would UNDERSTATE the damage twice over: removing
    // one node takes the facts it is part of, so a later entry of the same
    // cluster may already be gone, and the facts themselves need not be in
    // the cluster at all. What the caller reports is what actually went.
    size_t removed = 0;
    for (const Node n : nodes)
    {
        if (_pImpl->exists(n))
        {
            removed += remove_node(n);
        }
    }
    return removed;
}