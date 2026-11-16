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
#include <utility>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// The property that rule identity serves, across generated programs: when the
// identity takes a new rule for one already in force and rolls the new rule
// back, no outcome that the new rule would have derived is lost.
//
// A program here is positive (no negation; a guard `!=` only compares what is
// bound), so it does not derive fewer facts when it holds one additional line,
// unless a rule is rolled back due to being a duplicate of a rule that derives
// something different. For every rule line of a program, the program without
// that line must therefore derive no fact that the whole program does not. The
// facts under comparison are answers over named atoms: observer rules transfer
// to such facts what a set of conditions in a term connects, determined by the
// identity of that set. A rule a statement types twice is a single rule, and
// the second statement's set of conditions differs from the first's only as a
// node that nothing but the rolled-back rule would have held, which no observer
// can distinguish.
//
// Beside its firing, a ground rule possesses three readers: a rule pattern that
// quotes it, a variable bound at the subject of a rule, and the membership of a
// member within a rule's set of conditions. Every program holds the last two
// among its observers, and a rule pattern for each typed rule line in the pair
// and for each rule its shape quotes (Shape::quotes); a shape whose two lines
// are Janet and that quotes none holds no rule pattern of the pair, and the
// rules of such a pair hold variables, which no rule pattern sees. A rule
// holding a variable is visible to none of the three, which is why a set
// consisting of one condition holding a variable can be read as that condition
// at a rule's own subject. The tag of such a set, when it bears one, answers a
// query, so a rule over the tag would lose what it derives from the tagged
// spelling entered after another (read_container); no program here holds a rule
// over the tag.
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

    // The last two read a ground rule as no firing does: a variable bound at
    // the subject of a rule binds the node present at that location, and the
    // membership of a ground member within a rule's set of conditions answers
    // as data.
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
(S q L, X in L) => (S qin X)
(S => C) => (S heads C)
(M in S, S => C) => (M opens C))";

    // `e f O` reads the output that the data block's two rules derive once a
    // firing's claim asserts their conditions: a typed list of those conditions
    // asserts them, while a rule written over the set of one of those rules
    // does not.
    const std::vector<std::string> queries = {"X with Y", "X under Y", "X hit Y", "S qin O", "X distinct Y", "X both Y", "X quoted Y", "S heads O", "S opens O", "e f O"};

    struct Shape
    {
        std::string              name;
        std::vector<std::string> setup;
        std::string              a;
        std::string              b;
        // Rule patterns that quote a rule which the pair brings into force,
        // without any typed line from the pair stating it: a rule a Janet
        // line builds, or a rule a generator writes.
        std::vector<std::string> quotes = {};
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

    // A rule that notes X regarding its own set of conditions, and a rule over
    // a second set of the same conditions that notes X concerning a third.
    std::string noting_its_own_set(const std::string& conditions)
    {
        return janet_thunk(R"js((def y (zelph/var "Y")) (def s (zelph/collection )js" + conditions
                           + R"js()) (zelph/fact s "~" "conjunction") (zelph/fact s "=>" (zelph/fact s "noted" x)) )js");
    }

    std::string noting_another_set(const std::string& conditions)
    {
        return janet_thunk(R"js((def y (zelph/var "Y")) (def d (zelph/collection )js" + conditions + R"js()) (zelph/fact d "~" "conjunction") (def e (zelph/collection )js"
                           + conditions + R"js()) (zelph/fact e "~" "conjunction") (zelph/fact d "=>" (zelph/fact e "noted" x)) )js");
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
        const std::vector<std::string> collections = {
            R"js(%(def c1 (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "q" "d"))) )js",
            R"js(%(def c2 (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "q" "d"))) )js"};
        const std::string two_conditions = R"js((zelph/fact x "p" y) (zelph/fact x "r" y))js";
        const std::string one_condition  = R"js((zelph/fact x "p" y))js";
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
            // The other readers, excluding the firing, distinguish the ground
            // rules of these pairs (within the last pair, the rules the
            // generators write). Read as the firing reads them, the second
            // rule entered was the first one's duplicate, and what a reader
            // derived from it was lost. The consequences remain separate from
            // the rules over rules within the data block.
            {"a named set among the rule's own conditions", {}, "(a p b, sa) => (e f u1)", "(a p b, sb) => (e f u1)"},
            {"a named set as the whole of the rule's conditions", {}, "sa => (e f u1)", "sb => (e f u1)"},
            {"a collection of one ground condition and that condition", {}, "@{(a p b)} => (e f u1)", "(a p b) => (e f u1)"},
            {"a generator over a set of one condition and over that condition", {"x p h", "g go h"}, R"js(%(let [g (zelph/var "G") h (zelph/var "H") condition (zelph/collection (zelph/fact "x" "p" h))] (zelph/fact condition "~" "conjunction") (zelph/dedup-rule (fn [] (zelph/fact (zelph/fact g "go" h) "=>" (zelph/fact condition "=>" (zelph/fact "y" "q" h)))))) )js", "(G go H) => ((x p H) => (y q H))", {"((*{(x p h)} ~ conjunction) => Q) => (Q quoted a)", "((x p h) => Q) => (Q quoted b)"}},
            // Only a program makes an untagged collection of several
            // conditions: a rule over one never fires, and a rule pattern meets
            // it by node. Read through their members, the two collections made
            // whichever rule was entered second a duplicate of the first.
            {"two untagged collections of the same members as the whole of the rule's conditions", collections, R"js(%(zelph/dedup-rule (fn [] (zelph/fact c1 "=>" (zelph/fact "e" "f" "u1")))) )js", R"js(%(zelph/dedup-rule (fn [] (zelph/fact c2 "=>" (zelph/fact "e" "f" "u1")))) )js", {R"js(%(zelph/dedup-rule (fn [] (def q (zelph/var "Q")) (zelph/fact (zelph/fact c1 "=>" q) "=>" (zelph/fact q "quoted" "a")))) )js", R"js(%(zelph/dedup-rule (fn [] (def q (zelph/var "Q")) (zelph/fact (zelph/fact c2 "=>" q) "=>" (zelph/fact q "quoted" "b")))) )js"}},
            // A rule a switch writes is considered a rule solely after a
            // construction has built it, and the construction reads its set of
            // one condition in the same way the firing does. Read as that
            // condition, the set made whichever switch was entered second a
            // duplicate of the first, while the switch over the set built a
            // rule that derived nothing. Now, each switch builds a rule that
            // derives alone, so these shapes lose nothing regardless of which
            // switch the identity keeps. They lose an answer solely when both
            // faults reappear: the identity takes one switch as the duplicate
            // of the other, and the rule constructed by the switch over the
            // set derives nothing.
            {"a switch over a set of one condition and over that condition", {"a p b"}, "(K is on) => ({(X p b)} => (b hit yes))", "(K is on) => ((X p b) => (b hit yes))"},
            {"a generator over a membership in a set of one condition and over that membership", {}, "(K is on) => ({(X in @{K z})} => (X hit K))", "(K is on) => ((X in @{K z}) => (X hit K))"},
            // One set at a rule's conditions and in its consequence, which only
            // a program makes. Compared outside the pairing of the sets, or read
            // as its single condition, it made the rule over two sets a
            // duplicate of the rule over one, whose firing writes about its own
            // set, which no observer meets.
            {"a set that is a rule's conditions and a term of its consequence, over two conditions", {"u p v", "u r v"}, noting_its_own_set(two_conditions), noting_another_set(two_conditions)},
            {"a set that is a rule's conditions and a term of its consequence, over one condition", {"u p v", "u r v"}, noting_its_own_set(one_condition), noting_another_set(one_condition)},
            // The member holds a variable, so only the firing reads these sets,
            // apart from the tag of the first shape's set, and it reads each as
            // its own member: the two lines constitute a single rule, which
            // loses nothing here, as no observer reads the tag.
            {"a set of one condition with a variable and that condition", {"a p b"}, "(*{(X p Y)} ~ conjunction) => (X hit Y)", "(X p Y) => (X hit Y)"},
            {"a set of one set of conditions and that set", {"a p b", "a r b"}, "{(X p Y, X r Y)} => (X hit Y)", "(X p Y, X r Y) => (X hit Y)"},
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

    // The location where a typed line features `=>` outside every bracket, or
    // npos.
    std::size_t top_level_arrow(const std::string& text)
    {
        int depth = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '(' || text[i] == '{')
                ++depth;
            else if (text[i] == ')' || text[i] == '}')
                --depth;
            else if (depth == 0 && text.compare(i, 2, "=>") == 0)
                return i;
        }
        return std::string::npos;
    }

    // Whether a line states a rule: a typed line featuring `=>` outside every
    // bracket, or a Janet line constructing a `=>` fact. A typed fact that
    // encloses a rule within a set constant states none.
    bool states_rule(const std::string& text)
    {
        if (text.rfind('%', 0) == 0) return text.find("\"=>\"") != std::string::npos;
        return top_level_arrow(text) != std::string::npos;
    }

    // The rule patterns that quote the rules belonging to a shape's pair: for
    // every typed line `<subject> => <consequences>`,
    // `(<subject> => Q) => (Q quoted a)` (`b` for the second line), and the
    // shape's own. Q is a variable that no subject of a pair holds.
    std::vector<std::string> quotes(const Shape& shape)
    {
        std::vector<std::string> result;
        for (const auto& [line, label] : {std::pair{&shape.a, "a"}, std::pair{&shape.b, "b"}})
        {
            const std::size_t arrow = line->rfind("%", 0) == 0 ? std::string::npos : top_level_arrow(*line);
            if (arrow == std::string::npos) continue;
            std::string subject = line->substr(0, arrow);
            while (subject.ends_with(' '))
                subject.pop_back();
            result.push_back("(" + subject + " => Q) => (Q quoted " + label + ")");
        }
        result.insert(result.end(), shape.quotes.begin(), shape.quotes.end());
        return result;
    }

    // A set prints its members following the order of their node ids, and a
    // line removed before a member was created shifts that id. The members of
    // every set an answer prints are arranged in sorted order, so that the set
    // compares by them.
    std::string sorted_sets(const std::string& text)
    {
        std::string result;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            result += text[i];
            if (text[i] != '{') continue;
            std::vector<std::string> members(1);
            for (int depth = 0; ++i < text.size() && (depth > 0 || text[i] != '}');)
            {
                if (text[i] == '(' || text[i] == '{')
                    ++depth;
                else if (text[i] == ')' || text[i] == '}')
                    --depth;
                if (depth == 0 && text[i] == ' ')
                    members.emplace_back();
                else
                    members.back() += text[i];
            }
            for (std::string& member : members)
                member = sorted_sets(member);
            std::ranges::sort(members);
            for (std::size_t m = 0; m < members.size(); ++m)
                result += (m == 0 ? "" : " ") + members[m];
            if (i < text.size()) result += '}';
        }
        return result;
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
            std::set<std::string>& answers = outcome.answers[query];
            for (const std::string& answer : collect_answers(collector))
                answers.insert(sorted_sets(answer));
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
                    for (const std::string& quote : quotes(all[shape]))
                        lines.push_back({true, false, quote});

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
