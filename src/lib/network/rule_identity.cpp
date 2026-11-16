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

#include "container_reading.hpp"
#include "fact_structure.hpp"
#include "platform/platform_utils.hpp"
#include "zelph.hpp"

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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

            const std::vector<Node> members = container_members(z, n);
            return std::any_of(members.begin(), members.end(), [&](const Node m)
                               { return template_below(z, m, depth + 1); });
        }

        // How the reasoner reads this node within a term (read_container,
        // ReadAt::Term), regardless of its location in the rule, the rule's own
        // set of conditions and its members included: a rule pattern pairs the
        // members of a rule's set by node, a variable bound at the subject of a
        // ground rule binds the set's node, and the membership of a ground
        // member answers a query. Read as the firing reads them there, a member
        // named set by its members and a set of one condition treated as that
        // condition, two rules these readers distinguish were taken as one, and
        // the rule rolled back lost what it derived. The one exception is
        // own_subject's. Where a firing writes a set of conditions as it
        // stands, the matcher also inquires how it writes it (ReadAt::Written).
        // A set is identified by its id or its Conjunction tag, never by "has
        // PartOf neighbours": that question is costly to pose to a hub node,
        // and it is not the one the engine decides on either. Note that a rule
        // with a SINGLE condition has no set at all -- its => subject is the
        // condition itself, which is why the subject may not simply be assumed
        // to be a set.
        enum class Kind
        {
            Var,
            Set, // compared by its members: a set of conditions, the rule's own collection (see Matcher::match)
            Fact,
            SetConst, // a set constant without a name that incorporates a rule's own collection or a conjunction set located underneath it
            Opaque
        };

        Kind classify(const Zelph* const z, const Node n)
        {
            if (Zelph::is_var(n)) return Kind::Var;

            if (!Zelph::is_hash(n))
            {
                // The id decides, and no membership is read for it. A rule's
                // own collection forms a component of the rule's text and is
                // compared by its members, whereas every other collection
                // serves as a value -- data a rule refers to -- and functions
                // as the identical term exclusively as the same node, much like
                // an atom: an untagged container holding several members that a
                // program made is not a set of conditions, a rule over it never
                // fires, and a rule pattern matches it by node. A conjunction
                // set is read via its tag irrespective of position; when two
                // distinct ones meet, the matcher decides whether their members
                // are compared.
                switch (read_container(z, n, ReadAt::Term).reading)
                {
                case Reading::ConditionsInTerm:
                case Reading::OwnCollection:
                    return Kind::Set;
                default:
                    return Kind::Opaque;
                }
            }

            // A hash node that the store designates as variable-free is
            // recognized by its id, unless a rule's own collection or a
            // conjunction set lies beneath it (template_below). So is a set of
            // ground conditions expressed explicitly,
            // `(*{(a p b) (c q d)} ~ conjunction)`, regardless of position,
            // whether at a rule's own subject or within a term: it is a set
            // constant, and the readers that read it by its node tell it from
            // the comma list of the same conditions, which constitutes a
            // collection. A variable bound at the subject of a ground rule
            // binds the one or the other, a condition that names the set
            // constant matches facts pertaining exclusively to it, and a rule
            // pattern with a subject that is a set pattern meets solely the
            // rule over it (unify_nodes matches a set pattern only against a
            // set constant). Compared by its members, the rule over the set
            // constant, entered after the rule over the comma list, was rolled
            // back as its twin, both at the rule's own subject and within a
            // term, and what those readers derived from it was lost. A rule
            // over the set constant prints in its own spelling
            // (node_to_string), so the line printed and re-entered remains
            // identical to the original rule.
            bool below = false;
            if (variables_in(z, n) == Vars::None)
            {
                below = template_below(z, n, 0);
                if (!below) return Kind::Opaque;
            }

            const FactStructure fs = get_preferred_structure(z, n, 3);
            if (fs.predicate != 0 && fs.subject != 0) return Kind::Fact;

            // What remains is a set constant, read as the reasoner reads it
            // within a term (read_container). A term of type set constant and a
            // term of type collection represent two distinct kinds of term,
            // despite sharing identical members: `{@{c}}` and `@{@{c}}` derive
            // two different facts. Thus, one over the rule's text constitutes a
            // kind of its own, never matched to a collection. A named one is a
            // constant: the instantiation keeps it as written, so a rule writes
            // that node and no other. Compared by its members, the second of
            // two rules writing two named set constants over collections with
            // the same members was rolled back, and the fact it wrote was lost.
            // A tagged one is a set of conditions, compared by its members only
            // where the rule's text lies beneath it: a set of ground conditions
            // is its node, as above, also in cases where the store cannot say
            // whether the set holds a variable (variables_in), since the walk
            // gives the same answer the store gives.
            switch (read_container(z, n, ReadAt::Term).reading)
            {
            case Reading::ConditionsInTerm:
                return below || template_below(z, n, 0) ? Kind::Set : Kind::Opaque;
            case Reading::SetConstant:
                return below || template_below(z, n, 0) ? Kind::SetConst : Kind::Opaque;
            default:
                return Kind::Opaque;
            }
        }

        // The kind of a node as canon and the matcher read it, and the members
        // of a container; each reading is counted (rule_identity_reads).
        Kind read_fresh(const Zelph* const z, const Node n)
        {
            reads.fetch_add(1, std::memory_order_relaxed);
            return classify(z, n);
        }

        std::vector<Node> read_members(const Zelph* const z, const Node n)
        {
            reads.fetch_add(1, std::memory_order_relaxed);
            return container_members(z, n);
        }

        // How a firing writes the set of conditions `n` where it writes it as
        // it stands (Matcher::match), counted as a reading.
        Reading read_written_fresh(const Zelph* const z, const Node n)
        {
            reads.fetch_add(1, std::memory_order_relaxed);
            return read_container(z, n, ReadAt::Written).reading;
        }

        // What a rendering or a comparison has read of each node since it met
        // its first cycle. Sets that hold one another densely are a few
        // containers revisited again and again, and each interaction read all
        // their memberships anew, in classify and for the members, so that a
        // position incurred a cost proportional to the width of its set:
        // within the budget, a rule typed over a hundred sets that each held
        // all of them took fifteen seconds. Prior to its first cycle, a walk
        // reads as it always did, and a rule without one incurs the same cost
        // as before.
        class Readings
        {
        public:
            // The kind of `n`.
            Kind kind(const Zelph* const z, const Node n)
            {
                const auto it = _kinds.find(n);
                if (it != _kinds.end()) return it->second;
                return _kinds.emplace(n, read_fresh(z, n)).first->second;
            }

            // The members of the container `n`.
            const std::vector<Node>& members(const Zelph* const z, const Node n)
            {
                const auto it = _members.find(n);
                if (it != _members.end()) return it->second;
                return _members.emplace(n, read_members(z, n)).first->second;
            }

            // How a firing writes `n`, a set of conditions.
            Reading written(const Zelph* const z, const Node n)
            {
                const auto it = _written.find(n);
                if (it != _written.end()) return it->second;
                return _written.emplace(n, read_written_fresh(z, n)).first->second;
            }

        private:
            std::unordered_map<Node, Kind>              _kinds;
            std::unordered_map<Node, std::vector<Node>> _members;
            std::unordered_map<Node, Reading>           _written;
        };

        // The kind of `n` as a walk reads it: through `readings` once the walk
        // has met a cycle, directly prior.
        Kind read_node(const Zelph* const z, const Node n, Readings* const readings)
        {
            return readings != nullptr ? readings->kind(z, n) : read_fresh(z, n);
        }

        std::vector<Node> members_of(const Zelph* const z, const Node n, Readings* const readings)
        {
            return readings != nullptr ? readings->members(z, n) : read_members(z, n);
        }

        Reading written_as(const Zelph* const z, const Node n, Readings* const readings)
        {
            return readings != nullptr ? readings->written(z, n) : read_written_fresh(z, n);
        }

        // The `=>` subject of `rule` as the comparison and the fingerprint read
        // it: a set containing a single member that only the firing reads,
        // apart from a tag it carries, is that member (read_container,
        // ReadAt::OwnSubject, which names what the tag costs), meaning the four
        // spellings of a rule whose single condition holds a variable are one
        // rule; every other subject is read as a term, just as every node
        // beneath it is. Requested only in the context of the rule being
        // compared or fingerprinted: a rule that it writes is a term within it
        // until a construction builds that rule, and a rule that it quotes, or
        // that one of its conditions names or quotes, remains a term
        // throughout.
        Node own_subject(const Zelph* const z, const Node rule, const FactStructure& fs)
        {
            reads.fetch_add(1, std::memory_order_relaxed);
            const ContainerReading subject = read_container(z, fs.subject, ReadAt::OwnSubject, nullptr, rule);
            return subject.reading == Reading::Condition ? subject.node : fs.subject;
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
        std::string canon(const Zelph* const z, const Node n, const int depth, Walk& walk)
        {
            steps.fetch_add(1, std::memory_order_relaxed);
            if (walk.cyclic)
            {
                if (walk.rendered >= walk_budget) return "…";
                ++walk.rendered;
            }
            if (depth >= max_depth) return "…";
            const Kind kind = read_node(z, n, walk.cyclic ? &walk.readings : nullptr);
            switch (kind)
            {
            case Kind::Var:
                return "v";

            case Kind::Set:
            case Kind::SetConst:
            {
                if (std::find(walk.path.begin(), walk.path.end(), n) != walk.path.end())
                {
                    walk.cyclic = true;
                    return "?";
                }

                std::vector<std::string> parts;
                walk.path.push_back(n);
                for (const Node m : members_of(z, n, walk.cyclic ? &walk.readings : nullptr))
                    parts.push_back(canon(z, m, depth + 1, walk));
                walk.path.pop_back();
                std::sort(parts.begin(), parts.end());

                // The brace tells the kinds apart: a set constant and a
                // collection with identical members are two terms.
                std::string out = kind == Kind::SetConst ? "{=" : "{";
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
                    objs.push_back(canon(z, o, depth + 1, walk));
                std::sort(objs.begin(), objs.end());

                std::string out = "("
                                + canon(z, fs.subject, depth + 1, walk) + " "
                                + canon(z, fs.predicate, depth + 1, walk) + " ";
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
            // matches nothing. A container shared by both rules is compared by
            // its members in the same way as any other, so its content meets
            // the paths where canon's rendering meets them.
            std::vector<Node> path_a;
            std::vector<Node> path_b;

            // What the statement that built rule `a` created, none for a
            // construction; empty whenever neither rule is new (NewRuleNodes).
            const NewRuleNodes* made = nullptr;

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

            // Whether the distinct sets of conditions `a` and `b` can be
            // compared by their members (see match).
            bool alike_sets(const Node a, const Node b) const
            {
                if (z->is_named_any(a) || z->is_named_any(b)) return false;
                return made == nullptr || !*made || (*made)(a);
            }

            // Whether a firing writes a pair as it stands, as its claim reaches
            // it (Zelph::claim_nested). A place the claim bypasses due to a
            // reason the comparison does not read (an opaque predicate, a
            // refutation, or a membership within rule text) is considered
            // written: two rules separated at that point constitute a
            // duplicate, never a loss.
            enum class Written
            {
                // Read, or written into: the conditions of the rule and of a
                // rule it writes, a rule it quotes, the object of a membership.
                No,
                // A part of a fact, not a rule, that a firing writes, and a
                // member of a container its claim enters.
                Yes,
                // A consequence of the rule, or of a rule it writes, at any
                // level of depth: a `=>` there is a rule written in turn,
                // whose own firing writes its consequences. Read as rule
                // text there, a typed generator whose rule writes
                // `(a s b, c t d)` was rolled back as the twin of the
                // generator a rule over rules wrote over the set of
                // `(a s b, c t d) => (e f g)`, and `e f g` was lost.
                Consequence
            };

            bool match(const Node a, const Node b, Bijection& ab, Bijection& ba, int depth, const Written written, const Then& then)
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
                Readings* const known = cyclic ? &readings : nullptr;
                const Kind      ka    = read_node(z, a, known);
                if (ka != read_node(z, b, known)) return false;

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

                case Kind::Set:
                case Kind::SetConst:
                {
                    // The paths are examined first, before any other request
                    // made of the container: one met again along a path closes
                    // a cycle and serves as the token canon renders, regardless
                    // of any other aspect it might possess.
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

                    // One container node shared by both rules is compared by
                    // its members via the bijection, regardless of position. A
                    // container read as the rule's conditions binds variables
                    // for the rest of the rule, a set pattern within a
                    // condition binds the content it holds through its
                    // memberships, and a rule's own collection, a set constant
                    // over one, or a set of conditions that a generator
                    // rebuilds is instantiated with the bindings: the two rules
                    // read such a container differently when it holds a
                    // variable that both bind and the bijection maps that
                    // variable elsewhere. Taken by identity, two rules a
                    // program wrote over one container were treated as a single
                    // rule; one of them was rolled back, and what it derived
                    // was lost. A set of conditions that a firing keeps as its
                    // node is held to the same test, so two rules over it may
                    // remain distinct even when one would suffice.
                    //
                    // The members of a set of conditions are compared as terms,
                    // much like those in a rule's own collection, regardless of
                    // where the set appears, the rule's own set of conditions
                    // included: when the reasoner matches such a set through
                    // its members, it pairs them in the same way it matches any
                    // term (unify_members), meaning a member named set is the
                    // node it is, a set containing a single condition is not
                    // the condition itself, and an untagged container is a
                    // node. Read as conditions, two rules over `(a p b, n1)`
                    // and `(a p b, n2)`, with n1 and n2 named sets having
                    // identical members, were treated as a single rule, and
                    // likewise were two rules that name or quote such rules;
                    // the one rolled back lost what it derived.
                    const bool paired          = ka == Kind::Set;
                    Written    members_written = written == Written::No ? Written::No : Written::Yes;
                    if (paired)
                    {
                        // A set of conditions never meets a rule's own
                        // collection: an untagged collection containing several
                        // conditions does not fire, and the tagged rule entered
                        // to correct it constitutes a rule of its own.
                        const bool conditions = is_conjunction(z, a);
                        if (conditions != is_conjunction(z, b)) return false;

                        // Where a firing writes two sets of conditions as they
                        // are, they form a single term only when it writes them
                        // alike (read_container, ReadAt::Written): its claim
                        // asserts the members of a list the statement writes,
                        // and of a set constant, and writes a rule's set, or one
                        // a construction created, via its node. Read alike, a
                        // typed rule writing `(a s b, c t d)` was rolled back as
                        // the twin of a rule written over the set of the rule
                        // `(a s b, c t d) => (e f g)`, and `a s b`, `c t d` and
                        // `e f g` were lost.
                        if (conditions && written != Written::No)
                        {
                            const Reading reading = written_as(z, a, known);
                            if (reading != written_as(z, b, known)) return false;
                            members_written = reading == Reading::ConditionsInTerm ? Written::Yes : Written::No;
                        }

                        // Two distinct sets of conditions are treated as two
                        // separate terms by the reasoner, the rule's own set of
                        // conditions included: a firing writes its facts
                        // regarding the node it holds, a variable bound at the
                        // subject of a ground rule binds that node, and the
                        // membership of a ground member answers a query
                        // (NewRuleNodes). Where the comparison involves a new
                        // rule, they are the same set only when rule `a`'s
                        // statement made its set and neither set is named,
                        // because a set typed in a rule's text is made anew with
                        // each statement of the rule; in the claim search, which
                        // contains none, wherever neither is named. A named set
                        // is the same set only as itself.
                        if (conditions && a != b && !alike_sets(a, b)) return false;

                        // A set of conditions functions as a node that a firing
                        // writes facts about, just as the collection a firing
                        // makes from a rule's own collection of fixed members --
                        // each one per binding and template -- does; thus the
                        // container becomes involved in the bijection: once
                        // paired with a container of `b`, it stands for that
                        // particular one at every location, including the rule's
                        // own subject, and no other container of `a` stands for
                        // it, not even when a container shared by both rules is
                        // paired with itself. Paired independently at each
                        // position, one set of conditions from the new rule
                        // corresponded to two distinct sets from the rule
                        // currently in force, and a guard `L != M` or a join
                        // across the single set told the two rules apart after
                        // the second had been rolled back; a rule's own set of
                        // conditions that its consequence also holds was paired
                        // only at the consequence, and the rule over two sets
                        // was interpreted as the rule over one. A collection
                        // that holds a variable is written as the set constant
                        // of its members, which constitutes one node for two
                        // templates, so two rules over it can remain two even
                        // when one would suffice.
                        if (const auto partner = ab.find(a); partner != ab.end()) return partner->second == b && then(ab, ba);
                        if (ba.find(b) != ba.end()) return false;
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
                    // A match leaves the bijection in its prior state upon
                    // failure, thus reclaiming the pair along with it.
                    if (paired)
                    {
                        ab[a] = b;
                        ba[b] = a;
                    }
                    path_a.push_back(a);
                    path_b.push_back(b);
                    const bool matched = match_multiset(members_of(z, a, known), members_of(z, b, known), ab, ba, depth + 1, members_written, off_path);
                    path_a.pop_back();
                    path_b.pop_back();
                    if (!matched && paired)
                    {
                        ab.erase(a);
                        ba.erase(b);
                    }
                    return matched;
                }

                case Kind::Fact:
                default:
                {
                    const FactStructure fa = get_preferred_structure(z, a, 3);
                    const FactStructure fb = get_preferred_structure(z, b, 3);
                    if (fa.objects.size() != fb.objects.size()) return false;

                    // A firing writes the parts of a fact it writes, yet the
                    // objects within a membership are written into, and a rule
                    // it quotes is rule text. A rule it writes is rule text,
                    // except for its consequences, which are written by the
                    // firing of that rule.
                    const bool    rule    = fa.predicate == z->core.Causes;
                    const Written parts   = written == Written::No || rule ? Written::No : Written::Yes;
                    const Written objects = rule ? (written == Written::Consequence ? Written::Consequence : Written::No)
                                                 : (fa.predicate == z->core.PartOf ? Written::No : parts);

                    const std::vector<Node> oa(fa.objects.begin(), fa.objects.end());
                    const std::vector<Node> ob(fb.objects.begin(), fb.objects.end());
                    return match(fa.predicate, fb.predicate, ab, ba, depth + 1, parts, [&](Bijection& ab1, Bijection& ba1)
                                 { return match(fa.subject, fb.subject, ab1, ba1, depth + 1, parts, [&](Bijection& ab2, Bijection& ba2)
                                                { return match_multiset(oa, ob, ab2, ba2, depth + 1, objects, then); }); });
                }
                }
            }

            // The kind of `n`, read as match reads it, if it is a container
            // already present on `path`.
            std::optional<Kind> back_reference(const Node n, const std::vector<Node>& path)
            {
                if (std::find(path.begin(), path.end(), n) == path.end()) return std::nullopt;
                const Kind k = read_node(z, n, cyclic ? &readings : nullptr);
                if (k != Kind::Set && k != Kind::SetConst) return std::nullopt;
                return k;
            }

            // Objects and set members are UNORDERED, so the pairing is part
            // of the search. Both hold at most a handful of elements (the
            // conditions of one rule, the objects of one fact), and a wrong
            // pairing is rejected by the recursive match immediately, so the
            // backtracking never gets wide in practice.
            bool match_multiset(std::vector<Node> as, std::vector<Node> bs, Bijection& ab, Bijection& ba, int depth, const Written written, const Then& then)
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
                const std::optional<Kind> a_back     = back_reference(a0, path_a);
                bool                      tried_back = false;

                for (std::size_t i = 0; i < bs.size(); ++i)
                {
                    if (a_back)
                    {
                        if (tried_back || back_reference(bs[i], path_b) != a_back) continue;
                        tried_back = true;
                    }

                    Bijection         ab2 = ab;
                    Bijection         ba2 = ba;
                    std::vector<Node> rest(bs);
                    rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(i));

                    // The only multiset at depth 0 consists of the consequences of a rule.
                    const bool record = depth == 0 && pairing != nullptr;
                    if (record) pairing->emplace_back(a0, bs[i]);
                    if (match(a0, bs[i], ab2, ba2, depth + 1, written, [&](Bijection& ab3, Bijection& ba3)
                              { return match_multiset(as, rest, ab3, ba3, depth, written, then); }))
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

        // The comparison of the rules `a` and `b`: first their subjects, as
        // own_subject reads them, then the consequences, which a firing
        // writes, all contained within a single bijection. With `pairing`, the
        // consequence of `b` that is matched with each consequence of `a` in
        // the match that succeeds.
        bool compare_rules(const Zelph* const z, const Node a, const Node b, const FactStructure& fa, const FactStructure& fb, const NewRuleNodes& made, std::vector<std::pair<Node, Node>>* const pairing)
        {
            Matcher            m{z};
            Matcher::Bijection ab;
            Matcher::Bijection ba;
            m.made    = &made;
            m.pairing = pairing;

            const std::vector<Node> ca(fa.objects.begin(), fa.objects.end());
            const std::vector<Node> cb(fb.objects.begin(), fb.objects.end());
            return m.match(own_subject(z, a, fa), own_subject(z, b, fb), ab, ba, 0, Matcher::Written::No, [&](Matcher::Bijection& ab1, Matcher::Bijection& ba1)
                           { return m.match_multiset(ca, cb, ab1, ba1, 0, Matcher::Written::Consequence, [](Matcher::Bijection&, Matcher::Bijection&)
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
            consequences.push_back(canon(z, o, 1, walk));
        std::sort(consequences.begin(), consequences.end());

        std::string out = canon(z, own_subject(z, rule, fs), 1, walk) + "=>";
        for (const auto& c : consequences)
            out += c + " ";
        return out;
    }

    bool rules_alpha_equivalent(const Zelph* const z, const Node a, const Node b, const NewRuleNodes& made)
    {
        if (a == b) return true;

        const FactStructure fa = rule_structure(z, a);
        const FactStructure fb = rule_structure(z, b);
        if (fa.predicate == 0 || fb.predicate == 0) return false;
        if (fa.objects.size() != fb.objects.size()) return false;
        return compare_rules(z, a, b, fa, fb, made, nullptr);
    }

    Node corresponding_part(const Zelph* const z, const Node a, const Node b, const Node part, const NewRuleNodes& made)
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
        // comparison performed by rules_alpha_equivalent, in identical order,
        // within the same budget, and using the same nodes from the new rule,
        // so it pairs `part` wherever that comparison found the rules alike. A
        // standalone search, which prioritized pairing `part` first, might
        // exhaust its budget on an incorrect pairing over sets that hold one
        // another before attempting the right one, and the part of the kept
        // rule was node 0.
        std::vector<std::pair<Node, Node>> pairing;
        if (!compare_rules(z, a, b, fa, fb, made, &pairing)) return 0;
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
