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

#pragma once

// The manner in which a rule reads a node at the place where the node stands.
//
// The rule identity (rule_identity.cpp) and the instantiation (container_plan
// in reasoning.cpp) both must read a rule's text exactly as the reasoner
// does. Each previously maintained its own copy of that reading, and those
// versions diverged: a named set constant over a rule's own collection was
// treated as a constant by the instantiation and as its members by the
// identity, so two rules writing two such constants were interpreted as one,
// and the rule rolled back lost what it derived. Both now invoke this
// function, as does the claim of a firing when it writes a set of conditions
// as it stands (Reasoning::enters_kept_list, once per set and consequence in
// a run).
//
// Each reading below constitutes one of the reasoner's own predicates, asked
// in the order the reasoner asks them: the id class first (a bit test),
// followed by the conjunction tag (one lookup), then a name or the
// memberships. The reasoner's hot readers do not call this function: they
// call the branch they need directly, ensuring their cost remains unchanged.
// A set of conditions is Zelph::condition_set_members
// (zelph_maintenance.cpp), which unify_condition and extract_bindings
// (unification.cpp), Reasoning::evaluate, the semi-naive index,
// collect_conditions, the strata, the negation test, the caches, prune and
// condition_elements call; a rule's own collection is Zelph::is_rule_template
// (zelph.cpp), which the unifier's set-pattern branch reads via
// Zelph::is_set_pattern. A change to a reading here must change those
// branches alike.

#include "zelph.hpp"

#include <vector>

namespace zelph::network
{
    // The location where a rule reads a node.
    enum class ReadAt
    {
        // Where the conditions of a rule are positioned as the firing reads
        // them: the `=>` subject of a rule, and every member of a set of
        // conditions read at that location.
        Conditions,

        // Everywhere else. Asked solely of a node devoid of fact structure (an
        // atom, a container, or a variable): a statement is read via its parts,
        // and this place would take a statement as a set constant.
        Term,

        // Where a firing writes a node as it stands: a term of a consequence
        // not enclosed within a `=>`, other than the object of a membership,
        // which the firing writes into. Asked solely of a node lacking fact
        // structure.
        Written,

        // The `=>` subject of a rule that the rule identity compares or
        // fingerprints, located at the top of that rule: never the subject of a
        // rule that the rule writes or quotes, nor of a rule pattern.
        OwnSubject
    };

    // What a rule reads a node as.
    enum class Reading
    {
        // Nothing decided here: a variable, at Conditions a statement or a
        // container that holds no set of conditions, at Written a container
        // lacking the tag, at OwnSubject a subject that is read at Term.
        Plain,

        // (Term) The node itself: an atom, a data collection, a value term or
        // data term, a value set pattern, a named set constant.
        Value,

        // (Conditions) A set of conditions. Its members reside at Conditions,
        // and Zelph::flatten_condition_sets reads each set exactly once.
        Conditions,

        // (Term, Written) A container bearing the conjunction tag: a set of
        // conditions within a term, which a firing either preserves or writes
        // into. When the firing writes it as it stands, its claim enters it and
        // asserts its members, just as a typed statement asserts what it holds.
        ConditionsInTerm,

        // (Written) A container bearing the conjunction tag that a firing
        // writes via its node: the set of conditions of a rule (a `=>`
        // subject), or a set constructed by a construction
        // (Zelph::is_rule_template). Its claim does not enter it, so its
        // members stay rule text.
        ConditionsByNode,

        // (Term) A collection of the rule's own text, untagged, regenerated for
        // every binding.
        OwnCollection,

        // (Term) An untagged set constant lacking a name: it is its members,
        // and it is reconstructed whenever one of them is modified.
        SetConstant,

        // (OwnSubject) A set consisting of a single member that only the
        // firing reads, apart from a tag it carries (read_container), and
        // that therefore is its member, ContainerReading::node.
        Condition
    };

    struct ContainerReading
    {
        Reading reading = Reading::Plain;

        // Reading::Conditions: whether the tag says how the members are
        // combined; without it, the set holds exactly one condition.
        bool tagged = false;

        // Reading::Condition: the member that the set reads as.
        Node node = 0;
    };

    // The subjects of the PartOf facts directed toward a container: its
    // members, the reconstruction node_to_string prints as `{...}`.
    std::vector<Node> container_members(const Zelph* z, Node n);

    namespace detail
    {
        // read_container at ReadAt::OwnSubject (container_reading.cpp).
        ContainerReading read_own_subject(const Zelph* z, Node n, Node rule);
    }

    // How a rule reads `n` at `at`. `members` (Conditions exclusively, may be
    // null) receives the members of a set of conditions as
    // Zelph::condition_set_members reads them. `rule` (OwnSubject exclusively)
    // is the `=>` fact whose subject `n` is.
    //
    // Conditions: no predicate, at least one member, and several members only
    // with the tag (Zelph::condition_set_members); the name is not asked.
    //
    // Term: the id decides whether a collection is part of the rule's own text
    // (Zelph::is_rule_template), the tag determines whether a container is a
    // set of conditions, and for a set constant its name determines whether it
    // is a constant: the instantiation keeps a named set constant as written,
    // so it is the node it is wherever it stands.
    //
    // Written: how the claim of a firing reads a set of conditions it writes
    // (Zelph::claim_nested, Reasoning::enters_kept_list). It enters a set
    // constant and a list the statement writes, and asserts their members; a
    // rule's set, or one constructed by a construction, constitutes rule text
    // it writes by its node, since asserting the members of a rule's set would
    // turn that rule's conditions into data. A set constant is entered even
    // when it is a rule's set: a firing that writes the set of
    // `(*{(a s b) (c t d)} ~ conjunction) => C` asserts `a s b` and `c t d`,
    // and one that writes the set of `(a s b, c t d) => C` asserts nothing.
    // The fact that these two spellings of a single rule are written
    // differently here reflects today's reading, not a settled one. The rule
    // identity asks it of two sets at such a place: read alike there, a list a
    // typed rule writes and a rule's set with the same members were considered
    // a single term, and the typed rule, rolled back, lost what its claim had
    // asserted.
    //
    // OwnSubject: a set of exactly one member reads as that member where the
    // membership is rule text (member_is_rule_text in unification.cpp: the
    // member holds a variable, or is a rule's set holding one) and the set
    // stands nowhere else in the rule, neither in a consequence nor inside its
    // member. Beyond a tag it carries (below), only the firing reads such a
    // set, and it reads it as its member (Zelph::condition_set_members). No
    // other reader can see it: a rule holding a variable is no candidate for a
    // rule pattern, nor for a variable bound at a rule's subject (is_rule_text
    // in unification.cpp); the membership answers nothing (the two filters
    // member_is_rule_text asks); and no firing writes a fact about the set, as
    // no consequence holds it. So `(X p Y) => C`, `{(X p Y)} => C`,
    // `@{(X p Y)} => C` and `(*{(X p Y)} ~ conjunction) => C` are one rule. A
    // change that makes any of these readers see the set must change this
    // reading as well. One reader is not covered: the tag `S ~ conjunction` of
    // a rule's set holding a variable answers a query (test_rule_patterns.cpp,
    // "a rule's condition set is data"), so a rule over the tag tells the
    // tagged spelling from the others. When entered after the rule in another
    // spelling, the tagged one is rolled back as its twin, and whatever a rule
    // over the tag would have derived from it is lost, including a fact over
    // named atoms where nothing else carries the tag: beside
    // `(S ~ conjunction) => (k tagged yes)`, the rule
    // `(*{(X p Y)} ~ conjunction) => (X q Y)` yields `k tagged yes` when
    // entered first or alone, but yields nothing when `(X p Y) => (X q Y)` was
    // entered before it. A ground member is not read through: a rule pattern
    // tells `{(a p b)} => C` from `(a p b) => C`, and the membership of
    // `(a p b)` answers a query. The member itself is read at Term: a set of
    // one condition within it is not read through.
    inline ContainerReading read_container(const Zelph* const z, const Node n, const ReadAt at, adjacency_set* const members = nullptr, const Node rule = 0)
    {
        if (n == 0 || Zelph::is_var(n)) return {};
        if (at == ReadAt::OwnSubject) return detail::read_own_subject(z, n, rule);

        const auto tagged = [z, n]
        { return z->check_fact(n, z->core.IsA, {z->core.Conjunction}).is_known(); };

        if (at == ReadAt::Conditions)
        {
            adjacency_set  read;
            adjacency_set& out = members != nullptr ? *members : read;
            if (!z->condition_set_members(n, out)) return {};
            return {Reading::Conditions, out.size() > 1 || tagged()};
        }

        if (at == ReadAt::Written)
        {
            if (!tagged()) return {};
            if (Zelph::is_hash(n)) return {Reading::ConditionsInTerm};
            if (z->is_rule_template(n)) return {Reading::ConditionsByNode};
            for (const Node rel : z->get_right(n))
            {
                if (z->parse_relation(rel) != z->core.Causes) continue;
                adjacency_set consequences;
                if (z->parse_fact(rel, consequences, 0) == n) return {Reading::ConditionsByNode};
            }
            return {Reading::ConditionsInTerm};
        }

        if (!Zelph::is_hash(n))
        {
            const bool own = z->is_rule_template(n);
            if (tagged()) return {Reading::ConditionsInTerm};
            return {own ? Reading::OwnCollection : Reading::Value};
        }

        if (tagged()) return {Reading::ConditionsInTerm};
        return {z->is_named_any(n) ? Reading::Value : Reading::SetConstant};
    }
}
