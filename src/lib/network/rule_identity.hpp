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
// What "structurally identical" covers is deliberately exactly what the
// reasoner interprets (see collect_conditions / Reasoning::evaluate): set
// membership, the Conjunction and Negation tags, and each condition's
// subject / predicate / object set. Predicate identity is what separates a
// != guard and an ≈ neural condition from an ordinary pattern, so those
// need no special case. Consequently "alpha-equivalent" here means
// "operationally indistinguishable to the engine", which is the property
// that makes skipping the second rule safe.

#include "network_types.hpp"

#include <string>

namespace zelph::network
{
    class Zelph;

    // Fingerprint of a rule that ignores WHICH variable nodes it uses.
    // Alpha-equivalent rules always share it; sharing it does not imply
    // alpha-equivalence (`(A p B) (B p C)` and `(A p B) (C p D)` collide),
    // which is why it is a filter in front of the real test and never a
    // decision on its own. Empty if `rule` is not a rule.
    std::string rule_shape(const Zelph* z, Node rule);

    // The decision: same rule up to a bijective renaming of the variables.
    bool rules_alpha_equivalent(const Zelph* z, Node a, Node b);

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
}
