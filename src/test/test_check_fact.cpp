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

#include <doctest/doctest.h>

#include "io/output.hpp"
#include "network/zelph.hpp"

#include <string>

using namespace zelph::network;

// ---------------------------------------------------------------------------
// check_fact / create_hash fast paths and the parse_relation memo prefilter:
// all three changes are semantics-neutral rewrites of hot probe machinery.
// These tests pin the exact-triple semantics across object-set shapes and
// STORAGE MODES (the hash must stay a pure function of the element set even
// though small sets now hash via direct sorted iteration and only large
// unordered sets keep the copy+sort normalization), plus the tricky
// parse_relation branches the prefilter must not disturb.
// ---------------------------------------------------------------------------

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }
} // namespace

TEST_CASE("check_fact: exact-triple semantics across object-set shapes")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node d  = z.node("d");
    const Node op = z.node("op");

    const Node f = z.fact(a, op, {b, c});
    CHECK(z.check_fact(a, op, {b, c}).is_known());
    CHECK(z.check_fact(a, op, {c, b}).is_known());          // insertion order irrelevant
    CHECK_FALSE(z.check_fact(a, op, {b}).is_known());       // subset: different node
    CHECK_FALSE(z.check_fact(a, op, {b, c, d}).is_known()); // superset: different node
    CHECK_FALSE(z.check_fact(b, op, {a, c}).is_known());    // roles swapped
    CHECK(z.check_fact(a, op, {b, c}).relation() == f);

    // Self-fact: subject == object draws no separate object edge; the
    // t == subject exemption in the probe must accept it.
    const Node self = z.fact(a, op, {a});
    CHECK(z.check_fact(a, op, {a}).is_known());
    CHECK(z.check_fact(a, op, {a}).relation() == self);
}

TEST_CASE("check_fact: hash is independent of object-set storage mode and iteration order")
{
    Zelph      z(null_handler());
    const Node s  = z.node("s");
    const Node op = z.node("op");

    // 200 objects: Set storage on both sides, with DIFFERENT insertion
    // orders -- unordered iteration differs, so this is red if the hash
    // ever becomes iteration-order-dependent (i.e. if the copy+sort
    // normalization for Set storage were dropped).
    adjacency_set     objs_up;
    adjacency_set     objs_down;
    std::vector<Node> nodes;
    for (int i = 0; i < 200; ++i)
        nodes.push_back(z.node("o" + std::to_string(i)));
    for (int i = 0; i < 200; ++i)
        objs_up.insert(nodes[static_cast<size_t>(i)]);
    for (int i = 199; i >= 0; --i)
        objs_down.insert(nodes[static_cast<size_t>(i)]);

    const Node big = z.fact(s, op, objs_up);
    CHECK(z.check_fact(s, op, objs_down).is_known());
    CHECK(z.check_fact(s, op, objs_down).relation() == big);

    // 50 objects: Vector storage (sorted payload) -- the direct-iteration
    // fast path; descending insertion order must land on the same node.
    adjacency_set mid_up;
    adjacency_set mid_down;
    for (int i = 0; i < 50; ++i)
        mid_up.insert(nodes[static_cast<size_t>(i)]);
    for (int i = 49; i >= 0; --i)
        mid_down.insert(nodes[static_cast<size_t>(i)]);

    const Node midf = z.fact(s, op, mid_down);
    CHECK(z.check_fact(s, op, mid_up).is_known());
    CHECK(z.check_fact(s, op, mid_up).relation() == midf);
}

TEST_CASE("parse_relation: memo prefilter keeps every branch's semantics")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");

    const Node f1 = z.fact(a, op, {b});
    CHECK(z.parse_relation(f1) == op);

    // Subject that is ITSELF a declared relation type: the bidirectional
    // exclusion must still pick the genuine predicate, not the subject.
    const Node op2 = z.node("op2");
    const Node f2  = z.fact(op, op2, {b});
    CHECK(z.parse_relation(f2) == op2);

    // subject == predicate: the relation==0 -> relation=subject fallback.
    const Node op3 = z.node("op3");
    const Node f3  = z.fact(op3, op3, {b});
    CHECK(z.parse_relation(f3) == op3);

    // A predicate auto-declared AFTER the memo was built (the calls above
    // built it) must be visible: red if the declaration hook fails to
    // invalidate the memo consumed here.
    const Node op4 = z.node("op4");
    const Node f4  = z.fact(a, op4, {b});
    CHECK(z.parse_relation(f4) == op4);
}

TEST_CASE("parse_relation_scoped: agrees with parse_relation across branch shapes under one lock scope")
{
    Zelph      z(null_handler());
    const Node a   = z.node("a");
    const Node b   = z.node("b");
    const Node op  = z.node("op");
    const Node f1  = z.fact(a, op, {b});
    const Node op2 = z.node("op2");
    const Node f2  = z.fact(op, op2, {b}); // subject is itself a declared relation type
    const Node op3 = z.node("op3");
    const Node f3  = z.fact(op3, op3, {b}); // subject == predicate fallback
    const Node sf  = z.fact(a, op, {a});    // self-fact: no separate object edge

    // Contract under test, too: the memo is fetched BEFORE the scope opens
    // (its lazy build takes the very locks the scope holds). NOTE: the
    // locking parse_relation must NOT be called while the scope is alive,
    // hence the hardcoded expected values instead of a direct comparison.
    const auto rel_types = z.relation_type_set();

    const Network::ReadScope scope = z.read_scope();
    CHECK(z.parse_relation_scoped(scope, *rel_types, f1) == op);
    CHECK(z.parse_relation_scoped(scope, *rel_types, f2) == op2); // bidirectional subject excluded
    CHECK(z.parse_relation_scoped(scope, *rel_types, f3) == op3); // relation==0 -> subject fallback
    CHECK(z.parse_relation_scoped(scope, *rel_types, sf) == op);
    CHECK(z.parse_relation_scoped(scope, *rel_types, a) == 0); // atom: no relation
    CHECK(scope.exists(f1));
    CHECK_FALSE(scope.exists(Node{0}));
}
