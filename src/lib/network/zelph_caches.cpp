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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace zelph::network;

namespace
{
    // Whether the conditions `conditions` represents hold a variable: the
    // members Zelph::condition_set_members reads, and the members of a
    // member that itself qualifies as a container, which
    // Reasoning::evaluate enters as well. `holds` inquires about one
    // member. That reader decides what constitutes a container -- a
    // collection or a set constant, into which a construction writes its
    // conditions -- and reads no statement: a statement as a rule's subject
    // serves as its sole condition, and the closure reaches it as the
    // subject of the `=>` fact. `entered` holds the containers read so far,
    // apart from the nodes a walk has visited: the walk visits a set
    // constant as the subject of the `=>` fact before its members are read.
    template <class Holds>
    bool conditions_hold_variable(const Zelph* n, const Node conditions, std::unordered_set<Node>& entered, const Holds& holds)
    {
        if (conditions == 0 || Zelph::is_var(conditions)) return false;
        if (!entered.insert(conditions).second) return false; // cycle protection

        adjacency_set members;
        if (!n->condition_set_members(conditions, members)) return false;
        for (const Node m : members)
            if (holds(m) || conditions_hold_variable(n, m, entered, holds)) return true;
        return false;
    }

    // Whether any member of the container `c` -- whose members are attached
    // via membership facts, which no fact structure leads to -- is one that
    // `holds` answers yes for.
    template <class Holds>
    bool some_member(const Zelph* n, const Node c, const Holds& holds)
    {
        for (const Node rel : n->get_right(c))
        {
            if (n->predicate_of(rel) != n->core.PartOf) continue;
            adjacency_set objects;
            const Node    m = n->parse_fact(rel, objects, 0);
            if (m != 0 && objects.count(c) == 1 && holds(m)) return true;
        }
        return false;
    }

    bool var_in_closure_walk(const Zelph* n, Node nd, std::unordered_set<Node>& visited, Zelph::VariableReading reading);

    // The text contained within a rule's own collection or a set constant,
    // read by the walk below: a variable member, a fact or set constant the
    // walk finds one in, or a rule's own collection containing such a
    // member.
    bool members_text_walk(const Zelph* n, const Node c, std::unordered_set<Node>& visited, const Zelph::VariableReading reading)
    {
        return some_member(n, c, [&](const Node m)
                           {
                               if (Zelph::is_var(m)) return true;
                               if (Zelph::is_hash(m)) return var_in_closure_walk(n, m, visited, reading);
                               return n->is_rule_template(m) && visited.insert(m).second && members_text_walk(n, m, visited, reading); });
    }

    // Whether the rule text beneath `n` holds a membership whose object
    // is a rule's own collection containing a variable within its text:
    // `a in @{Y}` as a condition, `c in @{Y}` as a consequence.
    // The membership constitutes the collection's content and holds a
    // variable solely via its member, yet a rule that states it holds the
    // collection's. The walk reads facts and the members of a rule's own
    // collection or a conjunction set, halting at values, constants, and set
    // constants: the members of a set constant branch from it amidst all
    // facts that name it, and data can make those any number. `read` holds
    // the nodes read so far.
    bool membership_holds_variable(const Zelph* z, const Node n, std::unordered_set<Node>& read)
    {
        if (n == 0 || Zelph::is_var(n) || !read.insert(n).second) return false;

        const auto below = [&](const Node m)
        { return membership_holds_variable(z, m, read); };
        if (!Zelph::is_hash(n))
        {
            if (!z->is_rule_template(n) && !z->check_fact(n, z->core.IsA, {z->core.Conjunction}).is_known()) return false;
            return some_member(z, n, below);
        }

        const auto structs = get_fact_structures(z, n, 1);
        for (const auto& fs : *structs)
        {
            if (fs.predicate == z->core.PartOf
                && std::any_of(fs.objects.begin(), fs.objects.end(), [z](const Node o)
                               { return z->is_rule_template(o) && z->holds_variable_in_text(o); }))
                return true;
            if (below(fs.subject) || below(fs.predicate) || std::any_of(fs.objects.begin(), fs.objects.end(), below)) return true;
        }
        return false;
    }

    // Whether the rule a `=>` fact over `subject` and `objects` states holds
    // such a membership, in its conditions -- read as conditions_hold_variable
    // reads them -- or in its consequences.
    bool rule_memberships_hold_variable(const Zelph* z, const Node subject, const adjacency_set& objects)
    {
        std::unordered_set<Node> read;
        const auto               in_text = [&](const Node m)
        { return membership_holds_variable(z, m, read); };

        std::unordered_set<Node> entered;
        return conditions_hold_variable(z, subject, entered, in_text) || in_text(subject)
            || std::any_of(objects.begin(), objects.end(), in_text);
    }

    // Reference walk based on reconstruction -- the pre-flag implementation
    // of unification.cpp's contains_variable_deep, preserved exactly as-is
    // to ensure the fallback behaviour following binary loads / trusted
    // imports / removals matches historical semantics precisely, except for
    // the conditions of a `=>` fact and the members of a rule's own
    // collection and of a set constant, which it reads as
    // Zelph::var_in_closure says, by `reading`. Depth remains fixed at 1
    // (for logging purposes only).
    bool var_in_closure_walk(const Zelph* n, const Node nd, std::unordered_set<Node>& visited, const Zelph::VariableReading reading)
    {
        if (nd == 0) return false;
        if (Zelph::is_var(nd)) return true;
        if (!Zelph::is_hash(nd)) return false;        // plain atom -> no internal structure
        if (!visited.insert(nd).second) return false; // cycle protection

        const bool text    = reading == Zelph::VariableReading::Text;
        const auto structs = get_fact_structures(n, nd, 1);
        if (structs->empty()) return text && members_text_walk(n, nd, visited, reading); // a set constant

        // A rule's own collection is entered as Zelph::fact enters it: not
        // as the object of a membership, which is its content (`enter`
        // false).
        const auto component = [&](const Node c, const bool enter)
        {
            if (var_in_closure_walk(n, c, visited, reading)) return true;
            return text && enter && c != 0 && !Zelph::is_hash(c) && !Zelph::is_var(c) && n->is_rule_template(c)
                && visited.insert(c).second && members_text_walk(n, c, visited, reading);
        };

        for (const auto& fs : *structs)
        {
            if (component(fs.subject, true)) return true;
            if (component(fs.predicate, true)) return true;
            for (const Node o : fs.objects)
                if (component(o, fs.predicate != n->core.PartOf)) return true;
            if (text && fs.predicate == n->core.Causes)
            {
                std::unordered_set<Node> entered;
                if (conditions_hold_variable(n, fs.subject, entered, [&](const Node m)
                                             { return var_in_closure_walk(n, m, visited, reading); }))
                    return true;
                if (rule_memberships_hold_variable(n, fs.subject, fs.objects)) return true;
            }
        }
        return false;
    }

    // Zelph::holds_variable_in_text, with the containers that have
    // been entered so far.
    bool text_holds_variable(const Zelph* z, const Node n, std::unordered_set<Node>& entered)
    {
        if (n == 0) return false;
        if (Zelph::is_var(n)) return true;
        if (Zelph::is_hash(n)) return z->var_in_closure(n);
        if (!z->is_rule_template(n) && !z->check_fact(n, z->core.IsA, {z->core.Conjunction}).is_known()) return false;
        if (!entered.insert(n).second) return false; // cycle protection
        return some_member(z, n, [&](const Node m)
                           { return text_holds_variable(z, m, entered); });
    }
}

bool Zelph::try_get_fact_structures_cached(Node fact, FactStructurePtr& out) const
{
    // If cache is currently empty/known-invalid, avoid locking
    if (!_pImpl->_fs_cache_has_entries.load(std::memory_order_acquire))
    {
        if (logging_active()) _fs_cache_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::shared_lock lock(_pImpl->_fs_cache_mtx);
    auto             it = _pImpl->_fs_cache.find(fact);
    if (it == _pImpl->_fs_cache.end())
    {
        if (logging_active()) _fs_cache_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (logging_active()) _fs_cache_hits.fetch_add(1, std::memory_order_relaxed);
    out = it->second; // shared_ptr copy: one atomic increment, no allocation
    return true;
}

void Zelph::store_fact_structures_cached(Node fact, FactStructurePtr value) const
{
    // A bulk pass computes each structure once and never asks again, so
    // remembering them is pure cost: an exclusive lock per store, and a map
    // that grows into the millions and slows every later lookup. Measured on
    // a 1.5 M-node prune: 45.1 s with the cache against 16.8 s without it,
    // and 58.4 s against 27.2 s on a SINGLE core -- with several threads
    // collecting, this lock is also what stopped them scaling past four.
    //
    // Correctness cannot depend on it: get_fact_structures recomputes on a
    // miss and returns what it computed, never a re-read. The genuine
    // structure STORE, where absence IS meaningful, is a different thing and
    // is disarmed by the removal path anyway.
    if (_fs_cache_suspended.load(std::memory_order_relaxed)) return;

    {
        std::unique_lock lock(_pImpl->_fs_cache_mtx);
        _pImpl->_fs_cache[fact] = std::move(value);
    }
    _pImpl->_fs_cache_has_entries.store(true, std::memory_order_release);
}

void Zelph::invalidate_fact_structures_cache() const noexcept
{
    if (logging_active()) _fs_cache_full_clears.fetch_add(1, std::memory_order_relaxed);

    // Disarm and clear the fact-path stores. Single implementation shared
    // with the .fact-stores command: every path through this funnel either
    // bypasses triple-level construction (trusted imports, binary loads)
    // or destroys topology (removals, merges, name-merge) -- stale store
    // entries must never resurface afterwards.
    disable_fact_stores();

    _pImpl->invalidate_predicate_index();

    // If cache already empty, do nothing (avoid lock)
    if (!_pImpl->_fs_cache_has_entries.exchange(false, std::memory_order_acq_rel))
        return;

    std::unique_lock lock(_pImpl->_fs_cache_mtx);
    _pImpl->_fs_cache.clear();
}

// Per-fact cache invalidation, called by fact() AFTER the new relation's
// edges are drawn. Replaces the former wholesale clear on every new fact,
// which kept the cache near-permanently empty on rule-heavy workloads
// (21.8k created facts => 1.28M full get_fact_structures reconstructions
// in the Jacobian diffby phase -- the dominant cost in the perf profile).
//
// Correctness argument. The identifier of a fact node is
// generated via create_hash(predicate, subject, objects), ensuring that
// every node holds precisely ONE genuine triple, established
// permanently upon creation; the monotonic growth of the graph can
// only introduce new reconstruction candidates (each skip heuristic
// consistently shifts toward reducing skips), and hash verification
// prunes any ambiguous candidate set back to the genuine
// interpretation. What the growth process can genuinely modify,
// therefore, is:
//  (1) the new relation node itself and its components (their adjacency
//      grew, changing candidate collection),
//  (2) nodes whose child-fact heuristic inspects the components'
//      neighborhoods -- covered by one BIDIRECTIONAL adjacency level
//      around subject and objects; deeper levels (the heuristic reads up
//      to three hops) only feed checks whose outcome hash verification
//      makes result-neutral,
//  (3) across the entire scope: relation-type declarations (P ~ ->).
//      Predicate detection consults check_fact(p, IsA,
//      RelationTypeCategory) per right neighbor, so a new declaration
//      can change ANY cached entry -- that case falls back to the full
//      clear (rare: module load time only).
// Residual risk, consciously accepted: entries preserved as UNVERIFIED
// (no candidate hash-verifies) are not re-evaluated during
// deeper-level growth. The entire suite's `.semi-naive check`
// equivalence mechanism serves as a backstop for this.
//
// The bidirectional restriction keeps hubs harmless: nil sits in the
// RIGHT set of every terminating cons cell, but is bidirectional only
// with the few facts using it as SUBJECT. The smaller adjacency side is
// iterated with O(1) edge probes into the other. A neighborhood beyond
// stale_budget degrades to the full clear -- the anchoring budget
// philosophy: never unsound, never worse than the old semantics.
//
// The predicate-index coupling of the wholesale variant is kept: a built
// index for this predicate is stale after any new fact. That call is one
// atomic exchange when (as in the math workloads) no index exists.
void Zelph::invalidate_fact_structures_for(const Node subject, const Node predicate, const adjacency_set& objects, const Node relation) const
{
    _pImpl->invalidate_predicate_index();

    const auto full_clear = [this]
    {
        if (logging_active()) _fs_cache_full_clears.fetch_add(1, std::memory_order_relaxed);
        if (!_pImpl->_fs_cache_has_entries.exchange(false, std::memory_order_acq_rel)) return;
        std::unique_lock lock(_pImpl->_fs_cache_mtx);
        _pImpl->_fs_cache.clear();
    };

    // (3) relation-type declarations change predicate detection globally
    if (predicate == core.IsA && objects.count(core.RelationTypeCategory) != 0)
    {
        invalidate_relation_type_set();
        full_clear();
        return;
    }

    if (!_pImpl->_fs_cache_has_entries.load(std::memory_order_acquire)) return;

    constexpr size_t stale_budget = 256;

    std::vector<Node> stale;
    stale.reserve(16);
    stale.push_back(relation);
    stale.push_back(subject);
    stale.push_back(predicate); // cheap; a predicate that is itself a fact node gained a left edge
    for (const Node o : objects)
        stale.push_back(o);

    // Bidirectional neighbors of c, iterating the smaller adjacency side.
    const auto add_bidirectional_neighbors = [&](const Node c) -> bool
    {
        const bool          iterate_right = _pImpl->right_count_of(c) <= _pImpl->left_count_of(c);
        const adjacency_set side          = iterate_right ? _pImpl->get_right(c) : _pImpl->get_left(c);
        for (const Node n : side)
        {
            const bool bidirectional = iterate_right ? has_left_edge(c, n)   // n -> c exists too?
                                                     : has_right_edge(c, n); // c -> n exists too?
            if (!bidirectional) continue;
            stale.push_back(n);
            if (stale.size() > stale_budget) return false;
        }
        return true;
    };

    bool bounded = add_bidirectional_neighbors(subject);
    for (const Node o : objects)
    {
        if (!bounded) break;
        if (o != subject) bounded = add_bidirectional_neighbors(o);
    }

    if (!bounded)
    {
        full_clear();
        return;
    }

    size_t erased = 0;
    {
        std::unique_lock lock(_pImpl->_fs_cache_mtx);
        for (const Node n : stale)
            erased += _pImpl->_fs_cache.erase(n);
    }
    if (erased != 0 && logging_active()) _fs_cache_stale_erased.fetch_add(erased, std::memory_order_relaxed);
}

bool Zelph::is_relation_type_declaration(const Node fact) const
{
    if (fact == 0 || parse_relation(fact) != core.IsA) return false;

    adjacency_set objects;
    parse_fact(fact, objects, 0);
    return objects.count(core.RelationTypeCategory) != 0;
}

void Zelph::erase_fact_structures(const std::vector<Node>& nodes) const noexcept
{
    if (nodes.empty()) return;
    if (!_pImpl->_fs_cache_has_entries.load(std::memory_order_acquire)) return;

    size_t erased = 0;
    {
        std::unique_lock lock(_pImpl->_fs_cache_mtx);
        for (const Node n : nodes)
            erased += _pImpl->_fs_cache.erase(n);
    }
    if (erased != 0 && logging_active()) _fs_cache_stale_erased.fetch_add(erased, std::memory_order_relaxed);
}

std::shared_ptr<const ankerl::unordered_dense::set<Node>> Zelph::relation_type_set() const
{
    uint64_t gen;
    {
        std::shared_lock lock(_pImpl->_rel_types_mtx);
        if (_pImpl->_rel_types) return _pImpl->_rel_types;
        gen = _pImpl->_rel_types_gen;
    }

    // Build outside the lock: one pass over the declaration facts, each
    // candidate confirmed with the exact probe this set replaces.
    auto set = std::make_shared<ankerl::unordered_dense::set<Node>>();
    for (const Node p : get_sources(core.IsA, core.RelationTypeCategory, false))
    {
        if (check_fact(p, core.IsA, {core.RelationTypeCategory}).is_known())
            set->insert(p);
    }

    std::unique_lock lock(_pImpl->_rel_types_mtx);
    if (_pImpl->_rel_types) return _pImpl->_rel_types; // a concurrent build won
    if (_pImpl->_rel_types_gen != gen)
    {
        // Invalidated while building (new declaration): the snapshot is
        // valid for THIS caller -- equivalent to probing just before the
        // declaration -- but must not be stored.
        return set;
    }
    _pImpl->_rel_types = std::move(set);
    return _pImpl->_rel_types;
}

void Zelph::invalidate_relation_type_set() const
{
    _pImpl->invalidate_relation_type_set();
}

Zelph::FsCacheStats Zelph::fs_cache_stats() const
{
    return {_fs_cache_hits.load(std::memory_order_relaxed),
            _fs_cache_misses.load(std::memory_order_relaxed),
            _fs_cache_full_clears.load(std::memory_order_relaxed),
            _fs_cache_stale_erased.load(std::memory_order_relaxed)};
}

void Zelph::reset_fs_cache_stats() const
{
    _fs_cache_hits.store(0, std::memory_order_relaxed);
    _fs_cache_misses.store(0, std::memory_order_relaxed);
    _fs_cache_full_clears.store(0, std::memory_order_relaxed);
    _fs_cache_stale_erased.store(0, std::memory_order_relaxed);
}

bool Zelph::var_in_closure(const Node nd, const VariableReading reading) const
{
    if (nd == 0) return false;
    if (Impl::is_var(nd)) return true;
    if (!Impl::is_hash(nd)) return false;

    if (_pImpl->_template_vars_authoritative.load(std::memory_order_acquire))
    {
        if (logging_active()) _var_flag_queries.fetch_add(1, std::memory_order_relaxed);
        std::shared_lock lock(_pImpl->_template_vars_mtx);
        return _pImpl->_template_vars.find(nd) != _pImpl->_template_vars.end()
            || (reading == VariableReading::Text && !_pImpl->_rule_text_vars.empty() && _pImpl->_rule_text_vars.contains(nd));
    }

    if (logging_active()) _var_flag_fallbacks.fetch_add(1, std::memory_order_relaxed);
    std::unordered_set<Node> visited;
    return var_in_closure_walk(this, nd, visited, reading);
}

bool Zelph::conditions_hold_variable(const Node conditions) const
{
    std::unordered_set<Node> entered;
    return ::conditions_hold_variable(this, conditions, entered, [this](const Node m)
                                      { return var_in_closure(m); });
}

bool Zelph::rule_memberships_hold_variable(const Node subject, const adjacency_set& objects) const
{
    return ::rule_memberships_hold_variable(this, subject, objects);
}

bool Zelph::holds_variable_in_text(const Node n) const
{
    std::unordered_set<Node> entered;
    return text_holds_variable(this, n, entered);
}

bool Zelph::component_holds_text_variable(const Node component) const
{
    if (Impl::is_hash(component))
    {
        if (!_pImpl->_rule_text_vars_any.load(std::memory_order_acquire)) return false;
        std::shared_lock lock(_pImpl->_template_vars_mtx);
        return _pImpl->_rule_text_vars.contains(component);
    }
    // The id test first: for every other atom, this is the
    // total expense.
    return is_rule_template(component) && holds_variable_in_text(component);
}

bool Zelph::is_asserted_fact(const Node fact) const
{
    // A refuted fact is the third way a fact node can be in the graph without
    // anyone claiming it holds -- beside a rule's own pattern and a pattern
    // carrying variables. `¬(F)` claims the opposite, which is a claim about
    // F and not an instance of it.
    return !is_rule_pattern(fact) && !is_refuted_fact(fact) && !var_in_closure(fact);
}

std::shared_ptr<const adjacency_set> Zelph::unasserted_snapshot() const
{
    auto out = std::make_shared<adjacency_set>();

    if (_pImpl->_has_rule_patterns.load(std::memory_order_acquire))
    {
        std::shared_lock lock(_pImpl->_rule_patterns_mtx);
        for (const Node n : _pImpl->_rule_patterns)
            out->insert(n);
    }

    if (_pImpl->_has_refuted_facts.load(std::memory_order_acquire))
    {
        std::shared_lock lock(_pImpl->_refuted_facts_mtx);
        for (const Node n : _pImpl->_refuted_facts)
            out->insert(n);
    }

    if (_pImpl->_template_vars_authoritative.load(std::memory_order_acquire))
    {
        std::shared_lock lock(_pImpl->_template_vars_mtx);
        for (const auto& entry : _pImpl->_template_vars)
            out->insert(entry.first);
        for (const Node n : _pImpl->_rule_text_vars)
            out->insert(n);
    }

    if (out->empty()) return nullptr;
    return out;
}

Zelph::VarClosureStats Zelph::var_closure_stats() const
{
    return {_var_flag_queries.load(std::memory_order_relaxed),
            _var_flag_fallbacks.load(std::memory_order_relaxed)};
}

void Zelph::reset_var_closure_stats() const
{
    _var_flag_queries.store(0, std::memory_order_relaxed);
    _var_flag_fallbacks.store(0, std::memory_order_relaxed);
}

bool Zelph::try_get_template_vars(const Node nd, std::shared_ptr<const std::unordered_set<Node>>& out) const
{
    if (!_pImpl->_template_vars_authoritative.load(std::memory_order_acquire)) return false;

    if (logging_active()) _tvars_hits.fetch_add(1, std::memory_order_relaxed);
    std::shared_lock lock(_pImpl->_template_vars_mtx);
    const auto       it = _pImpl->_template_vars.find(nd);
    out                 = it == _pImpl->_template_vars.end() ? nullptr : it->second;
    return true;
}

void Zelph::count_template_vars_walk() const
{
    if (logging_active()) _tvars_walks.fetch_add(1, std::memory_order_relaxed);
}

Zelph::TemplateVarsStats Zelph::template_vars_stats() const
{
    return {_tvars_hits.load(std::memory_order_relaxed),
            _tvars_walks.load(std::memory_order_relaxed)};
}

void Zelph::reset_template_vars_stats() const
{
    _tvars_hits.store(0, std::memory_order_relaxed);
    _tvars_walks.store(0, std::memory_order_relaxed);
}

bool Zelph::try_get_genuine_structure(const Node fact, FactStructurePtr& out) const
{
    if (!_pImpl->_genuine_authoritative.load(std::memory_order_acquire)) return false;

    std::shared_lock lock(_pImpl->_genuine_mtx);
    const auto       it = _pImpl->_genuine.find(fact);
    if (it == _pImpl->_genuine.end()) return false;

    out = it->second; // shared_ptr copy: one atomic increment, no allocation
    return true;
}

void Zelph::count_genuine_hit() const
{
    if (logging_active()) _genuine_hits.fetch_add(1, std::memory_order_relaxed);
}

void Zelph::count_genuine_walk() const
{
    if (logging_active()) _genuine_walks.fetch_add(1, std::memory_order_relaxed);
}

Zelph::GenuineStats Zelph::genuine_stats() const
{
    return {_genuine_hits.load(std::memory_order_relaxed),
            _genuine_walks.load(std::memory_order_relaxed)};
}

void Zelph::reset_genuine_stats() const
{
    _genuine_hits.store(0, std::memory_order_relaxed);
    _genuine_walks.store(0, std::memory_order_relaxed);
}

bool Zelph::fact_stores_enabled() const
{
    return _pImpl->_template_vars_authoritative.load(std::memory_order_acquire)
        && _pImpl->_genuine_authoritative.load(std::memory_order_acquire);
}

void Zelph::disable_fact_stores() const
{
    _pImpl->_template_vars_authoritative.store(false, std::memory_order_release);
    {
        // Clear, not just disarm: entries may later reference removed
        // nodes, and freeing the memory is the point of the switch.
        std::unique_lock lock(_pImpl->_template_vars_mtx);
        _pImpl->_template_vars.clear();
        _pImpl->_rule_text_vars.clear();
        _pImpl->_rule_text_vars_any.store(false, std::memory_order_release);
    }

    _pImpl->_genuine_authoritative.store(false, std::memory_order_release);
    {
        std::unique_lock lock(_pImpl->_genuine_mtx);
        _pImpl->_genuine.clear();
    }
}
