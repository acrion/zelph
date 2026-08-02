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
#include "zelph.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

namespace zelph::network
{
    namespace
    {
        // Rules are small and shallow; the cap only stops a pathological
        // cycle from becoming an infinite walk. A truncated rendering makes
        // the shape less selective, never wrong -- the decision is taken by
        // the matcher, which carries the same cap.
        constexpr int max_depth = 32;

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
            // A HASH node is a set constant (or a fact), and a set constant
            // hash-conses, so identity already decides it correctly; only a
            // COLLECTION, whose id is a counter, can be the same container
            // under two different nodes. And a container has no NAME: nothing
            // that builds one gives it one, while every constant a rule
            // mentions has one. Naming a container by hand costs the dedup,
            // not correctness -- the pair is then compared by identity, which
            // is what it was before.
            if (Zelph::is_hash(n)) return false;
            if (!z->get_name(n).empty()) return false; // current language, no fallback walk

            for (const Node rel : z->get_right(n))
            {
                if (z->predicate_of(rel) != z->core.PartOf) continue;
                adjacency_set objs;
                const Node    s = z->parse_fact(rel, objs, 0);
                if (s != 0 && objs.count(n) == 1) return true;
            }
            return false;
        }

        // Where a node sits in the rule decides which questions are worth
        // asking about it -- and which ones the ENGINE asks. Only a
        // condition can be a conjunction or be negated (collect_conditions
        // tests exactly those two tags, and only there), so the tag lookups
        // are confined to condition positions instead of being paid for
        // every atom the walk passes.
        enum class Role
        {
            Condition, // the => subject, and every member of a condition set
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
            Set,
            Fact,
            Opaque
        };

        Kind classify(const Zelph* const z, const Node n, const Role role)
        {
            if (Zelph::is_var(n)) return Kind::Var;
            if (role == Role::Condition && z->check_fact(n, z->core.IsA, {z->core.Conjunction}).is_known())
                return Kind::Set;

            // A container is checked BEFORE the template-var store is asked.
            // That store answers about the node itself, and a container holds
            // its variables in its membership FACTS -- so it reports "no
            // variables" for `@{Y}` and the node used to leave as Opaque.
            if (is_container(z, n)) return Kind::Set;

            if (variables_in(z, n) == Vars::None) return Kind::Opaque;

            const FactStructure fs = get_preferred_structure(z, n, 3);
            if (fs.predicate != 0 && fs.subject != 0) return Kind::Fact;

            return Kind::Opaque;
        }

        // Negation is the other tag the engine reads on a CONDITION, and it
        // has to be spelled out: a negated pattern and the bare pattern are
        // otherwise structurally identical.
        bool negated(const Zelph* const z, const Node n, const Role role)
        {
            return role == Role::Condition
                && z->check_fact(n, z->core.IsA, {z->core.Negation}).is_known();
        }

        // Variable-agnostic rendering. Unordered collections (set members,
        // object sets) are sorted by their own rendering, so the result
        // does not depend on adjacency iteration order.
        std::string canon(const Zelph* const z, const Node n, const int depth, const Role role)
        {
            if (depth >= max_depth) return "…";

            switch (classify(z, n, role))
            {
            case Kind::Var:
                return "v";

            case Kind::Set:
            {
                // A conjunction set holds conditions, a term container holds
                // terms -- so the members inherit the role of the node they
                // hang off rather than being assumed to be conditions.
                std::vector<std::string> parts;
                for (const Node m : set_members(z, n))
                    parts.push_back(canon(z, m, depth + 1, role));
                std::sort(parts.begin(), parts.end());

                std::string out = "{";
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
                    objs.push_back(canon(z, o, depth + 1, Role::Term));
                std::sort(objs.begin(), objs.end());

                std::string out = (negated(z, n, role) ? "!(" : "(")
                                + canon(z, fs.subject, depth + 1, Role::Term) + " "
                                + canon(z, fs.predicate, depth + 1, Role::Term) + " ";
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

            // The bijection, kept in both directions so that two distinct
            // variables of `a` cannot collapse onto one variable of `b`.
            using Bijection = std::map<Node, Node>;

            bool match(Node a, Node b, Bijection& ab, Bijection& ba, int depth, Role role)
            {
                if (depth >= max_depth) return false;

                const Kind ka = classify(z, a, role);
                if (ka != classify(z, b, role)) return false;

                switch (ka)
                {
                case Kind::Var:
                {
                    const auto ia = ab.find(a);
                    if (ia != ab.end()) return ia->second == b;
                    if (ba.find(b) != ba.end()) return false;
                    ab[a] = b;
                    ba[b] = a;
                    return true;
                }

                case Kind::Opaque:
                    // Variable-free and hash-consed: identity settles it.
                    return a == b;

                case Kind::Set:
                    // The SAME container node is the same term, whatever the
                    // bijection says about the variables inside it. Two rules
                    // share one only when the second was alpha-renamed out of
                    // the first and the container was not rebuilt with it --
                    // an accumulator, whose whole point is that every rule
                    // naming it writes into the one container. Comparing its
                    // members would then ask the bijection to map a variable
                    // to itself, which it cannot after the rename, and the
                    // generator would write another copy of its rule on every
                    // run.
                    if (a == b) return true;

                    return match_multiset(set_members(z, a), set_members(z, b), ab, ba, depth + 1, role);

                case Kind::Fact:
                default:
                {
                    if (negated(z, a, role) != negated(z, b, role)) return false;

                    const FactStructure fa = get_preferred_structure(z, a, 3);
                    const FactStructure fb = get_preferred_structure(z, b, 3);
                    if (fa.objects.size() != fb.objects.size()) return false;
                    if (!match(fa.predicate, fb.predicate, ab, ba, depth + 1, Role::Term)) return false;
                    if (!match(fa.subject, fb.subject, ab, ba, depth + 1, Role::Term)) return false;

                    return match_multiset(std::vector<Node>(fa.objects.begin(), fa.objects.end()),
                                          std::vector<Node>(fb.objects.begin(), fb.objects.end()),
                                          ab, ba, depth + 1, Role::Term);
                }
                }
            }

            // Objects and set members are UNORDERED, so the pairing is part
            // of the search. Both hold at most a handful of elements (the
            // conditions of one rule, the objects of one fact), and a wrong
            // pairing is rejected by the recursive match immediately, so the
            // backtracking never gets wide in practice.
            bool match_multiset(std::vector<Node> as, std::vector<Node> bs, Bijection& ab, Bijection& ba, int depth, Role role)
            {
                if (as.size() != bs.size()) return false;
                if (as.empty()) return true;

                const Node a0 = as.back();
                as.pop_back();

                for (std::size_t i = 0; i < bs.size(); ++i)
                {
                    Bijection         ab2 = ab;
                    Bijection         ba2 = ba;
                    std::vector<Node> rest(bs);
                    rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(i));

                    if (match(a0, bs[i], ab2, ba2, depth + 1, role)
                        && match_multiset(as, rest, ab2, ba2, depth, role))
                    {
                        ab = std::move(ab2);
                        ba = std::move(ba2);
                        return true;
                    }
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
    }

    std::string rule_shape(const Zelph* const z, const Node rule)
    {
        const FactStructure fs = rule_structure(z, rule);
        if (fs.predicate == 0) return {};

        std::vector<std::string> consequences;
        consequences.reserve(fs.objects.size());
        for (const Node o : fs.objects)
            consequences.push_back(canon(z, o, 1, Role::Term));
        std::sort(consequences.begin(), consequences.end());

        std::string out = canon(z, fs.subject, 1, Role::Condition) + "=>";
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

        Matcher            m{z};
        Matcher::Bijection ab;
        Matcher::Bijection ba;

        if (!m.match(fa.subject, fb.subject, ab, ba, 0, Role::Condition)) return false;

        return m.match_multiset(std::vector<Node>(fa.objects.begin(), fa.objects.end()),
                                std::vector<Node>(fb.objects.begin(), fb.objects.end()),
                                ab, ba, 0, Role::Term);
    }
}
