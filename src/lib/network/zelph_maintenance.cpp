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

// Erase the names of a whole batch of removed nodes in one pass over the
// reverse name map -- what `remove_node`'s `deferred_names` collected.
void Zelph::remove_names_of(const adjacency_set& dead) const
{
    _pImpl->remove_names_of(dead);
}

// How often the reverse name map was walked end to end. A node's reverse
// entries can only be found BY VALUE, so the walk is unavoidable; what is
// avoidable is doing it once per node instead of once per batch, which is
// what made a full-dump prune run at one node per second. Hardware-
// independent, so a test can hold that shape without a quiet machine.
uint64_t Zelph::name_map_scans() const
{
    return _pImpl->_name_map_scans.load(std::memory_order_relaxed);
}

// `deferred_names`, when given, COLLECTS the removed nodes instead of having
// their names erased here, and the caller then owes one `remove_names_of` for
// the whole batch.
//
// That is the only way a bulk removal can afford names at all. Erasing them
// per node costs a walk of the reverse name map apiece, and on the full
// Wikidata network that map holds ~204 million entries: the 6.2 million
// removals of the full-dump prune measured 1.11 nodes per second with 97.7 %
// of the time in that walk, which is two months for a script that has to
// finish overnight. `.remove` keeps the per-node path, where one walk IS one
// batch.
// Everything that removing `node` would take with it: the node itself, whatever
// it is a PART of (transitively), and a container that loses a member. Adds to
// `out` and explores nothing `out` already holds, so a caller may accumulate
// several victims into one set and a node reached twice is walked once.
//
// READ-ONLY -- it touches no adjacency and no name map, which is what lets a
// bulk removal run MANY of these at once. Erasing is the half that cannot be
// parallelised: there is one shared_mutex per adjacency map, so every writer
// is exclusive anyway, and a fact whose parts are half deleted reads as a
// different fact, which is the very reasoning this cascade rests on.
//
// That collecting a batch against the UNMUTATED graph gives the same set as
// removing its victims one after another is the argument prune_nodes rests
// on, and it holds in both directions. Removing other victims first only
// DELETES structure, so a victim's closure on the reduced graph is a subset
// of its closure on the full one. And a node this closure reaches through a
// fact that another victim takes away is doomed by THAT victim's closure
// instead, since the cascade runs upwards and whatever contains a doomed
// node is doomed with it. The union is therefore the same -- and, unlike the
// sequential answer, it does not depend on the order an unordered_set
// happened to iterate in.
void Zelph::collect_doomed(const Node node, adjacency_set& out) const
{
    if (out.count(node) != 0) return; // walked already, cascade included

    // The EXACT decomposition, not parse_fact's adjacency reading. A fact
    // that uses `whole` as its PREDICATE points at it and is not pointed
    // back at -- exactly like an object -- so parse_fact reported every such
    // fact among the objects of `whole`. Removing one fact with a composite
    // predicate therefore doomed the predicate FACT, and with it every other
    // fact using that predicate: `.prune-facts (x (a p b) y)` reported one
    // removal and silently took `z (a p b) w` and `a p b` as well.
    //
    // Every reading counts: on removal the conservative answer is the safe
    // one, and the reconstruction offers all of them. It happens in two
    // phases below -- the graph reads under the scope, the rest after it --
    // and the structures are held in NAMED storage, never read through a
    // call returning a smart pointer: see the note at
    // Zelph::get_fact_subjects. This is where that bug was found -- a prune
    // that stops caching structures read freed memory here, and the cascade
    // then found nothing.

    // The predicate memo is fetched ONCE for the whole cascade, not once per
    // node walked. It cannot change while this runs -- the collection phase
    // only reads -- and its accessor takes a shared lock of its own, which is
    // one more contended cache line per node when a dozen threads collect in
    // parallel.
    const auto rel_types = relation_type_set();

    std::vector<Node> pending{node};
    out.insert(node);

    const auto doom = [&](const Node candidate)
    {
        if (out.count(candidate) != 0) return;
        // The engine's vocabulary is not data, as in prune_nodes.
        if (!get_core_name(candidate).empty()) return;

        out.insert(candidate);
        pending.push_back(candidate);
    };

    while (!pending.empty())
    {
        const Node current = pending.back();
        pending.pop_back();

        // Subject and object both point AT their fact, the predicate is
        // pointed at BY it, so both directions have to be looked at; which
        // of the two nodes is the WHOLE is then decided by is_part_of.
        //
        // Read under ONE scope, and by REFERENCE. Every adjacency read here
        // used to copy the whole set -- two per neighbour, since
        // parse_relation copies twice per call -- and a profile of the live
        // Wikidata prune put 100 % of its time under this loop, 51 % of that
        // in `adjacency_set::copy_from` and half of THAT in allocating the
        // copy's hash buckets. Copying is also the worst thing to do to a
        // graph that lives in swap: it touches every byte of the source and
        // then writes a second one.
        //
        // The scope covers the READS only. is_part_of goes through
        // get_fact_structures, which takes the same shared locks, so it runs
        // after the scope is closed -- hence the two-pass shape: the cheap
        // test names the candidates, the expensive one judges them.
        // A candidate whose structure still has to be finished once the scope
        // is released, with the state the scoped half produced for it.
        struct Unfinished
        {
            Node              candidate;
            FactStructureList structures;
            bool              no_predicates;
        };

        std::vector<Unfinished>                        unfinished;
        std::vector<std::pair<Node, FactStructurePtr>> answered;
        {
            const Network::ReadScope scope = read_scope();

            adjacency_set neighbours;
            for (const Node n : scope.right(current))
                neighbours.insert(n);
            for (const Node n : scope.left(current))
                neighbours.insert(n);

            for (const Node candidate : neighbours)
            {
                // parse_relation_scoped decides membership by the memo alone,
                // where parse_relation additionally probes the declaration's
                // probability. The two differ only for a declaration asserted
                // as WRONG, which nothing produces -- and get_fact_structures,
                // which is_part_of asks next, has always read the memo, so
                // this makes the two steps agree rather than differ.
                // ONE probe of the candidate's outgoing set for the whole
                // decision. It used to be three -- parse_relation_scoped
                // fetched it, then the reconstruction asked exists() and
                // fetched it again -- while do_find on these maps was 54 % of
                // this loop's profile once the lock traffic was gone.
                const adjacency_set* const outgoing = scope.try_right(candidate);
                if (outgoing == nullptr) continue; // gone already

                if (parse_relation_scoped(scope, *rel_types, candidate, outgoing) == 0) continue; // ordinary data

                // The structure is reconstructed HERE, under the scope that is
                // already open, rather than by is_part_of afterwards. That is
                // the whole reason this shape exists: is_part_of went through
                // get_fact_structures, which opened its OWN scope per
                // candidate, so a node with sixteen facts paid seventeen
                // shared-lock pairs instead of one. With a dozen threads
                // collecting in parallel those pairs do not contend for
                // exclusivity -- they contend for the CACHE LINE the rwlock
                // counter lives on, which is why the parallel phase measured
                // five times the CPU of the single-core one for the same wall
                // time.
                Unfinished       u{candidate, {}, false};
                FactStructurePtr done;
                if (begin_fact_structures_scoped(this, scope, *rel_types, candidate, outgoing, 3, u.structures, u.no_predicates, done))
                {
                    unfinished.push_back(std::move(u));
                }
                else if (done && !done->empty())
                {
                    answered.emplace_back(candidate, std::move(done));
                }
            }
        }

        // Scope released: finishing locks (disambiguation reads
        // parse_relation, logging reads format), so it may only happen here.
        const auto contains = [](const FactStructureList& structures, const Node part)
        {
            for (const auto& fs : structures)
            {
                if (fs.subject == part || fs.predicate == part) return true;
                if (fs.objects.count(part) != 0) return true;
            }
            return false;
        };

        for (auto& u : unfinished)
        {
            const auto structures = finish_fact_structures(this, u.candidate, 3, u.structures, u.no_predicates);
            if (contains(*structures, current)) doom(u.candidate);
        }

        for (const auto& [candidate, structures] : answered)
        {
            if (contains(*structures, current)) doom(candidate);
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
}

// Erase what collect_doomed found, with the cache invalidation and the
// relation-type memo that implies. The serial half of a removal.
void Zelph::remove_doomed(const adjacency_set& doomed, adjacency_set* const deferred_names) const
{
    // A refuted node that is being removed must leave the index with its
    // marking fact, and this is the one index where a stale entry is not
    // merely wasteful. A contradiction record is a SET CONSTANT, so it is
    // content-addressed: remove one of the facts it is about, enter that fact
    // again, and the record comes back with the SAME id. A stale entry then
    // says "already known" about a contradiction whose record the graph no
    // longer holds, and the report never returns.
    //
    // _rule_patterns beside it does not need this: a removed pattern is gone
    // and nothing asks about it again, because a pattern is not re-created by
    // re-entering data.
    forget_refuted(doomed);

    // What this removal can make stale in the structure cache, collected
    // WHILE THE EDGES STILL EXIST. The counterpart of
    // invalidate_fact_structures_for on the creation side, and it rests on the
    // same argument read backwards: a cached reading of a node can only change
    // if the adjacency it was reconstructed from changed, and removal changes
    // the adjacency of exactly the doomed nodes and their neighbours. Both
    // directions count, because removal deletes edges both ways -- the
    // bidirectional restriction of the creation side is about hubs GROWING and
    // has no analogue here.
    //
    // Why this matters: the wholesale clear that used to stand at the top of
    // this function ran once per removed node, so a bulk prune never let the
    // cache hold anything. Measured with the counters on a 99 893-node prune:
    // 99 900 full clears, fs_cache hits 1 009 against 199 797 misses, every
    // structure reconstruction cold. The creation side made this exact move
    // already -- see the comment above invalidate_fact_structures_for, which
    // records 1.28M reconstructions in the Jacobian diffby phase.
    //
    // Two cases still need the wholesale clear, and both are cheap to detect:
    // a neighbourhood beyond the budget (a hub), and the removal of a
    // relation-type DECLARATION, which changes predicate detection globally --
    // the same case (3) the creation side falls back on. The budget is larger
    // than the creation side's 256 because `stale` here is a union over the
    // whole doomed cascade rather than the neighbourhood of one new fact.
    constexpr size_t  stale_budget = 1024;
    std::vector<Node> stale;
    bool              bounded             = true;
    bool              declaration_removed = false;

    stale.reserve(doomed.size() * 4);

    for (const Node dead : doomed)
    {
        if (!declaration_removed && is_relation_type_declaration(dead)) declaration_removed = true;

        stale.push_back(dead);

        if (!bounded) continue;

        for (const Node n : _pImpl->get_right(dead))
            stale.push_back(n);
        for (const Node n : _pImpl->get_left(dead))
            stale.push_back(n);

        if (stale.size() > stale_budget) bounded = false;
    }

    for (const Node dead : doomed)
    {
        _pImpl->remove(dead); // Disconnects edges and removes from adjacency maps

        // Separate method for name cleanup -- unless the caller is removing
        // in bulk and takes the whole batch at the end.
        if (deferred_names == nullptr) _pImpl->remove_node_names(dead);
    }

    if (deferred_names != nullptr)
    {
        for (const Node dead : doomed)
            deferred_names->insert(dead);
    }

    if (declaration_removed)
    {
        // A predicate that has lost its relation-type declaration must stop
        // being read as one, and the memoized set is what every predicate test
        // goes through. Without this the SESSION went on reading the facts of
        // that predicate while a reload of its own `.save` did not: the same
        // network answered `.list-rules` with a rule before the round trip and
        // with "No rules found" after it, because the set is rebuilt from the
        // graph on load and the declaration is not in the graph any more.
        invalidate_relation_type_set();
    }

    if (declaration_removed || !bounded)
    {
        invalidate_fact_structures_cache();
    }
    else
    {
        _pImpl->invalidate_predicate_index();
        erase_fact_structures(stale);
    }
}

size_t Zelph::remove_node(Node node, adjacency_set* const deferred_names) const
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

    // The fact-path stores are authoritative PER NODE -- absence of an entry
    // is meaningful -- so an entry for a node that is about to go must never
    // resurface. Disarming is one-way and idempotent, which is why it stays
    // where the wholesale invalidation used to be. The structure CACHE is
    // handled at the end, targeted; see the comment there.
    disable_fact_stores();

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
    adjacency_set doomed;
    collect_doomed(node, doomed);
    remove_doomed(doomed, deferred_names);

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

    // A NAMED node for the same reason, and the criterion in CLAUDE.md under
    // "What must be a CORE node" gives the same answer: nothing has to reach
    // it without a name lookup.
    constexpr const char* refuted_fact_name = "refuted";
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

bool Zelph::is_refuted_fact(const Node node) const
{
    if (!_pImpl->_has_refuted_facts.load(std::memory_order_acquire)) return false;

    std::shared_lock lock(_pImpl->_refuted_facts_mtx);
    return _pImpl->_refuted_facts.count(node) != 0;
}

void Zelph::mark_refuted_fact(const Node node) const
{
    if (node == 0) return;

    // The marking is a fact like the rule-pattern one, so it survives a save
    // and is what rebuild_refuted_index reads a loaded network back from. The
    // probability alone cannot serve: the weight store is keyed by a hash of
    // the edge, so a loaded file cannot be asked which of its entries were
    // fact probabilities rather than synapse weights.
    auto*      self = const_cast<Zelph*>(this);
    const Node pred = self->node(refuted_fact_name, "zelph");
    self->fact(node, core.IsA, {pred});

    std::unique_lock lock(_pImpl->_refuted_facts_mtx);
    _pImpl->_refuted_facts.insert(node);
    _pImpl->_has_refuted_facts.store(true, std::memory_order_release);
}

bool Zelph::has_refuted_facts() const
{
    return _pImpl->_has_refuted_facts.load(std::memory_order_acquire);
}

void Zelph::forget_refuted(const adjacency_set& gone) const
{
    if (!_pImpl->_has_refuted_facts.load(std::memory_order_acquire)) return;

    std::unique_lock lock(_pImpl->_refuted_facts_mtx);
    for (const Node nd : gone)
        _pImpl->_refuted_facts.erase(nd);
    _pImpl->_has_refuted_facts.store(!_pImpl->_refuted_facts.empty(), std::memory_order_release);
}

void Zelph::rebuild_refuted_index() const
{
    std::unique_lock lock(_pImpl->_refuted_facts_mtx);
    _pImpl->_refuted_facts.clear();
    _pImpl->_has_refuted_facts.store(false, std::memory_order_release);

    auto*      self = const_cast<Zelph*>(this);
    const Node pred = self->get_node(refuted_fact_name, "zelph");
    if (pred == 0) return;

    for (const Node subject : get_sources(core.IsA, pred, true))
        _pImpl->_refuted_facts.insert(subject);

    _pImpl->_has_refuted_facts.store(!_pImpl->_refuted_facts.empty(), std::memory_order_release);
}

bool Zelph::unmark_rule_pattern(const Node node) const
{
    if (!_pImpl->_has_rule_patterns.load(std::memory_order_acquire)) return false;

    {
        std::unique_lock lock(_pImpl->_rule_patterns_mtx);
        if (_pImpl->_rule_patterns.erase(node) == 0) return false;
        _pImpl->_has_rule_patterns.store(!_pImpl->_rule_patterns.empty(), std::memory_order_release);
    }

    // The one change an experiment makes to a PRE-EXISTING node that a drop
    // has to undo -- see Network::note_unmarked for why this and nothing
    // else. Recorded before the marking fact goes, so a failure below cannot
    // leave the cluster claiming a revocation that did not happen.
    _pImpl->note_unmarked(node);

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

bool Zelph::condition_set_members(const Node condition, adjacency_set& out) const
{
    if (condition == 0 || !exists(condition)) return false;

    const bool tagged = check_fact(condition, core.IsA, {core.Conjunction}).is_known();

    // A STATEMENT has a predicate; a container has none. Asked first, this
    // keeps the member walk off every ordinary condition of every rule --
    // the hot path -- so only a container is ever walked, and only an
    // UNTAGGED one reaches this test at all.
    if (!tagged && parse_relation(condition) != 0) return false;

    // The members hang off the container as PartOf facts pointing AT it.
    adjacency_set members;
    for (const Node rel : get_right(condition))
    {
        if (parse_relation(rel) != core.PartOf) continue;
        adjacency_set objs;
        const Node    member = parse_fact(rel, objs, 0);
        if (member != 0 && objs.count(condition) == 1) members.insert(member);
    }

    if (members.empty()) return false;

    // Several members need the tag to say how they combine; one member does
    // not, because every combination of one thing is that thing.
    if (members.size() > 1 && !tagged) return false;

    out = std::move(members);
    return true;
}

bool Zelph::is_condition_set(const Node condition) const
{
    adjacency_set members;
    return condition_set_members(condition, members);
}

std::vector<std::pair<Node, Zelph::HashRecipe>> Zelph::collect_hash_dependents(const Node node) const
{
    // Read the recipe of one hash-identified node. A set constant is asked
    // for its members, everything else for its triple. Both readings hold
    // only while the ids do -- afterwards the components no longer hash back
    // to the node and the verifying reader rejects it.
    const auto recipe_of = [this](const Node nd, HashRecipe& out) -> bool
    {
        if (!Impl::is_hash(nd) || !exists(nd)) return false;

        if (is_set_constant(nd))
        {
            out.is_set = true;
            for (const Node rel : get_right(nd))
            {
                if (parse_relation(rel) != core.PartOf) continue;
                adjacency_set objs;
                const Node    member = parse_fact(rel, objs, 0);
                if (member != 0 && objs.count(nd) == 1) out.members.insert(member);
            }
            return !out.members.empty();
        }

        const FactStructure fs = get_preferred_structure(this, nd, 3);
        if (fs.subject == 0 || fs.predicate == 0) return false;

        out.is_set    = false;
        out.subject   = fs.subject;
        out.predicate = fs.predicate;
        out.objects   = fs.objects;
        return true;
    };

    const auto components_of = [](const HashRecipe& r)
    {
        adjacency_set out;
        if (r.is_set)
        {
            for (const Node m : r.members)
                out.insert(m);
        }
        else
        {
            out.insert(r.subject);
            out.insert(r.predicate);
            for (const Node o : r.objects)
                out.insert(o);
        }
        return out;
    };

    // Reach every node built on `node`, at any depth. A candidate counts when
    // one of its COMPONENTS is already known to be affected -- not merely
    // when it is adjacent to the node just popped. A set constant is why: it
    // is identified by its members, and the walk reaches it through a
    // membership FACT, which is not one of them.
    ankerl::unordered_dense::map<Node, HashRecipe> affected;
    std::unordered_set<Node>                       reached{node};
    std::vector<Node>                              frontier{node};

    while (!frontier.empty())
    {
        std::vector<Node> next;

        for (const Node current : frontier)
        {
            // Both directions: a fact points at its subject and predicate, an
            // object points at its fact, and a container is two hops away
            // through the membership fact.
            adjacency_set neighbours = get_right(current);
            for (const Node n : get_left(current))
                neighbours.insert(n);

            for (const Node cand : neighbours)
            {
                if (reached.count(cand) != 0) continue;

                HashRecipe recipe;
                if (!recipe_of(cand, recipe)) continue;

                const adjacency_set parts       = components_of(recipe);
                const auto          is_affected = [&reached](const Node c)
                { return reached.count(c) != 0; };
                if (std::none_of(parts.begin(), parts.end(), is_affected)) continue;

                reached.insert(cand);
                next.push_back(cand);
                affected.emplace(cand, std::move(recipe));
            }
        }

        frontier = std::move(next);
    }

    // Emit in DEPENDENCY order, which is not the order the walk found them
    // in: a membership fact is reached one step before its container, and the
    // container is what the fact is built FROM. Rebuilding the fact first
    // would compute its id from the container's OLD id and leave it stale a
    // second time. Hash-consing makes the relation acyclic -- a node's id is
    // computed from its components' ids -- so this terminates.
    std::vector<std::pair<Node, HashRecipe>> ordered;
    ordered.reserve(affected.size());
    std::unordered_set<Node> emitted;

    while (ordered.size() < affected.size())
    {
        const std::size_t before = ordered.size();

        for (const auto& [nd, recipe] : affected)
        {
            if (emitted.count(nd) != 0) continue;

            const adjacency_set parts     = components_of(recipe);
            const auto          waits_for = [&](const Node c)
            { return c != nd && affected.count(c) != 0 && emitted.count(c) == 0; };
            if (std::any_of(parts.begin(), parts.end(), waits_for)) continue;

            emitted.insert(nd);
            ordered.emplace_back(nd, recipe);
        }

        if (ordered.size() == before) break; // defensive: never loop forever
    }

    return ordered;
}

void Zelph::rehash_dependents(const std::vector<std::pair<Node, HashRecipe>>& recipes, const Node from, const Node into) const
{
    if (recipes.empty()) return;

    std::unordered_map<Node, Node> remap{{from, into}};

    // A repair is not new knowledge. Creating a rebuilt node while a cluster
    // is active would record it there, and dropping that cluster would then
    // delete a fact that existed long before it -- exactly the promise
    // .cluster-drop makes and keeps everywhere else. Recording is therefore
    // suspended, and each rebuilt node inherits the membership of the node it
    // replaces (see retag_cluster_member).
    const std::string previous_cluster = active_cluster_name();
    if (!previous_cluster.empty()) deactivate_cluster();

    const auto mapped = [&remap](const Node nd)
    {
        const auto it = remap.find(nd);
        return it == remap.end() ? nd : it->second;
    };

    for (const auto& [old_id, recipe] : recipes)
    {
        if (!exists(old_id)) continue; // folded away by an earlier round

        Node new_id = 0;
        if (recipe.is_set)
        {
            adjacency_set members;
            for (const Node m : recipe.members)
                members.insert(mapped(m));
            new_id = Impl::create_hash(members);
        }
        else
        {
            adjacency_set objects;
            for (const Node o : recipe.objects)
                objects.insert(mapped(o));
            new_id = Impl::create_hash(mapped(recipe.predicate), mapped(recipe.subject), objects);
        }

        if (new_id == 0 || new_id == old_id) continue;

        // The equal node may already be there -- `a p b` whose subject merges
        // into `c` lands on a `c p b` the graph already holds, and folding
        // the two is the right answer: they are the same statement. Where it
        // is not there, the id is created empty and the merge below gives it
        // the adjacency the old node already carries, which the component
        // merges have brought up to date.
        const bool created_here = !exists(new_id);
        if (created_here) _pImpl->create(new_id);

        _pImpl->merge(old_id, new_id);
        _pImpl->transfer_names_locked(old_id, new_id);

        // Only where the node was really re-created: folding into one that
        // was already there must not drag a pre-existing node into a cluster.
        _pImpl->retag_cluster_member(old_id, created_here ? new_id : 0);

        remap[old_id] = new_id;
    }

    if (!previous_cluster.empty()) set_active_cluster(previous_cluster);

    // Every index keyed by node id, and the relation-type set: a rebuilt node
    // is created through the graph primitives rather than through fact(), so
    // nothing on that path announces that `a ~ ->` now exists under a new id.
    // Without the last one parse_relation kept answering from the stale set,
    // and every rebuilt fact rendered as "??" although its id and its edges
    // were already correct.
    invalidate_fact_structures_cache();
    invalidate_relation_type_set();
    rebuild_rule_pattern_index();
    rebuild_refuted_index();
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
    if (is_condition_set(condition))
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

            // A rule's condition is a STATEMENT that has to hold, and neither
            // a bare variable nor a bare name is one. `=>` is an ordinary
            // relation type as well as the rule arrow, which is what makes
            // both reachable without anybody writing a rule:
            //
            //   * `S => O` -- asking which implications exist -- materializes
            //     that pattern, and it was counted by .stat and listed by
            //     .list-rules as a rule of the network, permanently;
            //   * `atom_A => atom_B` is a FACT (pinned by *parsing: arrow
            //     predicates*), and .remove-rules deleted it as if it were a
            //     rule -- a command that says it removes rules destroying
            //     data.
            //
            // Neither can fire: nothing binds a condition that is a variable,
            // and an atom is not something that holds. A conjunction of
            // conditions is a container with a counter id rather than a
            // triple hash, so it is admitted by its tag.
            const bool condition_is_statement =
                condition != 0
                && !Impl::is_var(condition)
                && (Impl::is_hash(condition) || is_condition_set(condition));

            // The consequence side asks the same question: a rule has to be
            // able to ASSERT something. A container cannot be asserted --
            // `(X p Y) => {(X q Y)}` was counted and listed and derived
            // nothing -- and neither can a bare name. `!` is the one atom
            // that can, and a statement is recognised by having a predicate,
            // which a container has not. ONE assertable consequence is
            // enough: a rule may carry several, and it is a rule if it can
            // derive at all.
            //
            // The RECORDED structure, not parse_relation: a consequence whose
            // predicate has lost its `~ ->` declaration is still a statement,
            // and deduce still builds it -- as does the renderer, from the
            // same source.
            const auto assertable = [this](const Node t)
            {
                if (t == core.Contradiction) return true;
                const FactStructure fs = get_preferred_structure(this, t, 3);
                return fs.subject != 0 && fs.predicate != 0;
            };

            const bool derives_something =
                std::any_of(deductions.begin(), deductions.end(), assertable);

            if (condition_is_statement && condition != core.Causes
                && derives_something
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

    // What nobody claimed does not belong in a slice. Read here for the same
    // reason as the rules: is_asserted_fact would take the pattern and
    // template-variable locks per candidate, and its fallback walks the
    // adjacency -- which this function already holds shared, and recursive
    // shared locking is undefined. get_fact_objects uses the snapshot the same
    // way, and for the same reason.
    const auto unasserted = unasserted_snapshot();

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

        // A contradiction record does NOT travel with a slice, and it is the
        // one thing the structural closure has to be told about. The record is
        // the refuted set of the facts that matched, so it is reached by
        // expanding any ONE of them -- and expanding it in turn drags in the
        // others, whatever predicate they belong to. A slice of `p` then held
        // `x q y` and answered `S q O` with it, which is exactly the promise
        // the slice makes and breaks: the facts of the named predicates, and
        // nothing else.
        //
        // Nothing is lost by leaving it behind. A record is derived from data
        // the slice either kept -- in which case its own first run finds the
        // contradiction and records it again -- or did not keep, in which case
        // the contradiction is not a property of the slice at all.
        // The marking fact goes with it. Keeping the record out of `keep`
        // while its `~ refuted` fact stays in is not half a record: the saver
        // writes the EDGES of what it keeps, so the loader rebuilds the set
        // node from them -- and with it the members of every predicate.
        // Read UNLOCKED, like exists_unlocked above: this function holds both
        // adjacency locks shared, and get_left would take one again --
        // recursive shared locking is undefined. It also copies the whole set,
        // which is the wrong thing to do once per retained node.
        //
        // The whole test is behind one atomic load, so a graph that refutes
        // nothing -- which is every graph without a contradiction rule, and
        // every one built before records existed -- pays that and no more.
        const bool any_refuted = has_refuted_facts();

        const auto refuted_or_its_marking = [this, any_refuted](const Node nd)
        {
            if (!any_refuted) return false;
            if (is_refuted_fact(nd)) return true;

            const auto it = _pImpl->_left.find(nd);
            if (it == _pImpl->_left.end()) return false;
            for (const Node candidate : it->second)
                if (is_refuted_fact(candidate)) return true;
            return false;
        };

        const auto retain = [&keep, &pending, &refuted_or_its_marking](const Node nd)
        {
            if (refuted_or_its_marking(nd)) return;
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

                // A query pattern is a fact of p in every structural sense and
                // an answer to nothing, so it must not travel. The is_var test
                // below was meant to keep it out and cannot: a fact ABOUT the
                // pattern -- the `closure` tag of `(S P279⁺ Q3)`, say -- stands
                // on both sides of it exactly as a subject does, so it passes
                // as the non-variable subject the pattern itself does not have.
                // The effect was a slice whose fact count depended on which
                // questions had been asked in the session that wrote it: two
                // real P279 facts and one pattern, reported as three, while
                // .list-predicate-usage in the same session said two.
                //
                // A rule's own patterns are not lost by this. They travel with
                // the rule, through get_rules above and the structural closure
                // below, which is what carries a rule whole.
                if (unasserted && unasserted->count(rel) != 0) continue;

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
            bool is_condition_set = false;
            for (const Node tag : {core.Conjunction, core.Negation})
            {
                const Node tagged = create_hash(core.IsA, nd, {tag});
                if (exists_unlocked(tagged))
                {
                    retain(tagged);
                    if (tag == core.Conjunction) is_condition_set = true;
                }
            }

            // ... and the relation-type declaration of every retained node,
            // for exactly the reason the named predicates get theirs above:
            // a fact structure is reconstructed only for a DECLARED
            // predicate. The rules travel whole (see the top of this
            // function), so a rule's patterns bring their predicate node
            // along -- but the predicate of a pattern is rarely one of the
            // sliced predicates, and its declaration stayed behind. The
            // effect was a rule that is structurally complete and cannot be
            // READ: ".save-predicates slice.bin p" over
            // "(a p b) => (c q d)" produced a slice whose .list-rules
            // answered "(a p b) => ??", and typing "q ~ ->" into the loaded
            // slice restored the line in full. The promise above -- that
            // such a rule "is complete and simply never matches" -- only
            // holds with this.
            //
            // Every retained node is asked, not just the predicates: a node
            // that IS a declared relation type in the source and is not one
            // in the slice makes the slice contradict itself about that
            // node. It costs one hash and one lookup per retained node, next
            // to the two the tags above already pay.
            const Node declaration = create_hash(core.IsA, nd, {core.RelationTypeCategory});
            if (exists_unlocked(declaration)) retain(declaration);

            // A condition set is expanded although it is not content-addressed:
            // its adjacency IS the rule's conditions, which is small and wanted,
            // where a plain node's adjacency is the rest of the graph. Without
            // this the conditions were reached only by accident -- the loop over
            // the sliced predicates retained them, because the set stands on
            // both sides of each of its members and so passed for their subject.
            // Once that accident was corrected the contradiction rule of
            // test_predicate_slice lost its conditions and stopped being a rule
            // at all, which is how the hole showed itself.
            if (!Impl::is_hash(nd) && !is_condition_set) continue;

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
    rebuild_refuted_index();
}

void Zelph::load_from_file(const std::string& filename, const BinChunkSelection& selection, const bool skip_payload) const
{
    invalidate_fact_structures_cache();

    _pImpl->loadFromFile(filename, selection, skip_payload);
    rebuild_rule_pattern_index();
    rebuild_refuted_index();
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
    rebuild_refuted_index();
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
size_t Zelph::drop_scratch_cluster(const std::string& name) const
{
    // A SCRATCH cluster is one the engine itself opened moments ago and is
    // about to throw away: zelph/dedup-rule building a rule that turns out to
    // exist, .explain evaluating its pattern read-only. Everything in it was
    // created by that construction, and nothing outside it can refer to any
    // of it yet.
    //
    // drop_cluster is the wrong instrument for that, for exactly the reason
    // unmark_rule_pattern gives: remove_node invalidates through the
    // WHOLESALE funnel, which DISARMS the genuine-structure store for the
    // rest of the session. Re-entering a rule that already exists was enough
    // -- one duplicate line took `genuine walks` from 0 to 12 on a four-fact
    // graph, and that store is what keeps get_fact_structures off the
    // adjacency walk (see the 3 August regression, where losing it tripled
    // the Jacobian workload).
    //
    // So: verify that every node is unreferenced from outside, then remove
    // each with the invalidation a fact() of the same shape performs. Any
    // node that fails the test sends the WHOLE cluster down the general path
    // -- this is an optimisation of a removal, never a weakening of one.
    const std::vector<Node> nodes = _pImpl->cluster_nodes(name);
    if (nodes.empty())
    {
        _pImpl->take_cluster(name); // the bookkeeping goes either way
        return 0;
    }

    const std::unordered_set<Node> doomed(nodes.begin(), nodes.end());

    // Decompose WHILE THE EDGES STILL HOLD -- afterwards there is nothing
    // left to read the triple from.
    std::vector<std::pair<Node, FactStructure>> structures;
    structures.reserve(nodes.size());

    for (const Node n : nodes)
    {
        if (!_pImpl->exists(n)) continue;

        const FactStructure fs = get_preferred_structure(this, n, 3);

        const auto is_component = [&fs](const Node m)
        { return m == fs.subject || m == fs.predicate || fs.objects.count(m) != 0; };

        // A neighbour that is neither doomed nor a part of this node is
        // something that was built ON it, or something that mentions it --
        // and then the removal has a cascade to run.
        for (const adjacency_set& side : {_pImpl->get_left(n), _pImpl->get_right(n)})
        {
            for (const Node m : side)
            {
                if (doomed.count(m) == 0 && !is_component(m))
                    return drop_cluster(name);
            }
        }

        structures.emplace_back(n, fs);
    }

    _pImpl->take_cluster(name);

    size_t removed = 0;
    for (const auto& [n, fs] : structures)
    {
        if (!_pImpl->exists(n)) continue;

        if (fs.subject != 0 && fs.predicate != 0)
            invalidate_fact_structures_for(fs.subject, fs.predicate, fs.objects, n);

        _pImpl->remove(n);
        _pImpl->remove_node_names(n);
        ++removed;
    }

    return removed;
}

size_t Zelph::drop_cluster(const std::string& name) const
{
    // Read before take_cluster, which drops both sets.
    const std::vector<Node> unmarked = _pImpl->cluster_unmarked(name);
    const std::vector<Node> nodes    = _pImpl->take_cluster(name);
    if (nodes.empty() && unmarked.empty()) return 0;

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

    restore_rule_patterns(unmarked);

    return removed;
}

// Re-mark the patterns whose marking the dropped cluster revoked. Runs AFTER
// the removals, because a pattern the drop took with it is not to be marked
// again -- and it may well have been taken: a rule built inside the cluster
// records its patterns, and asserting one of them inside the same cluster
// revokes the marking of a node the cluster itself created.
void Zelph::restore_rule_patterns(const std::vector<Node>& patterns) const
{
    if (patterns.empty()) return;

    const Node pred = rule_pattern_predicate(true);
    auto*      self = const_cast<Zelph*>(this);

    std::unique_lock lock(_pImpl->_rule_patterns_mtx);
    for (const Node p : patterns)
    {
        if (!_pImpl->exists(p)) continue;

        // Marking twice is not the same as marking once: the fact is
        // hash-consed, so re-creating it is a no-op, but the index is a set
        // and would grow. insert answers both questions at once.
        self->fact(p, core.IsA, {pred});
        _pImpl->_rule_patterns.insert(p);
    }
    _pImpl->_has_rule_patterns.store(!_pImpl->_rule_patterns.empty(), std::memory_order_release);
}