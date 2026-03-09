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
#include "io/mermaid.hpp"
#include "string/node_to_string.hpp"
#include "string/string_utils.hpp"
#include "zelph_impl.hpp"

#include <bitset>
#include <cassert>
#include <ranges>

using std::ranges::all_of;

using namespace zelph::network;

std::string Zelph::get_version()
{
    return "0.9.4";
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

    // 1. Check existing regular nodes
    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    {
        auto& node_of_name = _pImpl->_node_of_name[lang];
        auto  it           = node_of_name.find(name);
        if (it != node_of_name.end())
        {
            return it->second;
        }
    }

    // 2. Check core nodes
    {
        auto it = _core_names_by_name.find(name);
        if (it != _core_names_by_name.end())
        {
            return it->second;
        }
    }

    // we do not call invalidate_fact_structures_cache() here, because creating a node is isolated from the network

    // 3. Create new node
    Node            new_node = _pImpl->create();
    std::lock_guard lock2(_pImpl->_mtx_name_of_node);

    _pImpl->_node_of_name[lang][name]     = new_node;
    _pImpl->_name_of_node[lang][new_node] = name;

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
    bool known = false;

    Node relation = Impl::create_hash(predicate, subject, objects);

    if (_pImpl->exists(relation))
    {
        const adjacency_set& connectedFromRelation = _pImpl->get_right(relation);
        const adjacency_set& connectedToRelation   = _pImpl->get_left(relation);

        known = connectedFromRelation.count(subject) == 1
             && connectedToRelation.count(subject) == 1 // subject must be connected from and to <--> relation node (i.e. bidirectional, to distinguish it from objects)
             && std::all_of(objects.begin(), objects.end(), [&](Node t)
                            { return connectedToRelation.count(t) != 0; }) // objects must all be connected to relation
             && std::all_of(objects.begin(), objects.end(), [&](Node t)
                            { return t == subject || connectedFromRelation.count(t) == 0; }); // no object must be connected from relation node

        if (!known
            && !Impl::is_var(subject)
            && !Impl::is_var(predicate)
            && std::all_of(objects.begin(), objects.end(), [&](const Node t)
                           { return Impl::is_var(t); })
            && !string::is_inside_node_to_wstring())
        {
            const bool relationConnectsToSubject         = connectedFromRelation.count(subject) == 1;
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

        invalidate_fact_structures_cache();
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
    }

    return answer.relation();
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
 *   "create the union of all sets that contain X" directly in Zelph's reasoning engine.
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
    if (var_pick != 0) return var_pick;

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
    if (atom_pick != 0) return atom_pick;

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
    if (cons_pick != 0) return cons_pick;

    return 0; // Still ambiguous
}

Node Zelph::parse_relation(const Node rule) const
{
    Node relation = 0; // 0 means failure
    Node subject  = 0;
    for (Node nd : _pImpl->get_right(rule))
    {
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
    std::lock_guard lock(_pImpl->_mtx_node_of_name);
    auto            it = _pImpl->_node_of_name.find(lang);
    if (it == _pImpl->_node_of_name.end())
    {
        static const Impl::node_of_name_map empty;
        return LangNodeView(empty);
    }
    return LangNodeView(it->second);
}

bool Zelph::try_get_fact_structures_cached(Node fact, std::vector<FactStructure>& out) const
{
    // If cache is currently empty/known-invalid, avoid locking
    if (!_pImpl->_fs_cache_has_entries.load(std::memory_order_acquire))
        return false;

    std::shared_lock lock(_pImpl->_fs_cache_mtx);
    auto             it = _pImpl->_fs_cache.find(fact);
    if (it == _pImpl->_fs_cache.end()) return false;

    out = it->second; // copy (function returns by value anyway)
    return true;
}

void Zelph::store_fact_structures_cached(Node fact, const std::vector<FactStructure>& value) const
{
    {
        std::unique_lock lock(_pImpl->_fs_cache_mtx);
        _pImpl->_fs_cache[fact] = value;
    }
    _pImpl->_fs_cache_has_entries.store(true, std::memory_order_release);
}

void Zelph::invalidate_fact_structures_cache() const noexcept
{
    // If cache already empty, do nothing (avoid lock)
    if (!_pImpl->_fs_cache_has_entries.exchange(false, std::memory_order_acq_rel))
        return;

    std::unique_lock lock(_pImpl->_fs_cache_mtx);
    _pImpl->_fs_cache.clear();
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

zelph::io::OutputStream Zelph::out_stream() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return io::OutputStream(_pImpl->_output, io::OutputChannel::Out, false);
}

zelph::io::OutputStream Zelph::diagnostic_stream() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return io::OutputStream(_pImpl->_output, io::OutputChannel::Diagnostic, false);
}

zelph::io::OutputStream Zelph::error_stream() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return io::OutputStream(_pImpl->_output, io::OutputChannel::Error, false);
}

zelph::io::OutputStream Zelph::prompt_stream() const
{
    std::lock_guard lock(_pImpl->_mtx_print);
    return io::OutputStream(_pImpl->_output, io::OutputChannel::Prompt, false);
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
