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
#include "network/reasoning.hpp"
#include "network/zelph.hpp"

using namespace zelph::network;

// ---------------------------------------------------------------------------
// Variable-closure flag (Zelph::var_in_closure): the O(1) replacement for
// the reconstruction-based contains_variable_deep walk. Pins: (1) exact
// flag semantics across positions and depths, maintained bottom-up in
// fact(); (2) the authoritative-bit fallback -- after a path that bypasses
// triple-level construction (trusted import), queries must answer via the
// historical walk, including for templates created AFTER the switch (red
// if the query consulted the now-incomplete flag store regardless).
// End-to-end template rejection stays pinned by the existing suite (junk-
// fact regressions in test_seminaive.cpp), which now runs through the flag.
// ---------------------------------------------------------------------------

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }
} // namespace

TEST_CASE("var_in_closure: exact flag across positions and depths")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");
    const Node V  = z.var();

    CHECK_FALSE(z.var_in_closure(a));
    CHECK(z.var_in_closure(V));
    CHECK_FALSE(z.var_in_closure(0));

    const Node data = z.fact(a, op, {b});
    CHECK_FALSE(z.var_in_closure(data));

    const Node tmpl = z.fact(V, op, {b}); // var in subject position
    CHECK(z.var_in_closure(tmpl));

    // The flag must propagate bottom-up through parents (depths 2 and 3).
    const Node mid = z.fact(tmpl, op, {a});
    CHECK(z.var_in_closure(mid));
    const Node outer = z.fact(mid, op, {b});
    CHECK(z.var_in_closure(outer));

    const Node t2 = z.fact(a, op, {V}); // var in object position
    CHECK(z.var_in_closure(t2));
    const Node t3 = z.fact(a, V, {b}); // variable relation
    CHECK(z.var_in_closure(t3));

    // Nesting DATA inside data stays unflagged.
    const Node data2 = z.fact(data, op, {b});
    CHECK_FALSE(z.var_in_closure(data2));
}

TEST_CASE("var_in_closure: trusted import disarms the flag store; walk fallback stays correct")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node op = z.node("op");
    const Node V  = z.var();

    const Node tmpl = z.fact(V, op, {b});
    const Node data = z.fact(a, op, {b});

    // Trusted import funnels through invalidate_fact_structures_cache,
    // which clears the authoritative bit.
    z.fact_import_trusted_single_object(a, op, c);

    CHECK(z.var_in_closure(tmpl)); // answered by the reconstruction walk
    CHECK_FALSE(z.var_in_closure(data));
    CHECK(z.var_in_closure(V));
    CHECK_FALSE(z.var_in_closure(a));

    // Created AFTER the switch: maintenance is skipped, so ONLY the walk
    // can know this template -- red if the query still trusted the store.
    const Node tmpl2 = z.fact(V, op, {c});
    CHECK(z.var_in_closure(tmpl2));
}

namespace
{
    std::unordered_set<Node> vars_of(Zelph& z, const Node n)
    {
        std::unordered_set<Node> vs;
        std::vector<Node>        history;
        collect_variables(&z, n, vs, 1, history);
        return vs;
    }
} // namespace

TEST_CASE("template vars: exact variable sets across positions and depths")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");
    const Node V  = z.var();
    const Node W  = z.var();

    z.set_logging(-1);

    const Node data = z.fact(a, op, {b});
    CHECK(vars_of(z, data).empty());
    CHECK(vars_of(z, a).empty());
    CHECK(vars_of(z, V).count(V) == 1);

    const Node tmpl = z.fact(V, op, {b});
    {
        const auto vs = vars_of(z, tmpl);
        CHECK(vs.size() == 1);
        CHECK(vs.count(V) == 1);
    }

    // Union across positions, propagated bottom-up through parents.
    const Node two = z.fact(tmpl, op, {W});
    {
        const auto vs = vars_of(z, two);
        CHECK(vs.size() == 2);
        CHECK(vs.count(V) == 1);
        CHECK(vs.count(W) == 1);
    }
    const Node deep = z.fact(two, op, {a});
    {
        const auto vs = vars_of(z, deep);
        CHECK(vs.count(V) == 1);
        CHECK(vs.count(W) == 1);
    }

    CHECK(z.template_vars_stats().walks == 0); // never fell back to the walk
}

TEST_CASE("template vars: trusted import disarms; walk keeps sets identical")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node op = z.node("op");
    const Node V  = z.var();

    const Node tmpl   = z.fact(V, op, {b});
    const Node parent = z.fact(tmpl, op, {a});
    CHECK(vars_of(z, parent).count(V) == 1); // via store

    z.fact_import_trusted_single_object(a, op, c); // funnel: disarm + clear

    z.set_logging(-1);
    CHECK(vars_of(z, parent).count(V) == 1); // via reconstruction walk now
    CHECK(vars_of(z, z.fact(a, op, {b})).empty());

    // Created AFTER the disarm: only the walk can answer -- red if the
    // query still trusted the (empty) store.
    const Node tmpl2 = z.fact(V, op, {c});
    CHECK(vars_of(z, tmpl2).count(V) == 1);

    CHECK(z.template_vars_stats().hits == 0);
    CHECK(z.template_vars_stats().walks > 0);
}
