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
        R"js(%(defn api [s] (get (get root-env s) :value)) (def zq (api 'zelph/query)) (def zf (api 'zelph/fact)) (def zo (api 'zelph/out)) (def zn (api 'zelph/name)) (def zs (api 'zelph/set)))js";

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
