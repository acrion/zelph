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
// zelph/run and zelph/run-once: reaching the inference engine from Janet.
//
// Auto-run only fires when a REPL input has been processed, so a program that
// drives zelph as a library -- creating facts and rules through the Janet API
// and never entering a statement -- could previously define a rule but never
// obtain its consequences. These tests pin that the two functions run the
// engine, and that they differ in exactly the way the corresponding commands
// do: one pass versus a fixed point.
//
// Auto-run is switched off first, so what is observed is the effect of the
// call and not of the surrounding input processing. Results are reported
// through zelph/out rather than Janet's print, because only the former
// reaches the collector.
// ---------------------------------------------------------------------------

namespace
{
    // Report whether a fact is present, without creating it.
    constexpr const char* kReport =
        R"js(%(defn report [tag s p o] (zelph/out (string tag "=" (if (zelph/exists s p o) "yes" "no")))))js";
} // namespace

TEST_CASE("zelph/run: Janet can trigger forward chaining")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        CHECK(any_output_contains(collector, "Auto-run is now disabled"));
        collector.clear();

        interactive.process(kReport);
        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "mortal")))js");

        // The rule exists, but nothing has run it yet.
        interactive.process(R"js(%(report "before" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "before=no"));

        interactive.process(R"js(%(zelph/run))js");
        interactive.process(R"js(%(report "after" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "after=yes")); });
}

TEST_CASE("zelph/run: reaches a fixed point, run-once does one pass")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();

        interactive.process(kReport);

        // A three-link chain plus transitivity. One pass closes a .. c and
        // b .. d; only a fixed point also yields a .. d.
        interactive.process(R"js(%(zelph/fact "a" "step" "b"))js");
        interactive.process(R"js(%(zelph/fact "b" "step" "c"))js");
        interactive.process(R"js(%(zelph/fact "c" "step" "d"))js");
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "step" 'Y) (zelph/fact 'Y "step" 'Z)] (zelph/fact 'X "step" 'Z)))js");

        interactive.process(R"js(%(zelph/run-once))js");
        interactive.process(R"js(%(report "once-ac" "a" "step" "c"))js");
        interactive.process(R"js(%(report "once-ad" "a" "step" "d"))js");
        CHECK(any_output_contains(collector, "once-ac=yes"));

        interactive.process(R"js(%(zelph/run))js");
        interactive.process(R"js(%(report "full-ad" "a" "step" "d"))js");
        CHECK(any_output_contains(collector, "full-ad=yes")); });
}

TEST_CASE("zelph/run: arity is fixed at zero")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS(interactive.process(R"js(%(zelph/run "unexpected"))js"));
        CHECK_THROWS(interactive.process(R"js(%(zelph/run-once "unexpected"))js")); });
}
