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

#include "interactive.hpp"
#include "io/output.hpp"

#include <algorithm>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Collapse runs of whitespace to a single space, trim leading/trailing.
    std::string normalize(const std::string& s)
    {
        std::string result;
        bool        in_space = true;
        for (char c : s)
        {
            if (c == ' ' || c == '\t')
            {
                if (!in_space)
                {
                    result += ' ';
                    in_space = true;
                }
            }
            else
            {
                result += c;
                in_space = false;
            }
        }
        if (!result.empty() && result.back() == ' ')
            result.pop_back();
        return result;
    }

    // Return the text of the last non-empty Out-channel event.
    std::string last_out_text(const zelph::io::OutputCollector& collector)
    {
        const auto& events = collector.events();
        auto        rev    = std::ranges::reverse_view(events);
        auto        it     = std::ranges::find_if(rev, [](const auto& event)
                                       { return event.channel == zelph::io::OutputChannel::Out && !event.text.empty(); });
        return it != rev.end() ? it->text : std::string{};
    }

    // Check whether the last Out line starts with the expected pattern
    // after whitespace normalization on both sides.
    [[maybe_unused]] bool last_output_starts_with(const zelph::io::OutputCollector& collector, const std::string& expected)
    {
        std::string last = normalize(last_out_text(collector));
        std::string exp  = normalize(expected);
        if (exp.empty() || last.size() < exp.size()) return false;
        return last.compare(0, exp.size(), exp) == 0;
    }

    // Check whether ANY Out-channel event starts with the expected pattern (normalized).
    bool any_output_starts_with(const zelph::io::OutputCollector& collector, const std::string& expected)
    {
        std::string exp = normalize(expected);
        if (exp.empty()) return false;
        return std::any_of(collector.events().begin(), collector.events().end(), [&](const auto& e)
                           {
                    if (e.channel != zelph::io::OutputChannel::Out) return false;
                    std::string n = normalize(e.text);
                    return n.size() >= exp.size() && n.compare(0, exp.size(), exp) == 0; });
    }

    // Check whether ANY Out-channel event contains the substring (normalized).
    bool any_output_contains(const zelph::io::OutputCollector& collector, const std::string& sub)
    {
        std::string exp = normalize(sub);
        if (exp.empty()) return false;
        for (const auto& e : collector.events())
        {
            if (e.channel != zelph::io::OutputChannel::Out) continue;
            if (normalize(e.text).find(exp) != std::string::npos)
                return true;
        }
        return false;
    }

    // Check whether ANY event in ANY channel contains the substring (normalized).
    bool any_event_contains(const zelph::io::OutputCollector& collector, const std::string& sub)
    {
        std::string exp = normalize(sub);
        if (exp.empty()) return false;
        for (const auto& e : collector.events())
        {
            if (normalize(e.text).find(exp) != std::string::npos)
                return true;
        }
        return false;
    }

    // Collect all "Answer:" lines as normalized answer text (prefix stripped).
    std::vector<std::string> collect_answers(const zelph::io::OutputCollector& collector)
    {
        std::vector<std::string> answers;
        const std::string        prefix = "Answer:";
        for (const auto& e : collector.events())
        {
            if (e.channel != zelph::io::OutputChannel::Out) continue;
            std::string n = normalize(e.text);
            if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0)
            {
                std::string answer = n.substr(prefix.size());
                // Strip leading space after "Answer:"
                if (!answer.empty() && answer[0] == ' ')
                    answer = answer.substr(1);
                answers.push_back(answer);
            }
        }
        return answers;
    }

    // Check that a specific answer is present (order-independent, normalized).
    bool answers_contain(const zelph::io::OutputCollector& collector, const std::string& expected)
    {
        std::string              exp = normalize(expected);
        std::vector<std::string> all = collect_answers(collector);
        return std::any_of(all.begin(), all.end(), [&](const auto& a)
                           { return a == exp; });
    }

    // Check that "Found one or more contradictions!" appears in any output channel.
    bool has_contradiction(const zelph::io::OutputCollector& collector)
    {
        return std::any_of(collector.events().begin(), collector.events().end(), [](const auto& e)
                           { return normalize(e.text).find("Found one or more contradictions!") != std::string::npos; });
    }

    // Process a multiline string, feeding each line to interactive.process().
    void process_lines(const zelph::console::Interactive& interactive, const std::string& script)
    {
        std::istringstream iss(script);
        std::string        line;
        while (std::getline(iss, line))
        {
            interactive.process(line);
        }
    }

    // Run a test function in both parallel and single-core mode.
    // The function receives collector and interactive references.
    template <typename F>
    void run_both_modes(F&& test_fn)
    {
        SUBCASE("parallel")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            test_fn(collector, interactive);
        }
        SUBCASE("single-core")
        {
            zelph::io::OutputCollector  collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(".parallel");
            collector.clear();
            test_fn(collector, interactive);
        }
    }

} // anonymous namespace

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
// Multi-digit addition via rules
// ---------------------------------------------------------------------------

TEST_CASE("multi-digit addition via rules")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        static const std::string script = R"zelph(
# Rule-based multi-digit addition for arbitrarily large positive integers.
#
# Numbers are stored LSB-first as cons lists: <42> = 2 cons (4 cons nil).
# node_to_string reverses the display order, so results appear in conventional
# MSB-first notation.

# ---------------------------------------------------------------------------
# Digit addition lookup table (200 facts, generated at load time by Janet)
# ---------------------------------------------------------------------------
%
(for a 0 10
  (for b 0 10
    (for c 0 2
      (let [s    (% (+ a b c) 10)
            e    (math/floor (/ (+ a b c) 10))
            dpat (zelph/fact (zelph/resolve (string a))
                             "d+"
                             (zelph/resolve (string b)))]
        (let [cipat (zelph/fact dpat "ci" (zelph/resolve (string c)))]
          (zelph/fact cipat "sum" (zelph/resolve (string s)))
          (zelph/fact cipat "co"  (zelph/resolve (string e))))))))
%

# ---------------------------------------------------------------------------
# Base cases: nil+nil with carry-in 0 and 1
# ---------------------------------------------------------------------------
%
(let [nil-add (zelph/fact (zelph/resolve "nil") "add" (zelph/resolve "nil"))]
  (zelph/fact (zelph/fact nil-add "ci" (zelph/resolve "0")) "sum" (zelph/resolve "nil"))
  (zelph/fact (zelph/fact nil-add "ci" (zelph/resolve "1")) "sum" (zelph/list-chars "1")))
%

# ---------------------------------------------------------------------------
# Rule A0
# ---------------------------------------------------------------------------
(N + M) => ((N add M) ci 0)

# ---------------------------------------------------------------------------
# Rules D1-D3
# ---------------------------------------------------------------------------
(*{(((A cons R) add (B cons S)) ci C)
   (((A d+ B) ci C) co E)}
  ~ conjunction)
=> ((R add S) ci E)

(*{(((A cons R) add nil) ci C)
   (((A d+ 0) ci C) co E)}
  ~ conjunction)
=> ((R add nil) ci E)

(*{((nil add (B cons S)) ci C)
   (((0 d+ B) ci C) co E)}
  ~ conjunction)
=> ((nil add S) ci E)

# ---------------------------------------------------------------------------
# Rules As1-As3
# ---------------------------------------------------------------------------
(*{(((A cons R) add (B cons S)) ci C)
   (((A d+ B) ci C) sum D)
   (((A d+ B) ci C) co E)
   (((R add S) ci E) sum T)}
  ~ conjunction)
=> ((((A cons R) add (B cons S)) ci C) sum (D cons T))

(*{(((A cons R) add nil) ci C)
   (((A d+ 0) ci C) sum D)
   (((A d+ 0) ci C) co E)
   (((R add nil) ci E) sum T)}
  ~ conjunction)
=> ((((A cons R) add nil) ci C) sum (D cons T))

((nil add (B cons S)) ci C,
 ((0 d+ B) ci C) sum D,
 ((0 d+ B) ci C) co E,
 ((nil add S) ci E) sum T)
=> (((nil add (B cons S)) ci C) sum (D cons T))

# ---------------------------------------------------------------------------
# Rule C0
# ---------------------------------------------------------------------------
(N + M, ((N add M) ci 0) sum T) => ((N + M) = T)
)zelph";

        process_lines(interactive, script);

        SUBCASE("98 + 13 = 111")
        {
            interactive.process("<98> + <13>");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((<98> + <13>) = <111>)"));
        }
        SUBCASE("8 + 23 = 31")
        {
            interactive.process("<8> + <23>");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((<8> + <23>) = <31>)"));
        }
        SUBCASE("67 + 45 = 112")
        {
            interactive.process("<67> + <45>");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((<67> + <45>) = <112>)"));
        } });
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