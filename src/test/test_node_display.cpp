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

#include <filesystem>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Node display / reconstruction
//
// Most output checking in this suite is implicit: tests assert the presence
// of expected substrings as a side effect of testing reasoning semantics.
// That style has two systematic blind spots, both of which have produced
// real bugs:
//   1. Structures that only REASONING creates (never parsed input) take
//      reconstruction paths no test script exercises.
//   2. Presence checks don't catch silently DROPPED components -- a '?' or
//      a missing tail passes any contains() assertion aimed elsewhere.
// This file collects cases where the rendered output IS the tested
// semantics: round-trips and reconstruction of node structures. It is not
// the start of a systematic display suite; it grows when a reconstruction
// path breaks.
// ---------------------------------------------------------------------------

TEST_CASE("display: improper cons chains render their tail (rule patterns)")
{
    // A cons chain not ending at nil is not a proper list. The list
    // formatter used to collect the cars and silently drop the tail,
    // rendering the rule pattern (A cons R) as <A>. Improper chains must
    // render in explicit cons input syntax instead.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("((A cons R) probe M) => (R probe M)");
        // The echo must contain the full pattern including the tail
        // variable R, in round-trippable input syntax.
        CHECK(any_output_contains(collector, "(A cons R)"));
        CHECK_FALSE(any_output_contains(collector, "<A>")); });
}

TEST_CASE("display: improper cons chain with atomic tail")
{
    // Data-level improper list: (a cons b) where b is a plain atom, not
    // nil. Historically rendered as <a>, hiding both the tail and the
    // fact that the chain is unterminated.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("(a cons b) tagged t1");
        CHECK(any_output_contains(collector, "a cons b"));
        CHECK_FALSE(any_output_contains(collector, "<a>")); });
}

TEST_CASE("display: a one-element list does not read back as a compact list")
{
    // "<abc>" without whitespace is the COMPACT list, one node per
    // character. A one-element node list has no separator, so it rendered
    // into exactly that syntax: < item2 > printed as <item2>, which reads
    // back as the five-element list <2 m e t i> -- a different structure,
    // silently, and reachable from any list at all, since every inner cell
    // of <item1 item2> is a one-element list.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("f maps < item2 >");
        collector.clear();
        interactive.process("f maps X");
        CHECK(answers_contain(collector, "f maps < item2 >"));
        CHECK_FALSE(any_output_contains(collector, "m e t i"));

        // Re-entering what was printed must denote the SAME fact.
        interactive.process("f maps < item2 >");
        collector.clear();
        interactive.process("f maps X");
        std::size_t answers = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).rfind("Answer:", 0) == 0) ++answers;
        CHECK(answers == 1);

        // Unambiguous renderings stay as they were: a single CHARACTER
        // means the same list under either reading, and a separator or a
        // quoted element already stops the compact rule.
        interactive.process("g maps < 7 >");
        collector.clear();
        interactive.process("g maps X");
        CHECK(answers_contain(collector, "g maps <7>"));

        interactive.process("h maps < \"a b\" >");
        collector.clear();
        interactive.process("h maps X");
        CHECK(answers_contain(collector, "h maps <\"a b\">")); });
}

TEST_CASE("display: proper lists keep their compact rendering")
{
    // Guard against over-correction: nil-terminated chains must continue
    // to render as lists (<123>) and, with a registered digit alphabet,
    // as &-literals -- the existing display paths are untouched.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("<123> tagged t2");
        CHECK(any_output_contains(collector, "<3 2 1>"));

        interactive.process(".import decimal-arithmetic");
        collector.clear();
        interactive.process("&42 tagged t3");
        CHECK(any_output_starts_with(collector, "&42 tagged t3")); });
}

TEST_CASE("display: self-referential fact as subject of further facts")
{
    // End-to-end companion to the parse_fact pinning test in
    // test_reasoning.cpp: the constellation that division X/X produces
    // systematically. Kept here as the display-level regression anchor.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(x foo x) bar a
(x foo x) baz b
)");
        collector.clear();
        interactive.process("(x foo x) qux c");
        CHECK(any_output_contains(collector, "x foo x"));
        CHECK_FALSE(any_output_contains(collector, "foo ?")); });
}

TEST_CASE("node display: non-canonical digit lists render raw, not as &-literals (binary mul)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Binary &3 * &0 accumulates <0> + <00>: the prod fact carries the
        // zero-extended raw list <00> (only the user-facing = result is
        // canonicalized via canonnum, see common-arithmetic MC0). The raw
        // node must render structurally, visibly distinct from &0.
        interactive.process(".import binary-arithmetic");
        collector.clear();
        interactive.process("&3 * &0");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "((&3 mul &0) prod <00>)"));
        CHECK_FALSE(any_output_contains(collector, "((&3 mul &0) prod &0)"));
        // The canonicalized user-facing result stays a &-literal.
        CHECK(any_output_contains(collector, "((&3 * &0) = &0)")); });
}

TEST_CASE("node display: non-canonical digit lists render raw, not as &-literals (decimal sub)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Decimal &105 - &98 internally yields the raw diff <007> (the
        // SC0 comment's own example); the canonnum bridge line now shows
        // the connection between both renderings explicitly.
        interactive.process(".import decimal-arithmetic");
        collector.clear();
        interactive.process("&105 - &98");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "diff <007>"));
        CHECK(any_output_contains(collector, "(<007> canonnum &7)"));
        CHECK(any_output_contains(collector, "((&105 - &98) = &7)"));
        CHECK_FALSE(any_output_contains(collector, "diff &7")); });
}

TEST_CASE("display: tagging a structured term does not hide it behind the concept")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("(x + y) ~ t");
        // "is an instance of" is not "is": the term keeps its own structure,
        // so the echo stays re-enterable input.
        CHECK(any_output_contains(collector, "(x + y) ~ t"));
        CHECK_FALSE(any_output_contains(collector, "t ~ t")); });
}

TEST_CASE("display: a compiled term renders in full as a nested subterm" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // A term that has been through topoly acquires predicates of its
        // own (aspoly, needstopoly, mul). z->parse_fact's candidate filter
        // discards such a subject, and the fallback walk used to take the
        // first bidirectional neighbour of the fact node -- which includes
        // every fact the node is the SUBJECT of, among them the parent
        // currently being rendered. The history check then printed '?'.
        // Only the nested position was affected: at top level parent == 0,
        // so there was no ancestor to pick wrongly.
        interactive.process(".import topoly");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
x pouter y
:topoly (((x * x) * y) + x)
)");
        collector.clear();
        interactive.process("((x * x) * y) foo probe");
        CHECK(any_output_contains(collector, "((x * x) * y) foo probe"));
        // Both failure shapes seen in practice: the ancestor collapsing to
        // '?', and a fallback that finds no candidate at all ('??').
        CHECK_FALSE(any_output_contains(collector, "(? * y)"));
        CHECK_FALSE(any_output_contains(collector, "(?? * y)")); });
}

TEST_CASE("display: a compiled term keeps its structure across several parents" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // The wrong subject was picked from an unordered adjacency set, so
        // which foreign fact won depended on how many facts the term was
        // part of. Rendering the same term under several different parents
        // pins that the recorded structure wins every time.
        interactive.process(".import topoly");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
x pouter y
:topoly (((x * x) * y) + x)
((x * x) * y) foo probe
)");
        collector.clear();
        interactive.process("((x * x) * y) bar probe2");
        interactive.process("(x * x) qux probe3");
        CHECK(any_output_contains(collector, "((x * x) * y) bar probe2"));
        CHECK(any_output_contains(collector, "(x * x) qux probe3"));
        CHECK_FALSE(any_output_contains(collector, "?")); });
}

TEST_CASE("display: rendering under active logging does not recurse" * doctest::test_suite("slow"))
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Rendering consults get_fact_structures, whose log messages are
        // built with format() -- which renders again. With logging on,
        // that pair recursed without bound and overflowed the stack, so
        // should_log() suppresses log output while a rendering is in
        // progress. Reaching the CHECKs at all is half the assertion here.
        interactive.process(".import topoly");
        process_lines(interactive, R"(
x ~ symvar
y ~ symvar
x pouter y
:topoly (((x * x) * y) + x)
)");
        interactive.process(".log 3");
        collector.clear();
        interactive.process("((x * x) * y) foo probe");
        interactive.process(".log 0");
        CHECK(any_output_contains(collector, "((x * x) * y) foo probe")); });
}

TEST_CASE("display: a name the parser would misread is quoted")
{
    // "Output should round-trip" is a design rule, not a nicety: a printed
    // fact is meant to be re-enterable. Quoting only names containing a
    // SPACE was not enough -- a node named "x>y" printed as `a rel x>y`,
    // where '>' closes a list, so the line reads back as something else.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b");
        interactive.process(".name b \"x>y\"");

        collector.clear();
        interactive.process("a rel X");
        CHECK(answers_contain(collector, "a rel \"x>y\""));

        // Re-entering what was printed must denote the SAME fact, i.e. no
        // second fact appears for the same subject and predicate.
        interactive.process("a rel \"x>y\"");
        collector.clear();
        interactive.process("a rel X");
        std::size_t answers = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).rfind("Answer:", 0) == 0) ++answers;
        CHECK(answers == 1); });
}

TEST_CASE("display: a bracket-shaped name is still a name")
{
    // Whether a rendering is a leaf name or a composed structure was guessed
    // from its shape: anything starting with '(' '<' '{' or ending with ')'
    // '>' '}' was taken for a sub-expression and left unmarked, which also
    // took it past the quoting. "Mercury (planet)" -- the shape of every
    // disambiguated Wikidata label -- therefore printed bare, and
    // `Mercury (planet) orbits sun` reads back as four atoms, not as the
    // fact that was printed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("sun body star");
        interactive.process(".name body \"Mercury (planet)\"");

        collector.clear();
        interactive.process("sun X star");
        CHECK(answers_contain(collector, "sun \"Mercury (planet)\" star"));

        // What was printed must denote the same fact when entered again.
        interactive.process("sun \"Mercury (planet)\" star");
        collector.clear();
        interactive.process("sun X star");
        std::size_t answers = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).rfind("Answer:", 0) == 0) ++answers;
        CHECK(answers == 1); });
}

TEST_CASE("display: a name the grammar reads by its first character is quoted")
{
    // The quoting rule works from the PEG's reserved character SET, which
    // cannot see the tokens the grammar recognises by their first character:
    // "_x" and a single uppercase letter are variables, "&12" is a number
    // literal, "≈net" opens a neural condition. A node really named that way
    // exists -- Wikidata has single-letter labels -- and the variable case
    // was the damaging one, because it failed SILENTLY: `a rel _foo` read
    // back as a fact about the variable _foo, a different graph with no
    // complaint.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a rel b");
        interactive.process(".name b \"_foo\"");
        collector.clear();
        interactive.process("a rel Z");
        CHECK(answers_contain(collector, "a rel \"_foo\""));

        // Re-entering the printed form must denote the SAME fact.
        interactive.process("a rel \"_foo\"");
        collector.clear();
        interactive.process("a rel Z");
        std::size_t answers = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).rfind("Answer:", 0) == 0) ++answers;
        CHECK(answers == 1);

        interactive.process("e rel f");
        interactive.process(".name f \"&12\"");
        collector.clear();
        interactive.process("e rel Z");
        CHECK(answers_contain(collector, "e rel \"&12\""));

        interactive.process("p rel q");
        interactive.process(".name q \"≈net\"");
        collector.clear();
        interactive.process("p rel Z");
        CHECK(answers_contain(collector, "p rel \"≈net\"")); });
}

TEST_CASE("display: a variable keeps its bare name")
{
    // The counterpart: a VARIABLE is not a node reference and must stay
    // unquoted, or every rule echo would come out as {("A" R "B")}. The
    // difference is not in the string -- it is in the graph, and that is
    // where the renderer now asks.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        CHECK(any_output_contains(collector, "(A R C)"));
        CHECK_FALSE(any_output_contains(collector, "\"A\""));
        CHECK_FALSE(any_output_contains(collector, "\"R\"")); });
}

TEST_CASE("display: a sequence element is compared against the marked name")
{
    // The S-P-O formatter wraps an element in parentheses when its rendering
    // differs from its own name, i.e. when it is composite. Inside a
    // sequence that comparison was made against the UNmarked name, so every
    // plain name containing a space looked composite: <"a b" item> came out
    // as <("a b") item>.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("f maps <\"a b\" item2>");
        collector.clear();
        interactive.process("f maps X");
        CHECK(answers_contain(collector, "f maps <\"a b\" item2>"));
        CHECK_FALSE(any_output_contains(collector, "(\"a b\")")); });
}

TEST_CASE("display: a name starting with a colon is quoted")
{
    // A leading colon opens the self-fact sugar, so a node NAMED ":foo"
    // could not be printed bare: as a subject the line failed with an arity
    // error, and inside a multi-object fact the sugar swallowed the next
    // object and produced a nested self-fact instead -- silently.
    //
    // The quoting rule could not say so while the renderer marked
    // ":" + predicate as ONE leaf, because then every self-fact looked like
    // a name beginning with a colon. Splitting the two is what makes the
    // rule expressible.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("sfoo rel x");
        interactive.process(".name sfoo \":foo\"");

        collector.clear();
        interactive.process("S rel x");
        CHECK(answers_contain(collector, "\":foo\" rel x"));

        // Re-entering the printed form denotes the SAME fact.
        interactive.process("\":foo\" rel x");
        collector.clear();
        interactive.process("S rel x");
        std::size_t answers = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).rfind("Answer:", 0) == 0) ++answers;
        CHECK(answers == 1);

        // The sugar itself is untouched: its colon is not part of a name.
        interactive.process("narc friend narc");
        collector.clear();
        interactive.process("A friend B");
        CHECK(answers_contain(collector, ":friend narc"));
        CHECK_FALSE(any_output_contains(collector, "\":friend\"")); });
}

TEST_CASE("display: the self-fact sugar gives way to a predicate that needs quoting")
{
    // There is no way to quote a name inside ":pred subject", so the sugar
    // is only used where the predicate prints BARE. The gate asks the
    // quoting rules rather than restating them, which is what keeps a
    // predicate named "&12" -- a number literal to the parser -- in the
    // verbose form instead of rendering an unreadable :"&12".
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("narc \"&12\" narc");
        collector.clear();
        interactive.process("A \"&12\" B");
        CHECK(answers_contain(collector, "narc \"&12\" narc"));
        CHECK_FALSE(any_output_contains(collector, ":\"&12\"")); });
}

TEST_CASE("display: a name containing a quote is writable")
{
    // The quoted-atom rule had no escapes, so a name carrying a quote could
    // not be written at all. It was therefore printed BARE, and
    // `subj rel The "Big" One` reads back as a fact with THREE objects --
    // silently. Wikidata values really do carry quotes; the fixture in
    // test_wikidata_qualifiers.cpp is one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("subj rel obj");
        interactive.process(".name obj \"The \\\"Big\\\" One\"");

        collector.clear();
        interactive.process("subj rel X");
        CHECK(answers_contain(collector, "subj rel \"The \\\"Big\\\" One\""));

        // Re-entering the printed form denotes the SAME fact.
        interactive.process("subj rel \"The \\\"Big\\\" One\"");
        collector.clear();
        interactive.process("subj rel X");
        std::size_t answers = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).rfind("Answer:", 0) == 0) ++answers;
        CHECK(answers == 1); });
}

TEST_CASE("display: a backslash in a name is a backslash")
{
    // A quoted atom was handed to Janet verbatim, so JANET's escape set
    // applied to zelph text: a node written `a\b` arrived named
    // `a<backspace>`, and a Windows path lost most of itself. zelph knows
    // exactly two escapes inside a quoted atom, \" and \\; a backslash in
    // front of anything else is an ordinary character.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("x rel a\\b");
        collector.clear();
        interactive.process("x rel X");
        CHECK(answers_contain(collector, "x rel a\\b"));
        CHECK_FALSE(any_output_contains(collector, "a\b"));

        // Quoted, the escape is honoured and the printed form escapes it
        // again, so the line stays re-enterable.
        interactive.process("w rel \"C:\\\\Users\\\\x\"");
        collector.clear();
        interactive.process("w rel X");
        CHECK(answers_contain(collector, "w rel C:\\Users\\x"));

        interactive.process("v rel \"p\\\\b q\"");
        collector.clear();
        interactive.process("v rel X");
        CHECK(answers_contain(collector, "v rel \"p\\\\b q\"")); });
}

TEST_CASE("display: a name containing guillemets survives the identifier marking")
{
    // The renderer marks leaf names with « », in band. A name that contains
    // those characters itself used to be split by its own content:
    // "«Le Monde»" came out as "\"«Le Monde\"»". French and German Wikidata
    // labels make this a real case.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("s rel o");
        interactive.process(".name o \"«Le Monde»\"");

        collector.clear();
        interactive.process("s rel X");
        CHECK(answers_contain(collector, "s rel \"«Le Monde»\"")); });
}

TEST_CASE("display: a name the grammar has a token for stays bare")
{
    // The quoting rule keys on "the parser would not read this back as one
    // atom", and reserved characters are only a proxy for that. `*`, `<`,
    // `>` and the arrows have dedicated rules in the grammar, so quoting
    // them is not merely noisy -- the term-island parser of math-syntax
    // rejects `$( x "*" x )`, which quietly broke every mathematical
    // rendering the moment the proxy was taken literally.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a * b");
        collector.clear();
        interactive.process("a * X");
        CHECK(answers_contain(collector, "a * b"));
        CHECK_FALSE(any_output_contains(collector, "\"*\""));

        interactive.process("c => d");
        collector.clear();
        interactive.process("c => X");
        CHECK(answers_contain(collector, "c => d"));
        CHECK_FALSE(any_output_contains(collector, "\"=>\"")); });
}

namespace
{
    // Run a script in a network of its own and return its answers, sorted.
    // The round-trip test below needs two INDEPENDENT networks, which is
    // also why it does not go through run_both_modes: what a name prints as
    // does not depend on the evaluation strategy.
    std::vector<std::string> answers_of(const std::string& script)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        collector.clear();
        process_lines(interactive, script);

        std::vector<std::string> answers = collect_answers(collector);
        std::sort(answers.begin(), answers.end());
        answers.erase(std::unique(answers.begin(), answers.end()), answers.end());
        return answers;
    }

    // Names covering the PEG's reserved characters and the tokens it reads
    // by their first character, plus the shapes whose RENDERING is composed
    // rather than named: a sequence element, a self-fact, and a fact used as
    // subject and as object.
    const std::string round_trip_network = R"zph(.deductions off
p1 rel b1
.name b1 "Mercury (planet)"
p2 rel b2
.name b2 "_foo"
p3 rel b3
.name b3 "A"
p4 rel b4
.name b4 "&12"
p5 rel b5
.name b5 "«Le Monde»"
p6 rel b6
.name b6 "x>y"
p7 rel b7
.name b7 "[a]"
p8 rel b8
.name b8 "a,b"
p9 rel b9
.name b9 "≈net"
p10 rel b10
.name b10 "*"
p11 rel b11
.name b11 ":foo"
p12 rel b12
.name b12 "¬x"
p13 rel b13
.name b13 "a b"
p14 rel b14
.name b14 "<odd>"
p15 rel b15
.name b15 "{s}"
p16 rel b16
.name b16 "The \"Big\" One"
p17 rel b17
.name b17 "C:\\Users\\x"
f maps <"a b" item2>
g maps < item2 >
h <item2 "a b"> k
narcissus friend narcissus
(alice friend bob) supports (4 + 5)
)zph";

    const std::string round_trip_queries = R"zph(.deductions off
S rel O
S maps O
S friend O
S supports O
S Q O
)zph";
}

TEST_CASE("display: every printed answer is re-enterable as input")
{
    // "Output should round-trip" is a design rule, and it has been probed
    // one name shape at a time -- each probe finding another shape that did
    // not. This checks the rule as a whole rather than case by case: print
    // a network whose names cover the reserved characters and the
    // first-character tokens, enter the printed lines into a FRESH network,
    // and ask the same questions again. Anything that does not read back as
    // what it was shows up as a missing or an extra answer.
    const std::vector<std::string> printed =
        answers_of(round_trip_network + round_trip_queries);
    REQUIRE(printed.size() >= 18);

    std::string re_entered = ".deductions off\n";
    for (const std::string& answer : printed)
        re_entered += answer + "\n";

    CHECK(answers_of(re_entered + round_trip_queries) == printed);
}

TEST_CASE("display: a fact used as a predicate keeps its structure")
{
    // A predicate that is itself a fact node is not a DECLARED relation
    // type, so z->parse_relation could not name it and the whole line came
    // out as "x ?? y" -- unusable as input, although the identical
    // statement parses and matches perfectly well. Subject and objects
    // already came from the recorded triple; only the predicate was still
    // being reconstructed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
x (a p b) y
)");
        CHECK(any_output_contains(collector, "x (a p b) y"));
        CHECK_FALSE(any_output_contains(collector, "??"));

        // The printed line is input again: re-entering it as a query must
        // find the very fact that was printed.
        collector.clear();
        interactive.process("X (a p b) Y");
        CHECK(answers_contain(collector, "x (a p b) y")); });
}

TEST_CASE("display: nested predicates render at every level")
{
    // Each level of this statement puts a fact in predicate position, so
    // the failure was cumulative: the inner levels collapsed to "??" while
    // subject and object of the outermost level still printed.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(deep_nesting ~ ( Level1 ( Level2 ( Level3 predicate "Level3Object" ) Level2Object) Level1Object))");
        CHECK(any_output_contains(collector, "Level3 predicate Level3Object"));
        CHECK(any_output_contains(collector, "Level2Object"));
        CHECK(any_output_contains(collector, "Level1Object"));
        CHECK_FALSE(any_output_contains(collector, "??")); });
}

TEST_CASE("display: a list in predicate position renders as a list")
{
    // "<=>" is read by the grammar as the list <=>, i.e. a cons cell, and a
    // cons cell is no more a declared relation type than a fact node is.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(a <=> b) is_type equivalence");
        CHECK(any_output_contains(collector, "<=>"));
        CHECK_FALSE(any_output_contains(collector, "??")); });
}

TEST_CASE("display: a list of several elements survives predicate position")
{
    // One element was not enough to see it. A cons cell in predicate position
    // is pointed at by the fact it is the predicate of AND by the `~ ->`
    // declaration that being a predicate creates, and the chain walk read
    // both as part of the cell -- so it left the chain somewhere other than
    // nil, called the list improper and printed the explicit cons form
    // WITHOUT the parentheses that form needs: `x (a cons b cons nil) y`.
    // Re-entering that line is an error ("facts with same relation type and
    // object are not supported"), so the printed line was not input.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("x <a b c> y");

        collector.clear();
        interactive.process("S Q O");
        CHECK(answers_contain(collector, "x <a b c> y"));

        // And it reads back as the very same fact.
        collector.clear();
        interactive.process("X <a b c> Y");
        CHECK(answers_contain(collector, "x <a b c> y")); });
}

TEST_CASE("display: a generated node prints the same way wherever it appears")
{
    // A fresh variable's witness is a node with no name and no structure.
    // It is also the SUBJECT of the fact that was deduced about it -- and
    // that link is symmetric, so asking the node for its own structure
    // hands back the containing fact. The deduction line was rendered with
    // that fact as `parent` and therefore said "??", while the answer to a
    // query had no such parent and built a triple out of it: "(?? ?? ??)".
    // Two renderings of one node, and the second one cannot be entered.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        interactive.process("(A is human) => (B nameof A)");
        collector.clear();
        interactive.process("tim is human");
        REQUIRE(any_output_contains(collector, "?? nameof tim"));

        collector.clear();
        interactive.process("X nameof tim");
        CHECK(answers_contain(collector, "?? nameof tim"));
        CHECK_FALSE(any_output_contains(collector, "?? ?? ??")); });
}

TEST_CASE("display: a cons cell reads the same whether some list is a predicate or not")
{
    // The entry test of the list detection asked parse_relation which
    // NEIGHBOUR of the node is a declared relation type. A cons cell whose
    // own list is used as a predicate somewhere has two -- `cons`, and the
    // list head, which being a predicate declared. parse_relation then
    // reported the ambiguity as "no relation", the cell was not recognised,
    // and the very same node printed one way in a graph where some list
    // happens to be a predicate and another way in a graph where none is.
    //
    // Nothing about the cell itself differs between the two networks below,
    // so the two renderings have to agree.
    std::string as_predicate;
    std::string as_object;

    run_both_modes([&as_predicate](auto& collector, auto& interactive)
                   {
        interactive.process("x <a b> y");
        collector.clear();
        interactive.process("S Q O");
        for (const std::string& a : collect_answers(collector))
            if (a.starts_with("a cons")) as_predicate = a; });

    run_both_modes([&as_object](auto& collector, auto& interactive)
                   {
        interactive.process("x p <a b>");
        collector.clear();
        interactive.process("S Q O");
        for (const std::string& a : collect_answers(collector))
            if (a.starts_with("a cons")) as_object = a; });

    REQUIRE_FALSE(as_object.empty());
    CHECK(as_predicate == as_object);
    CHECK(as_object == "a cons <b>");
}

TEST_CASE("display: the negation of a rule condition stays with the rule")
{
    // "¬" tags the pattern NODE, and a ground pattern is hash-consed, so a
    // pattern negated in one rule carries the tag wherever it occurs. The
    // renderer wrote the tag into the term unconditionally, which made an
    // ASSERTED fact print as its own negation:
    //
    //     .explain (a p b)  ->  ¬(a p b)  [axiom]
    //
    // -- neither true nor re-enterable, since a top-level "¬" is a different
    // statement whose meaning is undecided. The operator is written where it
    // is syntactically part of the statement (a rule's condition slot, a
    // member of its conjunction set) and reported as a PROPERTY elsewhere.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        interactive.process("¬(a p b) => (c q d)");

        // The rule keeps it -- in both shapes.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(¬(a p b)) => (c q d)"));

        interactive.process("(X p Y, ¬(X q Y)) => (X r Y)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "¬(X q Y)"));

        // The asserted fact does not.
        collector.clear();
        interactive.process("A p B");
        CHECK(answers_contain(collector, "a p b"));

        collector.clear();
        interactive.process(".node a p b");
        CHECK(any_output_contains(collector, "Representation: a p b"));
        CHECK_FALSE(any_output_contains(collector, "Representation: ¬"));
        CHECK(any_output_contains(collector, "Negated by a rule: yes"));

        collector.clear();
        interactive.process(".explain (a p b)");
        CHECK(any_output_contains(collector, "a p b  [axiom; negated by a rule]"));

        // A fact no rule negates says nothing about negation.
        interactive.process("m rel n");
        collector.clear();
        interactive.process(".node m rel n");
        CHECK_FALSE(any_output_contains(collector, "Negated by a rule"));

        // The premise a NAF condition failed to find is still printed
        // negated -- there it IS the statement being made.
        interactive.process("u p v");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process(".explain (u r v)");
        CHECK(any_output_contains(collector, "¬(u q v)")); });
}

TEST_CASE("display: a rule prints its conditions in the surface syntax")
{
    // The brace form is the TOPOLOGY, and re-entering it built something
    // else: "{A B}" reads as a set literal, a literal carrying variables
    // builds a COLLECTION, and a collection is not tagged `~ conjunction` --
    // so the printed form of the commonest rule shape there is came back
    // inert. It now prints as it is written.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("(X p Y, X q Y) => (X r Y)");
        CHECK(any_output_contains(collector, "((X p Y), (X q Y)) => (X r Y)"));
        CHECK_FALSE(any_output_contains(collector, "{(X p Y) (X q Y)}"));

        // The justification keeps the brace form: a premise set is not a
        // statement being made, and nothing re-enters it.
        collector.clear();
        interactive.process("m p n");
        interactive.process("m q n");
        CHECK(any_output_contains(collector, "⇐ {(m p n) (m q n)}"));

        // The printed rule is a fixpoint of the rendering AND derives.
        collector.clear();
        interactive.process("((X s Y), (X t Y)) => (X u Y)");
        CHECK(any_output_contains(collector, "((X s Y), (X t Y)) => (X u Y)"));

        interactive.process("m s n");
        interactive.process("m t n");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("M u N");
        CHECK(answers_contain(collector, "m u n"));

        // The verbose spelling stays valid input -- with the conjunction tag
        // it is the same rule, and it prints in the surface syntax too.
        // The member ORDER follows the node ids, so the assertion asks for
        // the syntax rather than for one of the two spellings.
        collector.clear();
        interactive.process("(*{(A v B) (B v C)} ~ conjunction) => (A v C)");
        CHECK(any_output_contains(collector, "), (A v B)) => (A v C)"));
        CHECK_FALSE(any_output_contains(collector, "{(A v B)"));

        // ... and with any OTHER predicate it still builds what it says: the
        // focus operator asserts the inner fact and the set stays the rule's
        // subject, which no longer reads as a conjunction and prints as the
        // container it is.
        collector.clear();
        interactive.process("(*{(A w B) (B w C)} mypred whatever) => (A w C)");
        CHECK(any_output_contains(collector, "@{"));

        collector.clear();
        interactive.process("S mypred O");
        CHECK(any_output_contains(collector, "mypred whatever"));

        // The justification keeps the brace form whichever strategy found the
        // deduction. The premise set is handed to the renderer WITH the rule
        // that fired as its parent, which from inside the renderer looks
        // exactly like the set standing inside that rule -- so a deduction
        // found by a seeded iteration printed the rule's own surface syntax
        // for a statement nobody is making. Only the seeded path keeps the
        // rule node as the parent; the classic pass descends into the
        // conjunction and loses it, which is why the two strategies disagreed
        // on this one line.
        //
        // The CONSUMING rule is defined first, so the classic first pass
        // cannot complete the chain and the delta path is the one that does
        // (see the note at the top of test_seminaive.cpp). The producing
        // fact is entered LAST for the same reason: auto-run means every
        // line is a run of its own, so with (d k e) first the derived
        // (d h e) is already there when (e i f) arrives and the classic
        // pass gets the deduction. The member order inside the braces
        // follows the node ids, so the assertion asks for the notation
        // rather than for one of the two spellings. It stands last in this
        // test case because it creates nodes, and the ids they take decide
        // that order for the checks above.
        collector.clear();
        interactive.process("(X h Y, Y i Z) => (X j Z)");
        interactive.process("(X k Y) => (X h Y)");
        interactive.process("e i f");
        interactive.process("d k e");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "(d j f) ⇐ {("));
        CHECK_FALSE(any_output_contains(collector, "(d j f) ⇐ ((")); });
}

TEST_CASE("display: the rule-pattern marking is never printed in place of its node")
{
    // Same ruling as for the negation tag above, reached from the other side.
    // The marking is a fact ABOUT a node, and the proxy path of the renderer
    // -- which shows an anonymous, structureless node as the concept it is an
    // instance of -- read it as such a concept. A rule then printed as
    //
    //     (a p b) => ("rule pattern")
    //
    // for a consequence that IS `c q d`: a line that denotes something else
    // than the node it came from, and one nobody can enter again.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Using the rule as a predicate declares it a relation type, so the
        // consequence node has TWO declared relation types among its
        // neighbours and parse_relation gives up -- which is what sent the
        // node down the proxy path. The recorded triple still answers.
        interactive.process("(a p b) => (c q d)");
        collector.clear();
        interactive.process("x ((a p b) => (c q d)) y");
        CHECK_FALSE(any_output_contains(collector, "\"rule pattern\""));

        // The marking is still REPORTED, beside the term rather than inside
        // it -- the property that made removing it from the term acceptable.
        collector.clear();
        interactive.process(".node c q d");
        CHECK(any_output_contains(collector, "Representation: c q d"));
        CHECK(any_output_contains(collector, "Rule pattern (not asserted): yes"));

        collector.clear();
        interactive.process(".explain (c q d)");
        CHECK(any_output_contains(collector, "[rule pattern; not asserted]"));

        // ... and it goes when the statement becomes a claim.
        interactive.process("c q d");
        collector.clear();
        interactive.process(".node c q d");
        CHECK_FALSE(any_output_contains(collector, "Rule pattern")); });
}

TEST_CASE("display: a predicate that lost its relation-type declaration stops being one")
{
    // `q ~ ->` is what marks q as a relation type, and fact-structure
    // reconstruction rejects every predicate absent from the memoized set. So
    // removing that one fact means the nodes built with q are no longer facts.
    //
    // This used to be true only after a RELOAD. The set is a cache that the
    // removal paths did not refresh, so the session went on reading q while a
    // reload of its own `.save` did not: `.list-rules` answered with the rule
    // before the round trip and "No rules found" after it -- the same network,
    // two answers. Which of the two is right is a question about meaning, and
    // the reading is that a node stops being a fact when its predicate stops
    // being a predicate. The session now says so immediately.
    //
    // What this must NOT do is substitute the rule-pattern marking for the
    // unreadable node -- the defect the neighbouring test pins. `??` is the
    // honest answer; `("rule pattern")` would be a different wrong one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_undeclared_roundtrip.bin";
        std::filesystem::remove(out);

        interactive.process("(a p b) => (c q d)");
        interactive.process(".prune-facts (q ~ ->)");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));
        CHECK_FALSE(any_output_contains(collector, "\"rule pattern\""));

        collector.clear();
        interactive.process(".node c q d");
        CHECK(any_output_contains(collector, "Representation: ??"));
        CHECK_FALSE(any_output_contains(collector, "Representation: c q d"));

        // The point of the whole thing: the session and a reload of its own
        // save now agree. They did not.
        interactive.process(".save " + out.string());
        interactive.process(".new");
        interactive.process(".load " + out.string());

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "No rules found"));

        std::filesystem::remove(out); });
}
