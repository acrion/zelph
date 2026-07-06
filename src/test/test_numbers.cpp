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
