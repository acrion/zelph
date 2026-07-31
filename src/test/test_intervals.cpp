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
// Allen's interval algebra, as far as it is single-valued -- a use case whose
// whole content is a TABLE, read by one rule.
//
// It is here because the composition table is DATA: `((R then S) gives T,
// X R Y, Y S Z) => (X T Z)` quantifies over three predicates at once, which
// is the shape zelph exists for and which no relational query language can
// state. Adding a row to the table adds an inference; nothing about the rule
// changes. The inverse meta-rule is a second one of the same kind.
//
// The numbers are what makes it a test rather than a demo: on the chain
// a meets b meets c meets d, composition derives exactly THREE `before`
// pairs, and they can be counted by hand -- (a,c) and (b,d) from meets∘meets,
// then (a,d) from meets∘before. Everything else follows from those.
// ---------------------------------------------------------------------------

namespace
{
    // Only the pairs whose Allen composition is a SINGLE relation. Most are
    // disjunctive (b∘d is {b,o,m,d,s}), and a disjunction is not a fact --
    // it would need one rule per alternative, which is a different exercise.
    const char* const table = R"zelph(
((R then S) gives T, X R Y, Y S Z) => (X T Z)
(R "is inverse of" S, X R Y) => (Y S X)

(before then before) gives before
(before then meets)  gives before
(meets  then before) gives before
(meets  then meets)  gives before
(during then before) gives before
(during then during) gives during

before "is inverse of" after
during "is inverse of" contains
meets  "is inverse of" metby

a ~ interval
b ~ interval
c ~ interval
d ~ interval

a meets b
b meets c
c meets d
)zelph";
}

TEST_CASE("intervals: a composition table read by one rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, table);

        collector.clear();
        interactive.process("X before Y");
        // meets∘meets over (a,b,c) and (b,c,d), then meets∘before over
        // (a,b,d). Nothing else: `meets` alone is not `before`, so no
        // adjacent pair appears.
        CHECK(answers_contain(collector, "a before c"));
        CHECK(answers_contain(collector, "b before d"));
        CHECK(answers_contain(collector, "a before d"));

        std::size_t before_pairs = 0;
        for (const auto& answer : collect_answers(collector))
            if (answer.find(" before ") != std::string::npos) ++before_pairs;
        CHECK(before_pairs == 3);

        // The inverse meta-rule mirrors every one of them, and the typed
        // `meets` facts as well.
        collector.clear();
        interactive.process("X after Y");
        CHECK(answers_contain(collector, "c after a"));
        CHECK(answers_contain(collector, "d after b"));
        CHECK(answers_contain(collector, "d after a"));

        collector.clear();
        interactive.process("X metby Y");
        CHECK(answers_contain(collector, "b metby a"));
        CHECK(answers_contain(collector, "c metby b"));
        CHECK(answers_contain(collector, "d metby c"));

        // A relation the table can reach nothing for stays empty -- the rule
        // does not invent a composition it was not given.
        collector.clear();
        interactive.process("X during Y");
        CHECK_FALSE(any_output_starts_with(collector, "Answer:")); });
}

TEST_CASE("intervals: adding a row to the table adds an inference")
{
    // The point of putting the table in the graph: `meets` composed with
    // itself gives `before`, and saying so for `during` too costs one fact,
    // not one rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, table);
        process_lines(interactive, R"(
p during q
q during r
)");
        collector.clear();
        interactive.process("X during Y");
        CHECK(answers_contain(collector, "p during r"));

        // And its inverse, without a word about `contains` anywhere but in
        // the "is inverse of" fact.
        collector.clear();
        interactive.process("X contains Y");
        CHECK(answers_contain(collector, "r contains p")); });
}

TEST_CASE("intervals: what has nothing before it, via negation")
{
    // `meets` is not `before` in Allen's algebra -- touching is not a gap --
    // so on this chain BOTH a and b have nothing strictly before them, and
    // both c and d have nothing strictly after them. That is the answer the
    // table gives, and it is the reason the case is worth pinning: the
    // result follows from the rows supplied, not from what the relation
    // names suggest.
    //
    // The negation rules are entered AFTER the facts on purpose. With
    // auto-run every line starts a run, and a conclusion drawn while the
    // graph was still empty is never taken back -- the engine is monotonic.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, table);
        process_lines(interactive, R"(
(A ~ interval, ¬(B before A)) => (A is earliest)
(A ~ interval, ¬(A before B)) => (A is latest)
)");
        collector.clear();
        interactive.process("X is earliest");
        CHECK(answers_contain(collector, "a is earliest"));
        CHECK(answers_contain(collector, "b is earliest"));
        CHECK_FALSE(any_output_contains(collector, "c is earliest"));
        CHECK_FALSE(any_output_contains(collector, "d is earliest"));

        collector.clear();
        interactive.process("X is latest");
        CHECK(answers_contain(collector, "c is latest"));
        CHECK(answers_contain(collector, "d is latest"));
        CHECK_FALSE(any_output_contains(collector, "a is latest"));
        CHECK_FALSE(any_output_contains(collector, "b is latest")); });
}

TEST_CASE("intervals: a cyclic chain contradicts itself")
{
    // Closing the chain makes the composition derive both directions
    // between the same pair, which the contradiction rule reads off the
    // result of everything above it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, table);
        interactive.process("(X before Y, Y before X) => !");

        collector.clear();
        interactive.process("d meets a");
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("intervals: the proof names the table row it used")
{
    // A derivation through a rule with three variable predicates has to
    // reconstruct WHICH row fired, or the explanation says nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, table);

        collector.clear();
        interactive.process(".explain (a before d)");
        CHECK(any_output_contains(collector, "b before d"));
        CHECK(any_output_contains(collector, "a meets b"));
        CHECK(any_output_contains(collector, "gives before"));

        // A typed fact reached through a meta-rule reports honestly: the
        // rule's consequence (X T Z) unifies with everything, so the engine
        // cannot call it an axiom, but it must not suggest the graph is
        // broken either.
        CHECK_FALSE(any_output_contains(collector, "no acyclic justification")); });
}
