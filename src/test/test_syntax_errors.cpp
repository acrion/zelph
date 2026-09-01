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

#include "test_helpers.hpp"

#include <string>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// What a malformed statement is told
//
// These are about the MESSAGE, not about reasoning: nothing here reaches the
// fixpoint loop. What is pinned is that a fragment the code generator cannot
// turn into a statement is named -- the tokens as they were typed and the role
// the fragment plays -- instead of being reported as the arity of the call the
// generator was building. The messages themselves live in
// src/lib/script/syntax_errors.cpp.
// ---------------------------------------------------------------------------

namespace
{
    // The "Nodes: N" line of .stat, for asserting that a refusal built
    // nothing.
    std::string last_stat_nodes(const zelph::io::OutputCollector& collector)
    {
        for (const auto& e : collector.events())
        {
            const std::string t = zelph::test::normalize(e.text);
            if (t.rfind("Nodes:", 0) == 0) return t;
        }
        return {};
    }
}

TEST_CASE("syntax errors: a statement of two parts is named, not reported as an arity")
{
    // "(A father, B father C) => (A grandfather C)" is the most frequent
    // error there is -- the object of the first condition omitted -- and what
    // returned was Janet's own complaint about the call the generator had
    // built: "arity mismatch, expected at least 3, got 2". It names an
    // internal calling convention, does not say which of the two conditions
    // is meant, and does not say what is missing.
    //
    // The PEG's syntax tree knows both: the tokens as they were typed, and
    // the ROLE the fragment plays. Each surface form below therefore names
    // itself, and all of them share one sentence -- see
    // src/lib/script/syntax_errors.cpp.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The role comes from the AST node the fragment sits in.
        CHECK_THROWS_WITH_AS(interactive.process("(A father, B father C) => (A grandfather C)"),
                             doctest::Contains("condition 1 of the comma list, \"A father\""),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process("(A father B, B father) => (A grandfather C)"),
                             doctest::Contains("condition 2 of the comma list, \"B father\""),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process("(A father B, B father C) => (A grandfather)"),
                             doctest::Contains("the consequence, \"A grandfather\""),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process("(A father) => (B q C)"),
                             doctest::Contains("the condition, \"A father\""),
                             std::runtime_error);

        // A term in subject or in object position has no role to name, so the
        // fragment stands on its own.
        CHECK_THROWS_WITH_AS(interactive.process("(x q) r y"),
                             doctest::Contains("\"x q\" is a subject and a predicate"),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process("x (a p) y"),
                             doctest::Contains("\"a p\" is a subject and a predicate"),
                             std::runtime_error);

        // The way out is offered in the user's own tokens, because a two-part
        // statement IS a statement under exactly one reading.
        CHECK_THROWS_WITH_AS(interactive.process("(x q) r y"),
                             doctest::Contains("self-fact \":q x\", which is \"x q x\""),
                             std::runtime_error);

        // A variable is not an :atom in the tree, and reading only atoms
        // printed "(...) father" -- a message naming nothing that was typed.
        CHECK_THROWS_WITH_AS(interactive.process("(A father) => (B q C)"),
                             doctest::Contains("\"A father\""),
                             std::runtime_error);

        // A COMPOSITE fragment is rebuilt from the tree, in the brackets it
        // was read out of. There is no node to hand to the graph's renderer
        // here -- the refusal comes before the fragment is built, which is
        // what keeps a rejected line from leaving anything behind.
        CHECK_THROWS_WITH_AS(interactive.process("(x (q r s)) t y"),
                             doctest::Contains("\"x (q r s)\""),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process("(<a b> p) q r"),
                             doctest::Contains("\"<a b> p\""),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process("({a b} p) q r"),
                             doctest::Contains("\"{a b} p\""),
                             std::runtime_error);

        // ... and then the way out is NOT offered, because it could not be
        // typed: the self-fact sugar has nowhere to put quotes, so its
        // predicate has to be one bare token. The gate is the display's own,
        // so the advice and the printed form cannot disagree.
        CHECK_THROWS_WITH_AS(interactive.process("(x (q r s)) t y"),
                             doctest::Contains("Write the object after it."),
                             std::runtime_error);
        CHECK_THROWS_AS(interactive.process("(a \"is father of\") => (B q C)"), std::runtime_error);
        collector.clear();
        try
        {
            interactive.process("(a \"is father of\") => (B q C)");
        }
        catch (const std::exception& e)
        {
            CHECK(std::string(e.what()).find("self-fact") == std::string::npos);
        }

        // And the complaint that used to arrive is gone from all of them.
        collector.clear();
        CHECK_THROWS_AS(interactive.process("(A father, B father C) => (A grandfather C)"), std::runtime_error);
        CHECK_FALSE(any_event_contains(collector, "arity mismatch"));

        // A refused statement leaves the graph unchanged -- the names within
        // it are not formed during the act of refusal. Worth pinning, and
        // worth saying what it does NOT reveal: it was present prior to this
        // modification, so it does not indicate WHERE the refusal is emitted.
        // What makes the generation-time placement visible is the message
        // itself, which can only be written where the syntax tree still
        // exists.
        collector.clear();
        interactive.process(".stat");
        const std::string before = last_stat_nodes(collector);
        REQUIRE_FALSE(before.empty());

        CHECK_THROWS_AS(interactive.process("(Sub Pred) => (x q y)"), std::runtime_error);

        collector.clear();
        interactive.process(".stat");
        CHECK(last_stat_nodes(collector) == before);

        // What must NOT change: two parts at the TOP level are the beginning
        // of a statement, and the REPL waits for the rest of the line. That
        // is what makes a statement spannable, and it is the one place where
        // "a p" is not a mistake.
        collector.clear();
        interactive.process("a p");
        interactive.process("b");
        CHECK(any_output_starts_with(collector, "a p b"));

        // A command that takes a pattern says the same thing. It used to
        // answer "Could not parse pattern", because the two attempts it makes
        // -- as written, and unwrapped once -- both fail and neither reason
        // was kept.
        CHECK_THROWS_WITH_AS(interactive.process(".prune-facts (a p)"),
                             doctest::Contains("is a subject and a predicate"),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process(".prune-nodes (a p)"),
                             doctest::Contains("is a subject and a predicate"),
                             std::runtime_error); });
}
