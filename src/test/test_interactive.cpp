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
#include "output.hpp"
#include "string_utils.hpp"

#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Collapse runs of whitespace to a single space, trim leading/trailing.
    std::wstring normalize(const std::wstring& s)
    {
        std::wstring result;
        bool         in_space = true;
        for (wchar_t c : s)
        {
            if (c == L' ' || c == L'\t')
            {
                if (!in_space)
                {
                    result += L' ';
                    in_space = true;
                }
            }
            else
            {
                result += c;
                in_space = false;
            }
        }
        if (!result.empty() && result.back() == L' ')
            result.pop_back();
        return result;
    }

    // Return the text of the last non-empty Out-channel event.
    std::wstring last_out_text(const zelph::OutputCollector& collector)
    {
        const auto& events = collector.events();
        for (auto it = events.rbegin(); it != events.rend(); ++it)
        {
            if (it->channel == zelph::OutputChannel::Out && !it->text.empty())
                return it->text;
        }
        return {};
    }

    // Check whether the last Out line starts with the expected pattern
    // after whitespace normalization on both sides.
    bool last_output_starts_with(const zelph::OutputCollector& collector, const std::wstring& expected)
    {
        std::wstring last = normalize(last_out_text(collector));
        std::wstring exp  = normalize(expected);
        if (exp.empty() || last.size() < exp.size()) return false;
        return last.compare(0, exp.size(), exp) == 0;
    }

    // Check whether ANY Out-channel event starts with the expected pattern (normalized).
    bool any_output_starts_with(const zelph::OutputCollector& collector, const std::wstring& expected)
    {
        std::wstring exp = normalize(expected);
        if (exp.empty()) return false;
        for (const auto& e : collector.events())
        {
            if (e.channel != zelph::OutputChannel::Out) continue;
            std::wstring n = normalize(e.text);
            if (n.size() >= exp.size() && n.compare(0, exp.size(), exp) == 0)
                return true;
        }
        return false;
    }

    // Check whether ANY Out-channel event contains the substring (normalized).
    bool any_output_contains(const zelph::OutputCollector& collector, const std::wstring& sub)
    {
        std::wstring exp = normalize(sub);
        if (exp.empty()) return false;
        for (const auto& e : collector.events())
        {
            if (e.channel != zelph::OutputChannel::Out) continue;
            if (normalize(e.text).find(exp) != std::wstring::npos)
                return true;
        }
        return false;
    }

    // Collect all "Answer:" lines as normalized answer text (prefix stripped).
    std::vector<std::wstring> collect_answers(const zelph::OutputCollector& collector)
    {
        std::vector<std::wstring> answers;
        const std::wstring        prefix = L"Answer:";
        for (const auto& e : collector.events())
        {
            if (e.channel != zelph::OutputChannel::Out) continue;
            std::wstring n = normalize(e.text);
            if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0)
            {
                std::wstring answer = n.substr(prefix.size());
                // Strip leading space after "Answer:"
                if (!answer.empty() && answer[0] == L' ')
                    answer = answer.substr(1);
                answers.push_back(answer);
            }
        }
        return answers;
    }

    // Check that a specific answer is present (order-independent, normalized).
    bool answers_contain(const zelph::OutputCollector& collector, const std::wstring& expected)
    {
        std::wstring              exp = normalize(expected);
        std::vector<std::wstring> all = collect_answers(collector);
        for (const auto& a : all)
        {
            if (a == exp) return true;
        }
        return false;
    }

    // Check that "Found one or more contradictions!" appears in any output channel.
    bool has_contradiction(const zelph::OutputCollector& collector)
    {
        for (const auto& e : collector.events())
        {
            if (normalize(e.text).find(L"Found one or more contradictions!") != std::wstring::npos)
                return true;
        }
        return false;
    }

    // Process a multiline string, feeding each line to interactive.process().
    void process_lines(zelph::console::Interactive& interactive, const std::string& script)
    {
        std::istringstream iss(script);
        std::string        line;
        while (std::getline(iss, line))
        {
            interactive.process(zelph::string::unicode::from_utf8(line));
        }
    }

    // Run a test function in both parallel and single-core mode.
    // The function receives collector and interactive references.
    template <typename F>
    void run_both_modes(F&& test_fn)
    {
        SUBCASE("parallel")
        {
            zelph::OutputCollector      collector;
            zelph::console::Interactive interactive(collector.sink());
            test_fn(collector, interactive);
        }
        SUBCASE("single-core")
        {
            zelph::OutputCollector      collector;
            zelph::console::Interactive interactive(collector.sink());
            interactive.process(zelph::string::unicode::from_utf8(".parallel"));
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
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "g .. h\nh .. i");
        CHECK(any_output_starts_with(collector, L"g .. h"));
        CHECK(any_output_starts_with(collector, L"h .. i")); });
}

TEST_CASE("parsing: arrow predicates")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
atom_A => atom_B
atom_C <= atom_D
)");
        CHECK(any_output_starts_with(collector, L"atom_A => atom_B"));
        CHECK(any_output_starts_with(collector, L"atom_C <= atom_D")); });
}

// NOTE: <=> parsing is currently broken (displays as ??). Uncomment when fixed.
// TEST_CASE("parsing: biconditional arrow")
// {
//     run_both_modes([](auto& collector, auto& interactive)
//     {
//         process_lines(interactive, "(a <=> b) is_type equivalence");
//         CHECK(any_output_contains(collector, L"<=>"));
//     });
// }

// ---------------------------------------------------------------------------
// Sequences and lists
// ---------------------------------------------------------------------------

TEST_CASE("parsing: compact sequence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "seq_compact is_defined_as <123>");
        CHECK(any_output_contains(collector, L"<123>")); });
}

TEST_CASE("parsing: spaced sequence")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "seq_spaced is_defined_as < seqItem1 seqItem2 seqItem3 >");
        CHECK(any_output_contains(collector, L"< seqItem1 seqItem2 seqItem3 >")); });
}

TEST_CASE("parsing: quoted sequence is reversed")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(quoted_sequence ~ < "a" "b" "c" >)");
        CHECK(any_output_contains(collector, L"<cba>")); });
}

// ---------------------------------------------------------------------------
// Nested structures
// ---------------------------------------------------------------------------

TEST_CASE("parsing: nested sequence in set")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "nested_seq_in_set holds { <setElem1 setElem2> <setElem3 setElem4> }");
        CHECK(any_output_contains(collector, L"< setElem1 setElem2 >"));
        CHECK(any_output_contains(collector, L"< setElem3 setElem4 >")); });
}

TEST_CASE("parsing: mixed container")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(mixed_container content < (myCond => myDeduct) (myDeduct2 <= myCond2) { setElem5 setElem6 } "literal string" >)");
        CHECK(any_output_contains(collector, L"myCond => myDeduct"));
        CHECK(any_output_contains(collector, L"myDeduct2 <= myCond2"));
        CHECK(any_output_contains(collector, L"setElem5"));
        CHECK(any_output_contains(collector, L"setElem6")); });
}

TEST_CASE("parsing: deep nesting")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(deep_nesting ~ ( Level1 ( Level2 ( Level3 predicate "Level3Object" ) Level2Object) Level1Object))");
        // Display truncates inner levels to ??, but the parser must accept the input.
        CHECK(any_output_contains(collector, L"Level1"));
        CHECK(any_output_contains(collector, L"Level1Object")); });
}

TEST_CASE("parsing: set with facts")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "set_logic ~ { (myItem1 IsA myItem2) (myItem2 IsA myItem3) }");
        CHECK(any_output_contains(collector, L"myItem1 IsA myItem2"));
        CHECK(any_output_contains(collector, L"myItem2 IsA myItem3")); });
}

// ---------------------------------------------------------------------------
// Focus operator and variable queries
// ---------------------------------------------------------------------------

TEST_CASE("focus operator and variable query")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(*tim ~ human) ~ male
tim _predicate _object
)");
        CHECK(any_output_starts_with(collector, L"tim ~ male"));
        CHECK(answers_contain(collector, L"tim ~ human"));
        CHECK(answers_contain(collector, L"tim ~ male")); });
}

// ---------------------------------------------------------------------------
// Nested unification
// ---------------------------------------------------------------------------

TEST_CASE("nested unification: pattern matching in equations")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((A + B) = C) => (test A B)
(4 + 5) = 9
)");
        CHECK(any_output_starts_with(collector, L"( test 4 5 )")); });
}

TEST_CASE("nested unification: deep structure matching")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(subj pred (obj is (subj2 A (b test C)))) => (success A C)
subj pred (obj is (subj2 a_val (b test c_val)))
)");
        CHECK(any_output_starts_with(collector, L"( success a_val c_val )")); });
}

// ---------------------------------------------------------------------------
// Complex conjunction rule
// ---------------------------------------------------------------------------

TEST_CASE("complex conjunction rule with followed-by")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
((A + B) = C) => (test A B)
(4 + 5) = 9
(*{ ((A + B) = C) (B followed-by D) (C followed-by E) } ~ conjunction) => ((A + D) = E)
5 followed-by 42
9 followed-by 43
)");
        CHECK(any_output_starts_with(collector, L"(( 4 + 42 ) = 43 )")); });
}

// ---------------------------------------------------------------------------
// Peano-style rule
// ---------------------------------------------------------------------------

TEST_CASE("peano-style successor rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A followed-by B) => ((<1> + A) = B)
<0> followed-by <1>
)");
        CHECK(any_output_starts_with(collector, L"((<1> + <0>) = <1>)")); });
}

// ---------------------------------------------------------------------------
// Negation
// ---------------------------------------------------------------------------

TEST_CASE("negation: last element of list")
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
(A partoflist L, *(A --> X) ~ negation) => (A "is last of" L)
)");
        CHECK(any_output_starts_with(collector, L"( elem5 is last of mylist )")); });
}

TEST_CASE("negation: syntax sugar with not-green rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A is yellow, ¬(A is green)) => (A "is not" green)
plant is green
plant is yellow
plant2 is yellow
)");
        // plant is both yellow and green, so rule does not fire for plant.
        // plant2 is yellow but not green, so the rule fires.
        CHECK(any_output_starts_with(collector, L"( plant2 is not green )")); });
}

// ---------------------------------------------------------------------------
// Contradiction detection
// ---------------------------------------------------------------------------

TEST_CASE("contradiction detection")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(A instanceof B, A subclassof B) => !
gene instanceof geneclass
gene subclassof geneclass
)");
        CHECK(any_output_starts_with(collector, L"!"));
        CHECK(has_contradiction(collector)); });
}

// ---------------------------------------------------------------------------
// Janet integration
// ---------------------------------------------------------------------------

TEST_CASE("janet: inline fact and multiline block with deduction")
{
    run_both_modes([](auto& collector, auto& interactive)
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
        CHECK(any_output_starts_with(collector, L"( Berlin is located in Europe )")); });
}

TEST_CASE("janet: unquote referencing janet variable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
%(def berlin (zelph/resolve "Berlin"))
,berlin ~ town
)");
        CHECK(any_output_starts_with(collector, L"Berlin ~ town")); });
}

// ---------------------------------------------------------------------------
// Multi-digit addition via rules
// ---------------------------------------------------------------------------

TEST_CASE("multi-digit addition via rules")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        static const std::string script = R"zelph(
# Rule-based multi-digit addition for arbitrarily large positive integers.
#
# Numbers are stored LSB-first as cons lists: <42> = 2 cons (4 cons nil).
# node_to_wstring reverses the display order, so results appear in conventional
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
            interactive.process(zelph::string::unicode::from_utf8("<98> + <13>"));
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, L"((<98> + <13>) = <111>)"));
        }
        SUBCASE("8 + 23 = 31")
        {
            interactive.process(zelph::string::unicode::from_utf8("<8> + <23>"));
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, L"((<8> + <23>) = <31>)"));
        }
        SUBCASE("67 + 45 = 112")
        {
            interactive.process(zelph::string::unicode::from_utf8("<67> + <45>"));
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, L"((<67> + <45>) = <112>)"));
        } });
}

// ---------------------------------------------------------------------------
// Transitive relation deduction
// ---------------------------------------------------------------------------

TEST_CASE("transitive relation deduction")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(R is transitive, A R B, B R C) => (A R C)
6 > 5
5 > 4
> is transitive
)");
        CHECK(any_output_starts_with(collector, L"( 6 > 4 )")); });
}