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

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// The property for which rule identity exists, across generated programs: when
// the identity takes a new rule for one currently in force and rolls it back,
// no outcome that rule would have derived is lost.
//
// A program here is positive (no negation; a guard `!=` only compares what is
// bound), so it does not derive fewer facts when it holds one additional line,
// unless a rule is rolled back due to being a duplicate of a rule that derives
// something different. For every rule line of a program, the program without
// that line must therefore derive no fact that the whole program does not. The
// facts compared are answers whose terms are named atoms: observer rules carry
// into such facts what a set of conditions in a term relates, which the
// identity of that set determines. A rule a statement types twice is a single
// rule, and the second statement's set of conditions differs from the first's
// only as a node that nothing but the rolled-back rule would have held, which
// no observer can distinguish.
//
// Each program holds one pair of rule lines of a known shape, a shape the
// identity once took for one rule wrongly or must take for one, in either
// sequence of input, across multiple arrangements of node ids, and with or
// without a contextual line. Every shape and order is run, not drawn: an order
// in which a shape reveals its flaw is definitely encountered. Each rule line
// is dropped in one layout for each shape and order, and only the lines of the
// pair and the contextual line in every other layout. What a check that keeps a
// rule twice costs, or the name of a set, constitutes no loss and remains
// invisible to this property; the tests in test_rule_identity.cpp uphold these.
// ---------------------------------------------------------------------------

namespace
{
    // A construction finds a set of conditions it reconstructs without
    // alteration via its members, whichever set of them it meets first, and the
    // order in which one appears moves with the node IDs, thus with a missing
    // line. Therefore, a set that a generator rebuilds shares its members
    // exclusively with the sets its shape makes alike on purpose, and with no
    // line of context.
    const char* const data = R"(k1 is on
k2 is on
u1 q k1
u2 q k2
u1 s v
(a r d) in sa
(b r e) in sa
sa ~ conjunction
(a r d) in sb
(b r e) in sb
sb ~ conjunction
sa is special
(a s b, c t d) => (e f g)
(a s b, c t d) => (e f h)
(C => (e f g)) => (C is gset)
(C => (e f h)) => (C is hset))";

    const char* const observers = R"((L noted X, L noted Y) => (X with Y)
(sa noted X) => (X under sa)
(sb noted X) => (X under sb)
(C noted X, C is gset) => (X hit g)
(C noted X, C is hset) => (X hit h)
(L noted X, M seen X, L != M) => (X distinct yes)
(L noted X, L seen X) => (X both yes)
((C => (e f k)) noted X, C is gset) => (X quoted g)
((C => (e f k)) noted X, C is hset) => (X quoted h)
((C => (e f k)) noted X, C is special) => (X quoted special)
(L got X, L is noted) => (X hit noted)
(S q L, X in L) => (S qin X))";

    const std::vector<std::string> queries = {"X with Y", "X under Y", "X hit Y", "S qin O", "X distinct Y", "X both Y", "X quoted Y"};

    struct Shape
    {
        std::string              name;
        std::vector<std::string> setup;
        std::string              a;
        std::string              b;
    };

    std::string janet_rule(const std::string& consequence)
    {
        return R"js(%(zelph/dedup-rule (fn [] (def s (zelph/collection (zelph/fact x "p" y) (zelph/fact y "p" x))) (zelph/fact s "~" "conjunction") (zelph/fact s "=>" )js"
             + consequence + ")))";
    }

    std::string janet_thunk(const std::string& body)
    {
        return R"js(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) )js" + body + "))";
    }

    std::vector<Shape> shapes()
    {
        const std::string              island = R"js(%(zelph/register-keyword "<<" ">>" (fn [text] (def s (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "r" "d"))) (zelph/fact s "~" "conjunction") (zelph/fact s "is" "noted") s) ) )js";
        const std::vector<std::string> shared = {
            R"js(%(def x (zelph/var "X")) )js",
            R"js(%(def y (zelph/var "Y")) )js",
            R"js(%(var c nil) )js",
            R"js(%(zelph/dedup-rule (fn [] (set c (zelph/collection x)) (def s (zelph/collection (zelph/fact x "p" y) (zelph/fact y "p" x))) (zelph/fact s "~" "conjunction") (zelph/fact s "=>" (zelph/fact x "r" c)))) )js",
            "a p b",
            "b p a"};
        const std::vector<std::string> outside = {
            "u1 q k",
            R"js(%(def f (zelph/collection (zelph/fact "a" "p" "b"))) )js",
            R"js(%(zelph/fact f "~" "conjunction") )js"};
        return {
            {"a literal typed twice, ground", {}, "(X q k1) => ((a p b, c r d) noted X)", "(X q k1) => ((a p b, c r d) noted X)"},
            {"a literal typed twice, with variables", {}, "(X q k1) => ((X p Y, X r Y) noted X)", "(X q k1) => ((X p Y, X r Y) noted X)"},
            {"two named sets", {}, "(X q k1) => (sa noted X)", "(X q k1) => (sb noted X)"},
            {"a literal and a named set", {}, "(X q k1) => ((a r d, b r e) noted X)", "(X q k1) => (sa noted X)"},
            {"a typed literal and a generator, with variables", {}, "(X q k1) => ((X p Y, X r Y) noted X)", "(K is on) => ((X q K) => ((X p Y, X r Y) noted X))"},
            {"a typed literal and a generator, ground", {}, "(X q k1) => ((a p b, c r d) noted X)", "(K is on) => ((X q K) => ((a p b, c r d) noted X))"},
            {"two rules over rules", {}, "(C => (e f g)) => ((X q k1) => (C noted X))", "(C => (e f h)) => ((X q k1) => (C noted X))"},
            {"a typed literal and a rule over rules", {}, "(X q k1) => ((a s b, c t d) noted X)", "(C => (e f g)) => ((X q k1) => (C noted X))"},
            {"two typed sets and a rule over rules over one", {}, "(X q k1) => ((a s b, c t d) noted X) ((a s b, c t d) seen X)", "(C => (e f g)) => ((X q k1) => (C noted X) (C seen X))"},
            {"two typed sets and a generator that rebuilds two into one", {}, "(X q k1) => ((X p k1, X r Y) noted X) ((X p k1, X r Y) seen X)", "(K is on) => ((X q K) => ((X p K, X r Y) noted X) ((X p K, X r Y) seen X))"},
            {"a generator whose two rules share a set it makes, and a typed twin", {}, "(X q k1) => ((X p k1, X r Y) noted X)", "(K is on) => ((X q K) => ((X p K, X r Y) noted X)) ((X q K) => ((X p K, X r Y) seen X))"},
            {"two quoted rules over named sets", {}, "(X q k1) => ((sa => (e f k)) noted X)", "(X q k1) => ((sb => (e f k)) noted X)"},
            {"two rules over rules in quoted rules", {}, "(C => (e f g)) => ((X q k1) => ((C => (e f k)) noted X))", "(C => (e f h)) => ((X q k1) => ((C => (e f k)) noted X))"},
            {"a focus beside the rule", {}, "(X q k1) => ((a p b, c r d) got X)", "(X q k1) => ((*(a p b, c r d) is noted) got X)"},
            {"an island with a fact", {island}, "(X q k1) => ((a p b, c r d) got X)", "(X q k1) => (<<s>> got X)"},
            {"one shared container", shared, janet_rule(R"js((zelph/fact x "q" c) )js"), janet_rule(R"js((zelph/fact y "q" c) )js")},
            {"a set made outside the rules and an own one", outside, janet_thunk(R"js((zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact f "noted" x) (zelph/fact f "seen" x)) )js"), janet_thunk(R"js((def s (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact s "~" "conjunction") (zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact s "noted" x) (zelph/fact f "seen" x)) )js")},
            {"a rule in a set constant under a condition, over two named sets", {"u1 has {(sb => (e f k))}", "u2 has {(sa => (e f k))}"}, "(X has {(sa => (e f k))}) => (X hit yes)", "(X has {(sb => (e f k))}) => (X hit yes)"},
            {"a set of one condition and that condition, in a rule a condition names", {"(a p b) => (e f u1)", R"js(%(let [c (zelph/collection (zelph/fact "a" "p" "b"))] (zelph/fact c "~" "conjunction") (zelph/fact c "=>" (zelph/fact "e" "f" "u2"))) )js"}, janet_thunk(R"js((def c (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact c "~" "conjunction") (zelph/fact (zelph/fact c "=>" (zelph/fact "e" "f" x)) "=>" (zelph/fact x "hit" "yes")) )js"), "((a p b) => (e f X)) => (X hit yes)"},
            {"a named set among the conditions of a rule a condition names", {"(a p b, sa) => (e f u1)", "(a p b, sb) => (e f u2)"}, "((a p b, sa) => (e f X)) => (X hit yes)", "((a p b, sb) => (e f X)) => (X hit yes)"},
            {"one own set and two", {"u1 q k"}, janet_thunk(R"js((def s (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact s "~" "conjunction") (zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact s "noted" x) (zelph/fact s "seen" x)) )js"), janet_thunk(R"js((def s1 (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact s1 "~" "conjunction") (def s2 (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact s2 "~" "conjunction") (zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact s1 "noted" x) (zelph/fact s2 "seen" x)) )js")},
        };
    }

    // Lines that neither shape requires, apart from the pair.
    const std::vector<std::string> context = {
        "(X q k2) => ((X p Y, X r Y) noted X)",
        "(X q k1) => (sa seen X)",
        "(X q k2) => ((m p n, o r t) seen X)",
        "(K is on) => ((X q K) => ((X p Y, X r Y) seen X))"};

    struct Line
    {
        bool        rule;
        bool        pair;
        std::string text;
    };

    // Whether a line states a rule: a typed line featuring `=>` outside every
    // bracket, or a Janet line constructing a `=>` fact. A typed fact that
    // encloses a rule within a set constant states none.
    bool states_rule(const std::string& text)
    {
        if (text.rfind("%", 0) == 0) return text.find("\"=>\"") != std::string::npos;
        int depth = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '(' || text[i] == '{')
                ++depth;
            else if (text[i] == ')' || text[i] == '}')
                --depth;
            else if (depth == 0 && text.compare(i, 2, "=>") == 0)
                return true;
        }
        return false;
    }

    struct Outcome
    {
        std::map<std::string, std::set<std::string>> answers;
        std::string                                  rules;
        bool                                         failed = false;
    };

    // The program excluding its line `drop` (none when `drop` is out of range).
    Outcome run_program(const std::vector<Line>& lines, const std::size_t drop)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        // Without the line that builds what a subsequent line depends on, the
        // later line fails, and the program proceeds without it.
        Outcome outcome;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i == drop) continue;
            try
            {
                interactive.process(lines[i].text);
            }
            catch (const std::exception&)
            {
                outcome.failed = true;
            }
        }
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".stat");
        for (const auto& event : collector.events())
            if (const std::string t = normalize(event.text); t.rfind("Rules:", 0) == 0) outcome.rules = t;
        for (const std::string& query : queries)
        {
            collector.clear();
            interactive.process(query);
            const std::vector<std::string> answers = collect_answers(collector);
            outcome.answers[query]                 = std::set<std::string>(answers.begin(), answers.end());
        }
        return outcome;
    }
}

TEST_CASE("rule identity: a rule line added to a positive program loses no fact over named atoms" * doctest::test_suite("slow"))
{
    const std::vector<Shape> all     = shapes();
    int                      dropped = 0;
    int                      kept    = 0;
    for (std::size_t shape = 0; shape < all.size(); ++shape)
    {
        for (const bool b_first : {false, true})
        {
            for (const int shift : {0, 5, 11})
            {
                for (const bool with_context : {false, true})
                {
                    std::vector<Line> lines;
                    const auto        add_block = [&](const std::string& block)
                    {
                        for (std::size_t from = 0, to = 0; from <= block.size(); from = to + 1)
                        {
                            to = block.find('\n', from);
                            if (to == std::string::npos) to = block.size();
                            const std::string line = block.substr(from, to - from);
                            lines.push_back({states_rule(line), false, line});
                        }
                    };
                    for (int i = 0; i < shift; ++i)
                        lines.push_back({false, false, "pad" + std::to_string(i) + " is pad"});
                    add_block(data);
                    for (const std::string& line : all[shape].setup)
                        lines.push_back({states_rule(line), false, line});
                    const std::string& first  = b_first ? all[shape].b : all[shape].a;
                    const std::string& second = b_first ? all[shape].a : all[shape].b;
                    if (with_context && shift % 2 == 0) lines.push_back({true, true, context[(shape + static_cast<std::size_t>(shift)) % context.size()]});
                    lines.push_back({true, true, first});
                    if (shift == 5) lines.push_back({false, false, ".run"});
                    lines.push_back({true, true, second});
                    if (with_context && shift % 2 != 0) lines.push_back({true, true, context[(shape + static_cast<std::size_t>(shift)) % context.size()]});
                    add_block(observers);

                    const bool    every_rule_line = shift == 0 && with_context;
                    const Outcome whole           = run_program(lines, lines.size());
                    REQUIRE(!whole.failed);
                    bool drop = false;
                    for (std::size_t j = 0; j < lines.size(); ++j)
                    {
                        if (!lines[j].rule || !(lines[j].pair || every_rule_line)) continue;
                        const Outcome less = run_program(lines, j);
                        if (less.rules == whole.rules) drop = true;
                        for (const std::string& query : queries)
                        {
                            std::vector<std::string> lost;
                            std::ranges::set_difference(less.answers.at(query), whole.answers.at(query), std::back_inserter(lost));
                            // doctest prints a `const char*` it receives as a numerical value.
                            const std::string label = "shape \"" + all[shape].name + "\", " + std::string(b_first ? "second" : "first")
                                                    + " line of the pair entered first, shift " + std::to_string(shift)
                                                    + std::string(with_context ? ", with context" : "") + ": without \"" + lines[j].text + "\", " + query;
                            INFO(label);
                            CHECK(lost.empty());
                        }
                    }
                    ++(drop ? dropped : kept);
                }
            }
        }
    }
    // The shapes attain both verdicts of the identity: a line that adopts one
    // rule currently in force, ensuring the number of rules remains unchanged
    // without it, and programs where every rule line contributes an additional
    // rule.
    CHECK(dropped > 0);
    CHECK(kept > 0);
}
