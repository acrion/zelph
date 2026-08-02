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
// Everything in zelph is hash-consed, so entering the same FACT twice is a
// no-op: the node IS its structure. Rules are the one exception, and not by
// accident. A rule contains variables, variables are freshly allocated per
// statement, and a node built from fresh variables is a fresh node -- so the
// second occurrence of a rule is a second rule that derives exactly what the
// first one derives, at exactly twice the unification cost. (Variables used
// to be shared by name, which made rules hash-cons like facts; that stopped
// working when rules gained nested terms, because the shared subterm no
// longer had an unambiguous parent.)
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
}
