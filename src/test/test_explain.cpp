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

TEST_CASE("explain: depth limit and the ? companion idiom (all arithmetic modules)" * doctest::test_suite("slow"))
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

TEST_CASE("explain: NAF premises render as absent (all arithmetic modules)" * doctest::test_suite("slow"))
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

// The search stops at the first justification it can rebuild, which is a
// deliberate cost decision -- and it used to be invisible, so a tree over a
// fact reached two ways read as THE derivation of it. That is a stronger claim
// than the engine makes, and it is the one an auditability argument rests on.
// A second verified instantiation is now looked for at the root and named.
TEST_CASE("explain: a fact reached two ways says so")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X target Y)
(X q Y) => (X target Y)
a p b
a q b
)");
        interactive.run(false, false, false);

        collector.clear();
        interactive.process(".explain (a target b) 0");
        CHECK(any_output_contains(collector, "one of several justifications"));
        // The one it does show is still a complete, checkable derivation.
        CHECK(any_output_contains(collector, "[axiom]")); });
}

// The counterpart: one derivation must not grow the annotation, or it says
// nothing. The premise here is reachable by exactly one rule.
TEST_CASE("explain: a fact reached one way is not annotated")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X target Y)
a p b
)");
        interactive.run(false, false, false);

        collector.clear();
        interactive.process(".explain (a target b) 0");
        CHECK(any_output_contains(collector, "a p b"));
        CHECK_FALSE(any_output_contains(collector, "one of several justifications")); });
}

// Only the ROOT is scanned for a second justification, and that is a cost
// decision: looking at every level would turn a linear walk into a quadratic
// one. It is invisible in the two tests above, because there the fact reached
// twice IS the root -- so this is the case that pins it, and it is the one a
// later change would break silently.
//
// The same fact, in both positions, on one graph. `a mid b` is reached through
// p and through q, so as the root of its own tree it says so. Standing as the
// premise of `a target b` -- whose own derivation is unique -- it carries no
// annotation, because nothing looked. Asking both of ONE graph is what makes
// this a statement about the position rather than about the fact: a change that
// starts annotating deeper nodes makes the second half fail, and a change that
// stops annotating at all makes the first half fail.
TEST_CASE("explain: the annotation is the root's, not the premise's")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X mid Y)
(X q Y) => (X mid Y)
(X mid Y) => (X target Y)
a p b
a q b
)");
        interactive.run(false, false, false);

        collector.clear();
        interactive.process(".explain (a mid b) 0");
        REQUIRE(any_output_contains(collector, "one of several justifications"));

        collector.clear();
        interactive.process(".explain (a target b) 0");
        // The premise is in the tree and expanded -- so the annotation's
        // absence is about where it stands, not about the tree stopping short.
        CHECK(any_output_contains(collector, "a mid b"));
        CHECK(any_output_contains(collector, "[axiom]"));
        CHECK_FALSE(any_output_contains(collector, "one of several justifications")); });
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

TEST_CASE("explain: a transient variable does not take a name lookup with it")
{
    // .explain evaluates its pattern inside a scratch cluster and drops it
    // again, so the variables it builds are removed. They are NAMED while
    // they live, though, and the name map that answers ".node A" used to be
    // handed to the newest owner -- which was the transient one. Dropping it
    // erased the entry, so a read-only command turned a working lookup into
    // "No node found with name 'A'" while every rule went on displaying A,
    // and the two name maps disagreed from then on, in the session and in a
    // .bin saved from it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("(A p B) => (A q B)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".node A");
        CHECK(any_output_contains(collector, "Variable: yes"));
        const std::string before = last_out_text(collector);

        collector.clear();
        interactive.process(".explain (A p B)");

        collector.clear();
        interactive.process(".node A");
        CHECK(any_output_contains(collector, "Variable: yes"));
        CHECK(last_out_text(collector) == before);

        // The control for the branch next to it: a REAL node keeps its name
        // against a variable of the same letter, which is what a single-letter
        // Wikidata label depends on.
        interactive.process(".name a en \"B\"");
        interactive.process(".lang en");
        collector.clear();
        interactive.process("B p C");
        collector.clear();
        interactive.process(".node B");
        CHECK(any_output_contains(collector, "Variable: no"));
        interactive.process(".lang zelph"); });
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

TEST_CASE("explain: an argument that names a node is not a parse failure")
{
    // One message covered four different situations, and the one it named --
    // a parse failure -- was the only one it usually was NOT. It matters most
    // where it is most natural to type: the engine reports a contradiction
    // with its premises on the "⇐" line, and ".explain !" answered that the
    // argument might not parse. It parses; a contradiction is simply not a
    // fact, and it materializes nothing, so there is nothing left to
    // reconstruct afterwards.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto message_of = [&interactive](const char* line)
        {
            try
            {
                interactive.process(line);
            }
            catch (const std::exception& ex)
            {
                return std::string(ex.what());
            }
            return std::string{};
        };

        interactive.process("(X p Y) => !");
        interactive.process("a p b");
        interactive.run(true, false, false);

        CHECK(message_of(".explain !").find("contradiction marker") != std::string::npos);
        CHECK(message_of(".explain a").find("is a node, not a fact") != std::string::npos);
        CHECK(message_of(".explain ~").find("is a node, not a fact") != std::string::npos);

        // A name that denotes nothing keeps the message that fits it.
        CHECK(message_of(".explain nosuchnode").find("cannot parse fact pattern") != std::string::npos);

        // And the working case is untouched.
        collector.clear();
        interactive.process(".explain a p b");
        CHECK(any_output_contains(collector, "[axiom]")); });
}
