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

#include <doctest/doctest.h> // provides main()

#include "test_helpers.hpp"

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Semi-naive evaluation
//
// run_both_modes already puts every test into `.semi-naive check` mode, so
// the entire suite doubles as an equivalence test between delta-driven and
// classic evaluation. The tests in this file pin the specific design
// decisions and semantic corner cases of the semi-naive implementation.
//
// NOTE on rule definition order: rules are iterated in definition order
// (adjacency containers preserve insertion order), so defining the CONSUMING
// rule before the PRODUCING rule guarantees that the classic first pass
// cannot complete the derivation chain -- the completion must then happen
// via the delta path (or the delta-unsafe classic re-application), which is
// exactly what these tests exercise. Should iteration order ever change,
// the tests degrade gracefully: the checked property still holds, only the
// cross-iteration path is no longer guaranteed to be the one taken -- and
// check mode independently catches any completeness regression.
// ---------------------------------------------------------------------------

TEST_CASE("semi-naive: .semi-naive command reports and switches modes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // run_both_modes enables check mode for every test.
        collector.clear();
        interactive.process(".semi-naive");
        CHECK(any_output_contains(collector, "Semi-naive evaluation: check"));

        collector.clear();
        interactive.process(".semi-naive off");
        CHECK(any_output_contains(collector, "Semi-naive evaluation: off"));

        collector.clear();
        interactive.process(".semi-naive on");
        CHECK(any_output_contains(collector, "Semi-naive evaluation: on"));

        CHECK_THROWS_AS(interactive.process(".semi-naive banana"), std::runtime_error); });
}

TEST_CASE("semi-naive: classic mode (off) still computes full results")
{
    // Pins that .semi-naive off remains a fully functional evaluation
    // strategy (fresh variables, termination guard, cross-module cascade),
    // not just a dead code path behind the default.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".semi-naive off");
        interactive.process(".import arithmetic");
        collector.clear();
        interactive.process("&12 * &34");
        interactive.run(true, false, false);
        CHECK(any_output_starts_with(collector, "((&12 * &34) = &408)")); });
}

TEST_CASE("semi-naive: negation over a growing domain stays complete across iterations")
{
    // The negation in the consumer rule contains the variable X, which no
    // positive condition of that rule binds. Its complementary enumeration
    // therefore ranges over the DOMAIN of the flagged relation -- and the
    // producer rule EXTENDS that domain during reasoning. Pure delta
    // seeding could never re-fire the consumer (its only positive leaf has
    // predicate marker, which gains no new facts), so the engine must
    // classify such rules as delta-unsafe and re-apply them classically in
    // every iteration. The consumer is defined first (see NOTE above), so
    // in the classic first pass it runs before the producer has created
    // (t1 flagged good) -- the derivation is forced across iterations.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(M marker K, ¬(X flagged bad)) => (X unflagged K)
(A trigger B) => (B flagged good)
m marker k
a flagged good
)");
        CHECK(any_output_starts_with(collector, "( a unflagged k )"));

        collector.clear();
        interactive.process("s trigger t1");
        interactive.run(true, false, false);
        // t1 entered the flagged domain only through the producer's
        // deduction -- the negation rule must still see it.
        CHECK(any_output_starts_with(collector, "( t1 unflagged k )")); });
}

TEST_CASE("semi-naive: facts materialized as instantiation side effects seed rules")
{
    // Pins the delta-capture design decision (fact()-level observer, not a
    // deduce()-level hook): the producer's consequence ((A bar B) baz ok)
    // materializes the INNER fact (p bar q) as a side effect of
    // instantiate_fact -- it is not itself a deduction. The consumer
    // matches exactly that inner fact at top level. A deduce()-level delta
    // would only contain the baz fact, the consumer would never be seeded,
    // and (p linked q) would be missing (check mode would then fail the
    // run). The consumer is defined first (see NOTE above), so the classic
    // first pass cannot derive it either -- only the seeded path can.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X bar Y) => (X linked Y)
(A foo B) => ((A bar B) baz ok)
)");
        collector.clear();
        interactive.process("p foo q");
        interactive.run(true, false, false);
        CHECK(any_output_starts_with(collector, "( p linked q )")); });
}
