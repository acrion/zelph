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
// Inline keywords ("expression islands"): the 3-argument form of
// zelph/register-keyword. An island (open ... close) inside a zelph
// statement is passed to its Janet handler, whose zelph/node result is
// spliced back into the statement via the unquote mechanism. This is the
// host mechanism behind stdlib term islands like $( ... ): the island
// GRAMMAR lives in scripts, only the splice lives in C++.
// ---------------------------------------------------------------------------

namespace
{
    // Minimal island: resolve the trimmed content as a node name.
    constexpr const char* kAtomIsland =
        R"js(%(zelph/register-keyword "$(" ")" (fn [t] (zelph/resolve (string/trim t)))))js";

    // Paren-balanced island: vetoes with :incomplete until the content is
    // balanced -- the pattern real island grammars use. Side-effect-free
    // before acceptance, as the contract requires.
    constexpr const char* kBalancedIsland =
        R"js(%(zelph/register-keyword "$(" ")" (fn [t] (var d 0) (each c t (case c 40 (++ d) 41 (-- d))) (if (= d 0) (zelph/resolve (string/trim t)) :incomplete))))js";
} // namespace

TEST_CASE("inline keyword: islands splice into value positions")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kAtomIsland);

        SUBCASE("subject and object of a top-level fact")
        {
            interactive.process(R"($( berlin ) "is capital of" $( germany ))");
            collector.clear();
            interactive.process(R"js(%(string "IK-SPO-" (zelph/exists "berlin" "is capital of" "germany")))js");
            CHECK(any_output_contains(collector, "IK-SPO-true"));
        }
        SUBCASE("inside a nested fact")
        {
            interactive.process(R"((tom saw $( sunrise )) ~ verified)");
            collector.clear();
            interactive.process(R"js(%(string "IK-NEST-" (zelph/exists (zelph/fact "tom" "saw" "sunrise") "~" "verified")))js");
            CHECK(any_output_contains(collector, "IK-NEST-true"));
        }
        SUBCASE("a lone island is a complete statement and echoes its node")
        {
            collector.clear();
            interactive.process("$( solo )");
            CHECK(any_output_contains(collector, "solo"));
        } });
}

TEST_CASE("inline keyword: veto protocol extends the island across nested close delimiters")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(kBalancedIsland);
        interactive.process(R"($( a(b)c ) ~ tricky)");
        collector.clear();
        // The handler saw the FULL balanced content in one piece; the
        // truncated first split (" a(b") must never have been resolved --
        // the veto happens before any side effect.
        interactive.process(R"js(%(string "IK-VETO-" (zelph/exists "a(b)c" "~" "tricky")))js");
        interactive.process(R"js(%(string "IK-VETO-NOT-" (zelph/exists "a(b" "~" "tricky")))js");
        CHECK(any_output_contains(collector, "IK-VETO-true"));
        CHECK(any_output_contains(collector, "IK-VETO-NOT-false")); });
}

TEST_CASE("inline keyword: island handlers share the statement's variable scope")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The island builds the rule CONDITION pattern (A likes <content>);
        // the consequence outside the island reuses A. Both must bind the
        // SAME variable node, otherwise the rule never fires.
        interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] (zelph/fact 'A "likes" (zelph/resolve (string/trim t))))))js");
        interactive.process(R"($( beer ) => (A celebrates A))");
        interactive.process("tom likes beer");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(R"js(%(string "IK-VAR-" (zelph/exists "tom" "celebrates" "tom")))js");
        CHECK(any_output_contains(collector, "IK-VAR-true")); });
}

TEST_CASE("inline keyword: no expansion inside quoted atoms or comments")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(var ik-calls 0))js");
        interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] (++ ik-calls) (zelph/resolve (string/trim t)))))js");

        interactive.process(R"js("prefix $( not an island )" ~ quoted)js");
        collector.clear();
        interactive.process(R"js(%(zelph/out (string "IK-CALLS-A-" ik-calls)))js");
        CHECK(any_output_contains(collector, "IK-CALLS-A-0"));

        interactive.process(R"($( real ) ~ island)");
        collector.clear();
        interactive.process(R"js(%(zelph/out (string "IK-CALLS-B-" ik-calls)))js");
        interactive.process(R"js(%(string "IK-REAL-" (zelph/exists "real" "~" "island")))js");
        CHECK(any_output_contains(collector, "IK-CALLS-B-1"));
        CHECK(any_output_contains(collector, "IK-REAL-true")); });
}

TEST_CASE("inline keyword: registration and use from an imported script")
{
    namespace fs          = std::filesystem;
    const fs::path script = fs::temp_directory_path() / "zelph_test_inline_island.zph";
    {
        std::ofstream f(script);
        f << R"js(%(zelph/register-keyword "$(" ")" (fn [t] (zelph/resolve (string/trim t)))))js" << "\n";
        f << R"($( imported-a ) linked $( imported-b ))" << "\n";
    }

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(".import \"" + script.string() + "\"");
        collector.clear();
        interactive.process(R"js(%(string "IK-IMP-" (zelph/exists "imported-a" "linked" "imported-b")))js");
        CHECK(any_output_contains(collector, "IK-IMP-true"));

        // The registration persists after the import.
        interactive.process(R"($( post ) linked $( hoc ))");
        collector.clear();
        interactive.process(R"js(%(string "IK-POST-" (zelph/exists "post" "linked" "hoc")))js");
        CHECK(any_output_contains(collector, "IK-POST-true")); });

    std::error_code ec;
    fs::remove(script, ec);
}

TEST_CASE("inline keyword: error cases")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("handler that never accepts")
        {
            interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] :incomplete)))js");
            CHECK_THROWS_AS(interactive.process("$( x ) ~ y"), std::runtime_error);
        }
        SUBCASE("missing closing delimiter")
        {
            interactive.process(R"js(%(zelph/register-keyword "$[" "]" (fn [t] (zelph/resolve t))))js");
            CHECK_THROWS_AS(interactive.process("$[ x ~ y"), std::runtime_error);
        }
        SUBCASE("handler result must be a zelph/node")
        {
            interactive.process(R"js(%(zelph/register-keyword "$(" ")" (fn [t] "not-a-node")))js");
            CHECK_THROWS_AS(interactive.process("$( x ) ~ y"), std::runtime_error);
        } });
}
