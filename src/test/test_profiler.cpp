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
// Profiler plumbing for heavy measurements:
//  - ".log -1" is the counter-only mode: counters accumulate, but the
//    per-deduction [prof] block must NOT be printed (at scale it floods the
//    output and dominates the measurement -- the should_log(1) gate in
//    log_after_deduction).
//  - ".prof" dumps the accumulated counters on demand, including the top-N
//    rule/relation sections; ".prof reset" starts a fresh window.
//  - Without active logging, ".prof" explains how to enable the counters
//    instead of printing a meaningless all-zero block.
// ---------------------------------------------------------------------------

TEST_CASE("profiler: counter-only mode is silent per deduction; .prof dumps on demand")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import decimal-arithmetic");
        interactive.process(".deductions off");
        interactive.process(".log -1");
        collector.clear();

        interactive.process("(&2 + &3) = X");

        // Counter-only mode: no per-deduction profiler blocks.
        CHECK_FALSE(any_event_contains(collector, "[prof] epoch="));

        collector.clear();
        interactive.process(".prof");
        CHECK(any_event_contains(collector, "[prof] epoch="));
        CHECK(any_event_contains(collector, "facts_created="));
        CHECK(any_event_contains(collector, "top_rules_by_facts_created"));

        // .prof reset starts a fresh window: an immediate second dump shows
        // zero rule applications.
        interactive.process(".prof reset");
        collector.clear();
        interactive.process(".prof");
        CHECK(any_event_contains(collector, "rules_applied=0")); });
}

TEST_CASE("profiler: .prof without active logging explains how to enable counters")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process(".prof");
        CHECK(any_event_contains(collector, "Profiler counters are inactive"));
        CHECK_FALSE(any_event_contains(collector, "[prof] epoch=")); });
}
