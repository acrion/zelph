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

TEST_CASE("import: missing scripts fail with a standard-library hint, wrong extensions are rejected")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process(".import definitely-not-a-zelph-script"),
                             doctest::Contains("standard library"), std::runtime_error);
        CHECK_THROWS_AS(interactive.process(".import foo.txt"), std::runtime_error); });
}

// ---------------------------------------------------------------------------
// Predicate parsing
// ---------------------------------------------------------------------------

TEST_CASE("parsing: dot-dot predicate")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "g .. h\nh .. i");
        CHECK(any_output_starts_with(collector, "g .. h"));
        CHECK(any_output_starts_with(collector, "h .. i")); });
}

TEST_CASE("parsing: arrow predicates")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
atom_A => atom_B
atom_C <= atom_D
)");
        CHECK(any_output_starts_with(collector, "atom_A => atom_B"));
        CHECK(any_output_starts_with(collector, "atom_C <= atom_D")); });
}

// NOTE: there is no biconditional arrow in the grammar. `<=>` is read as the
// list <=>, which then sits in predicate position; that it renders as such is
// pinned by "display: a list in predicate position renders as a list" in
// test_node_display.cpp. It used to print as "??".

// ---------------------------------------------------------------------------
// Sequences and lists
// ---------------------------------------------------------------------------

TEST_CASE("parsing: compact sequence")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "seq_compact is_defined_as <123>");
        // Compact input builds the list LSB-first; the display no longer
        // reverses it -- that convention belongs to registered numerals.
        CHECK(any_output_contains(collector, "<3 2 1>")); });
}

TEST_CASE("parsing: spaced sequence")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "seq_spaced is_defined_as < seqItem1 seqItem2 seqItem3 >");
        CHECK(any_output_contains(collector, "<seqItem1 seqItem2 seqItem3>")); });
}

TEST_CASE("parsing: quoted sequence keeps its order")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(quoted_sequence ~ < "a" "b" "c" >)");
        CHECK(any_output_contains(collector, "<a b c>")); });
}

// ---------------------------------------------------------------------------
// Nested structures
// ---------------------------------------------------------------------------

TEST_CASE("parsing: an empty container denotes nil")
{
    // `<>` is the empty cons list, and the empty cons list IS the
    // terminator every list ends at; Zelph::set() has always answered nil
    // for the empty set. The parser used to answer neither: it produced
    // Janet's nil, which zelph/fact reads as "no object", so the whole
    // statement vanished without a word.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the empty node list")
        {
            process_lines(interactive, "q p <>");
            CHECK(any_output_contains(collector, "q p nil"));
        }
        SUBCASE("the empty set")
        {
            process_lines(interactive, "q p {}");
            CHECK(any_output_contains(collector, "q p nil"));
        }
        SUBCASE("all three spellings are the same node")
        {
            process_lines(interactive, "q p <>\nq p {}\nq p nil");
            collector.clear();
            interactive.process("q p X");
            std::size_t answers = 0;
            for (const auto& e : collector.events())
                if (normalize(e.text).rfind("Answer:", 0) == 0) ++answers;
            CHECK(answers == 1);
        } });
}

TEST_CASE("parsing: nested sequence in set")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "nested_seq_in_set holds { <setElem1 setElem2> <setElem3 setElem4> }");
        CHECK(any_output_contains(collector, "<setElem1 setElem2>"));
        CHECK(any_output_contains(collector, "<setElem3 setElem4>")); });
}

TEST_CASE("parsing: mixed container")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(mixed_container content < (myCond => myDeduct) (myDeduct2 <= myCond2) { setElem5 setElem6 } "literal string" >)");
        CHECK(any_output_contains(collector, "myCond => myDeduct"));
        CHECK(any_output_contains(collector, "myDeduct2 <= myCond2"));
        CHECK(any_output_contains(collector, "setElem5"));
        CHECK(any_output_contains(collector, "setElem6")); });
}

TEST_CASE("parsing: deep nesting")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(deep_nesting ~ ( Level1 ( Level2 ( Level3 predicate "Level3Object" ) Level2Object) Level1Object))");
        CHECK(any_output_contains(collector, "Level1"));
        CHECK(any_output_contains(collector, "Level1Object")); });
}

TEST_CASE("parsing: set with facts")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "set_logic ~ { (myItem1 IsA myItem2) (myItem2 IsA myItem3) }");
        CHECK(any_output_contains(collector, "myItem1 IsA myItem2"));
        CHECK(any_output_contains(collector, "myItem2 IsA myItem3")); });
}

// ---------------------------------------------------------------------------
// Focus operator and variable queries
// ---------------------------------------------------------------------------

TEST_CASE("focus operator and variable query")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
(*tim ~ human) ~ male
tim _predicate _object
)");
        CHECK(any_output_starts_with(collector, "tim ~ male"));
        CHECK(answers_contain(collector, "tim ~ human"));
        CHECK(answers_contain(collector, "tim ~ male")); });
}

// ---------------------------------------------------------------------------
// Nested unification
// ---------------------------------------------------------------------------

TEST_CASE("nested unification: pattern matching in equations")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
((A + B) = C) => (test A B)
(4 + 5) = 9
)");
        CHECK(any_output_starts_with(collector, "( test 4 5 )")); });
}

TEST_CASE("nested unification: deep structure matching")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
(subj pred (obj is (subj2 A (b test C)))) => (success A C)
subj pred (obj is (subj2 a_val (b test c_val)))
)");
        CHECK(any_output_starts_with(collector, "( success a_val c_val )")); });
}

// ---------------------------------------------------------------------------
// Complex conjunction rule
// ---------------------------------------------------------------------------

TEST_CASE("complex conjunction rule with followed-by")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
((A + B) = C) => (test A B)
(4 + 5) = 9
(*{ ((A + B) = C) (B followed-by D) (C followed-by E) } ~ conjunction) => ((A + D) = E)
5 followed-by 42
9 followed-by 43
)");
        CHECK(any_output_starts_with(collector, "(( 4 + 42 ) = 43 )")); });
}

// ---------------------------------------------------------------------------
// Peano-style rule
// ---------------------------------------------------------------------------

TEST_CASE("peano-style successor rule")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
(A followed-by B) => ((<1> + A) = B)
<0> followed-by <1>
)");
        CHECK(any_output_starts_with(collector, "((<1> + <0>) = <1>)")); });
}

// ---------------------------------------------------------------------------
// Negation
// ---------------------------------------------------------------------------

TEST_CASE("negation: last element of list")
{
    run_both_modes([](const auto& collector, const auto& interactive)
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
(A partoflist L, *(A --> X) ~ negation) => (A "is last of" L)
)");
        CHECK(any_output_starts_with(collector, "( elem5 \"is last of\" mylist )")); });
}

TEST_CASE("negation: syntax sugar with not-green rule")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
(A is yellow, ¬(A is green)) => (A "is not" green)
plant is green
plant is yellow
plant2 is yellow
)");
        // plant is both yellow and green, so rule does not fire for plant.
        // plant2 is yellow but not green, so the rule fires.
        CHECK(any_output_starts_with(collector, "( plant2 \"is not\" green )")); });
}

// ---------------------------------------------------------------------------
// Contradiction detection
// ---------------------------------------------------------------------------

TEST_CASE("contradiction detection")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
(A instanceof B, A subclassof B) => !
gene instanceof geneclass
gene subclassof geneclass
)");
        CHECK(any_output_starts_with(collector, "!"));
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("naming: core-name merge via .name does not deadlock")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.lang en
contradiction is unsatisfiable
.lang zelph
.name ! en contradiction
! P O
)");

        CHECK(answers_contain(collector, "! is unsatisfiable"));
        CHECK_FALSE(any_event_contains(collector, "Resource deadlock avoided")); });
}

TEST_CASE("naming: repeated .name assignment stays stable")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
.lang en
contradiction is unsatisfiable
.lang zelph
.name ! en contradiction
.name ! en contradiction
! P O
)");

        CHECK(answers_contain(collector, "! is unsatisfiable"));
        CHECK_FALSE(any_event_contains(collector, "Resource deadlock avoided")); });
}

// ---------------------------------------------------------------------------
// Janet integration
// ---------------------------------------------------------------------------

TEST_CASE("janet: inline fact and multiline block with deduction")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
%(zelph/fact "Berlin" "is capital of" "Germany")
Germany "is located in" Europe
%
(let [cond (zelph/set
            (zelph/fact 'X "is capital of" 'Y)
            (zelph/fact 'Y "is located in" 'Z))]
(zelph/fact cond "~" "conjunction")
(zelph/fact cond "=>" (zelph/fact 'X "is located in" 'Z)))
%
)");
        CHECK(any_output_starts_with(collector, "( Berlin \"is located in\" Europe )")); });
}

TEST_CASE("janet: unquote referencing janet variable")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
%(def berlin (zelph/resolve "Berlin"))
,berlin ~ town
)");
        CHECK(any_output_starts_with(collector, "Berlin ~ town")); });
}

// ---------------------------------------------------------------------------
// Transitive relation deduction
// ---------------------------------------------------------------------------

TEST_CASE("transitive relation deduction")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(
(R is transitive, A R B, B R C) => (A R C)
6 > 5
5 > 4
> is transitive
)");
        CHECK(any_output_starts_with(collector, "( 6 > 4 )")); });
}

// ---------------------------------------------------------------------------
// Inequality (!=) semantics
//
// Core design decision: different variable names do NOT imply inequality.
// Variables X and Y may bind to the same node unless an explicit X != Y
// constraint is present.  != is a built-in guard (not a fact lookup) that
// filters bindings after the involved variables are bound.
// ---------------------------------------------------------------------------

TEST_CASE("inequality: different variable names may bind to the same value")
{
    // Without !=, two distinct variables can unify with the same node.
    // Rule: if A has property X and property Y, derive has_pair.
    // With only one value "v", X and Y should both bind to "v".
    // Note: objects are stored as adjacency_set, so {v, v} collapses to {v}.
    // The key assertion is that the rule fires at all with only one value.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A prop X, A prop Y) => (A has_pair X Y)
a prop v
)");
        CHECK(any_output_starts_with(collector, "( a has_pair v )")); });
}

TEST_CASE("inequality: != prevents same-value binding")
{
    // Same setup, but X != Y blocks the (v, v) binding.
    // With only one value, the rule must NOT fire at all.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A prop X, A prop Y, X != Y) => (A has_pair X Y)
a prop v
)");
        CHECK_FALSE(any_output_starts_with(collector, "( a has_pair")); });
}

TEST_CASE("inequality: != allows binding when values differ")
{
    // Two different values exist. != should allow the pairs where X != Y.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A prop X, A prop Y, X != Y) => (A has_pair X Y)
a prop v1
a prop v2
)");
        CHECK(any_output_starts_with(collector, "( a has_pair"));
        // Both orderings may appear (objects are a set, so {v1,v2} = {v2,v1}):
        bool has_v1_v2 = any_output_starts_with(collector, "( a has_pair v1 v2 )") ||
                         any_output_starts_with(collector, "( a has_pair v2 v1 )");
        CHECK(has_v1_v2); });
}

TEST_CASE("inequality: contradiction with != (opposite scenario from log)")
{
    // The exact scenario from the bug report: != should not break
    // contradiction detection.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X opposite Y, A ~ X, A ~ Y, X != Y) => !
bright opposite dark
yellow ~ bright
yellow ~ dark
)");
        CHECK(any_output_starts_with(collector, "!"));
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("inequality: contradiction without != when data forces distinct bindings")
{
    // Without !=, X and Y CAN bind to the same value in principle.
    // However, here the only existing "opposite" fact is (bright opposite dark),
    // so the unification forces X=bright, Y=dark — they happen to be distinct
    // because of the data, not because of an implicit inequality constraint.
    // This test verifies that the engine still finds the contradiction in
    // this data-driven scenario.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X opposite Y, A ~ X, A ~ Y) => !
bright opposite dark
yellow ~ bright
yellow ~ dark
)");
        CHECK(any_output_starts_with(collector, "!"));
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("inequality: != with ground constants is trivially true")
{
    // When both sides are ground and unequal, != succeeds immediately.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A likes B, a != b) => (A taste diverse)
joe likes pizza
)");
        CHECK(any_output_starts_with(collector, "( joe taste diverse )")); });
}

TEST_CASE("inequality: != with identical ground constants blocks rule")
{
    // When both sides are ground and equal, != must block the rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A likes B, a != a) => (A taste diverse)
joe likes pizza
)");
        CHECK_FALSE(any_output_starts_with(collector, "( joe taste diverse )")); });
}

TEST_CASE("inequality: a structured operand is resolved, not compared as a pattern")
{
    // A != operand may be a plain variable, a ground node, or a STRUCTURED
    // PATTERN. The pattern case used to be treated as ground: the
    // comparison ran against the pattern node itself, which is never
    // identical to a concrete argument, so the guard silently permitted
    // everything -- the worst failure mode for a guard, since it reads
    // correctly. (A cons R) != &0 is the natural way to write "this
    // numeral is not zero", and the standard library needs it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import binary-arithmetic");
        process_lines(interactive, R"(
((A cons R) probe M, (A cons R) != &0) => ((A cons R) nonzero M)
&0 probe t
&5 probe t
)");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "UNEQ-KEEP-" (zelph/exists (zelph/number "5") "nonzero" "t")))js");
        interactive.process(R"js(%(string "UNEQ-BLOCK-" (zelph/exists (zelph/number "0") "nonzero" "t")))js");
        CHECK(any_output_contains(collector, "UNEQ-KEEP-true"));
        CHECK(any_output_contains(collector, "UNEQ-BLOCK-false")); });
}

TEST_CASE("inequality: a structured operand whose variables bind later still blocks")
{
    // Both call sites must agree: the immediate check in evaluate() and the
    // deferred one in contradicts(). Ordering the guard BEFORE the
    // condition that binds its variables exercises the deferred path.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import binary-arithmetic");
        process_lines(interactive, R"(
((A cons R) != &0, (A cons R) probe M) => ((A cons R) late M)
&0 probe t
&5 probe t
)");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "UNEQ-LATE-KEEP-" (zelph/exists (zelph/number "5") "late" "t")))js");
        interactive.process(R"js(%(string "UNEQ-LATE-BLOCK-" (zelph/exists (zelph/number "0") "late" "t")))js");
        CHECK(any_output_contains(collector, "UNEQ-LATE-KEEP-true"));
        CHECK(any_output_contains(collector, "UNEQ-LATE-BLOCK-false")); });
}

TEST_CASE("inequality: a structured operand denoting no existing fact does not block")
{
    // Resolve::Missing -- every variable bound, but the denoted fact is not
    // in the graph. It cannot be the node the other side is bound to, so
    // the guard must pass rather than block.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X pairs Y, (X op Y) != absent) => (X ok Y)
a pairs b
)");
        interactive.run(true, false, false);
        collector.clear();

        interactive.process(R"js(%(string "UNEQ-MISSING-" (zelph/exists "a" "ok" "b")))js");
        CHECK(any_output_contains(collector, "UNEQ-MISSING-true")); });
}

TEST_CASE("inequality: a guard that never gets its bindings is not a match")
{
    // logic.md states the contract: != is a guard constraint, NOT a fact
    // lookup, and it filters variable bindings AFTER the involved variables
    // are bound by positive conditions. With nothing bound there is nothing
    // to filter -- but the guard used to succeed vacuously, so a query
    // answered its own pattern with the variables still unbound:
    //
    //     zelph> S != O
    //     Answer: S != O          <- on an EMPTY network, too
    //
    // Deferring an undecidable guard stays right while conditions are being
    // joined; the check belongs at the terminal point, where no binding can
    // arrive any more. See Reasoning::guards_unresolved.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("S != O");
        CHECK(collect_answers(collector).empty());

        interactive.process("a rel b");

        // One side bound is no better: the other one still filtered nothing.
        collector.clear();
        interactive.process("a != O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S != b");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("inequality: a guard naming a variable no condition binds blocks the rule")
{
    // The rule-level half of the same contract, and the behaviour change it
    // implies: Z is bound by no positive condition, so the guard never
    // filters anything and the rule must not fire on it. It used to fire,
    // because an unresolvable guard was skipped as "not contradicting".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X rel Y, Y != Z) => (X differs Y)
a rel b
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S differs O");
        CHECK(collect_answers(collector).empty());

        // The same rule with a guard both of whose sides the conditions bind
        // fires as before -- this is the control that the terminal check did
        // not simply disable the guard.
        process_lines(interactive, R"(
(X rel Y, X != Y) => (X apart Y)
c rel c
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S apart O");
        CHECK(answers_contain(collector, "a apart b"));
        CHECK_FALSE(answers_contain(collector, "c apart c")); });
}

TEST_CASE("inequality: reflexive opposite without != causes false positive")
{
    // KEY MOTIVATION for !=:
    // If "opposite" includes a reflexive fact (bright opposite bright),
    // then without != the rule fires with X=Y=bright, A=yellow,
    // which is a spurious contradiction (yellow is bright AND bright,
    // but those are the same thing — not a real conflict).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X opposite Y, A ~ X, A ~ Y) => !
bright opposite bright
yellow ~ bright
)");
        // Without !=, this DOES fire — it's a false positive.
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("inequality: reflexive opposite with != prevents false positive")
{
    // Same scenario, but with != the X=Y=bright binding is blocked.
    // No contradiction should be found.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X opposite Y, A ~ X, A ~ Y, X != Y) => !
bright opposite bright
yellow ~ bright
)");
        CHECK_FALSE(has_contradiction(collector)); });
}

TEST_CASE("inequality: transitive rule with != prevents trivial self-deduction")
{
    // Without !=, (a > b, b > a) would derive a > a.  With != this is blocked.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R is transitive_strict, A R B, B R C, A != C) => (A R C)
a > b
b > a
> is transitive_strict
)");
        // a > b > a should NOT produce a > a
        CHECK_FALSE(any_output_starts_with(collector, "( a > a )"));
        CHECK_FALSE(any_output_starts_with(collector, "( b > b )")); });
}

TEST_CASE("inequality: multiple != constraints in one rule")
{
    // All three variables must be pairwise distinct.
    // Use logged mode to help debug if this fails.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
(X member group, Y member group, Z member group, X != Y, Y != Z, X != Z) => (triple X Y Z)
a member group
b member group
c member group
)");
        // Should produce triples of distinct elements.
        bool found = any_output_starts_with(collector, "( triple a b c )") ||
                     any_output_starts_with(collector, "( triple a c b )") ||
                     any_output_starts_with(collector, "( triple b a c )") ||
                     any_output_starts_with(collector, "( triple b c a )") ||
                     any_output_starts_with(collector, "( triple c a b )") ||
                     any_output_starts_with(collector, "( triple c b a )");
        CHECK(found);
        // Must NOT produce any triple with repeated elements.
        CHECK_FALSE(any_output_contains(collector, "( triple a a"));
        CHECK_FALSE(any_output_contains(collector, "( triple b b"));
        CHECK_FALSE(any_output_contains(collector, "( triple c c")); });
}

TEST_CASE("inequality: functional property conflict detection (Wikidata pattern)")
{
    // Wikidata use case: a property is declared functional (single-valued),
    // but an item has two different values => contradiction.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(P is functional, A P X, A P Y, X != Y) => !
date_of_birth is functional
alice date_of_birth 1990
alice date_of_birth 1991
)");
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("inequality: functional property with same value is not a conflict")
{
    // Same property value entered twice (redundant, not contradictory).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(P is functional, A P X, A P Y, X != Y) => !
date_of_birth is functional
alice date_of_birth 1990
alice date_of_birth 1990
)");
        CHECK_FALSE(has_contradiction(collector)); });
}

// ---------------------------------------------------------------------------
// Meta-rules: predicates as first-class nodes
// ---------------------------------------------------------------------------

TEST_CASE("meta-rule: symmetric relation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R is symmetric, X R Y) => (Y R X)
friend is symmetric
alice friend bob
)");
        CHECK(any_output_starts_with(collector, "( bob friend alice )")); });
}

TEST_CASE("meta-rule: opposite relation generates inverse")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R "is opposite of" S, X R Y) => (Y S X)
"has part" "is opposite of" "is part of"
chimpanzee "has part" hand
)");
        CHECK(any_output_starts_with(collector, "( hand \"is part of\" chimpanzee )")); });
}

// ---------------------------------------------------------------------------
// Multiple objects: unordered object set
// ---------------------------------------------------------------------------

TEST_CASE("multiple objects: unordered set with rule matching")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
alice parent_of bob charlie
(A parent_of B) => (B child_of A)
)");
        CHECK(any_output_starts_with(collector, "( bob child_of alice )"));
        CHECK(any_output_starts_with(collector, "( charlie child_of alice )")); });
}

// ---------------------------------------------------------------------------
// Deep unification: function composition (using lists for ordering)
// ---------------------------------------------------------------------------

TEST_CASE("deep unification: function composition as graph transformation")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
(F maps <A B>, G maps <B C>) => ((G compose F) maps <A C>)
f maps <item1 item2>
g maps <item2 item3>
)");
        CHECK(any_output_starts_with(collector, "((g compose f) maps <item1 item3>)")); });
}

// ---------------------------------------------------------------------------
// Constraint checking: graph coloring
// ---------------------------------------------------------------------------

TEST_CASE("constraint checking: valid graph coloring produces no contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A adjacent B, A color X, B color X) => !
r1 adjacent r2
r2 adjacent r3
r1 color red
r2 color blue
r3 color red
)");
        CHECK_FALSE(has_contradiction(collector)); });
}

TEST_CASE("constraint checking: invalid graph coloring produces contradiction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A adjacent B, A color X, B color X) => !
r1 adjacent r2
r2 adjacent r3
r1 color red
r2 color red
)");
        CHECK(has_contradiction(collector)); });
}

TEST_CASE("janet: zelph/sources returns only subjects, never objects")
{
    // Regression test: given a chain a R b R c, the sources of b must be
    // exactly {a}. The node c (where b is the *subject*) must not appear.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
nodeA P279 nodeB
nodeC P279 nodeB
nodeB P279 nodeD
)");
        collector.clear();
        interactive.process(R"(%(string/join (sorted (map (fn [n] (zelph/name n)) (zelph/sources "P279" (zelph/resolve "nodeB")))) ","))");
        CHECK(any_output_contains(collector, "nodeA,nodeC"));
        CHECK_FALSE(any_output_contains(collector, "nodeD")); });
}

TEST_CASE("janet: a statement ABOUT a fact is not a subject of that fact's predicate")
{
    // zelph/sources is documented to return "exactly those nodes S for which
    // the fact S predicate target exists". A fact that has the fact as its
    // SUBJECT -- a reified statement, a rule condition, the rule-pattern
    // marking -- is linked to it in both directions exactly like its own
    // subject, and the bidirectionality test could not tell the two apart. So
    // `(a p b) note ok` reported itself as a subject of `p`, and the SPARQL
    // layer, which is built on this primitive, returned it as a result row.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x p b
(a p b) note ok
)");
        collector.clear();
        interactive.process(R"(%(string "SRC-" (length (zelph/sources "p" "b"))))");
        CHECK(any_output_contains(collector, "SRC-2"));

        // The query is the second reading of the same question and answers
        // two; the count above must not exceed it.
        collector.clear();
        interactive.process("S p b");
        CHECK(collect_answers(collector).size() == 2);

        // The reifying statement is still reachable as what it is.
        collector.clear();
        interactive.process(R"(%(string "NOTE-" (length (zelph/sources "note" "ok"))))");
        CHECK(any_output_contains(collector, "NOTE-1")); });
}

TEST_CASE("janet: a self-fact has a subject and an object like any other")
{
    // `a p a` stores its object in the subject's bidirectional entry, so the
    // "object is unidirectional" test found no object at all and the "subject
    // is not the object" test dropped the subject: both traversals reported
    // NOTHING for a fact the query answers. Sets and lists are built on these
    // two functions, so this was not a curiosity of self-referential data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("m p m");

        collector.clear();
        interactive.process(R"(%(string "S-" (zelph/name (first (zelph/sources "p" "m")))))");
        CHECK(any_output_contains(collector, "S-m"));

        collector.clear();
        interactive.process(R"(%(string "T-" (zelph/name (first (zelph/targets "m" "p")))))");
        CHECK(any_output_contains(collector, "T-m"));

        collector.clear();
        interactive.process("S p m");
        CHECK(collect_answers(collector).size() == 1); });
}

// ---------------------------------------------------------------------------
// Variable name sharing across rules
// ---------------------------------------------------------------------------

TEST_CASE("naming: variable names re-used by a later rule keep earlier rules intact")
{
    // Each statement creates fresh variable nodes; rule topology is anchored
    // via core.Causes / core.Conjunction and stays unambiguous even when two
    // rules use the same variable NAMES (A, B, C). This test guards both the
    // functional property and the display: assigning "A" to rule 2's fresh
    // variable must not strip the name from rule 1's variable.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A ancestor B, B ancestor C) => (A ancestor C)
(R is transitive, A R B, B R C) => (A R C)
x ancestor y
y ancestor z
5 > 4
4 > 3
> is transitive
)");
        // Functional: BOTH rules fire despite shared variable names.
        CHECK(any_output_starts_with(collector, "( x ancestor z )"));
        CHECK(any_output_starts_with(collector, "( 5 > 3 )"));

        // Display: rule 1's variables are still shown by name in
        // .list-rules -- which prints the same unmarked form as every other
        // command, so a listed rule can be pasted straight back in.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(A ancestor B)"));
        CHECK_FALSE(any_output_contains(collector, "?? ancestor")); });
}

// ---------------------------------------------------------------------------
// Bound-pattern grounding: exact object-set semantics for nested patterns
// ---------------------------------------------------------------------------

TEST_CASE("grounding: fully bound nested pattern requires the exact fact node")
{
    // Documents a deliberate design decision of bound-pattern grounding
    // (ground_pattern in unification.cpp): once all variables of a
    // structured condition SUBJECT are bound, the pattern denotes exactly
    // one fact node, resolved via hash lookup with EXACT object-set
    // semantics -- consistent with instantiate_fact() and the termination
    // guard. Deep unification's greedy subset matching of objects is
    // deliberately NOT replicated at this point: a graph fact carrying
    // additional objects is a different node and is not found. (Top-level
    // condition objects keep the documented "objects as alternatives"
    // subset semantics via extract_bindings.) To restore subset matching
    // for this corner case, ground_pattern would have to return Unbound
    // instead of Missing -- at the cost of falling back to a full scan.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x trigger b1
(a d+ b1 b2) ci c
((a d+ b1 b2) ci c) co e
(x trigger B, ((a d+ B) ci c) co E) => (found B E)
)");
        // The first condition binds B = b1 (bound subject x guarantees it
        // is evaluated first). The grounded pattern ((a d+ b1) ci c) then
        // denotes a fact node that does not exist -- only the multi-object
        // variant ((a d+ b1 b2) ci c) does -- so the condition fails
        // instead of subset-matching the multi-object fact.
        CHECK_FALSE(any_output_starts_with(collector, "( found")); });
}

TEST_CASE("grounding: fully bound nested pattern anchors on the exact fact node")
{
    // Positive control for the exact-object semantics above: with the
    // exact single-object fact present, grounding resolves the pattern to
    // the concrete node and the rule fires via a direct anchor (no scan).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
.deductions all
x trigger b1
(a d+ b1) ci c
((a d+ b1) ci c) co e
(x trigger B, ((a d+ B) ci c) co E) => (found B E)
)");
        CHECK(any_output_starts_with(collector, "( found b1 e )")); });
}

// ---------------------------------------------------------------------------
// Template rejection: variables at depth >= 2
// ---------------------------------------------------------------------------

TEST_CASE("unification: consequence templates with variables only at depth 2 are not matched as data")
{
    // Distilled from the multiplication junk-fact regression: the first
    // rule's consequence pattern exists in the graph as an out-fact whose
    // ONLY variable A sits at structural depth 2 -- its subject decomposes
    // to a hash node plus the constant 0, so a shallow template check sees
    // nothing. The second rule scans out-facts; before the deep template
    // reject it matched the template, bound X to a variable-containing
    // node, and deduced junk.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((A probe nil) state 0) => (((A probe nil) state 0) out nil)
(X out P) => (X leaked P)
)");
        // IMPORTANT: clear AFTER defining the rules. The REPL echoes rule
        // definitions, and the echo of rule 2 literally contains the word
        // "leaked" -- the original version of this test checked the
        // collector including that echo and failed even though the engine
        // behaved correctly.
        collector.clear();
        interactive.process("seed1 unrelated seed2");
        interactive.run(true, false, false);
        CHECK_FALSE(any_output_contains(collector, "leaked"));

        // Positive control: a concrete state fact drives the same pipeline
        // legitimately -- rule 1 derives a ground out-fact, rule 2 consumes it.
        collector.clear();
        interactive.process("(d1 probe nil) state 0");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "leaked")); });
}

// ---------------------------------------------------------------------------
// Self-referential facts: subject == object reconstruction
// ---------------------------------------------------------------------------

TEST_CASE("parse_fact: self-referential fact keeps its object once it becomes the subject of further facts")
{
    // fact() draws no separate object edge when object == subject; the
    // subject IS the object, and parse_fact repairs the reconstructed
    // object set accordingly. The repair existed only in the
    // single-candidate branch: as soon as the self-referential fact became
    // the SUBJECT of other facts (their backlinks add bidirectional
    // neighbors), the disambiguation path dropped the implicit object --
    // rendering ((x foo ?) ...) and handing an empty object set to every
    // parse_fact consumer (node_to_string, deduce, != guard, negation).
    // Division X/X surfaced this systematically.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x foo x
(x foo x) bar a
)");
        // This third statement's echo renders the inner (x foo x) while a
        // second consumer (the bar fact) already exists -- the exact
        // constellation that forced the disambiguation path.
        collector.clear();
        interactive.process("(x foo x) baz b");
        CHECK(any_output_contains(collector, "x foo x"));
        CHECK_FALSE(any_output_contains(collector, "foo ?")); });
}

TEST_CASE("rules: a chained => says which arrow has to be parenthesised")
{
    // "A => B => C" is one statement whose predicate `=>` also stands among
    // its objects, so it lands in the generic "same relation type and
    // object" refusal -- accurate, and no help at all to someone writing a
    // rule whose conclusion is a rule. Which arrow binds tighter is
    // genuinely undecided, so the answer is a demand to say, not a default.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process("(R is transitive) => (X R Y, Y R Z) => (X R Z)"),
                             doctest::Contains("has to be parenthesised"),
                             std::runtime_error);

        // The parenthesised form is accepted.
        interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))"); });
}

TEST_CASE("rules: a ground condition is not a non-match")
{
    // Matching a condition that contains no variable binds nothing, and the
    // engine read "nothing bound" as "no match" -- which silently disabled
    // every rule one of whose conditions happens to name its nodes. Both
    // conditions here are satisfied, so the rule has to fire.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(a p b, X q d) => (X r s)");
        interactive.process("a p b");
        collector.clear();
        interactive.process("e q d");
        CHECK(any_deduction_of(collector, "e r s"));

        collector.clear();
        interactive.process("X r Y");
        CHECK(answers_contain(collector, "e r s")); });
}

TEST_CASE("rules: a ground condition that does NOT hold blocks the rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a p b, X q d) => (X r s)");
        interactive.process("e q d");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("X r Y");
        CHECK_FALSE(answers_contain(collector, "e r s")); });
}

TEST_CASE("rules: a negated consequence is refused, not ignored")
{
    // "(A p B) => ¬(A q B)" used to derive (x q y) from (x p y) -- the exact
    // opposite of what it says, in silence: deduce reads the consequence's
    // predicate and creates the fact, and the negation tag sits beside the
    // pattern where nothing on that path looks.
    //
    // The test has to be asked of the SYNTAX. The tag is a fact ABOUT the
    // pattern node, and a ground pattern is hash-consed, so a pattern negated
    // in one rule carries the tag in every other rule that mentions it --
    // only the statement itself knows where the "¬" was written.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const char* rule : {"(A p B) => ¬(A q B)",
                                 "(A p B) => (¬(A q B))",
                                 "(A p B) => (A q B) ¬(A r B)"})
        {
            CHECK_THROWS_WITH_AS(interactive.process(rule),
                                 doctest::Contains("condition operator"),
                                 std::runtime_error);
        }

        // A negated CONDITION is untouched, including when its pattern is
        // ground and therefore shared with whatever else mentions it.
        interactive.process("(A p B, ¬(A r B)) => (A q B)");
        collector.clear();
        interactive.process("x p y");
        CHECK(any_deduction_of(collector, "x q y"));

        collector.clear();
        interactive.process("X q Y");
        CHECK(answers_contain(collector, "x q y")); });
}

TEST_CASE("rules: a nested negation is refused, not silently halved")
{
    // "¬" tags the pattern node, and tagging it twice is tagging it once, so
    // "¬(¬(F))" meant "¬(F)" -- the exact opposite of a double negation. The
    // rule then fired precisely when it should not have, in silence. Reading
    // it properly needs a second negation stratum, which is the parked
    // feature "¬(A, B)" is refused for; saying so beats dropping half the
    // statement.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const char* rule : {"¬(¬(a p b)) => (c q d)",
                                 "¬((¬(a p b))) => (c q d)",
                                 "(X p Y, ¬(¬(X q Y))) => (X r Y)"})
        {
            CHECK_THROWS_WITH_AS(interactive.process(rule),
                                 doctest::Contains("does not nest"),
                                 std::runtime_error);
        }

        // One level is untouched, and still means what it says: the rule
        // fires while the fact is absent and not once it is there.
        interactive.process("¬(a p b) => (c q d)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("C q D");
        CHECK(answers_contain(collector, "c q d"));

        interactive.process("m p n");
        interactive.process("¬(m p n) => (e r f)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("E r F");
        CHECK_FALSE(answers_contain(collector, "e r f")); });
}

TEST_CASE("rules: two consequences are two objects, not a conjunction")
{
    // "A => (B, C)" builds a rule whose consequence is a conjunction SET.
    // The engine deduces the OBJECTS of a `=>` fact and has no reading for
    // a set node in that position, so the rule was accepted, listed by
    // .list-rules -- and derived nothing at all, in silence. zelph can say
    // what was meant; it is spelled with several objects.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH_AS(interactive.process("(X p Y) => (X q N, N r Y)"),
                             doctest::Contains("several objects"),
                             std::runtime_error);

        // The form the message names works, and the two consequences share
        // the fresh variable N -- which is the reason it is one rule and
        // not two.
        interactive.process(".deductions all");
        interactive.process("(X p Y) => (X q N) (N r Y)");
        collector.clear();
        interactive.process("a p b");
        REQUIRE(any_deduction_of(collector, "a q"));
        CHECK(any_deduction_of(collector, "r b"));

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "=>")); });
}

TEST_CASE("naming: a query variable does not take a real node's name")
{
    // Variable names are cosmetic and statement-scoped, but they went into
    // the same map as real names and won. A graph holding a node named "A"
    // -- a single-letter Wikidata label is enough -- lost that name to the
    // first query that mentioned the variable A, and the node afterwards
    // rendered as "(?? ?? ??)". Asking a question deleted data.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("alpha rel beta");
        interactive.process(".name alpha A");

        collector.clear();
        interactive.process("A rel beta");
        // The answer binds the variable A to the node NAMED "A", and that
        // name is quoted on the way out: bare, the line would read back as
        // the very query that produced it rather than as its answer.
        CHECK(answers_contain(collector, "\"A\" rel beta"));
        CHECK_FALSE(any_output_contains(collector, "??"));

        // The name still resolves to the node it was given to.
        collector.clear();
        interactive.process(".node A");
        CHECK(any_output_contains(collector, "Name in language"));
        CHECK_FALSE(any_output_contains(collector, "No node found")); });
}

TEST_CASE("rules: a rule whose only condition quantifies over predicates fires")
{
    // "For every declared relation type R, ..." is the shape that makes zelph
    // different from a query engine, and as the SOLE condition of a rule it
    // derived nothing at all -- silently. Adding any second condition, even a
    // pure guard like `R != p`, made it work again, which is why it went
    // unnoticed: every example that quantifies over predicates in the stdlib
    // and the documentation carries a second condition.
    //
    // The cause was in Zelph::filter, the three-argument form that answers
    // "which node of this fact is its predicate". A fact's outgoing edges
    // hold its PARENTS as well as its subject and predicate, so the
    // consequence `R declared yes` has the rule among them -- and the rule
    // points at its own subject, the condition `R ~ ->`. That condition has
    // the right predicate and the right object, so the walk reported the RULE
    // as a second relation type of the consequence, and deduce() refused the
    // ambiguity. The exact probe `check_fact(nd, ~, ->)` asks the question
    // that was meant: not "does nd reach such a fact" but "is nd its
    // subject".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");

        collector.clear();
        interactive.process("(R ~ ->) => (R declared yes)");
        interactive.process(".run");

        collector.clear();
        interactive.process("S declared yes");

        // Every relation type in the graph: the one the data declared, the
        // rule's own consequence predicate, and the core vocabulary --
        // including `~` itself, which is a relation type declared by a fact
        // whose subject IS its predicate.
        CHECK(answers_contain(collector, "p declared yes"));
        CHECK(answers_contain(collector, "declared declared yes"));
        CHECK(answers_contain(collector, "~ declared yes"));
        CHECK(answers_contain(collector, "cons declared yes"));
        CHECK(answers_contain(collector, "in declared yes"));

        // Not a relation type: `a` and `b` are data, `->` is the category.
        CHECK_FALSE(answers_contain(collector, "a declared yes"));
        CHECK_FALSE(answers_contain(collector, "b declared yes"));
        CHECK_FALSE(answers_contain(collector, "-> declared yes"));

        // A COMPOSITE predicate is bound like any other. logic.md claims
        // exactly this -- "the fact is found by a rule quantifying over
        // predicates just like any other" -- and the claim was false for the
        // single-condition form the sentence describes.
        interactive.process("x (a p b) y");
        interactive.process(".run");

        collector.clear();
        interactive.process("S declared yes");
        CHECK(answers_contain(collector, "(a p b) declared yes")); });
}

TEST_CASE("rules: a consequence subject that is itself a predicate stays the subject")
{
    // The control for the fix above: the exact probe replaced the
    // neighbourhood walk, but the SUBJECT exclusion in front of it still has
    // to hold. It only bites when the consequence's subject is a GROUND node
    // that is itself a declared relation type -- `p` here, used as data by a
    // rule that has nothing to do with predicates. Drop the exclusion and
    // `check_fact(p, ~, ->)` succeeds, the consequence has two candidate
    // predicates, and deduce() refuses it.
    //
    // A variable subject would not do: the pattern node carries the variable,
    // and a variable is not a declared relation type, so the test would pass
    // either way. That was the first version of this case, and it was
    // vacuous.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("alarm ranks high");

        collector.clear();
        interactive.process("(X ranks high) => (p scored X)");
        interactive.process(".run");

        collector.clear();
        interactive.process("p scored O");
        CHECK(answers_contain(collector, "p scored alarm")); });
}

TEST_CASE("rules: a composite predicate in a consequence is instantiated")
{
    // deduce() substituted a predicate that IS a variable and nothing else, so
    // a COMPOSITE one kept the rule's own variables. `(X p Y) => (X (Y r s) c)`
    // derived `a (Y r s) c` -- a fact carrying a template variable, which no
    // query can match and which the ground guard did not catch either, because
    // that guard read the subject and the objects but not the predicate.
    //
    // Both halves are pinned here: the predicate is substituted, and nothing
    // with a residual variable reaches the graph.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(q r s) ~ ->
(X p Y) => (X (Y r s) c)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S Q O");
        CHECK(answers_contain(collector, "a (b r s) c"));
        CHECK_FALSE(answers_contain(collector, "a (Y r s) c"));

        // The instantiated predicate is declared as a relation type, which is
        // what keeps the derived fact readable after a reload.
        collector.clear();
        interactive.process("S ~ ->");
        CHECK(answers_contain(collector, "(b r s) ~ ->")); });
}

TEST_CASE("rules: a container in predicate position is rebuilt like any other")
{
    // Same path, reached through a container rather than a fact: the
    // predicate is not exempt from the rebuild that objects get, since only
    // the object of a PartOf deduction is written INTO.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(X p Y) => (X {Y} c)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S Q O");
        CHECK(answers_contain(collector, "a {b} c"));
        CHECK_FALSE(answers_contain(collector, "a @{Y} c")); });
}

TEST_CASE("rules: a composite predicate in a condition unifies structurally")
{
    // Subject and object positions have unified structurally all along --
    // `((Y r s) p Z)` and `(X p (Y r s))` both match -- but the predicate was
    // compared by IDENTITY, so `(X (Y r s) Z)` matched nothing whatsoever: the
    // graph holds `(b r s)`, never `(Y r s)`. The rule was accepted and
    // silently inert, and no binding order helped, because the candidate set
    // is fixed when the condition is set up rather than when it is joined.
    //
    // The candidate set is now the one a predicate VARIABLE gets; what
    // separates the two is that extract_bindings unifies the pattern against
    // each candidate instead of binding one variable to it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a (b r s) c
d (e r s) f
a (b r t) c
a p c
(X (Y r s) Z) => (Y links X)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S links O");
        CHECK(answers_contain(collector, "b links a"));
        CHECK(answers_contain(collector, "e links d"));

        // The fixed parts of the pattern still select: `(b r t)` differs in
        // its object, `p` is not composite at all.
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("rules: a pattern predicate is narrowed by its own predicate")
{
    // A predicate pattern gets the candidate set a predicate VARIABLE gets --
    // every declared relation type -- which is correct but is the cost of a
    // variable, and the pattern says far more than a variable does. A
    // candidate has to unify with `(Y r s)`, and unify_nodes matches
    // predicates before anything else, so no fact whose predicate is not `r`
    // can survive; the candidates are therefore the facts of `r` alone.
    //
    // What is checked here is that the narrowing loses NOTHING. `bulk` is the
    // relation the narrowed scan never looks at, and its facts must be
    // exactly as absent from the answers as they were before.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a (b r s) c
d (e r s) f
g (h r t) i
j (k q s) l
m bulk n
o bulk p
(X (Y r s) Z) => (Y links X)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S links O");
        CHECK(answers_contain(collector, "b links a"));
        CHECK(answers_contain(collector, "e links d"));

        // `(h r t)` differs in the object, `(k q s)` in the predicate, and
        // `bulk` is not composite at all.
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("rules: a pattern predicate whose own predicate is a variable still matches")
{
    // The fallback the narrowing needs: with `(Y R s)` there is no ground
    // predicate to narrow by, so the candidate set stays every declared
    // relation type -- and the rule has to keep working, or the optimisation
    // would have silently taken a shape away.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a (b r s) c
d (e q s) f
g (h r t) i
(X (Y R s) Z) => (Y links Z)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S links O");
        CHECK(answers_contain(collector, "b links c"));
        CHECK(answers_contain(collector, "e links f"));

        // `(h r t)` still differs in the object.
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("reasoning: the two strategies agree on what is REPORTED, not only on what is derived")
{
    // `.semi-naive check` compares derived FACTS. Anything the engine REPORTS
    // rather than derives -- a contradiction, and since this session a refusal
    // -- is invisible to it by construction, so a divergence there would be
    // caught by nothing at all.
    //
    // This closes that gap for one deliberately awkward network: a rule
    // GENERATOR writes the transitivity rule, the closure it produces is what
    // a contradiction rule then fires on (so the contradiction depends on
    // DERIVED facts, which is where delta seeding differs from a classic
    // pass), a COMPOSITE PREDICATE PATTERN runs beside it, and a REFUSED
    // deduction is reported from a third rule. Both strategies have to agree
    // on the derived facts AND on how many `!` lines come out.
    const std::string network = R"(
p is transitive
a p b
b p c
c p d
m (n r s) o
z rel {a b}
q p2 r
(R is transitive) => ((X R Y, Y R Z) => (X R Z))
(A p d, A p b) => !
(X (Y r s) Z) => (Y links X)
(X p2 Y) => (X in {a b})
)";

    const auto contradiction_lines = [](const zelph::io::OutputCollector& c)
    {
        return std::count_if(c.events().begin(), c.events().end(), [](const auto& e)
                             { return normalize(e.text).find("⇐") != std::string::npos
                                   && normalize(e.text).starts_with("!"); });
    };

    const auto run_with = [&](const char* mode)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(mode);
        process_lines(interactive, network);
        interactive.run(true, false, false);

        const auto bangs = contradiction_lines(collector);

        collector.clear();
        interactive.process("S p O");
        auto answers = collect_answers(collector);

        collector.clear();
        interactive.process("S links O");
        for (const auto& a : collect_answers(collector))
            answers.push_back(a);

        std::sort(answers.begin(), answers.end());
        return std::make_pair(answers, bangs);
    };

    const auto delta   = run_with(".semi-naive on");
    const auto classic = run_with(".semi-naive off");

    // The closure, the pattern-predicate consequence, and nothing else.
    REQUIRE(delta.first.size() == 7);
    CHECK(delta.first == classic.first);

    // One contradiction from the closure, two refusals from the third rule --
    // and the same number either way.
    CHECK(delta.second > 0);
    CHECK(delta.second == classic.second);
}
