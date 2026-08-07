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

using namespace zelph::test;

// ---------------------------------------------------------------------------
// zelph/query from Janet.
//
// A query pattern is an ordinary graph node, so a caller may keep it in a
// binding and use it more than once -- a program driving zelph as a library
// naturally builds its patterns once and asks repeatedly.
//
// That used to fail silently. The query only ran when the *current statement*
// had created scoped variables, and the scope is cleared after every query, so
// a pattern built in an earlier expression produced an empty array: a wrong
// answer indistinguishable from "no matches". The bindings are labelled from
// the variable nodes' own names now, which the graph carries anyway.
// ---------------------------------------------------------------------------

namespace
{
    constexpr const char* kApi =
        R"js(%(defn api [s] (get (get root-env s) :value)) (def zq (api 'zelph/query)) (def zf (api 'zelph/fact)) (def zo (api 'zelph/out)) (def zn (api 'zelph/name)) (def zs (api 'zelph/set)) (def zv (api 'zelph/var)))js";

    // Report match count and the bindings, so both halves are pinned.
    constexpr const char* kReport =
        R"js(%(defn report [tag q] (def rs (zq q)) (zo (string tag "-count=" (length rs))) (each r rs (zo (string tag "-bind=" (zn (get r 'A)) "/" (zn (get r 'B)))))))js";
} // namespace

TEST_CASE("zelph/query: a pattern built in an earlier expression still matches")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(kReport);
        interactive.process(R"js(%(zf "e1" "hits" "e5"))js");
        collector.clear();

        // Pattern and query in separate statements: the scope that created the
        // variables is long gone by the time the query runs.
        interactive.process(R"js(%(def q (zf 'A "hits" 'B)))js");
        interactive.process(R"js(%(report "stored" q))js");
        CHECK(any_output_contains(collector, "stored-count=1"));
        CHECK(any_output_contains(collector, "stored-bind=e1/e5")); });
}

TEST_CASE("zelph/query: the same pattern answers the same way every time")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(kReport);
        interactive.process(R"js(%(zf "e1" "hits" "e5"))js");
        interactive.process(R"js(%(def q (zf 'A "hits" 'B)))js");
        collector.clear();

        interactive.process(R"js(%(report "first" q))js");
        interactive.process(R"js(%(report "second" q))js");
        interactive.process(R"js(%(report "third" q))js");
        CHECK(any_output_contains(collector, "first-count=1"));
        CHECK(any_output_contains(collector, "second-count=1"));
        CHECK(any_output_contains(collector, "third-count=1")); });
}

TEST_CASE("zelph/query: a stored conjunction matches, repeatedly")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(R"js(%(zf "e1" "hits" "e5"))js");
        interactive.process(R"js(%(zf "e5" "holds" "BN"))js");
        // A join across two conditions, which is the shape a rule condition has.
        interactive.process(R"js(%(def cs (let [s (zs (zf 'A "hits" 'B) (zf 'B "holds" 'K))] (zf s "~" "conjunction") s)))js");
        collector.clear();

        interactive.process(R"js(%(each r (zq cs) (zo (string "one=" (zn (get r 'A)) "/" (zn (get r 'B)) "/" (zn (get r 'K))))))js");
        interactive.process(R"js(%(each r (zq cs) (zo (string "two=" (zn (get r 'A)) "/" (zn (get r 'B)) "/" (zn (get r 'K))))))js");
        CHECK(any_output_contains(collector, "one=e1/e5/BN"));
        CHECK(any_output_contains(collector, "two=e1/e5/BN")); });
}

// ---------------------------------------------------------------------------
// The scope of a variable SYMBOL is one evaluation of a Janet block, exactly
// as a variable in zelph syntax is quantified by one statement. Two blocks
// writing 'B mean two different variables, so a conjunction assembled from
// conditions built in separate blocks does not join -- it multiplies.
//
// Nothing reports that: the query answers, with the cross product. On a
// Wikidata-sized graph the same mistake turns a two-row answer into hundreds
// of thousands of rows (400 facts of each condition already give 160 801) and
// exhausts memory long before it finishes.
//
// The two cases below are the same three lines of Janet, differing only in
// how they are split into statements. They are pinned together so the
// difference stays visible; the one-block form is the one to write.
// ---------------------------------------------------------------------------
TEST_CASE("zelph/query: conditions built in ONE statement share their variables")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(R"js(%(zf "e1" "hits" "e5"))js");
        interactive.process(R"js(%(zf "e2" "hits" "e6"))js");
        interactive.process(R"js(%(zf "e5" "holds" "BN"))js");
        collector.clear();

        // 'B is the same variable in both conditions: the second one selects
        // among the two "hits" facts, and only e1/e5 survives.
        interactive.process(R"js(%(def cs (let [s (zs (zf 'A "hits" 'B) (zf 'B "holds" 'K))] (zf s "~" "conjunction") s)))js");
        interactive.process(R"js(%(zo (string "joined=" (length (zq cs)))))js");
        CHECK(any_output_contains(collector, "joined=1")); });
}

TEST_CASE("zelph/query: conditions built in SEPARATE statements do not")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(R"js(%(zf "e1" "hits" "e5"))js");
        interactive.process(R"js(%(zf "e2" "hits" "e6"))js");
        interactive.process(R"js(%(zf "e5" "holds" "BN"))js");
        collector.clear();

        // Same symbols, one statement each: two distinct 'B nodes, so the
        // conditions are independent and the answer is 2 x 1, not the join.
        interactive.process(R"js(%(def c1 (zf 'A "hits" 'B)))js");
        interactive.process(R"js(%(def c2 (zf 'B "holds" 'K)))js");
        interactive.process(R"js(%(def cs (let [s (zs c1 c2)] (zf s "~" "conjunction") s)))js");
        interactive.process(R"js(%(zo (string "crossed=" (length (zq cs)))))js");
        CHECK(any_output_contains(collector, "crossed=2")); });
}

// zelph/var is the way out of the cross product above: the variable is a
// VALUE, so the caller's own binding decides its extent instead of the block
// boundary. Same three statements, joined.
TEST_CASE("zelph/var: a variable node joins conditions across statements")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(R"js(%(zf "e1" "hits" "e5"))js");
        interactive.process(R"js(%(zf "e2" "hits" "e6"))js");
        interactive.process(R"js(%(zf "e5" "holds" "BN"))js");
        collector.clear();

        interactive.process(R"js(%(def shared (zv "B")))js");
        interactive.process(R"js(%(def c1 (zf (zv "A") "hits" shared)))js");
        interactive.process(R"js(%(def c2 (zf shared "holds" (zv "K"))))js");
        interactive.process(R"js(%(def cs (let [s (zs c1 c2)] (zf s "~" "conjunction") s)))js");
        interactive.process(R"js(%(each r (zq cs) (zo (string "row=" (zn (get r 'A)) "/" (zn (get r 'B)) "/" (zn (get r 'K))))))js");

        CHECK(any_output_contains(collector, "row=e1/e5/BN"));
        CHECK_FALSE(any_output_contains(collector, "row=e2")); });
}

// Two calls are two variables, whatever they are called. Otherwise a program
// building many patterns in one loop would join them all by accident.
TEST_CASE("zelph/var: two calls with the same name are two variables")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(R"js(%(zf "e1" "hits" "e5"))js");
        interactive.process(R"js(%(zf "e2" "hits" "e6"))js");
        interactive.process(R"js(%(zf "e5" "holds" "BN"))js");
        collector.clear();

        interactive.process(R"js(%(def cs (let [s (zs (zf (zv "A") "hits" (zv "B")) (zf (zv "B") "holds" (zv "K")))] (zf s "~" "conjunction") s)))js");
        interactive.process(R"js(%(zo (string "crossed=" (length (zq cs)))))js");
        CHECK(any_output_contains(collector, "crossed=2")); });
}

// A name is what makes a binding readable -- an unnamed variable still
// matches, it just contributes no column.
TEST_CASE("zelph/var: an unnamed variable matches but binds nothing readable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(R"js(%(zf "e1" "hits" "e5"))js");
        collector.clear();

        interactive.process(R"js(%(def q (zf "e1" "hits" (zv))))js");
        interactive.process(R"js(%(def rs (zq q)))js");
        interactive.process(R"js(%(zo (string "rows=" (length rs) " keys=" (length (keys (first rs))))))js");
        CHECK(any_output_contains(collector, "rows=1 keys=0")); });
}

TEST_CASE("zelph/query: a pattern without variables stays a no-op")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kApi);
        interactive.process(R"js(%(zf "a" "p" "b"))js");
        collector.clear();

        // Ground facts are zelph/exists' job. Querying one must neither match
        // nor disturb the graph.
        interactive.process(R"js(%(zo (string "ground=" (length (zq (zf "a" "p" "b"))))))js");
        CHECK(any_output_contains(collector, "ground=0"));

        interactive.process(R"js(%(zo (string "intact=" ((api 'zelph/exists) "a" "p" "b"))))js");
        CHECK(any_output_contains(collector, "intact=true")); });
}
