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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Stratified negation-as-failure
//
// Rules with negated conditions form a deferred stratum: they are evaluated
// only when the positive rules have reached quiescence, so ¬(pattern) tests
// absence against the SATURATED positive fact base. These tests pin the
// probe scripts that demonstrated the former race (a ¬-rule firing before
// the facts matching its negated pattern were derived) as regressions.
// ---------------------------------------------------------------------------

// NOTE on negative checks: entering a rule ECHOES its definition on the
// Out channel -- "... => (A racewin A)" contains the consequence
// predicate name. A CHECK_FALSE on the bare predicate therefore
// false-positives on the echo. Negative checks must match the
// INSTANTIATED fact ("x racewin x"), never the predicate alone.

TEST_CASE("stratified: negation defers until positive quiescence (probe A)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A trigger A) => (A step1 A)
(A step1 A) => (A step2 A)
(A trigger A, ¬(A step2 A)) => (A racewin A)
x trigger x
)");
        CHECK(any_output_contains(collector, "x step2 x"));
        CHECK_FALSE(any_output_contains(collector, "x racewin x")); });
}

TEST_CASE("stratified: rule definition order does not matter (probe A')")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Same rules as probe A, but the negation rule is defined FIRST.
        // Under the racy engine, rule order (an unordered set internally)
        // could decide the outcome; under stratification it cannot.
        process_lines(interactive, R"(
(A trigger A, ¬(A step2 A)) => (A racewin A)
(A trigger A) => (A step1 A)
(A step1 A) => (A step2 A)
x trigger x
)");
        CHECK(any_output_contains(collector, "x step2 x"));
        CHECK_FALSE(any_output_contains(collector, "x racewin x")); });
}

TEST_CASE("stratified: deep positive chains are saturated first (probe B)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A s0 A) => (A s1 A)
(A s1 A) => (A s2 A)
(A s2 A) => (A s3 A)
(A s3 A) => (A s4 A)
(A s0 A, ¬(A s4 A)) => (A racewin A)
y s0 y
)");
        CHECK(any_output_contains(collector, "y s4 y"));
        CHECK_FALSE(any_output_contains(collector, "y racewin y")); });
}

TEST_CASE("stratified: completion-witness pattern is order-independent (probe C)")
{
    // Under the racy engine this case happened to be correct only because
    // the bad-rule was internally ordered before the seen/done chain --
    // pure hash-order luck. Stratification makes it correct by semantics.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A go A) => (A m A)
(A m A) => (A bad A)
(A m A) => (A seen A)
(A seen A) => (A done A)
(A done A, ¬(A bad A)) => (A verdict A)
z go z
)");
        CHECK(any_output_contains(collector, "z bad z"));
        CHECK_FALSE(any_output_contains(collector, "z verdict z")); });
}

TEST_CASE("stratified: latecomer facts replay the schedule (probe D)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A trigger A) => (A step1 A)
(A step1 A) => (A step2 A)
(A trigger A, ¬(A step2 A)) => (A racewin A)
x trigger x
)");
        // A fresh input after the fixpoint starts a new engine run; the
        // deferred schedule must hold there as well -- for the newcomer
        // AND (still) for x.
        collector.clear();
        interactive.process("x2 trigger x2");
        CHECK(any_output_contains(collector, "x2 step2 x2"));
        CHECK_FALSE(any_output_contains(collector, "racewin")); });
}

TEST_CASE("stratified: negation still fires when the pattern is truly absent")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Base facts BEFORE the trigger: stratification fixes races among
        // DERIVED facts; user assertions remain subject to plain NAF
        // semantics at the time of each run.
        process_lines(interactive, R"(
(A trigger A, ¬(A blocked A)) => (A allowed A)
gated blocked gated
gated trigger gated
free trigger free
)");
        CHECK(any_output_contains(collector, "free allowed free"));
        CHECK_FALSE(any_output_contains(collector, "gated allowed gated")); });
}

TEST_CASE("stratified: a negated statement subject with a variable predicate fails where the statement holds")
{
    // The negated pattern ¬((X R Y) noted yes) succeeded for every binding:
    // it has a statement subject with a variable predicate, and no fact was
    // matched by such a pattern, hence `j unnoted k` was derived in
    // addition to the asserted `(j sub k) noted yes` that refutes it. Where
    // the statement is missing, as in `c sub d`, the negation still
    // succeeds.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
sub is memrel
j sub k
c sub d
(j sub k) noted yes
(R is memrel, ¬((X R Y) noted yes), X R Y) => (X unnoted Y)
)");
        collector.clear();
        interactive.process("S unnoted O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c unnoted d"}); });
}

TEST_CASE("stratified: deferred consequences re-open the positive stratum")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The negation rule's output feeds an ordinary positive rule:
        // phase 2 -> delta -> phase 1 must cascade.
        process_lines(interactive, R"(
(A start A) => (A p A)
(A p A, ¬(A q A)) => (A r A)
(A r A) => (A s A)
w start w
)");
        CHECK(any_output_contains(collector, "w r w"));
        CHECK(any_output_contains(collector, "w s w")); });
}

TEST_CASE("stratified: a free variable inside a negation is quantified inside it")
{
    // `¬` has ONE reading: the condition succeeds exactly when no fact
    // matches, and a free variable inside it produces no binding that
    // leaves the rule. Both directions therefore answer the way their names
    // suggest on the same graph.
    //
    // There used to be a second reading, chosen by whether the pattern's
    // SUBJECT happened to be bound: with it free, the engine took the
    // subjects of that relation as a domain and let the negation succeed
    // once per candidate the pattern failed for, binding it. `is earliest`
    // then came out for all three intervals, one of them justified by the
    // self-fact `¬(b before b)` -- and adding a positive condition
    // elsewhere silently switched between the two.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a ~ interval
b ~ interval
c ~ interval
a before b
b before c
(A ~ interval, ¬(A before B)) => (A is latest)
(A ~ interval, ¬(B before A)) => (A is earliest)
)");
        CHECK(any_output_contains(collector, "c is latest"));
        CHECK_FALSE(any_output_contains(collector, "a is latest"));
        CHECK_FALSE(any_output_contains(collector, "b is latest"));

        CHECK(any_output_contains(collector, "a is earliest"));
        CHECK_FALSE(any_output_contains(collector, "b is earliest"));
        CHECK_FALSE(any_output_contains(collector, "c is earliest")); });
}

// What `¬(F)` means when it stands on its own line rather than in a rule
// condition. It used to mean the opposite of itself: the sugar builds its
// operand with zelph/fact and tags the result, so the line ASSERTED F and then
// marked the node it had just claimed as negated -- `.node` said "Negated by a
// rule: yes" on a fact that answered every positive query.
//
// It now reaches the mechanism zelph has always had for a negative claim: the
// probability argument of Zelph::fact, and Answer::is_wrong over it.
TEST_CASE("negation: ¬(F) on its own line claims that F does not hold")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
¬(a p b)
c p d
(X p Y) => (X q Y)
)");
        // The refuted fact answers nothing and no rule fires on it, while the
        // ordinary one beside it does both.
        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "c p d"));
        CHECK_FALSE(any_output_contains(collector, "a p b"));

        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "c q d"));
        CHECK_FALSE(any_output_contains(collector, "a q b"));

        collector.clear();
        interactive.process(R"(%(if (zelph/exists "a" "p" "b") "holds" "does-not-hold"))");
        CHECK(any_output_contains(collector, "does-not-hold"));

        // And it prints as what it is. The echo used to come back as `a p b`,
        // which re-enters as the OPPOSITE of the line that produced it -- the
        // round trip is part of the notation, not a convenience.
        collector.clear();
        interactive.process("¬(x q y)");
        CHECK(any_output_contains(collector, "¬(x q y)"));

        // And a command can still ADDRESS it by printing it back. Every
        // command that takes a fact pattern resolves it by generating the same
        // code a statement generates, which runs through zelph/fact -- so the
        // moment a fact could be refuted, ".node" and ".explain" answered
        // "Unknown node" for exactly the facts a user has most reason to look
        // at. They look the pattern UP now instead of asserting it.
        collector.clear();
        interactive.process(".node a p b");
        CHECK(any_output_contains(collector, "Refuted (claimed not to hold): yes"));
        CHECK(any_output_contains(collector, "¬(a p b)"));

        // And the graph refuses to claim both. Zelph::fact has always had this
        // guard; nothing could reach it before, because no spelling created a
        // fact below probability 0.5.
        CHECK_THROWS(interactive.process("a p b")); });
}

// A typed rule whose statement is rejected by fact() -- specifically due to one
// of its conditions being a fact that the graph holds as incorrect -- failed
// after its other conditions were built, and the construction preserved those
// conditions: `k is on`, stated solely as a condition of a rule that never
// materialized, was the answer to `k is O`, as data. A refused rule retains
// none of the elements its statement built.
TEST_CASE("negation: a typed rule that fact() refuses asserts none of its conditions")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("¬(a p b)");
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process("(k is on, a p b, X q Y) => (X r Y)"), doctest::Contains("this fact is known to be wrong"));

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("k is O");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "=>"));
        collector.clear();
        interactive.process("S ~ conjunction");
        CHECK(collect_answers(collector).empty()); });
}

// A refuted fact is one that the graph holds as NOT being true, thereby
// satisfying a negated condition applied to it. The failure occurred during
// the construction of the operand: after "¬(a p b)", both the rule "(X go Y,
// ¬(a p b)) => (X ok Y)" and the query "X go Y, ¬(a p b)" were halted with the
// message "this fact is known to be wrong", since the operand was treated as
// being asserted just like any other fact. An operand is a pattern, and it is
// retrieved from the graph where it is held. A positive condition concerning
// the refuted fact is rejected, as previously described (above).
TEST_CASE("negation: a refuted fact is negated in a condition as it stands")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "¬(a p b)\nt go k\n");

        interactive.process("(X go Y, ¬(a p b)) => (X ok Y)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S ok O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"t ok k"});

        collector.clear();
        interactive.process("X go Y, ¬(a p b)");
        CHECK(collect_answers(collector).size() == 1);

        // Consulted, not rebuilt: it remains the refuted fact, and neither
        // a pattern of the rule nor of the query.
        collector.clear();
        interactive.process(".node a p b");
        CHECK(any_output_contains(collector, "Refuted (claimed not to hold): yes"));
        CHECK_FALSE(any_output_contains(collector, "Rule pattern")); });
}

// The reading `¬(F)` got on its own line does not extend below the top level,
// and until it was refused there, the operator was not reported but DROPPED:
// the sugar builds its operand with `zelph/fact` and tags the result, so a
// statement that denies a fact left the graph claiming it. The residue was a
// negation tag on a node no rule negates, which `.node` then reported as
// "Negated by a rule: yes".
TEST_CASE("negation: ¬ inside a plain statement is refused, not silently dropped")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Object position, subject position, and one level further down --
        // the operator is dropped at whatever depth it sits, so the guard has
        // to ask the whole statement rather than its direct arguments.
        CHECK_THROWS(interactive.process("x q (¬(a p b))"));
        CHECK_THROWS(interactive.process("(¬(a p b)) q x"));
        CHECK_THROWS(interactive.process("x q (y r (¬(a p b)))"));

        // The assertion that matters is this one, not the throws above: the
        // damage was caused by the operand being BUILT, which happens before
        // any guard placed further in can fire. Nothing of `a p b` may be in
        // the graph.
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S q O");
        CHECK(collect_answers(collector).empty());

        // And the reading it does have remains untouched, in both places it
        // holds: on its own line, and as a rule condition.
        collector.clear();
        interactive.process("¬(a p b)");
        CHECK(any_output_contains(collector, "¬(a p b)"));

        process_lines(interactive, R"(
m q n
(M q N, ¬(M p N)) => (M r N)
)");
        collector.clear();
        interactive.process("S r O");
        CHECK(answers_contain(collector, "m r n")); });
}

// A line containing `¬(F)` by itself refutes F, and only a FACT may be
// refuted: a line with a variable poses a question. "¬(X p b)" was routed to
// zelph/refute regardless -- it refuted a pattern, and with `t p b` present
// in the graph it printed "Answer: ¬(t p b)", the refuted-fact rendering of
// a fact that holds. It is the query its syntax says, whether no `X p b`
// holds; binding nothing, it yields one answer when none matches and
// produces nothing otherwise.
TEST_CASE("negation: ¬ over a pattern with a variable is a query, not a refutation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("¬(X p b)");
        CHECK(collect_answers(collector) == std::vector<std::string>{"¬(X p b)"});

        interactive.process("t p b");
        collector.clear();
        interactive.process("¬(X p b)");
        CHECK(collect_answers(collector).empty());

        // The query refuted nothing.
        collector.clear();
        interactive.process("S ~ refuted");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"t p b"}); });
}

// "¬" negates a condition, and "¬(X)" holds none. The line contains a
// variable and functions as a query, and as such built nothing, printed
// nothing, and answered nothing, without a word. Two values form a statement
// with a segment absent, and that message names the part.
TEST_CASE("negation: ¬ over a lone variable is refused, not silently empty")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH(interactive.process("¬(X)"), doctest::Contains("needs one -- a pattern"));
        CHECK_THROWS_WITH(interactive.process("¬X"), doctest::Contains("needs one -- a pattern"));
        CHECK_THROWS_WITH(interactive.process("¬(X Y)"), doctest::Contains("object left off")); });
}

// A query functions as a condition list that undergoes evaluation just once,
// and it is BUILT before being assessed: "X go Y, ¬(a p b)" created `a p b`
// as data while progressing toward the negation, then returned nothing since
// it had built its own counterexample, and `A p B` began responding with `a
// p b` thereafter. The material a query builds beneath an operator -- the
// operand, and the negation fact itself -- constitutes a pattern, much like
// the components of a rule are.
TEST_CASE("negation: a query builds nothing beneath ¬ as data")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        collector.clear();
        interactive.process("X go Y, ¬(a p b)");
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("X ~ negation");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]")); });
}

// Only the elements generated by the query are marked. A fact asserted
// before the query remains data, and any negation applied to it fails as
// expected; a rule's pattern continues to be a pattern, so asserting it
// afterwards still fires the rule.
TEST_CASE("negation: a node that existed before a query keeps its status")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "t go k\na p b\n(c q d) => (e r f)\n");

        collector.clear();
        interactive.process("X go Y, ¬(a p b)");
        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});

        collector.clear();
        interactive.process("X go Y, ¬(c q d)");
        CHECK(collect_answers(collector).size() == 1);
        interactive.process("c q d");
        collector.clear();
        interactive.process("S r O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"e r f"}); });
}

// The positive part of a query is created, and thereby asserted, just as it
// has consistently been; solely what stands beneath the operator constitutes
// a pattern. The sequence in which the two appear is irrelevant, since which
// node stands beneath an operator is determined from the structure after the
// query is finalized. Marking what the operator's own invocation created
// would make the fact into a pattern in one arrangement and into data in the
// other.
TEST_CASE("negation: a query's positive part is asserted, whichever comes first")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        collector.clear();

        SUBCASE("positive first") { interactive.process("X go Y, (a p b), ¬(a p b)"); }
        SUBCASE("negation first") { interactive.process("X go Y, ¬(a p b), (a p b)"); }

        CHECK(collect_answers(collector).empty());
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"}); });
}

// A rule mentioned by the query outside every operator belongs to its
// positive side: the query creates it and thus asserts its ground parts,
// just as a mention within any query does. Within the scope of "¬"
// elsewhere on the line, its condition transformed into a pattern whereas
// its consequence remained data, since the traversal seeking the positive
// side stopped at the rule.
TEST_CASE("negation: a rule a query mentions outside ¬ keeps its ground parts as data")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        collector.clear();
        interactive.process("X knows ((a p b) => (c q d)), ¬(a p b)");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a p b"});
        collector.clear();
        interactive.process("C q D");
        CHECK(collect_answers(collector) == std::vector<std::string>{"c q d"}); });
}

// Beneath "¬" a mentioned rule is part of the operand, and everything in the
// operand is a pattern. "X go Y, ¬(x says ((a p b) => (c q d)))" marked `x
// says ...` as a pattern, but the facts contained within the rule were
// asserted: the walk from the operator stopped at the rule.
TEST_CASE("negation: a rule mentioned beneath ¬ in a query is part of the pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        collector.clear();
        interactive.process("X go Y, ¬(x says ((a p b) => (c q d)))");
        CHECK(collect_answers(collector).size() == 1);

        for (const char* query : {"A p B", "C q D", "S says O"})
        {
            CAPTURE(query);
            collector.clear();
            interactive.process(query);
            CHECK(collect_answers(collector).empty());
        } });
}

// Everything under the operator is a pattern, a fact embedded within the
// operand too: "X go Y, ¬(X p (a q b))" asserted `a q b`.
TEST_CASE("negation: a fact nested in a query's negated condition is a pattern too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        collector.clear();
        interactive.process("X go Y, ¬(X p (a q b))");
        CHECK(collect_answers(collector).size() == 1);

        collector.clear();
        interactive.process("A q B");
        CHECK(collect_answers(collector).empty()); });
}

// A variable free within a negation becomes quantified within that negation,
// whether in a query or in a rule (refer to "a free variable inside a
// negation is quantified inside it" above). "¬(X before Y)" asks whether no
// `before` fact holds at all, and it previously refuted a pattern instead.
TEST_CASE("negation: a free variable inside ¬ is quantified inside it in a query too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("¬(X before Y)");
        CHECK(collect_answers(collector).size() == 1);

        process_lines(interactive, "a ~ interval\nb ~ interval\nc ~ interval\na before b\nb before c\n");
        collector.clear();
        interactive.process("¬(X before Y)");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("A ~ interval, ¬(A before B)");
        const std::vector<std::string> answers = collect_answers(collector);
        REQUIRE(answers.size() == 1);
        CHECK(answers.front().find("(c ~ interval)") != std::string::npos); });
}

// A line lacking a variable makes an assertion, and a condition holds no
// significance in such a context -- in no part of it, across every form a
// line may assume. Each of these was accepted and claimed what the operator
// concerned itself with: the comma list asserted `t go k` and `a p b`, the
// set and the self-fact asserted their operand, and the refutation asserted
// the fact contained within the one it refuted. The refusal is determined
// before any construction, so not even `t go k` remains.
TEST_CASE("negation: a line without a variable refuses a condition operator anywhere in it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);

        SUBCASE("a comma list") { CHECK_THROWS(interactive.process("t go k, ¬(a p b)")); }
        SUBCASE("a set") { CHECK_THROWS(interactive.process("{(¬(a p b)) (c q d)}")); }
        SUBCASE("a self-fact") { CHECK_THROWS(interactive.process(":p ¬(a q b)")); }
        SUBCASE("the fact a refutation refutes") { CHECK_THROWS(interactive.process("¬(x says (¬(a p b)))")); }

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("S P O");
        for (const std::string& answer : collect_answers(collector))
        {
            CAPTURE(answer);
            CHECK(answer.find(" ~ ->") != std::string::npos);
        } });
}

// The fact `((P) ~ negation)` constitutes the negated condition
// (Zelph::negation_operand), thus when expressed explicitly it is refused where
// the sugar is: entered on its own line, "(a p b) ~ negation" asserted `a p b`
// beside the condition stating its absence. Within a query, it functions as the
// condition and means "¬".
TEST_CASE("negation: the negation written out is refused where ¬ is, and means ¬ in a query")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS(interactive.process("(a p b) ~ negation"));
        CHECK_THROWS(interactive.process("x says ((a p b) ~ negation)"));
        CHECK(node_count(collector, interactive) == before);

        collector.clear();
        interactive.process("X go Y, ((a p b) ~ negation)");
        CHECK(collect_answers(collector).size() == 1);
        CHECK(any_output_contains(collector, "¬(a p b)"));
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty()); });
}

// A focus on the operator's name alters the value the line produces, without
// affecting the fact it builds. "(a q b) ~ *negation" built the negation and
// asserted `a q b` alongside it, and in "(X p (a q b)) ~ *negation" the
// statement serves as the name `negation`, meaning the walk from it never
// arrived at the operator and `a q b` stayed data.
TEST_CASE("negation: a focus on the negation's name hides no operator")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::size_t before = node_count(collector, interactive);
        CHECK_THROWS(interactive.process("(a q b) ~ *negation"));
        CHECK(node_count(collector, interactive) == before);

        interactive.process("(X p (a q b)) ~ *negation");
        collector.clear();
        interactive.process("A q B");
        CHECK(collect_answers(collector).empty()); });
}

// A query that does not succeed during its construction retains no part of
// what was constructed. At that moment, the operator's own rejections are
// executed -- a set is not a statement to negate, a negation does not nest --
// and the operand they rejected had already been constructed: `a p b` was
// preserved as data.
TEST_CASE("negation: a query that fails keeps nothing it built")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        const std::size_t before = node_count(collector, interactive);

        CHECK_THROWS_WITH(interactive.process("X go Y, ¬{(a p b)}"), doctest::Contains("is not one"));
        CHECK_THROWS_WITH(interactive.process("X go Y, ¬((a p b) ~ negation)"), doctest::Contains("does not nest"));

        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process("A p B");
        CHECK(collect_answers(collector).empty()); });
}

// A set is DATA: it holds its members, and nothing interprets them as
// conditions. "{(X go Y) (¬(X p b))} => (X ok Y)" was permitted, displayed
// again without the "¬", and listed no rule -- the operator had simply
// vanished. Refused within a set or a collection literal wherever the literal
// stands, unless a rule inside it gives the operator a condition slot of its
// own.
TEST_CASE("negation: a condition operator inside a set literal is refused, in a rule as in a query")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        const std::size_t before = node_count(collector, interactive);

        for (const char* statement : {"{(X go Y) (¬(X p b))} => (X ok Y)",
                                      "X in @{(¬(X p b))}",
                                      "{(¬(a p b)) (c q d)}"})
        {
            CAPTURE(statement);
            CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("a set is DATA"));
        }
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        CHECK_NOTHROW(interactive.process("{((X go Y, ¬(X p b)) => (X ok Y))}")); });
}

// The operators written out are the same nodes as the sugar
// (Zelph::negation_operand), meaning that within a set they function as data
// in identical fashion. They were accepted: "{(X go Y) ((X p b) ~ negation)}
// => (X ok Y)" became a statement about a collection, and no rule was
// listed. When a query with a path condition is echoed, it prints the
// condition written out within a set, and upon being typed back, it became
// silently a different statement.
TEST_CASE("negation: a condition operator written out inside a set literal is refused too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("t go k");
        const std::size_t before = node_count(collector, interactive);

        for (const char* statement : {"{(X go Y) ((X p b) ~ negation)} => (X ok Y)",
                                      "X go Y, Z in {((a p b) ~ negation)}",
                                      "{(X go Y) ((a P279 d) closure one-or-more)}"})
        {
            CAPTURE(statement);
            CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("a set is DATA"));
        }
        CHECK(node_count(collector, interactive) == before);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found")); });
}

// "*{...} ~ conjunction" is the comma-separated list expressed in full: the
// focus returns the set, and this set constitutes the condition list,
// meaning its members are conditions rather than the data contained within a
// set literal. The symbol "¬" was refused as an operator within a set,
// whereas the negation expressed in full was accepted there.
TEST_CASE("negation: the comma list written out reads ¬ among its members")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "t go k\nu go v\nu p b\n");

        collector.clear();
        interactive.process("*{(X go Y) (¬(X p b))} ~ conjunction");
        CHECK(collect_answers(collector).size() == 1);

        interactive.process("(*{(X go Y) (¬(X p b))} ~ conjunction) => (X ok Y)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S ok O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"t ok k"}); });
}

// A rule a line mentions maintains its refusal of condition operators, and
// the message said what it states regarding a plain statement or a
// condition, neither of which applies here: it told a query to place the "¬"
// symbol before the condition that was already preceded by it.
TEST_CASE("negation: ¬ in a rule a line mentions is refused as an operator of a mentioned rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const char* statement : {"R knows ((X go Y, ¬(X p b)) => (X ok Y))",
                                      "r knows ((X go Y, ¬(X p b)) => (X ok Y))",
                                      "R knows ((X go Y, ≈net(X p b)) => (X ok Y))"})
        {
            CAPTURE(statement);
            CHECK_THROWS_WITH(interactive.process(statement), doctest::Contains("rule that this line mentions"));
        } });
}

TEST_CASE("negation: a pattern is looked up, not asserted, when a command resolves it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "c p d\n");
        collector.clear();

        // The other half of the same property, and the one that says the
        // lookup did not quietly become a second way to assert: naming a
        // pattern the graph does NOT hold still denotes its node -- the id is
        // the hash of the triple -- and must not put it into the graph.
        interactive.process(".explain (e p f)");
        CHECK(any_output_contains(collector, "not asserted"));

        collector.clear();
        interactive.process("S p O");
        CHECK(answers_contain(collector, "c p d"));
        CHECK_FALSE(any_output_contains(collector, "e p f")); });
}

TEST_CASE("negation: a refutation survives a save and a load")
{
    const auto file = std::filesystem::temp_directory_path() / "zelph_refuted_roundtrip.bin";

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, "¬(a p b)\nc p d\n");
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    collector.clear();

    // The probability alone could not carry this: the weight store is keyed by
    // a hash of the edge, so a loaded file cannot be asked which of its entries
    // were fact probabilities. The marking fact is what the index is rebuilt
    // from, exactly as for a rule pattern.
    interactive.process("S p O");
    CHECK(answers_contain(collector, "c p d"));
    CHECK_FALSE(any_output_contains(collector, "a p b"));

    std::filesystem::remove(file);
}

TEST_CASE("negation: negated rule conditions survive a save and a load")
{
    // A negated condition represents the fact `((P) ~ negation)`, meaning the
    // file carries it as rule text just like any other condition, and its
    // header says that the file names negated conditions in this way: the
    // load prints no note. A load disarms the stores that hold each fact's
    // exact parts, thus causing conditions to be read back from the edges
    // alone -- a negated ground path condition among them, whose path
    // pattern is the subject of the negation.
    const auto file = std::filesystem::temp_directory_path() / "zelph_negated_conditions.bin";
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"((a p b) => (c q d)
(X go Y, ¬(a p b)) => (X ok Y)
(X go Y, ¬(e p⁺ f)) => (X ok2 Y)
(X go Y, ¬(X p b)) => (X ok3 Y))");
        interactive.process(".save \"" + file.string() + "\"");
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load \"" + file.string() + "\"");
    CHECK_FALSE(any_output_contains(collector, "Note:"));
    std::filesystem::remove(file);

    process_lines(interactive, "t go k\nu p b\nu go v");
    interactive.run(true, false, false);

    collector.clear();
    interactive.process("C q D");
    CHECK(collect_answers(collector).empty());

    for (const std::string predicate : {"ok", "ok2"})
    {
        CAPTURE(predicate);
        collector.clear();
        interactive.process("S " + predicate + " O");
        auto answers = collect_answers(collector);
        std::sort(answers.begin(), answers.end());
        CHECK(answers == std::vector<std::string>{"t " + predicate + " k", "u " + predicate + " v"});
    }

    collector.clear();
    interactive.process("S ok3 O");
    CHECK(collect_answers(collector) == std::vector<std::string>{"t ok3 k"});
}

namespace zelph::test
{
    // Binary files located in older_networks/*.bin, compiled into this
    // executable via embed_files.cmake.
    const std::map<std::string, std::vector<unsigned char>>& older_networks();
}

TEST_CASE("negation: a network saved before negated conditions were facts says so on load")
{
    // The file `older_networks/negated_condition.bin` was saved by the 1.0.2
    // engine following the rule `(X go Y, ¬(X p b)) => (X ok Y)`. In that
    // engine, a condition was negated through a fact ABOUT its pattern, and
    // the pattern itself was named as the condition, so here the rule reads
    // `(X p b)` positively. Nothing is migrated: the load says so, and a save
    // of such a network keeps saying so on the next load, as its rules still
    // carry the older reading. A user reaches this state by loading such a
    // file.
    const auto& networks = zelph::test::older_networks();
    const auto  network  = networks.find("negated_condition");
    REQUIRE(network != networks.end());

    const auto path    = std::filesystem::temp_directory_path() / "zelph_older_negated_condition.bin";
    const auto resaved = std::filesystem::temp_directory_path() / "zelph_older_negated_condition_resaved.bin";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(network->second.data()), static_cast<std::streamsize>(network->second.size()));
    }

    const std::string note  = "Note: negated rule conditions in this network were saved by an older zelph and read as positive "
                              "here; rebuild the network from its scripts before reasoning over it.";
    const auto        noted = [&note](const zelph::io::OutputCollector& collector)
    {
        return std::ranges::any_of(collector.events(), [&note](const auto& event)
                                   { return event.channel == zelph::io::OutputChannel::Out && event.text == note; });
    };

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".load " + path.generic_string());
        CHECK(noted(collector));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X p b)"));
        CHECK_FALSE(any_output_contains(collector, "¬"));

        interactive.process(".save " + resaved.generic_string());
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + resaved.generic_string());
    CHECK(noted(collector));

    std::filesystem::remove(path);
    std::filesystem::remove(resaved);
}

TEST_CASE("stratified: ¬ over an inequality guard is refused, not silently dropped")
{
    // `!=` is a built-in constraint, not a fact to look up, and it is
    // dispatched by its predicate before the leaf branch, which previously
    // stood as the sole location that read the negation tag. So
    // `¬(X != Y)` used to reach the guard WITHOUT its tag and
    // be evaluated as `X != Y`: the rule below derived `alice same-as bob` --
    // exactly the pairs that are DIFFERENT -- and printed `¬(alice != bob)` in
    // the justification of it.
    //
    // It is refused rather than given a reading, because a negated inequality
    // is an equality constraint and writing the SAME VARIABLE twice already
    // says that, with the advantage that unification then uses it to narrow
    // the search instead of testing afterwards.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
alice member Q1
bob member Q1
(X member C, Y member C, ¬(X != Y)) => (X same-as Y)
)");
        CHECK(any_output_contains(collector, "¬ cannot be applied to \"!=\""));

        // And the refusal is one line, not one per candidate pair.
        size_t refusals = 0;
        for (const auto& event : collector.events())
            if (event.text.find("¬ cannot be applied") != std::string::npos) ++refusals;
        CHECK(refusals == 1);

        // The rule derives nothing at all -- asked of the graph rather than of
        // the output, where the echo of the rule itself mentions the predicate.
        collector.clear();
        interactive.process("P same-as Q");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("stratified: ranging over a domain is a positive condition")
{
    // What the second reading of `¬` used to do implicitly is written down
    // instead: the positive condition says WHICH candidates are considered,
    // the negation only filters them, and the justification of each result
    // names both.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x flagged good
y flagged bad
z flagged good
(X flagged S, ¬(X flagged bad)) => (X unflagged ok)
)");
        collector.clear();
        interactive.process("Q unflagged ok");
        CHECK(answers_contain(collector, "x unflagged ok"));
        CHECK(answers_contain(collector, "z unflagged ok"));
        CHECK_FALSE(any_output_contains(collector, "y unflagged")); });
}

TEST_CASE("stratified: doc example -- last element of a chain via negation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
elem1 --> elem2
elem2 --> elem3
elem3 --> elem4
elem4 --> elem5
elem1 partoflist mylist
elem2 partoflist mylist
elem3 partoflist mylist
elem4 partoflist mylist
elem5 partoflist mylist
(A partoflist L, ¬(A --> X)) => (A "is last of" L)
)");
        CHECK(any_output_contains(collector, "elem5 \"is last of\" mylist"));
        CHECK_FALSE(any_output_contains(collector, "elem1 \"is last of\""));
        CHECK_FALSE(any_output_contains(collector, "elem4 \"is last of\"")); });
}

TEST_CASE("stratified: classic (naive) evaluation defers negation too")
{
    // run_both_modes exercises the semi-naive scheduler (in check mode);
    // the classic two-phase loop in Reasoning::run is a separate code path.
    static const std::string script = R"(
(A trigger A) => (A step1 A)
(A step1 A) => (A step2 A)
(A trigger A, ¬(A step2 A)) => (A racewin A)
x trigger x
)";

    SUBCASE("parallel")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        collector.clear();
        process_lines(interactive, script);
        CHECK(any_output_contains(collector, "x step2 x"));
        CHECK_FALSE(any_output_contains(collector, "x racewin x"));
    }
    SUBCASE("single-core")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".parallel");
        interactive.process(".semi-naive off");
        collector.clear();
        process_lines(interactive, script);
        CHECK(any_output_contains(collector, "x step2 x"));
        CHECK_FALSE(any_output_contains(collector, "x racewin x"));
    }
}

TEST_CASE("stratified: the deferred stratum re-runs until the alternation reaches its fixpoint")
{
    // Two deferred rounds are required: the first deferred pass derives
    // (w q w), the positive rule turns it into (w r w), and only then can
    // the second deferred rule fire at the NEXT stratum boundary.
    // Distilled from the symbolic-math regression where simplifying a
    // compiled EML tree needed the identity fallback on two nesting
    // levels of one term: the semi-naive scheduler used to run the
    // deferred stratum exactly once, so the final fact was only derived
    // by the check-mode safety pass -- a completeness violation.
    static const std::string script = R"(
(A start A) => (A p A)
(A p A, ¬(A blockp A)) => (A q A)
(A q A) => (A r A)
(A r A, ¬(A blockr A)) => (A s A)
w start w
)";

    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, script);
        CHECK(any_output_contains(collector, "w q w"));
        CHECK(any_output_contains(collector, "w r w"));
        CHECK(any_output_contains(collector, "w s w")); });

    SUBCASE("classic (naive) evaluation alternates too")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        collector.clear();
        process_lines(interactive, script);
        CHECK(any_output_contains(collector, "w s w"));
    }
}

TEST_CASE("stratified: a negation waits for the negated rules its pattern depends on")
{
    // The program is stratifiable, and its perfect model contains (w blockr w)
    // while lacking (w s w): blockr is derived via inference from q, where q
    // is itself inferred under a negation, meaning that the rule negating
    // blockr belongs to a HIGHER stratum than the rule deriving q, and thus
    // cannot be evaluated before the completion of that derivation.
    //
    // Up to version 1.0.1, all rules featuring a negated condition sat in a
    // single deferred stratum and underwent a single evaluation pass. The rule
    // that negated blockr saw the graph prior to the existence of q -- and
    // thus before blockr -- and produced the derived (w s w). Since both
    // evaluation methods yielded identical outcomes, `.semi-naive check`
    // compared two equally wrong results and remained silent.
    static const std::string rules = R"(
(:start A, ¬(:blockp A)) => (:q A)
(:q A) => (:blockr A)
(:start A, ¬(:blockr A)) => (:s A)
)";
    // The identical rules, presented in reverse sequence: the stratum
    // constitutes a characteristic of that upon which the rules rely, not of
    // the location where they are situated.
    static const std::string reversed = R"(
(:start A, ¬(:blockr A)) => (:s A)
(:q A) => (:blockr A)
(:start A, ¬(:blockp A)) => (:q A)
)";

    const auto one_model = [](const std::string& program)
    {
        run_both_modes([&](auto& collector, auto& interactive)
                       {
            process_lines(interactive, program);
            interactive.process(":start w");
            CHECK(any_output_contains(collector, "w q w"));
            CHECK(any_output_contains(collector, "w blockr w"));
            CHECK_FALSE(any_output_contains(collector, "w s w")); });

        SUBCASE("classic (naive) evaluation")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".semi-naive off");
            collector.clear();
            process_lines(interactive, program);
            interactive.process(":start w");
            CHECK(any_output_contains(collector, "w blockr w"));
            CHECK_FALSE(any_output_contains(collector, "w s w"));
        }
    };

    SUBCASE("rules in the order they depend on each other") { one_model(rules); }
    SUBCASE("the same rules reversed") { one_model(reversed); }
}

TEST_CASE("stratified: a negation waits for a rule that another rule derives")
{
    // A rule which derives another rule may extend the scope of what a negation
    // tests: in this case, the derived (X p Y) => (X q Y) creates the specific
    // (a q b) that the first rule negates. The program is stratified, and its
    // perfect model holds (a q b) while excluding (a r b). The level analysis
    // already orders the negation following the rule that derives the rule,
    // since the consequence of the rule it writes indicates that it creates q
    // facts.
    //
    // The classic loop collected the rule set just once, saturated the positive
    // rules, executed each negation level, and only afterwards inspected rules
    // that had emerged during the process. Thus, the negation was tested before
    // the derived rule's execution, resulting in (a r b) being derived. The
    // semi-naive loop collects a new rule before traversing a level boundary
    // and was right all along. That is why run_both_modes by itself fails to
    // detect it: check mode runs the semi-naive loop along with classic passes
    // over ITS fixpoint, where (a q b) already holds, and never runs the
    // classic schedule from the start.
    //
    // All components must be available in a single run: when auto-run is
    // enabled, the rule is derived during a prior run and collected at the
    // start of the subsequent one. A user achieves this state by disabling
    // auto-run and then invoking .run, or by loading a script with .load, or by
    // importing a module in one step.
    static const std::string derived_first = R"(
(X p Y, ¬(X q Y)) => (X r Y)
(K is on) => ((X p Y) => (X q Y))
a p b
k is on
)";
    // The rule emerges solely within the positive saturation occurring after
    // negation level 1, once (k sw k) has been derived. A loop designed to seek
    // out new rules exclusively before the first level would still execute
    // level 2 in its absence.
    static const std::string derived_after_a_level = R"(
(A start A, ¬(A stop A)) => (A sw A)
(A sw A) => ((X p Y) => (X q Y))
(X p Y, ¬(X q Y)) => (X r Y)
k start k
a p b
)";
    // The rule that is derived negates in turn, thus requiring a distinct
    // level of its own, positioned beneath the rule that negates what it
    // derives: recalculating the levels is necessary, not merely adjusting
    // the rule list.
    static const std::string derived_negating = R"(
(X p Y, ¬(X q Y)) => (X r Y)
(K is on) => ((X p Y, ¬(X s Y)) => (X q Y))
a p b
k is on
)";

    const auto one_model = [](const std::string& program)
    {
        const auto evaluate = [&](auto& collector, auto& interactive)
        {
            interactive.process(".auto-run");
            process_lines(interactive, program);
            interactive.process(".run");
            interactive.process("X q Y");
            interactive.process("X r Y");
            CHECK(answers_contain(collector, "a q b"));
            CHECK_FALSE(any_output_contains(collector, "a r b"));
        };

        run_both_modes(evaluate);

        SUBCASE("classic (naive) evaluation")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".semi-naive off");
            collector.clear();
            evaluate(collector, interactive);
        }
        SUBCASE("classic (naive) evaluation on a single core")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".parallel");
            interactive.process(".semi-naive off");
            collector.clear();
            evaluate(collector, interactive);
        }
    };

    SUBCASE("the rule is derived in the first positive saturation") { one_model(derived_first); }
    SUBCASE("the rule is derived after a negation level has run") { one_model(derived_after_a_level); }
    SUBCASE("the derived rule negates as well") { one_model(derived_negating); }
}

TEST_CASE("stratified: a rule ranging over predicates leaves the negation levels as they are")
{
    // The program of "a negation waits for the negated rules its pattern
    // depends on" along with the transitivity meta-rule introduced
    // initially in logic.md. R functions as a variable within a predicate
    // position, meaning the analysis is unable to name the predicates
    // generated by the meta-rule. Previously, it counted these as all
    // existing predicates, causing the meta-rule to enter a cycle with
    // each negating rule, merging all into a single level, and
    // re-introducing the single-pass race that levels were designed to
    // prevent: (w s w) was derived once more, and only check mode detected
    // it.
    //
    // The meta-rule creates (A R C) solely for an R it has read in (A R B),
    // meaning it only extends predicates that already possess established
    // facts. Any rule awaiting those facts already waits for the sources that
    // produced them, and since the meta-rule executes during every positive
    // saturation, it completes before the initiation of any level. The only
    // aspect it can influence is the selection of predicates to extend: this
    // hinges on (R is transitive), hence any rule that derives `is` facts comes
    // before every rule that reads anything (pinned in ".strata orders a
    // negation after a rule that declares a relation transitive").
    static const std::string meta     = "(R is transitive, A R B, B R C) => (A R C)\n";
    static const std::string rules    = meta + R"(
(:start A, ¬(:blockp A)) => (:q A)
(:q A) => (:blockr A)
(:start A, ¬(:blockr A)) => (:s A)
)";
    static const std::string reversed = R"(
(:start A, ¬(:blockr A)) => (:s A)
(:q A) => (:blockr A)
(:start A, ¬(:blockp A)) => (:q A)
)" + meta;

    const auto one_model = [](const std::string& program)
    {
        const auto evaluate = [&](auto& collector, auto& interactive)
        {
            process_lines(interactive, program);
            interactive.process(":start w");
            CHECK(any_output_contains(collector, "w blockr w"));
            CHECK_FALSE(any_output_contains(collector, "w s w"));

            // The listing says the same before any fact is present: two
            // rules that negate on two distinct levels, and a meta-rule that
            // negates nothing and closes no cycle, nowhere.
            collector.clear();
            interactive.process(".strata");
            CHECK(any_output_contains(collector, "2 negation levels"));
            CHECK_FALSE(any_output_contains(collector, "Not stratifiable"));
            CHECK_FALSE(any_output_contains(collector, "R is transitive"));
        };

        run_both_modes(evaluate);

        SUBCASE("classic (naive) evaluation")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".semi-naive off");
            collector.clear();
            evaluate(collector, interactive);
        }
    };

    SUBCASE("the meta-rule first") { one_model(rules); }
    SUBCASE("the rules reversed and the meta-rule last") { one_model(reversed); }
}

namespace
{
    // The position of the initial occurrence at or following `from` where
    // the text includes `sub` exactly as written, across any channel. What
    // .strata claims is carried by the ORDER of its lines -- specifically,
    // which level header a rule appears under -- while the evaluation's
    // level announcements serve as diagnostic indicators, not Out lines.
    std::size_t line_index(const zelph::io::OutputCollector& collector, const std::string& sub, const std::size_t from = 0)
    {
        const auto& events = collector.events();
        for (std::size_t i = from; i < events.size(); ++i)
            if (events[i].text.find(sub) != std::string::npos) return i;
        return std::string::npos;
    }
}

TEST_CASE("stratified: a predicate a consequence creates only inside a term counts too")
{
    // The second rule creates (w times w) exclusively as the object within
    // (w rw (w times w)), and it is this embedded fact that the third rule
    // negates. The analysis collects the predicates of a consequence at every
    // depth, hence the third rule awaits the first; limiting collection to
    // just the top predicate, rw, would put both negating rules at level 1 and
    // derive (w s w). The blockr programs above are unable to distinguish
    // between the two: every predicate they negate is created at the top.
    static const std::string rules    = R"(
(:start A, ¬(:blockp A)) => (:q A)
(:q A) => (A rw (A times A))
(:start A, ¬(A times A)) => (:s A)
)";
    static const std::string reversed = R"(
(:start A, ¬(A times A)) => (:s A)
(:q A) => (A rw (A times A))
(:start A, ¬(:blockp A)) => (:q A)
)";

    const auto one_model = [](const std::string& program)
    {
        const auto evaluate = [&](auto& collector, auto& interactive)
        {
            process_lines(interactive, program);
            collector.clear();
            interactive.process(".strata");
            CHECK(any_output_contains(collector, "2 negation levels"));
            const std::size_t level2 = line_index(collector, "Level 2:");
            CHECK(level2 != std::string::npos);
            CHECK(level2 < line_index(collector, "¬(A times A)"));

            interactive.process(":start w");
            CHECK(any_output_contains(collector, "w rw (w times w)"));
            CHECK_FALSE(any_output_contains(collector, "w s w"));
        };

        run_both_modes(evaluate);

        SUBCASE("classic (naive) evaluation")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".semi-naive off");
            collector.clear();
            evaluate(collector, interactive);
        }
    };

    SUBCASE("rules in the order they depend on each other") { one_model(rules); }
    SUBCASE("the same rules reversed") { one_model(reversed); }
}

TEST_CASE("stratified: a negation waits for the rule that writes a rule creating what it negates")
{
    // The rule that the first one writes comes into existence only when it
    // fires, yet its shape is already determined: its consequence (X qq A)
    // says it creates qq facts. Thus, the first rule counts as responsible
    // for creating qq, while the second rule, which negates qq, is
    // positioned at the level above it. The rule that was written produces
    // precisely the (x qq y) that the second rule negates, hence the
    // program's perfect model holds (x qq y) and does not hold (x ss y).
    //
    // In a single run, with auto-run off followed by .run, the classic
    // evaluation used to test the negation before collecting the rule that
    // the first level had written (refer to "a negation waits for a rule
    // that another rule derives").
    static const std::string program = R"(
(:start A, ¬(:block A)) => ((X pp A) => (X qq A))
(X pp Y, ¬(X qq Y)) => (X ss Y)
)";

    const auto evaluate = [](auto& collector, auto& interactive)
    {
        process_lines(interactive, program);
        collector.clear();
        interactive.process(".strata");
        const std::size_t level2 = line_index(collector, "Level 2:");
        CHECK(level2 != std::string::npos);
        CHECK(level2 < line_index(collector, "¬(X qq Y)"));

        interactive.process(".auto-run");
        interactive.process("x pp y");
        interactive.process(":start y");
        interactive.process(".run");
        interactive.process("X qq Y");
        interactive.process("X ss Y");
        CHECK(answers_contain(collector, "x qq y"));
        CHECK_FALSE(any_output_contains(collector, "x ss y"));
    };

    run_both_modes(evaluate);

    SUBCASE("classic (naive) evaluation")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        collector.clear();
        evaluate(collector, interactive);
    }
}

TEST_CASE("stratified: a rule that derives a rule counts as creating what that rule creates")
{
    // A rule whose consequence is another rule used to count as creating
    // every predicate that exists. Any negating rule it depends on then
    // formed a cycle with it, and .strata reported "Not stratifiable" for
    // programs that are stratified: in this case, blockp is not derived, and
    // the rule written for (a kind gen) creates qq facts and nothing beyond.
    // The analysis now reads the predicates from the consequence of the rule
    // to be written; the rule that is written is collected, and the levels
    // recalculated once it is in place.
    static const std::string program = R"(
(A start B, ¬(A blockp B)) => (A kind gen)
(R kind gen) => ((X R Y) => (X qq Y))
(X pp Y, ¬(X qq Y)) => (X ss Y)
)";

    const auto evaluate = [](auto& collector, auto& interactive)
    {
        process_lines(interactive, program);
        collector.clear();
        interactive.process(".strata");
        CHECK(any_output_contains(collector, "2 negation levels"));
        CHECK_FALSE(any_output_contains(collector, "Not stratifiable"));
        const std::size_t level2 = line_index(collector, "Level 2:");
        CHECK(level2 != std::string::npos);
        CHECK(line_index(collector, "¬(A blockp B)") < level2);
        CHECK(level2 < line_index(collector, "¬(X qq Y)"));

        // During a single run: the written rule emerges solely after the first
        // level has derived (a kind gen), and the negation of qq must await
        // it.
        interactive.process(".auto-run");
        interactive.process("a start b");
        interactive.process("x a y");
        interactive.process("x pp y");
        interactive.process(".run");
        interactive.process("X qq Y");
        interactive.process("X ss Y");
        CHECK(answers_contain(collector, "x qq y"));
        CHECK_FALSE(any_output_contains(collector, "x ss y"));
    };

    run_both_modes(evaluate);

    SUBCASE("classic (naive) evaluation")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        collector.clear();
        evaluate(collector, interactive);
    }
}

TEST_CASE("stratified: a rule that negates and derives a rule is a cycle only if the rule closes one")
{
    // Both rules negate qq and write a rule. The first writes one that
    // creates hit facts, which nothing negates: no cycle. The second writes
    // one that creates qq facts, which the second rule itself negates: a
    // real cycle, reported as one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the written rule creates something else")
        {
            interactive.process("(A pp B, ¬(A qq B)) => ((C zz A) => (C hit A))");
            collector.clear();
            interactive.process(".strata");
            CHECK(any_output_contains(collector, "1 negation level"));
            CHECK_FALSE(any_output_contains(collector, "Not stratifiable"));
        }
        SUBCASE("the written rule creates what the rule negates")
        {
            interactive.process("(A pp B, ¬(A qq B)) => ((C zz A) => (C qq A))");
            collector.clear();
            interactive.process(".strata");
            const std::size_t header = line_index(collector, "Not stratifiable");
            REQUIRE(header != std::string::npos);
            CHECK(line_index(collector, "¬(A qq B)", header + 1) != std::string::npos);
        } });
}

TEST_CASE("stratified: a written rule with a variable predicate still counts as creating every predicate")
{
    // The rule written for (P kind gen) creates P facts, with P being
    // whatever the declaration names -- blockp included, provided that
    // (blockp start b) holds, which the first rule turns into
    // (blockp kind gen). No constraint in the rules limits P, hence the rule
    // responsible for writing it counts as generating all predicates, and the
    // cycle involving the negation of blockp is reported.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A start B, ¬(A blockp B)) => (A kind gen)");
        interactive.process("(P kind gen) => ((X pp Y) => (X P Y))");
        collector.clear();
        interactive.process(".strata");
        const std::size_t header = line_index(collector, "Not stratifiable");
        REQUIRE(header != std::string::npos);
        CHECK(line_index(collector, "¬(A blockp B)", header + 1) != std::string::npos);
        CHECK(line_index(collector, "(P kind gen) =>", header + 1) != std::string::npos); });
}

TEST_CASE("stratified: .strata shows each negating rule on the level the evaluation runs it")
{
    // The program from the previous case. blockr stems from q, and q is under
    // negation, thus the rule that negates blockr is placed one level above
    // the rule that derives q; the positive rule situated between them
    // carries no level at all. Without .strata, that order would be deducible
    // solely from a result.
    //
    // The numbers must be those the evaluation runs by, not an alternative
    // judgment. `.log -1` makes a run declare each deferred pass as "negation
    // level N of M". When (:blockp v) is asserted, the rule that .strata puts
    // on level 2 is the sole one that derives anything, and its (:s v) must
    // follow the announcement of level 2 -- in both strategies.
    static const std::string program = R"(
(:start A, ¬(:blockp A)) => (:q A)
(:q A) => (:blockr A)
(:start A, ¬(:blockr A)) => (:s A)
)";

    const auto levels_as_evaluated = [](auto& collector, auto& interactive)
    {
        interactive.process(".auto-run");
        interactive.process(".deductions all");
        process_lines(interactive, program);

        collector.clear();
        interactive.process(".strata");
        CHECK(any_output_contains(collector, "2 negation levels"));
        const std::size_t level1 = line_index(collector, "Level 1:");
        const std::size_t first  = line_index(collector, "¬(:blockp A)");
        const std::size_t level2 = line_index(collector, "Level 2:");
        const std::size_t third  = line_index(collector, "¬(:blockr A)");
        REQUIRE(third != std::string::npos);
        CHECK(level1 < first);
        CHECK(first < level2);
        CHECK(level2 < third);
        CHECK(line_index(collector, "(:q A) => (:blockr A)") == std::string::npos);
        CHECK(line_index(collector, "Not stratifiable") == std::string::npos);

        // A rule that has been printed comes back as the identical rule:
        // re-entering it should not add a third negating rule into the
        // listing.
        const std::string printed = collector.events()[third].text;
        interactive.process(printed);
        collector.clear();
        interactive.process(".strata");
        CHECK(count_outputs_containing(collector, "=>") == 2);

        interactive.process(":blockp v");
        interactive.process(":start v");
        interactive.process(".log -1");
        collector.clear();
        interactive.process(".run");
        const std::size_t announced1 = line_index(collector, "negation level 1 of 2");
        const std::size_t announced2 = line_index(collector, "negation level 2 of 2");
        const std::size_t derived    = line_index(collector, "(:s v) ⇐");
        REQUIRE(derived != std::string::npos);
        CHECK(announced1 < announced2);
        CHECK(announced2 < derived);
    };

    run_both_modes(levels_as_evaluated);

    SUBCASE("classic (naive) evaluation")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        collector.clear();
        levels_as_evaluated(collector, interactive);
    }
}

TEST_CASE("stratified: a negation directly over another negating rule's consequence waits for it, in either order")
{
    // The scenarios described earlier insert a positive rule between two
    // negating rules; in this instance, the second one negates what the first
    // derives, with no intermediary. Up to version 1.0.1, the negating rules
    // executed sequentially in a single pass, each accessing what the
    // preceding ones had derived in it. Consequently, the program delivered
    // the correct outcome when the rule that derives q was positioned first
    // and yielded an incorrect result when placed second: (w s w) was derived
    // before (w q w) was available, and check mode compared two identically
    // flawed outcomes. The blockr program failed in both orders, due to a
    // distinct issue: the absence of saturation between the negating rules.
    // In this case, the negative edge runs from a rule that itself negates,
    // in its most straightforward form.
    //
    // The outcome alone catches a merged level solely in the order in which
    // the engine happens to apply the reading rule first, and this order
    // constitutes an implementation detail. ".strata" pins the two levels
    // in both instances.
    static const std::string rules    = R"(
(:start A, ¬(:blockp A)) => (:q A)
(:start A, ¬(:q A)) => (:s A)
)";
    static const std::string reversed = R"(
(:start A, ¬(:q A)) => (:s A)
(:start A, ¬(:blockp A)) => (:q A)
)";

    const auto one_model = [](const std::string& program)
    {
        const auto check_model = [&](auto& collector, auto& interactive)
        {
            process_lines(interactive, program);
            interactive.process(":start w");
            CHECK(any_output_contains(collector, "w q w"));
            CHECK_FALSE(any_output_contains(collector, "w s w"));

            collector.clear();
            interactive.process(".strata");
            CHECK(any_output_contains(collector, "2 negation levels"));
        };

        run_both_modes(check_model);

        SUBCASE("classic (naive) evaluation")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".semi-naive off");
            collector.clear();
            check_model(collector, interactive);
        }
    };

    SUBCASE("the rule that derives q first") { one_model(rules); }
    SUBCASE("the rule that negates q first") { one_model(reversed); }
}

TEST_CASE("stratified: .strata says so when there is nothing to stratify")
{
    // The absence of any output could imply either that the command found
    // nothing or that it did not perform a search; an empty network and a
    // rule set lacking a negation are the only two scenarios in which there
    // is nothing to show.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".strata");
        CHECK(any_output_contains(collector, "No rules found."));

        interactive.process("(A p B) => (B q A)");
        collector.clear();
        interactive.process(".strata");
        CHECK(any_output_contains(collector, "No rule has a negated condition"));
        CHECK_FALSE(any_output_contains(collector, "Level 1:"));

        // Listed and documented like every other command.
        collector.clear();
        interactive.process(".help");
        CHECK(any_output_contains(collector, ".strata"));
        collector.clear();
        interactive.process(".help .strata");
        CHECK_FALSE(any_event_contains(collector, "Unknown command")); });
}

TEST_CASE("stratified: the NAND bootstrap completes before the simplifier's fallback in one evaluation")
{
    // The digit tables in binary-nand-arithmetic are generated via a negated
    // gate rule; symbolic-core's identity fallback negates (T rw S). When
    // loaded and asked within a single evaluation, the fallback previously
    // executed during the same deferred pass as the gate rule -- before any
    // digit table was established -- and (&2 + &3) answered itself while
    // also yielding &5. By loading the substrate first and posing the query in
    // a later evaluation, this behaviour was concealed, which is why every
    // other test, all of which load the substrate on an individual line,
    // failed to detect it.
    //
    // Two ways to achieve a single evaluation, each involving standard usage:
    // a module that imports all components and makes a request, captured
    // through one .import (a module executes once, upon completion), and
    // auto-run switched off with a single .run.
    namespace fs          = std::filesystem;
    const fs::path module = fs::temp_directory_path() / "zelph_test_nand_one_evaluation.zph";
    {
        std::ofstream f(module);
        f << ".import binary-nand-arithmetic\n"
          << ".import symbolic-core\n"
          << ":simplify (&2 + &3)\n";
    }

    const auto exactly_five = [](auto& collector, auto& interactive)
    {
        collector.clear();
        interactive.process("(:simplify (&2 + &3)) = X");
        CHECK(collect_answers(collector).size() == 1);
        CHECK(answers_contain(collector, "(:simplify (&2 + &3)) = &5"));
    };

    // A rule ranging over predicates within the same evaluation. Such a rule
    // formerly counted as creating each predicate that exists, thereby
    // combining the gate rule and the fallback into a single level once more:
    // the fallback answered (&2 + &3) with itself, and rule SU reported the
    // two answers as a contradiction. Nothing here is declared transitive; the
    // rule merely needs to be present.
    const auto with_meta_rule = [&](auto& collector, auto& interactive)
    {
        interactive.process(".auto-run");
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        interactive.process(".import binary-nand-arithmetic");
        interactive.process(".import symbolic-core");
        interactive.process(":simplify (&2 + &3)");
        interactive.process(".run");
        CHECK_FALSE(has_contradiction(collector));
        exactly_five(collector, interactive);
    };

    run_both_modes([&](auto& collector, auto& interactive)
                   {
        SUBCASE("one .import of a module that loads both and asks")
        {
            interactive.process(".import \"" + module.string() + "\"");
            exactly_five(collector, interactive);
        }
        SUBCASE("auto-run off, one .run")
        {
            interactive.process(".auto-run");
            interactive.process(".import binary-nand-arithmetic");
            interactive.process(".import symbolic-core");
            interactive.process(":simplify (&2 + &3)");
            interactive.process(".run");
            exactly_five(collector, interactive);
        }
        SUBCASE("one .run with a rule ranging over predicates") { with_meta_rule(collector, interactive); } });

    SUBCASE("classic (naive) evaluation, auto-run off, one .run")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        interactive.process(".auto-run");
        interactive.process(".import binary-nand-arithmetic");
        interactive.process(".import symbolic-core");
        interactive.process(":simplify (&2 + &3)");
        interactive.process(".run");
        exactly_five(collector, interactive);
    }

    SUBCASE("classic (naive) evaluation with a rule ranging over predicates")
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive off");
        collector.clear();
        with_meta_rule(collector, interactive);
    }

    std::error_code ec;
    fs::remove(module, ec);
}

TEST_CASE("stratified: check mode reports a derivation whose negated premise came to hold")
{
    // A program that is NOT stratifiable: r is derived through ¬q, and q is
    // derived via r. It lacks any supported model (nor any stable one): its
    // completion makes r equivalent to ¬q and q equivalent to r, hence no
    // evaluation order can reach a state that the rules justify. Interpreted
    // classically, {p, q} and {p, q, r} both satisfy it; what fails is
    // support, not satisfaction.
    // The engine processes it as it does every negation (after the positive
    // facts are fully saturated), derives (w r w), then (w q w), and
    // ultimately arrives at a fact whose sole justification has now failed.
    //
    // That is the category of flaw underlying the NAND and blockr examples
    // mentioned earlier, and it remains undetected by the comparison check mode
    // previously employed: both classic and semi-naive evaluation generate
    // identical pairs of facts. What exposes it is the inquiry into whether
    // each fact resulting from a negation remains valid once the run concludes.
    // This state is reached through a program a user may author, not by disrupting
    // the scheduler, thus ensuring the report is validated against a state the
    // engine must be capable of handling.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(:p A, ¬(:q A)) => (:r A)");
        interactive.process("(:r A) => (:q A)");
        collector.clear();
        CHECK_THROWS_WITH_AS(interactive.process(":p w"), doctest::Contains("negated premise"), std::runtime_error);
        CHECK(any_output_contains(collector, "w r w")); });
}

TEST_CASE("stratified: check mode keeps a fact that something else still derives")
{
    // A negated premise that has come to hold is not yet considered a lost
    // fact: check mode only reports a fact when no further derivations of it
    // remain, and the message explicitly states this
    // ("nothing else derives them"). Two possibilities exist for an
    // alternative derivation: a different rule, or the same rule with
    // different bindings. In the latter case, the fact is reported precisely
    // when its final derivation ceases to exist, demonstrating that a
    // previously preserved record is retested at a later stage.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("another rule")
        {
            interactive.process("(:p A, ¬(:q A)) => (:r A)");
            interactive.process("(:k A) => (:r A)");
            interactive.process(":p w");
            interactive.process(":k w");
            collector.clear();
            CHECK_NOTHROW(interactive.process(":q w"));
            CHECK_FALSE(any_output_contains(collector, "Negation check"));
            interactive.process(":r X");
            CHECK(answers_contain(collector, ":r w"));
        }
        SUBCASE("the same rule under other bindings")
        {
            interactive.process("(X p Y, ¬(:q Y)) => (:r X)");
            interactive.process("x p a");
            interactive.process("x p b");
            collector.clear();
            CHECK_NOTHROW(interactive.process(":q a"));
            CHECK_FALSE(any_output_contains(collector, "Negation check"));
            CHECK_THROWS_WITH_AS(interactive.process(":q b"), doctest::Contains("negated premise"), std::runtime_error);
        } });
}

TEST_CASE("stratified: check mode re-tests a negation after a run that did not check")
{
    // Check mode re-reads a record solely when a predicate its rule negates
    // has acquired new facts since the last check. These predicates were
    // gathered exclusively through check-mode semi-naive runs, meaning a
    // .run-once, or a run conducted while check mode was off, carried forward
    // the new facts without noting them, and the next check run skipped a
    // record whose negated premise had come to hold. Following such a run,
    // the next check run now re-tests every record.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        interactive.process("(:p A, ¬(:q A)) => (:r A)");

        SUBCASE("a .run-once in between")
        {
            interactive.process(":p w");
            interactive.process(".auto-run");
            interactive.process(":q w");
            interactive.process(".run-once");
            CHECK_THROWS_WITH_AS(interactive.process(".run"), doctest::Contains("negated premise"), std::runtime_error);
        }
        SUBCASE("the blocking fact derived by a .run-once")
        {
            // It is derived, and thus never arrives at the record of facts
            // transmitted across runs, meaning that merely noting what that
            // record holds would not suffice.
            interactive.process("(:t A) => (:q A)");
            interactive.process(":p w");
            interactive.process(".auto-run");
            interactive.process(":t w");
            interactive.process(".run-once");
            CHECK_THROWS_WITH_AS(interactive.process(".run"), doctest::Contains("negated premise"), std::runtime_error);
        }
        SUBCASE("a run with check mode off in between")
        {
            interactive.process(":p w");
            interactive.process(".semi-naive on");
            interactive.process(":q w");
            interactive.process(".semi-naive check");
            CHECK_THROWS_WITH_AS(interactive.process(".run"), doctest::Contains("negated premise"), std::runtime_error);
        } });
}

TEST_CASE("stratified: check mode re-tests a negation next to a path condition")
{
    // A path condition does not constitute a fact lookup, and a negation next
    // to one previously was not subject to re-testing at all. This exemption
    // is only needed in cases where the test is unable to distinguish a lost
    // fact from one that remains derived: explain() walks a path only when the
    // other conditions or the consequence bind both endpoints. In this
    // instance, they do, hence (x s y) is reported once (x q y) is true, just
    // as it would be without the ⁺.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        interactive.process("(A p B, A r⁺ B, ¬(A q B)) => (A s B)");
        interactive.process("x p y");
        interactive.process("x r y");
        CHECK_THROWS_WITH_AS(interactive.process("x q y"), doctest::Contains("negated premise"), std::runtime_error); });
}

TEST_CASE("stratified: check mode does not report a fact it cannot judge")
{
    // The remaining half of the prior case. In this scenario, the path BINDS
    // B, which nothing else does, thus explain() cannot rebuild a derivation
    // via it: (a s c) continues to be derived through b2 even after (b1 q c),
    // yet a re-test would find no derivation and report it as lost. The rule
    // stays exempt, and the documentation says which rules are.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A p C, A r⁺ B, ¬(B q C)) => (A s C)");
        interactive.process("a p c");
        interactive.process("a r b1");
        interactive.process("a r b2");
        collector.clear();
        CHECK_NOTHROW(interactive.process("b1 q c"));
        CHECK_FALSE(any_output_contains(collector, "Negation check")); });
}

TEST_CASE("stratified: .strata names the rules that negate what they derive")
{
    // The prior case's program, which check mode can only report after a run
    // has built a fact based on a premise that subsequently failed.
    // Dependency analysis sees the cause in the rules alone: the two
    // constitute a single strongly connected component containing a negative
    // edge, and .strata reports that component even before any fact is
    // present. It names the positive rule as well, given it forms half the
    // cycle, while omitting a negating rule at the same level that does not
    // belong to it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(:p A, ¬(:q A)) => (:r A)");
        interactive.process("(:r A) => (:q A)");
        interactive.process("(:p A, ¬(:t A)) => (:u A)");
        collector.clear();
        interactive.process(".strata");
        const std::size_t header = line_index(collector, "Not stratifiable");
        REQUIRE(header != std::string::npos);
        CHECK(line_index(collector, "¬(:t A)") < header);
        CHECK(line_index(collector, "¬(:q A)", header + 1) != std::string::npos);
        CHECK(line_index(collector, "(:r A) => (:q A)", header + 1) != std::string::npos);
        CHECK(line_index(collector, "¬(:t A)", header + 1) == std::string::npos); });
}

TEST_CASE("stratified: a negated condition depends on the predicates of its pattern, not on the ~ of its own fact")
{
    // A negated condition is the fact `((P) ~ negation)`, and what it
    // depends on is read from P. Read from the fact itself, `~` would be
    // treated as a negated predicate, thus the rule that derives
    // `X ~ okish` would feed both negations: .strata then reported two
    // levels and a cycle between the first two rules, neither of which
    // negates anything the other one derives.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X go Y, ¬(X p b)) => (X ok Y)
(X ok Y) => (X ~ okish)
(X ~ okish, ¬(X q c)) => (X fine yes)
)");
        collector.clear();
        interactive.process(".strata");
        CHECK(any_output_contains(collector, "1 negation level"));
        CHECK(line_index(collector, "Not stratifiable") == std::string::npos); });
}

TEST_CASE("stratified: .strata keeps a cycle that a rule over predicates really closes")
{
    // A rule that solely extends the predicates it reads maintains the levels
    // unchanged (refer to "a rule ranging over predicates leaves the negation
    // levels as they are"). This one reads every predicate and creates a fixed
    // one: the (w s w) derived by the second rule constitutes a fact that the
    // first rule reads, while the (w linked w) it creates is what the second
    // rule negates. That constitutes an actual cycle involving a negation, and
    // it must remain reported.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X R Y) => (X linked Y)");
        interactive.process("(:start A, ¬(:linked A)) => (:s A)");
        collector.clear();
        interactive.process(".strata");
        const std::size_t header = line_index(collector, "Not stratifiable");
        REQUIRE(header != std::string::npos);
        CHECK(line_index(collector, "¬(:linked A)", header + 1) != std::string::npos);
        CHECK(line_index(collector, "(X R Y) => (X linked Y)", header + 1) != std::string::npos); });
}

TEST_CASE("stratified: a rule whose predicate comes from an argument joins the negations into one level")
{
    // An inverse or sub-property rule creates a fact whose predicate is drawn
    // from the SUBJECT or OBJECT of a different fact, rather than being taken
    // from the predicate of a fact it has examined. No element within the rule
    // says which predicates these are, hence the analysis continues to regard
    // it as creating every predicate, and every negating rule it can reach ends
    // up on the same level as it. logic.md documents this situation and
    // identifies the solution, which this case also pins: when formulated as a
    // rule that derives one rule per declaration, the same axiom maintains the
    // level structure, since each derived rule explicitly names its predicates.
    static const std::string program = R"(
(:start A, ¬(:blockp A)) => (:q A)
(:q A) => (:blockr A)
(:start A, ¬(:blockr A)) => (:s A)
)";

    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("as a rule over predicates, .strata says so")
        {
            interactive.process("(R inverseof S, A R B) => (B S A)");
            process_lines(interactive, program);
            collector.clear();
            interactive.process(".strata");
            CHECK(any_output_contains(collector, "1 negation level:"));
            const std::size_t header = line_index(collector, "Not stratifiable");
            REQUIRE(header != std::string::npos);
            CHECK(line_index(collector, "inverseof", header + 1) != std::string::npos);
            CHECK(line_index(collector, "¬(:blockp A)", header + 1) != std::string::npos);
            CHECK(line_index(collector, "¬(:blockr A)", header + 1) != std::string::npos);
        }
        SUBCASE("as a rule that derives rules, the levels stay")
        {
            interactive.process("(R inverseof S) => ((A R B) => (B S A))");
            interactive.process("blockr inverseof blockedby");
            process_lines(interactive, program);
            collector.clear();
            interactive.process(".strata");
            CHECK(any_output_contains(collector, "2 negation levels"));
            CHECK_FALSE(any_output_contains(collector, "Not stratifiable"));

            interactive.process(":start w");
            CHECK(any_output_contains(collector, "w blockr w"));
            CHECK(any_output_contains(collector, "w blockedby w"));
            CHECK_FALSE(any_output_contains(collector, "w s w"));
        } });
}

TEST_CASE("stratified: .strata orders a negation after a rule that declares a relation transitive")
{
    // A rule that ranges over predicates is omitted from the levels if it
    // merely extends the predicates it reads. Which predicates it extends
    // remains contingent on data: specifically, (blk is transitive), which is
    // derived via a negating rule. Until that derivation is complete, the
    // meta-rule is unable to generate (w blk w), hence the rule that negates
    // (w blk w) must wait for the declaring rule, just as it would for any
    // other producer of the fact it negates. The analysis therefore places
    // anything that produces a fact the meta-rule reads under a fixed
    // predicate (`is` in this case) before every rule that reads anything;
    // omitting this step causes both negating rules to be assigned to the
    // same level, resulting in (w s w) being derived.
    //
    // Executed entirely in a single run, with auto-run off followed by .run:
    // line by line, (blk is transitive) is derived by the time (w blk v)
    // arrives, and the order ceases to matter.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        interactive.process("(:mk R, ¬(:off R)) => (R is transitive)");
        interactive.process("(:start A, ¬(A blk A)) => (:s A)");

        collector.clear();
        interactive.process(".strata");
        CHECK(any_output_contains(collector, "2 negation levels"));
        const std::size_t level2  = line_index(collector, "Level 2:");
        const std::size_t declare = line_index(collector, "¬(:off R)");
        const std::size_t negate  = line_index(collector, "¬(A blk A)");
        CHECK(level2 != std::string::npos);
        CHECK(declare < level2);
        CHECK(level2 < negate);

        interactive.process(".auto-run");
        interactive.process(":mk blk");
        interactive.process("w blk v");
        interactive.process("v blk w");
        interactive.process(":start w");
        interactive.process(".run");
        interactive.process("R is transitive");
        interactive.process("A blk A");
        interactive.process("A s A");
        CHECK(answers_contain(collector, "blk is transitive"));
        CHECK(answers_contain(collector, "w blk w"));
        CHECK_FALSE(any_output_contains(collector, "w s w")); });
}

TEST_CASE("stratified: a rule over predicates that reads a second predicate variable is not left out")
{
    // (R pairs S, A R B, B S C) => (A R C) creates R facts; R serves as the
    // predicate in a plain condition -- however, the R facts produced are also
    // contingent upon the facts associated with S, which may represent any
    // predicate. In this instance, the first negating rule creates (w hop z),
    // the meta-rule turns it into (w link z), and the second rule negates
    // exactly that outcome. Omitting the meta-rule from the levels, just as a
    // transitivity rule is excluded, would put both negating rules on level 1
    // with no intermediary, resulting in no report; thus, it still counts as
    // creating every predicate, and .strata lists it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R pairs S, A R B, B S C) => (A R C)");
        interactive.process("(:start A, ¬(:blockp A)) => (A hop z)");
        interactive.process("(:start A, ¬(A link z)) => (:s A)");
        collector.clear();
        interactive.process(".strata");
        const std::size_t header = line_index(collector, "Not stratifiable");
        REQUIRE(header != std::string::npos);
        CHECK(line_index(collector, "pairs", header + 1) != std::string::npos);
        CHECK(line_index(collector, "¬(A link z)", header + 1) != std::string::npos); });
}

// ---------------------------------------------------------------------------
// The payoff: the textbook primality rule, sound under the negation levels
// ---------------------------------------------------------------------------

TEST_CASE("primes-naf: textbook negation rule on the arithmetic modules" * doctest::test_suite("slow"))
{
    run_arithmetic_modules([](auto& collector, const auto& interactive)
                           {
        interactive.process(".import primes-naf");

        SUBCASE("2 is prime (base case)")
        {
            collector.clear();
            interactive.process("(&2 testprime &2) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&2 testprime &2) = prime"));
        }
        SUBCASE("13 is prime, result query is repeatable")
        {
            collector.clear();
            interactive.process("(&13 testprime &13) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&13 testprime &13) = prime"));
            CHECK_FALSE(any_output_contains(collector, "(&13 testprime &13) = composite"));

            collector.clear();
            interactive.process("(&13 testprime &13) = X");
            CHECK(answers_contain(collector, "(&13 testprime &13) = prime"));
        }
        SUBCASE("42 is composite and NOT prime (the race the old engine lost)")
        {
            collector.clear();
            interactive.process("(&42 testprime &42) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&42 testprime &42) = composite"));
            CHECK_FALSE(any_output_contains(collector, "&42 isprime &42"));
            CHECK_FALSE(any_output_contains(collector, "(&42 testprime &42) = prime"));
        }
        SUBCASE("9 is composite (square boundary E*E == N)")
        {
            collector.clear();
            interactive.process("(&9 testprime &9) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&9 testprime &9) = composite"));
            CHECK(any_output_contains(collector, "&9 hasdivisor &3"));
            CHECK_FALSE(any_output_contains(collector, "&9 isprime &9"));
        }
        SUBCASE("several numbers in one run")
        {
            // .strata lists the rule as not stratifiable, since the verdict
            // rule generates an = fact and contains (N testprime N), both of
            // which are examined by the divisor scan (primes-naf.zph says why
            // this is harmless). Soundness thus depends on the module's own
            // argument: every hasdivisor fact is derived before the negation
            // is tested. Check mode would issue a report on an isprime fact if
            // its negated premise were to hold; the most challenging scenario
            // involves multiple scans during a single run, particularly 49
            // with its witness 7 concluding its scan.
            interactive.process(".auto-run");
            collector.clear();
            interactive.process("(&9 testprime &9) = X");
            interactive.process("(&13 testprime &13) = X");
            interactive.process("(&49 testprime &49) = X");
            interactive.run(true, false, false);
            CHECK(any_output_contains(collector, "((&9 testprime &9) = composite"));
            CHECK(any_output_contains(collector, "((&13 testprime &13) = prime"));
            CHECK(any_output_contains(collector, "((&49 testprime &49) = composite"));
            CHECK_FALSE(any_output_contains(collector, "&49 isprime &49"));
            CHECK_FALSE(any_output_contains(collector, "&9 isprime &9"));
        }
        SUBCASE("0 and 1 are neither prime nor composite (no verdict)")
        {
            collector.clear();
            interactive.process("(&1 testprime &1) = X");
            interactive.process("(&0 testprime &0) = X");
            interactive.run(true, false, false);
            CHECK_FALSE(any_output_contains(collector, "(&1 testprime &1) = prime"));
            CHECK_FALSE(any_output_contains(collector, "(&1 testprime &1) = composite"));
            CHECK_FALSE(any_output_contains(collector, "(&0 testprime &0) = prime"));
            CHECK_FALSE(any_output_contains(collector, "(&0 testprime &0) = composite"));
        } });
}

TEST_CASE("stratified: deferred alternation with sugar-form negated conditions")
{
    // Same two-round alternation as the test above, written entirely in
    // self-fact sugar -- including the combination of two desugarings:
    // ":pred X" nested inside "¬(...)" inside a conjunction. Not a bug
    // regression; pinned because this stacking of sugar forms is exercised
    // nowhere else, and grammar changes to either sugar must not break it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(:start A) => (:p A)
(:p A, ¬(:blockp A)) => (:q A)
(:q A) => (:r A)
(:r A, ¬(:blockr A)) => (:s A)
:start w
)");
        CHECK(any_output_contains(collector, "w q w"));
        CHECK(any_output_contains(collector, "w r w"));
        CHECK(any_output_contains(collector, "w s w")); });
}

TEST_CASE("negation: a group of conditions cannot be negated")
{
    // zelph negates a single fact PATTERN. A node tagged as a conjunction
    // was read as one before its negation tag was ever consulted, so
    // "¬(A is y, A is z)" evaluated as "A is y AND A is z" -- the exact
    // opposite of what was written, and without a word about it. Rejecting
    // the combination is checked where the negation is created, which
    // catches the ¬ sugar and the explicit "((...) ~ negation)" spelling
    // alike. The focus spelling "*(...) ~ negation" is no actual negation
    // within a rule at all and is therefore refused, before anything is
    // built.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process("(A is x, ¬(A is y, A is z)) => (A r s)"),
                             doctest::Contains("conjunction and a negation"),
                             std::runtime_error);

        CHECK_THROWS_WITH_AS(
            interactive.process("(A is x, ((*{(A is y) (A is z)} ~ conjunction) ~ negation)) => (A r s)"),
            doctest::Contains("conjunction and a negation"),
            std::runtime_error);

        CHECK_THROWS_WITH_AS(
            interactive.process("(A is x, *(*{(A is y) (A is z)} ~ conjunction) ~ negation) => (A r s)"),
            doctest::Contains("is not a negation"),
            std::runtime_error);

        // The ordinary case is untouched.
        interactive.process("(A is yellow, ¬(A is green)) => (A notgreen green)");
        interactive.process("plant is green");
        interactive.process("plant is yellow");
        interactive.process("plant2 is yellow");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("X notgreen green");
        CHECK(answers_contain(collector, "plant2 notgreen green"));
        CHECK_FALSE(any_output_contains(collector, "plant notgreen")); });
}
