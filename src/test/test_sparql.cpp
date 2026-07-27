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

#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace zelph::test;

// The test cases below are modelled on 14 real-world SPARQL queries from the
// Wikidata Ontology Cleaning Task Force ("example query (N)" in comments).
// Several of them fail or time out on public SPARQL endpoints (QLever,
// Blazegraph); the miniatures here pin down the semantics zelph must provide
// to solve them. Qualifier-style structures (p:, pq:, wikibase:rank) are
// simulated as ordinary triples - the simulated shape is exactly what
// .wikidata-qualifiers materializes from real dumps.

namespace
{
    void load_sparql(const zelph::console::Interactive& interactive)
    {
        // Resolved via the standard library next to the test binary
        // (mirrored there by CMake), so the test is hermetic: it works in
        // the build tree and in packaged installations alike.
        interactive.process(".import sparql");
    }

    // Feed a SPARQL query through the keyword mechanism:
    // "sparql" line, query lines, terminating empty line.
    void run_sparql(const zelph::console::Interactive& interactive, const std::string& query)
    {
        interactive.process("sparql");
        std::istringstream iss(query);
        std::string        line;
        while (std::getline(iss, line))
        {
            interactive.process(line);
        }
        interactive.process("");
    }

    // Base test graph (Wikidata-flavored IDs as plain node names).
    // wd:/wdt: terms resolve explicitly in the "wikidata" language, so the
    // test data must be created there as well.
    void setup_base_graph(const zelph::console::Interactive& interactive)
    {
        process_lines(interactive, R"(
    .lang wikidata
    Q1 P31 Q5
    Q2 P31 Q5
    Q3 P31 Q6
    Q5 P279 Q50
    Q6 P279 Q50
    Q50 P279 Q500
    Q1 P21 Q6581097
    )");
    }

    // Disjointness graph in the exact shape .wikidata-qualifiers
    // materializes from real dumps:
    //   QC has a disjoint-union statement QL (main value QU) with the
    //   pair (QD1, QD2). QC2 has an equivalent statement QL2, but QL2 is
    //   deprecated. QX is a subclass of both QD1 and QD2 (the violation).
    //   QI is an instance of QX.
    void setup_disjointness_graph(const zelph::console::Interactive& interactive)
    {
        process_lines(interactive, R"(
        .lang wikidata
        QC p:P2738 QL
        QL ps:P2738 QU
        QL pq:P11260 QD1
        QL pq:P11260 QD2
        QC2 p:P2738 QL2
        QL2 wikibase:rank wikibase:DeprecatedRank
        QL2 pq:P11260 QD1
        QL2 pq:P11260 QD2
        QX P279 QD1
        QX P279 QD2
        QI P31 QX
        )");
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// Script loading and keyword registration
// ---------------------------------------------------------------------------

TEST_CASE("sparql: script import registers the keyword")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        CHECK(any_output_contains(collector, "SPARQL subset loaded")); });
}

// ---------------------------------------------------------------------------
// Basic graph patterns
// ---------------------------------------------------------------------------

TEST_CASE("sparql: single triple pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . }");

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "Q2"));
        CHECK_FALSE(any_output_contains(collector, "Q3"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

TEST_CASE("sparql: join of two triple patterns")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?x WHERE {
  ?x wdt:P31 wd:Q5 .
  ?x wdt:P21 wd:Q6581097 .
})");

        CHECK(any_output_contains(collector, "Q1"));
        CHECK_FALSE(any_output_contains(collector, "Q2"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: semicolon property list")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 ; wdt:P21 wd:Q6581097 . }");

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: empty result set")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?x WHERE { ?x wdt:P31 wd:Q999999 . }");

        CHECK(any_output_contains(collector, "No results.")); });
}

TEST_CASE("sparql: variable in predicate position")
{
    // The zelph equivalent of QLever's ql:has-predicate (example query 1).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?p WHERE { wd:Q1 ?p wd:Q5 . }");

        CHECK(any_output_contains(collector, "P31")); });
}

// ---------------------------------------------------------------------------
// Property paths (transitive closures)
// ---------------------------------------------------------------------------

TEST_CASE("sparql: transitive path + with bound subject")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?c WHERE { wd:Q5 wdt:P279+ ?c . }");

        CHECK(any_output_contains(collector, "Q50"));
        CHECK(any_output_contains(collector, "Q500"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

TEST_CASE("sparql: reflexive path * includes the start node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?c WHERE { wd:Q5 wdt:P279* ?c . }");

        CHECK(any_output_contains(collector, "Q5"));
        CHECK(any_output_contains(collector, "Q50"));
        CHECK(any_output_contains(collector, "Q500"));
        CHECK(any_output_contains(collector, "-- 3 result(s) --")); });
}

TEST_CASE("sparql: transitive path + with bound object (backward closure)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?i WHERE { ?i wdt:P279+ wd:Q500 . }");

        // Q50, Q5 and Q6 all reach Q500 via one or more P279 steps.
        CHECK(any_output_contains(collector, "Q50"));
        CHECK(any_output_contains(collector, "Q5"));
        CHECK(any_output_contains(collector, "Q6"));
        CHECK(any_output_contains(collector, "-- 3 result(s) --")); });
}

TEST_CASE("sparql: reflexive path * with bound object (example query 8)")
{
    // ?i wdt:P279* wd:Q24027515 - the "sixth-order classes" query shape.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?i WHERE { ?i wdt:P279* wd:Q500 . }");

        // Q500 itself plus Q50, Q5, Q6.
        CHECK(any_output_contains(collector, "Q500"));
        CHECK(any_output_contains(collector, "Q50"));
        CHECK(any_output_contains(collector, "Q6"));
        CHECK(any_output_contains(collector, "-- 4 result(s) --")); });
}

TEST_CASE("sparql: sequence path P31/P279*")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?x WHERE { ?x wdt:P31/wdt:P279* wd:Q50 . }");

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "Q2"));
        CHECK(any_output_contains(collector, "Q3"));
        CHECK(any_output_contains(collector, "-- 3 result(s) --")); });
}

TEST_CASE("sparql: three-element sequence path with bound object (example queries 9-14)")
{
    // ?i wdt:P279*/wdt:P31/wdt:P279* wd:QM - the "class order" query shape.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        process_lines(interactive, R"(
.lang wikidata
QA P279 QM
QA P31 QM
QB P279 QM
)");
        collector.clear();

        run_sparql(interactive, "SELECT ?i WHERE { ?i wdt:P279*/wdt:P31/wdt:P279* wd:QM . }");

        // Only QA has an instance-of step in its path to QM.
        CHECK(any_output_contains(collector, "QA"));
        CHECK_FALSE(any_output_contains(collector, "QB"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: classes without order via step reordering (example query 7)")
{
    // Both conditions share the same two variables; the first condition is a
    // both-ends-unbound * step on its own, which becomes executable only
    // after the second condition has bound ?class and ?meta. This is the
    // query that fails even on dedicated QLever hardware with a 10 minute
    // timeout.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        process_lines(interactive, R"(
.lang wikidata
QA P279 QM
QA P31 QM
QB P279 QM
)");
        collector.clear();

        run_sparql(interactive, R"(SELECT ?class ?meta WHERE {
  ?class wdt:P279* ?meta .
  ?class wdt:P279*/wdt:P31/wdt:P279* ?meta .
})");

        CHECK(any_output_contains(collector, "QA QM"));
        CHECK_FALSE(any_output_contains(collector, "QB"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: both-unbound * single step is rejected")
{
    // A standalone  ?x P279* ?y  would trivially relate every node to
    // itself ("ludicrous results" in the words of example queries 12/13).
    // It must be rejected until the native closure engine can handle it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        interactive.process("sparql");
        interactive.process("SELECT ?x ?y WHERE { ?x wdt:P279* ?y . }");
        CHECK_THROWS_AS(interactive.process(""), std::runtime_error); });
}

// ---------------------------------------------------------------------------
// DISTINCT
// ---------------------------------------------------------------------------

TEST_CASE("sparql: DISTINCT collapses duplicate projections")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        // Both ends of the + path are unbound: the evaluator enumerates all
        // subjects carrying P279 and expands their closures. Without
        // DISTINCT this yields 5 rows; the distinct classes are Q50, Q500.
        run_sparql(interactive, "SELECT DISTINCT ?class WHERE { ?i wdt:P279+ ?class . }");

        CHECK(any_output_contains(collector, "Q50"));
        CHECK(any_output_contains(collector, "Q500"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

// ---------------------------------------------------------------------------
// OPTIONAL / MINUS / UNION
// ---------------------------------------------------------------------------

TEST_CASE("sparql: OPTIONAL keeps unmatched rows")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?x ?g WHERE {
  ?x wdt:P31 wd:Q5 .
  OPTIONAL { ?x wdt:P21 ?g }
})");

        // Q1 row carries the optional binding, Q2 row survives without it.
        CHECK(any_output_contains(collector, "Q1 Q6581097"));
        CHECK(any_output_contains(collector, "Q2"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

TEST_CASE("sparql: OPTIONAL rdfs:label with lang filter keeps rows (label idiom)")
{
    // The OPTIONAL { ?x rdfs:label ?l . FILTER (lang(?l) = "en") } idiom
    // from example queries (1) and (2). zelph models labels through its
    // language system, not as triples, so the optional pattern binds
    // nothing - but the rows must survive. Labels appear in the output via
    // zelph/name instead.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?x ?label WHERE {
  ?x wdt:P31 wd:Q5 .
  OPTIONAL { ?x rdfs:label ?label . FILTER ( lang(?label) = "en" ) }
})");

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "Q2"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

TEST_CASE("sparql: MINUS removes matching rows")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?x WHERE {
  ?x wdt:P31 wd:Q5 .
  MINUS { ?x wdt:P21 wd:Q6581097 }
})");

        CHECK(any_output_contains(collector, "Q2"));
        CHECK_FALSE(any_output_contains(collector, "Q1\t"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: UNION combines group patterns")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?x WHERE {
  { ?x wdt:P31 wd:Q5 . } UNION { ?x wdt:P31 wd:Q6 . }
})");

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "Q2"));
        CHECK(any_output_contains(collector, "Q3"));
        CHECK(any_output_contains(collector, "-- 3 result(s) --")); });
}

TEST_CASE("sparql: UNION with three branches (example query 3)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?x WHERE {
  { ?x wdt:P31 wd:Q5 . } UNION { ?x wdt:P31 wd:Q6 . } UNION { ?x wdt:P279 wd:Q500 . }
})");

        // Q1, Q2 (branch 1), Q3 (branch 2), Q50 (branch 3).
        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "Q3"));
        CHECK(any_output_contains(collector, "Q50"));
        CHECK(any_output_contains(collector, "-- 4 result(s) --")); });
}

// ---------------------------------------------------------------------------
// FILTER
// ---------------------------------------------------------------------------

TEST_CASE("sparql: FILTER with str() comparison drops duplicate pairs")
{
    // The deduplication idiom from the disjointness queries (4)-(6):
    // FILTER ( ( str(?a) < str(?b) ) )
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?a ?b WHERE {
  ?a wdt:P279 wd:Q50 .
  ?b wdt:P279 wd:Q50 .
  FILTER ( ( str(?a) < str(?b) ) )
})");

        // Of the pairs (Q5,Q5) (Q5,Q6) (Q6,Q5) (Q6,Q6) only (Q5,Q6) survives.
        CHECK(any_output_contains(collector, "Q5 Q6"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: FILTER with numeric comparison")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        process_lines(interactive, R"(
.lang wikidata
QA P569 1985
QB P569 1995
)");
        collector.clear();

        run_sparql(interactive, R"(SELECT ?x WHERE {
  ?x wdt:P569 ?d .
  FILTER ( ?d > 1990 )
})");

        CHECK(any_output_contains(collector, "QB"));
        CHECK_FALSE(any_output_contains(collector, "QA"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

// ---------------------------------------------------------------------------
// GROUP BY / COUNT / ORDER BY / LIMIT
// ---------------------------------------------------------------------------

TEST_CASE("sparql: GROUP BY with COUNT")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?class (COUNT(?x) AS ?n) WHERE {
  ?x wdt:P31 ?class .
}
GROUP BY ?class)");

        CHECK(any_output_contains(collector, "Q5 2"));
        CHECK(any_output_contains(collector, "Q6 1"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

TEST_CASE("sparql: ORDER BY DESC with LIMIT")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?c WHERE {
  wd:Q5 wdt:P279* ?c .
}
ORDER BY DESC(?c) LIMIT 1)");

        // String ordering: "Q500" > "Q50" > "Q5"
        CHECK(any_output_contains(collector, "Q500"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

// ---------------------------------------------------------------------------
// Subqueries and PREFIX
// ---------------------------------------------------------------------------

TEST_CASE("sparql: subquery joined with outer pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT ?x WHERE {
  { SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . } }
  ?x wdt:P21 wd:Q6581097 .
})");

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: nested subqueries (example query 3)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT DISTINCT ?third WHERE {
  { SELECT ?second WHERE {
      { SELECT ?first WHERE { ?i wdt:P31 ?first . } }
      ?first wdt:P279 ?second .
  } }
  ?second wdt:P279 ?third .
})");

        CHECK(any_output_contains(collector, "Q500"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: subquery with GROUP BY, COUNT and ORDER BY (example query 1)")
{
    // Miniature of the astronaut predicate-histogram query. QLever's
    // ql:has-predicate is expressed as a plain predicate variable.
    // Also probes predicate variables inside a conjunction.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        process_lines(interactive, R"(
.lang wikidata
QA1 P106 Q11631
QA2 P106 Q11631
QA1 P19 QC1
QA2 P19 QC1
QA1 P569 D1
)");
        collector.clear();

        run_sparql(interactive, R"(SELECT ?pred ?count WHERE {
  {
    SELECT ?pred (COUNT(?pred) AS ?count) WHERE {
      ?astronaut wdt:P106 wd:Q11631 .
      ?astronaut ?pred ?obj .
    }
    GROUP BY ?pred
    ORDER BY DESC(?count)
  }
})");

        CHECK(any_output_contains(collector, "P106 2"));
        CHECK(any_output_contains(collector, "P19 2"));
        CHECK(any_output_contains(collector, "P569 1"));
        CHECK(any_output_contains(collector, "-- 3 result(s) --")); });
}

TEST_CASE("sparql: custom PREFIX declaration expands to full IRI node names")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        // The predicate node is named by its full IRI in the graph.
        process_lines(interactive, R"(
.lang wikidata
QA "http://example.org/knows" QB
)");
        collector.clear();

        run_sparql(interactive, R"(PREFIX ex: <http://example.org/>
SELECT ?x WHERE { wd:QA ex:knows ?x . })");

        CHECK(any_output_contains(collector, "QB"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: PREFIX with empty local name (example query 2)")
{
    // Example query (2) declares prefixes for full entity IRIs and uses
    // them as bare "violated_1:". The expansion must also be stripped to
    // the bare Q-ID.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(PREFIX violated_1: <http://www.wikidata.org/entity/Q500>
SELECT ?i WHERE { ?i wdt:P279+ violated_1: . })");

        CHECK(any_output_contains(collector, "Q50"));
        CHECK(any_output_contains(collector, "Q5"));
        CHECK(any_output_contains(collector, "Q6"));
        CHECK(any_output_contains(collector, "-- 3 result(s) --")); });
}

// ---------------------------------------------------------------------------
// End-to-end: disjointness detection (example queries 2, 4, 5, 6)
// ---------------------------------------------------------------------------

TEST_CASE("sparql: disjointness culprit query with MINUS and P279+ (example queries 2/5)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        // QD1 is a direct subclass of both (disjoint) classes QV1 and QV2.
        // QD2 inherits the violation via QD1 and must be filtered out by
        // MINUS - only the topmost culprit QD1 remains.
        process_lines(interactive, R"(
.lang wikidata
QD1 P279 QV1
QD1 P279 QV2
QD2 P279 QD1
)");
        collector.clear();

        run_sparql(interactive, R"(SELECT DISTINCT ?class WHERE {
  { SELECT ?class WHERE { ?class wdt:P279+ wd:QV1 . ?class wdt:P279+ wd:QV2 . } }
  MINUS {
    ?class wdt:P279 ?parent .
    ?parent wdt:P279+ wd:QV1 .
    ?parent wdt:P279+ wd:QV2 .
  }
})");

        CHECK(any_output_contains(collector, "QD1"));
        CHECK_FALSE(any_output_contains(collector, "QD2"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: disjointness with qualifier-style triples and rank MINUS (example query 4)")
{
    // Runs example query (4) verbatim against simulated qualifier triples.
    // This query fails on both QLever and Blazegraph public services.
    // Once the qualifier import materializes p:P2738 / pq:P11260 /
    // wikibase:rank as triples, this exact query runs on real dumps.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_disjointness_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT DISTINCT ?i ?class ?disj1 ?disj2 WHERE {
  ?class p:P2738 ?l .
  MINUS { ?l wikibase:rank wikibase:DeprecatedRank . }
  ?l pq:P11260 ?disj1 . ?l pq:P11260 ?disj2 .
  FILTER ( ( str(?disj1) < str(?disj2) ) )
  ?i wdt:P279* ?disj1 . ?i wdt:P279* ?disj2 .
})");

        // QX is a subclass of both QD1 and QD2; the deprecated statement
        // QL2 (and with it QC2) is removed by the rank MINUS.
        CHECK(any_output_contains(collector, "QX QC QD1 QD2"));
        CHECK_FALSE(any_output_contains(collector, "QC2"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: disjointness instances via P31/P279* (example query 6)")
{
    // Same as example query (4) but for instances: ?i wdt:P31/wdt:P279*.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_disjointness_graph(interactive);
        collector.clear();

        run_sparql(interactive, R"(SELECT DISTINCT ?i ?class ?disj1 ?disj2 WHERE {
  ?class p:P2738 ?l .
  MINUS { ?l wikibase:rank wikibase:DeprecatedRank . }
  ?l pq:P11260 ?disj1 . ?l pq:P11260 ?disj2 .
  FILTER ( ( str(?disj1) < str(?disj2) ) )
  ?i wdt:P31/wdt:P279* ?disj1 . ?i wdt:P31/wdt:P279* ?disj2 .
})");

        // QI is an instance of QX, which is a subclass of both QD1 and QD2.
        CHECK(any_output_contains(collector, "QI QC QD1 QD2"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: qualifier prefixes resolve in the wikidata language regardless of .lang")
{
    // p:, pq: and wikibase: now resolve explicitly in the wikidata
    // language, like wd:/wdt: - no prior .lang wikidata required.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_disjointness_graph(interactive);
        interactive.process(".lang zelph");
        collector.clear();

        run_sparql(interactive, R"(SELECT DISTINCT ?i ?class WHERE {
  ?class p:P2738 ?l .
  MINUS { ?l wikibase:rank wikibase:DeprecatedRank . }
  ?l pq:P11260 ?disj1 . ?l pq:P11260 ?disj2 .
  FILTER ( ( str(?disj1) < str(?disj2) ) )
  ?i wdt:P279* ?disj1 . ?i wdt:P279* ?disj2 .
})");

        CHECK(any_output_contains(collector, "QX QC"));
        CHECK_FALSE(any_output_contains(collector, "QC2"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: statement main value via sequence path p:/ps:")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_disjointness_graph(interactive);
        collector.clear();

        run_sparql(interactive, "SELECT ?v WHERE { wd:QC p:P2738/ps:P2738 ?v . }");

        CHECK(any_output_contains(collector, "QU"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST_CASE("sparql: unsupported feature is rejected with an error")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        collector.clear();

        interactive.process("sparql");
        interactive.process("SELECT ?x WHERE { BIND(1 AS ?x) }");
        // The handler runs (and throws) when the empty line closes the block.
        CHECK_THROWS_AS(interactive.process(""), std::runtime_error); });
}

TEST_CASE("sparql: syntax error in a complete block is rejected with an error")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        collector.clear();

        interactive.process("sparql");
        // SELECT present and braces balanced -> dispatched, then PEG fails.
        interactive.process("SELECT ?x WHERE { ?x }");
        CHECK_THROWS_AS(interactive.process(""), std::runtime_error); });
}

TEST_CASE("sparql: incomplete block is force-dispatched by two blank lines")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        collector.clear();

        interactive.process("sparql");
        interactive.process("this is not sparql at all");
        // First blank line: handler vetoes (:incomplete), accumulation
        // continues. Second blank line: forced dispatch -> error.
        interactive.process("");
        CHECK_THROWS_AS(interactive.process(""), std::runtime_error); });
}

TEST_CASE("sparql: blank lines inside a pasted query do not terminate the block")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        interactive.process("sparql");
        interactive.process("PREFIX wd: <http://www.wikidata.org/entity/>");
        interactive.process("PREFIX wdt: <http://www.wikidata.org/prop/direct/>");
        interactive.process(""); // blank line after the prologue (no SELECT yet)
        interactive.process("SELECT ?x WHERE {");
        interactive.process(""); // blank line inside WHERE (braces unbalanced)
        interactive.process("  ?x wdt:P31 wd:Q5 .");
        interactive.process("}");
        interactive.process(""); // complete -> dispatch

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "Q2"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

// ---------------------------------------------------------------------------
// Keyword mechanism in imported scripts
// ---------------------------------------------------------------------------

TEST_CASE("sparql: keyword block in an imported script is flushed at EOF")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);

        namespace fs = std::filesystem;
        const fs::path script = fs::temp_directory_path() / "zelph_sparql_eof_test.zph";
        {
            // Note: no terminating empty line - EOF must close the block.
            std::ofstream out(script);
            out << "sparql\n";
            out << "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . }";
        }

        collector.clear();
        interactive.process_file(script.string(), {});
        fs::remove(script);

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "Q2"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

TEST_CASE("sparql: diamond hierarchy is deduplicated in closures")
{
    // Exercises (a) BFS deduplication in the native closure and (b) the
    // membership tables keyed by zelph/node values (hash/compare of the
    // abstract type across distinct wrapper instances).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        process_lines(interactive, R"(
.lang wikidata
DA P279 DB
DA P279 DC
DB P279 DD
DC P279 DD
)");
        collector.clear();
        run_sparql(interactive, "SELECT ?c WHERE { wd:DA wdt:P279+ ?c . }");
        // DB, DC, DD - DD reached via two paths must appear once.
        CHECK(any_output_contains(collector, "-- 3 result(s) --"));

        collector.clear();
        run_sparql(interactive, "SELECT ?i WHERE { ?i wdt:P279+ wd:DD . }");
        CHECK(any_output_contains(collector, "-- 3 result(s) --")); });
}

TEST_CASE("sparql: wd:/wdt: terms resolve in the wikidata language regardless of .lang")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        interactive.process(".lang zelph");
        collector.clear();

        run_sparql(interactive, "SELECT ?x WHERE { ?x wdt:P31 wd:Q5 . }");

        CHECK(any_output_contains(collector, "Q1"));
        CHECK(any_output_contains(collector, "Q2"));
        CHECK(any_output_contains(collector, "-- 2 result(s) --")); });
}

TEST_CASE("sparql: COUNT(DISTINCT ...) collapses duplicate bindings")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        setup_base_graph(interactive);
        collector.clear();

        // Without DISTINCT this counts 5 rows; the distinct classes
        // reachable via P279+ are Q50 and Q500.
        run_sparql(interactive,
                   "SELECT (COUNT(DISTINCT ?class) AS ?n) WHERE { ?i wdt:P279+ ?class . }");

        CHECK(any_output_contains(collector, "2"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}

TEST_CASE("sparql: COUNT(DISTINCT) over intersecting closures (violation count query)")
{
    // The exact shape of the paper's per-pair violation count, which also
    // exercises the closure-intersection evaluation (eval-step-group).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        load_sparql(interactive);
        process_lines(interactive, R"(
.lang wikidata
QD1 P279 QV1
QD1 P279 QV2
QD2 P279 QD1
)");
        collector.clear();

        run_sparql(interactive, R"(SELECT (COUNT(DISTINCT ?c) AS ?n) WHERE {
  ?c wdt:P279+ wd:QV1 .
  ?c wdt:P279+ wd:QV2 .
})");

        CHECK(any_output_contains(collector, "2"));
        CHECK(any_output_contains(collector, "-- 1 result(s) --")); });
}
