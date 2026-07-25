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
#include "network/fact_structure.hpp"
#include "network/zelph.hpp"

#include <string>

using namespace zelph::network;

// ---------------------------------------------------------------------------
// Per-node fact-structure cache invalidation (Zelph::fact ->
// invalidate_fact_structures_for). The wholesale clear on every new fact is
// gone, so these tests deliberately interleave CACHE-WARMING reads with fact
// creation -- interleavings that could not exist before (the cache was empty
// after every fact()). Each case re-reads warmed entries after the graph
// grew around them and pins that the reading is still exactly the genuine
// triple. The suite-wide `.semi-naive check` net covers the same property
// end-to-end on the arithmetic workloads.
// ---------------------------------------------------------------------------

namespace
{
    zelph::io::OutputHandler null_handler()
    {
        return [](const zelph::io::OutputEvent&) {};
    }

    bool has_reading(const FactStructureList& structs, const Node subject, const Node object)
    {
        for (const auto& fs : structs)
            if (fs.subject == subject && fs.objects.count(object) == 1) return true;
        return false;
    }
} // namespace

TEST_CASE("fact cache: warm entries stay genuine across same-predicate parent growth")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node q  = z.node("q");
    const Node op = z.node("op");

    const Node inner = z.fact(a, op, {b});

    // Warm the cache for inner BEFORE any parent exists.
    {
        const auto s = get_fact_structures(&z, inner, 1);
        REQUIRE(s->size() == 1);
        CHECK((*s)[0].subject == a);
        CHECK((*s)[0].predicate == op);
        CHECK((*s)[0].objects.count(b) == 1);
    }

    // Same-predicate parent: mid <-> inner is bidirectional -- the exact
    // shape whose spurious reading hash verification prunes. inner sits in
    // the invalidation set (it is mid's subject), so the re-read must
    // reflect the verified fresh reconstruction.
    const Node mid = z.fact(inner, op, {c});
    CHECK(has_reading(*get_fact_structures(&z, mid, 1), inner, c));
    {
        const auto s = get_fact_structures(&z, inner, 1);
        REQUIRE(s->size() == 1);
        CHECK((*s)[0].subject == a);
        CHECK((*s)[0].objects.count(b) == 1);
    }

    // Two-level variant (the handoff's staleness trap): mid's entry is now
    // warm; outer grows the neighborhood one level further up.
    const Node outer = z.fact(mid, op, {q});
    CHECK(has_reading(*get_fact_structures(&z, outer, 1), mid, q));
    {
        const auto s = get_fact_structures(&z, mid, 1);
        CHECK(has_reading(*s, inner, c));
        CHECK_FALSE(has_reading(*s, outer, q)); // the spurious parent reading
    }
    {
        const auto s = get_fact_structures(&z, inner, 1);
        REQUIRE(s->size() == 1);
        CHECK((*s)[0].subject == a);
    }
}

TEST_CASE("fact cache: warm self-fact reading survives new consumers of its subject")
{
    Zelph      z(null_handler());
    const Node a      = z.node("a");
    const Node b      = z.node("b");
    const Node c      = z.node("c");
    const Node op     = z.node("op");
    const Node op2    = z.node("op2");
    const Node marker = z.node("marker");

    const Node term = z.fact(a, op, {b});
    const Node self = z.fact(term, marker, {term});

    // Warm: a self-fact stores no separate object edge; reconstruction
    // repairs objects to {subject}.
    {
        const auto s = get_fact_structures(&z, self, 1);
        REQUIRE(!s->empty());
        CHECK(has_reading(*s, term, term));
    }

    // A second consumer of term grows term's bidirectional neighborhood;
    // self is a bidirectional neighbor of term (the new fact's subject)
    // and must be invalidated with it.
    const Node other = z.fact(term, op2, {c});
    CHECK(has_reading(*get_fact_structures(&z, other, 1), term, c));
    CHECK(has_reading(*get_fact_structures(&z, self, 1), term, term));
}

TEST_CASE("fact cache: relation-type declarations take the full-clear path and stay correct")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");

    const Node f = z.fact(a, op, {b});
    CHECK(has_reading(*get_fact_structures(&z, f, 1), a, b)); // warm

    // Declaring a NEW relation type is the one growth event that can
    // change predicate detection for ANY cached entry.
    const Node p2 = z.node("p2");
    z.fact(p2, z.core.IsA, {z.core.RelationTypeCategory});
    CHECK(has_reading(*get_fact_structures(&z, f, 1), a, b));

    const Node g = z.fact(a, p2, {b});
    CHECK(has_reading(*get_fact_structures(&z, g, 1), a, b));
}

TEST_CASE("fact cache: pathological bidirectional neighborhoods degrade to the full clear, staying correct")
{
    Zelph      z(null_handler());
    const Node h  = z.node("h");
    const Node op = z.node("op");

    // 300 facts with h as SUBJECT: every relation node is bidirectional
    // with h, exceeding stale_budget on the next fact() involving h.
    for (int i = 0; i < 300; ++i)
        z.fact(h, op, {z.node("o" + std::to_string(i))});

    const Node probe = z.fact(z.node("x"), op, {z.node("y")});
    CHECK(has_reading(*get_fact_structures(&z, probe, 1), z.node("x"), z.node("y"))); // warm

    const Node big = z.fact(h, op, {z.node("z")});
    CHECK(has_reading(*get_fact_structures(&z, big, 1), h, z.node("z")));
    CHECK(has_reading(*get_fact_structures(&z, probe, 1), z.node("x"), z.node("y")));
}

TEST_CASE("fact cache: hit/miss statistics accumulate only while logging is active")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");
    const Node f  = z.fact(a, op, {b});

    // Logging off: reads and invalidations leave the statistics untouched
    // (measurement purity -- same gating as every profiler counter).
    get_fact_structures(&z, f, 1);
    {
        const auto s = z.fs_cache_stats();
        CHECK(s.hits == 0);
        CHECK(s.misses == 0);
        CHECK(s.full_clears == 0);
        CHECK(s.stale_erased == 0);
    }

    z.set_logging(-1); // counter-only mode

    // g's creation invalidates a's bidirectional neighborhood; the entry
    // for f (warmed above, subject a) is the single warm victim.
    const Node g = z.fact(a, op, {z.node("c")});
    get_fact_structures(&z, g, 1); // cold: reconstruction + store
    get_fact_structures(&z, g, 1); // warm: hit
    {
        const auto s = z.fs_cache_stats();
        CHECK(s.misses == 1);
        CHECK(s.hits == 1);
        CHECK(s.stale_erased == 1);
        CHECK(s.full_clears == 0); // op was already declared: per-node path only
    }

    // A relation-type declaration takes the wholesale path and empties the
    // cache: the next read of g is a miss again.
    z.fact(z.node("p9"), z.core.IsA, {z.core.RelationTypeCategory});
    {
        const auto s = z.fs_cache_stats();
        CHECK(s.full_clears == 1);
    }
    get_fact_structures(&z, g, 1);
    CHECK(z.fs_cache_stats().misses == 2);
}

TEST_CASE("fact cache: hits share one immutable list and held pointers are stable snapshots")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node c  = z.node("c");
    const Node op = z.node("op");
    const Node f  = z.fact(a, op, {b});

    const auto s1 = get_fact_structures(&z, f, 1);
    const auto s2 = get_fact_structures(&z, f, 1);
    // THE point of the shared-entry change: a hit hands out the SAME list,
    // not a deep copy. Red with value-copy cache entries.
    CHECK(s1.get() == s2.get());
    REQUIRE(s1->size() == 1);
    CHECK(has_reading(*s1, a, b));

    // Snapshot semantics: invalidate f's entry (new fact on subject a),
    // force a fresh reconstruction, and pin that the OLD pointer is still
    // a valid, unchanged, genuine reading.
    const Node g = z.fact(a, op, {c});
    (void)g;
    const auto s3 = get_fact_structures(&z, f, 1);
    CHECK(s3.get() == s1.get()); // the genuine store hands out ONE immutable
                                 // list forever; invalidation only dropped
                                 // the fs_cache COPY of it, which this probe
                                 // silently re-promoted
    CHECK(has_reading(*s1, a, b));
    CHECK(has_reading(*s3, a, b));
}

TEST_CASE("fact cache: structureless classes bypass the cache; walk-empty hash nodes still cache")
{
    Zelph      z(null_handler());
    const Node atom = z.node("plain-atom");
    const Node V    = z.var();

    z.set_logging(-1);

    // Atoms and variables answer lock-free with the shared empty
    // instance: no cache probe, no counter movement. Red if the bit-test
    // gate is missing (the old behaviour counted a miss and a hit here).
    const auto e1 = get_fact_structures(&z, atom, 1);
    const auto e2 = get_fact_structures(&z, atom, 1);
    CHECK(e1->empty());
    CHECK(e1.get() == e2.get());
    CHECK(get_fact_structures(&z, V, 1)->empty());
    {
        const auto s = z.fs_cache_stats();
        CHECK(s.hits == 0);
        CHECK(s.misses == 0);
    }

    // subject == predicate facts are the surviving walk-to-empty HASH
    // class: excluded from the genuine store, reconstructed empty by the
    // walk, and cached like any reconstruction result.
    const Node b   = z.node("b");
    const Node op3 = z.node("op3");
    const Node f3  = z.fact(op3, op3, {b});
    const auto w1  = get_fact_structures(&z, f3, 1); // miss: walk + store
    CHECK(w1->empty());
    const auto w2 = get_fact_structures(&z, f3, 1); // hit on the stored entry
    CHECK(w1.get() == w2.get());
    const auto s = z.fs_cache_stats();
    CHECK(s.misses == 1);
    CHECK(s.hits == 1);
    CHECK(z.genuine_stats().walks == 1);
}

TEST_CASE("fact cache: relation-type memo is refreshed by new declarations")
{
    Zelph      z(null_handler());
    const Node a  = z.node("a");
    const Node b  = z.node("b");
    const Node op = z.node("op");

    const Node f = z.fact(a, op, {b});
    CHECK(has_reading(*get_fact_structures(&z, f, 1), a, b)); // builds and uses the memo

    // A fact with a FRESH atom predicate auto-declares it (op2 ~ ->). Red
    // without the invalidation hook: reconstruction of g would consult the
    // pre-op2 memo, find no predicate, and cache an empty reading.
    const Node op2 = z.node("op2");
    const Node g   = z.fact(a, op2, {b});
    CHECK(has_reading(*get_fact_structures(&z, g, 1), a, b));
    CHECK(z.parse_relation(g) == op2);

    // The pre-existing predicate keeps working through the rebuilt memo.
    const Node h = z.fact(b, op, {a});
    CHECK(has_reading(*get_fact_structures(&z, h, 1), b, a));
}
