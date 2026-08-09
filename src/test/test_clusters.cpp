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

TEST_CASE("clusters: drop removes cluster-created facts, keeps prior knowledge")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
keep1 relK keep2
.cluster exp
tmp1 relT tmp2
)");
        collector.clear();
        interactive.process(".cluster-drop exp");

        collector.clear();
        interactive.process("X relT Y");
        CHECK_FALSE(answers_contain(collector, "tmp1 relT tmp2"));

        collector.clear();
        interactive.process("X relK Y");
        CHECK(answers_contain(collector, "keep1 relK keep2")); });
}

TEST_CASE("clusters: merge into default keeps facts, forgets membership")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.cluster exp
tmp1 relM tmp2
.cluster-merge exp default
)");
        collector.clear();
        interactive.process("X relM Y");
        CHECK(answers_contain(collector, "tmp1 relM tmp2"));

        collector.clear();
        interactive.process(".cluster");
        CHECK_FALSE(any_output_contains(collector, "exp")); });
}

TEST_CASE("clusters: pre-existing facts are never recorded, so drop keeps them")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a relP b
.cluster exp
a relP b
.cluster-drop exp
)");
        // Re-asserting an existing fact inside the cluster must not
        // hand its nodes to the cluster: after drop it still exists.
        collector.clear();
        interactive.process("X relP Y");
        CHECK(answers_contain(collector, "a relP b")); });
}

TEST_CASE("clusters: dropping an unknown cluster is an error, not a silent no-op")
{
    // ".cluster-merge" already rejects an unknown name. ".cluster-drop"
    // reported "removed 0 node(s)" instead -- indistinguishable from the
    // honest report for a cluster that exists but is empty, so a typo read
    // as a successful rollback while the experiment kept accumulating.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH_AS(interactive.process(".cluster-drop never-created"),
                             doctest::Contains("unknown cluster"),
                             std::runtime_error);

        // An EMPTY but existing cluster still drops successfully.
        interactive.process(".cluster empty-but-real");
        collector.clear();
        interactive.process(".cluster-drop empty-but-real");
        CHECK(any_output_contains(collector, "removed 0 node(s)"));
        CHECK_FALSE(any_output_contains(collector, "unknown cluster")); });
}

TEST_CASE("clusters: dropping the active cluster reports the fallback to default")
{
    // take_cluster deactivates the cluster it removes. Without saying so,
    // every node created afterwards is untracked while the user still
    // believes an experiment workspace is active.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        interactive.process("tmp1 relA tmp2");
        collector.clear();
        interactive.process(".cluster-drop exp");
        CHECK(any_output_contains(collector, "Active cluster: default"));

        collector.clear();
        interactive.process(".cluster");
        CHECK(any_output_contains(collector, "Active cluster: default")); });
}

// ---------------------------------------------------------------------------
// A cluster records what it CREATED, and that is what lets a drop promise
// never to destroy pre-existing knowledge. One change it makes to a
// PRE-EXISTING node has to be undone all the same: asserting a statement that
// was only a rule's ground pattern revokes that marking, and the node existed,
// so nothing was recorded and the drop undid nothing. An experiment could turn
// a rule's patterns into data permanently.
//
// The line the contract draws: a marking is the ENGINE's own bookkeeping about
// a node, not a claim anybody made. Names and merges stay outside, as before.
// ---------------------------------------------------------------------------

TEST_CASE("clusters: drop restores a rule pattern the experiment claimed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");

        // The control: both patterns are invisible to queries beforehand.
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        interactive.process(".cluster exp");
        interactive.process("a p b");

        // Inside the experiment it IS data, and the rule fires on it.
        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "c q d"));

        interactive.process(".cluster-drop exp");

        // ... and afterwards both are patterns again. The CONSEQUENCE too:
        // a ground consequence is materialized with the rule, so deriving it
        // creates no node either, and its marking was revoked the same way.
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));

        // The rule itself is untouched -- it was never the cluster's.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (c q d)")); });
}

TEST_CASE("clusters: merge commits the claim the experiment made")
{
    // Merging into `default` is how an experiment is KEPT, so the revocation
    // travels with the nodes instead of being rolled back.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) => (c q d)");
        interactive.process(".cluster exp");
        interactive.process("a p b");
        interactive.process(".cluster-merge exp default");

        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK_FALSE(any_output_contains(collector, "[rule pattern; not asserted]")); });
}

TEST_CASE("clusters: a pattern the drop removed is not marked again")
{
    // The restore runs AFTER the removals and skips what is gone. It has to:
    // a rule built inside the cluster records its patterns, and asserting one
    // of them inside the same cluster revokes the marking of a node the
    // cluster itself created.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        interactive.process("(a p b) => (c q d)");
        interactive.process("a p b");
        interactive.process(".cluster-drop exp");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());

        // Nothing was resurrected by the restore either.
        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty()); });
}
