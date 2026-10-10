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

#include <cstdint>
#include <string>

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

TEST_CASE("profiler: every fs_cache miss is answered by one genuine hit or one walk")
{
    // `fs_cache misses == genuine hits + genuine walks` is verified on each
    // protocol in internals/measurement.md: a miss goes to the genuine
    // store, and only a node lacking an entry gets reconstructed via the
    // walk. The store also responds to Zelph::predicate_of, and that lookup
    // was counted as a hit as well, causing the number of hits to exceed the
    // number of misses by many times on the math stack, rendering the
    // identity meaningless.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto dump = [&collector, &interactive]() -> std::string
        {
            collector.clear();
            interactive.process(".prof");
            std::string prof;
            for (const auto& event : collector.events())
                prof += event.text + "\n";
            return prof;
        };

        const auto counter = [](const std::string& prof, const std::string& section, const std::string& key) -> uint64_t
        {
            const size_t start = prof.find(section);
            REQUIRE(start != std::string::npos);
            const size_t at = prof.find(key + "=", start);
            REQUIRE(at != std::string::npos);
            return std::stoull(prof.substr(at + key.size() + 1));
        };

        interactive.process(".deductions off");
        interactive.process(".log -1");
        interactive.process(".import math");
        interactive.process("<x y> ~ polyring");
        interactive.process(".prof reset");
        interactive.process("? $( (x+1)*(x-1) ) ≡ $( x^2 - 1 )");

        const std::string armed  = dump();
        const uint64_t    misses = counter(armed, "fs_cache:", "misses");
        const uint64_t    hits   = counter(armed, "genuine:", "hits");
        const uint64_t    walks  = counter(armed, "genuine:", "walks");
        REQUIRE(misses > 0);
        CHECK(misses == hits + walks);

        // The window above never walks, as the store responds to every miss, so
        // it holds the hit side exclusively. Using `.fact-stores off` is the
        // user-facing method to send every miss to the walk instead. It must be
        // issued after the armed window: `.fact-stores on` will not re-arm the
        // stores during the same session. The term is a new one, as the first
        // one would mostly hit the fs_cache.
        interactive.process(".fact-stores off");
        interactive.process(".prof reset");
        interactive.process("? $( (x+2)*(x-2) ) ≡ $( x^2 - 4 )");

        const std::string walked        = dump();
        const uint64_t    walked_misses = counter(walked, "fs_cache:", "misses");
        const uint64_t    walked_hits   = counter(walked, "genuine:", "hits");
        const uint64_t    walked_walks  = counter(walked, "genuine:", "walks");
        REQUIRE(walked_walks > 0);
        CHECK(walked_hits == 0);
        CHECK(walked_misses == walked_walks);

        // The removal cascade acquires structures via a dedicated entry point
        // (begin_fact_structures_scoped), inaccessible to any query, so it gets
        // a distinct window that activates just before the `.remove`. Auto-run
        // is disabled exclusively for performance: with the stores deactivated,
        // executing a run across the maths stack after each input line incurs
        // significant delay, and these facts need no inference.
        interactive.process(".auto-run");
        interactive.process("gamma1 rel gamma2");
        interactive.process("gamma3 rel gamma4");
        interactive.process("gamma5 rel gamma3");
        interactive.process(".prof reset");
        interactive.process(".remove gamma3");

        const std::string removed        = dump();
        const uint64_t    removed_misses = counter(removed, "fs_cache:", "misses");
        const uint64_t    removed_hits   = counter(removed, "genuine:", "hits");
        const uint64_t    removed_walks  = counter(removed, "genuine:", "walks");
        REQUIRE(removed_walks > 0);
        CHECK(removed_hits == 0);
        CHECK(removed_misses == removed_walks); });
}

// ---------------------------------------------------------------------------
// The number of matches processed in the run summary ("N matches processed")
// is not a profiler counter: `.run` reports this figure with logging off. It
// counts the bindings produced by unification search for positive fact
// conditions, regardless of whether they originated from a scan or were
// introduced via seeding. Semi-naive evaluation finds a significant portion
// of its bindings through seeding, and previously, these seeded bindings were
// excluded from the count, leading to scenarios where a run that derived
// facts could display "0 matches processed".
// ---------------------------------------------------------------------------

TEST_CASE("run summary: a binding that a new fact seeds counts as a match")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The figures listed below reflect the REPL default configuration.
        // Check mode, which serves as the default for this test binary,
        // concludes each run with a classic safety pass that introduces its
        // own matches during the scan (one for each human in this case).
        interactive.process(".semi-naive on");
        interactive.process(".auto-run");
        interactive.process("(X ~ human) => (X ~ mortal)");
        interactive.process("socrates ~ human");

        // During the first pass of a run, a scan delivers a single binding:
        // the fact it derives is seeded into the same condition but fails to
        // unify with it, thus contributing nothing.
        collector.clear();
        interactive.process(".run");
        CHECK(any_event_contains(collector, "(socrates ~ mortal) ⇐ (socrates ~ human)"));
        CHECK(any_event_contains(collector, "Reasoning summary: 1 matches processed,"));

        // .run-delta lacks a scanning pass: each of the two newly introduced
        // facts is seeded into the condition and binds it. Before seeds were
        // counted, this run derived both facts and reported 0.
        interactive.process("plato ~ human");
        interactive.process("aristotle ~ human");
        collector.clear();
        interactive.process(".run-delta");
        CHECK(any_event_contains(collector, "(plato ~ mortal) ⇐ (plato ~ human)"));
        CHECK(any_event_contains(collector, "(aristotle ~ mortal) ⇐ (aristotle ~ human)"));
        CHECK(any_event_contains(collector, "Reasoning summary: 2 matches processed,")); });
}

TEST_CASE("run summary: each link of a derivation chain is a seeded match")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The default behaviour of the REPL, due to the rationale
        // provided in the case above.
        interactive.process(".semi-naive on");
        interactive.process(".auto-run");
        interactive.process("(X ~ human) => (X ~ mortal)");
        interactive.process("(X ~ mortal) => (X ~ finite)");
        interactive.process("(X ~ finite) => (X ~ countable)");
        interactive.process("socrates ~ human");

        // During the scanning pass, the first rule is bound once. Each
        // subsequent fact derived thereafter is seeded into the conditions
        // and binds the subsequent rule's, thus establishing two additional
        // seeded bindings: a total of three, whereas the scan alone
        // registered only one. Reversing the order of the rules likewise
        // results in three.
        collector.clear();
        interactive.process(".run");
        CHECK(any_event_contains(collector, "(socrates ~ countable) ⇐ (socrates ~ finite)"));
        CHECK(any_event_contains(collector, "Reasoning summary: 3 matches processed,")); });
}
