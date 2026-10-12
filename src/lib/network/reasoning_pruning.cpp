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

#include "fact_structure.hpp"
#include "zelph_impl.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    #include <sys/resource.h>
#endif

// Prune mode: what does the matched condition denote?
//
// resolve_pattern rather than a lookup in the bindings, because a pattern is
// not always a bare VARIABLE. `((Y r s) p Z)`, `(X (Y r s) Z)` and
// `(X p (Y r s))` all match -- unification resolves them structurally, the
// predicate included since it learned to -- but the reconstruction here
// substituted only where the pattern WAS a variable and then asked check_fact
// about the pattern node itself. It found nothing, and `.prune-facts`
// reported "Pruned 0 matching facts" for facts it had just matched.
//
// resolve_pattern is the read-only sibling of instantiate_fact: pure hash
// lookups, nothing created, which is what a prune pass may do.
void zelph::network::Reasoning::collect_prune_targets(const Node condition, const Variables& bindings, const Node parent)
{
    // `.prune-nodes A (...)`: the command named the variable whose bindings
    // die, so the matched condition says nothing about what to delete -- it
    // is one of several, and which one arrives here depends on the order
    // optimize_order chose. The other conditions are the FILTER that selected
    // the victim; their facts are not the user's to lose, and removing the
    // node takes the facts it is part of with it anyway. Hence: no
    // _facts_to_prune, and no reading of the condition at all.
    if (_prune_target_var != 0)
    {
        const auto it = bindings.find(_prune_target_var);
        if (it != bindings.end() && it->second != 0 && !Zelph::Impl::is_var(it->second))
        {
            _nodes_to_prune.insert(it->second);
        }
        return;
    }

    adjacency_set objects;
    const Node    subject_pattern  = parse_fact(condition, objects, parent);
    const Node    relation_pattern = parse_relation(condition);

    std::vector<Node> history;
    Node              subject  = 0;
    Node              relation = 0;

    if (resolve_pattern(this, subject_pattern, bindings, subject, history) != Resolve::Ok) return;

    history.clear();
    if (resolve_pattern(this, relation_pattern, bindings, relation, history) != Resolve::Ok) return;

    adjacency_set targets;
    for (const Node obj : objects)
    {
        history.clear();
        Node iobj = 0;
        if (resolve_pattern(this, obj, bindings, iobj, history) == Resolve::Ok && iobj != 0
            && !Zelph::Impl::is_var(iobj))
        {
            targets.insert(iobj);
        }
    }

    if (subject == 0 || relation == 0 || targets.empty()) return;
    if (Zelph::Impl::is_var(subject) || Zelph::Impl::is_var(relation)) return;

    const Answer ans = check_fact(subject, relation, targets);
    if (!ans.is_known() || ans.relation() == 0) return;

    _facts_to_prune.insert(ans.relation());

    if (_prune_nodes_mode)
    {
        // Unchanged from the three copies this replaces, parent-less call
        // included: which side is the variable decides what gets deleted.
        if (Zelph::Impl::is_var(parse_fact(condition, objects)))
            _nodes_to_prune.insert(subject);
        else if (objects.size() == 1)
            _nodes_to_prune.insert(*targets.begin());
    }
}

using namespace zelph::network;

// A prune of a real dump runs for hours and used to say nothing at all until
// it was finished, so there was no way to tell a slow run from a stuck one
// from outside the process. Below this many victims nothing is reported: a
// small prune is over before a line could help, and the transcripts of the
// documentation would change for nothing.
namespace
{
    constexpr size_t prune_progress_step = 100000;

    // ...and reporting once per BATCH is not enough, because a batch is where
    // the hours are. On the full Wikidata dump one batch of prune_progress_step
    // victims took twelve minutes, of which ~90 % is the collection phase, so
    // the log stood silent for twelve minutes at a time and looked hung on
    // exactly the run the progress output was built for. The collection phase
    // therefore reports on its own, this many victims at a time.
    constexpr size_t prune_collect_step = 10000;

    // Major faults per victim, below which the graph counts as RESIDENT and
    // the collection phase stops spreading itself over the pool.
    //
    // Why the collection is parallel at all: on the full Wikidata dump the
    // graph is ~200 GiB in zram, and there the pool pays twice over -- the
    // page faults are handled per thread and zram decompression is CPU work
    // that parallelises with them. Measured 5.1x on that workload.
    //
    // Why it must stop when the graph FITS: with nothing to fault, all that is
    // left is a dozen threads on the same two shared_mutexes, and they contend
    // for the cache line the rwlock counter lives on rather than for
    // exclusivity. Measured on a 6.8 M-node fixture in RAM: identical wall
    // time to a single core for five times the CPU. That CPU is not free --
    // this machine is shared, and on a laptop it is battery and heat.
    //
    // The threshold has three orders of magnitude of room either way: the live
    // dump ran at ~17 major faults per node, a resident graph at essentially
    // zero. Erring towards parallel is the deliberate direction, because
    // losing 5.1x on the workload that takes days is far worse than spending
    // CPU on one that takes seconds.
    constexpr double prune_resident_faults_per_victim = 1.0;

    // Major faults this process has taken, or -1 where the platform will not
    // say -- in which case the collection phase keeps the pool, which is what
    // it did before this was measured at all.
    long long process_major_faults()
    {
#if defined(__EMSCRIPTEN__) || defined(_WIN32)
        return -1; // single execution lane anyway / no portable counter here
#else
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
        return static_cast<long long>(usage.ru_majflt);
#endif
    }
}

// The engine's own vocabulary is not data -- and neither is the one fact that
// makes it usable. "A ~ ->" binds A to every declared relation type, the core
// predicates among them, and prune_nodes has always kept those NODES for that
// reason. Their relation-type DECLARATIONS were pruned all the same, which
// left a graph whose core predicates are no longer read as predicates: no fact
// of `~`, `=>` or `in` is reconstructible any more. That stayed invisible only
// because the memoized relation-type set was not refreshed on removal; with it
// refreshed, `.prune-nodes A ~ ->` empties the network of readable facts.
bool Reasoning::is_core_declaration(const Node fact) const
{
    if (!is_relation_type_declaration(fact)) return false;

    adjacency_set objects;
    return !get_core_name(parse_fact(fact, objects, 0)).empty();
}

// A statement may simultaneously function as a claim and a component of a
// rule -- serving as its condition, its consequence, or appearing nested
// within either: thanks to hash-consing, "a p b" asserted and
// "(a p b) => (c q d)" become a single node, and likewise for
// "((a p b) says Z) => (Z ok yes)".
// The prune commands eliminate claims, and when the node was removed, the
// rule vanished too -- a rule that nobody had requested to delete,
// disappearing silently, and every later run proceeding without it. Such a
// statement reverts to being the rule's pattern instead, just as a dropped
// cluster leaves it (restore_rule_patterns).
//
// The statements that hold it go with the claim, just as they go when no rule
// is involved and the removal carries them along. Only the claim went before,
// and `(a p b) q z` beside the rule `(a p b) => (c q d)` kept answering
// `S q O`: a claimed statement containing an unclaimed part, which the claims
// walk deems impossible. The text of a rule remains: a `=>` fact, either
// asserted or mentioned, claimed as a node and never entered; a membership
// within a collection that the rule's text holds, from which the walk
// proceeds to what holds the collection, since a set constant is a single
// node regardless of where its members appear; and a negation or conjunction
// tag, which defines how the rule reads the statement. A holder that is
// itself part of a rule is handled as the statement is: its claim is
// withdrawn, it stays as the rule's pattern, and what holds it goes in turn.
// Every other holder is removed with the cascade remove_node runs, leaving
// the parts of each rule standing.
//
// A container counts as rule text solely when it serves as a collection of
// the rule's text, not merely when a rule refers to it. A membership in `k`
// beside a rule over `X likes k` stayed as rule text, and the walk proceeded
// from `k`, eliminating every fact about it. That membership is now gone; the
// cascade would have carried `k` along as a container deprived of a member,
// yet `k` remains because it forms part of a rule. A relation-type
// declaration functions as rule text only during the period when a rule
// reads the statement as a predicate. Kept regardless, it continued to be a
// claim concerning an unclaimed statement, and the .cleanup every prune
// recommends then identified the predicate as unused, thereby removing its
// node: the rule's condition, and the rule with it.
void Reasoning::withdraw_claims(const std::vector<Node>& facts, const adjacency_set& parts) const
{
    if (facts.empty()) return;

    std::vector<Node>                  withdrawn(facts.begin(), facts.end());
    std::vector<Node>                  pending(facts.begin(), facts.end());
    std::vector<std::pair<Node, Node>> declarations; // (declaration, the statement it declares)
    adjacency_set                      seen;
    adjacency_set                      doomed;
    for (const Node fact : facts)
        seen.insert(fact);

    const auto rule_text_collection = [&](const Node container)
    { return holds_rule_text(container) || (parts.count(container) != 0 && is_set_constant(container)); };

    const auto read_as_predicate = [&](const Node statement)
    {
        adjacency_set neighbours = get_right(statement);
        for (const Node m : get_left(statement))
            neighbours.insert(m);

        for (const Node m : neighbours)
        {
            if (parts.count(m) == 0 || parse_relation(m) == 0) continue;
            const auto structures = get_fact_structures(this, m, 3);
            for (const FactStructure& fs : *structures)
                if (fs.predicate == statement) return true;
        }
        return false;
    };

    while (!pending.empty())
    {
        const Node n = pending.back();
        pending.pop_back();

        adjacency_set neighbours = get_right(n);
        for (const Node m : get_left(n))
            neighbours.insert(m);

        for (const Node holder : neighbours)
        {
            if (holder == n || seen.count(holder) != 0 || doomed.count(holder) != 0) continue;
            if (parse_relation(holder) == 0) continue; // not a fact

            const auto           structures = get_fact_structures(this, holder, 3); // held, see Zelph::get_fact_subjects
            const FactStructure* held       = nullptr;
            for (const FactStructure& fs : *structures)
            {
                if (fs.subject == n || fs.predicate == n || fs.objects.count(n) != 0)
                {
                    held = &fs;
                    break;
                }
            }
            if (held == nullptr) continue; // one of n's own parts

            if (held->predicate == core.Causes) continue;
            if (held->predicate == core.IsA
                && (held->objects.count(core.Negation) != 0 || held->objects.count(core.Conjunction) != 0))
                continue;
            if (held->predicate == core.IsA && held->subject == n && held->objects.count(core.RelationTypeCategory) != 0)
            {
                seen.insert(holder);
                declarations.emplace_back(holder, n);
                continue;
            }
            if (held->predicate == core.PartOf)
            {
                bool rule_text = false;
                for (const Node container : held->objects)
                {
                    if (!rule_text_collection(container)) continue;
                    rule_text = true;
                    if (seen.count(container) != 0) continue;
                    seen.insert(container);
                    pending.push_back(container);
                }
                if (rule_text) continue;
            }
            if (parts.count(holder) != 0)
            {
                seen.insert(holder);
                if (is_asserted_fact(holder))
                {
                    withdrawn.push_back(holder);
                    pending.push_back(holder);
                }
                continue;
            }

            collect_doomed(holder, doomed, &parts);
        }
    }

    for (const auto& [declaration, statement] : declarations)
    {
        if (!read_as_predicate(statement)) collect_doomed(declaration, doomed, &parts);
    }

    if (!doomed.empty())
    {
        disable_fact_stores(); // as the operation remove_node performs
        adjacency_set named;
        remove_doomed(doomed, &named);
        remove_names_of(named);
    }

    // The marking represents what persists after a removal, and dropping
    // a cluster undoes no prior removal. Stored within the active
    // cluster, the marking went with .cluster-drop, and the next .save
    // and .load read the statement back as asserted. Suspension of
    // recording occurs as in Zelph::rehash_dependents.
    const std::string active = active_cluster_name();
    if (!active.empty()) deactivate_cluster();
    restore_rule_patterns(withdrawn);
    if (!active.empty()) set_active_cluster(active);
}

// Each rule's conditions and consequences, the members of their condition
// sets, and every statement nested within them, are read once per prune:
// asked per fact, the question parsed the neighbours of each fact a
// predicate-wide prune removed, which caused that prune to become six
// times slower.
//
// A statement nested within a condition or a consequence is part of the rule
// just as fully as the condition is: the rule is built from it, removing its
// node takes the rule along, and mark_rule_patterns marks it as the rule's
// pattern. Read exclusively at the top level, `.prune-facts (a p b)` next to
// the rule `((a p b) says Z) => (Z ok yes)` removed the rule with the claim.
// The descent reads the structure of fact nodes solely: querying each node
// encountered to determine whether it is a condition set inspects the
// adjacency of every atomic element in every rule, including numerals and
// operators, and tripled the cost of a ground prune in the context of the
// 429 rules from the Jacobian session.
//
// The members of a collection inside a rule's text are part of the rule as
// well: a collection is defined by its members, so removing one took the
// collection and the rule along, and pruning an asserted `a p b` adjacent to
// `(X r Y) => (X about {(a p b)})` removed the rule. A rule's own collection
// is identified by its id and a set constant as a hash node lacking a fact's
// structure, hence neither costs a read of an atom.
adjacency_set Reasoning::rule_parts() const
{
    adjacency_set     parts;
    std::vector<Node> sets;
    std::vector<Node> nested;      // fact nodes whose subject, predicate, and objects remain to be read
    std::vector<Node> collections; // set constants and rules' own collections, the members of which are yet to be read
    const auto        add = [&](const Node nd)
    {
        if (nd == 0 || Zelph::Impl::is_var(nd) || parts.count(nd) != 0) return;
        parts.insert(nd);
        if (Zelph::Impl::is_hash(nd))
            nested.push_back(nd);
        else if (is_rule_template(nd))
            collections.push_back(nd);
    };
    for (const Node rel : get_left(core.Causes))
    {
        if (parse_relation(rel) != core.Causes) continue;
        adjacency_set objects;
        const Node    subject = parse_fact(rel, objects);
        if (subject == 0) continue;
        add(subject);
        for (const Node o : objects)
            add(o);
        if (is_condition_set(subject)) sets.push_back(subject);
    }
    while (!sets.empty())
    {
        const Node set = sets.back();
        sets.pop_back();
        adjacency_set members;
        if (!condition_set_members(set, members)) continue;
        for (const Node m : members)
        {
            if (parts.count(m) != 0) continue;
            add(m);
            if (is_condition_set(m)) sets.push_back(m);
        }
    }
    while (!nested.empty() || !collections.empty())
    {
        if (!collections.empty())
        {
            const Node collection = collections.back();
            collections.pop_back();
            for (const Node rel : get_right(collection))
            {
                if (parse_relation(rel) != core.PartOf) continue;
                adjacency_set containers;
                const Node    member = parse_fact(rel, containers, 0);
                if (containers.count(collection) != 0) add(member);
            }
            continue;
        }
        const Node nd = nested.back();
        nested.pop_back();
        const FactStructure fs = get_preferred_structure(this, nd, 3);
        if (fs.predicate == 0 || fs.subject == 0)
        {
            collections.push_back(nd); // a set constant
            continue;
        }
        add(fs.subject);
        add(fs.predicate);
        for (const Node o : fs.objects)
            add(o);
    }
    return parts;
}

void Reasoning::prune_facts(Node pattern, size_t& removed_count)
{
    const SuspendFactStructureCache no_cache(*this); // as in prune_nodes

    invalidate_fact_structures_cache();

    _prune_mode       = true;
    _prune_nodes_mode = false;
    _facts_to_prune.clear();
    _nodes_to_prune.clear();

    apply_rule(0, pattern);

    _pool->wait();

    removed_count = _facts_to_prune.size();

    // The matching is the long half on a real dump and says nothing while it
    // runs, so the count is worth a line the moment it is known -- the same
    // reason prune_nodes reports the size of its job.
    if (removed_count >= prune_progress_step)
        out_stream() << "Pruning " << removed_count << " matched fact(s)..." << std::endl;

    if (removed_count > 0)
    {
        const adjacency_set parts = rule_parts();

        // The facts that are matched and belong to a rule are handled first,
        // with the graph intact: what holds one of these facts is determined
        // by reading the facts it is part of, and after the loop below
        // eliminates a fact that shares the same holder, the holder then
        // reads as a different fact.
        std::vector<Node> rule_parts_hit;
        for (const Node fact : _facts_to_prune)
        {
            if (parts.count(fact) != 0 && !is_core_declaration(fact)) rule_parts_hit.push_back(fact);
        }
        withdraw_claims(rule_parts_hit, parts);

        std::lock_guard<std::mutex> lock(_mtx_network);
        bool                        declaration_removed = false;
        size_t                      kept_core           = 0;

        for (Node fact : _facts_to_prune)
        {
            if (is_core_declaration(fact))
            {
                ++kept_core;
                continue;
            }
            if (parts.count(fact) != 0) continue;

            // Same question remove_node asks: a predicate that has lost its
            // relation-type declaration must stop being read as one. This path
            // removes facts directly, so it has to ask for itself.
            if (!declaration_removed && is_relation_type_declaration(fact)) declaration_removed = true;
            _pImpl->remove(fact);
        }

        removed_count -= kept_core;
        if (kept_core > 0)
            out_stream() << "Kept " << kept_core
                         << " relation-type declaration(s) of core predicates; they are part of the engine, not data."
                         << std::endl;

        if (declaration_removed) invalidate_relation_type_set();
    }

    _prune_mode = false;
}

void Reasoning::prune_nodes(Node pattern, const Node target_var, size_t& removed_facts, size_t& removed_nodes)
{
    // A prune reconstructs the structure of each fact it touches ONCE and
    // never comes back to it, so remembering them buys nothing and costs an
    // exclusive lock per store plus a map that grows into the millions. It is
    // the single biggest item on this workload; see
    // store_fact_structures_cached for the measurement.
    const SuspendFactStructureCache no_cache(*this);

    invalidate_fact_structures_cache();

    _prune_mode       = true;
    _prune_nodes_mode = true;
    _prune_target_var = target_var;
    _facts_to_prune.clear();
    _nodes_to_prune.clear();

    apply_rule(0, pattern);

    _pool->wait();

    removed_facts = _facts_to_prune.size();
    removed_nodes = 0;

    std::lock_guard<std::mutex> lock(_mtx_network);

    bool   declaration_removed = false;
    size_t kept_core_facts     = 0;
    for (Node fact : _facts_to_prune)
    {
        if (is_core_declaration(fact))
        {
            ++kept_core_facts;
            continue;
        }
        if (!declaration_removed && is_relation_type_declaration(fact)) declaration_removed = true;
        _pImpl->remove(fact);
    }
    removed_facts -= kept_core_facts;
    if (declaration_removed) invalidate_relation_type_set();

    size_t kept_core_nodes = 0;

    // The size of the job is known here, and it is what makes the progress
    // lines below mean anything.
    const bool report = _nodes_to_prune.size() >= prune_progress_step;

    if (report)
    {
        out_stream() << "Pruning " << _nodes_to_prune.size() << " node(s) and "
                     << removed_facts << " matched fact(s)..." << std::endl;
    }

    // Erasing a node's names costs a SCAN of the reverse name map, so a bulk
    // removal collects its victims and pays for one scan at the end -- see
    // Zelph::remove_node. Doing it per node is what made this run at one node
    // per second on the full Wikidata network.
    adjacency_set named_dead;

    // The engine's own vocabulary is not data. A pattern as ordinary as
    // "A ~ ->" binds A to every declared relation type, the core predicates
    // among them, and deleting those leaves a network whose next negation or
    // list fails deep inside the engine. Skipped rather than refused: a prune
    // is a bulk operation, and aborting it halfway leaves a graph nobody
    // asked for.
    std::vector<Node> victims;
    victims.reserve(_nodes_to_prune.size());
    for (const Node node : _nodes_to_prune)
    {
        if (!get_core_name(node).empty())
        {
            ++kept_core_nodes;
            continue;
        }
        victims.push_back(node);
    }

    // Removal is TWO halves with opposite natures, and this is where they are
    // separated.
    //
    // Working out what a node takes with it is pure reading -- the cascade
    // upwards, plus the fact-structure reconstruction that decides each step
    // -- and a profile of the live Wikidata prune put ~90 % of the removal
    // phase there, against ~8.5 % in the erasing itself. Reading parallelises.
    //
    // Erasing does not, for two independent reasons: there is one
    // shared_mutex per adjacency map, so every writer is exclusive whatever
    // the thread count, and a fact whose parts are half deleted reads as a
    // DIFFERENT fact -- which is the reasoning Zelph::collect_doomed rests on
    // and would be reading its own destruction.
    //
    // So: collect a batch of victims on the pool, erase the union serially,
    // repeat. Zelph::collect_doomed carries the argument for why the union of
    // closures taken against the unmutated graph is the set the sequential
    // order would have removed.
    //
    // In BATCHES rather than all at once, because between two batches is the
    // only place the fact-structure cache gets cleared: a single pass over
    // eight million victims would let it grow all the way, and memory is the
    // binding constraint on this workload. The batch is the progress step, so
    // the two report together.
    disable_fact_stores(); // one-way and idempotent; remove_node does it per call

    // `.parallel` off has to mean single-threaded HERE too, or the setting
    // would be a half-truth and the suite's single-core subcase would not be
    // one.
    const size_t pool_workers = use_parallel() ? std::max<size_t>(1, _pool->count()) : 1;

    // The FIRST batch spreads over the pool whatever the graph looks like:
    // nothing has been measured yet, and the expensive mistake is the other
    // one. Each batch then decides for the next from what it faulted -- see
    // prune_resident_faults_per_victim. Re-deciding every time rather than
    // once is what makes it follow a graph that changes: the prune itself
    // frees memory, so a run can start out swapping and end up resident.
    size_t workers = pool_workers;

    for (size_t base = 0; base < victims.size(); base += prune_progress_step)
    {
        const size_t stop = std::min(base + prune_progress_step, victims.size());
        const size_t span = ((stop - base) + workers - 1) / workers;

        const long long faults_before = pool_workers > 1 ? process_major_faults() : -1;

        std::vector<adjacency_set> found(workers);
        std::vector<size_t>        taken(workers, 0);
        std::exception_ptr         failure;
        std::mutex                 failure_mtx;

        // Counts victims CONSIDERED, not walked, so it is monotone against the
        // batch size and reaches it exactly. Only the calling thread reads it
        // and only it prints, which is why no output lock is needed here.
        std::atomic<size_t> examined{0};

        for (size_t w = 0; w < workers; ++w)
        {
            const size_t from = base + w * span;
            const size_t to   = std::min(from + span, stop);
            if (from >= to) continue;

            _pool->enqueue([this, from, to, w, &victims, &found, &taken, &examined, &failure, &failure_mtx]()
                           {
                               try
                               {
                                   for (size_t i = from; i < to; ++i)
                                   {
                                       const Node victim = victims[i];

                                       examined.fetch_add(1, std::memory_order_relaxed);

                                       // A victim can be part of ANOTHER victim's
                                       // closure -- "X rel o" binds X to `a` and to
                                       // `(a p b)` alike. Whoever reaches it first
                                       // walks it; the second call returns at once.
                                       if (found[w].count(victim) != 0) continue;
                                       if (!_pImpl->exists(victim)) continue;

                                       collect_doomed(victim, found[w]);
                                       ++taken[w];
                                   }
                               }
                               catch (...)
                               {
                                   // The pool's worker loop does not catch, and an
                                   // escaping exception would take the process with
                                   // it. Kept and rethrown on the calling thread,
                                   // where the serial version threw.
                                   const std::lock_guard<std::mutex> guard(failure_mtx);
                                   if (!failure) failure = std::current_exception();
                               } });
        }

        // ThreadPool::wait() spins on pending_tasks, so waiting here costs the
        // same and buys the progress line. The batch is not divided into
        // smaller ones instead, because its size is what bounds the
        // fact-structure cache and what makes the union argument in
        // collect_doomed hold.
        size_t reported = 0;
        while (_pool->pending_tasks > 0)
        {
            if (report)
            {
                const size_t seen = examined.load(std::memory_order_relaxed);
                if (seen >= reported + prune_collect_step)
                {
                    reported = seen - seen % prune_collect_step;
                    out_stream() << "  " << (base + seen) << " of " << victims.size()
                                 << " node(s) examined" << std::endl;
                }
            }
            std::this_thread::yield();
        }

        _pool->wait();
        if (failure) std::rethrow_exception(failure);

        // What the COLLECTION faulted, sampled here and not later: the erase
        // below is serial either way, and counting its faults would credit the
        // pool for work it did not do. The verdict is applied at the end of
        // the batch, because `workers` still dimensions `found` and `taken`.
        size_t next_workers = workers;
        if (faults_before >= 0)
        {
            const long long faults_after = process_major_faults();
            const size_t    considered   = examined.load(std::memory_order_relaxed);

            if (faults_after >= faults_before && considered > 0)
            {
                const double per_victim = static_cast<double>(faults_after - faults_before)
                                        / static_cast<double>(considered);

                next_workers = per_victim < prune_resident_faults_per_victim ? 1 : pool_workers;
            }
        }

        adjacency_set doomed;
        size_t        victims_taken = 0;
        for (size_t w = 0; w < workers; ++w)
        {
            for (const Node dead : found[w])
                doomed.insert(dead);
            victims_taken += taken[w];
        }

        remove_doomed(doomed, &named_dead);

        // What the pattern NAMED against what came along: a doomed node that
        // is not a victim is a fact the victim took with it. The two add up to
        // the batch exactly, which the per-node counting could not promise --
        // it depended on which of two victims the unordered set offered first.
        removed_nodes += victims_taken;
        removed_facts += doomed.size() - victims_taken;

        if (report)
        {
            out_stream() << "  " << removed_nodes << " of " << _nodes_to_prune.size()
                         << " node(s) removed" << std::endl;
        }

        workers = next_workers; // `found` and `taken` are sized above, not here
    }

    if (report) out_stream() << "  removing the names of " << named_dead.size() << " node(s)..." << std::endl;

    remove_names_of(named_dead);

    if (kept_core_nodes > 0 || kept_core_facts > 0)
    {
        out_stream() << "Kept " << kept_core_nodes
                     << " core node(s) and " << kept_core_facts
                     << " of their relation-type declaration(s) that the pattern matched; "
                        "they are part of the engine, not data."
                     << std::endl;
    }

    _prune_mode       = false;
    _prune_nodes_mode = false;
    _prune_target_var = 0;
}

void Reasoning::purge_unused_predicates(size_t& removed_facts, size_t& removed_predicates)
{
    invalidate_fact_structures_cache();

    removed_facts      = 0;
    removed_predicates = 0;

    std::vector<Node> all_predicates;

    {
        std::lock_guard<std::mutex> lock(_mtx_network);

        adjacency_set def_facts = _pImpl->get_right(core.RelationTypeCategory);

        for (Node def_fact : def_facts)
        {
            if (_pImpl->get_right(def_fact).count(core.IsA) == 1)
            {
                for (Node cand : _pImpl->get_left(def_fact))
                {
                    if (cand != core.RelationTypeCategory && cand != core.IsA)
                    {
                        if (_pImpl->get_right(cand).count(def_fact) == 1)
                        {
                            all_predicates.push_back(cand);
                        }
                    }
                }
            }
        }
    }

    auto is_protected = [&](Node n)
    {
        return n == core.IsA || n == core.Causes || n == core.RelationTypeCategory || n == core.Unequal || n == core.Contradiction || n == core.Cons || n == core.Nil || n == core.PartOf || n == core.Conjunction;
    };

    // Removing a node takes every fact it is a part of, meaning a predicate
    // no fact uses as its relation any more is removed only when nothing else
    // holds it: no fact besides its own `~ ->` declaration refers to it as
    // subject, relation, or object. This applies to a rule at every depth --
    // its conditions, consequences, and operands are parts of its `=>` fact,
    // of a condition set, or of a tag fact -- and similarly to a rule a
    // statement refers to, along with data like `p is transitive`. A
    // statement used as a relation, `(a p b)`, is never removed in this
    // process: its node is the statement itself, and .cleanup removes a
    // statement solely when it is broken.
    auto is_held = [&](const Node pred)
    {
        if (Zelph::Impl::is_hash(pred)) return true;

        adjacency_set neighbours = _pImpl->get_right(pred);
        for (const Node from : _pImpl->get_left(pred))
            neighbours.insert(from);

        for (const Node whole : neighbours)
        {
            if (whole == pred || parse_relation(whole) == 0) continue;
            if (is_relation_type_declaration(whole)) continue;

            adjacency_set objects;
            if (parse_fact(whole, objects, 0) == pred || parse_relation(whole) == pred || objects.count(pred) != 0) return true;
        }
        return false;
    };

    diagnostic_stream() << "Found " << all_predicates.size() << " predicates. Starting deep scan..." << std::endl;

    std::lock_guard<std::mutex> lock(_mtx_network);

    for (size_t i = 0; i < all_predicates.size(); ++i)
    {
        Node pred = all_predicates[i];

        if (is_protected(pred)) continue;

        // The facts that USE pred as their relation type -- not everything
        // pointing at it. For an atomic predicate the two coincide; a
        // COMPOSITE one is a fact as well, so its own subject and objects
        // point at it too, and the scan below then read those atoms as
        // malformed facts and PURGED them: `.cleanup` on a graph holding
        // `a p b` and `z (a p b) w` deleted `a` and `b`, taking both facts
        // with them. get_facts_of_predicate applies the exact role test.
        adjacency_set incoming_to_pred = get_facts_of_predicate(pred);

        if (incoming_to_pred.size() > 200000)
        {
            std::string name = get_name(pred, "wikidata", true);
            diagnostic_stream() << "[" << (i + 1) << "/" << all_predicates.size() << "] Checking "
                                << name << " (" << pred << ") with "
                                << incoming_to_pred.size() << " entries..." << std::endl;
        }

        size_t valid_usage_count = 0;
        size_t local_removed     = 0;

        for (Node fact : incoming_to_pred)
        {
            if (_pImpl->get_right(fact).count(core.IsA) == 1)
            {
                continue;
            }

            bool is_zombie = false;

            adjacency_set incoming_to_fact = _pImpl->get_left(fact);
            if (incoming_to_fact.empty())
            {
                is_zombie = true;
            }
            else
            {
                adjacency_set outgoing_from_fact = _pImpl->get_right(fact);

                bool has_subject = false;
                bool has_object  = false;

                for (Node out_node : outgoing_from_fact)
                {
                    if (out_node == pred) continue;

                    if (incoming_to_fact.count(out_node) == 1)
                    {
                        has_subject = true;
                        break;
                    }
                }

                // When a fact has its relation as the subject, specifically
                // `p p b`, a single pair of edges fulfils both roles, and the
                // loop above passes over it as the relation. The function
                // get_preferred_structure tells whether `pred` also serves as
                // the subject.
                if (!has_subject && incoming_to_fact.count(pred) == 1)
                {
                    const FactStructure fs = get_preferred_structure(this, fact, 1);
                    has_subject            = fs.predicate == pred && fs.subject == pred;
                }

                if (has_subject)
                {
                    for (Node in_node : incoming_to_fact)
                    {
                        if (outgoing_from_fact.count(in_node) == 0)
                        {
                            has_object = true;
                            break;
                        }
                    }
                    if (!has_object)
                    {
                        // Self-referential: all incoming nodes are also outgoing (bidirectional subject only).
                        has_object = std::all_of(incoming_to_fact.begin(), incoming_to_fact.end(), [&](Node n)
                                                 { return outgoing_from_fact.count(n) != 0; });
                    }
                }

                if (!has_subject || !has_object)
                {
                    is_zombie = true;
                }
            }

            if (is_zombie)
            {
                _pImpl->remove(fact);
                local_removed++;
            }
            else
            {
                valid_usage_count++;
            }
        }

        removed_facts += local_removed;

        if (local_removed > 0 && incoming_to_pred.size() > 200000)
        {
            out_stream() << "   -> Purged " << local_removed << " broken facts." << std::endl;
        }

        if (valid_usage_count == 0 && !is_held(pred))
        {
            _pImpl->remove(pred);
            removed_predicates++;
        }
    }

    // A removed predicate is no longer a relation type, whether or not its
    // declaration fact was among the zombies: the memoized set holds the
    // predicate NODE, and that node is gone.
    if (removed_predicates > 0) invalidate_relation_type_set();
}
