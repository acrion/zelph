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
// Multi-digit addition via rules
// ---------------------------------------------------------------------------

TEST_CASE("numbers: multi-digit addition via rules")
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
// Binary addition via rules -- no generated lookup table
// ---------------------------------------------------------------------------

TEST_CASE("numbers: binary addition via rules (full-adder axioms, no generated table)")
{
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        // The digit-level knowledge is the hand-written full-adder truth
        // table (16 facts). The recursion rules are identical to the base-10
        // test above -- they are base-agnostic. No Janet code is involved.
        static const std::string script = R"zelph(
((0 d+ 0) ci 0) sum 0
((0 d+ 0) ci 0) co 0
((1 d+ 0) ci 0) sum 1
((1 d+ 0) ci 0) co 0
((0 d+ 1) ci 0) sum 1
((0 d+ 1) ci 0) co 0
((1 d+ 1) ci 0) sum 0
((1 d+ 1) ci 0) co 1
((0 d+ 0) ci 1) sum 1
((0 d+ 0) ci 1) co 0
((1 d+ 0) ci 1) sum 0
((1 d+ 0) ci 1) co 1
((0 d+ 1) ci 1) sum 0
((0 d+ 1) ci 1) co 1
((1 d+ 1) ci 1) sum 1
((1 d+ 1) ci 1) co 1
((nil add nil) ci 0) sum nil
((nil add nil) ci 1) sum <1>
(N + M) => ((N add M) ci 0)
(((A cons R) add (B cons S)) ci C,
 ((A d+ B) ci C) co E)
=> ((R add S) ci E)
(((A cons R) add nil) ci C,
 ((A d+ 0) ci C) co E)
=> ((R add nil) ci E)
((nil add (B cons S)) ci C,
 ((0 d+ B) ci C) co E)
=> ((nil add S) ci E)
(((A cons R) add (B cons S)) ci C,
 ((A d+ B) ci C) sum D,
 ((A d+ B) ci C) co E,
 ((R add S) ci E) sum T)
=> ((((A cons R) add (B cons S)) ci C) sum (D cons T))
(((A cons R) add nil) ci C,
 ((A d+ 0) ci C) sum D,
 ((A d+ 0) ci C) co E,
 ((R add nil) ci E) sum T)
=> ((((A cons R) add nil) ci C) sum (D cons T))
((nil add (B cons S)) ci C,
 ((0 d+ B) ci C) sum D,
 ((0 d+ B) ci C) co E,
 ((nil add S) ci E) sum T)
=> (((nil add (B cons S)) ci C) sum (D cons T))
(N + M, ((N add M) ci 0) sum T) => ((N + M) = T)
)zelph";

        process_lines(interactive, script);

        SUBCASE("101 + 11 = 1000 (5 + 3 = 8)")
        {
            interactive.process("<101> + <11>");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((<101> + <11>) = <1000>)"));
        }
        SUBCASE("1111 + 1 = 10000 (carry chain through all positions)")
        {
            interactive.process("<1111> + <1>");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((<1111> + <1>) = <10000>)"));
        }
        SUBCASE("1010 + 110 = 10000 (10 + 6 = 16, mixed lengths)")
        {
            interactive.process("<1010> + <110>");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((<1010> + <110>) = <10000>)"));
        } });
}

// ---------------------------------------------------------------------------
// Number literals
// ---------------------------------------------------------------------------

TEST_CASE("number literals: $ delegates to redefinable zelph/number")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
                       // Without a loaded representation, $-literals fail loudly.
                       // excluded from test, because it prints errors on stdout which might be misleading (test succeeds)
                       // CHECK_THROWS(interactive.process("$5 result_of test0"));

                       // Decimal representation: identical to compact <...> syntax.
                       interactive.process("%(defn zelph/number [s] (zelph/list-chars s))");
                       collector.clear();
                       interactive.process("&42 result_of test1");
                       CHECK(any_output_starts_with(collector, "<42> result_of test1"));

                       // Binary representation: numeric variant suffices for test range.
                       interactive.process(R"(%(defn zelph/number [s] (var n (scan-number s)) (def bits @"") (while (> n 0) (buffer/push-string bits (string (% n 2))) (set n (math/floor (/ n 2)))) (zelph/list-chars (if (empty? bits) "0" (string/reverse (string bits))))))");
                       collector.clear();
                       interactive.process("&5 result_of test2");
                       CHECK(any_output_starts_with(collector, "<101> result_of test2"));

                       // Existing atoms starting with $ semantics: plain atom untouched?
                       // ($foo is not a number literal because zelph/number rejects it --
                       // it never reaches zelph/number: the atom rule only loses to the
                       // number rule, whose transformation calls zelph/number; so &foo
                       // errors under the decimal defn? -> covered manually if needed.)
                   });
}

// ---------------------------------------------------------------------------
// Number display: registered digit alphabet
// ---------------------------------------------------------------------------

TEST_CASE("number display: registered digits render lists as decimal &-literals")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Decimal representation: display is the identity conversion.
        interactive.process("%(defn zelph/number [s] (zelph/list-chars s))");
        interactive.process("%(zelph/set-number-digits (map string (range 10)))");
        collector.clear();
        interactive.process("&42 result_of testd1");
        CHECK(any_output_starts_with(collector, "&42 result_of testd1"));

        // Cons lists with unregistered elements keep the generic display.
        collector.clear();
        interactive.process("<ab> tagged testd2");
        CHECK(any_output_contains(collector, "<ab>"));

        // Binary representation: display converts back to decimal (&5 <-> <101>).
        interactive.process(R"(%(defn zelph/number [s] (var n (scan-number s)) (def bits @"") (while (> n 0) (buffer/push-string bits (string (% n 2))) (set n (math/floor (/ n 2)))) (zelph/list-chars (if (empty? bits) "0" (string/reverse (string bits))))))");
        interactive.process(R"(%(zelph/set-number-digits ["0" "1"]))");
        collector.clear();
        interactive.process("&5 result_of testd3");
        CHECK(any_output_starts_with(collector, "&5 result_of testd3"));

        // Empty array disables the feature -> generic display again.
        interactive.process("%(zelph/set-number-digits [])");
        collector.clear();
        interactive.process("&5 result_of testd4");
        CHECK(any_output_starts_with(collector, "<101> result_of testd4")); });
}

// ---------------------------------------------------------------------------
// Comparison via rules (stdlib scripts, base-agnostic)
// ---------------------------------------------------------------------------

TEST_CASE("numbers: comparison via rules (decimal)")
{
    run_both_modes([](auto& collector, const auto& interactive)
                   {
        interactive.process(".import arithmetic");

        SUBCASE("123 > 45 (longer canonical number wins)")
        {
            collector.clear();
            interactive.process("&123 cmp &45");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "(&123 > &45)"));
        }
        SUBCASE("45 < 123")
        {
            collector.clear();
            interactive.process("&45 cmp &123");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "(&45 < &123)"));
        }
        SUBCASE("equal length, most significant digit dominates: 39 < 41")
        {
            // LSB-first the first digit pair is 9 vs 1 (locally gt); the
            // result must still be lt because the inner rest dominates.
            collector.clear();
            interactive.process("&39 cmp &41");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "(&39 < &41)"));
        }
        SUBCASE("7 == 7 (both sides bind to the same hash-consed node)")
        {
            collector.clear();
            interactive.process("&7 cmp &7");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "(&7 == &7)"));
        } });
}

// ---------------------------------------------------------------------------
// Subtraction via rules
// ---------------------------------------------------------------------------

TEST_CASE("numbers: subtraction via rules (decimal)")
{
    run_both_modes([](auto& collector, const auto& interactive)
                   {
        interactive.process(".import arithmetic");

        SUBCASE("123456 - 54321 = 69135")
        {
            collector.clear();
            interactive.process("&123456 - &54321");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&123456 - &54321) = &69135)"));
        }
        SUBCASE("1000 - 1 = 999 (borrow chain through all positions)")
        {
            collector.clear();
            interactive.process("&1000 - &1");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&1000 - &1) = &999)"));
        }
        SUBCASE("105 - 98 = 7 (result list <007>, value normalized by &-display)")
        {
            collector.clear();
            interactive.process("&105 - &98");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&105 - &98) = &7)"));
        }
        SUBCASE("5 - 5 = 0")
        {
            collector.clear();
            interactive.process("&5 - &5");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&5 - &5) = &0)"));
        }
        SUBCASE("5 - 7 yields no result (partial function on naturals)")
        {
            // The borrow chain reaches ((nil sub nil) bi 1), for which no
            // base fact exists -- the derivation dies out silently.
            collector.clear();
            interactive.process("&5 - &7");
            interactive.run(true, false, false);
            CHECK_FALSE(any_output_contains(collector, "(&5 - &7) ="));
        } });
}

TEST_CASE("numbers: comparison and subtraction via rules (binary, identical rules)")
{
    // The recursion rules are byte-identical to the decimal script; only the
    // digit tables differ. Same decimal &-I/O on a different internal base.
    run_both_modes([](auto& collector, const auto& interactive)
                   {
        interactive.process(".import binary-arithmetic");

        SUBCASE("5 > 3")
        {
            collector.clear();
            interactive.process("&5 cmp &3");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "(&5 > &3)"));
        }
        SUBCASE("123456 - 54321 = 69135 (17-bit multi-borrow)")
        {
            collector.clear();
            interactive.process("&123456 - &54321");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&123456 - &54321) = &69135)"));
        } });
}

// ---------------------------------------------------------------------------
// Composability: computed facts are ordinary facts
// ---------------------------------------------------------------------------

TEST_CASE("numbers: computed comparison facts compose with meta-rules")
{
    // Comparison results are relational facts (N > M), so they feed the same
    // meta-rules as declared knowledge -- here the generic transitivity rule.
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        interactive.process(".import arithmetic");
        process_lines(interactive, R"(
(R is transitive, A R B, B R C) => (A R C)
> is transitive
&30 cmp &20
&20 cmp &10
)");
        CHECK(any_output_starts_with(collector, "(&30 > &20)"));
        CHECK(any_output_starts_with(collector, "(&20 > &10)"));
        CHECK(any_output_starts_with(collector, "(&30 > &10)")); });
}

TEST_CASE("numbers: derived results trigger further rule-based computations")
{
    // A user rule consumes subtraction results (= facts) and asserts new cmp
    // triggers: computations cascade across rule modules. This is exactly the
    // pattern multiplication will rely on (partial products feeding
    // additions), pinned down here as a regression test.
    run_both_modes([](const auto& collector, const auto& interactive)
                   {
        interactive.process(".import arithmetic");
        process_lines(interactive, R"(
((N - M) = T) => (T cmp M)
&10 - &3
)");
        CHECK(any_output_starts_with(collector, "((&10 - &3) = &7)"));
        CHECK(any_output_starts_with(collector, "(&7 > &3)")); });
}

// ---------------------------------------------------------------------------
// Result queries: the repeatable user-facing idiom
// ---------------------------------------------------------------------------

TEST_CASE("numbers: result query (A + B) = X is repeatable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import arithmetic");

        // First entry: parsing the query materializes the inner (+) fact as
        // a side effect; the query itself is evaluated BEFORE auto-run and
        // finds nothing yet -- the result appears as a deduction.
        collector.clear();
        interactive.process("(&12 + &34) = X");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "((&12 + &34) = &46)"));

        // Repetition: fixpoint reached, no deductions -- but the query now
        // finds the persisted = fact and answers, repeatably.
        collector.clear();
        interactive.process("(&12 + &34) = X");
        CHECK(answers_contain(collector, "(&12 + &34) = &46"));

        collector.clear();
        interactive.process("(&12 + &34) = X");
        CHECK(answers_contain(collector, "(&12 + &34) = &46")); });
}

TEST_CASE("numbers: result query (A cmp B) = X is repeatable (rule CC0)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import arithmetic");

        collector.clear();
        interactive.process("(&42 cmp &9) = X");
        interactive.run(true, false, false);
        CHECK(any_output_starts_with(collector, "(&42 > &9)")); // relational fact still primary

        collector.clear();
        interactive.process("(&42 cmp &9) = X");
        CHECK(answers_contain(collector, "(&42 cmp &9) = gt")); });
}

// ---------------------------------------------------------------------------
// Multiplication via rules
// ---------------------------------------------------------------------------

TEST_CASE("numbers: multiplication via rules (decimal)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import arithmetic");

        SUBCASE("12 * 34 = 408 (pins the *-as-atom parse and the full pipeline)")
        {
            collector.clear();
            interactive.process("&12 * &34");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&12 * &34) = &408)"));

            // Regression (template leak): no partially instantiated junk
            // results -- template subjects displayed as e.g. "< 2  A>", and
            // junk = facts carried wrong products like &708.
            CHECK_FALSE(any_output_contains(collector, "A>"));
            CHECK_FALSE(any_output_contains(collector, "=  &708"));
        }
        SUBCASE("9 * 9 = 81 (maximum digit product, PB2 carry path)")
        {
            collector.clear();
            interactive.process("&9 * &9");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&9 * &9) = &81)"));
        }
        SUBCASE("10 * 25 = 250 (zero digit in the first operand)")
        {
            collector.clear();
            interactive.process("&10 * &25");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&10 * &25) = &250)"));
        }
        SUBCASE("99 * 99 = 9801 (carry-heavy, running carry reaches 8)")
        {
            collector.clear();
            interactive.process("&99 * &99");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&99 * &99) = &9801)"));
        } });
}

TEST_CASE("numbers: multiplication via rules (binary, identical rules)")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import binary-arithmetic");

        SUBCASE("12 * 34 = 408")
        {
            collector.clear();
            interactive.process("&12 * &34");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&12 * &34) = &408)"));
        }
        SUBCASE("5 * 5 = 25")
        {
            collector.clear();
            interactive.process("&5 * &5");
            interactive.run(true, false, false);
            CHECK(any_output_starts_with(collector, "((&5 * &5) = &25)"));
        } });
}

TEST_CASE("numbers: result query (A * B) = X is repeatable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import arithmetic");

        collector.clear();
        interactive.process("(&6 * &7) = X");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "((&6 * &7) = &42)"));

        collector.clear();
        interactive.process("(&6 * &7) = X");
        CHECK(answers_contain(collector, "(&6 * &7) = &42"));

        collector.clear();
        interactive.process("(&6 * &7) = X");
        CHECK(answers_contain(collector, "(&6 * &7) = &42")); });
}

TEST_CASE("numbers: multiplication cascades into comparison")
{
    // Three modules chained: mul internally drives add (MA1/MA2), a user
    // rule consumes the = result and asserts a cmp trigger.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".import arithmetic");
        process_lines(interactive, R"(
((N * M) = T) => (T cmp N)
&6 * &7
)");
        CHECK(any_output_starts_with(collector, "((&6 * &7) = &42)"));
        CHECK(any_output_starts_with(collector, "(&42 > &6)")); });
}
