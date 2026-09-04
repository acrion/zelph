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
// The '?' result-query prefix: "? <statement>" rewrites to the query
// "(<statement>) = _Result", processed twice -- a quiet pass (materialize
// the request, infer to the fixpoint) and an answer pass. The quiet pass
// must leak neither echoes nor deduction traces nor premature answers.
// ---------------------------------------------------------------------------

TEST_CASE("? prefix: arithmetic one-liners, quiet pre-pass (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        SUBCASE("parenthesized and bare statement forms")
        {
            collector.clear();
            interactive.process("? (&17 mod &5)");
            CHECK(answers_contain(collector, "(&17 mod &5) = &2"));

            collector.clear();
            interactive.process("? &12 * &3");
            CHECK(answers_contain(collector, "(&12 * &3) = &36"));
        }
        SUBCASE("the quiet pass suppresses deduction traces")
        {
            collector.clear();
            interactive.process("? (&12 * &3)");
            CHECK(answers_contain(collector, "(&12 * &3) = &36"));
            CHECK_FALSE(any_output_contains(collector, "⇐"));
            CHECK_FALSE(any_output_contains(collector, "skipped"));
            CHECK_FALSE(any_output_contains(collector, "_Result"));
        }
        SUBCASE("repeatable without duplicate answers")
        {
            interactive.process("? (&6 + &7)");
            collector.clear();
            interactive.process("? (&6 + &7)");
            CHECK(answers_contain(collector, "(&6 + &7) = &13"));
            CHECK_FALSE(any_output_contains(collector, "⇐"));
        }
        SUBCASE("partiality stays visible as silence: division by zero")
        {
            collector.clear();
            interactive.process("? (&5 / &0)");
            CHECK_FALSE(any_output_contains(collector, "Answer"));
        }
        SUBCASE("multi-line accumulation still works after '?'")
        {
            collector.clear();
            interactive.process("? (&2");
            interactive.process("+ &3)");
            CHECK(answers_contain(collector, "(&2 + &3) = &5"));
        } });
}

TEST_CASE("? prefix: self-fact requests (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import primes-naf");
        collector.clear();
        interactive.process("? :testprime &7");
        CHECK(answers_contain(collector, "(:testprime &7) = prime")); });
}

TEST_CASE("? prefix: symbolic pipeline and math-syntax islands (all arithmetic modules)" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import diff");
        interactive.process(".import math-syntax");
        interactive.process("x ~ symvar");

        collector.clear();
        interactive.process("? $( x*x ) diffby x");
        CHECK(answers_contain(collector, "((x * x) diffby x) = (x + x)"));

        collector.clear();
        interactive.process("? :simplify $( x + 0 )");
        CHECK(answers_contain(collector, "(:simplify (x + &0)) = x")); });
}

TEST_CASE("? prefix: the space after '?' is optional, as the trigger says")
{
    // The trigger accepts '(' and '$' directly after the '?' -- and then the
    // statement never completed: written without the space, the bracket opens
    // inside the token the '?' started, so the whole request counted as ONE
    // top-level token less than the spaced form and the accumulator waited
    // for a continuation that never came ("Input ends inside an unfinished
    // statement"). Both spellings are the same question.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b) = r");

        for (const char* form : {"?(a p b)", "? (a p b)", "? a p b"})
        {
            collector.clear();
            interactive.process(form);
            CHECK(answers_contain(collector, "(a p b) = r"));
        }

        // A bracket group FOLLOWED by more tokens is where the count was off
        // by one, and it is the shape the math tutorial uses
        // ("? $( ... ) ≡ $( ... )").
        interactive.process("((a p b) rel x) = done");
        for (const char* form : {"?(a p b) rel x", "? (a p b) rel x"})
        {
            collector.clear();
            interactive.process(form);
            CHECK(answers_contain(collector, "((a p b) rel x) = done"));
        } });
}

TEST_CASE("? prefix: error handling and non-trigger cases")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The Janet-level arity error additionally prints a stacktrace to
        // stderr -- expected noise for this negative test.
        CHECK_THROWS_AS(interactive.process("? (= x)"), std::runtime_error);

        // Atoms merely STARTING with '?' are not the prefix (no whitespace,
        // '(' or '$' follows): ordinary fact processing applies.
        interactive.process("?maybe ~ atom");
        collector.clear();
        interactive.process(R"js(%(string "RQ-ATOM-" (zelph/exists "?maybe" "~" "atom")))js");
        CHECK(any_output_contains(collector, "RQ-ATOM-true")); });
}
