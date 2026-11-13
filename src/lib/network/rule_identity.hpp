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

// Recognising a rule one has already got.
//
// In zelph, facts are hash-consed, meaning that entering the same FACT twice
// is a no-op: the node IS its structure. Rules stand as the sole exception,
// and this is intentional. A rule contains variables, which are freshly
// allocated for each statement, and a node built from these newly created
// variables constitutes a fresh node -- thus, the second occurrence of a
// rule is a second rule that derives exactly what the first one does, but at
// exactly twice the unification cost. (Previously, variables were shared by
// name, which caused rules to hash-cons like facts; however, this approach
// ceased to function when rules began incorporating nested terms, as the
// shared subterm then lacked an unambiguous parent.)
//
// The pair below restores the missing identity WITHOUT touching node
// identity: rule_shape() is a cheap filter, rules_alpha_equivalent() the
// decision. Two rules are the same rule iff they are structurally identical
// under SOME bijection of their variables -- alpha-equivalence, as in the
// lambda calculus.
//
// What "structurally identical" covers is what the reasoner interprets (see
// collect_conditions / Reasoning::evaluate): membership in a set, the
// Conjunction tag, and the subject / predicate / object set for each condition.
// Predicate identity is what separates a != guard, an ≈ neural condition and a
// negation from an ordinary pattern, so those need no special case. In a term,
// the subject of a rule quoted in a consequence or named or quoted by a
// condition included, a set of conditions is read by its node. Two such sets
// that are not named are compared by their members, as terms, where the new
// rule's statement has just made its set and takes it back with the rule
// (NewRuleNodes), and always during the claim search of a construction, which
// involves no new rule. Every container met in a term corresponds to one
// container of the other rule at every place. Thus, two rules that a comparison
// with a new rule determines to be alike derive the same outcome, up to
// renaming of elements that only the new rule's statement made, which is the
// property enabling the safety of skipping the second rule.

#include "adjacency_set.hpp"
#include "network_types.hpp"

#include <zelph_export.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zelph::network
{
    class Zelph;

    // The fingerprint of a rule that ignores WHICH variable nodes it uses.
    // Alpha-equivalent rules share this fingerprint, except when the rendering of a
    // rule over cycles exhausts its allocated budget of rendered positions (see
    // Matcher in rule_identity.cpp); sharing this fingerprint does not imply
    // alpha-equivalence (`(A p B) (B p C)` and `(A p B) (C p D)` collide, and so do
    // two rules over distinct sets of conditions in a term that are alike in their
    // members), which is why it functions as a filter prior to the real test and
    // never serves as a standalone decision. Empty if `rule` is not a rule.
    std::string rule_shape(const Zelph* z, Node rule);

    // Which nodes the statement that built the new rule of a comparison, its
    // first rule, created and reclaims when the rule is determined to be a
    // duplicate. In a term, a set of conditions is treated as a constant by the
    // reasoner: a firing keeps its node and records facts concerning that node.
    // The comparison reads the subject of a rule that a condition names or
    // quotes as such a term as well (subject_role). Two distinct sets of
    // conditions there are considered the same term solely
    // when the new rule's set is one of those nodes and neither set bears a
    // name: nothing beyond the rule retains a set that the rollback reclaims,
    // thus omitting the new rule results in no loss. A construction holds none
    // of its nodes as the new rule's own: it finds a set it reconstructs via
    // its members (instantiate_container), so a second rule from the
    // same firing might reuse a set created by the first, and upon rolling back
    // the first rule and reclaiming its set, the second rule generated a set of
    // its own, and a join over the shared set was lost. Empty when neither rule
    // is new (the claim search of a construction); two distinct sets of
    // conditions that are not named then match by their members, and the rule
    // the search identifies may refer to a different set than the rule it was
    // searching for.
    using NewRuleNodes = std::function<bool(Node)>;

    // The decision: identical rule up to a bijective renaming of the variables,
    // `a` representing the new rule when the comparison has one and `made` what
    // its statement created. A comparison of rules over cycles that exhausts
    // its budget or depletes the stack of its thread yields no (refer to
    // Matcher in rule_identity.cpp).
    bool rules_alpha_equivalent(const Zelph* z, Node a, Node b, const NewRuleNodes& made);

    // The part of rule `b` occupying the position where `part` stands in rule
    // `a`, given two alpha-equivalent rules: the condition, the `=>` predicate,
    // or the consequence that the bijection pairs with `part`. 0 if `part` is
    // none of these or the rules do not match. `made` is what the comparison
    // that found `b` was given.
    Node corresponding_part(const Zelph* z, Node a, Node b, Node part, const NewRuleNodes& made);

    // Whether a rule's own collection or a conjunction set appears
    // beneath the fact or set constant `n`, accessed via facts and set
    // constants: the part of a rule's text that the template-variable
    // store omits, and that an instantiation reconstructs even though no
    // variable from the store is substituted.
    bool rule_text_below(const Zelph* z, Node n);

    // Whether the fact `rel` constitutes a statement of a rule or forms part
    // of one, nested at any depth: it serves as the condition or a
    // consequence within a `=>` fact, as a member of a conjunction set or of
    // a rule's own collection, or as the subject or an object of a fact, or
    // as a member of a set constant, that is one of these.
    bool is_rule_statement(const Zelph* z, Node rel);

    // Why a rule cannot be written, when one of the facts produced by its
    // consequences -- a consequence itself or a fact nested within one --
    // features a predicate that is a variable not bound by any condition;
    // empty otherwise. No such predicate is provided when the rule fires: a
    // single consequence of this nature yielded nothing, and a variable that
    // also stood as a subject or an object got a new node, which then stood
    // as a relation type unnamed by anyone and printed as "->". A rule
    // located within a consequence is not entered (a generator binds the
    // variables of its inner rule during the construction of that rule), and
    // a negated condition binds nothing.
    //
    // Before writing anything, each rule builder queries it: the builders in
    // Janet, and zelph_rule in the C ABI.
    std::string unbound_predicate(const Zelph* z, std::vector<Node> conditions, const adjacency_set& consequences);

    // The number of facts that is_rule_statement has inspected in this
    // process. Each climb visits every fact that holds the node it starts
    // from, and data may hold a pattern without restriction, meaning a
    // caller that climbs once per member it outputs bears the cost of each
    // such fact across every rendering. It is independent of hardware,
    // allowing a test to hold that shape without requiring a quiet machine.
    ZELPH_EXPORT uint64_t rule_statement_climb_steps();

    // The number of times rule_shape and the comparison of two rules have read
    // a node -- as single_condition and classify read it -- or the members of a
    // container during this process. Beginning with its first cycle, a
    // rendering or a comparison reads each node once in every role it meets the
    // node in, and the members of each container once; before that, it reads
    // them at every encounter. Similar to the climb steps, it lets a test hold
    // that shape without requiring a quiet machine.
    ZELPH_EXPORT uint64_t rule_identity_reads();

    // The number of positions rule_shape has rendered and the number of pairs
    // the comparison of two rules has compared during this process, including
    // those at the depth cap and past the budget, which terminate immediately.
    // Once a rendering or a comparison has met a cycle, it takes at most its
    // budget of positions or pairs before every further one terminates
    // immediately.
    ZELPH_EXPORT uint64_t rule_identity_steps();
}
