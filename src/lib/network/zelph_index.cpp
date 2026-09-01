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

#include "network.hpp"
#include "zelph_impl.hpp"

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace zelph::network;

// The objects of `rel`, read from adjacency alone: everything the
// relation is pointed at by and does not point back at. Computed ONCE
// per relation, since the subject test below needs the whole set.
//
// EMPTY does not mean "no objects": a self-fact stores nothing but its
// subject on the right, and that subject is its object as well. The
// subject is not known here, so is_fact_subject handles that case.
adjacency_set Zelph::Impl::fact_objects_of(const adjacency_set& rel_left, const adjacency_set& rel_right)
{
    adjacency_set objects;

    for (const Node o : rel_right)
    {
        if (rel_left.count(o) == 0) objects.insert(o);
    }

    return objects;
}

// Is `cand` the SUBJECT of `rel`, whose predicate is `predicate` and
// whose objects are `objects` (from fact_objects_of)?
//
// The bidirectionality of a subject is not a discriminator: a fact
// that has `rel` as ITS subject -- a statement about the fact, a rule
// condition, the rule-pattern marking -- is linked to rel in both
// directions in exactly the same way. A node IS the hash of its
// triple, so recomputing that hash answers the question exactly.
//
// The two traversals below hold the adjacency locks and therefore
// cannot call get_fact_structures, which takes them itself; this is
// the same reading in pure hash arithmetic. Subject == predicate
// (`~ ~ ->`) needs no exemption: it hashes back like anything else.
bool Zelph::Impl::is_fact_subject(const Node rel, const Node predicate, const Node cand, const adjacency_set& rel_left, const adjacency_set& rel_right, const adjacency_set& objects)
{
    if (is_var(cand)) return false;
    if (rel_left.count(cand) == 0 || rel_right.count(cand) == 0) return false;

    if (!objects.empty()) return create_hash(predicate, cand, objects) == rel;

    // Self-fact: the candidate is its own object, and only then is
    // the extra set built -- once per candidate of a rare shape.
    return create_hash(predicate, cand, adjacency_set{cand}) == rel;
}

unsigned int Zelph::Impl::index_build_threads()
{
#ifdef __EMSCRIPTEN__
    return 1u; // single-threaded wasm build: forces the serial path below
#else
    const unsigned int hw = std::thread::hardware_concurrency();
    return hw == 0 ? 4u : hw;
#endif
}

// Phase 1: extract (subject, object) pairs from the predicate's
// relation nodes (parallel; see previous comments on locking and
// swap-bound random access). Returns the unsorted forward pairs.
std::vector<IndexPair> Zelph::Impl::extract_predicate_pairs(const Node predicate, const adjacency_set* skip) const
{
    // Same lock order as writers (connect): left before right.
    std::shared_lock<std::shared_mutex> lock_left(_smtx_left);
    std::shared_lock<std::shared_mutex> lock_right(_smtx_right);

    std::vector<IndexPair> fw;

    const auto rels_it = _right.find(predicate);
    if (rels_it == _right.end()) return fw;

    std::vector<Node> rels;
    rels.reserve(rels_it->second.size());
    for (const Node rel : rels_it->second)
        rels.push_back(rel);

    const size_t n_threads =
        rels.size() >= (size_t(1) << 15) ? index_build_threads() : 1;

    emit(io::OutputChannel::Diagnostic,
         "Building adjacency index over " + std::to_string(rels.size())
             + " relation nodes (" + std::to_string(n_threads) + " thread(s))...");

    std::vector<std::vector<IndexPair>> partial(n_threads);
    std::atomic<bool>                   failed{false};

    auto extract_chunk = [&](const size_t begin, const size_t end, std::vector<IndexPair>& out_pairs)
    {
        try
        {
            for (size_t i = begin; i < end; ++i)
            {
                const Node rel = rels[i];

                // A statement nobody claimed carries no edge: a rule's
                // ground pattern, or a fact carrying a variable. The
                // set is snapshotted by the caller because asking
                // directly would take its mutex under the adjacency
                // locks -- see Zelph::unasserted_snapshot.
                if (skip != nullptr && skip->count(rel) != 0) continue;

                const auto rl_it = _left.find(rel);
                const auto rr_it = _right.find(rel);
                if (rl_it == _left.end() || rr_it == _right.end()) continue;

                const adjacency_set& rel_left  = rl_it->second;
                const adjacency_set& rel_right = rr_it->second;

                // rel may be in _right[predicate] because the predicate
                // is the *subject* of rel (e.g. (P ~ ->)); such relations
                // contribute nothing below.
                if (rel_left.count(predicate) == 0) continue;

                // One pass, same reading as the direct traversal: the
                // objects on one side, the bidirectional nodes on the
                // other. A single bidirectional node IS the subject;
                // a second one is a fact that has rel as ITS subject
                // and only then is the exact triple hash paid for.
                adjacency_set objects;
                Node          subject       = 0;
                size_t        bidirectional = 0;

                for (const Node c : rel_right)
                {
                    if (rel_left.count(c) == 0)
                    {
                        objects.insert(c);
                    }
                    else
                    {
                        ++bidirectional;
                        subject = c;
                    }
                }

                if (bidirectional == 0) continue;

                // subject == predicate is the second case the fast
                // path may not decide: rel is a candidate because it
                // points AT the predicate, which it also does when
                // the predicate is its SUBJECT -- every `P ~ ->`
                // declaration is such a relation. The hash separates
                // the declaration from a genuine `~ ~ ->`.
                if (bidirectional > 1 || subject == predicate)
                {
                    subject = 0;
                    for (const Node c : rel_right)
                    {
                        if (is_fact_subject(rel, predicate, c, rel_left, rel_right, objects))
                        {
                            subject = c;
                            break;
                        }
                    }
                    if (subject == 0) continue;
                }

                if (is_var(subject)) continue;

                // A self-fact keeps its object in that bidirectional
                // entry, so the split above leaves it empty.
                if (objects.empty()) objects.insert(subject);

                for (const Node obj : objects)
                {
                    if (is_var(obj)) continue;
                    out_pairs.emplace_back(subject, obj);
                }
            }
        }
        catch (...)
        {
            failed.store(true, std::memory_order_relaxed);
        }
    };

    if (n_threads == 1)
    {
        extract_chunk(0, rels.size(), partial[0]);
    }
    else
    {
        std::vector<std::thread> workers;
        workers.reserve(n_threads);
        const size_t chunk = (rels.size() + n_threads - 1) / n_threads;
        for (size_t t = 0; t < n_threads; ++t)
        {
            const size_t begin = t * chunk;
            const size_t end   = std::min(rels.size(), begin + chunk);
            if (begin >= end) break;
            workers.emplace_back([&extract_chunk, &partial, begin, end, t]
                                 { extract_chunk(begin, end, partial[t]); });
        }
        for (auto& w : workers)
            w.join();
    }

    if (failed.load(std::memory_order_relaxed))
    {
        throw std::runtime_error("Predicate index build failed (worker exception)");
    }

    size_t total = 0;
    for (const auto& p : partial)
        total += p.size();

    fw.reserve(total);
    for (auto& p : partial)
    {
        fw.insert(fw.end(), p.begin(), p.end());
        p.clear();
        p.shrink_to_fit();
    }

    return fw;
}

// Phase 2: sort each direction and fill the maps with exact-size
// vectors. fw is sorted in place (and stays valid for persisting).
std::shared_ptr<const PredicateIndex> Zelph::Impl::index_from_pairs(std::vector<IndexPair>& fw)
{
    auto idx = std::make_shared<PredicateIndex>();

    std::vector<IndexPair> bw;
    bw.reserve(fw.size());
    for (const auto& [s, o] : fw)
        bw.emplace_back(o, s);

    std::atomic<bool> failed{false};

    auto fill = [&failed](std::vector<IndexPair>& pairs, PredicateIndex::adjacency& out)
    {
        try
        {
            std::sort(pairs.begin(), pairs.end());
            out.reserve(pairs.size());
            size_t i = 0;
            while (i < pairs.size())
            {
                size_t j = i;
                while (j < pairs.size() && pairs[j].first == pairs[i].first)
                    ++j;

                auto& vec = out[pairs[i].first];
                vec.reserve(j - i);
                for (size_t k = i; k < j; ++k)
                {
                    if (k > i && pairs[k] == pairs[k - 1]) continue;
                    vec.push_back(pairs[k].second);
                }
                i = j;
            }
        }
        catch (...)
        {
            failed.store(true, std::memory_order_relaxed);
        }
    };

#ifdef __EMSCRIPTEN__
    fill(bw, idx->backward);
    fill(fw, idx->forward);
#else
    std::thread bw_thread([&]
                          { fill(bw, idx->backward); });
    fill(fw, idx->forward);
    bw_thread.join();
#endif

    if (failed.load(std::memory_order_relaxed))
    {
        throw std::runtime_error("Predicate index build failed (fill exception)");
    }

    return idx;
}

std::shared_ptr<const PredicateIndex> Zelph::Impl::predicate_index(const Node predicate, const adjacency_set* skip) const
{
    {
        std::shared_lock lock(_pred_idx_mtx);
        const auto       it = _pred_idx_cache.find(predicate);
        if (it != _pred_idx_cache.end()) return it->second;
    }

    std::vector<IndexPair> fw;
#ifdef __EMSCRIPTEN__
    fw = extract_predicate_pairs(predicate, skip);
#else
    bool fresh = false;

    if (!try_load_pidx(predicate, fw))
    {
        fw    = extract_predicate_pairs(predicate, skip);
        fresh = true;
    }
#endif

    auto idx = index_from_pairs(fw); // sorts fw in place

    emit(io::OutputChannel::Diagnostic,
         "Adjacency index ready: " + std::to_string(fw.size()) + " edges.");

#ifndef __EMSCRIPTEN__
    if (fresh)
    {
        try_save_pidx(predicate, fw);
    }
#endif

    std::unique_lock lock(_pred_idx_mtx);
    const auto [it, inserted] = _pred_idx_cache.try_emplace(predicate, std::move(idx));
    if (inserted) _pred_idx_has_entries.store(true, std::memory_order_release);
    return it->second;
}

// Lookup that only consumes an already-built index; never triggers a
// build. Returns true if an index for the predicate exists (out then
// holds the complete answer, possibly empty).
bool Zelph::Impl::try_indexed_fact_lookup(Node predicate, Node node, bool forward, adjacency_set& out) const
{
    if (!_pred_idx_has_entries.load(std::memory_order_acquire)) return false;

    std::shared_ptr<const PredicateIndex> idx;
    {
        std::shared_lock lock(_pred_idx_mtx);
        const auto       it = _pred_idx_cache.find(predicate);
        if (it == _pred_idx_cache.end()) return false;
        idx = it->second;
    }

    const auto& map = forward ? idx->forward : idx->backward;
    const auto  it  = map.find(node);
    if (it != map.end())
    {
        for (const Node n : it->second)
            out.insert(n);
    }
    return true;
}

void Zelph::Impl::invalidate_predicate_index() const noexcept
{
    _pidx_io_enabled.store(false, std::memory_order_release);

    if (!_pred_idx_has_entries.exchange(false, std::memory_order_acq_rel)) return;
    std::unique_lock lock(_pred_idx_mtx);
    _pred_idx_cache.clear();
}

// Relation-type declarations (p ~ ->) can appear without passing
// through fact(): trusted imports bypass triple-level construction.
// Fact-structure reconstruction rejects every predicate absent from
// the memoized set, so a set built BEFORE such a bypass makes every
// fact using a newly declared predicate invisible to queries and
// unification. fact() covers its own case in
// invalidate_fact_structures_for.
void Zelph::Impl::invalidate_relation_type_set() const
{
    std::unique_lock lock(_rel_types_mtx);
    _rel_types.reset();
    ++_rel_types_gen;
}

// Lock-once transitive traversal working directly on _left/_right
// references: no adjacency_set copies, no per-edge lock acquisitions.
// Aborts and returns false once `scan_budget` relation entries have
// been scanned - hub nodes blow the budget immediately, signalling
// the caller to switch to the predicate index. On false, `result`
// is partial and must be discarded.
bool Zelph::Impl::try_transitive_direct(Node start, Node predicate, bool include_start, bool forward, size_t scan_budget, const adjacency_set* skip, adjacency_set& result) const
{
    // Same lock order as writers (connect): left before right.
    std::shared_lock<std::shared_mutex> lock_left(_smtx_left);
    std::shared_lock<std::shared_mutex> lock_right(_smtx_right);

    ankerl::unordered_dense::set<Node> seen;
    std::vector<Node>                  frontier{start};
    size_t                             scanned = 0;

    if (include_start)
    {
        seen.insert(start);
        result.insert(start);
    }

    auto expand = [&](const Node n, std::vector<Node>& next) -> bool
    {
        // Outgoing edges of n: relations where n is subject or object.
        const auto edges_it = _left.find(n);
        if (edges_it == _left.end()) return true;

        scanned += edges_it->second.size();
        if (scanned > scan_budget) return false;

        for (const Node rel : edges_it->second)
        {
            // Not an edge of the closure: nobody claimed it. Same
            // snapshot as the index build above.
            if (skip != nullptr && skip->count(rel) != 0) continue;

            const auto rl_it = _left.find(rel);
            if (rl_it == _left.end()) continue;
            const adjacency_set& rel_left = rl_it->second;
            if (rel_left.count(predicate) == 0) continue;

            const auto rr_it = _right.find(rel);
            if (rr_it == _right.end()) continue;
            const adjacency_set& rel_right = rr_it->second;

            // One pass over the incoming side splits it into the
            // objects and the bidirectional nodes. The SUBJECT is
            // always bidirectional and always present, so a single
            // bidirectional node IS the subject -- no hash needed.
            // More than one means the fact has a CHILD fact (a
            // statement about it, the rule it is a condition of, its
            // rule-pattern marking), which is linked in both
            // directions in exactly the same way; only then is the
            // exact triple hash consulted. That kept the common case
            // at the cost it had before.
            adjacency_set objects;
            Node          subject       = 0;
            size_t        bidirectional = 0;

            for (const Node c : rel_right)
            {
                if (rel_left.count(c) == 0)
                {
                    objects.insert(c);
                }
                else
                {
                    ++bidirectional;
                    subject = c;
                }
            }

            if (bidirectional == 0) continue;

            if (bidirectional > 1)
            {
                subject = 0;
                for (const Node c : rel_right)
                {
                    if (is_fact_subject(rel, predicate, c, rel_left, rel_right, objects))
                    {
                        subject = c;
                        break;
                    }
                }
                if (subject == 0) continue;
            }

            if (is_var(subject)) continue;

            if (forward)
            {
                if (subject != n) continue;

                for (const Node obj : objects)
                {
                    if (obj == n || is_var(obj)) continue;
                    if (seen.insert(obj).second)
                    {
                        result.insert(obj);
                        next.push_back(obj);
                    }
                }
            }
            else
            {
                if (subject == n || objects.count(n) == 0) continue;

                if (seen.insert(subject).second)
                {
                    result.insert(subject);
                    next.push_back(subject);
                }
            }
        }
        return true;
    };

    while (!frontier.empty())
    {
        std::vector<Node> next;
        for (const Node n : frontier)
        {
            if (!expand(n, next)) return false;
        }
        frontier = std::move(next);
    }
    return true;
}
