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
#include "rule_identity.hpp"

#include "zelph_impl.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    //
    // The same holds for the node that says how a Wikidata import named its
    // items. It possesses no edges either, and the published networks are
    // cut by a recipe concluding with .cleanup.
    const Node labels_marker = get_node(wikidata_labels_marker_name(), "zelph", true);
    _pImpl->remove_isolated_nodes(removed_count,
                                  [this, labels_marker](const Node nd)
                                  { return (labels_marker != 0 && nd == labels_marker) || !get_core_name(nd).empty(); });
}

size_t Zelph::cleanup_names() const
{
    return _pImpl->cleanup_dangling_names();
}

// Delete the names associated with an entire group of eliminated
// nodes -- the ones gathered by `remove_node`'s `deferred_names`.
void Zelph::remove_names_of(const adjacency_set& dead) const
{
    _pImpl->remove_names_of(dead);
}

// How frequently the reverse name map was traversed from end to end. A
// removal finds a node's reverse entries by means of its forward ones and
// performs no traversal; only a session whose name sections were partially
// loaded continues to traverse, once per node or batch, and that is what
// this metric tracks. Independent of hardware, enabling a test to maintain
// that shape even without a quiet machine.
uint64_t Zelph::name_map_scans() const
{
    return _pImpl->_name_map_scans.load(std::memory_order_relaxed);
}

// `deferred_names`, when given, COLLECTS the removed nodes instead of having
// their names erased here, and the caller then owes one `remove_names_of` for
// the whole batch.
//
// Erasing a node's names requires a single lookup per language, regardless of
// approach. The batch becomes significant when a removal still needs to walk
// the reverse name map following a partial load: one walk per command rather
// than one per node. On the full Wikidata network, that map contains
// approximately 204 million entries, and the prior approach of walking per
// node limited the full-dump prune run to 1.11 nodes per second. A label that
// several victims carry is settled one time, not one time for each victim.
// Everything that removing `node` would take with it: the node itself, whatever
// it is a PART of (transitively), and a container that loses a member. Adds to
// `out` and explores nothing `out` already holds, so a caller may accumulate
// several victims into one set and a node reached twice is walked once.
//
// A node listed in `keep` is never doomed, and the walk stops at it:
// Reasoning::withdraw_claims passes the parts of each rule, which remain
// after the removal of the rule pattern's holders.
//
// A membership listed in `added` is removed without its container:
// drop_cluster passes what the dropped cluster created, and a member that the
// cluster added to an older collection leaves that collection exactly as it
// existed before the cluster.
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
void Zelph::collect_doomed(const Node node, adjacency_set& out, const adjacency_set* const keep, const adjacency_set* const added) const
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
        if (keep != nullptr && keep->count(candidate) != 0) return;

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
        const Node relation = parse_relation(current);
        if (relation == core.PartOf)
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
            //
            // Nor was a member that a dropped cluster added: the container
            // existed without it before the cluster, and that is the set
            // the drop returns. Dooming it took `k has @{a b}` with the
            // membership `c in @{a b}` that the cluster had written.
            const bool added_here = added != nullptr && added->count(current) != 0;
            if (!Zelph::is_var(member) && !added_here)
            {
                for (const Node container : containers)
                {
                    // Nor does a statement holding a variable qualify as a
                    // member of a set constant (Zelph::is_pattern_member):
                    // `.remove X` following the rule `((X p z) in {(x p z) c})
                    // => (X ok yes)` took the set and every statement
                    // regarding it.
                    if (Zelph::is_hash(container) && Zelph::is_hash(member) && is_pattern_member(member) && is_set_constant(container)) continue;
                    doom(container);
                }
            }
            // The container stays, and when it is either a rule's own
            // collection or a conjunction set, the rule governing it says
            // something else henceforth: its fingerprint and associated
            // claim are stale (see the fingerprint index).
            else if (std::any_of(containers.begin(), containers.end(), [this](const Node c)
                                 { return holds_rule_text(c); }))
            {
                note_rule_text_changed();
            }
        }
        // A conjunction tag functions as rule text, and the rule remains
        // without it: a condition set turns into a collection.
        else if (relation == core.IsA)
        {
            adjacency_set tags;
            parse_fact(current, tags, 0);
            if (tags.count(core.Conjunction) != 0) note_rule_text_changed();
        }
    }
}

void Zelph::announce_removal(const adjacency_set& removed, const bool derived) const
{
    if (_on_removal && (derived || !removed.empty())) _on_removal(removed, derived);
}

// A recorded set pattern leaves the record along with its node. The
// counter id is never restored, thus this keeps the record at minimal
// size and does nothing else.
void Zelph::forget_set_patterns(const adjacency_set& gone) const
{
    if (!_pImpl->_has_set_patterns.load(std::memory_order_acquire)) return;

    std::unique_lock lock(_pImpl->_set_patterns_mtx);
    for (const Node nd : gone)
        _pImpl->_set_patterns.erase(nd);
    _pImpl->_has_set_patterns.store(!_pImpl->_set_patterns.empty(), std::memory_order_release);
}

// Erase what collect_doomed found, with the cache invalidation and the
// relation-type memo that implies. The serial half of a removal. A cluster
// drop passes `restored`: its journal restores what the refutation of a
// removed marking changed (forget_refuted).
void Zelph::remove_doomed(const adjacency_set& doomed, adjacency_set* const deferred_names, const bool derived, const bool restored) const
{
    refuse_opacity_removal([&doomed](const Node nd)
                           { return doomed.count(nd) != 0; });
    announce_removal(doomed, derived);

    // A refuted node that is being removed must leave the index with its
    // marking fact, and this is the one index where a stale entry is not
    // merely wasteful. A contradiction record is a SET CONSTANT, so it is
    // content-addressed: remove one of the facts it is about, enter that fact
    // again, and the record comes back with the SAME id. A stale entry then
    // says "already known" about a contradiction whose record the graph no
    // longer holds, and the report never returns.
    forget_refuted(doomed, restored);

    // The same applies to a rule pattern that has been removed. A fact is
    // identified by the hash of its triple, meaning that where its components
    // outlive it, entering or deriving the same statement again recreates the
    // identical identifier: once a dropped cluster had contained a mention of
    // `t p k`, or a refutation that had created it, a rule that derived
    // `t p k` wrote a pattern, which no query answered until a later run
    // claimed it.
    forget_rule_patterns(doomed);

    forget_set_patterns(doomed);

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

    // The predicates whose index becomes outdated due to the removal, read
    // while the edges still exist; consulted only when an index is built or
    // when sidecars are active.
    const bool        indexed = _pImpl->predicate_index_in_use();
    std::vector<Node> predicates;

    for (const Node dead : doomed)
    {
        if (!declaration_removed && is_relation_type_declaration(dead)) declaration_removed = true;

        if (indexed)
            if (const Node p = predicate_of(dead); p != 0) predicates.push_back(p);

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
        for (const Node p : predicates)
            _pImpl->invalidate_predicate_index(p);
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
// The corner this cannot reach is a rule that is simultaneously asserted and
// mentioned as a single node. A GROUND rule constitutes one node regardless
// of where its text appears -- hash-consing ensures this -- unless that text
// holds a comma list of conditions or a collection of the rule's own, each of
// which is constructed anew per statement; a rule involving variables counts
// as two, since each statement names its own variables. A statement that a
// rule derives from the parts of an asserted rule, however, mentions that
// rule's own node irrespective of what its text holds, and a rule operating
// on rules can bind any rule the engine reads as data: specifically, one
// where var_in_closure finds no variable. A script or program holding a
// rule's node -- zelph/fact in Janet, zelph_fact in the C ABI -- names that
// node directly.
//
// A rule that is a mention in its own right -- one returned by
// zelph/mention or zelph_mention, or one held by an opaque argument -- is
// named by its rule-pattern mark (mark_mention), a statement like any
// other, which keeps it out of force once its holder has ceased to exist;
// a claim revokes the mark.
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

constexpr const char* kRefutedFactName = "refuted";
constexpr const char* kOpaqueName      = "opaque";

namespace
{
    constexpr const char* rule_pattern_name = "rule pattern";

    std::atomic<uint64_t> claim_steps{0}; // refer to Zelph::claim_walk_steps
}

// The named nodes that the engine itself reads; Zelph::node creates them
// outside the cluster recording. Only within the language zelph: in any
// different language, the identical spelling corresponds to an ordinary
// content node.
Zelph::Vocabulary Zelph::engine_vocabulary(const std::string& name, const std::string& lang)
{
    if (lang != "zelph") return Vocabulary::none;
    for (const char* const predicate : {"closure", "nn", "nn-layers"})
        if (name == predicate) return Vocabulary::predicate;
    for (const char* const node : {kRefutedFactName, kOpaqueName, rule_pattern_name, "one-or-more", "zero-or-more"})
        if (name == node) return Vocabulary::node;
    return name == wikidata_labels_marker_name() ? Vocabulary::node : Vocabulary::none;
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

std::vector<Node> Zelph::rule_patterns_snapshot() const
{
    if (!_pImpl->_has_rule_patterns.load(std::memory_order_acquire)) return {};

    std::shared_lock lock(_pImpl->_rule_patterns_mtx);
    return {_pImpl->_rule_patterns.begin(), _pImpl->_rule_patterns.end()};
}

void Zelph::forget_rule_patterns(const adjacency_set& gone) const
{
    if (!_pImpl->_has_rule_patterns.load(std::memory_order_acquire)) return;

    std::unique_lock lock(_pImpl->_rule_patterns_mtx);
    for (const Node nd : gone)
        _pImpl->_rule_patterns.erase(nd);
    _pImpl->_has_rule_patterns.store(!_pImpl->_rule_patterns.empty(), std::memory_order_release);
}

bool Zelph::is_older_rule_literal(const Node membership) const
{
    return _pImpl->_older_rule_literals.count(membership) != 0;
}

bool Zelph::is_refuted_fact(const Node node) const
{
    if (!_pImpl->_has_refuted_facts.load(std::memory_order_acquire)) return false;

    std::shared_lock lock(_pImpl->_refuted_facts_mtx);
    return _pImpl->_refuted_facts.count(node) != 0;
}

// Distinguished solely by structure, ensuring that a network loaded from a
// file is interpreted identically: the indexes that a load rebuilds (the
// rule patterns and the refuted facts) and the statements that name a
// record are the only elements it consults. A contradiction record
// constitutes a refuted set constant -- what count_contradiction_records
// counts -- whereas the refutation marking a user writes, `(F) ~ refuted`,
// names a fact: it functions as a statement, and a condition `(F) ~ refuted`
// keeps matching it. The subject of a mark is a rule pattern, and the
// subject of a record is refuted, hence a probe of either index precedes the
// lookup of the predicate's name.
//
// A rule's text does not serve bookkeeping purposes: what a rule over rules
// interprets (is_rule_text_fact) is the tag `~ conjunction` and the
// memberships within a condition set. Likewise, the membership of a set that a
// statement refers to is not bookkeeping: `{(x p y) (x q y)} label foo` keeps
// answering `S in O` even after a contradiction has recorded the same two
// facts.
bool Zelph::is_bookkeeping(const Node subject, const Node predicate, const adjacency_set& objects) const
{
    const auto is_record = [this](const Node n)
    { return is_refuted_fact(n) && is_set_constant(n); };

    if (predicate == core.PartOf)
        return std::any_of(objects.begin(), objects.end(), [&](const Node container)
                           { return is_bare_record(container); });

    if (predicate != core.IsA || objects.size() != 1) return false;
    const Node object = *objects.begin();
    if (subject == 0 || Impl::is_var(subject) || !Impl::is_hash(subject)) return false;
    if (is_rule_pattern(subject) && object == rule_pattern_predicate(false)) return true;
    return is_record(subject) && object == const_cast<Zelph*>(this)->get_node(kRefutedFactName, "zelph");
}

bool Zelph::is_bookkeeping(const Node fact) const
{
    const Node predicate = predicate_of(fact);
    if (predicate != core.IsA && predicate != core.PartOf) return false;
    adjacency_set objects;
    const Node    subject = parse_fact(fact, objects);
    return subject != 0 && is_bookkeeping(subject, predicate, objects);
}

// A set constant equals its content, meaning that a record and a set authored
// by a user based on identical facts constitute a single node. What tells them
// apart is what names the node: the record's intrinsic statements are its
// marking and the memberships forming the set. A pattern holding a variable
// does not name the set in this way, or a query that mentions the set would
// convert the set's memberships into data.
bool Zelph::is_bare_record(const Node set) const
{
    adjacency_set members;
    if (!is_refuted_fact(set) || !set_constant_members(set, members)) return false;

    // The memberships forming the set, each individual member within the set
    // alone, as set() writes them. A membership that names a second container
    // beside the set, `(x p y) in bag {(x p y) (x q y)}`, constitutes a
    // statement someone wrote.
    adjacency_set own;
    for (const Node member : members)
        own.insert(create_hash(core.PartOf, member, {set}));

    const Node    marking    = create_hash(core.IsA, set, {const_cast<Zelph*>(this)->get_node(kRefutedFactName, "zelph")});
    adjacency_set statements = _pImpl->get_right(set);
    for (const Node nd : _pImpl->get_left(set))
        statements.insert(nd);

    for (const Node statement : statements)
    {
        if (statement == marking || own.count(statement) != 0) continue;
        if (!var_in_closure(statement)) return false;
    }
    return true;
}

// A statement that names a refuted set constant makes the set its own
// (is_bare_record): starting at that moment, the set's memberships are
// treated as data, yet they predate the statement, meaning nothing announced
// them, and semi-naive seeding never offered them to a rule whose conditions
// they now meet -- `.run-delta` following `{(x p y) (x q y)} label foo`
// missed what `(M in C) => (M member yes)` derives from them. They are
// announced here as the facts they have become, akin to a claimed rule
// pattern (unmark_rule_pattern). A set that a statement named before incurs
// seeds that derive nothing new. Without refutations, a newly created fact
// incurs a single atomic load, whereas with refutations, a probe of the
// refuted index is required for each component that functions as a hash.
void Zelph::announce_named_records(const Node statement, const Node subject, const Node predicate, const adjacency_set& objects) const
{
    if (!_on_fact_created || !_pImpl->_has_refuted_facts.load(std::memory_order_acquire)) return;

    const auto announce = [&](const Node set)
    {
        if (!Impl::is_hash(set) || !is_refuted_fact(set)) return;
        if (var_in_closure(statement)) return; // a pattern names no set (is_bare_record)
        if (predicate == core.IsA && subject == set && objects.size() == 1
            && *objects.begin() == const_cast<Zelph*>(this)->get_node(kRefutedFactName, "zelph"))
            return; // its marking
        adjacency_set members;
        if (!set_constant_members(set, members)) return;
        for (const Node member : members)
            _on_fact_created(create_hash(core.PartOf, member, {set}), core.PartOf);
    };

    announce(subject);
    announce(predicate);
    for (const Node object : objects)
        announce(object);
}

// Read off the two indexes instead of the extent of the predicate: the
// marks represent the facts `(P) ~ "rule pattern"` associated with rule
// patterns, the record markings denote those `(S) ~ refuted` where the
// subject is a set constant, and the memberships of a bare record
// constitute the facts forming its set. Each is addressed by its content
// hash, which also refers to a fact that is missing; a closure that
// skips it loses nothing.
std::shared_ptr<const adjacency_set> Zelph::bookkeeping_facts(const Node predicate) const
{
    if (predicate != core.IsA && predicate != core.PartOf) return nullptr;

    auto                    out     = std::make_shared<adjacency_set>();
    const std::vector<Node> refuted = refuted_facts_snapshot();
    if (predicate == core.IsA)
    {
        const Node mark = _pImpl->_has_rule_patterns.load(std::memory_order_acquire) ? rule_pattern_predicate(false) : 0;
        if (mark != 0)
        {
            std::shared_lock lock(_pImpl->_rule_patterns_mtx);
            for (const Node pattern : _pImpl->_rule_patterns)
                out->insert(create_hash(core.IsA, pattern, {mark}));
        }

        const Node marking = refuted.empty() ? 0 : const_cast<Zelph*>(this)->get_node(kRefutedFactName, "zelph");
        for (const Node r : refuted)
            if (is_set_constant(r)) out->insert(create_hash(core.IsA, r, {marking}));
    }
    else
    {
        for (const Node r : refuted)
        {
            adjacency_set members;
            if (!is_bare_record(r) || !set_constant_members(r, members)) continue;
            for (const Node member : members)
                out->insert(create_hash(core.PartOf, member, {r}));
        }
    }

    if (out->empty()) return nullptr;
    return out;
}

bool Zelph::is_rule_text_fact(const Node subject, const Node predicate, const adjacency_set& objects) const
{
    if (predicate == core.IsA) return subject != 0 && objects.size() == 1 && *objects.begin() == core.Conjunction;
    if (predicate != core.PartOf) return false;
    return std::any_of(objects.begin(), objects.end(), [this](const Node container)
                       { return holds_rule_text(container); });
}

bool Zelph::prune_scan() const
{
    return _pImpl->_prune_scan.load(std::memory_order_relaxed);
}

void Zelph::set_prune_scan(const bool on) const
{
    _pImpl->_prune_scan.store(on, std::memory_order_relaxed);
}

bool Zelph::is_rule_text_fact(const Node fact) const
{
    const Node predicate = predicate_of(fact);
    if (predicate != core.IsA && predicate != core.PartOf) return false;
    adjacency_set objects;
    const Node    subject = parse_fact(fact, objects);
    return subject != 0 && is_rule_text_fact(subject, predicate, objects);
}

const char* Zelph::refuted_fact_name()
{
    return kRefutedFactName;
}

// The index half of a refutation, without the marking fact -- for the two
// callers that already have one: mark_refuted_fact, which is about to write
// it, and Zelph::fact, which has just seen one written by hand.
void Zelph::note_refuted(const Node node) const
{
    if (node == 0) return;

    invalidate_predicate_index_of(node);

    std::unique_lock lock(_pImpl->_refuted_facts_mtx);
    _pImpl->_refuted_facts.insert(node);
    _pImpl->_has_refuted_facts.store(true, std::memory_order_release);
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
    const Node pred = self->node(kRefutedFactName, "zelph");

    // The marking constitutes a claim made here, regardless of prior
    // existence: one that a rule wrote as its pattern becomes claimed, and
    // the refutation is applied even when Zelph::fact holds a new marking
    // back, within a rule's construction or a pattern scope.
    const Node marking = self->fact(node, core.IsA, {pred});
    unmark_rule_pattern(marking);
    apply_refutation(node);
}

void Zelph::apply_refutation(const Node fact) const
{
    if (fact == 0) return;

    // The probability half, which is what the refusals of Zelph::fact and
    // deduce's check of a derived fact read: lacking it, a marking hid F
    // from every query, whereas "mary sleeps nil" entered afterwards was
    // permitted and absorbed without comment, while after "¬(mary sleeps
    // nil)" it is refused. A contradiction record is a set constant and has
    // no edge to weight, and a fact holding a variable is a pattern.
    const Node predicate = predicate_of(fact);
    const bool weighted  = predicate != 0 && !is_set_constant(fact) && !var_in_closure(fact);

    // Before either half changes, the active cluster journals its findings
    // (Network::note_refutation), ensuring that dropping it takes the
    // refutation back from a fact it did not create (restore_refutations).
    _pImpl->note_refutation(fact, weighted ? predicate : 0, is_refuted_fact(fact));

    note_refuted(fact);

    if (!weighted) return;
    _pImpl->set_edge_weight(fact, predicate, 0.0);
}

Node Zelph::refuted_marking_subject(const Node marking) const
{
    if (marking == 0 || predicate_of(marking) != core.IsA) return 0;

    const Node refuted = const_cast<Zelph*>(this)->get_node(kRefutedFactName, "zelph");
    if (refuted == 0) return 0;

    adjacency_set objects;
    const Node    subject = parse_fact(marking, objects);
    return objects.size() == 1 && *objects.begin() == refuted && subject != refuted ? subject : 0;
}

bool Zelph::has_refuted_facts() const
{
    return _pImpl->_has_refuted_facts.load(std::memory_order_acquire);
}

std::vector<Node> Zelph::refuted_facts_snapshot() const
{
    if (!_pImpl->_has_refuted_facts.load(std::memory_order_acquire)) return {};

    std::shared_lock lock(_pImpl->_refuted_facts_mtx);
    return {_pImpl->_refuted_facts.begin(), _pImpl->_refuted_facts.end()};
}

void Zelph::forget_refuted(const adjacency_set& gone, const bool restored) const
{
    if (!_pImpl->_has_refuted_facts.load(std::memory_order_acquire)) return;

    // The index caches the markings, meaning a marking that has been
    // removed ends the refutation of its subject, which itself stays. A
    // query that failed, where the scratch held `(john waits nil) ~
    // refuted`, removed the marking and kept the entry: `S waits O` said
    // nothing about a fact that no one had refuted, until a .save and a .load
    // rebuilt the index from the markings. The subject is a neighbour of the
    // marking whose triple hashes to the marking's id, which settles the
    // reading without requiring a reconstruction. Read before acquiring the
    // lock; see refuted_facts_snapshot.
    std::vector<Node> subjects;
    if (const Node pred = get_node(kRefutedFactName, "zelph"); pred != 0)
    {
        for (const Node nd : gone)
        {
            if (!Impl::is_hash(nd) || Impl::is_var(nd) || !_pImpl->has_left_edge(nd, pred)) continue;
            for (const Node f : _pImpl->get_right(nd))
            {
                if (Impl::create_hash(core.IsA, f, pred) == nd && is_refuted_fact(f)) subjects.push_back(f);
            }
        }
    }

    for (const Node f : subjects)
        invalidate_predicate_index_of(f);

    // The 0 that the refutation wrote on the edge to the predicate goes too,
    // meaning the fact is in the state the refutation found it in. It stayed
    // behind: after the marking was pruned or removed, a fact that a rule had
    // only written down read as asserted and fired the rule, whereas a later
    // assertion of it was refused as known to be wrong. A cluster drop
    // restores the state its journal recorded instead (restore_refutations),
    // encompassing a weight that existed previously.
    if (!restored)
    {
        for (const Node f : subjects)
        {
            const Node predicate = predicate_of(f);
            if (predicate != 0 && _pImpl->has_synapse(f, predicate) && _pImpl->edge_weight(f, predicate) == 0.0) _pImpl->erase_weight(f, predicate);
        }
    }

    std::unique_lock lock(_pImpl->_refuted_facts_mtx);
    for (const Node nd : gone)
        _pImpl->_refuted_facts.erase(nd);
    for (const Node f : subjects)
        _pImpl->_refuted_facts.erase(f);
    _pImpl->_has_refuted_facts.store(!_pImpl->_refuted_facts.empty(), std::memory_order_release);
}

// A statement that joins or leaves the facts a closure skips (the rule
// patterns and the refuted facts, see unasserted_snapshot) changes what the
// index associated with its predicate holds, even though no fact was created
// or removed.
void Zelph::invalidate_predicate_index_of(const Node fact) const
{
    if (!_pImpl->predicate_index_in_use()) return;
    _pImpl->invalidate_predicate_index(predicate_of(fact));
}

const char* Zelph::opaque_name()
{
    return kOpaqueName;
}

bool Zelph::has_opaque_predicates() const
{
    return _pImpl->_has_opaque_predicates.load(std::memory_order_acquire);
}

bool Zelph::is_opaque_predicate(const Node predicate) const
{
    if (!_pImpl->_has_opaque_predicates.load(std::memory_order_acquire) || predicate == 0) return false;

    {
        std::shared_lock lock(_pImpl->_opaque_mtx);
        if (_pImpl->_opaque_predicates.count(predicate) == 0) return false;
    }

    // An entry remains even after its declaration was removed or became a
    // pattern again, thus the declaration is read: removals and restored
    // marks do not require a hook.
    const Node opaque = get_node(kOpaqueName, "zelph");
    if (opaque == 0) return false;
    const Answer declared = check_fact(predicate, core.IsA, {opaque});
    return declared.is_correct() && !is_rule_pattern(declared.relation());
}

Node Zelph::opaque_declaration_subject(const Node fact) const
{
    if (fact == 0 || !Impl::is_hash(fact) || Impl::is_var(fact) || predicate_of(fact) != core.IsA) return 0;

    const Node opaque = get_node(kOpaqueName, "zelph");
    if (opaque == 0) return 0;

    adjacency_set objects;
    const Node    subject = parse_fact(fact, objects);
    if (subject == 0 || Impl::is_var(subject) || var_in_closure(subject) || objects.size() != 1 || *objects.begin() != opaque) return 0;
    return subject;
}

Node Zelph::fact_holding_statement(const Node predicate, const std::function<bool(Node)>& ignore) const
{
    // A statement held is a fact, or a set constant whose members hold one.
    // A node that is not a hash is not read: the adjacency of an atom
    // comprises every fact regarding it, which on a hub constitutes most of
    // the graph.
    const auto holds_statement = [this](const Node start)
    {
        std::vector<Node>        pending{start};
        std::unordered_set<Node> seen;
        while (!pending.empty())
        {
            const Node nd = pending.back();
            pending.pop_back();
            if (nd == 0 || Impl::is_var(nd) || !Impl::is_hash(nd) || !seen.insert(nd).second) continue;
            if (predicate_of(nd) != 0) return true;
            if (!is_set_constant(nd)) continue;
            for (const Node rel : _pImpl->get_right(nd))
            {
                if (predicate_of(rel) != core.PartOf) continue;
                adjacency_set containers;
                const Node    member = parse_fact(rel, containers, 0);
                if (member != 0 && containers.count(nd) == 1) pending.push_back(member);
            }
        }
        return false;
    };

    for (const Node fact : _pImpl->get_left(predicate))
    {
        if (fact == predicate || !Impl::is_hash(fact) || Impl::is_var(fact) || (ignore && ignore(fact))) continue;
        if (predicate_of(fact) != predicate || is_rule_pattern(fact)) continue;

        adjacency_set objects;
        const Node    subject = parse_fact(fact, objects);
        if (subject == 0 || subject == predicate) continue;
        if (holds_statement(subject)) return fact;
        for (const Node o : objects)
            if (holds_statement(o)) return fact;
    }
    return 0;
}

void Zelph::refuse_opacity_removal(const std::function<bool(Node)>& doomed) const
{
    if (!_pImpl->_has_opaque_predicates.load(std::memory_order_acquire)) return;

    std::vector<Node> declared;
    {
        std::shared_lock lock(_pImpl->_opaque_mtx);
        declared.assign(_pImpl->_opaque_predicates.begin(), _pImpl->_opaque_predicates.end());
    }

    const Node opaque = get_node(kOpaqueName, "zelph");
    if (opaque == 0) return;
    for (const Node predicate : declared)
    {
        if (doomed(predicate)) continue;
        const Node declaration = Impl::create_hash(core.IsA, predicate, opaque);
        if (!doomed(declaration) || !is_opaque_predicate(predicate)) continue;
        refuse_opacity_change(predicate, true, doomed);
    }
}

void Zelph::refuse_opaque_merge(const Node from, const Node into) const
{
    // The survivor carries the declarations from both nodes, meaning that
    // merging a declared predicate with one that lacks a declaration renders
    // the latter opaque. A fact of it then keeps a claimed statement within an
    // opaque argument, the state the declaration refusal exists to prevent,
    // and the declaration could not be removed anymore. The `opaque` node
    // does not merge with any other node (resolve_name_conflict_locked).
    if (_pImpl->_has_opaque_predicates.load(std::memory_order_acquire))
    {
        const bool from_opaque = is_opaque_predicate(from);
        if (from_opaque != is_opaque_predicate(into)) refuse_opacity_change(from_opaque ? into : from, false);
    }
}

void Zelph::note_opaque(const Node predicate) const
{
    if (predicate == 0) return;
    std::unique_lock lock(_pImpl->_opaque_mtx);
    _pImpl->_opaque_predicates.insert(predicate);
    _pImpl->_has_opaque_predicates.store(true, std::memory_order_release);
}

void Zelph::rebuild_opaque_index() const
{
    {
        std::unique_lock lock(_pImpl->_opaque_mtx);
        _pImpl->_opaque_predicates.clear();
        _pImpl->_has_opaque_predicates.store(false, std::memory_order_release);
    }

    const Node opaque = get_node(kOpaqueName, "zelph");
    if (opaque == 0) return;

    // Only the claimed declarations; the pattern index is rebuilt first at
    // the call site, just as for the refuted index.
    std::vector<Node> declared;
    for (const Node declaration : _pImpl->get_right(opaque))
    {
        const Node predicate = opaque_declaration_subject(declaration);
        if (predicate != 0 && !is_rule_pattern(declaration) && check_fact(declaration).is_correct()) declared.push_back(predicate);
    }

    std::unique_lock lock(_pImpl->_opaque_mtx);
    _pImpl->_opaque_predicates.insert(declared.begin(), declared.end());
    _pImpl->_has_opaque_predicates.store(!_pImpl->_opaque_predicates.empty(), std::memory_order_release);
}

void Zelph::rebuild_refuted_index() const
{
    {
        std::unique_lock lock(_pImpl->_refuted_facts_mtx);
        _pImpl->_refuted_facts.clear();
        _pImpl->_has_refuted_facts.store(false, std::memory_order_release);
    }

    auto*      self = const_cast<Zelph*>(this);
    const Node pred = self->get_node(kRefutedFactName, "zelph");
    if (pred == 0) return;

    // The subject to which each marking PARSES, as the session's
    // Zelph::fact takes it, and exclusively from a claimed marking.
    // get_sources answers every left neighbour of a marking fact, and a fact
    // whose subject is the marking is among them: after a save and a load,
    // "((john waits nil) ~ refuted) => (mary sleeps nil)" listed as the
    // refuted rule "¬(...)", while "((a p b) ~ refuted) noted-by sue"
    // stopped answering. A marking that serves solely as a rule's pattern
    // refutes nothing (the pattern index is rebuilt first at every call
    // site). Gathered before the index lock is acquired: parsing reads the
    // graph, and a graph writer takes that lock while holding the network.
    std::vector<Node> subjects;
    for (const Node marking : _pImpl->get_right(pred))
    {
        if (_pImpl->get_right(marking).count(core.IsA) != 1 || is_rule_pattern(marking)) continue;
        const Node subject = refuted_marking_subject(marking);
        if (subject == 0 || Impl::is_var(subject)) continue;

        // A refutation that was applied left a 0 on the edge of the fact
        // (apply_refutation), and a claimed fact is never refuted. A marking
        // that a query wrote next to a fact it claims -- the condition within
        // a rule the query mentions, an argument of an opaque predicate -- was
        // never applied during the session, and here it took the claimed fact
        // from every query: following `bob waits nil` and the query
        // `S noted (((bob waits nil) ~ refuted) => (c d e))`, a .save and a
        // .load left `X waits nil` with no answer.
        if (predicate_of(subject) != 0 && !is_set_constant(subject) && !var_in_closure(subject) && check_fact(subject).is_correct()) continue;
        subjects.push_back(subject);
    }

    std::unique_lock lock(_pImpl->_refuted_facts_mtx);
    _pImpl->_refuted_facts.insert(subjects.begin(), subjects.end());
    _pImpl->_has_refuted_facts.store(!_pImpl->_refuted_facts.empty(), std::memory_order_release);
}

void Zelph::refuse_claim_of_pattern(const Node pattern) const
{
    // A declaration that a rule only wrote down becomes one when it is
    // claimed, and such a claim is refused, as its creation would be.
    if (const Node declared = opaque_declaration_subject(pattern); declared != 0) refuse_opacity_change(declared, false);

    // A marking functioning as a rule's pattern refutes its fact once it is
    // claimed (unmark_rule_pattern), and a fact the graph claims cannot be
    // refuted. A statement that held the marking through reference -- one a
    // Janet block or the C API had taken from a rule -- un-claimed the fact
    // without a word.
    const Node refuted = refuted_marking_subject(pattern);
    if (refuted != 0 && !var_in_closure(refuted) && predicate_of(refuted) != 0 && !is_set_constant(refuted)
        && check_fact(refuted).is_correct() && is_asserted_fact(refuted))
        throw std::runtime_error("fact(): this fact is known to be true");

    // A pattern or a mention that the graph refutes is a fact known to be
    // wrong, and claiming it is refused, as asserting it is. A statement that
    // held it by node -- a mention a Janet block built, the fact the focus of
    // a refutation stands for, what an inline keyword provided -- claimed it
    // and left it refuted at once, and once the refutation was withdrawn, it
    // answered and fired the rule whose pattern it was.
    if (predicate_of(pattern) != 0 && !is_set_constant(pattern) && check_fact(pattern).is_wrong())
        throw std::runtime_error("fact(): this fact is known to be wrong");
}

void Zelph::refuse_claim_of_marked_parts(const Node subject, const Node predicate, const adjacency_set& objects) const
{
    if (!_pImpl->_has_rule_patterns.load(std::memory_order_acquire)) return;

    // The walk of claim_marked_parts, without alteration: along the facts it
    // would claim, which are those that carry a mark, and past what it does
    // not enter. The statement used to be written first, and when the claim
    // was refused, it stayed: `x says ((a p b) ~ refuted)` persisted in the
    // graph even though its marking of a claimed `a p b` had been refused.
    const auto marked = [this](const Node x)
    { return x != 0 && !Impl::is_var(x) && Impl::is_hash(x) && is_rule_pattern(x); };

    std::vector<Node> pending;
    for (const Node x : {subject, predicate})
        if (marked(x)) pending.push_back(x);
    for (const Node o : objects)
        if (marked(o)) pending.push_back(o);
    if (pending.empty()) return;

    // Where claim_nested does not walk through the components of the
    // statement.
    if (predicate == core.Causes || is_opaque_predicate(predicate)) return;
    if (const Node refuted = get_node(kRefutedFactName, "zelph");
        refuted != 0 && predicate == core.IsA && objects.size() == 1 && *objects.begin() == refuted)
        return;

    std::unordered_set<Node> seen;
    while (!pending.empty())
    {
        const Node n = pending.back();
        pending.pop_back();
        if (!seen.insert(n).second || !marked(n)) continue;

        const FactStructure fs = get_preferred_structure(this, n, 1);
        if (fs.subject == 0) continue;
        if (fs.predicate == core.PartOf && std::any_of(fs.objects.begin(), fs.objects.end(), [this](const Node o)
                                                       { return holds_rule_text(o); }))
            continue;

        refuse_claim_of_pattern(n);

        if (fs.predicate == core.Causes || is_opaque_predicate(fs.predicate) || refuted_marking_subject(n) != 0) continue;
        pending.push_back(fs.subject);
        pending.push_back(fs.predicate);
        for (const Node o : fs.objects)
            pending.push_back(o);
    }
}

bool Zelph::unmark_rule_pattern(const Node node) const
{
    if (!_pImpl->_has_rule_patterns.load(std::memory_order_acquire) || !is_rule_pattern(node)) return false;

    // Refused before the mark goes, thus nothing has changed.
    refuse_claim_of_pattern(node);
    const Node declared = opaque_declaration_subject(node);

    {
        std::unique_lock lock(_pImpl->_rule_patterns_mtx);
        if (_pImpl->_rule_patterns.erase(node) == 0) return false;
        _pImpl->_has_rule_patterns.store(!_pImpl->_rule_patterns.empty(), std::memory_order_release);
    }

    const Node refuted = refuted_marking_subject(node);

    if (logging_active()) _pImpl->_rule_patterns_unmarked.fetch_add(1, std::memory_order_relaxed);

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
                announce_removal({marker});
                _pImpl->remove(marker);
                _pImpl->remove_node_names(marker);
            }
            else
            {
                const_cast<Zelph*>(this)->remove_node(marker);
            }
        }
    }

    // The index of the statement's predicate had bypassed the statement until
    // now; the marking's own predicate, invalidated by the removal above, is
    // `~`.
    invalidate_predicate_index_of(node);

    // A marking that served merely as a rule's pattern is now a refutation.
    if (refuted != 0 && !var_in_closure(refuted))
        apply_refutation(refuted);
    if (declared != 0) note_opaque(declared);

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

void Zelph::claim_nested(const Node statement, const ClaimReach& reach) const
{
    if (!_pImpl->_has_rule_patterns.load(std::memory_order_acquire)) return;

    // The genuine-structure store first, which the profiler excludes from
    // counting, so that a claim leaves the counters of a workload
    // unchanged; a fact missing from the store, along with a set constant,
    // is reconstructed.
    const auto structure = [this](const Node n) -> FactStructure
    {
        FactStructurePtr genuine;
        if (try_get_genuine_structure(n, genuine) && genuine && !genuine->empty()) return genuine->front();
        return get_preferred_structure(this, n, 1);
    };

    // The first step consists of the statement in question.
    struct Steps
    {
        uint64_t n{1};
        ~Steps() { claim_steps.fetch_add(n, std::memory_order_relaxed); }
    } steps;

    // A refutation marking `(F) ~ refuted` is claimed as a node, and its
    // subject is passed as an argument of an opaque predicate is: a refutation
    // says that F does not hold, thus claiming it claims nothing of F.
    // `¬(x says (a p b))` claimed `a p b`, and following `¬(F)` on a separate
    // line, F ceased to be a rule's pattern, thus read as asserted once the
    // refutation was withdrawn. The name is looked up once per walk, and
    // solely when a walked fact features the predicate `~` and a single
    // object.
    Node       refuted_node = 0;
    bool       looked_up    = false;
    const auto is_marking   = [&](const FactStructure& fs)
    {
        if (fs.predicate != core.IsA || fs.objects.size() != 1) return false;
        if (!looked_up)
        {
            refuted_node = get_node(kRefutedFactName, "zelph");
            looked_up    = true;
        }
        return refuted_node != 0 && *fs.objects.begin() == refuted_node;
    };

    const FactStructure top = structure(statement);
    if (top.subject == 0 || top.predicate == core.Causes || is_opaque_predicate(top.predicate) || is_marking(top)) return;

    // Only beneath a direct part that is to be entered or that carries a
    // mark does the walk have work.
    const auto needs_walk = [&](const Node n, const bool into)
    {
        if (n == 0 || Impl::is_var(n)) return false;
        const ClaimStep step = reach(n, into);
        return step == ClaimStep::Enter || (step == ClaimStep::Claim && Impl::is_hash(n) && is_rule_pattern(n));
    };
    bool needed = needs_walk(top.subject, false) || needs_walk(top.predicate, false);
    for (const Node o : top.objects)
        needed = needed || needs_walk(o, top.predicate == core.PartOf);
    if (!needed) return;

    std::vector<std::pair<Node, bool>> pending;
    const auto                         push_parts = [&](const FactStructure& fs)
    {
        pending.emplace_back(fs.subject, false);
        pending.emplace_back(fs.predicate, false);
        for (const Node o : fs.objects)
            pending.emplace_back(o, fs.predicate == core.PartOf);
    };
    push_parts(top);

    std::unordered_set<Node> seen{statement};
    while (!pending.empty() && _pImpl->_has_rule_patterns.load(std::memory_order_acquire))
    {
        const auto [n, into] = pending.back();
        pending.pop_back();
        ++steps.n;
        if (n == 0 || Impl::is_var(n) || !seen.insert(n).second) continue;

        const ClaimStep step = reach(n, into);
        if (step == ClaimStep::Pass) continue;

        if (Impl::is_hash(n))
        {
            const FactStructure fs = structure(n);
            if (fs.subject != 0)
            {
                // A membership in a rule's own collection or in its condition
                // set is rule text, identified by the id of what it is
                // written into, regardless of its position: a rule over rules
                // that binds a rule's consequence `c in @{d}` references it
                // within data, and when claimed, it answered beside the member
                // that the rule's firing wrote into the term.
                if (fs.predicate == core.PartOf && std::any_of(fs.objects.begin(), fs.objects.end(), [this](const Node o)
                                                               { return holds_rule_text(o); }))
                    continue;

                const bool marked = is_rule_pattern(n) && unmark_rule_pattern(n);

                // A rule that is mentioned is data the statement discusses,
                // and what that rule says is not claimed by saying it;
                // likewise the arguments of every other opaque predicate.
                if (fs.predicate == core.Causes || is_opaque_predicate(fs.predicate) || is_marking(fs)) continue;

                // Constructed with a rule's text, a marked fact can hold more
                // of it.
                if (marked || step == ClaimStep::Enter) push_parts(fs);
                continue;
            }
        }
        else if (is_rule_template(n))
        {
            continue;
        }

        // A set constant or a collection. The identifier decides before the
        // adjacency is read: an atom serves as the object of every fact
        // concerning it.
        if (step != ClaimStep::Enter) continue;
        for (const Node rel : get_right(n))
        {
            ++steps.n;
            if (predicate_of(rel) != core.PartOf) continue;
            adjacency_set objs;
            const Node    member = parse_fact(rel, objs, 0);
            if (member != 0 && objs.count(n) == 1) pending.emplace_back(member, false);
        }
    }
}

void Zelph::claim_marked_parts(const Node fact, const Node subject, const Node predicate, const adjacency_set& objects) const
{
    const auto marked = [this](const Node x)
    { return x != 0 && !Impl::is_var(x) && Impl::is_hash(x) && is_rule_pattern(x); };
    if (!marked(subject) && !marked(predicate) && std::none_of(objects.begin(), objects.end(), marked)) return;
    claim_nested(fact, [](Node, bool)
                 { return ClaimStep::Claim; });
}

uint64_t Zelph::rule_patterns_unmarked() const
{
    return _pImpl->_rule_patterns_unmarked.load(std::memory_order_relaxed);
}

void Zelph::reset_rule_patterns_unmarked() const
{
    _pImpl->_rule_patterns_unmarked.store(0, std::memory_order_relaxed);
}

uint64_t Zelph::claim_walk_steps()
{
    return claim_steps.load(std::memory_order_relaxed);
}

void Zelph::rebuild_rule_pattern_index() const
{
    {
        std::unique_lock lock(_pImpl->_rule_patterns_mtx);
        _pImpl->_rule_patterns.clear();
        _pImpl->_has_rule_patterns.store(false, std::memory_order_release);
    }

    const Node pred = rule_pattern_predicate(false);
    if (pred == 0) return;

    // For the reason given in `rebuild_refuted_index`, each marking's
    // parsed subject is taken: `get_sources` would also take a fact whose
    // subject is the marking fact itself.
    std::vector<Node> subjects;
    for (const Node marking : _pImpl->get_right(pred))
    {
        if (_pImpl->get_right(marking).count(core.IsA) != 1) continue;
        adjacency_set objects;
        const Node    subject = parse_fact(marking, objects);
        if (subject == 0 || subject == pred || Impl::is_var(subject) || objects.size() != 1 || *objects.begin() != pred) continue;
        subjects.push_back(subject);
    }

    std::unique_lock lock(_pImpl->_rule_patterns_mtx);
    _pImpl->_rule_patterns.insert(subjects.begin(), subjects.end());
    _pImpl->_has_rule_patterns.store(!_pImpl->_rule_patterns.empty(), std::memory_order_release);
}

// --- The fingerprint index of the rules -------------------------------------
// For every rule that is written (zelph/dedup-rule), it is determined whether
// a rule in force states the same as another rule. rule_shape
// serves as a fingerprint common to alpha-equivalent rules;
// rules_alpha_equivalent makes the decision. The index maps a fingerprint to
// the rules possessing it, enabling a lookup to select among candidates
// sharing the rule's fingerprint without consulting any other rule.
//
// A rule is fingerprinted once, at the moment it becomes complete: fact()
// queues every new `=>` fact, and a lookup applies a fingerprint to what has
// been queued before any other action. A fingerprint remains valid as long as
// the rule's text remains unchanged. Once a rule has been written, its text
// is no longer modified -- fact() blocks inclusion of a membership within a
// rule's own collection or conjunction set unless during the process of
// writing a rule, and a statement that binds one writes into its data term --
// thus, the text changes when note_rule_text_changed is called: a name merge,
// which rebuilds rules and merges the members of their templates
// (rehash_dependents); a variable's membership or a conjunction tag being
// excised from a rule's text (collect_doomed); and a load, which writes rules
// without invoking fact(). The subsequent lookup
// then clears the index and fingerprints every rule anew, operating at
// rule-scale, once per batch of such modifications. Unrecorded: a tag added
// to a condition afterwards, and a membership inserted into another rule's
// collection or conjunction set while the template scope remains open -- a
// Janet program that holds the node and writes into it while constructing a
// rule of its own -- which changes the other rule's text beneath its
// fingerprint. An entry for a rule that no longer exists is dropped by the
// lookup that encounters it; likewise, the rule whose ground member is
// removed is dropped, since removing a membership of a ground element dooms
// its container and anything built upon it.
//
// Each `=>` fact corresponds to a single entry: the index grows with the
// rules, not with the data.

void Zelph::note_rule_text_changed() const
{
    _pImpl->_rule_index_stale.store(true, std::memory_order_release);
}

Node Zelph::find_equivalent_rule(const Node rule, const bool mentioned) const
{
    std::lock_guard lock(_pImpl->_rule_index_mtx);

    auto& index        = _pImpl->_rule_index;
    auto& fingerprints = _pImpl->_rule_fingerprints;
    auto& queue        = _pImpl->_rule_index_queue;

    if (_pImpl->_rule_index_stale.exchange(false, std::memory_order_acq_rel))
    {
        index.clear();
        fingerprints.clear();
        _pImpl->_claims.clear();
        const auto& rules = _pImpl->get_left(core.Causes);
        queue.assign(rules.begin(), rules.end());
    }

    const auto fingerprint = [&](const Node r) -> std::size_t
    {
        if (const auto known = fingerprints.find(r); known != fingerprints.end()) return known->second;
        if (!exists(r) || predicate_of(r) != core.Causes) return 0;

        const std::string shape = rule_shape(this, r);
        if (logging_active()) _rules_fingerprinted.fetch_add(1, std::memory_order_relaxed);
        if (shape.empty()) return 0; // not a rule

        const std::size_t h = std::hash<std::string>{}(shape);
        fingerprints.emplace(r, h);
        index.emplace(h, r);
        return h;
    };

    for (const Node r : queue)
        fingerprint(r);
    queue.clear();

    const std::size_t shape = fingerprint(rule);
    if (shape == 0) return 0;

    Node only_mentioned = 0;
    for (auto [it, end] = index.equal_range(shape); it != end;)
    {
        const Node candidate = it->second;
        if (!exists(candidate))
        {
            fingerprints.erase(candidate);
            it = index.erase(it);
            continue;
        }
        ++it;

        if (candidate == rule || !rules_alpha_equivalent(this, rule, candidate)) continue;
        if (!is_mentioned(candidate)) return candidate;
        if (only_mentioned == 0) only_mentioned = candidate;
    }

    return mentioned ? only_mentioned : 0;
}

uint64_t Zelph::rules_fingerprinted() const
{
    return _rules_fingerprinted.load(std::memory_order_relaxed);
}

void Zelph::reset_rules_fingerprinted() const
{
    _rules_fingerprinted.store(0, std::memory_order_relaxed);
}

// A remembered construction remains exact provided the rule it returned is the
// same rule it originally was -- still present and in force -- and so long as
// neither the rule's own text nor the text of the rule whose construction
// returned it has undergone modification. Every alteration to a rule's text
// that the fingerprint index is informed about (see there) marks the index
// stale, and a stale index results in this record being emptied too. A change
// that the index is not told of leaves the entry standing. The construction
// itself depends on the text of its statement and its key.
Node Zelph::claimed_rule(const Node statement, const Node key) const
{
    std::lock_guard lock(_pImpl->_rule_index_mtx);
    if (_pImpl->_rule_index_stale.load(std::memory_order_acquire))
    {
        _pImpl->_claims.clear();
        return 0;
    }

    const auto it = _pImpl->_claims.find({statement, key});
    if (it == _pImpl->_claims.end()) return 0;

    const Node rule = it->second;
    if (exists(rule) && predicate_of(rule) == core.Causes && !is_mentioned(rule)) return rule;

    _pImpl->_claims.erase(it);
    return 0;
}

void Zelph::note_claim(const Node statement, const Node key, const Node rule) const
{
    std::lock_guard lock(_pImpl->_rule_index_mtx);
    _pImpl->_claims[{statement, key}] = rule;
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

Node Zelph::negation_operand(const Node condition) const
{
    // Each condition within every rule is asked, hence the common answer
    // must be cheap: only a fact that names core.Negation has an edge from
    // it, and a single edge probe settles every other condition.
    if (condition == 0 || !Impl::is_hash(condition) || Impl::is_var(condition)) return 0;
    if (!_pImpl->has_left_edge(condition, core.Negation)) return 0;

    const FactStructure fs = get_preferred_structure(this, condition, 3);
    if (fs.predicate != core.IsA || fs.objects.size() != 1 || *fs.objects.begin() != core.Negation) return 0;

    const Node operand = fs.subject;
    if (operand == 0 || Impl::is_var(operand)) return 0;
    if (predicate_of(operand) != 0 || check_fact(operand, core.IsA, {core.Conjunction}).is_known()) return operand;
    return 0;
}

Node Zelph::negate(const Node pattern)
{
    if (predicate_of(pattern) == 0 && !check_fact(pattern, core.IsA, {core.Conjunction}).is_known())
        throw std::runtime_error("\"¬\" applies to a statement, and \"" + format(pattern) + "\" is not one: its negation could never be read as a condition.");

    return fact(pattern, core.IsA, {core.Negation});
}

// Read the recipe of one hash-identified node. A set constant is asked
// for its members, everything else for its triple. Both readings hold
// only while the ids do -- afterwards the components no longer hash back
// to the node and the verifying reader rejects it.
bool Zelph::hash_recipe(const Node nd, HashRecipe& out) const
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
}

std::vector<std::pair<Node, Zelph::HashRecipe>> Zelph::collect_hash_dependents(const Node node, const Node also) const
{
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
    if (also != 0 && reached.insert(also).second) frontier.push_back(also);

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
                if (!hash_recipe(cand, recipe)) continue;

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

namespace
{
    // A statement or a set constant: a node recognized through its
    // composition. A variable never qualifies as such, regardless of the
    // bits its identifier holds.
    bool is_term(const Node n)
    {
        return Zelph::is_hash(n) && !Zelph::is_var(n);
    }

    // Whether a part of `whole`, at any level of the terms it is built from,
    // yields true for `found`. Read while the ids remain valid.
    template <class Found>
    bool term_holds(const Zelph& z, const Node whole, const Found& found)
    {
        std::vector<Node>        pending{whole};
        std::unordered_set<Node> seen{whole};
        while (!pending.empty())
        {
            const Node n = pending.back();
            pending.pop_back();

            Zelph::HashRecipe recipe;
            if (!z.hash_recipe(n, recipe)) continue;
            adjacency_set parts = recipe.is_set ? recipe.members : recipe.objects;
            if (!recipe.is_set)
            {
                parts.insert(recipe.subject);
                parts.insert(recipe.predicate);
            }
            for (const Node part : parts)
            {
                if (found(part)) return true;
                if (is_term(part) && seen.insert(part).second) pending.push_back(part);
            }
        }
        return false;
    }
}

// The forms, by the equation that a name merge states. An atom is a name
// without the unique-name supposition, thus two atoms merge, and what is
// constructed upon them is rebuilt. An atom and a term make a definition,
// x := t, sound when the term keeps its identifier and x is not contained
// within t. Two terms that differ conflict, with one sole exception: a
// statement `s p O` IS the conjunction of its `s p o`, hence two statements
// sharing one subject and one predicate state the statement that holds the
// objects from both, and its identifier still follows from its content.
// Before this rule, every pair of terms reached Network::merge, which reads
// all subjects multiplied by all predicates multiplied by all objects:
// `a p b` and `c q d` answered `a q d` and `c p b`, `a p b` and `c p b` lost
// `a p b` without comment, and two set constants made one set of the members
// of both.
Node Zelph::merge_shape(const Node from, const Node into, const Node holder, const std::string& name, const std::string& lang) const
{
    if (!is_term(from) && !is_term(into)) return 0;

    const auto refuse = [&](const std::string& reason)
    {
        std::stringstream s;
        s << "Requested name '" << name << "' is already used by node " << holder << " in language '" << lang
          << "'. Merging the two nodes is impossible because " << reason;
        throw std::runtime_error(s.str());
    };

    // A term written out through its content instead of its name: the term
    // might already carry the contested name, and "both" says neither which
    // statement it is nor how it differs from the alternative. A part that
    // is one of the two nodes is written out in the same manner.
    const std::string                           shown = _pImpl->published_lang();
    std::function<std::string(Node, int)>       text;
    const std::function<std::string(Node, int)> part = [&](const Node c, const int depth) -> std::string
    {
        if (!is_term(c)) return format(c);
        if (c != from && c != into)
            if (std::string named = get_formatted_name(c, shown); !named.empty()) return named;
        HashRecipe recipe;
        if (!hash_recipe(c, recipe)) return format(c);
        return recipe.is_set ? text(c, depth + 1) : "(" + text(c, depth + 1) + ")";
    };
    text = [&](const Node n, const int depth) -> std::string
    {
        HashRecipe recipe;
        if (depth > 16 || !is_term(n) || !hash_recipe(n, recipe)) return format(n);
        const auto joined = [&](const adjacency_set& nodes)
        {
            std::vector<std::string> words;
            for (const Node c : nodes)
                words.push_back(part(c, depth));
            std::ranges::sort(words);
            std::string out;
            for (const std::string& word : words)
                out += (out.empty() ? "" : " ") + word;
            return out;
        };
        if (recipe.is_set) return "{" + joined(recipe.members) + "}";
        return part(recipe.subject, depth) + " " + part(recipe.predicate, depth) + " " + joined(recipe.objects);
    };
    const auto is_set = [this](const Node n)
    {
        HashRecipe recipe;
        return hash_recipe(n, recipe) && recipe.is_set;
    };
    const auto quoted = [&](const Node n)
    { return !is_term(n) || is_set(n) ? text(n, 0) : "\"" + text(n, 0) + "\""; };

    // A core node remains perpetually present, thus the term would have
    // to become the core node; resolve_name_conflict_locked keeps the
    // core node `into`.
    for (const Node n : {into, from})
    {
        if (get_core_name(n).empty()) continue;
        const Node other = n == into ? from : into;
        refuse("node " + std::to_string(n) + " is a core node, and a core node never becomes " + (is_set(other) ? "a set constant." : "a statement."));
    }

    // This is the statement-level form of the refusal "one node is a
    // variable, the other not": the variable is tied to its rule's
    // quantifier. The expression `a p X` from the rule
    // `(a p X) => (X ok y)` merged with `a p c` to form `a p {X c}`, and
    // as a result, the rule no longer derived `d ok y` from `a p d`.
    for (const Node n : {into, from})
        if (is_term(n) && var_in_closure(n))
            refuse(quoted(n) + " holds a variable of a rule, and a statement over a rule's variable merges with no other.");

    // The occurs check: x = f(...x...) admits no finite term. `a p b`
    // merged with its own subject `a`, or with `a p (a p b)`, resulted in
    // a node that points at itself, and the other statement was lost.
    for (const auto& [whole, inner] : {std::pair{into, from}, std::pair{from, into}})
    {
        if (!is_term(whole) || !term_holds(*this, whole, [inner](const Node c)
                                           { return c == inner; }))
            continue;
        refuse(quoted(inner) + " is part of " + quoted(whole)
               + (is_set(whole) ? ", and a set constant cannot be one of its own members." : ", and a statement cannot be one of its own parts."));
    }

    // An atom and a term: the term keeps its identifier, while the atom's
    // facts are rebuilt over it. The members of an atom would become
    // members of a set constant, which IS precisely its members: `x in foo`
    // turned into `x in {b c}`, thereafter causing the set to cease mapping
    // back to its id.
    if (!is_term(from) || !is_term(into))
    {
        const Node atom = is_term(from) ? into : from;
        const Node term = is_term(from) ? from : into;
        HashRecipe recipe;
        if (!hash_recipe(term, recipe) || !recipe.is_set) return 0;
        for (const Node rel : get_right(atom))
        {
            if (parse_relation(rel) != core.PartOf) continue;
            adjacency_set objects;
            const Node    member = parse_fact(rel, objects, 0);
            if (member == 0 || Impl::is_var(member) || objects.count(atom) == 0 || recipe.members.count(member) != 0) continue;
            refuse(quoted(atom) + " holds " + quoted(member) + ", which the set constant " + quoted(term) + " does not, and a set constant IS its members.");
        }
        return 0;
    }

    HashRecipe gone;
    HashRecipe kept;
    if (!hash_recipe(from, gone) || !hash_recipe(into, kept)) return 0;

    const Node first  = holder == from ? from : into;
    const Node second = first == from ? into : from;

    // Extensionality: `{b c}` = `{b d}` says that c equals d, not that the
    // set comprises `{b c d}`.
    if (gone.is_set && kept.is_set) refuse(quoted(first) + " and " + quoted(second) + " are set constants, and a set constant IS its members.");
    if (gone.is_set != kept.is_set)
    {
        const Node statement = gone.is_set ? into : from;
        const Node set       = gone.is_set ? from : into;
        refuse(quoted(statement) + " is a statement and " + quoted(set) + " a set constant.");
    }

    // No single statement involving one subject and one predicate states
    // both. When the subjects or predicates they differ in are merged, the
    // two statements become one: the rebuilt fact folds into the other.
    if (gone.subject != kept.subject || gone.predicate != kept.predicate)
        refuse(quoted(first) + " and " + quoted(second)
               + " differ in subject or predicate, and no single statement states both. To make them one statement, merge the subjects or predicates in which they differ.");

    adjacency_set objects = kept.objects;
    for (const Node o : gone.objects)
        objects.insert(o);
    const Node id = Impl::create_hash(kept.predicate, kept.subject, objects);
    return id == into ? 0 : id;
}

Zelph::MergePlan Zelph::plan_merge(const Node from, const Node into, const std::vector<std::pair<Node, HashRecipe>>& recipes, const Node survivor) const
{
    MergePlan plan;
    plan.from = from;
    plan.into = into;
    plan.moved.emplace(from, into);

    // Each node's meaning up to this point, read while the hashes remain
    // intact; a fold makes its survivor represent both parts.
    std::unordered_map<Node, Network::FoldState> state;
    const auto                                   current = [&](const Node n) -> Network::FoldState
    {
        if (const auto it = state.find(n); it != state.end()) return it->second;
        const Network::FoldState read{predicate_of(n) != 0 && is_asserted_fact(n), is_refuted_fact(n)};
        return state.emplace(n, read).first->second;
    };
    const auto mapped = [&plan](const Node nd)
    {
        const auto it = plan.moved.find(nd);
        return it == plan.moved.end() ? nd : it->second;
    };

    plan.from_state = current(from);
    plan.into_state = current(into);
    state[into]     = {plan.from_state.claimed || plan.into_state.claimed, plan.from_state.refuted || plan.into_state.refuted};

    std::unordered_set<Node> gone{from};
    std::unordered_set<Node> created;
    // The two named nodes constitute the initial fold. Upon merging a claimed
    // statement with a rule's pattern, the claim was lost: the pattern's
    // marking stayed on `into` or was rebuilt on it or on the survivor, and
    // only the folds of dependents were claimed below.
    std::unordered_set<Node> folded{into};

    // A single node that the merge re-creates under `new_id`, which the
    // step either creates or folds `old_id` into.
    const auto take_step = [&](const Node old_id, const Node new_id)
    {
        MergeStep step;
        step.old_id    = old_id;
        step.new_id    = new_id;
        step.created   = gone.count(new_id) != 0 || (!exists(new_id) && created.count(new_id) == 0);
        step.old_state = current(old_id);
        if (step.created)
        {
            created.insert(new_id);
            gone.erase(new_id);
            state[new_id] = step.old_state;
            // A fold relocating to a new identifier continues to be
            // classified as a fold.
            if (folded.count(old_id) != 0) folded.insert(new_id);
        }
        else
        {
            step.new_state = current(new_id);
            state[new_id]  = {step.old_state.claimed || step.new_state.claimed, step.old_state.refuted || step.new_state.refuted};
            folded.insert(new_id);
        }

        gone.insert(old_id);
        plan.moved[old_id] = new_id;
        plan.steps.push_back(step);
    };

    // The survivor moves first, before any node built on it or on `from`,
    // ensuring that their new ids are computed from its new one
    // (merge_shape).
    if (survivor != 0)
    {
        take_step(into, survivor);
        plan.moved[from] = survivor;
    }

    for (const auto& [old_id, recipe] : recipes)
    {
        if (!exists(old_id) || gone.count(old_id) != 0) continue; // folded away by an earlier round

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
        // is not there, the id is created empty and the merge gives it the
        // adjacency the old node already carries, which the component merges
        // have brought up to date.
        take_step(old_id, new_id);
    }

    for (const Node n : folded)
        if (gone.count(n) == 0 && state[n].claimed) plan.claimed.push_back(n);

    return plan;
}

void Zelph::refuse_conflicting_merge(MergePlan& plan, const Node holder, const std::string& name, const std::string& lang) const
{
    // Where a node ends up once each step has been completed.
    const auto final_of = [&plan](Node n)
    {
        for (size_t hops = 0; hops <= plan.steps.size(); ++hops)
        {
            const auto it = plan.moved.find(n);
            if (it == plan.moved.end()) break;
            n = it->second;
        }
        return n;
    };

    std::unordered_set<Node> moving{plan.from};
    std::vector<Node>        order{plan.from};
    for (const MergeStep& step : plan.steps)
        if (moving.insert(step.old_id).second) order.push_back(step.old_id);

    // Network::merge combines two links that end up connecting the same
    // pair of nodes, and declines when one link has a probability exceeding
    // 0.5 while the other has one under 0.5. Outside a neural net, a
    // probability under 0.5 represents the 0 of a refutation, thus such
    // links are gathered first, and a merge that moves none of them reads
    // each moved link exactly once.
    struct Edge
    {
        Node        from = 0;
        Node        to   = 0;
        long double p    = 1;
    };
    using Ends            = std::pair<Node, Node>;
    const auto each_moved = [&](const auto& visit)
    {
        for (const Node m : order)
        {
            for (const Node t : _pImpl->get_right(m))
                visit(Edge{m, t, _pImpl->probability(m, t)});
            for (const Node s : _pImpl->get_left(m))
                if (moving.count(s) == 0) visit(Edge{s, m, _pImpl->probability(s, m)});
        }
    };
    const auto ends_of = [&](const Edge& e)
    { return Ends{final_of(e.from), final_of(e.to)}; };
    const auto staying = [&](const Ends& ends, Edge& out)
    {
        const auto [a, b] = ends;
        if (moving.count(a) != 0 || moving.count(b) != 0 || !_pImpl->has_right_edge(a, b)) return false;
        out = {a, b, _pImpl->probability(a, b)};
        return true;
    };

    // A statement that has been refuted and another that nobody claimed may
    // coincide: a second rule, typed with the refuted `john sleeps nil` as its
    // consequence, shares the same refuted node. A fold of these two takes the
    // refutation, and the merge assigns to the connection between the
    // unclaimed statement and its predicate the refutation's probability
    // before any other alteration (MergePlan::refuted_with), which
    // Network::merge subsequently joins without a conflict. Only a claimed
    // statement meeting a refuted one is refused: a claimed fact is never
    // refuted. The merge of the two named nodes themselves, a fold involving a
    // neural weight, and a connection that carries its own weight all keep the
    // refusal.
    const auto takes_refutation = [&](const Edge& l, const Edge& h)
    {
        for (const Node n : {l.from, h.from})
            if (n == plan.from || n == plan.into) return false;
        return predicate_of(l.from) == l.to && predicate_of(h.from) == h.to && is_refuted_fact(l.from)
            && !_pImpl->has_synapse(h.from, h.to) && !is_asserted_fact(h.from);
    };

    std::map<Ends, Edge>                 low;
    std::optional<std::pair<Edge, Edge>> conflict; // the low side, the high side
    const auto                           meet = [&](const Edge& l, const Edge& h)
    {
        if (conflict) return;
        if (takes_refutation(l, h))
            plan.refuted_with.emplace_back(h.from, h.to);
        else
            conflict = std::pair{l, h};
    };
    each_moved([&](const Edge& e)
               {
                   if (conflict) return;
                   const Ends ends = ends_of(e);
                   Edge       kept;
                   if (e.p < 0.5L)
                       low.try_emplace(ends, e);
                   else if (e.p > 0.5L && _pImpl->has_synapse(ends.first, ends.second) && staying(ends, kept) && kept.p < 0.5L)
                       meet(kept, e); });
    if (!conflict && !low.empty())
    {
        for (const auto& [ends, l] : low)
        {
            Edge kept;
            if (staying(ends, kept) && kept.p > 0.5L) meet(l, kept);
        }
        each_moved([&](const Edge& e)
                   {
                       if (conflict || e.p <= 0.5L) return;
                       if (const auto it = low.find(ends_of(e)); it != low.end()) meet(it->second, e); });
    }
    if (!conflict) return;

    const auto& [l, h] = *conflict;
    std::stringstream s;
    s << "Requested name '" << name << "' is already used by node " << holder << " in language '" << lang
      << "'. Merging the two nodes is impossible because ";
    if (predicate_of(l.from) == l.to && predicate_of(h.from) == h.to)
    {
        // The statement in question, not the refutation that prints as
        // `¬(...)`.
        std::string refuted = format(l.from);
        if (refuted.starts_with("¬(") && refuted.ends_with(")")) refuted = refuted.substr(3, refuted.size() - 4);
        s << "\"" << refuted << "\" is known to be wrong, and the merge would make it the same statement as \""
          << format(h.from) << "\", which is not.";
    }
    else
        s << "it would join the connection from node " << l.from << " to node " << l.to << ", of probability "
          << static_cast<double>(l.p) << ", with the one from node " << h.from << " to node " << h.to
          << ", of probability " << static_cast<double>(h.p) << ".";
    throw std::runtime_error(s.str());
}

void Zelph::rehash_dependents(const MergePlan& plan) const
{
    // A claimed statement without any dependents that merges into a
    // rule's pattern lacks a step, yet the pattern's marking must still
    // be removed.
    if (plan.steps.empty() && plan.claimed.empty()) return;

    // A repair is not new knowledge. Creating a rebuilt node while a cluster
    // is active would record it there, and dropping that cluster would then
    // delete a fact that existed long before it -- exactly the promise
    // .cluster-drop makes and keeps everywhere else. Recording is therefore
    // suspended, and each rebuilt node inherits the membership of the node it
    // replaces (see retag_cluster_member).
    const ClusterRecordingPause pause(*this);

    // A refutation's marking folds after its fact, and it stays in the cluster
    // where the fact's refutation is taken back (Network::fold_cluster_member).
    const Node                            refuted = const_cast<Zelph*>(this)->get_node(kRefutedFactName, "zelph");
    std::unordered_map<Node, std::string> marking_keepers;

    for (const MergeStep& step : plan.steps)
    {
        if (!exists(step.old_id)) continue; // folded away by an earlier round

        const bool created_here = !exists(step.new_id);
        if (created_here) _pImpl->create(step.new_id);

        _pImpl->merge(step.old_id, step.new_id);
        _pImpl->transfer_names_locked(step.old_id, step.new_id);

        // It is not permissible to pull a pre-existing node into
        // a cluster (fold_cluster_member) when folding into an
        // existing one.
        if (created_here)
        {
            _pImpl->retag_cluster_member(step.old_id, step.new_id);
            continue;
        }
        const auto                       kept   = marking_keepers.find(step.new_id);
        const std::optional<std::string> keeper = kept != marking_keepers.end() ? std::optional<std::string>{kept->second} : std::nullopt;
        if (const auto refutation = _pImpl->fold_cluster_member(step.old_id, step.new_id, step.old_state, step.new_state, keeper); refutation && refuted != 0)
            marking_keepers.emplace(Impl::create_hash(core.IsA, step.new_id, refuted), *refutation);
    }

    // A claim is not discarded through a merge. A fact asserted outside any
    // rule and a rule's pattern that unite into a single statement fold into
    // one node, with the pattern's marking preserved -- re-established on the
    // survivor, or kept on it when the claimed fact was folded into the
    // pattern: the asserted `jim sleeps nil` beside the rule
    // `(mary waits nil) => (jon sleeps nil)` failed to respond to any query
    // after `.name jim jon`, and `.explain` identified it as a pattern that
    // is not asserted. The marking goes, as the claim of a pattern removes it
    // (unmark_rule_pattern), and the indexes below read the survivor as the
    // claim it represents.
    if (const Node marks = rule_pattern_predicate(false); marks != 0)
    {
        for (const Node survivor : plan.claimed)
        {
            const Node marking = Impl::create_hash(core.IsA, survivor, marks);
            if (!exists(survivor) || !_pImpl->fact_edges_hold(marking, survivor, {marks})) continue;

            if (_pImpl->get_right(marking).size() <= 2 && _pImpl->get_left(marking).size() <= 2)
            {
                announce_removal({marking});
                _pImpl->remove(marking);
                _pImpl->remove_node_names(marking);
            }
            else
            {
                adjacency_set doomed;
                collect_doomed(marking, doomed);
                remove_doomed(doomed);
            }
        }
    }

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
    rebuild_opaque_index();

    // An atom defined as a statement: every claimed fact regarding the atom
    // now nests the statement, and a statement claims what it nests, just as
    // its typed line does. `foo` merged with the pattern `a p b` of
    // `(a p b) => (x ok y)` answered `(a p b) q r`, yet the rule was never
    // fired, whereas the typed line `(a p b) q r` fires it. A fact that holds
    // a variable is either a query's or a rule's text, and it claims nothing,
    // as its line would not.
    if (plan.defines)
    {
        const ClaimReach reach = [](const Node n, bool)
        { return Impl::is_hash(n) ? ClaimStep::Enter : ClaimStep::Claim; };
        for (const MergeStep& step : plan.steps)
        {
            const Node n = step.new_id;
            if (!exists(n) || predicate_of(n) == 0 || !is_asserted_fact(n) || is_bookkeeping(n) || is_rule_text_fact(n)) continue;
            claim_nested(n, reach);
        }
    }

    // A rule that has been rebuilt is a new node that the fact() function
    // never queued, and a rule whose node remains can still hold a
    // template whose member was merged.
    note_rule_text_changed();
}

void Zelph::mark_rule_patterns(const Node rule, const std::vector<Node>& created) const
{
    if (created.empty()) return;

    // The conditions and the consequences, and inside them every ground fact
    // node at any depth: "((a p b) g c)" leaks `a p b` just as readily as
    // itself. The `=>` fact is deliberately not walked as a pattern -- a rule
    // is not data, and whether it was asserted or merely mentioned is
    // decided by is_mentioned.
    adjacency_set consequences;
    const Node    condition = parse_fact(rule, consequences);
    if (condition == 0) return;

    mark_rule_parts(condition, consequences, created);
}

void Zelph::mark_rule_parts(const Node condition, const adjacency_set& consequences, const std::vector<Node>& created) const
{
    if (created.empty() || condition == 0) return;

    std::unordered_set<Node> fresh(created.begin(), created.end());
    std::vector<Node>        pending(consequences.begin(), consequences.end());

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

    mark_patterns(constructed_patterns(std::move(pending), fresh, nullptr));
}

std::vector<Node> Zelph::constructed_patterns(std::vector<Node> pending, std::unordered_set<Node>& fresh, const std::function<bool(Node)>& stop) const
{
    std::unordered_set<Node> seen;
    std::vector<Node>        patterns;

    while (!pending.empty())
    {
        const Node nd = pending.back();
        pending.pop_back();
        if (nd == 0 || !seen.insert(nd).second) continue;
        if (Impl::is_var(nd)) continue;
        if (stop && stop(nd)) continue;

        // A COLLECTION carries no intrinsic structure -- its members are
        // attached via PartOf facts, precisely as for the conjunction set
        // above -- and its identifier is either a counter or a template's,
        // not a triple hash. Consequently, it did not pass the is_hash gate
        // below before the structural descent could even stop at it, and the
        // membership facts generated by the rule's own collection literal
        // were never marked. They subsequently functioned as data:
        // `(X in @{a b}) => (X flagged yes)` derived `a flagged yes` and
        // `b flagged yes`, and `.explain` labelled `a in @{a b}` as an axiom,
        // even though the sole reason for that fact's existence is that the
        // rule was written. The same leak that afc0f3e closed for the other
        // shapes.
        //
        // Only a container created by THIS construction is walked -- a set
        // that the rule only references keeps its members and its own facts --
        // thus the adjacency read remains within what the rule brought into
        // being. A SET CONSTANT is excluded: its membership is determined by
        // definition, not asserted -- `a in {a b}` holds because that is what
        // the set IS -- hence a rule quantifying over it using `(X in {a b})`
        // legitimately binds a and b. Only a COLLECTION that the rule itself
        // constructed holds members that nobody claimed. Its membership facts
        // belong to the rule regardless of whether they appear in `created`:
        // the collection emerged alongside the rule, so whatever it holds is
        // the rule's text.
        if (fresh.count(nd) != 0 && !is_set_constant(nd))
        {
            for (const Node rel : get_right(nd))
            {
                if (parse_relation(rel) != core.PartOf) continue;
                adjacency_set objs;
                const Node    member = parse_fact(rel, objs, 0);
                if (member == 0 || objs.count(nd) != 1) continue;
                fresh.insert(rel);
                pending.push_back(rel); // the membership fact is the pattern
                pending.push_back(member);
            }
        }

        if (!Impl::is_hash(nd)) continue; // an atom has no fact structure

        const FactStructure fs = get_preferred_structure(this, nd, 3);
        if (fs.predicate == 0 || fs.subject == 0)
        {
            // A set constant: its own memberships define what it is and stay
            // unmarked, yet what the rule wrote within it constitutes the
            // rule's text, just as the rest does -- the collection in
            // `{@{c}}`, the fact in `{(k in m)}`.
            if (is_set_constant(nd))
            {
                for (const Node rel : get_right(nd))
                {
                    if (parse_relation(rel) != core.PartOf) continue;
                    adjacency_set objs;
                    const Node    member = parse_fact(rel, objs, 0);
                    if (member != 0 && objs.count(nd) == 1) pending.push_back(member);
                }
            }
            continue;
        }

        // Only that which this construction has brought into existence, and
        // only where no variable serves to give it away as a template. A
        // variable within the conditions of a rule that the part mentions
        // does not give it away to the check seeking an earlier witness,
        // which looks for what a firing writes (VariableReading::Instance):
        // unmarked, a rule's own consequence
        // `k about ((A r B, A s B) => (c q d))` would hold there from the
        // moment the rule is written.
        if (fresh.count(nd) != 0 && !var_in_closure(nd, VariableReading::Instance)) patterns.push_back(nd);

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

    return patterns;
}

void Zelph::mark_patterns(const std::vector<Node>& patterns) const
{
    if (patterns.empty()) return;

    const Node pred = rule_pattern_predicate(true);
    auto*      self = const_cast<Zelph*>(this);

    std::unique_lock lock(_pImpl->_rule_patterns_mtx);
    for (const Node p : patterns)
    {
        self->fact(p, core.IsA, {pred}, 1, Writer::pattern_mark);
        _pImpl->_rule_patterns.insert(p);
        invalidate_predicate_index_of(p);
    }
    _pImpl->_has_rule_patterns.store(true, std::memory_order_release);
}

bool Zelph::is_condition_tag(const Node node) const
{
    if (negation_operand(node) != 0) return true;
    if (node == 0 || !Impl::is_hash(node) || Impl::is_var(node)) return false;

    // The remaining two are recognised by their predicate
    // (Reasoning::evaluate), and similarly to a negation, they pertain to
    // a statement: `x nn y` on an atom constitutes the parser's ordinary
    // fact (explicit_condition_op).
    const Node closure = get_node("closure", "zelph");
    const Node nn      = get_node("nn", "zelph");
    if (closure == 0 && nn == 0) return false;

    const FactStructure fs = get_preferred_structure(this, node, 3);
    if (fs.subject == 0 || Impl::is_var(fs.subject) || fs.objects.size() != 1) return false;
    if ((closure == 0 || fs.predicate != closure) && (nn == 0 || fs.predicate != nn)) return false;
    return predicate_of(fs.subject) != 0 || check_fact(fs.subject, core.IsA, {core.Conjunction}).is_known();
}

void Zelph::mark_query_operands(const Node statement, const std::vector<Node>& created, const std::vector<Node>& refuted) const
{
    if (created.empty()) return;

    // A query consists of a condition list that undergoes evaluation just
    // once, and it is assembled before evaluation -- thus, "X go Y, ¬(a p
    // b)" built `a p b` as data and subsequently found its own
    // counterexample. What it built beneath an operator is what a rule's
    // parts are: a pattern. The specific nodes involved depend on
    // structure, not on the sequence in which the calls were executed: the
    // traversal beginning at the statement stops at each operator's fact,
    // the walk from those facts goes through all elements located beneath
    // them, and only what the initial traversal fails to reach is marked --
    // in "X go Y, (a p b), ¬(a p b)" the positive part is created and
    // thereby asserted, regardless of which of the two appears first.
    //
    // Both walks remain confined to what the query created: a node that
    // existed before cannot hold one created afterwards, since its
    // identifier is the hash of its parts.
    const std::unordered_set<Node> made(created.begin(), created.end());
    const auto                     beyond = [&](const Node nd)
    { return made.count(nd) == 0; };

    // A rule mentioned by the query outside every operator constitutes part
    // of what it creates and thereby asserts, as zelph/rule-text leaves a
    // mention inside a query: in "X knows ((a p b) => (c q d)), ¬(a p b)",
    // `a p b` stays data just as `c q d` does. An operator within the
    // mentioned rule is still an operator, meaning its operand is a
    // pattern: in "R knows ((X go Y, ¬(a p b)) => (X ok Y))" the walk
    // through the rule stops at the negation, just as the walk outside it
    // does.
    //
    // The fact that one of the query's ground markings refutes is not among
    // its positive parts (Zelph::refute): the walk outside the operators stops
    // there as well, and what the query created of it is marked as the operand
    // of an operator is. Built as data, it answered once the refutation was
    // pruned: following "X sleeps nil, (mary waits nil) ~ refuted",
    // `S waits O` responded with `mary waits nil`.
    const std::unordered_set<Node> refutes(refuted.begin(), refuted.end());
    std::unordered_set<Node>       entered;
    std::vector<Node>              mentions;
    const auto                     stop_outside = [&](const Node nd)
    {
        if (beyond(nd) || is_condition_tag(nd) || refutes.count(nd) != 0) return true;
        if (predicate_of(nd) == core.Causes)
        {
            mentions.push_back(nd);
            return true;
        }
        entered.insert(nd);
        return false;
    };
    const auto stop_in_mention = [&](const Node nd)
    {
        if (beyond(nd) || is_condition_tag(nd)) return true;
        entered.insert(nd);
        return false;
    };
    std::unordered_set<Node> fresh_outside = made;
    std::vector<Node>        outside       = constructed_patterns({statement}, fresh_outside, stop_outside);
    const std::vector<Node>  mentioned     = constructed_patterns(mentions, fresh_outside, stop_in_mention);
    outside.insert(outside.end(), mentioned.begin(), mentioned.end());

    // The operators are every operator's fact the query created and the
    // walk outside did not enter, also one it cannot reach: in "(X p (a q
    // b)) ~ *negation" the focus makes the statement the node `negation`,
    // and the negation fact hangs off nothing the query returns.
    std::vector<Node> operators;
    for (const Node nd : created)
        if (entered.count(nd) == 0 && is_condition_tag(nd)) operators.push_back(nd);
    for (const Node nd : refuted)
        if (made.count(nd) != 0 && entered.count(nd) == 0) operators.push_back(nd);
    if (operators.empty()) return;

    std::unordered_set<Node> fresh_inside = made;
    const std::vector<Node>  inside       = constructed_patterns(operators, fresh_inside, beyond);

    const std::unordered_set<Node> reached_outside(outside.begin(), outside.end());
    std::vector<Node>              patterns;
    for (const Node nd : inside)
        if (reached_outside.count(nd) == 0) patterns.push_back(nd);
    mark_patterns(patterns);
}

size_t Zelph::drop_query_text(const std::vector<Node>& created, const std::vector<std::pair<Node, Node>>& declared) const
{
    // A query's statement takes the form of a pattern, and it remained within
    // the graph after the query had been answered: its variables made it one
    // only while something accessed them. Within the members of a set -- a
    // set pattern, a set holding one, or the conjunction set of a
    // comma-separated list -- no structure extends to them, and following a
    // .save and a .load, the record that names a set pattern disappears, so
    // `S can nil` yielded `@{(C ~ card) (X enters C)} can nil` beside the
    // facts. What the query created that holds one of its variables is its
    // text, and it ceases to exist once the query is answered; what it created
    // without one persists, because a query asserts what it constructs outside
    // its operators.
    std::unordered_set<Node> text;
    for (const Node nd : created)
        if (Impl::is_var(nd) && _pImpl->exists(nd)) text.insert(nd);
    if (text.empty() && declared.empty()) return 0;

    // A collection that the query constructs is part of the query's text as
    // well, even if no member holds a variable. A collection possesses an
    // identity of its own: no fact outside the query refers to the
    // collection that the query has just created, and each inquiry
    // generates a new one. Kept, they caused `X says @{k}` to leave a
    // collection and its membership behind at every instance. A set
    // constant consists of its members, the identical node at each inquiry,
    // and stays.
    const std::unordered_set<Node> made(created.begin(), created.end());
    for (const Node nd : created)
    {
        if (Impl::is_var(nd) || !Impl::is_hash(nd) || !_pImpl->exists(nd) || parse_relation(nd) != core.PartOf) continue;
        adjacency_set objects;
        parse_fact(nd, objects);
        for (const Node container : objects)
            if (made.count(container) != 0 && !Impl::is_var(container) && !Impl::is_hash(container) && !is_named_any(container)) text.insert(container);
    }

    // Holding is interpreted through the structure of what the query
    // created: a fact holds what its subject, predicate, or objects hold, and
    // a collection holds what a member holds -- after which every membership
    // of it also holds it. A collection the query did not create is the data
    // it inquires about, just as a node the query names is the data it
    // queries: `X in newd, newd r e` requests the members of `newd` and
    // asserts `newd r e`.
    const auto held = [&text](const Node part)
    { return text.count(part) != 0; };
    for (bool grew = true; grew;)
    {
        grew = false;
        for (const Node nd : created)
        {
            if (!Impl::is_hash(nd) || held(nd) || !_pImpl->exists(nd)) continue;

            adjacency_set objects;
            const Node    subject   = parse_fact(nd, objects);
            const Node    predicate = parse_relation(nd);
            if (!held(subject) && !held(predicate) && std::none_of(objects.begin(), objects.end(), held)) continue;

            text.insert(nd);
            grew = true;
            if (predicate == core.PartOf && held(subject))
                for (const Node container : objects)
                    if (made.count(container) != 0 && !is_named_any(container)) text.insert(container);
        }
    }

    // mark_query_operands marked a pattern under an operator once the query's
    // scratch had been closed, so the marking is stored alongside the
    // caller's nodes; it goes with its pattern. Moved into a standalone
    // cluster, the nodes exit via drop_cluster, which keeps the fact stores
    // in an armed state.
    std::vector<Node> doomed(text.begin(), text.end());
    if (const Node marked = rule_pattern_predicate(false); marked != 0)
        for (const Node nd : text)
            if (is_rule_pattern(nd))
                if (const Node marking = Impl::create_hash(core.IsA, nd, marked); _pImpl->exists(marking)) doomed.push_back(marking);

    // The relation-type declaration that fact() wrote for the query's first
    // use of a predicate is part of the query's text too, even though it
    // holds no variable: `X b Y`, asked after `a likes b`, left `b ~ ->` in
    // its wake, and both a .save and a .load kept it. It remains tied to the
    // text, and the drop maintains it as long as something that persists
    // continues to utilize it (keep_shared_declarations): either a fact that
    // the query asserts using that predicate, or a statement it formulates
    // regarding the declaration. A declaration that the query also states,
    // as part of what it created, is a ground component that the query
    // asserts and stays: in `X b Y, b ~ ->`, the declaration that `X b Y`
    // writes first is the one subsequently held by the comma list.
    std::unordered_map<Node, Node> unstated;
    for (const auto& [declaration, predicate] : declared)
        if (text.count(declaration) == 0 && _pImpl->exists(declaration)) unstated.emplace(declaration, predicate);
    for (const Node nd : created)
    {
        if (unstated.empty()) break;
        if (!Impl::is_hash(nd) || !_pImpl->exists(nd) || unstated.count(nd) != 0) continue;
        adjacency_set objects;
        unstated.erase(parse_fact(nd, objects));
        unstated.erase(parse_relation(nd));
        for (const Node o : objects)
            unstated.erase(o);
    }
    const std::vector<std::pair<Node, Node>> provisional(unstated.begin(), unstated.end());
    for (const auto& entry : provisional)
        doomed.push_back(entry.first);

    const std::string scratch = "__query-text";
    _pImpl->move_into_cluster(scratch, doomed, provisional);
    return drop_cluster(scratch);
}

void Zelph::mark_mention(const Node root, const std::vector<Node>& created) const
{
    if (root == 0 || created.empty()) return;

    // Only fact nodes: a collection the mention created is a value, much
    // like a firing's collection within a mention, so its memberships
    // stay data. The walk stops at a rule, which is rule text: its parts
    // are marked as a rule's, and a rule the mention holds beneath its
    // root is mentioned by its holder (is_mentioned).
    std::unordered_set<Node> fresh;
    for (const Node nd : created)
        if (Impl::is_hash(nd)) fresh.insert(nd);

    std::vector<Node> rules;
    const auto        stop_at_rule = [&](const Node nd)
    {
        if (predicate_of(nd) != core.Causes) return false;
        rules.push_back(nd);
        return true;
    };
    mark_patterns(constructed_patterns({root}, fresh, stop_at_rule));
    for (const Node rule : rules)
        mark_rule_patterns(rule, created);

    // A rule located at the root is mentioned by nothing else:
    // zelph/mention and zelph_mention return it before any statement can
    // hold it, and an opaque argument is a mention scope of its own.
    // Unmarked, it was in force immediately upon being written: .list-rules
    // listed it, it fired, and it answered `S => O`. The mark is a
    // statement that names the rule, thus is_mentioned reads it just as it
    // reads any holder, and the mark stays when a holder written later is
    // pruned. A claim of the rule revokes it, as it revokes the mark of
    // every mention (unmark_rule_pattern): a typed rule, zelph/fact within
    // a block, zelph_fact, and the rule builders, which assert what they
    // keep. A rule that was present before the mention keeps its status.
    // Unification skips a marked candidate, so a rule over rules does not
    // read the mentioned rule, whereas one held by a plain statement stays
    // readable.
    if (fresh.count(root) != 0 && predicate_of(root) == core.Causes) mark_patterns({root});
}

Node Zelph::mention(const Node subject, const Node predicate, const adjacency_set& objects)
{
    const Answer known = check_fact(subject, predicate, objects);
    if (known.is_known()) return known.relation();

    Node fact_node = 0;
    {
        const PatternScope held(*this);
        fact_node = fact(subject, predicate, objects);
    }
    mark_mention(fact_node, {fact_node});
    return fact_node;
}

Node Zelph::refute(const Node subject, const Node predicate, const adjacency_set& objects)
{
    const bool existed   = check_fact(subject, predicate, objects).is_known();
    const Node fact_node = fact(subject, predicate, objects, 0.0L);
    if (fact_node == 0) return 0;
    if (!existed) mark_mention(fact_node, {fact_node});
    mark_refuted_fact(fact_node);
    return fact_node;
}

bool Zelph::is_mention(const Node node) const
{
    if (!is_rule_pattern(node)) return false;

    // The fact of a condition operator is a condition, not a mention:
    // nothing holds the negation a query wrote after the query was
    // answered, and `.node` labelled it "Mentioned (not asserted)", whereas
    // its operand read "Rule pattern (not asserted)", just as with every
    // other item a query wrote beneath an operator.
    if (is_condition_tag(node)) return false;

    std::vector<Node>        pending{node};
    std::unordered_set<Node> seen{node};
    while (!pending.empty())
    {
        const Node nd = pending.back();
        pending.pop_back();

        adjacency_set holders = _pImpl->get_right(nd);
        for (const Node from : _pImpl->get_left(nd))
            holders.insert(from);

        for (const Node whole : holders)
        {
            if (whole == nd || !Impl::is_hash(whole) || Impl::is_var(whole)) continue;
            const Node predicate = parse_relation(whole);
            if (predicate == 0) continue;
            adjacency_set objects;
            const Node    subject = parse_fact(whole, objects, 0);
            if (subject != nd && predicate != nd && objects.count(nd) == 0) continue;

            if (predicate == core.Causes || is_condition_tag(whole)) return false;
            if (predicate == core.PartOf && subject == nd)
            {
                for (const Node container : objects)
                {
                    if (holds_rule_text(container)) return false;
                    if (seen.insert(container).second) pending.push_back(container);
                }
                continue;
            }
            if ((is_rule_pattern(whole) || var_in_closure(whole)) && seen.insert(whole).second) pending.push_back(whole);
        }
    }
    return true;
}

bool Zelph::has_rule_form(const Node node) const
{
    adjacency_set deductions;
    Node          condition = parse_fact(node, deductions);

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

    return condition_is_statement && condition != core.Causes && derives_something;
}

adjacency_set Zelph::get_rules() const
{
    const adjacency_set& rule_candidates = _pImpl->get_left(core.Causes);

    adjacency_set rules;

    for (Node rule_candidate : rule_candidates)
    {
        // We filter the rule candidates in the same way as Reasoning::apply_rule() does it.
        // Note that a rule candidate with empty deductions is interpreted as a question, see Reasoning::evaluate()
        if (rule_candidate && has_rule_form(rule_candidate) && !is_mentioned(rule_candidate))
        {
            rules.insert(rule_candidate);
        }
    }

    return rules;
}

void Zelph::remove_rules() const
{
    // A rule whose only mention is through a removed rule -- the rule located
    // within a rule generator, at any depth -- becomes a part of nothing once
    // that rule has vanished, and is therefore in force (is_mentioned).
    // Consequently, the rules are read again until no rule remains. A pass
    // that removes nothing also terminates the loop, because the next pass
    // would read the same rules once more.
    for (bool removed = true; removed;)
    {
        removed = false;
        for (Node rule : get_rules())
        {
            invalidate_fact_structures_cache();

            announce_removal({rule});
            _pImpl->remove(rule);
            if (!_pImpl->exists(rule)) removed = true;
            _pImpl->remove_node_names(rule);
        }
    }

    // The older engine's rules went with the others, and a save writes the
    // template-id field again once no text from them remains. Asked again
    // instead of being cleared: a rule mentioned by a statement is not
    // removed, and its text might belong to the older engine.
#ifndef __EMSCRIPTEN__
    if (!_pImpl->_older_rule_literals.empty()) _pImpl->_older_rule_literals = literals_written_before_template_ids();
#endif
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

    // Read before the locks as well: a declaration `P ~ opaque`
    // accompanies P just as P's relation-type declaration does, or a fact
    // within the slice would claim what its arguments merely mention.
    const Node opaque = has_opaque_predicates() ? get_node(kOpaqueName, "zelph") : 0;

    // Read before the locks as well, because is_set_constant reads the
    // adjacency: the refuted nodes that are contradiction records, which stay
    // behind (below), excluding the refuted facts, which move along with
    // their refutation.
    //
    // A record stays behind whole: the set, its marking, and its memberships.
    // The memberships were accessed via a kept member and moved without their
    // set, so after a slice of `p` beside a contradiction was loaded, `S in O`
    // answered `:in @{?}`. A set that a statement refers to is that
    // statement's set too (is_bare_record) and moves just like any set; from
    // the record, only the marking stays behind.
    ankerl::unordered_dense::set<Node> records;
    ankerl::unordered_dense::set<Node> refuted;
    ankerl::unordered_dense::set<Node> record_facts;
    struct RecordStatements
    {
        Node              marking;
        std::vector<Node> memberships; // only the ones of a record that stays behind whole
    };
    ankerl::unordered_dense::map<Node, RecordStatements> record_statements;
    ankerl::unordered_dense::map<Node, Node>             record_of_marking;
    if (const std::vector<Node> refuted_nodes = refuted_facts_snapshot(); !refuted_nodes.empty())
    {
        const Node marking = const_cast<Zelph*>(this)->get_node(kRefutedFactName, "zelph");
        for (const Node r : refuted_nodes)
        {
            if (!is_set_constant(r))
            {
                refuted.insert(r);
                continue;
            }
            const Node record_marking = create_hash(core.IsA, r, {marking});
            record_facts.insert(record_marking);
            record_statements[r].marking      = record_marking;
            record_of_marking[record_marking] = r;
            adjacency_set members;
            if (!is_bare_record(r) || !set_constant_members(r, members)) continue;
            records.insert(r);
            for (const Node member : members)
            {
                const Node membership = create_hash(core.PartOf, member, {r});
                record_facts.insert(membership);
                record_statements[r].memberships.push_back(membership);
            }
        }
    }
    const ankerl::unordered_dense::set<Node> record_statement_ids = record_facts; // does not diminish, in contrast to record_facts

    ankerl::unordered_dense::set<Node> keep{
        core.RelationTypeCategory, core.Causes, core.IsA, core.Unequal, core.Contradiction, core.Cons, core.Nil, core.PartOf, core.Conjunction, core.Negation};

    // The labels a slice maintains are titled according to how the import
    // named them, thus the node stating this goes along even though no
    // kept fact arrives at it.
    if (const Node labels_marker = get_node(wikidata_labels_marker_name(), "zelph", true); labels_marker != 0) keep.insert(labels_marker);

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
        // The memberships go with it too, for the identical
        // rationale (record_facts, above).
        //
        // The whole test lies behind a single emptiness check, meaning a
        // graph that records no contradiction incurs only that one and
        // nothing additional.
        //
        // A refuted fact is not a record: it travels with its marking and its
        // weight, as the session maintains it. Left behind alongside the
        // records, it carried its refutation with it: after
        // "¬(mary waits nil)", a slice of `waits` and `~` loaded without the
        // refutation, and accepted "mary waits nil".
        const auto stays_behind = [&records, &record_facts](const Node nd)
        {
            if (record_facts.empty()) return false;
            return records.count(nd) != 0 || record_facts.count(nd) != 0;
        };

        // A record that stays behind may still be reached from something that
        // travels, and leaving it behind then broke that holder: within a `says`
        // slice, `x says ({(x p y) (x q y)} ~ refuted)` lost its object, and a rule
        // whose condition names a record's set beside a variable lost the set in
        // every slice, because every rule travels. Reached from a holder of its set or
        // of its marking, the record thus travels whole -- the set, the marking, and
        // the memberships -- and the slice reads it as the source does. Only the
        // marking of a set that a statement refers to, reached from that set,
        // continues to stay behind (as shown above). A record's statement is never
        // counted as a fact within the slice, regardless of which predicate the loop
        // below reads first.
        const auto retain = [&](const Node nd, const Node from)
        {
            if (stays_behind(nd))
            {
                Node record = records.count(nd) != 0 ? nd : 0;
                if (record == 0)
                {
                    const auto it = record_of_marking.find(nd);
                    if (it == record_of_marking.end() || it->second == from) return;
                    record = it->second;
                }
                records.erase(record);
                const RecordStatements& statements = record_statements[record];
                record_facts.erase(statements.marking);
                for (const Node membership : statements.memberships)
                    record_facts.erase(membership);
            }
            if (keep.insert(nd).second) pending.push_back(nd);
        };

        for (const Node p : predicates)
            retain(p, 0);
        for (const Node rule : rules)
            retain(rule, 0);

        // The declarations that make the predicates usable, including the
        // core ones (a freshly constructed network re-creates those, but a
        // load replaces the whole state, so they have to be in the file).
        std::vector<Node> declared(predicates);
        declared.insert(declared.end(), {core.IsA, core.Unequal, core.Causes, core.Cons, core.PartOf});
        for (const Node nd : declared)
        {
            const Node declaration = create_hash(core.IsA, nd, {core.RelationTypeCategory});
            if (exists_unlocked(declaration)) retain(declaration, 0);
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
                //
                // A refuted fact of p travels, accompanied by its refutation,
                // even though nobody claims it: a slice of `waits` loaded
                // without "¬(mary waits nil)" and accepted "mary waits nil",
                // although its source refuses it. The slice does not count it
                // as a fact.
                const bool refutation = refuted.count(rel) != 0;
                if (!refutation && unasserted && unasserted->count(rel) != 0) continue;

                // A record's statement is not a fact of the slice, and
                // it must not bring its subject and objects along.
                if (record_statement_ids.count(rel) != 0) continue;

                // rel can also be a fact ABOUT p -- the declaration above, or
                // "P279 is a transitive relation". Those carry p as their
                // SUBJECT, which puts it on both sides of rel just like the
                // predicate position does, so the two are told apart the same
                // way the index builder does it: a fact OF p has a subject
                // that is not p and stands on both sides of rel.
                //
                // A fact with subject p, `p p q`, has no other subject, and
                // the slice left it out. Its identifier is the hash of its
                // triple, distinguishing it from a fact concerning p, as the
                // query candidate filters distinguish it
                // (ReadScope::subject_is_predicate).
                const bool is_fact_of_p =
                    std::any_of(rel_left->second.begin(), rel_left->second.end(), [&](const Node candidate)
                                { return candidate != p
                                      && !Impl::is_var(candidate)
                                      && rel_right->second.count(candidate) == 1; })
                    || Impl::subject_is_predicate(rel, p, rel_right->second, rel_left->second);
                if (!is_fact_of_p) continue;

                retain(rel, 0);
                if (!refutation) ++facts;
                for (const Node nd : rel_left->second)
                    retain(nd, rel);
                for (const Node nd : rel_right->second)
                    retain(nd, rel);
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
                    retain(tagged, nd);
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
            if (exists_unlocked(declaration)) retain(declaration, nd);
            if (opaque != 0)
            {
                const Node opacity = create_hash(core.IsA, nd, {opaque});
                if (exists_unlocked(opacity)) retain(opacity, nd);
            }

            // A condition set is expanded although it is not content-addressed:
            // its adjacency IS the rule's conditions, which is small and wanted,
            // where a plain node's adjacency is the rest of the graph. Without
            // this the conditions were reached only by accident -- the loop over
            // the sliced predicates retained them, because the set stands on
            // both sides of each of its members and so passed for their subject.
            // Once that accident was corrected the contradiction rule of
            // test_predicate_slice lost its conditions and stopped being a rule
            // at all, which is how the hole showed itself.
            //
            // A collection of a rule's own text (is_rule_template, a bit test
            // on the id) is expanded for the same reason: its adjacency
            // consists of the memberships the rule wrote and the rule's
            // statements that name it, and the memberships bring their members
            // and their rule-pattern marks with them. Without this, the rule
            // arrives bearing a different text -- its `@{c d}` empty, printed
            // `(X likes ??)` -- and a firing writes the emptied collection
            // itself into the data. A term a firing built is data, just like
            // any other collection: its memberships are facts of `in` and
            // travel in a slice of `in`.
            if (!Impl::is_hash(nd) && !is_condition_set && !is_rule_template(nd)) continue;

            const auto left_it = _pImpl->_left.find(nd);
            if (left_it != _pImpl->_left.end())
            {
                for (const Node participant : left_it->second)
                    retain(participant, nd);
            }

            const auto right_it = _pImpl->_right.find(nd);
            if (right_it != _pImpl->_right.end())
            {
                for (const Node participant : right_it->second)
                    retain(participant, nd);
            }
        }
    }

    _pImpl->saveToFile(filename, &keep);

    return facts;
}

// A load that advanced the epoch has discarded the graph, even if it
// encountered failure following the file's header: the indexes read from the
// graph are reconstructed using the data the load left, the display
// registrations are forgotten, and the derived engine loses memory of what it
// had retained regarding the old graph (_on_graph_replaced). A load that
// encountered failure before interacting with the graph introduces no
// alterations in this context.
void Zelph::adopt_loaded_graph(const std::uint64_t epoch_before) const
{
    if (graph_epoch() == epoch_before) return;

    _pImpl->rebuild_label_index();
    rebuild_rule_pattern_index();
    rebuild_refuted_index();
    rebuild_opaque_index();
    note_rule_text_changed();
    forget_display_registrations();
    if (_on_graph_replaced) _on_graph_replaced();
}

void Zelph::load_from_file(const std::string& filename) const
{
    invalidate_fact_structures_cache();

    const std::uint64_t epoch               = graph_epoch();
    bool                negation_conditions = true;
    bool                template_ids        = false;
    try
    {
        template_ids = _pImpl->loadFromFile(filename, negation_conditions);
    }
    catch (...)
    {
        adopt_loaded_graph(epoch);
        throw;
    }
    adopt_loaded_graph(epoch);

    // Only the complete network, and solely one whose file leaves the
    // template-id field as false: an engine before template IDs wrote it,
    // or this engine saved a network in which a load detected rules which
    // that engine had previously saved (their collections keep their
    // counter IDs throughout the save). A view is not subject to reasoning
    // (.run rejects it), and might be missing the facts and names that the
    // test reads.
    if (!template_ids) _pImpl->_older_rule_literals = literals_written_before_template_ids();
    if (!_pImpl->_older_rule_literals.empty())
    {
        out("Note: rules in this network were saved by an older zelph, and their collections are data here; "
            "rebuild the network from its scripts before reasoning over it.",
            true);
    }

    // An older engine negated a condition by a fact concerning its pattern
    // and named the pattern itself as the condition, so here each such
    // condition reads as positive. Nothing within such a file says which
    // rule negated what, and a file saved by this engine cannot be told from
    // it based solely on its facts: the header field decides, and any
    // negation fact within the network gets the note.
    if (!negation_conditions && !_pImpl->_older_negations)
    {
        for (const Node f : _pImpl->get_right(core.Negation))
        {
            if (negation_operand(f) == 0) continue;
            _pImpl->_older_negations = true;
            break;
        }
    }
    if (!negation_conditions && _pImpl->_older_negations)
    {
        out("Note: negated rule conditions in this network were saved by an older zelph and read as positive "
            "here; rebuild the network from its scripts before reasoning over it.",
            true);
    }
}

// Before the template ids, the engine wrote the collections associated with a
// rule's text under counter ids, just as it wrote every data collection.
// Within a network it saved, such a collection is thus a value: a firing
// names the rule's own collection in the data, and the rule re-typed is a
// second rule, because its literal is now a template. The older engine also
// named the rule's own collection for a container the rule writes into and
// for a literal that has nothing to substitute, yet it rebuilt, for each
// binding, a literal whose members the binding makes ground; a firing in this
// context names that literal, including any variable. Only by reconstructing
// the network from its scripts does it provide those rules with their own
// collections, as the load says.
//
// What gives such a collection away is a membership written as part of the
// rule's literal: one marked as a rule pattern or holding a variable, and no
// node of any rule's text itself. A data collection that a generated rule
// refers to carries the rule's own statements, `X in D` or `k in D`, which
// are nodes within its text, and its data, which passes while no member of
// it holds a variable. A member that does -- a rule, for instance -- takes
// on the shape of a literal's membership, and a literal can contain a rule
// as well, thus such a data collection gets the note even though it is data
// for both engines. A named node is a constant across every engine, a
// conjunction set is read by its tag, and a template along with a value
// recipe belong to this engine. A rule over a collection made outside every
// rule scope -- by a Janet program before it builds the rule, or via the C
// ABI -- exhibits the same shape when that collection holds a variable,
// which is why only a file whose header lacks the template-id field is
// asked.
//
// The outcome is every such membership, not merely the first one found:
// the printer outputs precisely these as members of a data collection
// even though they are patterns (is_older_rule_literal), ensuring the
// rule continues to print the literal as originally written.
//
// A single traversal through the rules' texts, and through the
// memberships contained within the collections encountered there,
// once per load.
std::unordered_set<Node> Zelph::literals_written_before_template_ids() const
{
    constexpr int max_depth = 64; // a guard against cycles; the rules are shallow

    std::unordered_set<Node>          text;        // each fact of a rule's text
    std::vector<Node>                 collections; // the counter collections it holds
    std::unordered_set<Node>          seen;
    std::vector<std::pair<Node, int>> pending;
    for (const Node rule : _pImpl->get_left(core.Causes))
        if (predicate_of(rule) == core.Causes) pending.emplace_back(rule, 0);

    const auto push_members = [&](const Node container, const int depth)
    {
        for (const Node rel : get_right(container))
        {
            if (predicate_of(rel) != core.PartOf) continue;
            adjacency_set objects;
            const Node    member = parse_fact(rel, objects, 0);
            if (member != 0 && objects.count(container) == 1) pending.emplace_back(member, depth + 1);
        }
    };

    while (!pending.empty())
    {
        const auto [n, depth] = pending.back();
        pending.pop_back();
        if (n == 0 || Impl::is_var(n) || depth > max_depth || !seen.insert(n).second) continue;

        if (Impl::is_hash(n))
        {
            const FactStructure fs = get_preferred_structure(this, n, 3);
            if (fs.subject == 0 || fs.predicate == 0)
            {
                push_members(n, depth); // a set constant
                continue;
            }
            text.insert(n);
            pending.emplace_back(fs.subject, depth + 1);
            pending.emplace_back(fs.predicate, depth + 1);
            for (const Node o : fs.objects)
                pending.emplace_back(o, depth + 1);
            continue;
        }

        if (is_rule_template(n) || check_fact(n, core.IsA, {core.Conjunction}).is_known())
        {
            push_members(n, depth);
            continue;
        }
        if (Impl::is_recipe(n) || is_named_any(n)) continue;
        collections.push_back(n);
    }

    std::unordered_set<Node> literals;
    for (const Node collection : collections)
    {
        for (const Node rel : get_right(collection))
        {
            if (predicate_of(rel) != core.PartOf || text.count(rel) != 0) continue;
            if (!is_rule_pattern(rel) && !var_in_closure(rel)) continue;
            adjacency_set objects;
            if (parse_fact(rel, objects, 0) != 0 && objects.count(collection) == 1) literals.insert(rel);
        }
    }
    return literals;
}

void Zelph::load_from_file(const std::string& filename, const BinChunkSelection& selection, const bool skip_payload) const
{
    invalidate_fact_structures_cache();

    const std::uint64_t epoch = graph_epoch();
    try
    {
        _pImpl->loadFromFile(filename, selection, skip_payload);
    }
    catch (...)
    {
        adopt_loaded_graph(epoch);
        throw;
    }
    adopt_loaded_graph(epoch);
}

void Zelph::load_from_manifest(const std::string&       manifest_path,
                               const BinChunkSelection& selection,
                               const std::string&       shard_root,
                               const std::string&       bin_path_override,
                               const bool               skip_payload) const
{
    invalidate_fact_structures_cache();

    const std::uint64_t epoch = graph_epoch();
    try
    {
        _pImpl->loadFromManifest(manifest_path, selection, shard_root, bin_path_override, skip_payload);
    }
    catch (...)
    {
        adopt_loaded_graph(epoch);
        throw;
    }
    adopt_loaded_graph(epoch);
}
#endif

void        Zelph::set_active_cluster(const std::string& name) const { _pImpl->set_active_cluster(name); }
void        Zelph::deactivate_cluster() const { _pImpl->deactivate_cluster(); }
void        Zelph::pause_cluster_recording() const { _pImpl->pause_cluster_recording(); }
void        Zelph::resume_cluster_recording() const { _pImpl->resume_cluster_recording(); }
std::string Zelph::active_cluster_name() const { return _pImpl->active_cluster_name(); }

std::vector<std::pair<std::string, size_t>> Zelph::list_clusters() const { return _pImpl->list_clusters(); }

std::vector<Node>                  Zelph::cluster_nodes(const std::string& name) const { return _pImpl->cluster_nodes(name); }
std::vector<std::pair<Node, Node>> Zelph::cluster_declarations(const std::string& name) const { return _pImpl->cluster_declarations(name); }

bool Zelph::merge_cluster(const std::string& from, const std::string& to) const
{
    const bool merged = _pImpl->merge_cluster(from, to);
    if (merged && from != to && _on_cluster_closed) _on_cluster_closed(from, &to);
    return merged;
}

void Zelph::move_into_cluster(const std::string& name, const std::vector<Node>& nodes) const
{
    _pImpl->move_into_cluster(name, nodes);
}

bool Zelph::open_firing_journal(const char* const name) const { return _pImpl->open_firing_journal(name); }

// The report of the closed cluster that merge_cluster gives with each merge,
// the merge of a scratch included.
void Zelph::keep_firing_journal(const char* const name) const
{
    const std::string to = _pImpl->keep_firing_journal();
    if (_on_cluster_closed) _on_cluster_closed(name, &to);
}

void              Zelph::close_firing_journal() const { _pImpl->close_firing_journal(); }
std::vector<Node> Zelph::firing_journal_nodes() const { return _pImpl->firing_journal_nodes(); }
void              Zelph::move_into_firing_journal(const std::vector<Node>& nodes) const { _pImpl->move_into_firing_journal(nodes); }

// Each drop of a cluster is this one, whichever name is used.
size_t Zelph::drop_scratch_cluster(const std::string& name) const
{
    return drop_cluster(name);
}

// Destructive: removes every node recorded in the cluster, including all
// of their edges and names. Nodes that no longer exist (e.g. merged away
// by set_name) are skipped silently.
size_t Zelph::drop_cluster(const std::string& name) const
{
    if (_on_cluster_closed) _on_cluster_closed(name, nullptr);
    keep_shared_declarations(name);

    // Removing node by node is the wrong instrument for a drop, for exactly
    // the reason unmark_rule_pattern gives: remove_node invalidates through
    // the WHOLESALE funnel, which DISARMS the genuine-structure store for the
    // rest of the session. Re-entering a rule that already exists was enough
    // -- one duplicate line took `genuine walks` from 0 to 12 on a four-fact
    // graph, and that store is what keeps get_fact_structures off the
    // adjacency walk (see the 3 August regression, where losing it tripled
    // the Jacobian workload). A cluster that a user or script drops had the
    // identical consequence: a dry run within a cluster of its own, dropped
    // once, disarmed the store for the rest of the session.
    //
    // So: verify that every node is unreferenced from outside, then remove
    // each with the invalidation a fact() of the same shape performs. Any
    // node that fails the test sends the WHOLE cluster down the general path
    // -- this is an optimisation of a removal, never a weakening of one.
    //
    // Read before take_cluster, which drops them.
    const std::vector<Node> unmarked = _pImpl->cluster_unmarked(name);
    const auto              refuted  = _pImpl->cluster_refutations(name);
    const std::vector<Node> nodes    = _pImpl->cluster_nodes(name);
    const bool              derived  = _pImpl->cluster_derived(name);

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
                    return drop_cluster_cascading(name);
            }
        }

        structures.emplace_back(n, fs);
    }

    refuse_opacity_removal([&doomed](const Node nd)
                           { return doomed.count(nd) != 0; });
    _pImpl->take_cluster(name); // the bookkeeping goes either way

    adjacency_set gone;
    for (const auto& entry : structures)
        gone.insert(entry.first);

    // A cluster that a run derived into gives the record up when the drop
    // takes anything back -- a node, or a claim the run made of a pattern.
    announce_removal(gone, derived && (!gone.empty() || !unmarked.empty()));

    // As remove_doomed does on the general path, the refuted index discards
    // the nodes that have been removed, together with the subjects
    // associated with the markings within them, and the pattern set also
    // discards them, as does the record of set patterns: a dropped
    // question's set pattern otherwise kept every new fact querying the
    // record for the rest of the session.
    forget_refuted(gone, true);
    forget_rule_patterns(gone);
    forget_set_patterns(gone);

    size_t removed = 0;
    for (const auto& [n, fs] : structures)
    {
        if (!_pImpl->exists(n)) continue;

        if (fs.subject != 0 && fs.predicate != 0)
            invalidate_fact_structures_for(fs.subject, fs.predicate, fs.objects, n);

        _pImpl->remove(n);
        ++removed;
    }

    // The names in a single batch, as prune_nodes removes them: a label
    // shared by several of these nodes is settled once, and it is never
    // handed to a node that the same drop removes next.
    remove_names_of(gone);

    restore_rule_patterns(unmarked);
    restore_refutations(refuted);

    return removed;
}

// A drop takes back the declaration that fact() wrote for the first fact of a
// predicate (Network::note_declaration), along with the facts that used the
// predicate. Something outside the record might still need it: a fact
// utilizing the predicate reads it as a relation type via the declaration, and
// a statement concerning the declaration holds it as a part. Either keeps the
// declaration. Taken along, the first left `likes` devoid of its declaration,
// while the second sent the drop down the cascade, which also took the
// statement. The predicate gained its declaration within the record, making
// such a user rare, and the traversal across its adjacency occurs solely
// during a drop that holds the declaration of a predicate older than the
// record.
void Zelph::keep_shared_declarations(const std::string& name) const
{
    const std::vector<std::pair<Node, Node>> declared = _pImpl->cluster_declarations(name);
    if (declared.empty()) return;

    const std::vector<Node>        recorded = _pImpl->cluster_nodes(name);
    const std::unordered_set<Node> doomed(recorded.begin(), recorded.end());

    const auto used_beyond = [&](const Node declaration, const Node predicate)
    {
        for (const Node fact : _pImpl->get_left(predicate))
        {
            if (fact == declaration || doomed.count(fact) != 0 || !Impl::is_hash(fact) || Impl::is_var(fact)) continue;
            if (predicate_of(fact) == predicate) return true;
        }
        for (const adjacency_set& side : {_pImpl->get_left(declaration), _pImpl->get_right(declaration)})
            for (const Node m : side)
                if (m != predicate && m != core.IsA && m != core.RelationTypeCategory && doomed.count(m) == 0) return true;
        return false;
    };

    std::vector<Node> shared;
    for (const auto& [declaration, predicate] : declared)
    {
        if (doomed.count(declaration) == 0 || doomed.count(predicate) != 0 || !_pImpl->exists(declaration)) continue;
        if (used_beyond(declaration, predicate)) shared.push_back(declaration);
    }
    if (!shared.empty()) _pImpl->move_into_cluster("", shared);
}

// The general path, for a cluster that something outside refers to: the
// upward cascade of remove_node, gathered across all the cluster's nodes at
// once, whose union, as collect_doomed shows, is what removing them one
// after another takes.
size_t Zelph::drop_cluster_cascading(const std::string& name) const
{
    // Read before take_cluster, which drops them.
    const std::vector<Node> unmarked = _pImpl->cluster_unmarked(name);
    const auto              refuted  = _pImpl->cluster_refutations(name);
    const bool              derived  = _pImpl->cluster_derived(name);
    const std::vector<Node> nodes    = _pImpl->cluster_nodes(name);
    if (nodes.empty() && unmarked.empty() && refuted.empty())
    {
        _pImpl->take_cluster(name);
        return 0;
    }

    invalidate_fact_structures_cache();

    // What the cluster created, which is also what it added: a membership
    // among these takes nothing older along with it (see collect_doomed).
    adjacency_set added;
    for (const Node n : nodes)
        added.insert(n);

    adjacency_set doomed;
    for (const Node n : nodes)
    {
        if (_pImpl->exists(n)) collect_doomed(n, doomed, nullptr, &added);
    }

    // Refused before the record goes, as on the fast path: the cluster
    // remains intact, allowing it to be dropped once no fact of the
    // predicate holds a statement.
    refuse_opacity_removal([&doomed](const Node nd)
                           { return doomed.count(nd) != 0; });
    _pImpl->take_cluster(name);

    const bool takes_derivations = derived && (!doomed.empty() || !unmarked.empty()); // as on the fast path
    if (!doomed.empty())
    {
        adjacency_set named;
        remove_doomed(doomed, &named, takes_derivations, true);
        remove_names_of(named);
    }
    else if (takes_derivations)
    {
        announce_removal({}, true);
    }

    restore_rule_patterns(unmarked);
    restore_refutations(refuted);

    // Counting the nodes in the cluster would UNDERSTATE the impact: removing
    // a single node takes the facts it is part of, and those facts might not
    // reside within the cluster at all. What the caller reports is what
    // actually went.
    return doomed.size();
}

// Re-mark the patterns whose marking the dropped cluster revoked. Runs AFTER
// the removals, because a pattern the drop took with it is not to be marked
// again -- and it may well have been taken: a rule built inside the cluster
// records its patterns, and asserting one of them inside the same cluster
// revokes the marking of a node the cluster itself created.
void Zelph::restore_rule_patterns(const std::vector<Node>& patterns) const
{
    if (patterns.empty()) return;

    // A statement whose claim is revoked ceases to be data, and to the
    // record of additions that constitutes a removal: a claim introduced
    // after the most recent run leaves the record, and one that was observed
    // during the last run leaves its predicate behind, signalling a seeded run
    // that a negation could hold once more. Announced while the statement
    // continues to appear as asserted.
    {
        adjacency_set revoked;
        for (const Node p : patterns)
            if (_pImpl->exists(p)) revoked.insert(p);
        announce_removal(revoked);
    }

    // A marking represents the engine's bookkeeping for a node that predated
    // the cluster, meaning no cluster owns the one restored here. Created
    // while the caller's cluster was active, it was recorded within that
    // cluster: dropping that cluster afterwards removed it while the index
    // still said "pattern", and a subsequent .save and .load read the pattern
    // as a claim.
    const ClusterRecordingPause pause(*this);

    const Node pred = rule_pattern_predicate(true);
    auto*      self = const_cast<Zelph*>(this);

    std::unique_lock lock(_pImpl->_rule_patterns_mtx);
    for (const Node p : patterns)
    {
        if (!_pImpl->exists(p)) continue;

        // Marking twice is not the same as marking once: the fact is
        // hash-consed, so re-creating it is a no-op, but the index is a set
        // and would grow. insert answers both questions at once.
        self->fact(p, core.IsA, {pred}, 1, Writer::pattern_mark);
        _pImpl->_rule_patterns.insert(p);
        invalidate_predicate_index_of(p);
    }
    _pImpl->_has_rule_patterns.store(!_pImpl->_rule_patterns.empty(), std::memory_order_release);
}

// The mark returns just as a drop restores it, owned by no cluster. A cluster
// that journaled the revocation while it remained active has nothing further
// to take back, and its drop announced the claim as removed once again.
void Zelph::restore_mention(const Node node) const
{
    _pImpl->forget_unmarked(node);
    restore_rule_patterns({node});
}

// Put back what the dropped cluster's refutations changed in the facts it
// did not create (Network::note_refutation). Runs after
// restore_rule_patterns: by that time, a marking that the cluster claimed is
// a pattern again, and the fact it refuted is to revert to its prior state.
// Only the modifications the refutation itself wrote are reversed -- an
// index entry the fact lacked, a 0 that is still there -- while a fact that
// was removed by the drop is skipped.
void Zelph::restore_refutations(const std::vector<std::pair<Node, Network::RefutationPrior>>& priors) const
{
    if (priors.empty()) return;

    // The pause of restore_rule_patterns, for the same reason.
    const ClusterRecordingPause pause(*this);

    std::vector<Node> unrefuted;
    for (const auto& [fact, prior] : priors)
    {
        if (!_pImpl->exists(fact)) continue;

        if (!prior.refuted && is_refuted_fact(fact)) unrefuted.push_back(fact);

        const Node predicate = predicate_of(fact);
        if (predicate == 0 || !_pImpl->has_synapse(fact, predicate) || _pImpl->edge_weight(fact, predicate) != 0.0) continue;
        if (prior.weighted)
            _pImpl->set_edge_weight(fact, predicate, prior.weight);
        else
            _pImpl->erase_weight(fact, predicate);
    }

    for (const Node f : unrefuted)
        invalidate_predicate_index_of(f);

    std::unique_lock lock(_pImpl->_refuted_facts_mtx);
    for (const Node f : unrefuted)
        _pImpl->_refuted_facts.erase(f);
    _pImpl->_has_refuted_facts.store(!_pImpl->_refuted_facts.empty(), std::memory_order_release);
}