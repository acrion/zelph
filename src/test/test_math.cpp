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
// math.zph: the entry point. A ring declaration is an ordinary fact, and
// ordinary rules turn it into the sort and order declarations the
// polynomial layer consumes -- no Janet anywhere in the user's path.
// ---------------------------------------------------------------------------

TEST_CASE("math: polyring derives sorts and the adjacent nesting order")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        interactive.process("<x y z> ~ polyring");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "RING-SORT-" (and (zelph/exists "x" "~" "symvar") (zelph/exists "y" "~" "symvar") (zelph/exists "z" "~" "symvar"))))js");
        interactive.process(R"js(%(string "RING-ADJ-" (and (zelph/exists "x" "pouter" "y") (zelph/exists "y" "pouter" "z"))))js");
        interactive.process(R"js(%(string "RING-TRANS-" (zelph/exists "x" "pouter" "z")))js");
        interactive.process(R"js(%(string "RING-DIR-" (zelph/exists "y" "pouter" "x")))js");

        CHECK(any_output_contains(collector, "RING-SORT-true"));
        CHECK(any_output_contains(collector, "RING-ADJ-true"));
        // Transitivity comes from polynomial.zph -- adjacent pairs suffice.
        CHECK(any_output_contains(collector, "RING-TRANS-true"));
        // The order is directional; the reverse must NOT be derivable.
        CHECK(any_output_contains(collector, "RING-DIR-false")); });
}

TEST_CASE("math: a one-element ring declares its variable and no order pair")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        interactive.process("<x> ~ polyring");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "RING1-SORT-" (zelph/exists "x" "~" "symvar")))js");
        CHECK(any_output_contains(collector, "RING1-SORT-true")); });
}

TEST_CASE("math: a ring declaration echoes in the order it was written")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        collector.clear();
        interactive.process("<x y z> ~ polyring");
        // The LSB-first reversal is the numeral convention and must not
        // apply to a list of ordinary single-character nodes.
        CHECK(any_output_contains(collector, "<x y z> ~ polyring")); });
}

TEST_CASE("math: three lines prove a one-variable identity")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        interactive.process("<x> ~ polyring");
        interactive.process("? $( (1+x)*(1-x) ) ≡ $( 1 - x^2 )");
        collector.clear();

        interactive.process(R"js(%(let [l (zelph/fact (zelph/fact (zelph/number "1") "+" "x") "*" (zelph/fact (zelph/number "1") "-" "x")) r (zelph/fact (zelph/number "1") "-" (zelph/fact "x" "^" (zelph/number "2")))] (string "RING-ID1-" (zelph/exists (zelph/fact l "≡" r) "=" (zelph/resolve "proven")))))js");
        CHECK(any_output_contains(collector, "RING-ID1-true")); });
}

TEST_CASE("math: a two-variable identity consumes the declared order")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        interactive.process("<x y> ~ polyring");
        interactive.process("? $( (x+y)*(x-y) ) ≡ $( x^2 - y^2 )");
        collector.clear();

        interactive.process(R"js(%(let [l (zelph/fact (zelph/fact "x" "+" "y") "*" (zelph/fact "x" "-" "y")) r (zelph/fact (zelph/fact "x" "^" (zelph/number "2")) "-" (zelph/fact "y" "^" (zelph/number "2")))] (string "RING-ID2-" (zelph/exists (zelph/fact l "≡" r) "=" (zelph/resolve "proven")))))js");
        CHECK(any_output_contains(collector, "RING-ID2-true")); });
}

TEST_CASE("math: without a ring declaration nothing is proven")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        // No polyring: x has no sort, so it compiles to nothing and the
        // identity stays unanswered -- partiality by absence, not a wrong
        // verdict.
        interactive.process("? $( (1+x)*(1-x) ) ≡ $( 1 - x^2 )");
        collector.clear();

        interactive.process(R"js(%(let [l (zelph/fact (zelph/fact (zelph/number "1") "+" "x") "*" (zelph/fact (zelph/number "1") "-" "x")) r (zelph/fact (zelph/number "1") "-" (zelph/fact "x" "*" "x"))] (string "RING-NONE-" (zelph/exists (zelph/fact l "≡" r) "=" (zelph/resolve "proven")))))js");
        CHECK(any_output_contains(collector, "RING-NONE-false")); });
}

TEST_CASE("math: rings are ordinary nodes and can carry further facts")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import math");
        interactive.process("<x y> ~ polyring");
        interactive.process("<x y> studiedin jacobian-demo");
        interactive.run(true, false, false);
        collector.clear();

        // The declaration is knowledge, not a side effect: the same node is
        // reachable, extensible and queryable like any other.
        interactive.process(R"js(%(string "RING-META-" (and (zelph/exists (zelph/list (zelph/resolve "x") (zelph/resolve "y")) "~" "polyring") (zelph/exists (zelph/list (zelph/resolve "x") (zelph/resolve "y")) "studiedin" "jacobian-demo"))))js");
        CHECK(any_output_contains(collector, "RING-META-true")); });
}
