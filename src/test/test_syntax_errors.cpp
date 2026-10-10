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

// ---------------------------------------------------------------------------
// What an UNPARSABLE statement is told.
//
// The cases above work on the PEG's syntax tree: the parse succeeded and only
// the shape was refused. A parse that FAILS leaves no tree, no position and no
// expectation -- a PEG has no error productions -- so the whole message had to
// come from somewhere, and for years it came from nowhere: "Syntax error:
// Could not parse statement." names nothing at all.
//
// One rule accounts for most of what people actually type. `:stmt-any`
// separates two values by `:s+`, so a value glued to an opening parenthesis is
// not a statement -- and `f(x)` is the shape every reader of mathematics
// writes. The report that prompted this arrived through `%`, which is a LINE
// escape and can never be a term; that case has an answer of its own, the
// unquote `,name`, so it gets its own sentence.
// ---------------------------------------------------------------------------

TEST_CASE("syntax errors: a value glued to a parenthesis says so, and says where the space goes")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process("a b x(c d e)"),
                             doctest::Contains("\"x(\" is a value glued to a \"(\""),
                             std::runtime_error);
        // The advice has to be the thing that actually parses.
        interactive.process("a b x (c d e)");
        CHECK(any_output_contains(collector, "a b x (c d e)")); });
}

TEST_CASE("syntax errors: '%' inside a statement is answered with the unquote, not with a space")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // `%` is a whole-LINE escape (Interactive::process step 6), so no
        // amount of whitespace makes it a term. Advising a space here would
        // send the reader in the wrong direction; what they want is to bind
        // the Janet value and unquote its name.
        CHECK_THROWS_WITH_AS(interactive.process(R"(x knows %(some/fn "arg"))"),
                             doctest::Contains("escapes a whole LINE to Janet"),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process(R"(x knows %(some/fn "arg"))"),
                             doctest::Contains(",v"),
                             std::runtime_error); });
}

TEST_CASE("syntax errors: '$(' names the module that registers it, not a missing space")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // `$( ... )` is an inline keyword the standard library registers, not
        // core syntax. Without `.import math-syntax` it reaches the parser as
        // a `$` glued to a group, and "write $ (" is advice that produces a
        // nonsense fact instead of an error.
        CHECK_THROWS_WITH_AS(interactive.process("$(x + y) ist gut"),
                             doctest::Contains("math-syntax"),
                             std::runtime_error); });
}

TEST_CASE("syntax errors: the glue rule does not fire where a glued group is correct")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Three rules read their own operand with `:s*`, so an operand glued
        // to them is right and saying otherwise would name a token the user
        // got correct. `¬` is the one that costs attention: it is TWO bytes,
        // and a byte-wise reading of the reserved set reported `¬(a b)` --
        // which parses -- as a value glued to a parenthesis.
        collector.clear();
        interactive.process("(A p B, ¬(A q B)) => (A r B)");
        CHECK_FALSE(any_event_contains(collector, "glued"));

        collector.clear();
        interactive.process(":unary(a b c)");
        CHECK_FALSE(any_event_contains(collector, "glued"));

        // And a comma followed by a group is the conjunction separator, which
        // is why the diagnosis for ",(" is restricted to the start of a
        // statement.
        collector.clear();
        interactive.process("(x p y, (a q b)) => (x r y)");
        CHECK_FALSE(any_event_contains(collector, "glued")); });
}

TEST_CASE("syntax errors: a parenthesis inside a quoted atom is text, not structure")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // The scan has to honour the two escapes a quoted atom knows, or a
        // name ending in `\"` closes the atom one character early and the
        // rest of the line is read in the wrong state -- which would name a
        // token out of the middle of a string.
        collector.clear();
        interactive.process(R"(a b "name with (a paren")");
        CHECK_FALSE(any_event_contains(collector, "glued"));
        CHECK_FALSE(any_event_contains(collector, "Could not parse statement")); });
}

// ---------------------------------------------------------------------------
// A refusal must leave the session usable.
//
// Janet unwinds with setjmp/longjmp. A C++ exception travelling out of a Janet
// C function therefore skips Janet's own cleanup and leaves the VM in a state
// whose NEXT failure has nothing to do with what caused it -- so every zelph
// function reachable from Janet now converts an exception into a Janet panic at
// the boundary (see the guard in script_engine_setup.cpp).
//
// The case below is how it was found, and it is the reason this belongs here:
// the poison is an ordinary REFUSAL, one the engine is right to raise. A
// session that met it once was silently broken from then on, and the damage
// surfaced somewhere else entirely -- three imports deep in the standard
// library, on a line that is perfectly correct, with a bare "Janet error".
// ---------------------------------------------------------------------------

namespace
{
    // A rule written without the parentheses around its comma list. The last
    // condition then reads as one statement whose predicate `R` also stands
    // among its objects, which fact() refuses.
    constexpr const char* kRefusedRule = "R is transitive, A R B, B R C => A R C";
}

TEST_CASE("syntax errors: a refused statement does not poison the Janet VM")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_AS(interactive.process(kRefusedRule), std::runtime_error);

        collector.clear();
        interactive.process(R"js(%(zelph/out (string "SUM-" (+ 1 2))))js");
        CHECK(any_output_contains(collector, "SUM-3"));

        collector.clear();
        interactive.process(R"js(%(zelph/out (string "MAP-" (length (map inc [1 2 3])))))js");
        CHECK(any_output_contains(collector, "MAP-3"));

        // Nothing may have been reported that the session did not ask for.
        CHECK_FALSE(any_event_contains(collector, "Janet error")); });
}

TEST_CASE("syntax errors: a refused statement does not break a later import" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The reported shape, verbatim: the refusal, then `.import math`,
        // which pulls in topoly and polynomial and does a great deal of Janet
        // work on the way. It used to die on polynomial.zph:294 with "Janet
        // error", naming a rule that has nothing wrong with it.
        CHECK_THROWS_AS(interactive.process(kRefusedRule), std::runtime_error);

        collector.clear();
        CHECK_NOTHROW(interactive.process(".import math"));
        CHECK_FALSE(any_event_contains(collector, "Janet error"));

        // And the imported module actually works afterwards.
        collector.clear();
        interactive.process("<x> ~ polyring");
        CHECK_FALSE(any_event_contains(collector, "Janet error")); });
}

TEST_CASE("syntax errors: an unparenthesised rule names the node in both roles")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // "facts with same relation type and object are not supported" said
        // neither WHICH node stands in both roles nor what to do, and nobody
        // reaches this refusal by writing that on purpose. The rule arrow
        // among the objects is the tell that a rule was meant.
        CHECK_THROWS_WITH_AS(interactive.process(kRefusedRule),
                             doctest::Contains("\"R\" is both the predicate and an object"),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process(kRefusedRule),
                             doctest::Contains("(A, B, C) => (D)"),
                             std::runtime_error);

        // The parenthesised form is what the advice points at, so it has to
        // be the one that works.
        collector.clear();
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        CHECK_FALSE(any_event_contains(collector, "both the predicate and an object")); });
}

TEST_CASE("syntax errors: a statement spanning lines is named whole, not by its last line")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        // A rule written over two lines -- the shape the standard library uses
        // throughout -- was reported by the line that happened to complete it.
        // `=> (A r C, X)` is correct on its own and is not where the mistake
        // is, so the message pointed at the wrong place; in an imported module
        // it pointed at a line the reader had never written.
        interactive.process("(A p B, B q C)");
        CHECK_THROWS_WITH_AS(interactive.process("=> (A r C, X)"),
                             doctest::Contains("Error in statement \"(A p B, B q C) => (A r C, X)\""),
                             std::runtime_error);

        // A statement on ONE line keeps the wording it always had.
        CHECK_THROWS_WITH_AS(interactive.process("a b x(c d e)"),
                             doctest::Contains("Error in line \"a b x(c d e)\""),
                             std::runtime_error);

        // Additionally, a comment follows it. The parser interprets the
        // line excluding the comment, meaning that matching this processed
        // text against the original line would classify every line with a
        // comment as a statement spanning multiple lines; only a genuine
        // line break may switch the wording, and the line is quoted
        // exactly as it was typed.
        CHECK_THROWS_WITH_AS(interactive.process("a b x(c d e)   # glued"),
                             doctest::Contains("Error in line \"a b x(c d e)   # glued\""),
                             std::runtime_error); });
}
