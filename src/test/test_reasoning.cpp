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

// NOTE: <=> parsing is currently broken (displays as ??). Uncomment when fixed.
// TEST_CASE("parsing: biconditional arrow")
// {
//     run_both_modes([](const auto& collector, const auto& interactive)
//     {
//         process_lines(interactive, "(a <=> b) is_type equivalence");
//         CHECK(any_output_contains(collector, "<=>"));
//     });
// }

// ---------------------------------------------------------------------------
// Sequences and lists
// ---------------------------------------------------------------------------

TEST_CASE("parsing: compact sequence")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "seq_compact is_defined_as <123>");
        CHECK(any_output_contains(collector, "<123>")); });
}

TEST_CASE("parsing: spaced sequence")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "seq_spaced is_defined_as < seqItem1 seqItem2 seqItem3 >");
        CHECK(any_output_contains(collector, "< seqItem1 seqItem2 seqItem3 >")); });
}

TEST_CASE("parsing: quoted sequence is reversed")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, R"(quoted_sequence ~ < "a" "b" "c" >)");
        CHECK(any_output_contains(collector, "<cba>")); });
}

// ---------------------------------------------------------------------------
// Nested structures
// ---------------------------------------------------------------------------

TEST_CASE("parsing: nested sequence in set")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        process_lines(interactive, "nested_seq_in_set holds { <setElem1 setElem2> <setElem3 setElem4> }");
        CHECK(any_output_contains(collector, "< setElem1 setElem2 >"));
        CHECK(any_output_contains(collector, "< setElem3 setElem4 >")); });
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
        // Display truncates inner levels to ??, but the parser must accept the input.
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
        CHECK(any_output_starts_with(collector, "( elem5 is last of mylist )")); });
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
        CHECK(any_output_starts_with(collector, "( plant2 is not green )")); });
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
        CHECK(any_output_starts_with(collector, "( Berlin is located in Europe )")); });
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
        CHECK(any_output_starts_with(collector, "( hand is part of chimpanzee )")); });
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
(F maps <A B>, G maps <B C>) => ((G compose F) maps <A C>)
f maps <item1 item2>
g maps <item2 item3>
)");
        CHECK(any_output_starts_with(collector, "(( g compose f ) maps < item1 item3 >)")); });
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

        // Display: rule 1's variables are still shown by name in .list-rules.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(A «ancestor» B)"));
        CHECK_FALSE(any_output_contains(collector, "«??» «ancestor»")); });
}
