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

TEST_CASE("? prefix: a contradiction derived in the quiet pass is printed")
{
    // The quiet pass drops the Out and Diagnostic channels, and a
    // contradiction it derived accompanied them: counted, recorded in the
    // graph, never displayed -- even though `.help .deductions` says that a
    // contradiction's `!` line appears in every mode except off. These
    // entries are now marked as findings inside the output event, and the
    // quiet pass keeps what is marked.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(:p A) => !");
        interactive.process("(:p A) => (:q A)");

        SUBCASE("a contradiction prints as after any other line")
        {
            collector.clear();
            interactive.process("? :p a");
            CHECK(any_output_starts_with(collector, "! ⇐ (:p a)"));
            CHECK(has_contradiction(collector));
            // What is not a finding stays quiet.
            CHECK_FALSE(any_output_starts_with(collector, "(:q a) ⇐"));
            CHECK_FALSE(any_output_contains(collector, "_Result"));
        }
        SUBCASE("with .deductions off, as after any other line: the notice only")
        {
            interactive.process(".deductions off");
            collector.clear();
            interactive.process("? :p a");
            CHECK(has_contradiction(collector));
            CHECK_FALSE(any_output_contains(collector, "⇐"));
        }
        SUBCASE("after a '?' in the default mode, an ordinary run under off prints the notice only")
        {
            // The quiet pass activates contradiction printing for its own run
            // and must deactivate it afterwards. The subcase mentioned above
            // deactivates before the '?', thus unable to detect whether that
            // occurs: in this instance, the '?' comes first, and the
            // subsequent run would print the '!' line although
            // `.deductions off` promises only the notice.
            interactive.process("? :p a");
            interactive.process(".deductions off");
            collector.clear();
            interactive.process(":p b");
            CHECK(has_contradiction(collector));
            CHECK_FALSE(any_output_contains(collector, "⇐"));
        } });
}

TEST_CASE("? prefix: a declaration that contradicts the simplifier is reported by the query")
{
    // The case that made the loss visible: symbolic-core states that simp is
    // single-valued, (T simp A, T simp B, A != B) => !, and a declaration
    // conflicting with a rewrite gives the term a second normal form. When
    // queried with '?', the response returned both normal forms and said
    // nothing additional.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import symbolic-core");
        process_lines(interactive, R"(
x ~ symvar
z ~ symconst
(x * &1) = z
)");
        collector.clear();
        interactive.process("? :simplify (x * &1)");
        CHECK(answers_contain(collector, "(:simplify (x * &1)) = x"));
        CHECK(answers_contain(collector, "(:simplify (x * &1)) = z"));
        CHECK(any_output_starts_with(collector, "! ⇐"));
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("? prefix: check mode lists the facts it reports, also from the quiet pass")
{
    // Check mode ends a run when the outcome includes a fact that the
    // rules no longer support, triggering an error that cites the facts
    // "listed above" -- however, during the quiet pass, that list was
    // excluded, resulting in the error lacking a reference. The program is
    // the one test_stratified.cpp uses for this report: r is derived via
    // ¬q, and q is derived via r.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(:p A, ¬(:q A)) => (:r A)");
        interactive.process("(:r A) => (:q A)");
        collector.clear();
        CHECK_THROWS_WITH_AS(interactive.process("? :p w"), doctest::Contains("negated premise"), std::runtime_error);
        CHECK(any_output_starts_with(collector, "Negation check:"));
        CHECK(any_output_contains(collector, "w r w")); });
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

// "?" asserts the statement it queries and subsequently requests its value,
// whereas a condition operator has neither. "? ¬(a p b)" refuted `a p b`
// during the quiet pass and only later failed, on its value; "? X go Y, ¬(a p
// b)" asserted `a p b` and then failed. Rejected before any pass: no assertion
// occurs, no refutation takes place. The self-path ":p⁺ m" functions as a path
// condition too, and "? X go Y, :p⁺ m" passed the check and asserted `m p m`.
TEST_CASE("? prefix: a condition operator is refused before anything is asserted")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        const std::size_t before = node_count(collector, interactive);

        for (const char* request : {"? ¬(a p b)", "? X go Y, ¬(a p b)", "? (a p b) ~ negation", "? a P279⁺ d", "? X go Y, :p⁺ m"})
        {
            CAPTURE(request);
            CHECK_THROWS_WITH(interactive.process(request), doctest::Contains("'?' asserts the statement it asks about"));
        }
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("S ~ refuted");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty()); });
}

// A rule is a statement that "?" may query, with its operators occupying its
// own condition slots. The quiet pass puts it in force as a typed rule; the
// subsequent request mentions it within "(...) = _Result", interpreting the
// mentioned rule's operators as those of a typed rule. It was rejected, with
// the advice to write it on a separate line, since a mentioned rule could not
// contain an operator; "? (X go Y) => (X ok Y)" was consistently accepted.
TEST_CASE("? prefix: a rule with a condition operator is asserted as a typed rule is")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");

        CHECK_NOTHROW(interactive.process("? (X go Y, ¬(X p b)) => (X ok Y)"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "No rules found"));
        collector.clear();
        interactive.process("S ok O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"t ok k"}); });
}

// A focus on a rule's arrow names the rule, so "?" asserts that rule just as
// it asserts one written without the focus. The guard against a condition
// operator reads the grammar's tree, and that is where the arrow is read as
// the arrow: read as a focus, the line was refused for the "¬" within the
// rule's own condition, and without such an operator, each typing put
// several rules in force, none of which were checked for a twin.
TEST_CASE("? prefix: a rule with the focus on its arrow is asserted as a typed rule is")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a condition operator in the rule's own condition")
        {
            interactive.process("t go k");

            CHECK_NOTHROW(interactive.process("? (X go Y, ¬(X p b)) *=> (X ok Y)"));

            collector.clear();
            interactive.process("S ok O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"t ok k"});
        }
        SUBCASE("typed twice under other names, one rule")
        {
            interactive.process("? (X go Y) *=> (X ok Y)");
            interactive.process("? (A go B) *=> (A ok B)");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(count_outputs_containing(collector, "=>") == 1);
        } });
}
