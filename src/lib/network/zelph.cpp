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
#include "io/mermaid.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"
#include "zelph_version.hpp"

#include <bitset>
#include <cassert>
#include <ranges>

using std::ranges::all_of;

using namespace zelph::network;

namespace
{
    // Relation entries a direct (index-free) closure traversal may scan
    // before switching to the predicate index. Small closures on small
    // graphs stay index-free and fast; hub-heavy traversals on Wikidata
    // exhaust the budget immediately and pay the (cached) index build once.
    constexpr size_t kDirectClosureScanBudget = size_t(1) << 16;

    adjacency_set bfs_over_index(const PredicateIndex::adjacency& adj, const Node start, const bool include_start)
    {
        adjacency_set                      result;
        ankerl::unordered_dense::set<Node> seen;
        std::vector<Node>                  frontier{start};

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

    // Reconstruction-based reference walk -- the pre-flag implementation
    // of unification.cpp's contains_variable_deep, kept verbatim so the
    // fallback after binary loads / trusted imports / removals is exactly
    // the historical semantics. Depth is fixed at 1 (logging only).
    bool var_in_closure_walk(const Zelph* n, const Node nd, std::unordered_set<Node>& visited)
    {
        if (nd == 0) return false;
        if (Zelph::is_var(nd)) return true;
        if (!Zelph::is_hash(nd)) return false;        // plain atom -> no internal structure
        if (!visited.insert(nd).second) return false; // cycle protection

        const auto structs = get_fact_structures(n, nd, 1);
        for (const auto& fs : *structs)
        {
            if (var_in_closure_walk(n, fs.subject, visited)) return true;
            if (var_in_closure_walk(n, fs.predicate, visited)) return true;
            for (const Node o : fs.objects)
                if (var_in_closure_walk(n, o, visited)) return true;
        }
        return false;
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
    fact(core.IsA, core.IsA, {core.RelationTypeCategory});
    fact(core.Unequal, core.IsA, {core.RelationTypeCategory});
    fact(core.Causes, core.IsA, {core.RelationTypeCategory});
    fact(core.Cons, core.IsA, {core.RelationTypeCategory});
    fact(core.PartOf, core.IsA, {core.RelationTypeCategory});
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
    }
}

Node Zelph::node(const std::string& name, std::string lang)
{
    if (lang.empty()) lang = _lang;
    if (name.empty())
    {
        throw std::invalid_argument("Zelph::node(): name cannot be empty");
    }

    // 1. Fast path: shared lock for lookup
    {
        std::shared_lock lock_node(_pImpl->_mtx_node_of_name);

        // Check existing regular nodes
        auto lang_it = _pImpl->_node_of_name.find(lang);
        if (lang_it != _pImpl->_node_of_name.end())
        {
            auto it = lang_it->second.find(name);
            if (it != lang_it->second.end())
            {
                return it->second;
            }
        }

        // Check core nodes
        auto it_core = _core_names_by_name.find(name);
        if (it_core != _core_names_by_name.end())
        {
            return it_core->second;
        }
    }

    // 2. Slow path: exclusive lock for creation (double-checked)
    std::unique_lock lock_node(_pImpl->_mtx_node_of_name);

    // Re-check: another thread may have created it while we re-acquired the lock
    {
        auto lang_it = _pImpl->_node_of_name.find(lang);
        if (lang_it != _pImpl->_node_of_name.end())
        {
            auto it = lang_it->second.find(name);
            if (it != lang_it->second.end())
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
    Node new_node = _pImpl->create();

    std::unique_lock lock_name(_pImpl->_mtx_name_of_node);

    auto [reverse_outer_it, inserted_reverse_outer] = _pImpl->_node_of_name.try_emplace(lang);
    auto [forward_outer_it, inserted_forward_outer] = _pImpl->_name_of_node.try_emplace(lang);
    (void)inserted_reverse_outer;
    (void)inserted_forward_outer;

    std::string_view sv = _pImpl->_string_pool.intern(name);

    reverse_outer_it->second.emplace(sv, new_node);
    forward_outer_it->second.emplace(new_node, sv);

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

    for (const Node rel : get_right(subject))
    {
        // Validate: predicate in right(rel), subject bidirectional (in left and right).
        if (has_right_edge(rel, predicate) && has_right_edge(rel, subject) && has_left_edge(rel, subject))
        {
            for (const Node obj : get_left(rel))
            {
                // Objects: in left(rel) but NOT in right(rel) (unidirectional).
                if (obj != subject && !is_var(obj) && !has_right_edge(rel, obj))
                {
                    objects.insert(obj);
                }
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

    for (const Node rel : get_right(object))
    {
        if (has_right_edge(rel, predicate) && has_left_edge(rel, object) && !has_right_edge(rel, object))
        {
            for (const Node subj : get_left(rel))
            {
                // Subjects: bidirectional (in both left and right of rel).
                if (subj != object && subj != predicate && !is_var(subj) && has_right_edge(rel, subj))
                {
                    subjects.insert(subj);
                }
            }
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
adjacency_set Zelph::transitive_targets(const Node start, const Node predicate, const bool include_start) const
{
    adjacency_set result;
    if (_pImpl->try_transitive_direct(start, predicate, include_start, /*forward*/ true, kDirectClosureScanBudget, result))
        return result;

    result.clear();
    const auto idx = _pImpl->predicate_index(predicate);
    return bfs_over_index(idx->forward, start, include_start);
}

// Transitive closure following the predicate backward (object -> subject).
adjacency_set Zelph::transitive_sources(const Node target, const Node predicate, const bool include_target) const
{
    adjacency_set result;
    if (_pImpl->try_transitive_direct(target, predicate, include_target, /*forward*/ false, kDirectClosureScanBudget, result))
        return result;

    result.clear();
    const auto idx = _pImpl->predicate_index(predicate);
    return bfs_over_index(idx->backward, target, include_target);
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
        adjacency_set possible_relations = _pImpl->get_right(nd);
        for (Node relation : filter(possible_relations, relationType))
        {
            if (_pImpl->get_left(relation).count(target) == 1
                && left_nodes.count(nd) == 0) // exclude the subject of the fact, since it is connected bidirectional. If <subject relationType target> is true, the subject would be included in the result by mistake
            {
                result.insert(nd);
            }
        }
    }

    return result;
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

Answer Zelph::check_fact(const Node subject, const Node predicate, const adjacency_set& objects) const
{
    const Node relation = Impl::create_hash(predicate, subject, objects);

    const bool known = _pImpl->fact_edges_hold(relation, subject, objects);

    if (!known
        && !Impl::is_var(subject)
        && !Impl::is_var(predicate)
        && std::all_of(objects.begin(), objects.end(), [](const Node t)
                       { return Impl::is_var(t); })
        && !string::is_inside_node_to_wstring()
        && _pImpl->exists(relation))
    {
        // Suspected hash collision / corrupt state: this cold diagnostic
        // path fetches its own adjacency copies -- the hot path above no
        // longer materializes any.
        const adjacency_set connectedFromRelation = _pImpl->get_right(relation);
        const adjacency_set connectedToRelation   = _pImpl->get_left(relation);

        const bool relationConnectsToSubject = connectedFromRelation.count(subject) == 1;

        const bool subjectConnectsToRelation         = connectedToRelation.count(subject) == 1;
        const bool allObjectsConnectToRelation       = std::all_of(objects.begin(), objects.end(), [&](Node t)
                                                                   { return connectedToRelation.count(t) != 0; });
        const bool noObjectsAreConnectedFromRelation = std::all_of(objects.begin(), objects.end(), [&](Node t)
                                                                   { return connectedFromRelation.count(t) == 1; });

        // inconsistent state => debug output TODO
        std::string output;
        string::node_to_string(this, output, _lang, relation, 3);
        error(output, true);

        io::gen_mermaid_html(this,
                             relation,
                             "debug.html",
                             1,
                             3,
                             {},
                             true,
                             true,
                             true);
        error("relationConnectsToSubject         == " + std::to_string(relationConnectsToSubject), true);
        error("subjectConnectsToRelation         == " + std::to_string(subjectConnectsToRelation), true);
        error("allObjectsConnectToRelation       == " + std::to_string(allObjectsConnectToRelation), true);
        error("noObjectsAreConnectedFromRelation == " + std::to_string(noObjectsAreConnectedFromRelation), true);

        FactComponents actual = extract_fact_components(relation);
        error("Hash collision detected for relation=" + std::to_string(relation), true);
        error("Expected inputs to create_hash:", true);
        error("  Subject:   " + std::to_string(subject) + " (hex: 0x" + string::to_hex(subject) + ", bin: " + std::bitset<64>(subject).to_string() + ")", true);
        error("  Predicate: " + std::to_string(predicate) + " (hex: 0x" + string::to_hex(predicate) + ", bin: " + std::bitset<64>(predicate).to_string() + ")", true);
        error("  Objects:", true);
        for (Node obj : objects)
        {
            error("    " + std::to_string(obj) + " (hex: 0x" + string::to_hex(obj) + ", bin: " + std::bitset<64>(obj).to_string() + ")", true);
        }

        error("Actual inputs in existing relation:", true);
        error("  Subject:   " + std::to_string(actual.subject) + " (hex: 0x" + string::to_hex(actual.subject) + ", bin: " + std::bitset<64>(actual.subject).to_string() + ")", true);
        error("  Predicate: " + std::to_string(actual.predicate) + " (hex: 0x" + string::to_hex(actual.predicate) + ", bin: " + std::bitset<64>(actual.predicate).to_string() + ")", true);
        error("  Objects:", true);
        for (Node obj : actual.objects)
        {
            error("    " + std::to_string(obj) + " (hex: 0x" + string::to_hex(obj) + ", bin: " + std::bitset<64>(obj).to_string() + ")", true);
        }

        static int hash_collision_count = 0;
        ++hash_collision_count;
        error("Hash collision count: " + std::to_string(hash_collision_count), true);

        assert(false);
    }

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

    // No store entry: either a subject == predicate fact (never stored), or
    // the stores were disarmed by a bulk path. parse_relation resolves both;
    // for subject == predicate it returns the subject, which IS the predicate.
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

Node Zelph::fact(const Node subject, const Node predicate, const adjacency_set& objects, const long double probability)
{
    const Answer answer = check_fact(subject, predicate, objects);

    if (answer.is_known())
    {
        if (answer.is_wrong() && probability > 0.5L)
        {
            throw std::runtime_error("fact(): this fact is known to be wrong");
        }
        else if (answer.is_correct() && probability < 0.5L)
        {
            throw std::runtime_error("fact(): this fact is known to be true");
        }
    }
    else
    {
        if (objects.count(predicate) == 1)
        {
            // 1 13 13
            // ~ is for example is for example <= (~  is opposite of  is for example), (is for example  ~  ->)
            throw std::runtime_error("fact(): facts with same relation type and object are not supported.");
        }

        if (predicate != core.IsA && (!Impl::is_hash(predicate) || Network::is_var(predicate))) // note that the initial constructor call fact(core.IsA, core.IsA, core.RelationTypeCategory) is executed as intended
        {
            fact(predicate, core.IsA, {core.RelationTypeCategory});
        }

        if (_pImpl->exists(answer.relation()))
        {
            // check_fact returns !answer.is_known() though answer.relation exists, which must not happen. Indicates corrupt database or hash collision.
            assert(false);
        }
        else
        {
            _pImpl->create(answer.relation());
        }

        _pImpl->connect(subject, answer.relation());
        _pImpl->connect(answer.relation(), subject);
        for (const Node t : objects)
        {
            if (t == subject)
            {
                if (objects.size() > 1)
                {
                    // We only allow relations with the same subject and object in the case of a single object. If there are several
                    // objects and one of them is identical to the subject, we wouldn't know that such an object exists.
                    // Real life examples from Wikidata:
                    // South Africa (Q258)  country (P17)  South Africa (Q258)
                    // or
                    // chemical substance  has part  chemical substance ⇐ (matter  has part  chemical substance), (chemical substance  is subclass of  matter)

                    const std::string name_subject_object = get_name(subject, _lang, true);
                    const std::string name_relationType   = get_name(predicate, _lang, true);

                    throw std::runtime_error("fact(): facts with same subject and object are only supported for facts with a single object: " + name_subject_object + " " + name_relationType + " " + name_subject_object);
                }
            }
            else
            {
                _pImpl->connect(t, answer.relation());
            }
        }

        _pImpl->connect(answer.relation(), predicate, probability);

        // Per-node invalidation AFTER the edges are drawn: a reader that
        // cached a partial view of the half-constructed node between the
        // connects is invalidated here -- the former up-front wholesale
        // clear left that window open. See invalidate_fact_structures_for.
        invalidate_fact_structures_for(subject, predicate, objects, answer.relation());

        // Maintain the template-variable store from the ACTUAL triple
        // (see Impl's _template_vars). Data facts -- the overwhelming
        // majority -- cost three store misses and allocate nothing.
        if (_pImpl->_template_vars_authoritative.load(std::memory_order_acquire))
        {
            std::shared_ptr<std::unordered_set<Node>> vars; // lazily allocated

            const auto add_component = [&](const Node c)
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
            };

            add_component(subject);
            add_component(predicate);
            for (const Node t : objects)
                add_component(t);

            if (vars)
            {
                std::unique_lock lock(_pImpl->_template_vars_mtx);
                _pImpl->_template_vars.emplace(answer.relation(),
                                               std::shared_ptr<const std::unordered_set<Node>>(std::move(vars)));
            }
        }

        // Record the genuine structure (reconstruction bypass): the exact
        // triple, known right here and immutable forever -- the node ID is
        // its hash. subject == predicate is deliberately excluded: the
        // reconstruction walk yields EMPTY for those (the subject loop
        // skips s == p), and unification's atom treatment of them must not
        // change. Self-facts arrive with objects == {subject}, matching
        // the walk's self-referential repair exactly.
        if (subject != predicate
            && _pImpl->_genuine_authoritative.load(std::memory_order_acquire))
        {
            auto           list = std::make_shared<FactStructureList>(1);
            FactStructure& fs   = list->front();
            fs.subject          = subject;
            fs.predicate        = predicate;
            fs.objects          = objects;

            std::unique_lock lock(_pImpl->_genuine_mtx);
            _pImpl->_genuine.emplace(answer.relation(), FactStructurePtr(std::move(list)));
        }

        if (_on_fact_created) _on_fact_created(answer.relation(), predicate);
    }

    return answer.relation();
}

Node Zelph::fact_import_trusted_single_object(Node subject, Node predicate, Node object) const
{
    invalidate_fact_structures_cache();
    return _pImpl->insert_fact_single_object_trusted(subject, predicate, object);
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

void Zelph::set_number_digits(const std::vector<Node>& digits_ascending)
{
    std::shared_ptr<const std::unordered_map<Node, uint32_t>> table;

    if (!digits_ascending.empty())
    {
        if (digits_ascending.size() < 2)
            throw std::invalid_argument("set_number_digits: at least 2 digits are required (or none to disable)");

        auto map = std::make_shared<std::unordered_map<Node, uint32_t>>();
        for (size_t i = 0; i < digits_ascending.size(); ++i)
            (*map)[digits_ascending[i]] = static_cast<uint32_t>(i);

        if (map->size() != digits_ascending.size())
            throw std::invalid_argument("set_number_digits: duplicate digit nodes");

        table = std::move(map);
    }

    std::unique_lock lock(_smtx_number_digits);
    _number_digits = std::move(table);
}

std::shared_ptr<const std::unordered_map<Node, uint32_t>> Zelph::number_digit_values() const
{
    std::shared_lock lock(_smtx_number_digits);
    return _number_digits;
}

std::shared_ptr<const DisplayTables> Zelph::display_tables() const
{
    std::shared_lock lock(_smtx_display_tables);
    return _display_tables;
}

bool Zelph::find_display_scheme(const std::string& name, std::size_t& index) const
{
    const auto tables = display_tables();
    if (!tables) return false;

    for (std::size_t i = 0; i < tables->schemes.size(); ++i)
    {
        if (tables->schemes[i].name == name)
        {
            index = i;
            return true;
        }
    }
    return false;
}

std::size_t Zelph::register_display_scheme(const DisplayScheme& scheme)
{
    if (scheme.name.empty())
        throw std::invalid_argument("register_display_scheme(): the scheme name must not be empty");

    std::unique_lock lock(_smtx_display_tables);

    // Copy-on-write: readers hold an immutable snapshot for the duration of
    // a rendering and are never disturbed by a concurrent registration.
    auto tables = _display_tables
                    ? std::make_shared<DisplayTables>(*_display_tables)
                    : std::make_shared<DisplayTables>();

    for (std::size_t i = 0; i < tables->schemes.size(); ++i)
    {
        if (tables->schemes[i].name == scheme.name)
        {
            // Update in place: operator entries reference schemes by index.
            tables->schemes[i] = scheme;
            _display_tables    = tables;
            return i;
        }
    }

    tables->schemes.push_back(scheme);
    _display_tables = tables;
    return tables->schemes.size() - 1;
}

void Zelph::set_infix_display(const std::size_t scheme, const std::vector<InfixEntry>& operators)
{
    std::vector<Node> preds;
    preds.reserve(operators.size());

    {
        std::unique_lock lock(_smtx_display_tables);

        if (!_display_tables || scheme >= _display_tables->schemes.size())
            throw std::invalid_argument("set_infix_display(): unknown display scheme");

        auto tables = std::make_shared<DisplayTables>(*_display_tables);

        for (const InfixEntry& op : operators)
        {
            if (op.predicate == 0)
                throw std::invalid_argument("set_infix_display(): predicate must not be 0");

            const auto it = tables->operators.find(op.predicate);
            if (it != tables->operators.end())
            {
                throw std::runtime_error("set_infix_display(): predicate '"
                                         + get_name(op.predicate, _lang, true)
                                         + "' is already registered in display scheme '"
                                         + tables->schemes[it->second.scheme].name + "'");
            }

            tables->operators.emplace(op.predicate, InfixOperator{scheme, op.precedence, op.assoc});
            preds.push_back(op.predicate);
        }

        _display_tables = tables;
    }

    // See the header: the self-fact sugar would render (X op X) as ":op X",
    // which the scheme's parser cannot read back.
    add_verbose_selffact_predicates(preds);
}

// Register predicates whose self-facts must always render in the verbose
// "S P S" form instead of the ":pred S" display sugar. Deliberately
// ADDITIVE across calls (unlike the replace-the-set semantics of
// set_number_digits): modules stack (arithmetic -> symbolic-core -> eml),
// and a later module must not clobber an earlier module's registrations.
// Display-only session state, cleared by .reset and not persisted --
// like the digit alphabet, the graph topology is unaffected.
void Zelph::add_verbose_selffact_predicates(const std::vector<Node>& preds)
{
    std::unique_lock lock(_smtx_verbose_selffact_preds);
    _verbose_selffact_preds.insert(preds.begin(), preds.end());
}

// True if self-facts on this predicate must not use the ":pred S" display
// sugar. Queried by node_to_string for every self-fact candidate; the
// shared lock keeps concurrent formatting cheap.
bool Zelph::selffact_sugar_suppressed(const Node pred) const
{
    std::shared_lock lock(_smtx_verbose_selffact_preds);
    return _verbose_selffact_preds.contains(pred);
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
 * Each string is first converted to a Node via node(element), then the general
 * Node-based sequence() overload is called. This centralizes the cons-building logic
 * and guarantees both overloads produce exactly the same Lisp-style structure.
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
Node Zelph::list(const std::vector<std::string>& elements)
{
    if (elements.empty()) return core.Nil;

    std::vector<Node> node_elements;
    node_elements.reserve(elements.size());

    for (const auto& element : elements)
    {
        node_elements.emplace_back(node(element));
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
Node Zelph::set(const std::unordered_set<Node>& elements)
{
    if (elements.empty()) return core.Nil;

    // Create the super-node representing the set itself
    Node set_node = _pImpl->create();

    for (const auto& current_node : elements)
    {
        // Link to the set container
        fact(current_node, core.PartOf, {set_node});
    }

    return set_node;
}

Node Zelph::parse_fact(Node rule, adjacency_set& deductions, Node parent) const
{
    deductions.clear();
    adjacency_set candidates;

    for (Node nd : _pImpl->get_left(rule))
    {
        // Check for bidirectional link (characteristic of Subject <-> Relation connection)
        if (_pImpl->get_left(nd).count(rule) == 1)
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

    for (Node cand : candidates)
    {
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

void Zelph::collect_anchored_facts(const Node anchor, const Node relation, adjacency_set& out) const
{
    _pImpl->collect_anchored_facts(anchor, relation, out);
}

// Semantic caveat, deliberate: parse_relation's exact probe uses
// is_correct(), which additionally rejects declarations with
// probability < 0.5 -- a state nothing produces. get_fact_structures'
// own predicate detection has always used is_known semantics (the memo),
// so within fact-structure reconstruction membership and the exact probe
// are exactly equivalent.
Node Zelph::parse_relation_scoped(const Network::ReadScope&                 scope,
                                  const ankerl::unordered_dense::set<Node>& rel_types,
                                  const Node                                rule) const
{
    Node relation = 0; // 0 means failure
    Node subject  = 0;

    for (const Node nd : scope.right(rule))
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
// Correctness argument. A fact node's ID IS create_hash(predicate,
// subject, objects), so each node has exactly ONE genuine triple, fixed
// at creation; monotone graph growth can only ADD reconstruction
// candidates (every skip heuristic flips only towards skipping less),
// and hash verification prunes any ambiguous candidate set back to the
// genuine reading. What growth can actually change is therefore:
//  (1) the new relation node itself and its components (their adjacency
//      grew, changing candidate collection),
//  (2) nodes whose child-fact heuristic inspects the components'
//      neighborhoods -- covered by one BIDIRECTIONAL adjacency level
//      around subject and objects; deeper levels (the heuristic reads up
//      to three hops) only feed checks whose outcome hash verification
//      makes result-neutral,
//  (3) globally: relation-type declarations (P ~ ->). Predicate detection
//      consults check_fact(p, IsA, RelationTypeCategory) per right
//      neighbor, so a new declaration can change ANY cached entry -- that
//      case falls back to the full clear (rare: module load time only).
// Residual risk, consciously accepted: entries kept UNVERIFIED (no
// candidate hash-verifies, e.g. subject==predicate facts) are not
// re-checked on deeper-level growth. The suite-wide `.semi-naive check`
// equivalence net backstops this.
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
    std::unique_lock lock(_pImpl->_rel_types_mtx);
    _pImpl->_rel_types.reset();
    ++_pImpl->_rel_types_gen;
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

bool Zelph::var_in_closure(const Node nd) const
{
    if (nd == 0) return false;
    if (Impl::is_var(nd)) return true;
    if (!Impl::is_hash(nd)) return false;

    if (_pImpl->_template_vars_authoritative.load(std::memory_order_acquire))
    {
        if (logging_active()) _var_flag_queries.fetch_add(1, std::memory_order_relaxed);
        std::shared_lock lock(_pImpl->_template_vars_mtx);
        return _pImpl->_template_vars.find(nd) != _pImpl->_template_vars.end();
    }

    if (logging_active()) _var_flag_fallbacks.fetch_add(1, std::memory_order_relaxed);
    std::unordered_set<Node> visited;
    return var_in_closure_walk(this, nd, visited);
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

    if (logging_active()) _genuine_hits.fetch_add(1, std::memory_order_relaxed);
    out = it->second; // shared_ptr copy: one atomic increment, no allocation
    return true;
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
    }

    _pImpl->_genuine_authoritative.store(false, std::memory_order_release);
    {
        std::unique_lock lock(_pImpl->_genuine_mtx);
        _pImpl->_genuine.clear();
    }
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

void Zelph::emit(io::OutputChannel channel, const std::string& text, bool newline) const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    _pImpl->emit(channel, text, newline);
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
    return _pImpl->_logging && depth <= _pImpl->_max_log_depth;
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
