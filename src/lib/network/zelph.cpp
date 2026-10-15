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
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"
#include "zelph_version.hpp"

#include <algorithm>
#include <limits>
#include <ranges>

using std::ranges::all_of;

using namespace zelph::network;

namespace
{
    // A statement written out from its parts, as it would appear when
    // entered: a part that is itself a statement is enclosed in parentheses,
    // whereas a list or a set brings its own brackets. Whether a part
    // qualifies as a statement is determined by the node, not inferred from
    // its textual form: "(a p b) q c" and "<1 2> p x" count as statements
    // even though they begin with a bracket. A part is rendered with all its
    // objects, at every level of nesting: the ordinary rendering shortens a
    // nested fact containing numerous objects to "(... 34 objects ...)", so
    // two statements that vary only within such a fact would then appear
    // identical.
    std::string render_statement(const Zelph& z, const Node subject, const Node predicate, const adjacency_set& objects)
    {
        const auto part = [&](const Node n)
        {
            std::string marked;
            zelph::string::node_to_string(&z, marked, z.lang(), n, std::numeric_limits<int>::max());
            const std::string text = zelph::string::unmark_identifiers(marked);
            const Node        pred = z.predicate_of(n);
            return pred != 0 && pred != z.core.Cons ? "(" + text + ")" : text;
        };

        std::string result = part(subject) + " " + part(predicate);
        for (const Node o : objects)
            result += " " + part(o);
        return result;
    }

    // What fact() and the bulk import say about a node that already
    // possesses the identifier of a statement yet holds a different one. The
    // node's own statement is read in the way unification reads it: from the
    // genuine store where the node has an entry, and otherwise from its
    // edges. Both statements are fully written out, including all objects,
    // because distinguishing between them is the point of the message.
    std::string occupied_node_message(const Zelph& z, const Node subject, const Node predicate, const adjacency_set& objects, const Node node)
    {
        const std::string   wanted = "\"" + render_statement(z, subject, predicate, objects) + "\"";
        const FactStructure held   = get_preferred_structure(&z, node, 0);
        const std::string   what   = held.predicate != 0
                                       ? "it holds \"" + render_statement(z, held.subject, held.predicate, held.objects) + "\", not " + wanted
                                       : "no statement can be read from its edges, and they do not hold " + wanted;

        return "node " + std::to_string(node) + " already exists, but " + what
             + ". A fact's node is the 62-bit hash of its statement, so this is a partially loaded view, "
               "a damaged file, or two statements whose hashes are equal. The statement was not stored.";
    }

    // Relation entries a direct (index-free) closure traversal may scan
    // before switching to the predicate index. Small closures on small
    // graphs stay index-free and fast; hub-heavy traversals on Wikidata
    // exhaust the budget immediately and pay the (cached) index build once.
    constexpr size_t kDirectClosureScanBudget = size_t(1) << 16;

    // The steps a closure over the index omits, from a subject to its
    // objects.
    using SkippedSteps = ankerl::unordered_dense::map<Node, adjacency_set>;

    // The index stores nodes instead of facts, meaning a bookkeeping fact
    // is omitted as the step from its subject to its object, unless a fact
    // that includes additional objects takes the same step.
    SkippedSteps bookkeeping_steps(const Zelph* z, const adjacency_set& facts, const Node predicate)
    {
        SkippedSteps steps;
        for (const Node fact : facts)
        {
            adjacency_set objects;
            const Node    subject = z->parse_fact(fact, objects);
            if (subject == 0) continue;
            for (const Node object : objects)
                if (containing_fact(z, subject, predicate, {object}) == 0) steps[subject].insert(object);
        }
        return steps;
    }

    adjacency_set bfs_over_index(const PredicateIndex::adjacency& adj, const Node start, const bool include_start, const SkippedSteps* skipped = nullptr, const bool forward = true)
    {
        adjacency_set                      result;
        ankerl::unordered_dense::set<Node> seen;
        std::vector<Node>                  frontier{start};

        const auto is_skipped = [skipped](const Node subject, const Node object)
        {
            if (skipped == nullptr) return false;
            const auto it = skipped->find(subject);
            return it != skipped->end() && it->second.count(object) != 0;
        };

        if (include_start)
        {
            seen.insert(start);
            result.insert(start);
        }

        while (!frontier.empty())
        {
            std::vector<Node> next;
            for (const Node n : frontier)
            {
                const auto it = adj.find(n);
                if (it == adj.end()) continue;
                for (const Node t : it->second)
                {
                    if (forward ? is_skipped(n, t) : is_skipped(t, n)) continue;
                    if (seen.insert(t).second)
                    {
                        result.insert(t);
                        next.push_back(t);
                    }
                }
            }
            frontier = std::move(next);
        }
        return result;
    }

}

std::string Zelph::get_version()
{
    return get_zelph_version();
}

Zelph::Zelph(const io::OutputHandler& output)
    : _pImpl{new Impl(output)}
    , core({_pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create(), _pImpl->create()})
{
    _pImpl->_core_names = &_core_names_by_node;
    _pImpl->publish_lang(_lang);

    fact(core.IsA, core.IsA, {core.RelationTypeCategory});
    fact(core.Unequal, core.IsA, {core.RelationTypeCategory});
    fact(core.Causes, core.IsA, {core.RelationTypeCategory});
    fact(core.Cons, core.IsA, {core.RelationTypeCategory});
    fact(core.PartOf, core.IsA, {core.RelationTypeCategory});

    for (const auto& entry : _pImpl->_left)
        _pImpl->_core_graph.push_back(entry.first);

    advance_content_epoch();
}

Zelph::~Zelph()
{
    delete _pImpl;
}

Node Zelph::var() const
{
    return _pImpl->var();
}

void Zelph::set_lang(const std::string& lang)
{
    if (lang != _lang)
    {
        _lang = lang;
        _pImpl->publish_lang(lang);
    }
}

// The language currently active for a reader within any thread. The function
// lang() retrieves the value stored by set_lang, which only the thread
// responsible for setting the language may read that way; published_lang is the
// copy that set_lang publishes.
std::string Zelph::published_lang() const
{
    return _pImpl->published_lang();
}

void Zelph::check_node_name(const std::string& name)
{
    if (name.empty())
    {
        throw std::invalid_argument("Zelph::node(): name cannot be empty");
    }
}

Node Zelph::node(const std::string& name, std::string lang)
{
    if (lang.empty()) lang = _lang;
    check_node_name(name);

    // This function answers "the ATOM of this name", and creates it when it
    // is not there: the parser calls it for every quoted name, the Janet API
    // for every string argument, the Wikidata importer for every label. A
    // VARIABLE that happens to carry the name is therefore not an answer.
    // Its name is display-only -- variables are quantified per statement and
    // resolved through the parser's scoped table, never through this map --
    // and returning it built a fact ABOUT a rule's variable: `"A" rel c`
    // next to a rule using A was accepted, echoed as `A rel c`, and then
    // invisible to every query, because a statement carrying a variable is a
    // pattern rather than data. The name lookup employed by commands
    // (get_node without atoms_only) continues to locate variables; solely the
    // creating path refuses them.
    //
    // 1. Fast path: perform a lookup via get_node, under a shared lock,
    //    restricted to atoms only
    if (const Node existing = get_node(name, lang, true)) return existing;

    // 2. Slow path: exclusive lock for creation (double-checked)
    std::unique_lock lock_node(_pImpl->_mtx_node_of_name);

    // Re-check: another thread may have created it while we re-acquired the lock
    {
        auto lang_it = _pImpl->_node_of_name.find(lang);
        if (lang_it != _pImpl->_node_of_name.end())
        {
            auto it = lang_it->second.find(name);
            if (it != lang_it->second.end() && !Impl::is_var(it->second))
            {
                return it->second;
            }
        }

        auto it_core = _core_names_by_name.find(name);
        if (it_core != _core_names_by_name.end())
        {
            return it_core->second;
        }
    }

    // 3. Create new node
    // We do not call invalidate_fact_structures_cache() here, because creating a node is isolated from the network
    //
    // A node from the engine's internal vocabulary (`refuted`, `opaque`, the
    // rule-pattern predicate, and the other terms named by engine_vocabulary)
    // is never recorded in any cluster. Whatever needs such a node first
    // creates it, and every later statement of the same kind refers to it.
    // Once created within a cluster, it went with the cluster's drop, together
    // with every statement that referred to it, even those made outside the
    // cluster: after a dry run that refuted first, a refutation typed later
    // was gone, and its fact answered as asserted.
    const Vocabulary vocabulary = engine_vocabulary(name, lang);
    Node             new_node   = _pImpl->create(vocabulary == Vocabulary::none ? Network::Record::yes : Network::Record::no);

    std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

    auto [reverse_outer_it, inserted_reverse_outer] = _pImpl->_node_of_name.try_emplace(lang);
    auto [forward_outer_it, inserted_forward_outer] = _pImpl->_name_of_node.try_emplace(lang);
    (void)inserted_reverse_outer;
    (void)inserted_forward_outer;

    std::string_view sv = _pImpl->_string_pool.intern(name);

    // The reverse entry exists only when a variable holds the display name,
    // and then the atom takes it over -- the same order assign_name_locked
    // applies, and the one the lookup above needs to find the atom next
    // time. The variable keeps its own forward entry, so rules go on
    // rendering it.
    auto [reverse_it, inserted_reverse] = reverse_outer_it->second.emplace(sv, new_node);
    if (!inserted_reverse) reverse_it->second = new_node;
    forward_outer_it->second.emplace(new_node, sv);

    // A relation type is declared here for a predicate within that vocabulary,
    // also outside every cluster. Otherwise its first fact writes the
    // declaration, and a cluster that held this first fact dropped the
    // declaration alongside it: the facts of the predicate written outside the
    // cluster remained, yet were no longer read as facts, and the path
    // condition of a rule written after the cluster stopped matching.
    if (vocabulary == Vocabulary::predicate)
    {
        lock_name.unlock();
        lock_node.unlock();
        const ClusterRecordingPause pause(*this);
        fact(new_node, core.IsA, {core.RelationTypeCategory});
    }

    return new_node;
}

bool Zelph::exists(uint64_t nd) const
{
    return _pImpl->exists(nd);
}

adjacency_set Zelph::get_sources(const Node relationType, const Node target, const bool exclude_vars) const
{
    adjacency_set sources;

    for (Node relation : _pImpl->get_right(target))
        if (_pImpl->get_right(relation).count(relationType) == 1)
            for (Node source : _pImpl->get_left(relation))
                if (source != target && (!exclude_vars || !Impl::is_var(source)))
                    sources.insert(source);

    return sources;
}

// Find all objects O such that the fact (subject predicate O) exists.
// Topology: subject <-> relation_node (bidirectional), object -> relation_node,
// relation_node -> predicate. Moved here from the Janet binding layer, which
// previously duplicated this topology knowledge.
adjacency_set Zelph::get_fact_objects(const Node subject, const Node predicate) const
{
    adjacency_set objects;

    // Consume an already-built predicate index if one exists (built lazily
    // by the transitive closures); this never triggers a build itself.
    if (_pImpl->try_indexed_fact_lookup(predicate, subject, /*forward*/ true, objects))
        return objects;

    // A rule's pattern is not an answer: nobody claimed it. Taken from the
    // snapshot rather than from is_asserted_fact, which would cost a store
    // probe -- or, after a binary load, a reconstruction walk -- per
    // CANDIDATE; the snapshot is one lookup per call and nullptr whenever the
    // graph holds no patterns at all. The is_var tests below cover the rest.
    const auto skip = unasserted_snapshot();

    for (const Node rel : get_right(subject))
    {
        if (!has_right_edge(rel, predicate)) continue;
        if (skip && skip->count(rel) != 0) continue;

        // The EXACT decomposition, not the role test on the adjacency. The
        // unidirectionality of an object is not a property of the graph: a
        // SELF-fact stores its object in the subject's bidirectional entry,
        // so `a p a` had no object at all and was invisible here while the
        // query answered it. get_fact_structures is the reading unification
        // uses, and it is what makes the two agree.
        // get_fact_structures returns the list BY SHARED POINTER, so binding a
        // range-for to *get_fact_structures(...) reads a container whose only
        // owner died at the end of that expression: the temporary is not
        // lifetime-extended, because the reference binds to the POINTEE, not to
        // the pointer. It survived on borrowed time -- while the structure
        // cache held a second owner the memory stayed valid -- so the bug was
        // invisible until a bulk pass stopped caching, and then the cascade in
        // remove_node silently found nothing. Hold the pointer.
        const auto structures = get_fact_structures(this, rel, 3);
        for (const auto& fs : *structures)
        {
            if (fs.predicate != predicate || fs.subject != subject) continue;

            for (const Node obj : fs.objects)
            {
                if (!is_var(obj)) objects.insert(obj);
            }
        }
    }

    return objects;
}

// Find all subjects S such that the fact (S predicate object) exists.
// The directional counterpart of get_fact_objects: object must participate
// in the pure object role (in left(rel) but NOT in right(rel)).
adjacency_set Zelph::get_fact_subjects(const Node predicate, const Node object) const
{
    adjacency_set subjects;

    if (_pImpl->try_indexed_fact_lookup(predicate, object, /*forward*/ false, subjects))
        return subjects;

    const auto skip = unasserted_snapshot(); // see get_fact_objects

    for (const Node rel : get_right(object))
    {
        if (!has_right_edge(rel, predicate)) continue;
        if (skip && skip->count(rel) != 0) continue;

        // The EXACT decomposition, for the same reason as in
        // get_fact_objects -- and here the adjacency reading was not merely
        // incomplete but wrong: a fact that has `rel` as ITS subject (a
        // statement ABOUT the fact, a rule condition, the rule-pattern
        // marking) is linked to rel in BOTH directions, exactly like rel's
        // own subject, and was reported as one. `(a p b) note ok` made
        // itself a subject of `p` with object `b`, which the documented
        // contract of zelph/sources -- and the query `S p b` -- deny.
        // get_fact_structures returns the list BY SHARED POINTER, so binding a
        // range-for to *get_fact_structures(...) reads a container whose only owner
        // died at the end of that expression -- the temporary is not lifetime-extended,
        // because the reference binds to the POINTEE. It survived on borrowed time:
        // while the structure cache held a second owner the memory stayed valid, so
        // the bug was invisible until a bulk pass stopped caching. Hold the pointer.
        const auto structures = get_fact_structures(this, rel, 3);
        for (const auto& fs : *structures)
        {
            if (fs.predicate != predicate || fs.objects.count(object) == 0) continue;

            if (!is_var(fs.subject)) subjects.insert(fs.subject);
        }
    }

    return subjects;
}

// Transitive closure following the predicate forward (subject -> object).
// include_start true gives the reflexive closure (SPARQL `*`); with false
// (SPARQL `+`) the start node is still included when it is reachable from
// itself via a cycle of one or more steps.
//
// Two-stage strategy: a lock-once direct traversal handles small closures
// without any index; once its scan budget is exhausted (hub nodes), the
// closure switches to the cached per-predicate index.
//
// A closure does not traverse the engine's bookkeeping, just as a rule
// condition does not match it: `{(g p h) (g q h)} ~⁺ O` answered
// `refuted` for the contradiction's record. The predicate index cannot
// exclude bookkeeping, because zelph/sources reads the identical index and
// incorporates bookkeeping into its lists, thus causing the walk over the
// index to omit its steps. For each predicate except `~` and `in`, the sole
// expense lies in the comparison within bookkeeping_facts.
adjacency_set Zelph::transitive_targets(const Node start, const Node predicate, const bool include_start) const
{
    // Before the locks, never inside them -- see unasserted_snapshot.
    const auto skip        = unasserted_snapshot();
    const auto bookkeeping = bookkeeping_facts(predicate);

    adjacency_set result;
    if (_pImpl->try_transitive_direct(start, predicate, include_start, /*forward*/ true, kDirectClosureScanBudget, skip.get(), result, 0, nullptr, bookkeeping.get()))
        return result;

    result.clear();
    const auto         idx   = _pImpl->predicate_index(predicate, skip.get());
    const SkippedSteps steps = bookkeeping ? bookkeeping_steps(this, *bookkeeping, predicate) : SkippedSteps{};
    return bfs_over_index(idx->forward, start, include_start, bookkeeping ? &steps : nullptr, /*forward*/ true);
}

// A shortest walk, as the facts it traverses: the inquiry into which edges a
// path condition rests on. The direct traversal halts upon reaching the
// target and records how it reached every node; it walks around the facts in
// `unasserted` -- unasserted_snapshot(), which the caller takes once for
// multiple walks -- and in `without`. Within `scan_budget` adjacency entries,
// because the index the closure falls back to beyond that recognizes nodes,
// not facts; `exhausted` says the budget was depleted.
bool Zelph::transitive_walk(const Node start, const Node target, const Node predicate, const bool include_start, const adjacency_set* unasserted, const adjacency_set* without, const size_t scan_budget, std::vector<Node>& edges, bool& exhausted, size_t* const scanned) const
{
    edges.clear();
    exhausted = false;

    // This traversal sidesteps the bookkeeping that the closure avoids (see
    // transitive_targets); otherwise it would explain a route through a
    // step that the closure did not take.
    const auto           bookkeeping = bookkeeping_facts(predicate);
    adjacency_set        around;
    const adjacency_set* avoided = without;
    if (bookkeeping)
    {
        around = *bookkeeping;
        if (without != nullptr)
            for (const Node nd : *without)
                around.insert(nd);
        avoided = &around;
    }

    adjacency_set                                             reached;
    ankerl::unordered_dense::map<Node, std::pair<Node, Node>> via;
    if (!_pImpl->try_transitive_direct(start, predicate, include_start, /*forward*/ true, scan_budget, unasserted, reached, target, &via, avoided, scanned))
    {
        exhausted = true;
        return false;
    }
    if (reached.count(target) == 0) return false;
    if (include_start && target == start) return true;

    for (Node at = target;;)
    {
        const auto it = via.find(at);
        if (it == via.end()) return false;
        edges.push_back(it->second.first);
        at = it->second.second;
        if (at == start) break;
    }
    std::reverse(edges.begin(), edges.end());
    return true;
}

// Transitive closure following the predicate backward (object -> subject).
adjacency_set Zelph::transitive_sources(const Node target, const Node predicate, const bool include_target) const
{
    const auto skip        = unasserted_snapshot();
    const auto bookkeeping = bookkeeping_facts(predicate); // see transitive_targets

    adjacency_set result;
    if (_pImpl->try_transitive_direct(target, predicate, include_target, /*forward*/ false, kDirectClosureScanBudget, skip.get(), result, 0, nullptr, bookkeeping.get()))
        return result;

    result.clear();
    const auto         idx   = _pImpl->predicate_index(predicate, skip.get());
    const SkippedSteps steps = bookkeeping ? bookkeeping_steps(this, *bookkeeping, predicate) : SkippedSteps{};
    return bfs_over_index(idx->backward, target, include_target, bookkeeping ? &steps : nullptr, /*forward*/ false);
}

adjacency_set Zelph::filter(const adjacency_set& source, const Node target) const
{
    adjacency_set result;

    for (Node nd : source)
    {
        if (_pImpl->get_right(nd).count(target) == 1)
        {
            result.insert(nd);
        }
    }

    return result;
}

adjacency_set Zelph::filter(const Node fact, const Node relationType, const Node target) const
{
    adjacency_set source     = _pImpl->get_right(fact);
    adjacency_set left_nodes = _pImpl->get_left(fact);
    adjacency_set result;

    for (Node nd : source)
    {
        // Exclude the subject of the fact, since it is connected
        // bidirectionally. If <subject relationType target> is true, the
        // subject would be included in the result by mistake.
        if (left_nodes.count(nd) != 0) continue;

        // The question is whether `nd relationType target` HOLDS, and the
        // exact probe is the only way to ask it. Walking nd's neighbourhood
        // for a fact with the right predicate and the right node among its
        // objects answers a weaker question -- it never checks that nd is
        // that fact's SUBJECT -- and the two come apart on the one shape
        // that matters most here.
        //
        // A fact's outgoing edges hold its parents as well as its subject
        // and predicate, so for the consequence of a rule, `source` contains
        // the RULE. The rule in turn points at its own subject, the
        // condition; and if that condition happens to be a relation-type
        // declaration -- `(R ~ ->) => (R declared yes)`, the natural way to
        // write a rule that quantifies over all predicates -- the walk found
        // `R ~ ->` from the rule, saw the right predicate and the right
        // object, and reported the RULE as a second relation type of the
        // consequence. deduce() then refused the ambiguity and the rule
        // derived nothing at all, silently. Adding any second condition hid
        // it again.
        if (check_fact(nd, relationType, {target}).is_known())
        {
            result.insert(nd);
        }
    }

    return result;
}

adjacency_set Zelph::relation_types_of(const Node fact) const
{
    adjacency_set relations = filter(fact, core.IsA, core.RelationTypeCategory);
    if (!relations.empty() || !Impl::is_hash(fact) || Impl::is_var(fact)) return relations;

    // The identifier decides: it is the hash of the triple, and only the
    // reading where the subject serves as the predicate reproduces it.
    adjacency_set objects;
    const Node    subject = parse_fact(fact, objects);
    if (subject != 0 && !objects.empty() && create_hash(subject, subject, objects) == fact
        && check_fact(subject, core.IsA, {core.RelationTypeCategory}).is_known())
        relations.insert(subject);
    return relations;
}

adjacency_set Zelph::filter(const adjacency_set& source, const std::function<bool(const Node nd)>& f)
{
    adjacency_set result;

    for (const Node nd : source)
    {
        if (f(nd)) result.insert(nd);
    }

    return result;
}

adjacency_set Zelph::get_left(const Node b) const
{
    return _pImpl->get_left(b);
}

adjacency_set Zelph::get_right(const Node b) const
{
    return _pImpl->get_right(b);
}

void Zelph::watch_removals(std::shared_ptr<const ankerl::unordered_dense::set<Node>> nodes) const
{
    _pImpl->watch_removals(std::move(nodes));
}

bool Zelph::touched_by_removal(const Node n) const
{
    return _pImpl->touched_by_removal(n);
}

std::uint64_t Zelph::graph_epoch() const
{
    return _pImpl->graph_epoch();
}

namespace
{
    // A single counter for every engine within the process, ensuring that a
    // value read before an engine was substituted with a new one is never
    // met again.
    std::atomic<std::uint64_t> content_epoch_counter{0};
}

std::uint64_t Zelph::content_epoch()
{
    return content_epoch_counter.load(std::memory_order_acquire);
}

void Zelph::advance_content_epoch()
{
    content_epoch_counter.fetch_add(1, std::memory_order_acq_rel);
}

std::size_t Zelph::left_count(const Node b) const
{
    return _pImpl->left_count_of(b);
}

std::size_t Zelph::right_count(const Node b) const
{
    return _pImpl->right_count_of(b);
}

// Facts that use `relation` as their PREDICATE. get_left(relation) is NOT
// that set: it also holds the facts in which the relation is the SUBJECT,
// starting with its own `relation ~ ->` declaration.
adjacency_set Zelph::get_facts_of_predicate(const Node relation) const
{
    adjacency_set facts;

    // The nodes pointing AT a relation are the facts that use it as their
    // PREDICATE plus the facts that merely have it as their SUBJECT -- a
    // fact points at both of them. What separates the two roles is the back
    // edge: a subject (like an object) points at its fact, a predicate does
    // not. Without that second test a predicate's own `relation ~ ->`
    // declaration counted as a use of it, so .list-predicate-usage reported
    // one use for a predicate nothing had ever used.
    //
    // A fact whose subject IS its predicate carries both roles in ONE edge,
    // so the back edge cannot separate them: _left[fact] = {subject,
    // predicate} then has a single entry, and the relation is the predicate
    // after all. `~ ~ ->` is that fact, and every network has it.
    // A COMPOSITE relation -- a fact or a cons cell in predicate position --
    // is pointed at by its own subject and objects as well, and those passed
    // both tests above: the subject through the single-outgoing-edge
    // exemption, the object because a fact does not point back at it. So
    // `x (a p b) y` reported THREE uses of `(a p b)` where there is one. The
    // parts are excluded by name rather than by another edge test, because
    // no edge distinguishes them -- one decomposition of the relation
    // itself, and nothing at all for the atomic predicate every bulk import
    // consists of.
    adjacency_set own_parts;
    if (Impl::is_hash(relation))
    {
        const FactStructure fs = get_preferred_structure(this, relation, 3);
        if (fs.predicate != 0 && fs.subject != 0)
        {
            own_parts.insert(fs.subject);
            own_parts.insert(fs.predicate);
            for (const Node o : fs.objects)
                own_parts.insert(o);
        }
    }

    const Network::ReadScope scope = read_scope();

    for (const Node fact : scope.left(relation))
    {
        if (own_parts.count(fact) == 1) continue;

        if (scope.left(fact).count(relation) == 0 || scope.right(fact).size() == 1 || scope.subject_is_predicate(fact, relation))
        {
            facts.insert(fact);
        }
    }

    return facts;
}

bool Zelph::has_left_edge(Node b, Node a) const
{
    return _pImpl->has_left_edge(b, a);
}

bool Zelph::has_right_edge(Node a, Node b) const
{
    return _pImpl->has_right_edge(a, b);
}

Node Zelph::create_hash(const adjacency_set& vec)
{
    return Network::create_hash(vec);
}

Node Zelph::create_hash(const Node predicate, const Node subject, const adjacency_set& objects)
{
    return Network::create_hash(predicate, subject, objects);
}

bool Zelph::is_hash(Node a)
{
    return Network::is_hash(a);
}

bool Zelph::is_var(Node a)
{
    return Network::is_var(a);
}

bool Zelph::is_recipe(Node a)
{
    return Network::is_recipe(a);
}

bool Zelph::is_value_recipe(Node a)
{
    return Network::is_value_recipe(a);
}

bool Zelph::is_template_id(Node a)
{
    return Network::is_template_id(a);
}

// When the rule names its own bucket, the node a firing writes into is the
// bucket's term, keyed by no variable -- one node for every firing of the rule.
Node Zelph::bucket_term_id(const Node bucket)
{
    return Network::recipe_id(bucket, Network::recipe_key({}), false);
}

bool Zelph::is_rule_template(const Node n) const
{
    return Network::is_template_id(n);
}

void Zelph::enter_template_scope()
{
    std::lock_guard lock(_pImpl->_template_scopes_mtx);
    const auto [scope, opened] = _pImpl->_template_scopes.try_emplace(std::this_thread::get_id());
    if (opened) _pImpl->_template_scopes_open.fetch_add(1, std::memory_order_release);
    ++scope->second.depth;
}

void Zelph::leave_template_scope()
{
    std::lock_guard lock(_pImpl->_template_scopes_mtx);
    const auto      scope = _pImpl->_template_scopes.find(std::this_thread::get_id());
    if (scope == _pImpl->_template_scopes.end() || --scope->second.depth > 0) return;
    _pImpl->_template_scopes.erase(scope);
    _pImpl->_template_scopes_open.fetch_sub(1, std::memory_order_release);
}

void Zelph::enter_pattern_scope(std::vector<Node>* const deferred) const
{
    std::lock_guard lock(_pImpl->_pattern_scopes_mtx);
    const auto [scope, opened] = _pImpl->_pattern_scopes.try_emplace(std::this_thread::get_id());
    if (opened) _pImpl->_pattern_scopes_open.fetch_add(1, std::memory_order_release);
    ++scope->second.depth;
    scope->second.collectors.push_back(deferred);
}

void Zelph::leave_pattern_scope() const
{
    std::lock_guard lock(_pImpl->_pattern_scopes_mtx);
    const auto      scope = _pImpl->_pattern_scopes.find(std::this_thread::get_id());
    if (scope == _pImpl->_pattern_scopes.end()) return;
    scope->second.collectors.pop_back();
    if (--scope->second.depth > 0) return;
    _pImpl->_pattern_scopes.erase(scope);
    _pImpl->_pattern_scopes_open.fetch_sub(1, std::memory_order_release);
}

bool Zelph::defer_marking(const Node marking) const
{
    if (_pImpl->_pattern_scopes_open.load(std::memory_order_acquire) == 0) return false;
    std::lock_guard lock(_pImpl->_pattern_scopes_mtx);
    const auto      scope = _pImpl->_pattern_scopes.find(std::this_thread::get_id());
    if (scope == _pImpl->_pattern_scopes.end()) return false;
    if (std::vector<Node>* const deferred = scope->second.collectors.back()) deferred->push_back(marking);
    return true;
}

bool Zelph::holds_rule_text(const Node container) const
{
    return is_rule_template(container) || check_fact(container, core.IsA, {core.Conjunction}).is_known();
}

Answer Zelph::check_fact(const Node subject, const Node predicate, const adjacency_set& objects) const
{
    const Node relation = Impl::create_hash(predicate, subject, objects);

    // A pure probe: a node that is present yet does not possess these edges
    // is answered as unknown, and fact() refuses to write the statement into
    // it (refer to the refusal there, which names both statements).
    const bool known = _pImpl->fact_edges_hold(relation, subject, objects);

    if (known)
    {
        return {_pImpl->probability(relation, predicate), relation};
    }
    else
    {
        return Answer(relation); // unknown
    }
}

Node Zelph::predicate_of(const Node nd) const
{
    if (nd == 0 || !Impl::is_hash(nd) || Impl::is_var(nd)) return 0;

    FactStructurePtr genuine;
    if (try_get_genuine_structure(nd, genuine) && genuine && !genuine->empty())
        return genuine->front().predicate;

    // No store entry: the stores were disarmed via a bulk path (or the
    // node was never created through fact()). parse_relation resolves it;
    // when subject equals predicate, it returns the subject, which IS the
    // predicate.
    return parse_relation(nd);
}

Answer Zelph::check_fact(const Node relation) const
{
    if (relation == 0
        || !Impl::is_hash(relation)
        || Impl::is_var(relation)
        || !_pImpl->exists(relation))
    {
        return Answer(relation); // unknown
    }

    const Node predicate = predicate_of(relation);
    if (predicate == 0) return Answer(relation); // structure unreadable -> unknown

    return {_pImpl->probability(relation, predicate), relation};
}

void Zelph::refuse_opacity_change(const Node predicate, const bool removing, const std::function<bool(Node)>& ignore) const
{
    // The engine directly reads a core predicate: `~` declared opaque
    // would cause every subject in a marking or tag to become a
    // mention.
    if (!removing && _core_names_by_node.count(predicate) != 0)
        throw std::runtime_error("fact(): a core predicate cannot be declared opaque -- " + _core_names_by_node.at(predicate));

    const Node holding = fact_holding_statement(predicate, ignore);
    if (holding == 0) return;

    std::string rendered;
    zelph::string::node_to_string(this, rendered, _lang, holding, 3);
    const std::string name = get_name(predicate, _lang, true);
    throw std::runtime_error(std::string(removing ? "a predicate's opacity cannot be removed while one of its facts holds a statement -- "
                                                  : "fact(): a predicate cannot be declared opaque while one of its facts holds a statement -- ")
                             + zelph::string::unmark_identifiers(name) + ": " + zelph::string::unmark_identifiers(rendered));
}

Node Zelph::fact(const Node subject, const Node predicate, const adjacency_set& objects, const long double probability)
{
    return fact(subject, predicate, objects, probability, Writer::statement);
}

Node Zelph::resolution_fact(const Node subject, const Node predicate, const adjacency_set& objects)
{
    return fact(subject, predicate, objects, 1, Writer::resolution);
}

Node Zelph::fact(const Node subject, const Node predicate, const adjacency_set& objects, const long double probability, const Writer writer)
{
    // Once a rule has been written, its text becomes immutable. Writing a
    // membership into a collection within the rule's own text, or into the
    // rule's condition set, when no rule is currently being written -- via
    // Janet, the C ABI, or a name given to the collection -- would alter what
    // the rule says: the rule would print the member, its fingerprint and the
    // variables contained in its text would no longer belong to it, and the
    // rule typed again would be a second rule. A member already held by the
    // collection, restated, does not create a new node, yet the scripting
    // interface takes a statement as a claim, and a claim turns the rule's
    // literal member into data; thus, the write is refused regardless of
    // whether the membership already exists, before any modification. A
    // statement that binds such a collection writes into its data term instead
    // (instantiate_fact), as does every firing (container_plan). Within the
    // template scope -- which the parser opens for a rule, and which
    // zelph/build-rule, zelph/rule-text, and a construction open, each on the
    // thread responsible for writing the rule -- a rule is being written, and
    // its own collections are written as its text. Ordinary data pays the
    // comparison on the predicate; a membership outside every rule pays a bit
    // test per object and a tag lookup per object that is no rule's own
    // collection.
    if (predicate == core.PartOf && !_pImpl->template_scope_open())
    {
        for (const Node t : objects)
        {
            if (!holds_rule_text(t)) continue;
            std::string rendered;
            zelph::string::node_to_string(this, rendered, _lang, t, 3);
            const bool own = is_rule_template(t);
            throw std::runtime_error(
                "fact(): " + zelph::string::unmark_identifiers(rendered)
                + (own ? " is a collection of a rule's own text" : " is the condition set of a rule")
                + ", and a rule's text is fixed once the rule is written. Data about it goes to its data term, which "
                + (own ? "the rule's firings and a rule that binds the collection write into." : "a rule that binds the set writes into."));
        }
    }

    // A refutation writes its fact at probability 0. When that fact is itself a
    // refutation marking -- `¬((a p b) ~ refuted)` or zelph/refute or
    // zelph_refute of the marking -- the marking branch below still applied it,
    // and the denial of a refutation refuted the fact: a claimed `a p b` left
    // every query, and one nobody claimed was refuted. Only a claimed marking
    // refutes, and a refutation is withdrawn by removing its marking, so the
    // statement is refused before anything is written.
    if (probability <= 0.0L && predicate == core.IsA && objects.size() == 1)
    {
        if (const Node refuted = get_node(refuted_fact_name(), "zelph");
            refuted != 0 && *objects.begin() == refuted && predicate_of(subject) != 0 && !var_in_closure(subject))
        {
            const std::string marking = "(" + format(subject) + ") ~ refuted";
            throw std::runtime_error("fact(): \"" + marking + "\" is a refutation, and a refutation cannot itself be refuted: it is withdrawn by removing its marking");
        }
    }

    // A statement enclosed within a condition matches a fact that carries
    // additional objects too (refer to `ground_pattern` in unification.cpp).
    // The first such fact switches grounding and the anchors to their reading
    // via the statement's atoms. A pattern carries variables and states
    // nothing, hence it does not count.
    if (objects.size() > 1 && !_pImpl->_may_hold_wider_statements.load(std::memory_order_relaxed)
        && !Impl::is_var(subject) && !var_in_closure(subject) && !Impl::is_var(predicate) && !var_in_closure(predicate)
        && std::none_of(objects.begin(), objects.end(), [this](const Node o)
                        { return Impl::is_var(o) || var_in_closure(o); }))
        _pImpl->_may_hold_wider_statements.store(true, std::memory_order_release);

    const Answer answer = check_fact(subject, predicate, objects);

    if (answer.is_known())
    {
        if (answer.is_wrong() && probability > 0.5L)
        {
            throw std::runtime_error("fact(): this fact is known to be wrong");
        }
        else if (answer.is_correct() && probability < 0.5L)
        {
            // The refusal asks whether anybody CLAIMED the fact, not what the
            // probability says. A rule's pattern has probability 1 and does
            // not constitute a claim: following "(mary waits nil) => (john
            // sleeps nil)", the line "¬(john sleeps nil)" was refused as known
            // to be true, even though no assertion was made that John sleeps.
            // Refuting such a statement assigns it a weight of 0 and returns
            // it.
            if (is_rule_pattern(answer.relation()))
            {
                apply_refutation(answer.relation());
                return answer.relation();
            }
            throw std::runtime_error("fact(): this fact is known to be true");
        }
    }
    else
    {
        if (objects.count(predicate) == 1)
        {
            // 1 13 13
            // ~ is for example is for example <= (~  is opposite of  is for example), (is for example  ~  ->)
            //
            // A chained "A => B => C" lands here: the parser reads one
            // statement whose predicate `=>` also stands among the objects.
            // Which of the two arrows binds tighter is genuinely undecided,
            // so the answer is not a default reading but a demand to say
            // which was meant.
            if (predicate == core.Causes)
            {
                throw std::runtime_error("fact(): a rule inside a rule has to be parenthesised -- "
                                         "write A => (B => C) or (A => B) => C, since \"A => B => C\" "
                                         "does not say which arrow binds tighter");
            }

            // Naming the node is the whole point: "facts with same relation
            // type and object are not supported" told the reader neither
            // WHICH node stands in both roles nor what to do about it, and
            // this refusal is almost never reached by writing that on
            // purpose.
            const std::string named = get_formatted_name(predicate, get_lang());
            const std::string what  = named.empty() ? std::string("the predicate") : "\"" + named + "\"";

            // The rule arrow among the OBJECTS is the tell. It means the line
            // was meant as a rule and was read as ONE statement, because the
            // comma list and the consequence carry no parentheses:
            // "R is transitive, A R B, B R C => A R C" makes `R` the predicate
            // of the last condition and `=>` one of its objects. That is how
            // this refusal is reached in practice.
            if (objects.count(core.Causes) == 1)
            {
                throw std::runtime_error(
                    "fact(): " + what + " is both the predicate and an object here, and the rule arrow \"=>\" "
                                        "stands among the objects as well -- so this line was read as one statement "
                                        "rather than as a rule. A rule needs parentheses around its condition list "
                                        "and around its consequence: \"(A, B, C) => (D)\".");
            }

            throw std::runtime_error(
                "fact(): " + what + " is both the predicate and an object of this statement. A fact is identified "
                                    "by its predicate, so one node cannot stand in both roles at once.");
        }

        // A rule whose consequence is a CONJUNCTION. "A => (B, C)" reads as
        // "A implies both", and zelph can say that -- but as several OBJECTS
        // of one rule, not as a set. The engine deduces the objects of a `=>`
        // fact and has no reading for a set node in that position, so the
        // rule was built, was listed by .list-rules, and then derived nothing
        // at all, without a word. The one comparison against core.Causes is
        // what ordinary data pays for this.
        if (predicate == core.Causes)
        {
            for (const Node t : objects)
            {
                if (check_fact(t, core.IsA, {core.Conjunction}).is_known())
                {
                    throw std::runtime_error(
                        "fact(): a rule cannot have a conjunction as its consequence. "
                        "Write the consequences as several objects of the same rule: "
                        "\"A => (B) (C)\" instead of \"A => (B, C)\". They then share "
                        "their fresh variables, which two separate rules would not.");
                }
            }
        }

        // A conjunction cannot be negated: interpreting "¬(A, B)" requires
        // a negation over several conditions at once, a capability the
        // evaluation lacks, and it used to be evaluated as "A and B" -- the
        // opposite of what was written, silently. Rejecting the combination
        // here catches both spellings, because the sugar and the explicit
        // "((...) ~ negation)" form both end up creating this very fact. The
        // cost for ordinary data is one comparison against core.IsA.
        if (predicate == core.IsA && objects.size() == 1)
        {
            const Node tag   = *objects.begin();
            const Node other = tag == core.Negation    ? core.Conjunction
                             : tag == core.Conjunction ? core.Negation
                                                       : 0;

            if (other != 0 && check_fact(subject, core.IsA, {other}).is_known())
            {
                throw std::runtime_error(
                    "fact(): a condition cannot be a conjunction and a negation at once. "
                    "zelph negates a single fact pattern, not a group of them. "
                    "Use De Morgan: not(A and B) is the same as (not A) or (not B), and a "
                    "disjunction is written as several rules with the same consequence.");
            }

            // `¬¬(F)` says F, and interpreting it in that manner requires a
            // second negation level, which evaluation does not support. The
            // parser refuses the shorthand; the fact written out, `(((F) ~
            // negation) ~ negation)`, and a Janet or C program that negates
            // a negation both arrive at this point, before anything can name
            // the fact as a condition.
            if (tag == core.Negation && negation_operand(subject) != 0)
                throw std::runtime_error("\"¬\" does not nest: \"" + format(subject) + "\" is a negation already, and negating it would need a second negation level. Write the plain pattern for the positive condition.");
        }

        // A set constant is its members -- that is what identifies it -- so
        // there is nothing to add to. `x in {a b}` used to extend the very
        // set it named, leaving a node whose identity said {a b} while it
        // rendered {a b x}. The extensible container has its own literal and
        // the message names it. One comparison against core.PartOf for
        // ordinary data; is_set_constant then rejects every collection on the
        // id alone, without reading a member.
        // A pattern is not a claim. `X in {a b}` is how a rule quantifies
        // over the members, and the fact has to exist for unification to
        // match it; only a variable-free subject would really be adding an
        // element. (The already-known case never reaches here: this branch
        // runs only when the fact is about to be CREATED, so writing
        // `a in {a b}` -- true by construction -- is a no-op, not an error.)
        if (predicate == core.PartOf && objects.size() == 1
            && !Impl::is_var(subject) && !var_in_closure(subject)
            && is_set_constant(*objects.begin()))
        {
            std::string rendered;
            zelph::string::node_to_string(this, rendered, _lang, *objects.begin(), 3);
            throw std::runtime_error(
                "fact(): a set constant cannot be extended -- " + zelph::string::unmark_identifiers(rendered)
                + " IS its members. Write the collection literal @{...} for a container that membership can grow.");
        }

        // The mark of a rule's pattern is the engine's own bookkeeping, which it
        // writes itself (mark_patterns, restore_rule_patterns). When manually entered
        // -- on a standalone line, embedded within a statement, generated by a
        // program, triggered by a firing, or present in a rule's text -- it was a
        // standard fact within the session that produced it, whereas a .load
        // reconstructs the pattern index from each such fact: following a .save and a
        // subsequent .load, a claimed `a p b` beside a typed
        // `(a p b) ~ "rule pattern"` answered no query. A mark of a pattern
        // containing a variable is a pattern in either reading, and a mark on an atom
        // changes nothing that is read about the atom, thus both remain permissible;
        // the name is looked up only for a fact concerning another fact. A command
        // that resolves a printed pattern builds the mark within a scratch cluster,
        // which it discards before reading the answer (resolution_fact), so that
        // writer is exempt as well.
        if (writer != Writer::pattern_mark && writer != Writer::resolution && predicate == core.IsA && objects.size() == 1 && Impl::is_hash(subject) && !Impl::is_var(subject))
        {
            if (const Node mark = rule_pattern_predicate(false); mark != 0 && *objects.begin() == mark && !var_in_closure(subject))
                throw std::runtime_error("fact(): (" + format(subject) + ") ~ \"rule pattern\" is the mark the engine keeps of a rule's own pattern, and only the engine writes it -- a statement becomes a rule's pattern by being written in a rule");
        }

        // A declaration `P ~ opaque` that is claimed without being written
        // down as a pattern inside a rule's construction or within a
        // pattern scope.
        bool declares_opaque = false;
        if (predicate == core.IsA && objects.size() == 1 && !Impl::is_var(subject))
        {
            const Node opaque = get_node(opaque_name(), "zelph");
            declares_opaque   = opaque != 0 && *objects.begin() == opaque && !var_in_closure(subject)
                             && !_pImpl->template_scope_open() && !_pImpl->pattern_scope_open();
            if (declares_opaque) refuse_opacity_change(subject, false);
        }

        // We only allow relations with the same subject and object in the case of a single object. If there are several
        // objects and one of them is identical to the subject, we wouldn't know that such an object exists.
        // Real life examples from Wikidata:
        // South Africa (Q258)  country (P17)  South Africa (Q258)
        // or
        // chemical substance  has part  chemical substance ⇐ (matter  has part  chemical substance), (chemical substance  is subclass of  matter)
        //
        // Refused here, before any writing, like every refusal above.
        // It was formerly identified during the drawing of the new node's
        // edges, causing the refused statement to remain as an incomplete
        // node, alongside the declaration of its predicate. A subsequent
        // retry encountered that node, and when another object had been
        // connected before the throw, the remnants were interpreted as a fact
        // known to be wrong.
        if (objects.size() > 1 && objects.count(subject) == 1)
        {
            const std::string name_subject_object = get_name(subject, _lang, true);
            const std::string name_relationType   = get_name(predicate, _lang, true);

            throw std::runtime_error("fact(): facts with same subject and object are only supported for facts with a single object: " + name_subject_object + " " + name_relationType + " " + name_subject_object);
        }

        // The node corresponding to a fact IS the hash of its statement; thus,
        // a node possessing this identifier yet failing to hold the statement
        // (as previously indicated by check_fact) is unable to adopt it:
        // connecting edges there would merge two distinct statements into a
        // single one authored by nobody, an act that already occurred silently
        // before this refusal existed. A partially loaded view or a corrupted
        // file can result in such a node, as can two different statements
        // whose 62-bit hashes happen to coincide. Since nothing compares the
        // underlying structures, this is precisely where a collision becomes
        // evident.
        //
        // It is not a race either: fact() has no concurrent caller.
        // Inference creates facts on the thread executing it (within
        // Reasoning's network mutex, in addition), the worker pool
        // exclusively performs reads, the scripting and C interfaces perform
        // writes solely from the main thread, and the Wikidata importer's
        // threads write via the trusted path below, not through fact().
        if (_pImpl->exists(answer.relation()))
        {
            throw std::runtime_error("fact(): " + occupied_node_message(*this, subject, predicate, objects, answer.relation()));
        }

        // Whatever stands in predicate position IS a relation type, and saying
        // so is what makes the fact readable again later. The declaration used
        // to be skipped for hash nodes, i.e. for a predicate that is itself a
        // fact or a cons cell ("x (a p b) y", "a <=> b"): the fact was created,
        // the genuine store held its triple, and everything worked -- until a
        // .save/.load disarmed the store, after which the reconstruction had no
        // way to recognise the predicate and the fact silently stopped
        // answering queries. Declaring it costs one extra fact per DISTINCT
        // composite predicate, and only for those; ordinary data, every import
        // and the whole stdlib name their predicates and are unaffected.
        // (Note that the initial constructor call fact(core.IsA, core.IsA,
        // core.RelationTypeCategory) is executed as intended.)
        if (predicate != core.IsA)
        {
            fact(predicate, core.IsA, {core.RelationTypeCategory}, 1, Writer::implicit_declaration);
        }

        // The declaration that fact() writes itself is recorded within the
        // active cluster as a provisional entry (Network::note_declaration):
        // it goes with the facts that needed it, and stays as long as
        // something outside the cluster still uses it
        // (keep_shared_declarations). Recorded as an ordinary node for an
        // older predicate, the drop took `likes ~ ->` while `c likes d`,
        // which was written outside the cluster's boundary, remained intact.
        // Left unrecorded, it outlived everything the cluster had written
        // with it: a refused firing, a dropped scratch, and a question that a
        // program posed within a cluster it then dropped each left `P ~ ->`
        // behind. A declaration that is typed, derived, or written by a
        // program constitutes a statement just like any other and goes with
        // the drop.
        const bool implicit = writer == Writer::implicit_declaration;
        _pImpl->create(answer.relation(), implicit ? Network::Record::no : Network::Record::yes);
        if (implicit) _pImpl->note_declaration(answer.relation(), subject);

        _pImpl->connect(subject, answer.relation());
        _pImpl->connect(answer.relation(), subject);
        for (const Node t : objects)
        {
            if (t != subject) _pImpl->connect(t, answer.relation());
        }

        _pImpl->connect(answer.relation(), predicate, probability);

        // Per-node invalidation AFTER the edges are drawn: a reader that
        // cached a partial view of the half-constructed node between the
        // connects is invalidated here -- the former up-front wholesale
        // clear left that window open. See invalidate_fact_structures_for.
        invalidate_fact_structures_for(subject, predicate, objects, answer.relation());

        // The refuted index is a CACHE of the marking facts, and a cache has
        // to agree with what it caches whichever way round the graph was
        // built. rebuild_refuted_index reads the markings back on load, so a
        // marking asserted BY HAND -- `*(a p b) ~ refuted` -- has to reach the
        // index here, or the same network answers `S p O` with `a p b` in the
        // session that wrote it and without it after a save and a load.
        //
        // Whether writing an engine marking by hand SHOULD mean anything is
        // the open question `(myrel ~ ->) is odd` belongs to; this is not that
        // question. Whatever it means, it has to mean the same before and
        // after a round trip.
        //
        // Costs two comparisons, and only on the IsA path.
        //
        // Only a CLAIMED marking refutes. The patterns of a rule are
        // written through identical fact() calls, within the template
        // scope, and a pattern scope holds what a command resolves and
        // what a query still needs to claim: "((john waits nil) ~ refuted)
        // => (mary sleeps nil)" hid a `john waits nil` asserted either
        // before or after it. When such a marking is later claimed,
        // unmark_rule_pattern applies it. A marking whose fact holds a
        // variable is a pattern in either case and enters the index as it
        // always did, which is how the index read back on load treats it
        // too.
        if (predicate == core.IsA && objects.size() == 1)
        {
            if (const Node refuted = get_node(refuted_fact_name(), "zelph");
                refuted != 0 && *objects.begin() == refuted)
            {
                if (var_in_closure(subject))
                    note_refuted(subject);
                else if (!_pImpl->template_scope_open() && !defer_marking(answer.relation()))
                    apply_refutation(subject);
            }

            if (declares_opaque) note_opaque(subject);
        }

        // Maintain the template-variable store from the ACTUAL triple
        // (see Impl's _template_vars). Data facts -- the overwhelming
        // majority -- cost three store misses and allocate nothing.
        if (_pImpl->_template_vars_authoritative.load(std::memory_order_acquire))
        {
            std::shared_ptr<std::unordered_set<Node>> vars; // lazily allocated

            // A set pattern that a query or a program typed represents a
            // value, with its members not part of any structure; a statement
            // involving it holds its variables all the same (see
            // set_pattern_variables). Absent these, the statement a query
            // leaves in the graph answered as data: following
            // `{(X enters C) (C ~ card)} can nil`, `S can nil` also answered
            // `@{(C ~ card) (X enters C)} can nil`. The object of a membership
            // is entered as the walk enters it, not at all, and likewise the
            // set named by the conjunction tag being written.
            const bool set_patterns = _pImpl->_has_set_patterns.load(std::memory_order_acquire)
                                   && !(predicate == core.IsA && objects.size() == 1 && objects.count(core.Conjunction) == 1);

            const auto add_component = [&](const Node c, const bool enter)
            {
                if (Impl::is_var(c))
                {
                    if (!vars) vars = std::make_shared<std::unordered_set<Node>>();
                    vars->insert(c);
                }
                else if (Impl::is_hash(c))
                {
                    std::shared_ptr<const std::unordered_set<Node>> sub;
                    if (try_get_template_vars(c, sub) && sub)
                    {
                        if (!vars) vars = std::make_shared<std::unordered_set<Node>>();
                        vars->insert(sub->begin(), sub->end());
                    }
                }
                else if (set_patterns && enter)
                {
                    if (const auto sub = set_pattern_variables(c))
                    {
                        if (!vars) vars = std::make_shared<std::unordered_set<Node>>();
                        vars->insert(sub->begin(), sub->end());
                    }
                }
            };

            add_component(subject, true);
            add_component(predicate, true);
            for (const Node t : objects)
                add_component(t, predicate != core.PartOf);

            if (vars)
            {
                std::unique_lock lock(_pImpl->_template_vars_mtx);
                _pImpl->_template_vars.emplace(answer.relation(),
                                               std::shared_ptr<const std::unordered_set<Node>>(std::move(vars)));
            }
            else
            {
                // No variable within the structural closure, though
                // potentially one in the rule text: a component already
                // entered, a rule's own collection whose members hold one, or
                // the conditions of this `=>` fact (see Impl's
                // _rule_text_vars). Data facts incur a single atomic load and
                // a bit test per component, plus a probe per hash component
                // only once some rule text has been entered.
                //
                // A membership in a rule's own collection is that
                // collection's content, and holds a variable exactly when its
                // member does: read through the collection, `c in @{c Y}`
                // would depend on whether `Y in @{c Y}` was written before
                // it, and a membership flagged by the engine as a pattern
                // would read as one via another member's variable. A rule
                // asserting such a membership holds the collection's
                // variables, read here once, through a walk of the rule's
                // text. Outside the writing of a rule, no member is written
                // into a collection of a rule's text (refer to the refusal
                // above), thus nothing that reads one has to be entered
                // later. A member written into another rule's collection
                // while the template scope remains open is the exception, and
                // is not entered: the reconstruction walk sees it (see Impl's
                // _rule_text_vars).
                bool text_var = component_holds_text_variable(subject) || component_holds_text_variable(predicate);
                for (const Node t : objects)
                    text_var = text_var || ((predicate != core.PartOf || Impl::is_hash(t)) && component_holds_text_variable(t));
                if (!text_var && predicate == core.Causes)
                    text_var = conditions_hold_variable(subject) || rule_memberships_hold_variable(subject, objects);

                if (text_var)
                {
                    std::unique_lock lock(_pImpl->_template_vars_mtx);
                    _pImpl->_rule_text_vars.insert(answer.relation());
                    _pImpl->_rule_text_vars_any.store(true, std::memory_order_release);
                }
            }
        }

        // Record the genuine structure (reconstruction bypass): the exact
        // triple, known right here and immutable forever -- the node ID is
        // its hash. Self-facts arrive with objects == {subject}, matching
        // the walk's self-referential repair exactly, and subject ==
        // predicate now matches the walk too: it used to be excluded here
        // because the walk yielded EMPTY for those and the two stores had to
        // agree, but the walk reads them since the s == p branch in
        // get_fact_structures.
        if (_pImpl->_genuine_authoritative.load(std::memory_order_acquire))
        {
            auto           list = std::make_shared<FactStructureList>(1);
            FactStructure& fs   = list->front();
            fs.subject          = subject;
            fs.predicate        = predicate;
            fs.objects          = objects;

            std::unique_lock lock(_pImpl->_genuine_mtx);
            _pImpl->_genuine.emplace(answer.relation(), FactStructurePtr(std::move(list)));
        }

        // The index of fingerprints for the rules (find_equivalent_rule). A
        // new rule is placed in the queue, to undergo fingerprinting during
        // the next lookup, once it is complete. Data incurs a single
        // comparison on the predicate.
        if (predicate == core.Causes)
        {
            std::lock_guard lock(_pImpl->_rule_index_mtx);
            _pImpl->_rule_index_queue.push_back(answer.relation());
        }

        if (_on_fact_created)
        {
            _on_fact_created(answer.relation(), predicate);
            announce_named_records(answer.relation(), subject, predicate, objects);
        }
    }

    return answer.relation();
}

Node Zelph::fact_import_trusted_single_object(Node subject, Node predicate, Node object) const
{
    invalidate_fact_structures_cache();

    // A declaration typing a predicate must also drop the memoized
    // relation-type set, or every fact created with that predicate stays
    // invisible to queries and unification (fact-structure reconstruction
    // rejects predicates absent from the set). The condition keeps the
    // extra lock off the bulk import path: it holds once per predicate,
    // not once per fact.
    if (predicate == core.IsA && object == core.RelationTypeCategory)
        _pImpl->invalidate_relation_type_set();

    bool       foreign  = false;
    const Node relation = _pImpl->insert_fact_single_object_trusted(subject, predicate, object, foreign);

    // This triple's node holds something else. No data was written,
    // and the import continues: in an import of a billion triples, a damaged
    // file might produce such a node per line, hence only the first of an
    // import is reported, and end_bulk_import() reports the total number
    // observed. Precisely one thread draws the count that the import began
    // with. Rendering the report takes the locks that the insert operation
    // has just released.
    if (foreign
        && _pImpl->_import_conflicts.fetch_add(1, std::memory_order_relaxed)
               == _pImpl->_import_conflicts_at_begin.load(std::memory_order_relaxed))
    {
        error("Import: " + occupied_node_message(*this, subject, predicate, {object}, relation)
                  + " Further cases are counted, not reported one by one.",
              true);
    }

    return relation;
}

void Zelph::begin_bulk_import()
{
    _pImpl->_import_conflicts_at_begin.store(_pImpl->_import_conflicts.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

uint64_t Zelph::end_bulk_import()
{
    advance_content_epoch();

    const uint64_t unwritten = _pImpl->_import_conflicts.load(std::memory_order_relaxed)
                             - _pImpl->_import_conflicts_at_begin.load(std::memory_order_relaxed);

    if (unwritten == 1)
        error("Import: 1 triple was left unwritten because its node holds a different statement; it is reported above.", true);
    else if (unwritten > 1)
        error("Import: " + std::to_string(unwritten) + " triples were left unwritten because their nodes hold different statements; the first of them is reported above.", true);

    return unwritten;
}

// --- Synapses (neural substrate) ---
//
// A synapse is an entry in the sparse edge-weight store for a directed
// node pair -- and nothing else. It creates no adjacency: see the
// rationale in network.hpp. This replaces the former connect_weighted,
// whose adjacency insertion corrupted the fact structure of relation-node
// neurons (cons cells) and, conversely, let structural fact edges between
// neurons enter compiled masks as phantom synapses.
//
// No caches are invalidated here: fact structures and predicate indexes
// depend only on fact topology, which synapses do not touch. This keeps
// weight write-back during training cheap.
void Zelph::set_synapse(const Node from, const Node to, const double weight) const
{
    _pImpl->set_synapse(from, to, weight);
}

bool Zelph::has_synapse(const Node from, const Node to) const
{
    return _pImpl->has_synapse(from, to);
}

double Zelph::edge_weight(const Node from, const Node to, const double fallback) const
{
    return _pImpl->edge_weight(from, to, fallback);
}

void Zelph::set_edge_weight(const Node from, const Node to, const double weight) const
{
    _pImpl->set_edge_weight(from, to, weight);
}

// --- Node annotations ---
//
// No component related to the graph is affected: neither cache, index,
// nor record tied to the graph undergoes modification, and no rule or
// query can detect the presence of an annotation.
bool Zelph::annotate(const Node node, const std::string& key, const std::string& value) const
{
    return _pImpl->annotate(node, key, value);
}

Zelph::node_annotations Zelph::annotations(const Node node) const
{
    return _pImpl->annotations_of(node);
}

std::size_t Zelph::unannotate(const Node node, const std::string& key, const std::string* const value) const
{
    return _pImpl->unannotate(node, key, value);
}

void Zelph::set_fact_creation_observer(FactCreationObserver observer)
{
    _on_fact_created = std::move(observer);
}

/**
 * Builds a Lisp-style singly linked list from a vector of Node elements using cons cells.
 *
 * This implements exactly the classic Lisp representation:
 * (cons A (cons B (cons C nil)))
 *
 * Fundamental Lisp principle since McCarthy 1958: The entire list is represented solely
 * by the pointer to the outermost (first) cons cell. There is no additional list header
 * or wrapper node anywhere. This is why we can say "the outermost cons cell IS the list".
 *
 * Empty input returns core.Nil, which is the canonical empty list in Lisp.
 *
 * Crucial for identity: Repeated calls to sequence() with identical input vectors of Nodes
 * (or equivalently with identical strings via the other overload) will always return exactly
 * the same Node value. This is guaranteed because fact(subject, predicate, objects) computes
 * the Node via a reproducible hash based on the triple (subject, predicate, objects) and
 * returns the existing Node if one with that exact triple already exists; it never creates
 * duplicates. For the string-based overload, node(const std::string&) additionally ensures
 * that identical names map to the same Node before fact() is called.
 *
 * This structural identity is essential for rule-based arithmetic and consistent
 * reasoning in zelph, as it ensures that equivalent lists are literally the same object.
 */
Node Zelph::list(const std::vector<Node>& elements)
{
    if (elements.empty()) return core.Nil;

    // Build from right to left (Lisp-style cons list)
    // (cons A (cons B (cons C nil)))
    Node rest = core.Nil;

    for (const Node current_node : std::ranges::reverse_view(elements))
    {
        if (current_node == 0) continue;

        rest = fact(current_node, core.Cons, {rest});
    }

    return rest; // The outermost cons cell IS the list
}

/**
 * Builds a Lisp-style cons list from a vector of wide strings (typically single characters
 * or digits).
 *
 * Each string is first converted to a Node via node(element, lang), then the general
 * Node-based sequence() overload is called. This centralizes the cons-building logic
 * and guarantees both overloads produce exactly the same Lisp-style structure.
 *
 * The caller names the language used to locate the elements, and an empty value
 * indicates the current language. A list of characters names "zelph" there: a
 * character does not constitute a word in the session's language, and only a
 * fixed language makes the list the same node under every .lang.
 *
 * See the detailed explanation of structural identity in the Node-based overload above.
 *
 * Note that we could name the outermost cons cell like the concatenation of all element
 * node names using set_name(result, value, _lang, false). This would make some sense for
 * numbers, e.g. the elements "4" and "2" would give the list the name "42". Two nodes in
 * zelph can have the same name without any issues. We don't do this for several reasons:
 *  - It would only make sense for sequences that represent numbers.
 *  - It would raise several issues, e.g. what to do if a preloaded dataset like Wikidata
 *    includes that number as a named node already.
 *  - A natural distinction between digits and numbers already exists in this representation:
 *    the digit "4" is node("4"), while the number 4 is the cons cell fact(node("4"), Cons,
 *    {Nil}) — a structurally different node. Giving the cons cell the same name "4" would
 *    conflate two concepts that are better kept separate.
 */
Node Zelph::list(const std::vector<std::string>& elements, const std::string& lang)
{
    if (elements.empty()) return core.Nil;

    std::vector<Node> node_elements;
    node_elements.reserve(elements.size());

    for (const auto& element : elements)
    {
        node_elements.emplace_back(node(element, lang));
    }

    return list(node_elements);
}

/**
 * Creates a set represented as a dedicated node in the knowledge graph.
 *
 * In classic Lisp there is no direct equivalent to an unordered set as a primitive data structure.
 * Lisp traditionally uses lists (cons cells) for collections, and sets are usually simulated
 * with lists while manually ensuring uniqueness (member, adjoin, etc.) or with hash-tables in Common Lisp.
 *
 * This implementation follows a graph-theoretic / triple-store approach that fits Zelph perfectly:
 * - A dedicated "set node" is created that represents the set as a whole (the super-node).
 * - Each element is linked to this set node via the core.PartOf predicate: (element PartOf set_node).
 * - This allows natural, rule-based queries such as "which nodes are PartOf this set?" or
 *   "create the union of all sets that contain X" directly in zelph's reasoning engine.
 * - The representation is inherently unordered (no head/tail like cons lists) and supports
 *   easy extension for future rule-based arithmetic (union, intersection, cardinality etc.).
 *
 * Empty input returns core.Nil (consistent with sequence() and the canonical empty list/set in Lisp).
 */
// A COLLECTION -- the `@{...}` literal, and a rule's conjunction set.
//
// A container with an identity of its OWN: two collections written the same
// way are two different containers, and membership is asserted, so `x in c`
// extends one. That is the mereological reading zelph's own predicate name
// (PartOf) already carries.
Node Zelph::collection(const std::unordered_set<Node>& elements)
{
    return new_collection(elements, _pImpl->template_scope_open());
}

Node Zelph::conjunction_collection(const std::unordered_set<Node>& elements)
{
    return new_collection(elements, false);
}

Node Zelph::new_collection(const std::unordered_set<Node>& elements, const bool rule_text)
{
    if (elements.empty()) return core.Nil;

    // Create the super-node that embodies the collection as a whole.
    // Written while a rule is written, it forms a component of that rule's
    // text, and its identifier says so for good: no subsequent writing --
    // be it a claim naming it or another rule writing into it -- can turn
    // it into data.
    const Node collection_node = rule_text ? _pImpl->create_written_template() : _pImpl->create();
    if (rule_text) _pImpl->_has_template_collections.store(true, std::memory_order_release);

    for (const auto& current_node : elements)
    {
        // Link to the container
        fact(current_node, core.PartOf, {collection_node});
    }

    return collection_node;
}

namespace
{
    // The variables `node` holds outside every rule it mentions, appended to
    // `out`: the ones a query binds in it. Each rule's variables belong
    // exclusively to that rule, and a statement mentioning a rule does not
    // function as a query (the parser decides the same). A set pattern within
    // the components is read via its associated record.
    void free_variables(const Zelph* const z, const Node node, std::unordered_set<Node>& out, std::unordered_set<Node>& seen)
    {
        if (node == 0) return;
        if (Zelph::is_var(node))
        {
            out.insert(node);
            return;
        }
        if (!Zelph::is_hash(node))
        {
            if (const auto vars = z->set_pattern_variables(node)) out.insert(vars->begin(), vars->end());
            return;
        }
        if (!seen.insert(node).second || !z->var_in_closure(node, Zelph::VariableReading::Instance)) return;

        const auto structures = get_fact_structures(z, node, 1); // held: see get_fact_objects
        for (const auto& fs : *structures)
        {
            if (fs.predicate == z->core.Causes) continue;
            free_variables(z, fs.subject, out, seen);
            free_variables(z, fs.predicate, out, seen);
            for (const Node o : fs.objects)
                if (fs.predicate != z->core.PartOf || Zelph::is_hash(o) || Zelph::is_var(o)) free_variables(z, o, out, seen);
        }
    }
}

// A SET CONSTANT -- the `{...}` literal.
//
// Identified by its members, as the axiom of extensionality demands: two
// occurrences of `{a b}` are ONE node. That identity is the whole point --
// it is what lets a set literal in a rule condition denote the same thing
// as the same literal in the data, which a collection never can.
//
// Its membership is definitional rather than asserted, so it cannot be
// extended; the guard in fact() refuses that and names the alternative.
Node Zelph::set(const std::unordered_set<Node>& elements)
{
    if (elements.empty()) return core.Nil; // the empty set IS nil, as for `<>`

    // Extensionality needs KNOWN members. A literal carrying a variable
    // denotes a different set for every binding, so it is a pattern, not a
    // constant -- and it becomes the container that a pattern can be. This is
    // not a fallback but the definition: `{a b}` IS its members and can be
    // hash-consed; `{Y}` has none yet and cannot.
    //
    // It is also what keeps the engine's own conjunction sugar
    // `*{(A rel B) (B rel C)} ~ conjunction` working: those members are
    // condition patterns, never ground.
    //
    // A member that is itself a set pattern with a variable within it makes
    // the literal one as well: `{{X} a}` denotes a distinct set for every
    // binding of X, just like `{X a}` does. Built as a set constant based on
    // the inner collection, it matched only itself, and a statement involving
    // it held no variable, so the statement a program asked with it answered
    // as data.
    if (kind_unknowable(elements)
        || std::any_of(elements.begin(), elements.end(), [this](const Node e)
                       { return set_pattern_variables(e) != nullptr; }))
    {
        // Constructed outside the template scope, the collection takes the id
        // class of a set pattern (Network::create_value_set_pattern), which a
        // save writes with it and a load reads back. The record beside it
        // caches the variables that a statement over it holds (see
        // set_pattern_variables).
        if (_pImpl->template_scope_open()) return new_collection(elements, true);

        const Node pattern = _pImpl->create_value_set_pattern();
        for (const Node e : elements)
            fact(e, core.PartOf, {pattern});
        {
            std::unordered_set<Node> free;
            std::unordered_set<Node> seen;
            for (const Node e : elements)
                free_variables(this, e, free, seen);
            auto vars = free.empty() ? nullptr : std::make_shared<const std::unordered_set<Node>>(std::move(free));

            std::unique_lock lock(_pImpl->_set_patterns_mtx);
            _pImpl->_set_patterns.insert_or_assign(pattern, std::move(vars));
            _pImpl->_has_set_patterns.store(true, std::memory_order_release);
        }
        return pattern;
    }

    adjacency_set members;
    for (const Node e : elements)
        members.insert(e);

    const Node set_node = Impl::create_hash(members);

    const bool created = !_pImpl->exists(set_node);
    if (created) _pImpl->create(set_node);

    for (const auto& current_node : elements)
    {
        // Written a second time, the literal lands on the very same node and
        // every membership fact is already there. Skipping those is not an
        // optimisation: creating one would hit the extension guard, since by
        // then the node IS a complete set constant. While the FIRST occurrence
        // is being built the members are still incomplete, so the guard cannot
        // fire on it either.
        if (check_fact(current_node, core.PartOf, {set_node}).is_known()) continue;
        fact(current_node, core.PartOf, {set_node});
    }

    // A member that is a rule's own collection holding a variable makes the
    // set constant rule text (see var_in_closure): `{@{Y}}`. A set constant
    // is not a fact, so fact() never enters it.
    if (created && _pImpl->_template_vars_authoritative.load(std::memory_order_acquire)
        && std::any_of(elements.begin(), elements.end(), [this](const Node e)
                       { return component_holds_text_variable(e); }))
    {
        std::unique_lock lock(_pImpl->_template_vars_mtx);
        _pImpl->_rule_text_vars.insert(set_node);
        _pImpl->_rule_text_vars_any.store(true, std::memory_order_release);
    }

    return set_node;
}

// A member that is a collection does not make the kind unknowable,
// regardless of its contents: a collection is not a fact, and
// var_in_closure returns no in such cases. `{@{Y}}` is a set constant
// surrounding a single collection.
bool Zelph::kind_unknowable(const std::unordered_set<Node>& members) const
{
    for (const Node m : members)
    {
        if (Impl::is_var(m) || var_in_closure(m)) return true;
    }
    return false;
}

Node Zelph::recipe_collection(const Node id, const std::unordered_set<Node>& members, bool& created, std::vector<Node>* const asserted, const bool derived)
{
    // create(Node) records that node within the active cluster, meaning
    // that dropping the cluster takes the collection back with its
    // membership facts.
    created = !_pImpl->exists(id);
    if (created) _pImpl->create(id);

    // A node that exists can be missing one of these memberships in two
    // distinct manners. The `.remove` operation can eliminate a membership
    // where the member is a variable, and the collection remains intact:
    // since a variable was never an actual element, collect_doomed does not
    // trigger the container's doom for it, unlike when a ground member is
    // involved. The next construction under the identical recipe asserts
    // the membership once more and reports it in `asserted`, ensuring it is
    // marked with the rule's other parts rather than reading as a claim.
    // Furthermore, when two recipes collide, they share a single node,
    // which takes the members from both.
    //
    // A firing derives the memberships it writes. One that already exists
    // can be a ground pattern of a rule written earlier -- a rule
    // governing rules names a collection's data term in `(d in C) noted X` --
    // and obtaining it through derivation removes the mark, just as deduce()
    // does for a derived fact.
    for (const Node m : members)
    {
        if (const Answer membership = check_fact(m, core.PartOf, {id}); membership.is_known())
        {
            if (derived) unmark_rule_pattern(membership.relation());
            continue;
        }
        const Node membership = fact(m, core.PartOf, {id});
        if (asserted != nullptr) asserted->push_back(membership);
    }

    return id;
}

// Is this node a set constant, i.e. does it hash back to its own members?
//
// No marker and no side table: the identity IS the answer. A collection is
// assigned a counter or a recipe id, neither of which constitutes a hash,
// thus the cheap is_hash test rejects every collection before reading its
// members.
bool Zelph::is_set_constant(const Node node) const
{
    adjacency_set members;
    return set_constant_members(node, members);
}

bool Zelph::set_constant_members(const Node node, adjacency_set& out) const
{
    if (node == 0 || !Impl::is_hash(node) || Impl::is_var(node)) return false;

    adjacency_set members;
    bool          statements = false;

    for (const Node rel : _pImpl->get_right(node))
    {
        if (parse_relation(rel) != core.PartOf) continue;
        adjacency_set objs;
        const Node    member = parse_fact(rel, objs, 0);
        // A VARIABLE member is a rule pattern, not an element: the
        // condition `X in {a b}` has to exist as a fact for unification to
        // match against, and it would otherwise change what the set is.
        if (member != 0 && !Impl::is_var(member) && objs.count(node) == 1)
        {
            members.insert(member);
            statements = statements || Impl::is_hash(member);
        }
    }
    if (members.empty()) return false;

    // So is a member that is a statement holding a variable, nested one level
    // deeper: `(X p z) in {(x p z) c}`, asked or expressed as a rule's
    // condition, made the set `{c (x p z) (X p z)}`, which produces no node
    // upon hashing back, and the set constant was interpreted as a collection
    // while the pattern remained in existence. Asked solely when the members
    // miss the identity, meaning a set constant over which nothing quantifies
    // incurs no additional cost.
    if (Impl::create_hash(members) != node)
    {
        if (!statements) return false;
        adjacency_set elements;
        for (const Node m : members)
            if (!is_pattern_member(m)) elements.insert(m);
        if (elements.empty() || Impl::create_hash(elements) != node) return false;
        members = std::move(elements);
    }
    out = std::move(members);
    return true;
}

// The tests execute in order of lowest cost. During unification, each atom in
// a pattern that differs from the graph's node is examined, and an atom fails
// on its identifier and a single atomic load, meaning a session lacking set
// patterns incurs no additional expense.
bool Zelph::set_pattern_members(const Node node, adjacency_set& out) const
{
    if (node == 0 || Impl::is_var(node)) return false;

    // A set constant qualifies as a pattern if a template among its members
    // holds a variable, which var_in_closure reads (`{@{Y} a}`). A set
    // constant over a collection that set() built does not constitute a
    // pattern: no record names it.
    if (Impl::is_hash(node)) return var_in_closure(node) && set_constant_members(node, out);

    if (!is_rule_template(node) && !Impl::is_value_set_pattern(node)) return false;
    if (check_fact(node, core.IsA, {core.Conjunction}).is_known()) return false;
    if (is_rule_template(node) && !holds_variable_in_text(node)) return false;

    adjacency_set members;
    for (const Node rel : _pImpl->get_right(node))
    {
        if (parse_relation(rel) != core.PartOf) continue;
        adjacency_set objs;
        const Node    member = parse_fact(rel, objs, 0);
        if (member != 0 && objs.count(node) == 1) members.insert(member);
    }

    if (members.empty()) return false;
    out = std::move(members);
    return true;
}

bool Zelph::is_set_pattern(const Node node) const
{
    adjacency_set members;
    return set_pattern_members(node, members);
}

// The id test is performed initially: once a session has written a set
// pattern, every component of each new fact undergoes this check, and any
// node belonging to a different class costs a single bit test.
std::shared_ptr<const std::unordered_set<Node>> Zelph::set_pattern_variables(const Node node) const
{
    if (!Impl::is_value_set_pattern(node)) return nullptr;

    std::shared_ptr<const std::unordered_set<Node>> vars;
    bool                                            recorded = false;
    {
        std::shared_lock lock(_pImpl->_set_patterns_mtx);
        if (const auto it = _pImpl->_set_patterns.find(node); it != _pImpl->_set_patterns.end())
        {
            vars     = it->second;
            recorded = true;
        }
    }

    // A load clears the record, whereas a set pattern persists as one
    // through its identifier: its variables are read from its members the
    // first time they are requested, and recorded.
    if (!recorded)
    {
        // A program may write a set pattern into itself, either directly or via
        // a set pattern among its members, so that its members can lead back to
        // a pattern that this thread is still reading. Read again, that pattern
        // adds nothing, resulting in every pattern read in between lacking its
        // variables: their reading is returned but not recorded, and it is
        // redone the next time they are requested.
        thread_local std::vector<Node> reading;
        thread_local std::size_t       incomplete_from = std::numeric_limits<std::size_t>::max();
        if (const auto at = std::ranges::find(reading, node); at != reading.end())
        {
            incomplete_from = std::min(incomplete_from, static_cast<std::size_t>(at - reading.begin()) + 1);
            return nullptr;
        }

        struct Reading
        {
            explicit Reading(const Node n) { reading.push_back(n); }
            ~Reading()
            {
                reading.pop_back();
                if (reading.size() <= incomplete_from) incomplete_from = std::numeric_limits<std::size_t>::max();
            }
            Reading(const Reading&)            = delete;
            Reading& operator=(const Reading&) = delete;
        };

        const std::size_t depth    = reading.size();
        bool              complete = false;
        {
            const Reading read(node);
            adjacency_set members;
            if (!set_pattern_members(node, members)) return nullptr;
            std::unordered_set<Node> free;
            std::unordered_set<Node> seen;
            for (const Node m : members)
                free_variables(this, m, free, seen);
            if (!free.empty()) vars = std::make_shared<const std::unordered_set<Node>>(std::move(free));
            complete = depth < incomplete_from;
        }

        if (complete)
        {
            std::unique_lock lock(_pImpl->_set_patterns_mtx);
            _pImpl->_set_patterns.try_emplace(node, vars);
            _pImpl->_has_set_patterns.store(true, std::memory_order_release);
        }
    }

    // A conjunction set lists conditions, and it is not a set
    // pattern (set_pattern_members).
    if (!vars || check_fact(node, core.IsA, {core.Conjunction}).is_known()) return nullptr;
    return vars;
}

bool Zelph::may_hold_wider_statements() const
{
    return _pImpl->_may_hold_wider_statements.load(std::memory_order_acquire);
}

bool Zelph::may_hold_set_patterns() const
{
    return _pImpl->_has_set_patterns.load(std::memory_order_acquire)
        || _pImpl->_has_template_collections.load(std::memory_order_acquire)
        || !_pImpl->_template_vars_authoritative.load(std::memory_order_acquire);
}

Node Zelph::parse_fact(Node rule, adjacency_set& deductions, Node parent) const
{
    deductions.clear();
    adjacency_set candidates;

    for (Node nd : _pImpl->get_left(rule))
    {
        // Verify the presence of a bidirectional link (a trait typical of
        // Subject <-> Relation connections).
        // A probe, not a copy: the inner variable within a generator's
        // template serves as the subject for each rule built by the generator,
        // and copying its adjacency for each rule made reading N such rules
        // cost N^2.
        if (_pImpl->has_left_edge(nd, rule))
        {
            if (nd != parent)
            {
                candidates.insert(nd);
            }
        }
        else
        {
            if (nd != parent) deductions.insert(nd);
        }
    }

    if (candidates.empty()) return 0;
    if (candidates.size() == 1)
    {
        if (deductions.empty())
            deductions.insert(*candidates.begin()); // Self-referential: subject is its own object.
        return *candidates.begin();
    }

    // Conflict detected: Multiple nodes look like the subject.
    // This happens when a fact node is also the subject of other facts,
    // creating extra bidirectional links. For example, a cons cell <3>
    // that is also the subject of (<3> .. <4>) and (<3> ~ digit) will
    // have the relation nodes for those facts as additional candidates.
    //
    // Strategy: Filter out candidates that are themselves relation nodes
    // (i.e., nodes that represent other facts). A relation node always has
    // a recognized predicate (a RelationTypeCategory instance) in its
    // outgoing connections. We also filter the original structural cases.

    // --- Disambiguation ---
    // Multiple candidates look like the subject.  This happens when `rule`
    // is also the subject of other facts, creating extra bidirectional links.
    //
    // Strategy: identify and filter out "child-fact" candidates — hash nodes
    // whose only bidirectional neighbor (besides their own predicate) is `rule`
    // itself, meaning `rule` is THEIR subject, not the other way around.
    // This mirrors the proven logic in get_fact_structures().

    std::vector<Node> valid;
    valid.reserve(candidates.size());

    // The negated condition over `rule`, the fact `((rule) ~ negation)`,
    // features `rule` as its subject and never in reverse, and its
    // identifier says so exactly. The test below misses it once it is the
    // subject of a `~` fact of its own -- the mark of a ground rule pattern
    // -- since it does not follow a grandchild bearing the candidate's
    // predicate, and a negated ground path or neural condition then lost its
    // reading.
    const Node negation_of_rule = Impl::create_hash(core.IsA, rule, core.Negation);

    for (Node cand : candidates)
    {
        if (cand == negation_of_rule) continue;

        bool is_child_fact = false;

        // A candidate is a child-fact if 'rule' is its only subject.
        // Rule variables act as hash nodes but are primitive subjects, so exclude them from check.
        if (Impl::is_hash(cand) && !Impl::is_var(cand))
        {
            Node cand_pred = parse_relation(cand);
            if (cand_pred != 0)
            {
                adjacency_set cand_right = _pImpl->get_right(cand);
                adjacency_set cand_left  = _pImpl->get_left(cand);

                // `rule` must be bidirectional with `cand` for a child-fact relationship
                if (cand_right.count(rule) > 0 && cand_left.count(rule) > 0)
                {
                    // Check whether `cand` has another bidirectional neighbor
                    // besides `rule` and `cand_pred`.  If not, `rule` is cand's
                    // only subject candidate → cand is a child-fact of `rule`.
                    bool has_alternative_subject = false;
                    for (Node x : cand_right)
                    {
                        if (x == rule || x == cand_pred) continue;
                        if (cand_left.count(x) > 0)
                        {
                            // x is bidirectional with cand.
                            // If x is a hash node (and not a var) with different predicate,
                            // check if it is just a grandchild.
                            if (Impl::is_hash(x) && !Impl::is_var(x))
                            {
                                Node x_pred = parse_relation(x);
                                if (x_pred != 0 && x_pred != cand_pred)
                                {
                                    // x has a different predicate — check if its
                                    // only bidi neighbor (besides its own pred) is cand.
                                    adjacency_set x_right            = _pImpl->get_right(x);
                                    adjacency_set x_left             = _pImpl->get_left(x);
                                    bool          x_is_child_of_cand = true;
                                    for (Node y : x_right)
                                    {
                                        if (y == cand || y == x_pred) continue;
                                        if (x_left.count(y) > 0)
                                        {
                                            x_is_child_of_cand = false;
                                            break;
                                        }
                                    }
                                    if (x_is_child_of_cand) continue; // x is grandchild, not alt subject
                                }
                            }
                            has_alternative_subject = true;
                            break;
                        }
                    }
                    if (!has_alternative_subject)
                    {
                        is_child_fact = true;
                    }
                }
            }
        }

        if (!is_child_fact)
        {
            valid.push_back(cand);
        }
    }

    // --- Self-referential repair (disambiguation path) ---------------------
    // Mirrors the single-candidate branch above: a fact node whose
    // reconstructed object set is empty is a fact with subject == object --
    // fact() draws no separate object edge in that case, so the subject IS
    // the object. The disambiguation path is reached precisely when the
    // fact node is ALSO the subject of further facts (their backlinks are
    // additional bidirectional neighbors); those extra facts land in
    // `candidates`, get filtered as child-facts, and previously left
    // `deductions` empty -- silently dropping the implicit object.
    // Symptom: ((X op X) ...) reconstructed and rendered as ((X op ?) ...)
    // as soon as the inner fact acquired a second consumer. Division X/X
    // triggers this systematically (candidate q=1 makes P == M == N).
    auto selfref_repair = [&deductions](Node subj) -> Node
    {
        if (subj != 0 && deductions.empty())
            deductions.insert(subj);
        return subj;
    };

    if (valid.size() == 1) return selfref_repair(valid[0]);
    if (valid.empty()) return 0;

    if (valid.size() == 1) return valid[0];
    if (valid.empty()) return 0;

    // Heuristic Preferences if still ambiguous

    // 1) Prefer Variable (Rule Pattern)
    Node var_pick = 0;
    for (Node cand : valid)
    {
        if (Impl::is_var(cand))
        {
            if (var_pick != 0)
            {
                var_pick = 0;
                break;
            }
            var_pick = cand;
        }
    }
    if (var_pick != 0) return selfref_repair(var_pick);

    // 2) Prefer Atomic (Non-Hash)
    Node atom_pick = 0;
    for (Node cand : valid)
    {
        if (!Impl::is_hash(cand))
        {
            if (atom_pick != 0)
            {
                atom_pick = 0;
                break;
            }
            atom_pick = cand;
        }
    }
    if (atom_pick != 0) return selfref_repair(atom_pick);

    // 3) Prefer Cons Cell (List/Number)
    Node cons_pick = 0;
    for (Node cand : valid)
    {
        if (Impl::is_hash(cand) && parse_relation(cand) == core.Cons)
        {
            if (cons_pick != 0)
            {
                cons_pick = 0;
                break;
            }
            cons_pick = cand;
        }
    }
    if (cons_pick != 0) return selfref_repair(cons_pick);

    return 0; // Still ambiguous
}

Node Zelph::parse_relation(const Node rule) const
{
    Node relation = 0; // 0 means failure
    Node subject  = 0;

    // Memo prefilter: is_correct() implies is_known(), i.e. membership in
    // relation_type_set(). One O(1) set probe rejects every non-relation
    // neighbor (subjects, objects' backlinks, parent facts -- typically
    // all but one) WITHOUT building the {->} probe set, hashing it and
    // walking edges. Members still run the exact original probe, which
    // additionally checks the declaration's probability (is_correct).
    const auto rel_types = relation_type_set();

    for (Node nd : _pImpl->get_right(rule))
    {
        if (rel_types->count(nd) == 0) continue;
        if (check_fact(nd, core.IsA, {core.RelationTypeCategory}).is_correct())
        {
            if (_pImpl->get_right(nd).count(rule) == 1) // In case nd is the subject of the rule, it may be also a relation, but not the one of the current rule. So exclude it by checking for bidirectional connection.
                subject = nd;                           // The rule has a subject that is a relation. We don't know yet if it is a rule that has same subject and predicate.
            else if (relation)
                return 0; // there may be only 1 relation
            else
                relation = nd;
        }
    }

    if (relation == 0)
    {
        // Since we exclude setting relation to the subject of the rule, now that we have a rule without a relation, it must be a rule where subject and relation are identical.
        relation = subject;
    }

    return relation;
}

Network::ReadScope Zelph::read_scope() const
{
    // Impl -> Network conversion requires the complete Impl type, which
    // only this translation unit has (zelph_impl.hpp is included ONLY
    // here). Never construct a ReadScope in another header.
    return Network::ReadScope(*_pImpl);
}

namespace
{
    // The loop of both forms of Zelph::collect_anchored_facts, which vary
    // solely in what they collect into.
    template <typename Keep>
    std::size_t anchored_facts(const Network::ReadScope& scope, const Node anchor, const Node relation, const Zelph::AnchorRole role, const Node after, const Keep& keep)
    {
        const adjacency_set& adjacent = scope.right(anchor);

        for (const Node fact : adjacent)
        {
            if (fact <= after) continue;
            const adjacency_set& outgoing = scope.right(fact);
            if (outgoing.count(relation) == 0) continue;                                     // not this predicate
            if (role == Zelph::AnchorRole::Subject && outgoing.count(anchor) == 0) continue; // the anchor serves as nothing more than an object
            if (role == Zelph::AnchorRole::Object && relation != anchor && outgoing.count(anchor) != 0
                && fact != Network::create_hash(relation, anchor, anchor))
                continue; // the anchor serves as nothing but the subject
            // relation -> fact makes the relation the fact's SUBJECT -- unless
            // the fact's whole outgoing adjacency is that one node, which is
            // how {subject, predicate} collapses when the two are the same.
            // See get_facts_of_predicate, which applies the same test from the
            // other end.
            if (scope.left(fact).count(relation) != 0 && scope.right(fact).size() > 1 && !scope.subject_is_predicate(fact, relation)) continue;
            keep(fact);
        }
        return adjacent.size();
    }
}

// Anchored-candidate filter for Unification::increment_fact_index: from the
// outgoing edges of `anchor`, collect the facts that use `relation` as their
// PREDICATE. Same role test as get_facts_of_predicate, from the other end --
// there the starting set is everything pointing at the relation, here it is
// one anchor's adjacency.
//
// All checks under ONE shared lock pair on references; the implementation
// before the ReadScope existed copied the anchor's full adjacency and paid
// two locked edge probes per candidate. This used to live in Network, which
// is the wrong layer: reading an edge pair as subject-versus-predicate is
// knowledge about zelph's fact topology, and Network only stores edges.
//
// The adjacency of the anchor holds the facts for which it serves as subject
// and those for which it serves as object. AnchorRole::Subject keeps the
// former: a fact points back at its subject (and at its predicate, and at a
// fact of which it is the subject, which are also kept), but not at its
// objects. AnchorRole::Object keeps a fact that points back at the anchor only
// where it is (anchor relation anchor): a fact includes its subject among its
// objects only as its one object (Zelph::fact refuses any other), and the node
// of a fact is the hash of its triple. Its edges cannot tell: the other
// objects of a fact point at it without a reciprocal edge, and so does each
// fact that uses it as a predicate. A fact where the anchor is the predicate
// also points back at it, and when the anchor is the relation itself, every
// fact is kept.
//
// `after` retains solely the facts whose identifiers are greater: a
// resumption of enumeration following a specific fact
// (Unification::start_after) considers only the remaining candidates.
//
// Returns the count of adjacency entries it processed, encompassing all
// associated with the anchor: a hub costs its complete adjacency even
// where the predicate selects nothing.
std::size_t Zelph::collect_anchored_facts(const Node anchor, const Node relation, adjacency_set& out, const AnchorRole role, const Node after) const
{
    out.clear();
    return anchored_facts(read_scope(), anchor, relation, role, after, [&](const Node fact)
                          { out.insert(fact); });
}

std::size_t Zelph::collect_anchored_facts(const Node anchor, const Node relation, std::vector<Node>& out, const AnchorRole role, const Node after) const
{
    out.clear();
    return anchored_facts(read_scope(), anchor, relation, role, after, [&](const Node fact)
                          { out.push_back(fact); });
}

// Semantic caveat, deliberate: parse_relation's exact probe uses
// is_correct(), which additionally rejects declarations with
// probability < 0.5 -- a state nothing produces. get_fact_structures'
// own predicate detection has always used is_known semantics (the memo),
// so within fact-structure reconstruction membership and the exact probe
// are exactly equivalent.
Node Zelph::parse_relation_scoped(const Network::ReadScope&                 scope,
                                  const ankerl::unordered_dense::set<Node>& rel_types,
                                  const Node                                rule,
                                  const adjacency_set*                      outgoing) const
{
    // An ATOM decomposes into nothing: a fact node's id IS the hash of its
    // triple, an atom's id is a counter, and a variable is neither. Two bit
    // tests, and they save the adjacency lookup below -- which on a graph
    // that does not fit in RAM is a random page touch.
    //
    // The other two readers of a node's structure guard exactly like this
    // (predicate_of, and get_fact_structures, whose comment records the atom
    // share as ~17M cache probes per Jacobian phase). This one did not, and
    // it is the hot path of a bulk removal: every doomed FACT offers its
    // subject, its predicate and its objects as candidates, and all of those
    // are atoms. Three quarters of the calls asked memory a question that
    // the node id already answers.
    if (!is_hash(rule) || is_var(rule)) return 0;

    Node relation = 0; // 0 means failure
    Node subject  = 0;

    if (outgoing == nullptr) outgoing = scope.try_right(rule);
    if (outgoing == nullptr) return 0;

    for (const Node nd : *outgoing)
    {
        if (rel_types.count(nd) == 0) continue;

        if (scope.right(nd).count(rule) == 1) // nd is the rule's subject (bidirectional); it may be a relation type, but not THIS rule's relation
            subject = nd;
        else if (relation)
            return 0; // there may be only 1 relation
        else
            relation = nd;
    }

    if (relation == 0)
    {
        // No relation besides the subject: subject and relation are identical.
        relation = subject;
    }

    return relation;
}

Node Zelph::count() const
{
    return _pImpl->count();
}

bool Zelph::holds_only_core() const
{
    if (count() != _pImpl->_core_graph.size()) return false;
    for (const Node n : _pImpl->_core_graph)
        if (!_pImpl->exists(n)) return false;

    {
        std::shared_lock lock_node(_pImpl->_mtx_node_of_name);
        std::shared_lock lock_name(_pImpl->_mtx_name_of_node);
        for (const auto& [language, names] : _pImpl->_node_of_name)
            if (!names.empty()) return false;
        for (const auto& [language, nodes] : _pImpl->_name_of_node)
            if (!nodes.empty()) return false;
    }

    std::shared_lock lock(_pImpl->_mtx_weights);
    return _pImpl->_weights.empty();
}

Zelph::AllNodeView Zelph::get_all_nodes_view() const
{
    return AllNodeView(_pImpl->_left);
}

Zelph::LangNodeView Zelph::get_lang_nodes_view(const std::string& lang) const
{
    std::unique_lock lock(_pImpl->_mtx_node_of_name);
    auto             it = _pImpl->_node_of_name.find(lang);
    if (it == _pImpl->_node_of_name.end())
    {
        static const Impl::node_of_name_map empty;
        return LangNodeView(empty);
    }
    return LangNodeView(it->second);
}

// Extracts the components (subject, predicate, objects) from a relation node.
Zelph::FactComponents Zelph::extract_fact_components(Node relation) const
{
    FactComponents components;
    auto           left  = get_left(relation);
    auto           right = get_right(relation);

    // Find subject: The node present in both left and right (bidirectional connection)
    for (Node candidate : right)
    {
        if (left.count(candidate) == 1)
        {
            components.subject = candidate;
            break;
        }
    }

    if (components.subject == 0)
    {
        // No subject found (possibly corrupted data)
        return components;
    }

    // Find predicate: In right, but not the subject
    for (Node candidate : right)
    {
        if (candidate != components.subject)
        {
            components.predicate = candidate;
            break;
        }
    }

    // Find objects: In left, but not the subject
    for (Node candidate : left)
    {
        if (candidate != components.subject)
        {
            components.objects.insert(candidate);
        }
    }

    return components;
}

void Zelph::set_output_handler(io::OutputHandler output) const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    _pImpl->_output = std::move(output);
}

zelph::io::OutputHandler Zelph::get_output_handler() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return _pImpl->_output;
}

void Zelph::emit(io::OutputChannel channel, const std::string& text, bool newline, bool finding) const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    _pImpl->emit(channel, text, newline, finding);
}

void Zelph::out(const std::string& msg, bool newline) const
{
    emit(io::OutputChannel::Out, msg, newline);
}

void Zelph::error(const std::string& msg, bool newline) const
{
    emit(io::OutputChannel::Error, msg, newline);
}

void Zelph::diagnostic(const std::string& msg, bool newline) const
{
    emit(io::OutputChannel::Diagnostic, msg, newline);
}

void Zelph::out_finding(const std::string& msg, bool newline) const
{
    emit(io::OutputChannel::Out, msg, newline, true);
}

void Zelph::diagnostic_finding(const std::string& msg, bool newline) const
{
    emit(io::OutputChannel::Diagnostic, msg, newline, true);
}

void Zelph::prompt(const std::string& msg, bool newline) const
{
    emit(io::OutputChannel::Prompt, msg, newline);
}

// Shared implementation of the four *_stream() accessors.
//
// OutputStream carries a COPY of the handler and flushes on endl /
// destruction WITHOUT holding any lock -- unlike emit(), which
// serializes every handler call through _mtx_print. Pool workers log
// via diagnostic_stream() (u_log, Zelph::log), so two workers could
// invoke the handler concurrently: a data race on any stateful
// handler, observed as double-free crashes of OutputCollector's
// event vector once a logged test exercised the parallel scan path.
// Wrapping the handler so that the flush itself takes _mtx_print
// restores the emit() guarantee for all stream users. _mtx_print is
// recursive, so handlers that re-enter zelph output remain safe.
zelph::io::OutputStream Zelph::locked_stream(zelph::io::OutputChannel channel) const
{
    zelph::io::OutputHandler handler;
    {
        std::lock_guard lock(_pImpl->_mtx_print);
        handler = _pImpl->_output;
    }
    zelph::network::Zelph::Impl* impl = _pImpl;

    return {
        [impl, handler](const zelph::io::OutputEvent& event)
        {
            std::lock_guard lock(impl->_mtx_print);
            if (handler) handler(event);
        },
        channel,
        false};
}

zelph::io::OutputStream Zelph::out_stream() const
{
    return locked_stream(io::OutputChannel::Out);
}

zelph::io::OutputStream Zelph::diagnostic_stream() const
{
    return locked_stream(io::OutputChannel::Diagnostic);
}

zelph::io::OutputStream Zelph::error_stream() const
{
    return locked_stream(io::OutputChannel::Error);
}

zelph::io::OutputStream Zelph::prompt_stream() const
{
    return locked_stream(io::OutputChannel::Prompt);
}

void Zelph::set_logging(int max_depth) const
{
    _pImpl->_logging       = max_depth != 0;
    _pImpl->_max_log_depth = max_depth;
    out_stream() << (_pImpl->_logging ? "Logging enabled with max depth " : "Logging disabled. ") << max_depth << std::endl;
}

bool Zelph::should_log(int depth) const
{
    if (!_pImpl->_logging || depth > _pImpl->_max_log_depth) return false;

    // Never emit logs within a rendering. Log messages are
    // constructed using format(), which runs node_to_string, which in
    // turn accesses get_fact_structures -- and that operation triggers
    // logging. Without this guard, the pair would engage in unbounded
    // recursion (log -> format -> log), leading to stack overflow. A
    // log entry describing the node currently being rendered would, in
    // any case, be self-referential noise. Checked last to ensure the
    // typical scenario -- logging being disabled -- costs exactly what
    // it did before.
    return !string::is_inside_node_to_wstring();
}

bool Zelph::logging_active() const
{
    return _pImpl->_logging;
}

void Zelph::log(int depth, const std::string& category, const std::string& message) const
{
    if (!should_log(depth)) return;
    std::string indent(depth * 2, ' ');
    out_stream() << indent << "[depth " << depth << ", " << category << "] " << message << std::endl;
}
