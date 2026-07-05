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
