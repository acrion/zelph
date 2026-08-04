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
// .explain: proof reconstruction from the saturated graph. No provenance is
// tracked during inference -- these tests pin that the backward search alone
// recovers justifications, labels axioms honestly, respects the depth limit,
// and prints shared subproofs once.
// ---------------------------------------------------------------------------

TEST_CASE("explain: axioms and single-step derivations")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X likes Y) => (Y liked-by X)");
        interactive.process("alice likes bob");
        interactive.run(true, false, false);

        SUBCASE("input facts are axioms")
        {
            collector.clear();
            interactive.process(".explain alice likes bob");
            CHECK(any_output_contains(collector, "[axiom]"));
        }
        SUBCASE("derived facts show their premise")
        {
            collector.clear();
            interactive.process(".explain bob liked-by alice");
            CHECK(any_output_contains(collector, "alice likes bob"));
            CHECK(any_output_contains(collector, "[axiom]"));
        }
        SUBCASE("unasserted facts are reported, not invented")
        {
            collector.clear();
            interactive.process(".explain bob likes alice");
            CHECK(any_output_contains(collector, "not asserted"));
        } });
}

TEST_CASE("explain: a premise carrying further objects is found")
{
    // Unification matches a one-object condition against a fact that carries
    // more -- `(X p Y)` binds Y to b and to c of `a p b c`, and the rule
    // fires twice, exactly as the query answers twice. The proof search
    // resolved its premises by the exact triple hash, where `a p b` is a
    // DIFFERENT node that does not exist, so it found no derivation at all
    // and labelled a derived fact "asserted; no derivation found" -- the one
    // thing .explain must never say about a fact nobody asserted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b c");
        interactive.process("(X p Y) => (X q Y)");
        interactive.run(true, false, false);

        for (const char* target : {".explain (a q b)", ".explain (a q c)"})
        {
            collector.clear();
            interactive.process(target);
            CHECK(any_output_contains(collector, "a p b c")); // the fact that MATCHED
            CHECK(any_output_contains(collector, "[axiom]"));
            CHECK_FALSE(any_output_contains(collector, "no derivation found"));
        }

        // The exactness the proof search opts out of stays elsewhere: a
        // prune asked for `a p b` must not take the longer fact with it.
        collector.clear();
        interactive.process(".prune-facts (a p b)");
        CHECK(any_output_contains(collector, "Pruned 0"));

        collector.clear();
        interactive.process("S p O");
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("explain: depth limit and the ? companion idiom (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process("? (&6 + &7)");

        SUBCASE("bare .explain explains the last output node")
        {
            collector.clear();
            interactive.process(".explain");
            CHECK(any_output_contains(collector, "&13"));
        }
        SUBCASE("depth 1 truncates, depth 0 does not")
        {
            collector.clear();
            interactive.process(".explain ((&6 + &7) = &13) 1");
            CHECK(any_output_contains(collector, "depth limit"));

            collector.clear();
            interactive.process(".explain ((&6 + &7) = &13) 0");
            CHECK_FALSE(any_output_contains(collector, "depth limit"));
        } });
}

TEST_CASE("explain: shared subproofs print once")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // (a q b) is DERIVED and used twice: directly as a premise of
        // (a done b), and again as the premise of (a r b). Hash-consing
        // makes both occurrences the same node, so the second one must
        // reference the first instead of re-printing its subtree.
        // Axiom leaves are NOT subject to this: "[axiom]" already is the
        // complete expansion and stays readable when repeated.
        interactive.process("(X p Y) => (X q Y)");
        interactive.process("(X q Y) => (X r Y)");
        interactive.process("(X q Y, X r Y) => (X done Y)");
        interactive.process("a p b");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain (a done b) 0");
        CHECK(any_output_contains(collector, "[see above]")); });
}

TEST_CASE("explain: NAF premises render as absent (all arithmetic modules)")
{
    run_arithmetic_modules([](auto& collector, auto& interactive)
                           {
        interactive.process(".import primes-naf");
        interactive.process("? :testprime &7");

        collector.clear();
        interactive.process(".explain (:testprime &7) = prime 0");
        CHECK(any_output_contains(collector, "[absent]")); });
}

TEST_CASE("explain: a numeral object is not mistaken for the depth argument")
{
    // A trailing all-digit token is read as max-depth. That reading must
    // not swallow the fact's own OBJECT: ".explain <subject> <pred> 0"
    // would otherwise leave a two-component statement behind, which the
    // AST builder rejects with a leaked arity error. Every digit-level
    // fact of the arithmetic modules has this shape, so the failure was
    // reachable from the very first thing a reader is likely to inspect.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A gate B) => ((A gate B) out 0)");
        interactive.process("1 gate 1");
        interactive.run(true, false, false);

        SUBCASE("the numeral stays part of the pattern")
        {
            collector.clear();
            interactive.process(".explain (1 gate 1) out 0");
            CHECK(any_output_contains(collector, "1 gate 1"));
            CHECK_FALSE(any_output_contains(collector, "arity mismatch"));
            CHECK_FALSE(any_output_contains(collector, "cannot parse"));
        }
        SUBCASE("the parenthesized spelling keeps working")
        {
            collector.clear();
            interactive.process(".explain ((1 gate 1) out 0)");
            CHECK(any_output_contains(collector, "1 gate 1"));
            CHECK_FALSE(any_output_contains(collector, "cannot parse"));
        }
        SUBCASE("an explicit depth is still honoured")
        {
            // Reading (1) resolves here, so the trailing token IS the depth
            // -- and depth 1 must cut the tree below the root.
            collector.clear();
            interactive.process(".explain ((1 gate 1) out 0) 1");
            CHECK(any_output_contains(collector, "depth limit"));
        } });
}

TEST_CASE("explain: a NAF premise is printed bound, and negated exactly once")
{
    // The rule's negated condition is stored as a pattern node tagged
    // (~ negation), which node_to_string already renders as "¬(...)".
    // The renderer used to add a SECOND "¬(...)" around it and passed no
    // bindings, so the honest premise "¬(plant2 is green)" came out as
    // "¬(¬(A is green))" -- the opposite claim, with an unbound variable.
    // Variables that occur ONLY inside the negation stay unbound on
    // purpose: that is what "for no D" quantifies over.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A is yellow, ¬(A is green)) => (A notgreen green)");
        interactive.process("plant is green");
        interactive.process("plant is yellow");
        interactive.process("plant2 is yellow");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain plant2 notgreen green");
        CHECK(any_output_contains(collector, "¬(plant2 is green)  [absent]"));
        CHECK_FALSE(any_output_contains(collector, "¬(¬")); });
}

TEST_CASE("explain: a quoted multi-word predicate resolves")
{
    // zelph PRINTS a predicate containing spaces quoted, so pasting that
    // line back into .explain must work. The command tokenizer strips the
    // quotes, and rejoining the tokens with blanks turned `a "is not" b`
    // into the four-component `a is not b`, which denotes a different
    // node -- the fact was reported as not asserted.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a \"is not\" b");

        SUBCASE(".explain finds it")
        {
            collector.clear();
            interactive.process(".explain a \"is not\" b");
            CHECK(any_output_contains(collector, "[axiom]"));
            CHECK_FALSE(any_output_contains(collector, "not asserted"));
        }
        SUBCASE("the .why alias behaves identically")
        {
            collector.clear();
            interactive.process(".why a \"is not\" b");
            CHECK(any_output_contains(collector, "[axiom]"));
        } });
}

TEST_CASE("explain: a quoted name without spaces resolves too")
{
    // Whitespace used to be the only surviving evidence that a token had
    // been quoted, so only a name with a space could be re-quoted for the
    // parser. Every other name that zelph prints quoted -- and the quoting
    // work made that a large set -- could not be pasted back: `x>y` was
    // handed to the parser bare and read as the three atoms `x > y`.
    // The tokenizer now reports each token in parser form as well.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // One representative per reason a name gets quoted: a reserved
        // character, each of the tokens the grammar reads by its first
        // character, and a name carrying a quote of its own.
        const char* names[] = {"x>y", ":foo", "&12", "_foo", "A", "a,b", "«Le Monde»"};
        int         i       = 0;
        for (const char* name : names)
        {
            const std::string subj = "s" + std::to_string(++i);
            interactive.process(subj + " rel obj" + std::to_string(i));
            interactive.process(".name obj" + std::to_string(i) + " \"" + name + "\"");

            collector.clear();
            interactive.process(".explain (" + subj + " rel \"" + name + "\")");
            CHECK(any_output_contains(collector, "[axiom]"));
            CHECK_FALSE(any_output_contains(collector, "cannot parse fact pattern"));
        }

        // A name carrying a quote, which needs the escape on both sides.
        interactive.process("sq rel objq");
        interactive.process(".name objq \"The \\\"Big\\\" One\"");
        collector.clear();
        interactive.process(".explain (sq rel \"The \\\"Big\\\" One\")");
        CHECK(any_output_contains(collector, "[axiom]")); });
}

TEST_CASE("pruning: a quoted name without spaces reaches the pattern")
{
    // Same cause, worse consequence: .prune-facts refused the pattern
    // outright ("Could not parse pattern"), so the fact zelph printed could
    // not be deleted by pasting the line back.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("sub rel obj");
        interactive.process(".name obj \"x>y\"");
        interactive.process("keep rel other");

        collector.clear();
        interactive.process(".prune-facts sub rel \"x>y\"");
        CHECK(any_output_contains(collector, "Pruned 1 matching facts"));

        collector.clear();
        interactive.process("S rel O");
        CHECK(answers_contain(collector, "keep rel other"));
        CHECK_FALSE(any_output_contains(collector, "x>y")); });
}

TEST_CASE("explain: a rejected reading of the argument stays silent")
{
    // cmd_explain TRIES readings of its argument; a failing one is a
    // normal outcome. janet_dostring prints its stack trace before it
    // returns the error status, so the speculative evaluation has to
    // suppress Janet's own reporting -- otherwise a successful .explain
    // is preceded by an "arity mismatch" trace that looks like a crash.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A gate B) => ((A gate B) out 0)");
        interactive.process("1 gate 1");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".explain ((1 gate 1) out 0)");
        CHECK(any_output_contains(collector, "[axiom]"));
        CHECK_FALSE(any_output_contains(collector, "arity mismatch")); });
}

TEST_CASE("help: an alias is documented under its canonical command")
{
    // One table drives both the dispatch registration and ".help <alias>".
    // While ".why" was registered separately, it was a working command
    // that ".help .why" claimed not to know.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process(".help .why");
        CHECK(any_output_contains(collector, "alias: .why"));
        CHECK_FALSE(any_output_contains(collector, "Unknown command")); });
}

TEST_CASE("help: a topic may be named with or without its dot")
{
    // The listing prints every command with its dot, so that is what gets
    // pasted -- but the bare name is at least as natural to type, and
    // ".help deductions" answered "Unknown command: deductions" about a
    // command that not only exists but is one of the most used.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process(".help deductions");
        CHECK(any_output_contains(collector, "Sets the deduction printing mode"));
        CHECK_FALSE(any_output_contains(collector, "Unknown command"));

        // Aliases resolve bare too, and an unknown topic is still an error.
        collector.clear();
        interactive.process(".help why");
        CHECK(any_output_contains(collector, "alias: .why"));

        collector.clear();
        // The refusal goes to the error channel, not to Out.
        interactive.process(".help nosuchthing");
        CHECK(any_event_contains(collector, "Unknown command")); });
}
