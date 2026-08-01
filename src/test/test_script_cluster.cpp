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
// zelph/cluster, zelph/cluster-drop, zelph/clusters
//
// The graph is monotonic, so a program that asserts a fact base, reasons about
// it and reads the conclusions has no way to take the fact base back out
// again: every question it ever asked stays. Clusters are the answer the
// engine already has -- nodes CREATED while one is active are recorded in it,
// and dropping it removes exactly those -- but until now they existed only as
// REPL commands, which meant the capability was unreachable from a program
// driving zelph as a library.
//
// These tests pin the three properties that make a cluster usable as scratch
// space: what is created inside it goes away, what existed before it does not,
// and the caller can tell how much went away.
// ---------------------------------------------------------------------------

namespace
{
    constexpr const char* kReport =
        R"js(%(defn report [tag s p o] (zelph/out (string tag "=" (if (zelph/exists s p o) "yes" "no")))))js";
} // namespace

TEST_CASE("zelph/cluster: what is created inside a cluster is removed by dropping it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        interactive.process(R"js(%(zelph/cluster "scratch"))js");
        interactive.process(R"js(%(zelph/fact "ephemeral" "~" "thing"))js");
        interactive.process(R"js(%(report "inside" "ephemeral" "~" "thing"))js");
        CHECK(any_output_contains(collector, "inside=yes"));

        interactive.process(R"js(%(zelph/cluster nil))js");
        interactive.process(R"js(%(zelph/cluster-drop "scratch"))js");
        interactive.process(R"js(%(report "after" "ephemeral" "~" "thing"))js");
        CHECK(any_output_contains(collector, "after=no")); });
}

TEST_CASE("zelph/cluster: nodes that already existed survive the drop")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();
        interactive.process(kReport);

        // Created before any cluster is active, so it is never recorded in one
        // and a drop cannot reach it. This is the property that makes a
        // cluster safe as scratch space over a graph that was loaded from disk.
        interactive.process(R"js(%(zelph/fact "persistent" "~" "thing"))js");

        interactive.process(R"js(%(zelph/cluster "scratch"))js");
        interactive.process(R"js(%(zelph/fact "ephemeral" "~" "thing"))js");
        interactive.process(R"js(%(zelph/cluster nil))js");
        interactive.process(R"js(%(zelph/cluster-drop "scratch"))js");

        interactive.process(R"js(%(report "persistent" "persistent" "~" "thing"))js");
        interactive.process(R"js(%(report "ephemeral" "ephemeral" "~" "thing"))js");
        CHECK(any_output_contains(collector, "persistent=yes"));
        CHECK(any_output_contains(collector, "ephemeral=no")); });
}

TEST_CASE("zelph/cluster: returns the active name, and clusters lists them")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();

        // No cluster active: nil, and "default" deactivates rather than
        // creating a cluster of that name.
        interactive.process(R"js(%(zelph/out (string "active0=" (type (zelph/cluster)))))js");
        CHECK(any_output_contains(collector, "active0=nil"));

        interactive.process(R"js(%(zelph/out (string "active1=" (zelph/cluster "a"))))js");
        CHECK(any_output_contains(collector, "active1=a"));

        interactive.process(R"js(%(zelph/out (string "active2=" (type (zelph/cluster "default")))))js");
        CHECK(any_output_contains(collector, "active2=nil"));

        interactive.process(R"js(%(zelph/cluster "a"))js");
        interactive.process(R"js(%(zelph/fact "in-a" "~" "thing"))js");
        interactive.process(R"js(%(zelph/cluster nil))js");
        interactive.process(R"js(%(zelph/out (string "listed=" (length (zelph/clusters)))))js");
        CHECK(any_output_contains(collector, "listed=1")); });
}

TEST_CASE("zelph/cluster-drop: reports how many nodes it removed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        collector.clear();

        interactive.process(R"js(%(zelph/out (string "empty=" (zelph/cluster-drop "never-existed"))))js");
        CHECK(any_output_contains(collector, "empty=0"));

        interactive.process(R"js(%(zelph/cluster "scratch"))js");
        interactive.process(R"js(%(zelph/fact "one" "~" "thing"))js");
        interactive.process(R"js(%(zelph/cluster nil))js");
        interactive.process(R"js(%(zelph/out (string "removed>0=" (> (zelph/cluster-drop "scratch") 0))))js");
        CHECK(any_output_contains(collector, "removed>0=true")); });
}

TEST_CASE("zelph/cluster: the default cluster cannot be dropped, and arities are fixed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS(interactive.process(R"js(%(zelph/cluster-drop "default"))js"));
        CHECK_THROWS(interactive.process(R"js(%(zelph/cluster-drop))js"));
        CHECK_THROWS(interactive.process(R"js(%(zelph/cluster "a" "b"))js"));
        CHECK_THROWS(interactive.process(R"js(%(zelph/clusters "unexpected"))js")); });
}
