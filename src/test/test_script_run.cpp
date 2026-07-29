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
        CHECK_THROWS(interactive.process(R"js(%(zelph/run-once "unexpected"))js"));
        CHECK_THROWS(interactive.process(R"js(%(zelph/run-delta "unexpected"))js")); });
}

// ---------------------------------------------------------------------------
// .run-delta / zelph/run-delta
//
// A plain run always opens with a classic pass over the whole graph, so
// "add a little, run again" costs time proportional to everything accumulated
// rather than to the addition. run-delta seeds the fixpoint with the facts
// created since the last run instead.
//
// The tests below pin the two halves of that contract: it must derive what a
// full run would from the new facts, and it must refuse to skip the pass in
// the cases where the pass is exactly what would have found the answer.
// ---------------------------------------------------------------------------

namespace
{
    constexpr const char* kMortalRule =
        R"js(%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "mortal")))js";
} // namespace

TEST_CASE("run-delta: derives from the facts added since the last run")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run))js");
        interactive.process(R"js(%(report "socrates" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "socrates=yes"));

        // New fact, same rules: the delta is all the engine needs.
        interactive.process(R"js(%(zelph/fact "plato" "~" "human"))js");
        interactive.process(R"js(%(report "plato-before" "plato" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "plato-before=no"));

        interactive.process(R"js(%(zelph/run-delta))js");
        interactive.process(R"js(%(report "plato-after" "plato" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "plato-after=yes")); });
}

TEST_CASE("run-delta: falls back to a full pass before any run has happened")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        // Nothing has ever been saturated, so there is no fixpoint to extend.
        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run-delta))js");
        // The notice goes to the diagnostic channel, not to Out.
        CHECK(any_event_contains(collector, "Incremental run not applicable"));

        interactive.process(R"js(%(report "socrates" "socrates" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "socrates=yes")); });
}

TEST_CASE("run-delta: falls back to a full pass when a rule was added")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run))js");
        collector.clear();

        // The new rule has to see a fact that is older than it. Seeding from
        // the delta would only offer it the rule's own construction, so the
        // classic pass must not be skipped here.
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "~" "human")] (zelph/fact 'X "~" "fallible")))js");
        interactive.process(R"js(%(zelph/run-delta))js");
        // The notice goes to the diagnostic channel, not to Out.
        CHECK(any_event_contains(collector, "Incremental run not applicable"));

        interactive.process(R"js(%(report "socrates" "socrates" "~" "fallible"))js");
        CHECK(any_output_contains(collector, "socrates=yes")); });
}

TEST_CASE("run-delta: seeds facts that arrived through an import")
{
    // Facts created by an imported script are ordinary additions -- the
    // record must not key on input capture, or a program driving zelph as a
    // library (which never enters a statement) would never get a seeded run.
    namespace fs          = std::filesystem;
    const fs::path script = fs::temp_directory_path() / "zelph_test_run_delta_facts.zph";
    {
        std::ofstream f(script);
        f << "aristotle ~ human\n";
    }

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run))js");
        collector.clear();

        interactive.process(".import \"" + script.string() + "\"");
        interactive.process(R"js(%(zelph/run-delta))js");
        CHECK_FALSE(any_event_contains(collector, "Incremental run not applicable"));

        interactive.process(R"js(%(report "aristotle" "aristotle" "~" "mortal"))js");
        CHECK(any_output_contains(collector, "aristotle=yes")); });

    std::error_code ec;
    fs::remove(script, ec);
}

TEST_CASE("run-delta: falls back when an import brings a rule")
{
    // A rule arriving with the import has to see the facts that were already
    // there, which is exactly what the skipped classic pass would show it.
    namespace fs          = std::filesystem;
    const fs::path script = fs::temp_directory_path() / "zelph_test_run_delta_rule.zph";
    {
        std::ofstream f(script);
        f << "(X ~ human) => (X ~ fallible)\n";
    }

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/fact "socrates" "~" "human"))js");
        interactive.process(kMortalRule);
        interactive.process(R"js(%(zelph/run))js");
        collector.clear();

        interactive.process(".import \"" + script.string() + "\"");
        interactive.process(R"js(%(zelph/run-delta))js");
        CHECK(any_event_contains(collector, "Incremental run not applicable"));

        interactive.process(R"js(%(report "socrates" "socrates" "~" "fallible"))js");
        CHECK(any_output_contains(collector, "socrates=yes")); });

    std::error_code ec;
    fs::remove(script, ec);
}

TEST_CASE("run-delta: chains, and matches what a full run would derive")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        // Transitivity, so a seeded fact must also combine with older ones
        // rather than only being looked at on its own.
        interactive.process(R"js(%(zelph/rule [(zelph/fact 'X "step" 'Y) (zelph/fact 'Y "step" 'Z)] (zelph/fact 'X "step" 'Z)))js");
        interactive.process(R"js(%(zelph/fact "a" "step" "b"))js");
        interactive.process(R"js(%(zelph/run))js");

        interactive.process(R"js(%(zelph/fact "b" "step" "c"))js");
        interactive.process(R"js(%(zelph/run-delta))js");
        interactive.process(R"js(%(report "ac" "a" "step" "c"))js");
        CHECK(any_output_contains(collector, "ac=yes"));

        // A second link, added and seeded separately, must still close the
        // three-step chain against what the previous delta produced.
        interactive.process(R"js(%(zelph/fact "c" "step" "d"))js");
        interactive.process(R"js(%(zelph/run-delta))js");
        interactive.process(R"js(%(report "ad" "a" "step" "d"))js");
        CHECK(any_output_contains(collector, "ad=yes")); });
}
