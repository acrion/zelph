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
// Partial-pattern anchoring (unification.cpp): a condition whose structured
// subject is only PARTIALLY bound -- the SC-congruence shape
// ((U op v) mark (U op v)) with bound v inside the pattern -- must not scan
// the whole relation; the candidate set is climbed from the bound inner
// node. Three pins:
//  1. completeness: anchored evaluation (both parallelism modes) finds the
//     same matches as the anchor-free naive reference (.anchors off),
//     additionally guarded by `.semi-naive check`;
//  2. the anchor path is actually taken in optimized mode (log marker --
//     red without the feature);
//  3. a hub anchor exceeding the work budget falls back to the full scan
//     and stays complete (no marker, same matches).
// ---------------------------------------------------------------------------

namespace
{
    template <typename Interactive>
    void setup_mark_workload(Interactive& interactive)
    {
        // 40 facts involving v, plus 40 distractors that share the mark
        // relation but not v: a complete result has exactly 40 matches, an
        // anchored candidate set never visits the distractors.
        interactive.process(R"js(%(each i (range 40) (def p (zelph/fact (string "u" i) "op" "v")) (zelph/fact p "mark" p)))js");
        interactive.process(R"js(%(each i (range 40) (def q (zelph/fact (string "w" i) "op" (string "z" i))) (zelph/fact q "mark" q)))js");
        process_lines(interactive, R"(
(V go V) => (V trig t)
(V trig Q,
 (U op V) mark (U op V))
=> ((U op V) found Q)
)");
    }
} // namespace

TEST_CASE("unification: partial-pattern anchor finds all matches of a partially bound pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        setup_mark_workload(interactive);
        interactive.process("v go v");
        collector.clear();
        interactive.process("_A found _B");
        CHECK(collect_answers(collector).size() == 40);
        CHECK(answers_contain(collector, "(u0 op v) found t"));
        CHECK(answers_contain(collector, "(u39 op v) found t")); });
}

TEST_CASE("unification: partial-pattern anchor is taken in optimized mode")
{
    run_parallel_mode([](auto& collector, auto& interactive)
                      {
        setup_mark_workload(interactive);
        interactive.process(".log 1");
        collector.clear();
        interactive.process("v go v");
        CHECK(any_event_contains(collector, "partial-anchor:")); });
}

TEST_CASE("unification: hub anchor exceeding the work budget falls back to the full scan")
{
    run_parallel_mode([](auto& collector, auto& interactive)
                      {
        // Blow up the anchor's adjacency well past the work budget
        // (max(1024, 16 * extent) with extent 3): 1200 unrelated facts on
        // the hub h. The climb must abort and the full scan of the tiny
        // mark2 relation must still deliver all 3 matches.
        interactive.process(R"js(%(each i (range 1200) (zelph/fact (string "x" i) "touch" "h")))js");
        interactive.process(R"js(%(each i (range 3) (def p (zelph/fact (string "k" i) "op2" "h")) (zelph/fact p "mark2" p)))js");
        process_lines(interactive, R"(
(V go2 V) => (V trig2 t)
(V trig2 Q,
 (U op2 V) mark2 (U op2 V))
=> ((U op2 V) found2 Q)
)");
        interactive.process(".log 1");
        interactive.process("h go2 h");
        collector.clear();
        interactive.process("_A found2 _B");
        CHECK(collect_answers(collector).size() == 3);
        CHECK(answers_contain(collector, "(k0 op2 h) found2 t")); });
}

TEST_CASE("unification: partial-pattern anchor engages in single-core mode")
{
    // Red before the use_anchors() decoupling: the anchor-construction
    // block sat inside a use_parallel() guard, so classic evaluation fell
    // back to full-relation scans (the >20 min single-core det gap).
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        setup_mark_workload(interactive);
        interactive.process(".log 1");
        collector.clear();
        interactive.process("v go v");
        CHECK(any_event_contains(collector, "partial-anchor:")); });
}

TEST_CASE("unification: .anchors off restores the anchor-free naive reference")
{
    // THE independent completeness net for anchoring: with anchors off,
    // every condition scans the full relation extent, and the results must
    // be identical. (The former reference -- plain single-core mode -- now
    // anchors too, by design.)
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        interactive.process(".anchors off");
        setup_mark_workload(interactive);
        interactive.process(".log 1");
        collector.clear();
        interactive.process("v go v");
        CHECK_FALSE(any_event_contains(collector, "partial-anchor:"));
        interactive.process(".log 0");
        collector.clear();
        interactive.process("_A found _B");
        CHECK(collect_answers(collector).size() == 40);
        CHECK(answers_contain(collector, "(u0 op v) found t"));
        CHECK(answers_contain(collector, "(u39 op v) found t")); });
}

TEST_CASE("unification: .anchors command reports and switches modes")
{
    run_parallel_mode([](auto& collector, auto& interactive)
                      {
        collector.clear();
        interactive.process(".anchors");
        CHECK(any_output_contains(collector, "Anchor-based lookups: on"));

        collector.clear();
        interactive.process(".anchors off");
        CHECK(any_output_contains(collector, "Anchor-based lookups: off"));

        collector.clear();
        interactive.process(".anchors on");
        CHECK(any_output_contains(collector, "Anchor-based lookups: on"));

        CHECK_THROWS_AS(interactive.process(".anchors banana"), std::runtime_error); });
}

TEST_CASE("unification: subject/object-driven anchors engage in single-core mode")
{
    // Red when increment_fact_index's anchor branch is gated on
    // use_parallel() instead of use_anchors(): the remaining condition
    // (B bar C) of the seeded rule has its subject bound and must take
    // the subject-driven anchor, logged as optimized_snapshot=YES.
    run_single_core_mode([](auto& collector, auto& interactive)
                         {
        process_lines(interactive, R"(
m1 foo n1
n1 bar p1
(A foo B, B bar C) => (A chain C)
)");
        interactive.process(".log 2");
        collector.clear();
        interactive.process("m2 foo n1");
        CHECK(any_event_contains(collector, "optimized_snapshot=YES")); });
}
