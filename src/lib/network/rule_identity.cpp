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

#include "rule_identity.hpp"

#include "fact_structure.hpp"
#include "platform/platform_utils.hpp"
#include "zelph.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    namespace
    {
        std::atomic<uint64_t> climb_steps{0}; // refer to rule_statement_climb_steps
        std::atomic<uint64_t> reads{0};       // refer to rule_identity_reads
        std::atomic<uint64_t> steps{0};       // refer to rule_identity_steps

        // Rules are small and shallow; the cap stops a pathological structure
        // from turning into an endless walk, and a container met again along
        // the current path stops at the point it is met (canon,
        // Matcher::match). A rendering cut short at the cap merely causes more
        // rules to share a shape: two rules alike up to their variables are
        // truncated at identical points, and the matcher, which carries the
        // same cap and the same stop, decides between them.
        constexpr int max_depth = 32;

        // Sets that hold one another densely leave every container on the
        // current path once and continue expanding along every simple path
        // among them: ten sets that each hold all ten generate roughly a
        // million paths. Once a rendering encounters a container again along
        // its path, it renders at most this many further positions, and once
        // a comparison does, it compares at most this many additional pairs;
        // beyond this point, canon renders "…" as at the depth cap, and the
        // matcher finds no match. A rule without a cycle is not included in
        // the count, and the depth cap bounds its walk. Where the budget
        // truncates, two rules alike up to their variables may be truncated
        // at distinct points, since a rendering takes the members of a set
        // in the order their memberships are stored, and the two remain
        // separate.
        constexpr std::size_t walk_budget = 100000;

        // The matcher operates within nested continuations: each pair it has
        // matched stays on the stack until the whole rule has been matched,
        // approximately one kilobyte per pair in a release build, and within
        // the budget a comparison over cycles nests deeper than a thread's
        // stack typically accommodates. Once it has met a cycle, a comparison
        // halts where the thread's stack has less than the reserve remaining,
        // which accounts for what runs between two checks and the guard pages,
        // and finds no match; a rule without a cycle nests as deep as it is
        // large and is not monitored. When the platform reports no stack for
        // the thread, a comparison goes at most to the fallback below the
        // frame where it first encountered its cycle.
        constexpr std::size_t stack_reserve  = 128 * 1024;
        constexpr std::size_t stack_fallback = 128 * 1024;

        // A node that PROVABLY carries no variable needs no traversal: it
        // is hash-consed, so its ID already is its canonical identity, and
        // any tag on it is a property of that one node, seen alike by every
        // rule referring to it. Answering from the engine's own
        // template-variable store costs O(1).
        //
        // "Unknown" is the honest third answer -- the store is disarmed
        // after a bulk load -- and it must NOT be read as "none": that
        // would make every rule in a loaded graph fingerprint as a bundle
        // of opaque IDs, and .load followed by .import (the case this
        // whole mechanism exists for) would deduplicate nothing. Unknown
        // therefore falls through to the structural walk, which is more
        // work and the same answer.
        enum class Vars
        {
            None,
            Some,
            Unknown
        };

        Vars variables_in(const Zelph* const z, const Node n)
        {
            if (Zelph::is_var(n)) return Vars::Some;

            std::shared_ptr<const std::unordered_set<Node>> vars;
            if (!z->try_get_template_vars(n, vars)) return Vars::Unknown;
            return (vars && !vars->empty()) ? Vars::Some : Vars::None;
        }

        // Elements of a set node: the subjects of the PartOf facts pointing
        // at it. Same reconstruction node_to_string uses to print `{...}`.
        std::vector<Node> set_members(const Zelph* const z, const Node n)
        {
            std::vector<Node> members;
            for (const Node rel : z->get_right(n))
            {
                if (z->parse_relation(rel) != z->core.PartOf) continue;
                adjacency_set objs;
                const Node    s = z->parse_fact(rel, objs, 0);
                if (s != 0 && objs.count(n) == 1) members.push_back(s);
            }
            return members;
        }

        // Is this node a CONTAINER -- the object of PartOf facts -- rather
        // than an atom? Asked only where the answer can still matter, i.e.
        // for a node that has no fact structure of its own, and rejected per
        // neighbour by the O(1) predicate_of lookup, because such a node may
        // just as well be an ordinary constant with a large adjacency.
        bool is_container(const Zelph* const z, const Node n)
        {
            // Two gates before the adjacency is touched at all, because this
            // node may be a hub -- `nil` and the digits are, throughout the
            // math stack.
            //
            // A HASH node functions as a set constant (or a fact), and since a
            // set constant is hash-consed, identity already decides it
            // correctly; only a COLLECTION, whose identifier stems from a
            // counter or a template, may represent the same container across
            // two different nodes. And a container lacks a NAME: no mechanism
            // that builds one gives it one, whereas every constant referenced
            // by a rule possesses one, within a given language. Manually
            // naming a container incurs a loss of deduplication, not of
            // correctness -- the pair is then compared by identity, just as it
            // was previously.
            if (Zelph::is_hash(n)) return false;
            if (z->is_named_any(n)) return false;

            for (const Node rel : z->get_right(n))
            {
                if (z->predicate_of(rel) != z->core.PartOf) continue;
                adjacency_set objs;
                const Node    s = z->parse_fact(rel, objs, 0);
                if (s != 0 && objs.count(n) == 1) return true;
            }
            return false;
        }

        bool is_conjunction(const Zelph* const z, const Node n)
        {
            return z->check_fact(n, z->core.IsA, {z->core.Conjunction}).is_known();
        }

        // Whether the rule's own collection or a conjunction set is positioned
        // beneath the fact or set constant `n`, accessed via facts and set
        // constants. The template-variable store retains only the variables
        // from a fact's hash components, and those two hold theirs in
        // membership facts: a variable-free fact or set constant with one of
        // them beneath it, like the inner rule `((X q H), (X p H)) => !` in a
        // generator or `{@{c}}`, must be compared by its structure, because
        // each statement in the rule builds a distinct node there. A value
        // located beneath it is irrelevant: it represents the same node across
        // all statements, so the hash of what holds it already determines the
        // outcome.
        bool template_below(const Zelph* const z, const Node n, const int depth)
        {
            if (depth >= max_depth || n == 0 || Zelph::is_var(n)) return false;
            if (!Zelph::is_hash(n)) return z->is_rule_template(n) || is_conjunction(z, n);

            const FactStructure fs = get_preferred_structure(z, n, 3);
            if (fs.subject != 0 && fs.predicate != 0)
            {
                if (template_below(z, fs.subject, depth + 1) || template_below(z, fs.predicate, depth + 1)) return true;
                return std::any_of(fs.objects.begin(), fs.objects.end(), [&](const Node o)
                                   { return template_below(z, o, depth + 1); });
            }

            const std::vector<Node> members = set_members(z, n);
            return std::any_of(members.begin(), members.end(), [&](const Node m)
                               { return template_below(z, m, depth + 1); });
        }

        // Where a node sits in the rule decides which questions are worth
        // asking about it -- and which ones the ENGINE asks. Only a
        // condition may serve as a set of conditions (collect_conditions
        // makes this request, and solely at that point), hence the lookup
        // is confined to condition positions rather than being paid for
        // every atom traversed during the walk.
        enum class Role
        {
            Condition, // the => subject, also of a nested rule, and each member of a condition set
            Term       // subject / predicate / object inside a condition
        };

        // How the REASONER reads this node -- see collect_conditions. A set
        // is recognised by its Conjunction tag, never by "has PartOf
        // neighbours": that question is expensive to ask of a hub node, and
        // it is not the one the engine decides on either. Note that a rule
        // with a SINGLE condition has no set at all -- its => subject is
        // the condition itself, which is why the subject may not simply be
        // assumed to be a set.
        enum class Kind
        {
            Var,
            Set,       // compared by its members: a set of conditions, a rule's own collection
            Container, // a container in condition position the engine does not read as a set: several members, no tag
            Fact,
            SetConst, // a set constant that incorporates a rule's own collection or a conjunction set positioned beneath it
            Opaque
        };

        Kind classify(const Zelph* const z, const Node n, const Role role)
        {
            if (Zelph::is_var(n)) return Kind::Var;
            if (role == Role::Condition && z->is_condition_set(n))
                return Kind::Set;

            if (!Zelph::is_hash(n))
            {
                // In the CONDITION position, the two kinds of container must
                // remain distinct: a container containing multiple members
                // WITHOUT the conjunction tag is not read as a set of
                // conditions and cannot fire, whereas the tagged version
                // does. Rendering both as a set made them alpha-equivalent,
                // causing the system to identify the entry of the tagged rule
                // after the untagged one as a duplicate and silently roll it
                // back -- the correction of a rule that does not function
                // could not be entered.
                if (role == Role::Condition) return is_container(z, n) ? Kind::Container : Kind::Opaque;

                // Elsewhere the id decides, and no membership is read for it:
                // a rule's own collection forms a component of the rule's
                // text and is compared by its members, whereas every other
                // collection serves as a value -- data a rule refers to --
                // and functions as the identical term solely as the same
                // node, akin to an atom. A conjunction set is read by its tag
                // regardless of position: the conditions within a nested
                // rule.
                return z->is_rule_template(n) || is_conjunction(z, n) ? Kind::Set : Kind::Opaque;
            }

            // A hash node that the store designates as variable-free is
            // identified by its id, except when a rule's own collection or a
            // conjunction set is positioned beneath it (template_below).
            bool below = false;
            if (variables_in(z, n) == Vars::None)
            {
                below = template_below(z, n, 0);
                if (!below) return Kind::Opaque;
            }

            const FactStructure fs = get_preferred_structure(z, n, 3);
            if (fs.predicate != 0 && fs.subject != 0) return Kind::Fact;

            // A term of type set constant and a term of type collection
            // represent two distinct kinds of term, despite sharing identical
            // members: `{@{c}}` and `@{@{c}}` derive two different facts.
            // Thus, it constitutes a kind of its own, never matched to a
            // collection.
            if (below || template_below(z, n, 0)) return Kind::SetConst;

            return Kind::Opaque;
        }

        // When a rule is nested in another rule, it transforms into a rule
        // upon derivation, so its subject is read as a condition, just as
        // the engine reads it at that location: a set containing a single
        // condition is equivalent to that condition.
        Role subject_role(const Zelph* const z, const FactStructure& fs)
        {
            return fs.predicate == z->core.Causes ? Role::Condition : Role::Term;
        }

        // A condition set with a single element is equivalent to that
        // condition: the engine reads it in this way (collect_conditions),
        // and it records contradictions for both in the same fashion
        // (record_contradiction). A rule typed with the single condition
        // `(X p Y)` and no set therefore meets the rule whose set contains
        // `(X p Y)` exclusively, regardless of entry order.
        Node single_condition(const Zelph* const z, const Node n, const Role role)
        {
            if (role != Role::Condition || Zelph::is_hash(n) || Zelph::is_var(n)) return n;

            adjacency_set members;
            if (!z->condition_set_members(n, members) || members.size() != 1) return n;
            return *members.begin();
        }

        // A node as canon and the matcher read it in `role` -- the node
        // single_condition reads it as, and its kind -- and the members of a
        // container; each reading is counted (rule_identity_reads).
        std::pair<Node, Kind> read_fresh(const Zelph* const z, Node n, const Role role)
        {
            reads.fetch_add(1, std::memory_order_relaxed);
            n = single_condition(z, n, role);
            return {n, classify(z, n, role)};
        }

        std::vector<Node> read_members(const Zelph* const z, const Node n)
        {
            reads.fetch_add(1, std::memory_order_relaxed);
            return set_members(z, n);
        }

        // What a rendering or a comparison has read of each node since it met
        // its first cycle. Sets that hold one another densely are a few
        // containers met again and again, and each encounter read all their
        // memberships anew, in single_condition, in classify and for the
        // members, so that a position incurred a cost proportional to the
        // width of its set: within the budget, a rule typed over a hundred
        // sets that each held all of them took fifteen seconds. Prior to its
        // first cycle, a walk reads as it always did, and a rule without one
        // incurs the same cost as before.
        class Readings
        {
        public:
            // `n` as single_condition reads it in `role`, and the kind of
            // what it reads.
            std::pair<Node, Kind> node(const Zelph* const z, const Node n, const Role role)
            {
                auto&      known = role == Role::Condition ? _conditions : _terms;
                const auto it    = known.find(n);
                if (it != known.end()) return it->second;
                return known.emplace(n, read_fresh(z, n, role)).first->second;
            }

            // The members of the container `n`.
            const std::vector<Node>& members(const Zelph* const z, const Node n)
            {
                const auto it = _members.find(n);
                if (it != _members.end()) return it->second;
                return _members.emplace(n, read_members(z, n)).first->second;
            }

        private:
            std::unordered_map<Node, std::pair<Node, Kind>> _conditions;
            std::unordered_map<Node, std::pair<Node, Kind>> _terms;
            std::unordered_map<Node, std::vector<Node>>     _members;
        };

        // `n` as a walk reads it in `role`: via `readings` once the walk has
        // met a cycle, directly prior.
        std::pair<Node, Kind> read_node(const Zelph* const z, const Node n, const Role role, Readings* const readings)
        {
            return readings != nullptr ? readings->node(z, n, role) : read_fresh(z, n, role);
        }

        std::vector<Node> members_of(const Zelph* const z, const Node n, Readings* const readings)
        {
            return readings != nullptr ? readings->members(z, n) : read_members(z, n);
        }

        // What a rendering carries: the containers it is within, whether
        // it has met one of them again, the count of positions it has
        // rendered since, and what it has read since.
        struct Walk
        {
            std::vector<Node> path;
            bool              cyclic   = false;
            std::size_t       rendered = 0;
            Readings          readings;
        };

        // Variable-agnostic rendering. Unordered collections (set members,
        // object sets) are sorted by their own rendering, so the result
        // does not depend on adjacency iteration order.
        //
        // `walk.path` holds the containers that the rendering resides within.
        // When a container is encountered a second time along it, a cycle is
        // closed and it renders as the fixed token `?`, as the printer shows
        // it: a set that holds itself directly and indirectly through a
        // second set was entered twice at every level, and the rendering
        // doubled with every level down to the depth cap. Starting from the
        // first token onward, the rendering counts its positions
        // (walk_budget).
        std::string canon(const Zelph* const z, Node n, const int depth, const Role role, Walk& walk)
        {
            steps.fetch_add(1, std::memory_order_relaxed);
            if (walk.cyclic)
            {
                if (walk.rendered >= walk_budget) return "…";
                ++walk.rendered;
            }
            if (depth >= max_depth) return "…";
            const std::pair<Node, Kind> read = read_node(z, n, role, walk.cyclic ? &walk.readings : nullptr);
            n                                = read.first;
            const Kind kind                  = read.second;
            switch (kind)
            {
            case Kind::Var:
                return "v";

            case Kind::Container:
            case Kind::Set:
            case Kind::SetConst:
            {
                if (std::find(walk.path.begin(), walk.path.end(), n) != walk.path.end())
                {
                    walk.cyclic = true;
                    return "?";
                }

                // A conjunction set holds conditions, a term container holds
                // terms -- so the members inherit the role of the node they
                // hang off rather than being assumed to be conditions.
                const bool conditions = role == Role::Condition || (kind == Kind::Set && is_conjunction(z, n));

                std::vector<std::string> parts;
                walk.path.push_back(n);
                for (const Node m : members_of(z, n, walk.cyclic ? &walk.readings : nullptr))
                    parts.push_back(canon(z, m, depth + 1, conditions ? Role::Condition : Role::Term, walk));
                walk.path.pop_back();
                std::sort(parts.begin(), parts.end());

                // The brace tells the kinds apart: identical members, yet
                // one is a rule while the other is not, or one is a set
                // constant whereas the other is a collection.
                std::string out = kind == Kind::SetConst ? "{=" : kind == Kind::Set ? "{"
                                                                                    : "@{";
                for (const auto& p : parts)
                    out += p + " ";
                return out + "}";
            }

            case Kind::Fact:
            {
                const FactStructure fs = get_preferred_structure(z, n, 3);

                std::vector<std::string> objs;
                objs.reserve(fs.objects.size());
                for (const Node o : fs.objects)
                    objs.push_back(canon(z, o, depth + 1, Role::Term, walk));
                std::sort(objs.begin(), objs.end());

                std::string out = "("
                                + canon(z, fs.subject, depth + 1, subject_role(z, fs), walk) + " "
                                + canon(z, fs.predicate, depth + 1, Role::Term, walk) + " ";
                for (const auto& o : objs)
                    out += o + " ";
                return out + ")";
            }

            case Kind::Opaque:
            default:
                return "#" + std::to_string(n);
            }
        }

        struct Matcher
        {
            const Zelph* z;

            // The containers enclosing the match, one path per rule. A pair
            // encountered once more along both paths closes a cycle in both
            // rules and matches as the token canon renders for it; a container
            // met again on only one path meets a structure on the other and
            // matches nothing. A container shared by both rules matches by its
            // identity only within a term (below), also when its content leads
            // back to a container on a path, which canon then renders as the
            // token in one rule and expanded in the other.
            std::vector<Node> path_a;
            std::vector<Node> path_b;

            // Whether the comparison has met a container once more along a
            // path. From that point onward it tracks the number of pairs it
            // compares (walk_budget) and watches its stack down to
            // `stack_end` (stack_reserve); once either is depleted, it is
            // exhausted, and every subsequent pair fails.
            bool           cyclic    = false;
            bool           exhausted = false;
            std::size_t    compared  = 0;
            std::uintptr_t stack_end = 0;

            // The content that the comparison has read since encountering its
            // first cycle.
            Readings readings;

            // Where set, the consequence of `b` that each consequence of `a`
            // is paired with in the successful match (corresponding_part).
            std::vector<std::pair<Node, Node>>* pairing = nullptr;

            // The bijection, kept in both directions so that two distinct
            // variables of `a` cannot collapse onto one variable of `b`.
            using Bijection = std::map<Node, Node>;

            // What must match following the present pair, based on the
            // bijection the match has established up to now. The act of
            // pairing members of a set or objects of a fact is a choice, and a
            // choice that fits all previously matched components might still
            // fail when confronted with what follows -- in
            // (A p B, B p A) => (A q B), the conditions pair up under A -> A
            // as well as under A -> B, yet only one of the two holds for the
            // conclusion. Each choice is therefore tried against the WHOLE
            // rest of the rule. Pairing the conditions first and keeping that
            // pairing caused the answer to rely on the order in which the set
            // held its members -- on node identifiers -- leading to an
            // equivalent rule being treated as a new one on some entries and
            // recognized on others.
            using Then = std::function<bool(Bijection&, Bijection&)>;

            bool match(Node a, Node b, Bijection& ab, Bijection& ba, int depth, Role role, const Then& then)
            {
                steps.fetch_add(1, std::memory_order_relaxed);
                if (exhausted) return false;
                if (cyclic)
                {
                    if (compared >= walk_budget || platform::stack_position() < stack_end)
                    {
                        exhausted = true;
                        return false;
                    }
                    ++compared;
                }
                if (depth >= max_depth) return false;
                Readings* const             known  = cyclic ? &readings : nullptr;
                const std::pair<Node, Kind> read_a = read_node(z, a, role, known);
                const std::pair<Node, Kind> read_b = read_node(z, b, role, known);
                a                                  = read_a.first;
                b                                  = read_b.first;
                const Kind ka                      = read_a.second;
                if (ka != read_b.second) return false;

                switch (ka)
                {
                case Kind::Var:
                {
                    const auto ia = ab.find(a);
                    if (ia != ab.end()) return ia->second == b && then(ab, ba);
                    if (ba.find(b) != ba.end()) return false;
                    Bijection ab2 = ab;
                    Bijection ba2 = ba;
                    ab2[a]        = b;
                    ba2[b]        = a;
                    if (!then(ab2, ba2)) return false;
                    ab = std::move(ab2);
                    ba = std::move(ba2);
                    return true;
                }

                case Kind::Opaque:
                    // Variable-free and hash-consed: identity settles it.
                    return a == b && then(ab, ba);

                case Kind::Container:
                case Kind::Set:
                case Kind::SetConst:
                {
                    // The paths are queried first, before determining the
                    // identity of a term beneath: a container met again along
                    // a path closes a cycle and becomes the token canon
                    // renders, regardless of any other aspect it may have.
                    const bool again_a = std::find(path_a.begin(), path_a.end(), a) != path_a.end();
                    const bool again_b = std::find(path_b.begin(), path_b.end(), b) != path_b.end();
                    if (again_a || again_b)
                    {
                        if (!cyclic)
                        {
                            cyclic    = true;
                            stack_end = platform::stack_floor(stack_reserve, stack_fallback);
                        }
                        return again_a && again_b && then(ab, ba);
                    }

                    // In a term, the SAME container node is considered the same
                    // term, regardless of what the bijection says about the
                    // variables inside it. Two rules share one when the second
                    // was alpha-renamed from the first and the container was
                    // not rebuilt with it -- an accumulator, whose whole point
                    // is that every rule naming it writes into the single
                    // container. Comparing its members would then ask the
                    // bijection to map a variable to itself, which it cannot do
                    // after the rename, causing the generator to write another
                    // copy of its rule on each execution. Two rules that a
                    // program writes over one container are taken as a single
                    // rule in the same manner, also in cases where the
                    // container holds a variable that both rules bind and the
                    // two therefore derive different facts.
                    //
                    // In condition position, a container is read as the rule's
                    // conditions, along with the variables it binds for the
                    // rest of the rule and the sets read before it, so the same
                    // node may represent distinct conditions across two rules,
                    // and it is compared according to its members. Taken by
                    // identity, two rules sharing a single set of conditions
                    // yet differing in consequences were treated as a single
                    // rule; one of them was rolled back, and what it derived
                    // was lost.
                    if (a == b && role != Role::Condition) return then(ab, ba);

                    // Outside the condition position, a set of conditions
                    // constitutes the condition part of a rule nested in a
                    // term, compared as conditions, while a rule's own
                    // collection forms part of the rule's text, compared by its
                    // members as terms.
                    Role members_role = role;
                    if (ka == Kind::Set && role != Role::Condition)
                    {
                        const bool conditions = is_conjunction(z, a);
                        if (conditions != is_conjunction(z, b)) return false;
                        members_role = conditions ? Role::Condition : Role::Term;
                    }

                    // The members are matched on the path, and the rest of
                    // the rule beyond them is matched off it; a backtrack
                    // into the members finds the path exactly as it was left.
                    const Then off_path = [&](Bijection& ab1, Bijection& ba1)
                    {
                        path_a.pop_back();
                        path_b.pop_back();
                        const bool rest = then(ab1, ba1);
                        path_a.push_back(a);
                        path_b.push_back(b);
                        return rest;
                    };
                    path_a.push_back(a);
                    path_b.push_back(b);
                    const bool matched = match_multiset(members_of(z, a, known), members_of(z, b, known), ab, ba, depth + 1, members_role, off_path);
                    path_a.pop_back();
                    path_b.pop_back();
                    return matched;
                }

                case Kind::Fact:
                default:
                {
                    const FactStructure fa = get_preferred_structure(z, a, 3);
                    const FactStructure fb = get_preferred_structure(z, b, 3);
                    if (fa.objects.size() != fb.objects.size()) return false;

                    const std::vector<Node> oa(fa.objects.begin(), fa.objects.end());
                    const std::vector<Node> ob(fb.objects.begin(), fb.objects.end());
                    return match(fa.predicate, fb.predicate, ab, ba, depth + 1, Role::Term, [&](Bijection& ab1, Bijection& ba1)
                                 { return match(fa.subject, fb.subject, ab1, ba1, depth + 1, subject_role(z, fa), [&](Bijection& ab2, Bijection& ba2)
                                                { return match_multiset(oa, ob, ab2, ba2, depth + 1, Role::Term, then); }); });
                }
                }
            }

            // The kind of `n`, read in `role` as match reads it, if it is a
            // container already on `path`.
            std::optional<Kind> back_reference(Node n, const Role role, const std::vector<Node>& path)
            {
                Kind k = Kind::Opaque;
                if (cyclic)
                    std::tie(n, k) = readings.node(z, n, role);
                else
                {
                    reads.fetch_add(1, std::memory_order_relaxed);
                    n = single_condition(z, n, role);
                }
                if (std::find(path.begin(), path.end(), n) == path.end()) return std::nullopt;
                if (!cyclic) k = classify(z, n, role);
                if (k != Kind::Container && k != Kind::Set && k != Kind::SetConst) return std::nullopt;
                return k;
            }

            // Objects and set members are UNORDERED, so the pairing is part
            // of the search. Both hold at most a handful of elements (the
            // conditions of one rule, the objects of one fact), and a wrong
            // pairing is rejected by the recursive match immediately, so the
            // backtracking never gets wide in practice.
            bool match_multiset(std::vector<Node> as, std::vector<Node> bs, Bijection& ab, Bijection& ba, int depth, Role role, const Then& then)
            {
                if (as.size() != bs.size()) return false;
                if (as.empty()) return then(ab, ba);

                const Node a0 = as.back();
                as.pop_back();

                // A back-reference matches every back-reference of its kind on
                // the opposite side as the same token and binds nothing, so the
                // first of them represents all; trying each one in turn
                // multiplied the work by the number of their orders whenever
                // the rest of the rule failed.
                const std::optional<Kind> a_back     = back_reference(a0, role, path_a);
                bool                      tried_back = false;

                for (std::size_t i = 0; i < bs.size(); ++i)
                {
                    if (a_back)
                    {
                        if (tried_back || back_reference(bs[i], role, path_b) != a_back) continue;
                        tried_back = true;
                    }

                    Bijection         ab2 = ab;
                    Bijection         ba2 = ba;
                    std::vector<Node> rest(bs);
                    rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(i));

                    // The only multiset at depth 0 consists of the consequences of a rule.
                    const bool record = depth == 0 && pairing != nullptr;
                    if (record) pairing->emplace_back(a0, bs[i]);
                    if (match(a0, bs[i], ab2, ba2, depth + 1, role, [&](Bijection& ab3, Bijection& ba3)
                              { return match_multiset(as, rest, ab3, ba3, depth, role, then); }))
                    {
                        ab = std::move(ab2);
                        ba = std::move(ba2);
                        return true;
                    }
                    if (record) pairing->pop_back();
                }
                return false;
            }
        };

        // The rule triple, or predicate 0 if `rule` is not a Causes fact.
        FactStructure rule_structure(const Zelph* const z, const Node rule)
        {
            FactStructure fs = get_preferred_structure(z, rule, 3);
            if (fs.predicate != z->core.Causes || fs.subject == 0 || fs.objects.empty())
                return FactStructure{};
            return fs;
        }

        // The comparison between two rules: first the conditions, then the
        // consequences. With `pairing`, the consequence of `b` that is matched
        // with each consequence of `a` in the match that succeeds.
        bool compare_rules(const Zelph* const z, const FactStructure& fa, const FactStructure& fb, std::vector<std::pair<Node, Node>>* const pairing)
        {
            Matcher            m{z};
            Matcher::Bijection ab;
            Matcher::Bijection ba;
            m.pairing = pairing;

            const std::vector<Node> ca(fa.objects.begin(), fa.objects.end());
            const std::vector<Node> cb(fb.objects.begin(), fb.objects.end());
            return m.match(fa.subject, fb.subject, ab, ba, 0, Role::Condition, [&](Matcher::Bijection& ab1, Matcher::Bijection& ba1)
                           { return m.match_multiset(ca, cb, ab1, ba1, 0, Role::Term, [](Matcher::Bijection&, Matcher::Bijection&)
                                                     { return true; }); });
        }
    }

    std::string rule_shape(const Zelph* const z, const Node rule)
    {
        const FactStructure fs = rule_structure(z, rule);
        if (fs.predicate == 0) return {};

        Walk                     walk;
        std::vector<std::string> consequences;
        consequences.reserve(fs.objects.size());
        for (const Node o : fs.objects)
            consequences.push_back(canon(z, o, 1, Role::Term, walk));
        std::sort(consequences.begin(), consequences.end());

        std::string out = canon(z, fs.subject, 1, Role::Condition, walk) + "=>";
        for (const auto& c : consequences)
            out += c + " ";
        return out;
    }

    bool rules_alpha_equivalent(const Zelph* const z, const Node a, const Node b)
    {
        if (a == b) return true;

        const FactStructure fa = rule_structure(z, a);
        const FactStructure fb = rule_structure(z, b);
        if (fa.predicate == 0 || fb.predicate == 0) return false;
        if (fa.objects.size() != fb.objects.size()) return false;
        return compare_rules(z, fa, fb, nullptr);
    }

    Node corresponding_part(const Zelph* const z, const Node a, const Node b, const Node part)
    {
        const FactStructure fa = rule_structure(z, a);
        const FactStructure fb = rule_structure(z, b);
        if (fa.predicate == 0 || fb.predicate == 0 || fa.objects.size() != fb.objects.size()) return 0;

        if (part == fa.subject) return fb.subject;
        if (part == fa.predicate) return fb.predicate;
        if (fa.objects.count(part) == 0) return 0;
        if (fb.objects.size() == 1) return *fb.objects.begin();

        // Several consequences: the one that the comparison between the two
        // rules pairs with `part` in the successful match. It is the
        // comparison performed by rules_alpha_equivalent, in identical
        // order and within the same budget, so it pairs `part` wherever
        // that comparison found the rules alike. A standalone search, which
        // prioritized pairing `part` first, might exhaust its budget on an
        // incorrect pairing over sets that hold one another before
        // attempting the right one, and the part of the kept rule was node
        // 0.
        std::vector<std::pair<Node, Node>> pairing;
        if (!compare_rules(z, fa, fb, &pairing)) return 0;
        for (const auto& [mine, theirs] : pairing)
            if (mine == part) return theirs;
        return 0;
    }

    bool rule_text_below(const Zelph* const z, const Node n)
    {
        return template_below(z, n, 0);
    }

    bool is_rule_statement(const Zelph* const z, const Node rel)
    {
        // Climbs from `rel` through what holds it. The facts that hold a node
        // are its neighbours on both sides: a subject is joined to its fact
        // in both ways, an object in one direction only. A set constant that
        // holds it is also climbed; any membership in a different collection
        // is considered data, and the climb ends there.
        std::vector<Node>        pending{rel};
        std::unordered_set<Node> seen{rel};
        while (!pending.empty())
        {
            const Node n = pending.back();
            pending.pop_back();
            for (const adjacency_set& side : {z->get_left(n), z->get_right(n)})
            {
                for (const Node r : side)
                {
                    if (!Zelph::is_hash(r) || r == n) continue;
                    climb_steps.fetch_add(1, std::memory_order_relaxed);

                    adjacency_set objects;
                    const Node    subject = z->parse_fact(r, objects, 0);
                    if (subject != n && objects.count(n) == 0) continue;

                    const Node predicate = z->predicate_of(r);
                    if (predicate == z->core.Causes) return true;
                    if (predicate == z->core.PartOf)
                    {
                        if (subject != n) continue;
                        for (const Node o : objects)
                        {
                            if (is_conjunction(z, o) || z->is_rule_template(o)) return true;
                            if (Zelph::is_hash(o) && seen.insert(o).second) pending.push_back(o);
                        }
                        continue;
                    }
                    if (seen.insert(r).second) pending.push_back(r);
                }
            }
        }
        return false;
    }

    std::string unbound_predicate(const Zelph* const z, std::vector<Node> conditions, const adjacency_set& consequences)
    {
        // Before examining the conditions, the facts are read from the
        // genuine store: a consequence with a predicate that is a name, which
        // is almost every case, incurs a lookup for each fact it writes.
        std::vector<Node>                  pending(consequences.begin(), consequences.end());
        std::unordered_set<Node>           seen;
        std::vector<std::pair<Node, Node>> written;
        while (!pending.empty())
        {
            const Node n = pending.back();
            pending.pop_back();
            if (n == 0 || Zelph::is_var(n) || !Zelph::is_hash(n) || !seen.insert(n).second) continue;
            const Node p = z->predicate_of(n);
            if (p == 0 || p == z->core.Causes) continue;
            // A composite predicate constitutes a fact that the consequence
            // writes as well: `(A is p) => (A (b M s) c)` derived
            // `a (b -> s) c`. A fact having `n` as its predicate appears
            // within the objects of `n` (parse_fact), and it is not a
            // component of `n`: walked, a ground composite predicate led into
            // every fact in the graph that uses it.
            if (Zelph::is_var(p))
                written.emplace_back(n, p);
            else
                pending.push_back(p);
            adjacency_set objects;
            pending.push_back(z->parse_fact(n, objects));
            for (const Node object : objects)
                if (z->predicate_of(object) != n) pending.push_back(object);
        }
        if (written.empty()) return {};

        // The variables a condition names, the members of the sets it holds
        // included, as `(Z in {W})` binds W.
        const auto gather = [z](const Node root, std::unordered_set<Node>& into)
        {
            std::vector<Node>        stack{root};
            std::unordered_set<Node> visited;
            while (!stack.empty())
            {
                const Node n = stack.back();
                stack.pop_back();
                if (n == 0 || !visited.insert(n).second) continue;
                if (Zelph::is_var(n))
                {
                    into.insert(n);
                    continue;
                }
                adjacency_set parts;
                if (!Zelph::is_hash(n))
                {
                    if (z->set_pattern_members(n, parts)) stack.insert(stack.end(), parts.begin(), parts.end());
                    continue;
                }
                const Node p = z->predicate_of(n);
                if (p == 0) continue;
                stack.push_back(p);
                stack.push_back(z->parse_fact(n, parts));
                // A fact whose predicate is `n` does not constitute a part
                // of it, as previously stated.
                for (const Node part : parts)
                    if (z->predicate_of(part) != n) stack.push_back(part);
            }
        };

        // At every level, the conditions, each set read once: a set that holds
        // itself, either directly or through another set, had its members again
        // and again taken up, and the walk never ended.
        adjacency_set leaves;
        for (const Node c : conditions)
            leaves.insert(c);
        std::unordered_set<Node> sets;
        z->flatten_condition_sets(leaves, sets);

        std::unordered_set<Node> bound;
        std::unordered_set<Node> negated;
        for (const Node c : leaves)
            gather(c, z->negation_operand(c) != 0 ? negated : bound);

        for (const auto& [fact, predicate] : written)
        {
            if (bound.count(predicate) != 0) continue;
            const std::string name = z->format(predicate);
            return "The predicate of \"" + z->format(fact) + "\" in the rule's consequence is the variable " + name + ", which no condition binds"
                 + (negated.count(predicate) != 0 ? " (a negated condition binds nothing)" : "") + ": a firing has no predicate to write, and a new "
                 + "node standing in for one would be a relation type without a name. Bind " + name + " in a condition, or write the predicate as a name.";
        }
        return {};
    }

    uint64_t rule_statement_climb_steps()
    {
        return climb_steps.load(std::memory_order_relaxed);
    }

    uint64_t rule_identity_reads()
    {
        return reads.load(std::memory_order_relaxed);
    }

    uint64_t rule_identity_steps()
    {
        return steps.load(std::memory_order_relaxed);
    }
}
