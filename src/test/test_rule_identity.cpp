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
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace zelph::test;

// ---------------------------------------------------------------------------
// Rule identity: a rule statement that says nothing new is skipped.
//
// Facts hash-cons, so asserting one twice is free. Rules did not: they carry
// variables, variables are fresh per statement, and a node built from fresh
// variables is a fresh node. The second occurrence of a rule was therefore a
// second rule -- same consequences, twice the unification work. That bites
// hardest where it is least expected: .load restores the rules but not the
// Janet side of a module, so `.load x.bin` + `.import <same module>` (the
// documented way to get &-literals back) used to DOUBLE the entire rule set.
//
// The tests below pin both directions. Missing a duplicate only costs what
// the old behaviour cost; treating two DIFFERENT rules as one would silently
// delete knowledge, so the negative cases carry the weight here.
// ---------------------------------------------------------------------------

namespace
{
    // The "Nodes: N" line of .stat -- the cheapest way to see whether a
    // statement left anything in the graph.
    std::string node_count(const zelph::io::OutputCollector& collector)
    {
        for (const auto& e : collector.events())
        {
            const std::string t = normalize(e.text);
            if (t.rfind("Nodes:", 0) == 0) return t;
        }
        return {};
    }

    // How many rules ".list-rules" just listed. Comparing counts rather
    // than rendered text keeps these tests independent of the identifier
    // markup, which the listing does not strip.
    std::size_t listed_rules(const zelph::io::OutputCollector& collector)
    {
        std::size_t n = 0;
        for (const auto& e : collector.events())
            if (normalize(e.text).find("=>") != std::string::npos) ++n;
        return n;
    }
}

TEST_CASE("rule identity: the same rule entered twice stays one rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);

        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);

        // ... and the surviving rule still works.
        interactive.process("rel is transitive");
        interactive.process("a rel b");
        interactive.process("b rel c");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("a rel X");
        CHECK(answers_contain(collector, "a rel c")); });
}

TEST_CASE("rule identity: a rule written with the focus operator, entered twice, stays one rule")
{
    // The focus makes a rule statement evaluate to one of the rule's parts, and
    // the parser used to pass precisely that part to dedup-rule as the value
    // the thunk returned: the duplicate check detected no rule to match, thus
    // every typing of the same rule introduced yet another. Each spelling
    // listed below is documented (logic.md lists the first beside the plain
    // rule).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto twice = [&](const std::string& first, const std::string& second)
        {
            interactive.process(first);
            interactive.process(second);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
        };

        SUBCASE("the focus on the condition set")
        {
            twice("*{(X p Y)} => (X q Y)", "*{(X p Y)} => (X q Y)");
        }
        SUBCASE("the focus on the condition")
        {
            twice("*(X p Y) => (X q Y)", "*(A p B) => (A q B)");
        }
        SUBCASE("the focus on a consequence")
        {
            twice("(X p Y) => *(X q Y)", "(X p Y) => *(X q Y)");
        }
        SUBCASE("the focused spelling after the plain one")
        {
            // One rule, as the set's sole condition holds a variable: no
            // reader other than the firing sees the set (read_container,
            // ReadAt::OwnSubject).
            twice("(X p Y) => (X q Y)", "*{(X p Y)} => (X q Y)");
        } });
}

TEST_CASE("rule identity: a rule with the focus on its arrow is the rule statement itself")
{
    // A focus on a rule's arrow names the rule itself -- the node that the
    // statement evaluates to without a focus. Since the parser's rule test
    // required the bare arrow, the expression `(X p Y) *=> (X q Y)` was
    // treated as a standard statement: `zelph/fact` built the `=>` fact
    // without performing a duplicate check, each typing added a new rule, and
    // the line evaluated to the arrow's name, which elicited no echo. The
    // grammar reads a focused arrow as the arrow itself (`zelph-arrow`).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        const auto node_id = [&]
        {
            collector.clear();
            interactive.process(".node");
            for (const auto& e : collector.events())
                if (const std::string t = normalize(e.text); t.rfind("Node ID:", 0) == 0) return t;
            return std::string{};
        };

        SUBCASE("typed twice under other names, one rule")
        {
            interactive.process("(X p Y) *=> (X q Y)");
            interactive.process("(A p B) *=> (A q B)");
            CHECK(rules() == 1);
        }
        SUBCASE("the focused arrow after the plain one, one rule")
        {
            interactive.process("(X p Y) => (X q Y)");
            interactive.process("(A p B) *=> (A q B)");
            CHECK(rules() == 1);
        }
        SUBCASE("the statement names the rule, typed once and again")
        {
            collector.clear();
            interactive.process("(X p Y) *=> (X q Y)");
            CHECK(normalize(last_out_text(collector)) == "(X p Y) => (X q Y)");
            const std::string first = node_id();
            REQUIRE_FALSE(first.empty());
            CHECK(any_output_contains(collector, "Representation: (X p Y) => (X q Y)"));

            // Under different names, it refers to the rule that
            // the graph keeps.
            collector.clear();
            interactive.process("(A p B) *=> (A q B)");
            CHECK(normalize(last_out_text(collector)) == "(X p Y) => (X q Y)");
            CHECK(node_id() == first);
        }
        SUBCASE("a ground rule names the rule, and stays one rule")
        {
            collector.clear();
            interactive.process("(a p b) *=> (c q d)");
            CHECK(normalize(last_out_text(collector)) == "(a p b) => (c q d)");

            interactive.process("(a p b) *=> (c q d)");
            CHECK(rules() == 1);
        }
        SUBCASE("beside a second focus, the arrow keeps its own")
        {
            // A single statement cannot contain two foci, and the
            // arrow is read as the rule exclusively when its focus
            // is the statement's sole focus.
            CHECK_THROWS_WITH_AS(interactive.process("(X p Y) *=> *(X q Y)"), doctest::Contains("Multiple focus markers"), std::runtime_error);
            CHECK(rules() == 0);
        }
        SUBCASE("as a command's pattern, it names the kept rule")
        {
            // A command's pattern traverses the identical grammar, thus the
            // focus on the arrow names the rule the graph keeps there as well.
            // Interpreted as a focus, the pattern denoted no node, and `.node`
            // refused it.
            interactive.process("(X p Y) => (X q Y)");

            collector.clear();
            interactive.process(".node (A p B) *=> (A q B)");
            CHECK(any_output_contains(collector, "Representation: (X p Y) => (X q Y)"));
            CHECK(rules() == 1);
        } });
}

// The arrow in parentheses or in quotation marks is the node `=>`, and the
// line represents the rule it encodes. Since the parser's rule test required
// the bare arrow, the expression with variables `(X p Y) (=>) (X q Y)` was
// interpreted as a query and did not yield a rule, whereas the ground form
// `(a p b) "=>" (c q d)` built the `=>` fact via `zelph/fact`, which claimed
// its parts. The grammar reads either spelling as the arrow (`zelph-arrow`),
// with or without a focus.
TEST_CASE("rule identity: an arrow in parentheses or in quotes is the arrow")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        SUBCASE("in parentheses typed twice under other names is one rule")
        {
            interactive.process("(X p Y) (=>) (X q Y)");
            interactive.process("(A p B) (=>) (A q B)");
            CHECK(rules() == 1);
        }
        SUBCASE("in quotes after the plain spelling the statement names the kept rule")
        {
            interactive.process("(X p Y) => (X q Y)");
            collector.clear();
            interactive.process("(A p B) \"=>\" (A q B)");
            CHECK(normalize(last_out_text(collector)) == "(X p Y) => (X q Y)");
            CHECK(rules() == 1);
        }
        SUBCASE("a ground rule in either spelling keeps its parts patterns and fires")
        {
            interactive.process("(a p b) (=>) (c q d)");
            interactive.process("(e r f) \"=>\" (g s h)");
            CHECK(rules() == 2);
            CHECK(answers("A p B").empty());
            CHECK(answers("C q D").empty());
            CHECK(answers("E r F").empty());
            CHECK(answers("G s H").empty());

            interactive.process("a p b");
            interactive.process("e r f");
            interactive.run(true, false, false);
            CHECK(answers("C q D") == std::vector<std::string>{"c q d"});
            CHECK(answers("G s H") == std::vector<std::string>{"g s h"});
        }
        SUBCASE("a focus over either spelling names the kept rule")
        {
            interactive.process("(X p Y) => (X q Y)");
            for (const std::string arrow : {"*\"=>\"", "*(\"=>\")", "(*=>)", "((=>))"})
            {
                collector.clear();
                interactive.process("(A p B) " + arrow + " (A q B)");
                CHECK(normalize(last_out_text(collector)) == "(X p Y) => (X q Y)");
            }
            CHECK(rules() == 1);
        }
        SUBCASE("inside a statement it mentions the rule")
        {
            interactive.process("x says ((a p b) (=>) (c q d))");
            CHECK(rules() == 0);
            CHECK(answers("A p B").empty());
            CHECK(answers("S says O").size() == 1);
        }
        // A rule that a set literal holds is a mention, just as one that a
        // statement holds is. With the arrow enclosed in quotation marks, it
        // was the `=>` fact constructed via `zelph/fact`, and its ground parts
        // were claimed.
        SUBCASE("inside a set the quoted arrow mentions the rule")
        {
            interactive.process("k has {((a p b) \"=>\" (c q d))}");
            CHECK(rules() == 0);
            CHECK(answers("A p B").empty());
            CHECK(answers("C q D").empty());
            CHECK(answers("S has O").size() == 1);
        }
        // A command reads its pattern using the identical grammar that a line
        // employs: by means of either spelling, `.explain` finds the rule that
        // the plain spelling names, where it reported that the fact was not
        // asserted.
        SUBCASE("a command pattern reads either spelling as the arrow")
        {
            interactive.process("(X p Y) => (X q Y)");
            const auto explained = [&](const std::string& arrow)
            {
                collector.clear();
                interactive.process(".explain (A p B) " + arrow + " (A q B)");
                std::string text;
                for (const auto& e : collector.events())
                    text += normalize(e.text) + "\n";
                return text;
            };
            const std::string plain = explained("=>");
            CHECK(plain.find("[axiom]") != std::string::npos);
            CHECK(explained("\"=>\"") == plain);
            CHECK(explained("(=>)") == plain);
        }
        SUBCASE("beside a focus on a consequence the focus stays where it is")
        {
            collector.clear();
            interactive.process("(X p Y) (=>) *(X q Y)");
            CHECK(normalize(last_out_text(collector)) == "X q Y");
            CHECK(rules() == 1);
            CHECK_THROWS_WITH_AS(interactive.process("(A p B) *\"=>\" *(A q B)"), doctest::Contains("Multiple focus markers"), std::runtime_error);
            CHECK(rules() == 1);
        } });
}

TEST_CASE("rule identity: a line that is one rule under a focus is the rule statement")
{
    // A focus on an entire line lacks a surrounding statement to hand a part
    // to, thus `*((X p Y) => (X q Y))` evaluates to the rule, just as the
    // plain spelling does. It was read as a value: the parser wrote the rule
    // down as a mention, which no entity named, hence the rule remained in
    // force without undergoing the duplicate check, and each typing added one.
    // The grammar reads the line as the rule statement (`zelph-line`).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };

        SUBCASE("typed twice under other names, one rule")
        {
            interactive.process("*((X p Y) => (X q Y))");
            interactive.process("*((A p B) => (A q B))");
            CHECK(rules() == 1);
        }
        SUBCASE("after the plain spelling, one rule, and the line names it")
        {
            interactive.process("(X p Y) => (X q Y)");

            collector.clear();
            interactive.process("*((A p B) => (A q B))");
            CHECK(normalize(last_out_text(collector)) == "(X p Y) => (X q Y)");
            CHECK(rules() == 1);
        }
        SUBCASE("as a command's pattern, it names the kept rule")
        {
            // A command's pattern traverses the identical grammar, thus
            // the line names the rule the graph keeps there as well.
            // Interpreted as a value, the pattern denoted no node, and
            // `.node` refused it.
            interactive.process("(X p Y) => (X q Y)");

            collector.clear();
            interactive.process(".node *((A p B) => (A q B))");
            CHECK(any_output_contains(collector, "Representation: (X p Y) => (X q Y)"));
            CHECK(rules() == 1);
        } });
}

TEST_CASE("rule identity: a rule statement asks nothing")
{
    // The variables within a rule are scoped in the same manner as those in a
    // query, and upon completion of the statement, the engine evaluated the
    // statement's value as if it were a query: the focused consequence answered
    // with the data that matched it, while the rule as a whole answered with
    // another rule that its own pattern matched. A rule statement states a
    // rule, regardless of its spelling.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a focused consequence typed again answers nothing")
        {
            interactive.process("a p b");
            interactive.process("(X p Y) => *(X q Y)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("(X p Y) => *(X q Y)");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a focused consequence answers nothing on its first typing either")
        {
            interactive.process("c q d");

            collector.clear();
            interactive.process("(X p Y) => *(X q Y)");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a plain rule answers nothing with another rule its pattern matches")
        {
            interactive.process("(a p b) => (c q d)");

            collector.clear();
            interactive.process("(X p b) => (c q d)");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        } });
}

TEST_CASE("rule identity: a rule with the focus operator evaluates to that part of the rule it keeps")
{
    // The statement continues to evaluate to the focused part: it is echoed,
    // and `.node` without an argument refers to it, just as it did before the
    // duplicate check saw the rule. When the rule repeats one that the graph
    // holds, the part is the kept rule's: the one the statement built is rolled
    // back.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto node_id = [&]
        {
            collector.clear();
            interactive.process(".node");
            for (const auto& e : collector.events())
                if (const std::string t = normalize(e.text); t.rfind("Node ID:", 0) == 0) return t;
            return std::string{};
        };

        SUBCASE("the condition set, typed once and again")
        {
            collector.clear();
            interactive.process("*{(X p Y)} => (X q Y)");
            CHECK(normalize(last_out_text(collector)) == "@{(X p Y)}");
            const std::string first = node_id();
            REQUIRE_FALSE(first.empty());
            CHECK(any_output_contains(collector, "Representation: @{(X p Y)}"));

            collector.clear();
            interactive.process("*{(X p Y)} => (X q Y)");
            CHECK(normalize(last_out_text(collector)) == "@{(X p Y)}");
            CHECK(node_id() == first);
        }
        SUBCASE("a consequence among several, typed again under other names")
        {
            interactive.process("(A p B) => *(B q A) (A q B)");

            // The consequence of the kept rule that pairs with the focused one,
            // displayed using the variable names defined by the kept rule.
            collector.clear();
            interactive.process("(X p Y) => (X q Y) *(Y q X)");
            CHECK(normalize(last_out_text(collector)) == "B q A");

            collector.clear();
            interactive.process("(X p Y) => *(X q Y) (Y q X)");
            CHECK(normalize(last_out_text(collector)) == "A q B");
        }
        SUBCASE("a Janet program asks for the part with :focus")
        {
            // The identical path the parser follows, crafted manually: the rule
            // repeats the typed one, thus the condition that comes back is the
            // typed rule's.
            interactive.process("(X p Y) => (X q Y)");
            CHECK(janet_value(collector, interactive, R"js((= (zelph/dedup-rule (fn [] (let [c (zelph/fact 'A "p" 'B)] [(zelph/fact c "=>" (zelph/fact 'A "q" 'B)) c])) :focus) (get (zelph/parse ((first (zelph/rules)) :rule)) 0)))js")
                  == "true");

            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
        } });
}

// The part of the kept rule is read from the pairings of the comparison that identified
// the rule, and a pairing that did not succeed is withdrawn from them. Left among them, a
// consequence initially attempted and subsequently rejected would come before the one
// that matched, causing the statement to evaluate to it: to `A q B` where the focus was
// on the twin of `B q A`. Which pairing a comparison attempts first is governed by node
// ids, so the rule is entered after twenty distinct numbers of unrelated facts; when this
// test was written, one of the two statements below came out wrong that way in twelve of
// them.
TEST_CASE("rule identity: the part of the kept rule is the consequence that matched, not one tried before it")
{
    for (int shift = 0; shift < 20; ++shift)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        for (int i = 0; i < shift; ++i)
            interactive.process("u" + std::to_string(i) + " v w" + std::to_string(i));
        interactive.process("(A p B) => *(B q A) (A q B)");

        INFO("shift " << shift);
        collector.clear();
        interactive.process("(X p Y) => (X q Y) *(Y q X)");
        CHECK(normalize(last_out_text(collector)) == "B q A");

        collector.clear();
        interactive.process("(X p Y) => *(X q Y) (Y q X)");
        CHECK(normalize(last_out_text(collector)) == "A q B");
    }
}

// The comparison that finds the kept rule pairs each consequence of the new
// rule with one of the kept rule's, while a set of conditions in a term is
// matched by its members exclusively when the new rule's statement made it. The
// kept part is taken from a second run of that comparison
// (network::corresponding_part), which therefore needs the same nodes of the
// new rule: a rule whose consequences note `X` about its own set and about a
// set made outside it pairs its own set solely with the kept rule's own set.
// Without these, the second comparison might match the new rule's own set with
// the externally created one, causing the focus to evaluate to the kept rule's
// other consequence. The set created externally constitutes the conditions of a
// rule of its own, so a firing writes it via its node, just as it writes a
// rule's own set (ReadAt::Written), and only the new rule's nodes tell the two
// apart; a list entered by a firing is not a term for a set it writes by its
// node. The order in which the comparison tries the consequences is determined
// by their node ids, hence the twenty layouts.
TEST_CASE("rule identity: the kept part pairs a set of conditions in a term as the comparison that found the rule did")
{
    for (int shift = 0; shift < 20; ++shift)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("%(for i 0 " + std::to_string(shift) + R"js( (zelph/var "D")) )js");
        process_lines(interactive, R"js(
%(def outside (zelph/collection (zelph/fact "a" "p" "b")))
%(zelph/fact outside "~" "conjunction")
%(zelph/fact outside "=>" (zelph/fact "c" "d" "e"))
%(var kept-own nil)
%(def kept (zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def own (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact own "~" "conjunction") (set kept-own (zelph/fact own "noted" x)) (zelph/fact (zelph/fact x "q" "k") "=>" kept-own (zelph/fact outside "noted" x)))))
)js");

        INFO("shift " << shift);
        CHECK(janet_value(collector, interactive, R"js((= kept-own (zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def own (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact own "~" "conjunction") (def mine (zelph/fact own "noted" x)) [(zelph/fact (zelph/fact x "q" "k") "=>" mine (zelph/fact outside "noted" x)) mine]) :focus)) )js")
              == "true");
        // The rule over the set made outside, and the rule that is kept.
        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 2"));
    }
}

TEST_CASE("rule identity: wherever the focus stands, a rule statement keeps all of its properties at once")
{
    // Each position a focus occupies within a rule statement held to four
    // properties together: the statement asks nothing, when re-typed (using
    // different names where variables appear) it remains a single rule and
    // evaluates to the same part of the kept rule, and no ground part of the
    // rule answers a query. The cases above take one property across several
    // positions each; the focus on the arrow failed all four, while a focus on
    // an entire line kept a rule for every typing, and neither position was
    // among them. A fact that a focus nests in a condition or consequence is
    // written beside the rule and claimed, hence those positions are taken
    // with variables only.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        using Ground = std::vector<std::pair<std::string, std::string>>; // a query and the part it must not answer
        const auto holds = [&](const std::string& first, const std::string& second, const Ground& ground)
        {
            // Each pattern listed below is matched by the data,
            // such that a statement evaluated as a query would
            // yield a response.
            interactive.process("m p n");
            interactive.process("m q n");

            collector.clear();
            interactive.process(first);
            CHECK(collect_answers(collector).empty());
            const std::string echo = normalize(last_out_text(collector));
            CHECK_FALSE(echo.empty());

            collector.clear();
            interactive.process(second);
            CHECK(collect_answers(collector).empty());
            CHECK(normalize(last_out_text(collector)) == echo);

            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);

            interactive.run(true, false, false);
            for (const auto& [query, part] : ground)
            {
                collector.clear();
                interactive.process(query);
                CHECK_FALSE(answers_contain(collector, part));
            }
        };
        const Ground abcd{{"A p B", "a p b"}, {"C q D", "c q d"}};

        SUBCASE("the condition, with variables")
        {
            holds("*(X p Y) => (X q Y)", "*(A p B) => (A q B)", {});
        }
        SUBCASE("the condition, ground")
        {
            holds("*(a p b) => (c q d)", "*(a p b) => (c q d)", abcd);
        }
        SUBCASE("the one-condition set, with variables")
        {
            holds("*{(X p Y)} => (X q Y)", "*{(A p B)} => (A q B)", {});
        }
        SUBCASE("the one-condition set, ground")
        {
            holds("*{(a p b)} => (c q d)", "*{(a p b)} => (c q d)", abcd);
        }
        SUBCASE("the comma list, with variables")
        {
            holds("*(X p Y, Y r Z) => (X q Z)", "*(A p B, B r C) => (A q C)", {});
        }
        SUBCASE("the comma list, ground")
        {
            holds("*(a p b, b r e) => (c q d)", "*(a p b, b r e) => (c q d)", {{"A p B", "a p b"}, {"B r E", "b r e"}, {"C q D", "c q d"}});
        }
        SUBCASE("a consequence, with variables")
        {
            holds("(X p Y) => *(X q Y)", "(A p B) => *(A q B)", {});
        }
        SUBCASE("a consequence, ground")
        {
            holds("(a p b) => *(c q d)", "(a p b) => *(c q d)", abcd);
        }
        SUBCASE("a consequence among several, typed in another order")
        {
            holds("(a p b) => *(c q d) (e r f)", "(a p b) => (e r f) *(c q d)", {{"A p B", "a p b"}, {"C q D", "c q d"}, {"E r F", "e r f"}});
        }
        SUBCASE("a fact nested in a condition")
        {
            holds("(x says *(X p Y)) => (X q Y)", "(x says *(A p B)) => (A q B)", {});
        }
        SUBCASE("a fact nested in a consequence")
        {
            holds("(X p Y) => (x says *(X q Y))", "(A p B) => (x says *(A q B))", {});
        }
        SUBCASE("the arrow, with variables")
        {
            holds("(X p Y) *=> (X q Y)", "(A p B) *=> (A q B)", {});
        }
        SUBCASE("the arrow, ground")
        {
            holds("(a p b) *=> (c q d)", "(a p b) *=> (c q d)", abcd);
        }
        SUBCASE("the arrow over a comma list with a ground condition")
        {
            holds("(X p Y, a r b) *=> (X q Y)", "(A p B, a r b) *=> (A q B)", {{"A r B", "a r b"}});
        }
        // A focus on a second focus, or on parentheses enclosing a solitary
        // value, is that single focus: `**x`, `*(x)` and `*((x))` are
        // interpreted as `*x`. The grammar took the arrow and the whole line
        // for the rule solely in the shape `*x`, so one additional layer
        // reintroduced the flaws mentioned earlier: a rule built without the
        // duplicate check, its ground parts claimed.
        SUBCASE("the arrow under a second focus, with variables")
        {
            holds("(X p Y) **=> (X q Y)", "(A p B) **=> (A q B)", {});
        }
        SUBCASE("the arrow in parentheses under the focus, ground")
        {
            holds("(a p b) *(=>) (c q d)", "(a p b) *(=>) (c q d)", abcd);
        }
        // The arrow enclosed in quotation marks beneath a focus, and a
        // focus located within the parentheses around the arrow, are
        // also the focused arrow.
        SUBCASE("the arrow in quotes under the focus with variables")
        {
            holds("(X p Y) *\"=>\" (X q Y)", "(A p B) *\"=>\" (A q B)", {});
        }
        SUBCASE("the focused arrow in parentheses on a ground rule")
        {
            holds("(a p b) (*=>) (c q d)", "(a p b) (*=>) (c q d)", abcd);
        }
        SUBCASE("the whole rule, with variables")
        {
            holds("*((X p Y) => (X q Y))", "*((A p B) => (A q B))", {});
        }
        SUBCASE("the whole rule, ground")
        {
            holds("*((a p b) => (c q d))", "*((a p b) => (c q d))", abcd);
        }
        // A focus on the whole line reads the arrow from the rule it holds,
        // and that arrow, whether enclosed in quotation marks or within
        // parentheses, remains the same arrow: the line functioned as a query
        // that left no rule behind.
        SUBCASE("the whole rule over the arrow in quotes and in parentheses")
        {
            holds("*((X p Y) \"=>\" (X q Y))", "*((A p B) (=>) (A q B))", {});
        }
        // The entire line, beneath the same layers.
        SUBCASE("the whole rule under a second focus, with variables")
        {
            holds("**((X p Y) => (X q Y))", "**((A p B) => (A q B))", {});
        }
        SUBCASE("the whole rule in a second pair of parentheses, with variables")
        {
            holds("*(((X p Y) => (X q Y)))", "*(((A p B) => (A q B)))", {});
        } });
}

TEST_CASE("rule identity: a focus on a consequence written as a name or a variable is a focus on that part")
{
    // A name or variable that is focused arrives at dedup-rule as the string or
    // symbol provided by the parser to zelph/fact, rather than as a node, and
    // the check that the part belongs to the rule refused it: `(X p Y) => *!`
    // was reported as "not a rule and one of its parts" and resulted in nothing
    // being built. Before the existence of this check, the line built its rule
    // without marking it, thus `(a p b) => *!` fired immediately.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto twice = [&](const std::string& first, const std::string& second)
        {
            CHECK_NOTHROW(interactive.process(first));
            CHECK_NOTHROW(interactive.process(second));
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
        };

        SUBCASE("the contradiction")
        {
            twice("(X p Y) => *!", "(A p B) => *!");

            collector.clear();
            interactive.process("a p b");
            CHECK(any_output_contains(collector, "! ⇐ (a p b)"));
        }
        SUBCASE("a name among several consequences")
        {
            twice("(X p Y) => (X q Y) *!", "(X p Y) => (X q Y) *!");
        }
        SUBCASE("a variable among several consequences, after the plain spelling")
        {
            // A rule requires just a single consequence it is able to assert
            // (Zelph::has_rule_form), hence the bare variable next to it does
            // not keep the line from qualifying as a rule. The focused spelling
            // must locate the rule that the plain spelling built.
            twice("(X p Y) => (X q Y) Y", "(X p Y) => (X q Y) *Y");
        }
        SUBCASE("a ground rule is a pattern until its condition is claimed")
        {
            interactive.process("(a p b) => *!");
            CHECK_FALSE(any_output_contains(collector, "! ⇐"));

            collector.clear();
            interactive.process("a p b");
            CHECK(any_output_contains(collector, "! ⇐ (a p b)"));
        } });
}

TEST_CASE("rule identity: dedup-rule refuses a thunk that does not return a rule")
{
    // The duplicate check compares the `=>` fact produced by the thunk and
    // finds no matching twin for any other item, hence the dedup-rule kept
    // whatever that thunk constructed: a plain fact serving as a claim, and
    // a rule whose thunk returned its condition set as a rule that had
    // never been checked for a duplicate. Both are rejected just like a
    // failing thunk, and none of the components they generated are
    // preserved.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto nodes = [&]
        {
            collector.clear();
            interactive.process(".stat");
            return node_count(collector);
        };
        const std::string before = nodes();

        SUBCASE("a plain fact")
        {
            CHECK_THROWS_WITH_AS(interactive.process(R"js(%(zelph/dedup-rule (fn [] (zelph/fact "a" "b" "c"))))js"),
                                 doctest::Contains("=>"),
                                 std::runtime_error);
            CHECK(nodes() == before);

            collector.clear();
            interactive.process("A b C");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a condition set")
        {
            CHECK_THROWS_WITH_AS(interactive.process(R"js(%(zelph/dedup-rule (fn [] (let [c (zelph/conjunction (zelph/fact 'X "p" 'Y) (zelph/fact 'Y "p" 'Z))] (zelph/fact c "=>" (zelph/fact 'X "q" 'Z)) c)))))js"),
                                 doctest::Contains("=>"),
                                 std::runtime_error);
            CHECK(nodes() == before);

            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 0);
        }
        SUBCASE("a typed rule with the focus operator is not refused")
        {
            // The focus operator makes the statement evaluate to its condition
            // set. Parsed statements were previously exempt from the refusal
            // for that reason. The parser's thunk now returns the `=>` fact
            // alongside the focused part (:focus), and the refusal applies to
            // every caller.
            interactive.process("*{(X p Y)} => (X q Y)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
        }
        SUBCASE("with :focus, a rule without a part of it")
        {
            // Using :focus, the thunk returns a tuple consisting of the rule
            // and the focused part. A rule by itself, or a part that belongs to
            // no rule the thunk authored, fails to indicate which part to
            // return.
            CHECK_THROWS_WITH_AS(interactive.process(R"js(%(zelph/dedup-rule (fn [] (zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X "q" 'Y))) :focus))js"),
                                 doctest::Contains("not a rule and one of its parts"),
                                 std::runtime_error);
            CHECK(nodes() == before);
        }
        SUBCASE("with :focus, a part the rule does not have")
        {
            CHECK_THROWS_WITH_AS(interactive.process(R"js(%(zelph/dedup-rule (fn [] [(zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X "q" 'Y)) (zelph/fact 'X "r" 'Y)]) :focus))js"),
                                 doctest::Contains("not a rule and one of its parts"),
                                 std::runtime_error);
            CHECK(nodes() == before);

            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 0);
        }
        SUBCASE("an option other than :focus")
        {
            CHECK_THROWS_WITH_AS(interactive.process(R"js(%(zelph/dedup-rule (fn [] (zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X "q" 'Y))) :part))js"),
                                 doctest::Contains("the only option is :focus"),
                                 std::runtime_error);
            CHECK(nodes() == before);
        } });
}

TEST_CASE("rule identity: renaming the variables does not make a new rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X p Y) => (X q Y)");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);

        interactive.process("(_alpha p _beta) => (_alpha q _beta)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1); });
}

// The pairing of a rule's conditions and the pairing of its variables
// constitute a single search, not two separate ones: in
// (A p B, B p A) => (A q B), the conditions match each other under A -> A and
// under A -> B, yet only the first holds for the conclusion as well. The
// conditions were paired first, and that pairing was preserved, meaning
// whether the conclusion matched depended on the order in which the set held
// its members -- on node ids. An equivalent rule was subsequently taken for a
// new rule, and a rule .explain was given as text for a missing rule,
// appearing on certain calls but not on others. The variants spell the rule
// using different variable names and present the conditions in both orders,
// thereby altering the ids.
TEST_CASE("rule identity: symmetric conditions are paired together with the conclusion")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A p B, B p A) => (A q B)");
        for (const auto& [x, y] : std::vector<std::pair<std::string, std::string>>{{"C", "D"}, {"X", "Y"}, {"U", "V"}, {"M", "N"}, {"P", "Q"}, {"K", "L"}, {"E", "F"}, {"G", "H"}})
        {
            interactive.process("(" + x + " p " + y + ", " + y + " p " + x + ") => (" + x + " q " + y + ")");
            interactive.process("(" + y + " p " + x + ", " + x + " p " + y + ") => (" + x + " q " + y + ")");
        }
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1); });
}

// A comparison counts the pairs it compares against a budget only after
// encountering a set that holds itself, and neither rule below possesses one.
// The eight conditions form pairs under each reassignment of the variables A
// through H, and the comparison tests these pairings until the conclusion
// fits as well; the number of pairs required depends on the order in which
// the set holds its conditions, here more than a hundred thousand. Seven
// hundred ground conditions take more than that in any order, as each is
// paired by trying the conditions of the other rule until the same one
// emerges. Counted starting from its first pair, the second rule was written
// beside the first.
TEST_CASE("rule identity: a rule over many conditions, entered twice, stays one rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("eight interchangeable conditions")
        {
            const std::string rule = "((X p A), (X p B), (X p C), (X p D), (X p E), (X p F), (X p G), (X p H)) => (A q B)";
            interactive.process(rule);
            interactive.process(rule);
        }
        SUBCASE("seven hundred ground conditions")
        {
            interactive.process(R"js(%(defn build [] (def x (zelph/var "X")) (zelph/fact (zelph/conjunction (zelph/fact x "p" "a") ;(seq [i :range [0 700]] (zelph/fact (string "c" i) "p" (string "d" i)))) "=>" (zelph/fact x "q" "a"))))js");
            interactive.process("%(zelph/dedup-rule build)");
            interactive.process("%(zelph/dedup-rule build)");
        }
        collector.clear();
        interactive.process(".stat");
        CHECK(any_output_contains(collector, "Rules: 1")); });
}

// A program may write two rules over a single set of conditions, built once.
// The set is read as the conditions for each rule, and the variables it binds
// are identical nodes in both, so a rule concluding about `X` is not a rule
// concluding about `Z`. Recognized by its identity, the set matched itself
// without binding any variable, the second rule was rolled back as a
// duplicate of the first, and `cc q a1` was never derived. Compared by its
// members, the set still leaves two rules that are alike up to a fresh
// variable as one rule.
TEST_CASE("rule identity: two rules over one set of conditions are compared by what the set binds")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
aa p bb
bb r cc
%(def x (zelph/var "X"))
%(def y (zelph/var "Y"))
%(def z (zelph/var "Z"))
%(def s (zelph/collection (zelph/fact x "p" y) (zelph/fact y "r" z)))
%(zelph/fact s "~" "conjunction")
)");
        SUBCASE("conclusions about two variables of the set")
        {
            interactive.process(R"js(%(def r1 (zelph/dedup-rule (fn [] (zelph/fact s "=>" (zelph/fact x "q" "a1"))))))js");
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "same=" (= r1 (zelph/dedup-rule (fn [] (zelph/fact s "=>" (zelph/fact z "q" "a1"))))))))js");
            CHECK(any_output_contains(collector, "same=false"));
            collector.clear();
            interactive.process("X q a1");
            std::vector<std::string> answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"aa q a1", "cc q a1"});
        }
        SUBCASE("conclusions alike up to a fresh variable")
        {
            interactive.process(R"js(%(def r1 (zelph/dedup-rule (fn [] (zelph/fact s "=>" (zelph/fact x "q" (zelph/var "W")))))))js");
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "same=" (= r1 (zelph/dedup-rule (fn [] (zelph/fact s "=>" (zelph/fact x "q" (zelph/var "W")))))))))js");
            CHECK(any_output_contains(collector, "same=true"));
        } });
}

// The same applies to a container shared by two rules in a term. A program
// reaches such a pair via the Janet API, by keeping a variable and a container
// and employing both within two rules, or through a set of conditions made
// outside the rules; typed rules get fresh variables and fresh containers per
// statement. Each rule instantiates the container using the bindings of the
// variables within it, so where the bijection maps such a variable elsewhere,
// the two rules derive different facts. Taken by its identity, the container
// matched itself, the second rule was rolled back as a duplicate of the first,
// and what it derived was lost. Each subcase checks the derivations of both
// rules; the last one checks that a container both rules read alike still
// leaves one rule.
TEST_CASE("rule identity: two rules over one container in a term are compared by what the container holds")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
b p a
%(def x (zelph/var "X"))
%(def y (zelph/var "Y"))
%(var c nil)
%(var sc nil)
)");
        // A rule over the conditions `(X p Y, Y p X)`, which the thunk builds
        // anew after `prelude`, built by zelph/dedup-rule.
        const auto dedup = [](const std::string& prelude, const std::string& consequence)
        {
            return "(zelph/dedup-rule (fn [] " + prelude
                 + R"js( (def s (zelph/collection (zelph/fact x "p" y) (zelph/fact y "p" x))) (zelph/fact s "~" "conjunction") (zelph/fact s "=>" )js"
                 + consequence + ")))";
        };
        const auto first  = [&](const std::string& prelude, const std::string& consequence)
        { interactive.process("%(def r1 " + dedup(prelude, consequence) + ")"); };
        const auto second = [&](const std::string& consequence)
        {
            collector.clear();
            interactive.process("%(zelph/out (string \"same=\" (= r1 " + dedup("", consequence) + ")))");
        };

        SUBCASE("a rule's own collection in a consequence")
        {
            first("(set c (zelph/collection x))", R"js((zelph/fact x "q" c) )js");
            second(R"js((zelph/fact y "q" c) )js");
            CHECK(any_output_contains(collector, "same=false"));

            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S q O");
            std::vector<std::string> answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"a q {a}", "a q {b}", "b q {a}", "b q {b}"});
        }
        SUBCASE("a set constant over a rule's own collection in a consequence")
        {
            first(R"js((set c (zelph/collection x)) (set sc (zelph/set c "e")) )js", R"js((zelph/fact x "q" sc) )js");
            second(R"js((zelph/fact y "q" sc) )js");
            CHECK(any_output_contains(collector, "same=false"));

            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S q O");
            CHECK(collect_answers(collector).size() == 4);
            CHECK(answers_contain(collector, "a q {e {b}}"));
            CHECK(answers_contain(collector, "b q {e {a}}"));
        }
        SUBCASE("a set pattern in a condition")
        {
            // `(X has {Y})` binds Y via the membership, both rules hold the
            // same pattern, and the second draws its conclusion regarding Z.
            process_lines(interactive, R"(
x has {a}
a r b
b r a
%(def z (zelph/var "Z"))
)");
            const auto over = [](const std::string& prelude, const std::string& concluded)
            {
                return "(zelph/dedup-rule (fn [] " + prelude
                     + R"js( (def s (zelph/collection (zelph/fact x "has" c) (zelph/fact y "r" z) (zelph/fact z "r" y))) (zelph/fact s "~" "conjunction") (zelph/fact s "=>" (zelph/fact x "q" )js"
                     + concluded + "))))";
            };
            interactive.process("%(def r1 " + over("(set c (zelph/collection y))", "y") + ")");
            collector.clear();
            interactive.process("%(zelph/out (string \"same=\" (= r1 " + over("", "z") + ")))");
            CHECK(any_output_contains(collector, "same=false"));

            interactive.run(true, false, false);
            collector.clear();
            interactive.process("x q O");
            std::vector<std::string> answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"x q a", "x q b"});
        }
        SUBCASE("a set of conditions made outside the rules, in the consequence of a generator")
        {
            // A generator rebuilds the set `{(X s K)}` of its inner rule using
            // the binding of K: the first generator writes it regarding K, the
            // subsequent one regarding V.
            process_lines(interactive, R"js(
c r d
%(def k (zelph/var "K"))
%(def v (zelph/var "V"))
%(def t (zelph/collection (zelph/fact x "s" k)))
%(zelph/fact t "~" "conjunction")
%(defn gen [who] (fn [] (def s (zelph/collection (zelph/fact k "p" v) (zelph/fact v "p" k))) (zelph/fact s "~" "conjunction") (zelph/fact s "=>" (zelph/fact (zelph/fact x "r" y) "=>" (zelph/fact who "has" t)))))
%(def g1 (zelph/dedup-rule (gen k)))
)js");
            collector.clear();
            interactive.process("%(zelph/out (string \"same=\" (= g1 (zelph/dedup-rule (gen v)))))");
            CHECK(any_output_contains(collector, "same=false"));

            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S has O");
            CHECK(collect_answers(collector).size() == 4);
        }
        SUBCASE("a container both rules read alike")
        {
            first("(set c (zelph/collection x))", R"js((zelph/fact x "q" c) )js");
            second(R"js((zelph/fact x "q" c) )js");
            CHECK(any_output_contains(collector, "same=true"));
        } });
}

TEST_CASE("rule identity: a rule whose variables sit only in a nested conjunction is compared by its structure")
{
    // The template-variable store captures only the variables associated
    // with hash components, and a conjunction set does not qualify as one --
    // it holds its members through membership facts. Thus, the inner rule
    // `((X q H), (X p H)) => !`, along with any fact constructed from it,
    // lacks an entry, and rule identity read it as a variable-free node,
    // comparing it by identity. The second statement of such a rule
    // constituted a second rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto enters_once = [&](const std::string& rule)
        {
            interactive.process(rule);
            collector.clear();
            interactive.process(".stat");
            const std::string before = node_count(collector);
            REQUIRE_FALSE(before.empty());

            interactive.process(rule);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
            collector.clear();
            interactive.process(".stat");
            CHECK(node_count(collector) == before);
        };

        SUBCASE("the conjunction belongs to the consequence")
        {
            enters_once("(G go H) => (((X q H), (X p H)) => !)");
        }
        SUBCASE("the conjunction belongs to a rule one level further down")
        {
            enters_once("(G go H) => ((a is up) => (((X q H), (X p H)) => !))");
        }
        SUBCASE("the conjunction belongs to a switch's rule")
        {
            enters_once("(K is on) => ((X p Y, X r Y) => (X likes Y))");
        }
        SUBCASE("the conjunction holds a collection of the generator")
        {
            enters_once("(G go H) => ((X s Y) => (X p @{(Z q Y)})) ((X p @{(Z q H)}, X r Y) => (X likes Y))");
        }

        // A set constant does not have an entry in the store either, and it
        // hash-conses, so it was compared by its node. Each statement builds a
        // set constant of its own around its individual collection, and the
        // second statement was a second rule.
        SUBCASE("the conjunction belongs to a rule inside a set constant")
        {
            enters_once("(G go H) => ((X p H) => (X likes {(((Y q H), (Y p H)) => !)}))");
        }
        SUBCASE("a collection inside a set constant")
        {
            enters_once("(G go H) => ((X p H) => (X likes {@{(Y q H)}}))");
        } });
}

TEST_CASE("rule identity: a set of conditions that a rule's text makes is compared by its members wherever it stands")
{
    // Every statement of a rule makes the sets of conditions its text holds
    // anew: the conditions contained within a rule nested in a consequence, and
    // a set that a consequence refers to. Compared via its node, a counter that
    // each statement builds anew, such a set never matched the first
    // statement's, and each statement of the rule added one. A set that the
    // statement has just made, and that the rule's rollback retracts, is held
    // by nothing else, so its members determine the outcome.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        std::string rule;
        SUBCASE("in a switch with a ground condition") { rule = "(k is on) => ((k partof I, I q k) => (k is green))"; }
        SUBCASE("talked about in a consequence") { rule = "(X q Y) => ((X p Y, X r Y) is noted)"; }

        interactive.process(rule);
        collector.clear();
        interactive.process(".stat");
        const std::string before = node_count(collector);
        REQUIRE_FALSE(before.empty());

        interactive.process(rule);
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);
        collector.clear();
        interactive.process(".stat");
        CHECK(node_count(collector) == before); });
}

// A set of conditions within a term is a constant for the reasoner: a firing
// keeps the node and writes its facts regarding that node. Two distinct sets
// alike in their members are therefore two separate terms, and two rules naming
// them derive facts about two different nodes, unless the new rule's statement
// has just made its set and takes it back with the rule, so that nothing else
// holds it. Compared via their members, the two sets made the second rule a
// duplicate of the first, and what it derived was lost: a set named by the
// user, a set that a rule over rules bound, the set of a generator's product
// beside a typed twin, a set in a rule quoted within a consequence, a set that
// a fact written beside the rule or an island holds, a set constant of ground
// conditions, which every statement over its members builds again. A named set
// that stands in a term is the same term only as itself, in either order of
// entry.
TEST_CASE("rule identity: two distinct sets of conditions in a term are one term only where the new rule made its set")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };

        SUBCASE("two named sets in a consequence")
        {
            process_lines(interactive, R"(
(a r d) in sa
sa ~ conjunction
(a r d) in sb
sb ~ conjunction
aa p bb
(X p Y) => (sa q X)
(X p Y) => (sb q X)
)");
            CHECK(rules() == 2);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S q O");
            std::vector<std::string> answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"sa q aa", "sb q aa"});
        }
        SUBCASE("two named sets in a condition")
        {
            process_lines(interactive, R"(
(a r d) in sa
sa ~ conjunction
(a r d) in sb
sb ~ conjunction
x has sb
(X has sa) => (X q yes)
(X has sb) => (X q yes)
)");
            CHECK(rules() == 2);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S q O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"x q yes"});
        }
        SUBCASE("a named set and a typed set of the same members, in either order")
        {
            // The answers of a rule over the named set print its name wherever
            // the other rule's statement typed a list, so both rules stay, in
            // either order of entry.
            process_lines(interactive, R"(
(a r d) in sa
(b r e) in sa
sa ~ conjunction
)");
            std::string first;
            std::string second;
            SUBCASE("the named set first")
            {
                first  = "(X q Y) => (sa got X)";
                second = "(X q Y) => ((a r d, b r e) got X)";
            }
            SUBCASE("the typed set first")
            {
                first  = "(X q Y) => ((a r d, b r e) got X)";
                second = "(X q Y) => (sa got X)";
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(rules() == 2);
        }
        SUBCASE("a set of ground conditions written out and a typed list of the same members, in either order")
        {
            // `(*{(a s b) (c t d)} ~ conjunction)` builds a set of ground
            // conditions as a set constant, which is hash-consed: a subsequent
            // statement involving the same members builds the identical node,
            // thus no rule's statement holds it as its own. A condition that
            // names it matches facts concerning that node exclusively, just as
            // a set pattern does (unify_nodes matches a set pattern only with
            // a set constant), making the rule over the set constant and the
            // rule over the list two distinct terms. Compared by their
            // members, the rule over the set constant, entered after the rule
            // over the list, was rolled back as its twin, and `u1 seen yes`,
            // which only it derives, was lost.
            std::string over_list;
            std::string over_constant;
            std::string first;
            std::string second;
            std::string data;
            std::size_t observers = 0;
            SUBCASE("in a condition")
            {
                // The rule over the list matches facts concerning its own
                // list alone, which nothing other than the rule holds. The
                // fact comes after the rules: constructed before them, the
                // set constant was not a node the statement made, and the
                // rules stayed two.
                over_list     = "((a s b, c t d) noted X) => (X seen yes)";
                over_constant = "((*{(a s b) (c t d)} ~ conjunction) noted X) => (X seen yes)";
                data          = "(*{(a s b) (c t d)} ~ conjunction) noted u1";
                SUBCASE("the list first")
                {
                    first  = over_list;
                    second = over_constant;
                }
                SUBCASE("the set constant first")
                {
                    first  = over_constant;
                    second = over_list;
                }
            }
            SUBCASE("in a consequence")
            {
                // The observer's set pattern meets the fact regarding the
                // set constant, and not the one concerning the list.
                process_lines(interactive, R"(
u1 q k1
({(A s B) (C t D)} noted V) => (V seen yes)
)");
                observers     = 1;
                over_list     = "(X q k1) => ((a s b, c t d) noted X)";
                over_constant = "(X q k1) => ((*{(a s b) (c t d)} ~ conjunction) noted X)";
                SUBCASE("the list first")
                {
                    first  = over_list;
                    second = over_constant;
                }
                SUBCASE("the set constant first")
                {
                    first  = over_constant;
                    second = over_list;
                }
            }
            interactive.process(first);
            interactive.process(second);
            if (!data.empty()) interactive.process(data);
            CHECK(rules() == observers + 2);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S seen O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u1 seen yes"});
        }
        SUBCASE("a named set that the new statement makes is the same term only as itself")
        {
            // The set belonging to the new rule is its statement's own, and a
            // name maintains its distinction regardless.
            process_lines(interactive, R"js(
u q v
(X q Y) => ((a r d, b r e) got X)
%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def y (zelph/var "Y")) (zelph/fact (zelph/fact "a" "r" "d") "in" "sx") (zelph/fact (zelph/fact "b" "r" "e") "in" "sx") (zelph/fact (zelph/resolve "sx") "~" "conjunction") (zelph/fact (zelph/fact x "q" y) "=>" (zelph/fact (zelph/resolve "sx") "got" x))) )
)js");
            CHECK(rules() == 2);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S got O");
            CHECK(answers_contain(collector, "sx got u"));
        }
        SUBCASE("a rule quoted in a consequence holds its set of conditions as a term")
        {
            // A rule that stands in a term of a consequence is a value: a
            // firing writes it as it stands, with its set of conditions serving
            // as the node it is. Its conditions are not conditions of a rule in
            // this context, in contrast to those of a rule a generator writes,
            // so two quoted rules over two named sets are two distinct terms.
            process_lines(interactive, R"(
(a r d) in sa
(b r e) in sa
sa ~ conjunction
(a r d) in sb
(b r e) in sb
sb ~ conjunction
sa is special
u s v
)");
            std::string first;
            std::string second;
            SUBCASE("the rule over the special set first")
            {
                first  = "(X s Y) => ((sa => (e f k)) noted X)";
                second = "(X s Y) => ((sb => (e f k)) noted X)";
            }
            SUBCASE("the rule over the other set first")
            {
                first  = "(X s Y) => ((sb => (e f k)) noted X)";
                second = "(X s Y) => ((sa => (e f k)) noted X)";
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(rules() == 2);
            interactive.process("((C => (e f k)) noted X, C is special) => (X hit yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S hit O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("a rule quoted in an object of a consequence holds its set of conditions as a term")
        {
            // The identical scenario, with the quoted rule serving as the
            // object of the fact that a consequence writes: only the objects of
            // a rule that a rule writes are considered consequences.
            process_lines(interactive, R"(
(a r d) in sa
(b r e) in sa
sa ~ conjunction
(a r d) in sb
(b r e) in sb
sb ~ conjunction
sa is special
u s v
)");
            std::string first;
            std::string second;
            SUBCASE("the rule over the special set first")
            {
                first  = "(X s Y) => (X noted (sa => (e f k)))";
                second = "(X s Y) => (X noted (sb => (e f k)))";
            }
            SUBCASE("the rule over the other set first")
            {
                first  = "(X s Y) => (X noted (sb => (e f k)))";
                second = "(X s Y) => (X noted (sa => (e f k)))";
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(rules() == 2);
            interactive.process("(X noted (C => (e f k)), C is special) => (X hit yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S hit O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("two rules a condition quotes over named sets of the same members stay two")
        {
            // A condition matches the subject of a quoted rule through its
            // members when both are sets of conditions, whether named or not,
            // so both observers match the rule over `sb` that the first rule
            // writes. The comparison reads that subject as a term, as required
            // for a rule within a set constant and for a subject that is a set
            // containing one condition or an untagged container (see "a rule
            // that a condition names or quotes is read as a term"), and the two
            // observers stay two distinct rules where one would be sufficient;
            // both derive.
            process_lines(interactive, R"(
(a r d) in sa
(b r e) in sa
sa ~ conjunction
(a r d) in sb
(b r e) in sb
sb ~ conjunction
u s v
(X s Y) => ((sb => (e f k)) noted X)
((sa => (e f k)) noted X) => (X hit yes)
((sb => (e f k)) noted X) => (X hit yes)
)");
            CHECK(rules() == 3);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S hit O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("rules over rules bind the condition sets of two rules in quoted rules")
        {
            // As in the subcase "rules over rules bind the condition sets of
            // two rules", with each bound set appearing in a rule that the
            // generated rule quotes.
            process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(a p b, c q d) => (e f h)
(C => (e f g)) => ((X s Y) => ((C => (e f k)) noted X))
(C => (e f h)) => ((X s Y) => ((C => (e f k)) noted X))
(C => (e f h)) => (C is hset)
u s v
((C => (e f k)) noted X, C is hset) => (X hit yes)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S hit O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("a set that a fact written beside the rule holds is not the statement's own")
        {
            // A focus writes the fact it marks beside the rule, and that fact
            // stays when the rule is rolled back as a duplicate, along with the
            // set it holds. Taken as the first rule's set, the second rule's
            // set stayed `noted` with no rule writing what is `got` about it.
            process_lines(interactive, R"(
u q v
(X q Y) => ((a p b, c r d) got X)
(X q Y) => ((*(a p b, c r d) is noted) got X)
)");
            CHECK(rules() == 2);
            interactive.process("(L got X, L is noted) => (X hit yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S hit O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("a set an island makes is not the statement's own")
        {
            // What the islands of a statement construct remains upon the rule
            // being rolled back as a duplicate, carrying every fact an island
            // wrote.
            process_lines(interactive, R"js(
u q v
%(zelph/register-keyword "<<" ">>" (fn [text] (def s (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "r" "d"))) (zelph/fact s "~" "conjunction") (zelph/fact s "is" "noted") s) )
(X q Y) => ((a p b, c r d) got X)
(X q Y) => (<<s>> got X)
)js");
            CHECK(rules() == 2);
            interactive.process("(L got X, L is noted) => (X hit yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S hit O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("rules over rules bind the condition sets of two rules")
        {
            // The two rules over `(a p b, c q d)` possess two condition sets,
            // and the generators write one rule per set. The rule generated
            // for the second set derives what the join beneath requires.
            process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(a p b, c q d) => (e f h)
(C => (e f g)) => ((X s Y) => (C first X))
(C => (e f h)) => ((X s Y) => (C first X))
(C => (e f h)) => (C is hset)
u s v
(C first X, C is hset) => (X hit yes)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S first O");
            CHECK(collect_answers(collector).size() == 2);
            collector.clear();
            interactive.process("S hit O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("a typed set, then a rule over rules that binds a rule's set of the same members")
        {
            process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(X s Y) => ((a p b, c q d) first X)
(C => (e f g)) => ((X s Y) => (C first X))
(C => (e f g)) => (C is gset)
u s v
(C first X, C is gset) => (X hit yes)
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S hit O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("a generator's rules share its set beside a typed twin entered before them")
        {
            // The generator writes one rule for each switch, both concerning
            // the set the generator quotes, and the join relates what the two
            // rules note about that set. The typed twin of the first rule
            // records information about its own set. When taken as the typed
            // twin, the first generated rule was never written, and `u1 with
            // u2` was missing. The set holds variables that the switch does not
            // bind, so the generator finds no alternative set for it; a set
            // lacking variables it reconstructs identically, it finds among all
            // sets sharing the same members, including the typed twin's,
            // depending on the node ids.
            process_lines(interactive, R"(
k1 is on
k2 is on
u1 q k1
u2 q k2
(X q k1) => ((X p Y, X r Y) noted X)
(K is on) => ((X q K) => ((X p Y, X r Y) noted X))
)");
            interactive.run(true, false, false);
            interactive.process("(L noted X, L noted Y) => (X with Y)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("u1 with O");
            CHECK(answers_contain(collector, "u1 with u2"));
        }
        SUBCASE("a typed twin before a generator stays beside a rule the generator makes a set for")
        {
            // The generator substitutes `k` into the set of its inner rule and
            // makes the set anew. A different rule from the same firing might
            // locate that set through its members, so the construction does not
            // count it as its rule's own, and the typed twin entered before
            // remains beside the generated rule. Typed afterwards, the twin
            // stays as well, because a set a firing writes is read as the
            // firing writes it (ReadAt::Written): the generated rule's set is
            // one the construction made, which a firing writes through its
            // node, and the twin's is a list its own statement writes, which a
            // firing's claim enters. The identity asks how a firing writes each
            // set, not what the claim subsequently asserts, and here it asserts
            // nothing: the firing writes the list as it stands, and both its
            // members hold the variable X, so no query or condition meets them.
            // Thus, in this order the twin is a duplicate, whose firing writes
            // its fact about its own list, a second answer that prints alike.
            // With a ground member, the claim makes that member data, which the
            // generated rule's firing does not ("a switch writes a rule over a
            // set its construction makes").
            process_lines(interactive, R"(
k is on
u q k
)");
            const std::string typed     = "(X q k) => ((X p k, X r Y) noted X)";
            const std::string generator = "(K is on) => ((X q K) => ((X p K, X r Y) noted X))";
            SUBCASE("typed before")
            {
                interactive.process(typed);
                interactive.process(generator);
                interactive.run(true, false, false);
            }
            SUBCASE("typed after")
            {
                interactive.process(generator);
                interactive.run(true, false, false);
                interactive.process(typed);
            }
            CHECK(rules() == 3);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S noted O");
            CHECK(collect_answers(collector).size() == 2);
        }
        SUBCASE("a typed twin before a generator stays beside the generator's rule")
        {
            // The rule generated for `k` refers to the set quoted by the
            // generator, the typed twin to its own: two sets that print alike,
            // and two rules. Typed after the generator's rule, the twin's set
            // is one its statement has just made, and the twin is recognized.
            process_lines(interactive, R"(
k is on
u q k
)");
            const std::string typed     = "(X q k) => ((X p Y, X r Y) noted X)";
            const std::string generator = "(K is on) => ((X q K) => ((X p Y, X r Y) noted X))";
            std::size_t       expected  = 0;
            SUBCASE("typed before")
            {
                interactive.process(typed);
                interactive.process(generator);
                interactive.run(true, false, false);
                expected = 3;
            }
            SUBCASE("typed after")
            {
                interactive.process(generator);
                interactive.run(true, false, false);
                interactive.process(typed);
                expected = 2;
            }
            CHECK(rules() == expected);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S noted O");
            CHECK(collect_answers(collector).size() == expected - 1);
        } });
}

// A firing writes a set of conditions in its consequence as it stands, and its
// claim asserts the members of a list the statement writes, or of a set
// constant, but writes a rule's own set, or one a construction built, via its
// node (read_container, ReadAt::Written). A rule's own set that is a set
// constant is entered just as every set constant is: a firing that writes the
// set of a rule spelled as a set constant asserts the conditions of that rule,
// and one that writes the set of the same rule spelled as a comma list asserts
// nothing. That is today's reading, not a settled one: the last subcase holds
// it, and the preceding one does not rely upon it. Two such sets are
// considered a single term only when the firing writes them in an identical
// manner: a rule rolled back as the twin of a rule whose firing asserts less
// lost what its own claim had asserted.
TEST_CASE("rule identity: two sets of conditions a firing writes are one term only where it writes them alike")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };

        SUBCASE("the typed rule alone asserts the members of the list it writes")
        {
            // The control for the next subcase: the firing of the typed rule
            // transforms the conditions of the rule over the same members
            // into data, and that rule fires.
            process_lines(interactive, R"(
u1 q k1
(a s b, c t d) => (e f g)
(X q k1) => ((a s b, c t d) noted X)
)");
            interactive.run(true, false, false);
            CHECK(rules() == 2);
            CHECK(answers("e f O") == std::vector<std::string>{"e f g"});
            CHECK(answers("X s b") == std::vector<std::string>{"a s b"});
            CHECK(answers("X t d") == std::vector<std::string>{"c t d"});
        }
        SUBCASE("a rule over rules writes a rule over the set of a rule of the same members")
        {
            // The rule the rule over rules writes names the set of the rule
            // `(a s b, c t d) => (e f g)` via its node, and its firing
            // asserts nothing. Read in the same way, the typed rule, entered
            // after it, was rolled back as its twin, and `a s b`, `c t d`,
            // and `e f g` were lost. Entered before it, the typed rule
            // remains beside the written one: a construction does not take a
            // set for its own (NewRuleNodes).
            process_lines(interactive, R"(
u1 q k1
(a s b, c t d) => (e f g)
)");
            const std::string typed = "(X q k1) => ((a s b, c t d) noted X)";
            const std::string over  = "(C => (e f g)) => ((X q k1) => (C noted X))";
            SUBCASE("the rule over rules first")
            {
                interactive.process(over);
                interactive.process(typed);
            }
            SUBCASE("the typed rule first")
            {
                interactive.process(typed);
                interactive.process(over);
            }
            interactive.run(true, false, false);
            CHECK(rules() == 4);
            CHECK(answers("e f O") == std::vector<std::string>{"e f g"});
            CHECK(answers("X s b") == std::vector<std::string>{"a s b"});
            CHECK(answers("X t d") == std::vector<std::string>{"c t d"});
        }
        SUBCASE("a switch writes a rule over a set its construction makes")
        {
            // The switch substitutes `k` into its list, and no set of these
            // members is present prior to the typed rule's: the construction
            // makes one, which the written rule's firing writes via its node.
            // Read in the same way, the typed rule, entered after it, was
            // rolled back as its twin, and `a s k` and `e f g` were lost.
            // Entered beforehand, the typed rule's set is the one the
            // construction finds, and the switch's rule is the typed rule.
            process_lines(interactive, R"(
u1 q k1
k is on
(a s k) => (e f g)
)");
            const std::string typed  = "(X q k1) => ((a s k, c t d) noted X)";
            const std::string sw     = "(K is on) => ((X q k1) => ((a s K, c t d) noted X))";
            std::size_t       listed = 0;
            SUBCASE("the switch first")
            {
                interactive.process(sw);
                interactive.process(typed);
                listed = 4;
            }
            SUBCASE("the typed rule first")
            {
                interactive.process(typed);
                interactive.process(sw);
                listed = 3;
            }
            interactive.run(true, false, false);
            CHECK(rules() == listed);
            CHECK(answers("e f O") == std::vector<std::string>{"e f g"});
            CHECK(answers("X s k") == std::vector<std::string>{"a s k"});
        }
        SUBCASE("a typed rule writes a set constant over a list, beside a rule written over a rule's set of that list")
        {
            // The claim of a firing enters a set constant that the firing
            // writes, just as it enters a list the statement writes: through
            // the set constant, the typed rule's firing asserts `j r s` and
            // the list's members, `a s b` and `c t d`, and the nested rule
            // fires. The rule that the rule over rules writes names the set
            // of that nested rule, a collection, via its node, and its firing
            // asserts nothing. The set constant is compared by its members
            // here, since the list beneath it is text of the rule. Read as a
            // set that the firing writes via its node, it would be the nested
            // rule's set: the typed rule, entered after the written one,
            // would be rolled back as its twin, and `a s b`, `c t d`, `j r s`
            // and `e f g` would be lost.
            process_lines(interactive, R"(
u1 q k1
((a s b, c t d), j r s) => (e f g)
)");
            const std::string typed = "(X q k1) => ((*{((a s b), (c t d)) (j r s)} ~ conjunction) noted X)";
            const std::string over  = "(C => (e f g)) => ((X q k1) => (C noted X))";
            SUBCASE("the rule over rules first")
            {
                interactive.process(over);
                interactive.process(typed);
            }
            SUBCASE("the typed rule first")
            {
                interactive.process(typed);
                interactive.process(over);
            }
            interactive.run(true, false, false);
            CHECK(rules() == 4);
            CHECK(answers("e f O") == std::vector<std::string>{"e f g"});
            CHECK(answers("X s b") == std::vector<std::string>{"a s b"});
            CHECK(answers("X t d") == std::vector<std::string>{"c t d"});
            CHECK(answers("X r s") == std::vector<std::string>{"j r s"});
        }
        SUBCASE("a rule over rules writes a rule over a set constant of the same members")
        {
            // The written rule's firing asserts the members of the set constant
            // it names, the set of a rule: today's reading, which differs from
            // that of the same rule spelled as a comma list (the subcase "a
            // rule over rules writes a rule over the set of a rule of the same
            // members") and is not settled. The answers do not rest on it,
            // since the typed rule's firing asserts the identical members,
            // those of its list. What the subcase holds is the count: the
            // identity reads a set constant of ground conditions through its
            // node wherever it stands, as a condition that names it does, so
            // the typed rule, entered after the written one, stays beside it:
            // its firing writes its fact regarding its own list, and the
            // written rule's firing writes its fact regarding the set constant.
            process_lines(interactive, R"(
u1 q k1
(*{(a s b) (c t d)} ~ conjunction) => (e f g)
(C => (e f g)) => ((X q k1) => (C noted X))
(X q k1) => ((a s b, c t d) noted X)
)");
            interactive.run(true, false, false);
            CHECK(rules() == 4);
            CHECK(answers("e f O") == std::vector<std::string>{"e f g"});
            CHECK(answers("X s b") == std::vector<std::string>{"a s b"});
            CHECK(answers("X t d") == std::vector<std::string>{"c t d"});
        }
        SUBCASE("the rule a rule over rules writes over a set constant asserts the members of that constant")
        {
            // The program of the preceding subcase without the typed rule:
            // the rule over the set constant is written solely by the rule
            // over rules, so nothing other than the claim of its firing
            // asserts `a s b` and `c t d`. The claim enters the set constant
            // that the firing writes, even though it is the set of a rule,
            // the conditions become data, and `e f g` follows; written by its
            // node, just as the set of the same rule spelled as a comma list
            // is, the constant would keep its members as rule text and
            // nothing would follow. This is the reading as it currently
            // stands, which differs between the two spellings of one rule and
            // is not settled: the subcase holds it so that a change to what
            // the claim enters shows here.
            process_lines(interactive, R"(
u1 q k1
(*{(a s b) (c t d)} ~ conjunction) => (e f g)
(C => (e f g)) => ((X q k1) => (C noted X))
)");
            interactive.run(true, false, false);
            CHECK(rules() == 3);
            CHECK(answers("S noted O").size() == 1);
            CHECK(answers("e f O") == std::vector<std::string>{"e f g"});
            CHECK(answers("X s b") == std::vector<std::string>{"a s b"});
            CHECK(answers("X t d") == std::vector<std::string>{"c t d"});
        } });
}

// A rule that a rule writes writes its own consequences upon firing, so the
// sets of conditions there are read as its firing writes them, one rule deeper
// and at any depth beneath. Read as rule text, the typed generator entered
// after the rule over rules was rolled back as the twin of the generator that
// rule wrote over the set of `(a s b, c t d) => (e f g)`, and `e f g` never
// came. The rule a generator writes lands on its own list or on that set as the
// node ids fall (find_conjunction_set), and the rule over rules entered first
// moves the ids of the typed generator's list, so whether `e f g` comes then
// follows the layout, not the identity. Each layout checks what holds in every
// one: both generators stay rules of their own; entered after the rule over
// rules, the typed generator derives `e f g` wherever its rule lands on its own
// list; entered first, it writes its rules before the rule over rules is
// entered and derives all it derives alone.
TEST_CASE("rule identity: the consequences of a rule that a rule writes are read as its firing writes them, at any depth")
{
    // Generators of the second shape write a generator in turn.
    struct Shape
    {
        std::string name;
        std::string switches;
        std::string typed;
        std::string over;
    };
    const std::vector<Shape> shapes = {
        {"one rule deeper", "k is on", "(K is on) => ((X q k1) => ((a s b, c t d) noted X))", "(C => (e f g)) => ((K is on) => ((X q k1) => (C noted X)))"},
        {"two rules deeper", "k is on\nj is up", "(J is up) => ((K is on) => ((X q k1) => ((a s b, c t d) noted X)))", "(C => (e f g)) => ((J is up) => ((K is on) => ((X q k1) => (C noted X))))"}};

    // Which rules `.list-rules` lists: the typed generator, the generator that
    // the rule over rules writes, and a rule a generator writes over the typed
    // list, which asserts the list's members at the moment it fires.
    struct Outcome
    {
        bool                     typed   = false;
        bool                     written = false;
        bool                     own     = false;
        std::vector<std::string> answers;
    };
    const auto run = [](const Shape& shape, const int shift, const std::vector<std::string>& rules)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        for (int i = 0; i < shift; ++i)
            interactive.process("pad" + std::to_string(i) + " is pad");
        process_lines(interactive, "u1 q k1\n" + shape.switches + "\n(a s b, c t d) => (e f g)");
        for (const std::string& rule : rules)
            interactive.process(rule);
        interactive.run(true, false, false);

        collector.clear();
        interactive.process(".list-rules");
        std::vector<std::string> lines;
        for (const auto& event : collector.events())
            lines.push_back(normalize(event.text));
        // The listing prints the typed list as a comma list and a rule's set
        // enclosed in braces, with their members arranged according to the
        // order of their node ids.
        const auto listed = [&](std::string rule, const std::string& ab, const std::string& ba)
        {
            const std::string list = "(a s b, c t d)";
            const std::size_t at   = rule.find(list);
            std::string       swap = rule;
            rule.replace(at, list.size(), ab);
            swap.replace(at, list.size(), ba);
            return std::ranges::find(lines, rule) != lines.end() || std::ranges::find(lines, swap) != lines.end();
        };
        Outcome outcome;
        outcome.typed   = listed(shape.typed, "((a s b), (c t d))", "((c t d), (a s b))");
        outcome.written = listed(shape.typed, "{(a s b) (c t d)}", "{(c t d) (a s b)}");
        outcome.own     = listed("(X q k1) => ((a s b, c t d) noted X)", "((a s b), (c t d))", "((c t d), (a s b))");

        collector.clear();
        interactive.process("e f O");
        outcome.answers = collect_answers(collector);
        std::ranges::sort(outcome.answers);
        return outcome;
    };

    for (const Shape& shape : shapes)
    {
        for (int shift = 0; shift < 16; ++shift)
        {
            INFO(shape.name << ", shift " << shift);

            // The control case, excluding the rule over rules.
            const Outcome alone = run(shape, shift, {shape.typed});
            CHECK(alone.typed);
            CHECK_FALSE(alone.written);

            const Outcome over_first = run(shape, shift, {shape.over, shape.typed});
            CHECK(over_first.typed);
            CHECK(over_first.written);
            if (over_first.own) CHECK(over_first.answers == std::vector<std::string>{"e f g"});

            const Outcome typed_first = run(shape, shift, {shape.typed, shape.over});
            CHECK(typed_first.typed);
            CHECK(typed_first.written);
            CHECK(std::ranges::includes(typed_first.answers, alone.answers));
        }
    }
}

// The claim search compares a switch's mentioned rule against every rule in
// force, and neither rule is new within that context; a named set matches only
// itself therein as well. Claimed for the rule over `sa`, the switch never
// wrote its own rule, and what it derives regarding the typed set was missing.
// Refused, the switch builds a copy of its rule, and the construction finds the
// copy's set through its members among every such set, `sa` included; the one
// it encounters first follows the node ids and the order of a hash set, and in
// certain layouts the copy lands on `sa`. Thus the test counts the layouts
// where the switch's own derivation is present.
TEST_CASE("rule identity: a switch over a typed set does not claim a rule over a named set of its members")
{
    int found = 0;
    for (int shift = 0; shift < 16; ++shift)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        for (int i = 0; i < shift; ++i)
            interactive.process("pad" + std::to_string(i) + " is pad");
        process_lines(interactive, R"(
(a r d) in sa
(b r e) in sa
sa ~ conjunction
k is on
u q v
(X q Y) => (sa got X)
(K is on) => ((X q Y) => ((a r d, b r e) got X))
)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S got O");
        // The switch's own rule provides responses regarding the typed set,
        // whose members print in a sequence that follows the node ids.
        const std::vector<std::string> answers = collect_answers(collector);
        if (std::ranges::any_of(answers, [](const std::string& answer)
                                { return answer != "sa got u" && answer.ends_with(" got u"); }))
            ++found;
    }
    CHECK(found > 0);
}

// A generator whose two inner rules hold sets composed of identical members,
// which it reconstructs using its binding, writes both rules over a single set:
// the second construction finds the set the first one created through its
// members. Beside a typed twin of the first inner rule entered earlier, the
// first rule was rolled back as its duplicate, its set reclaimed alongside it,
// and the second rule made a set independently, thus causing the join over the
// shared set to be lost. The order in which the generator builds its inner
// rules follows the node ids, hence the sixteen layouts; the join must hold in
// every one of them.
TEST_CASE("rule identity: a generator's rules that share a set it makes are written beside a typed twin")
{
    for (int shift = 0; shift < 16; ++shift)
    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process(".semi-naive check");
        for (int i = 0; i < shift; ++i)
            interactive.process("pad" + std::to_string(i) + " is pad");
        process_lines(interactive, R"(
k is on
u q k
(X q k) => ((X p k, X r Y) noted X)
(K is on) => ((X q K) => ((X p K, X r Y) noted X)) ((X q K) => ((X p K, X r Y) seen X))
)");
        interactive.run(true, false, false);
        interactive.process("(L noted X, L seen X) => (X both yes)");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S both O");
        INFO("shift " << shift);
        CHECK(collect_answers(collector) == std::vector<std::string>{"u both yes"});
    }
}

// A set of conditions that a rule reads within a term constitutes a node that a
// firing writes its facts about, and likewise the collection a firing makes
// from a rule's own collection of fixed members, one per binding and template;
// the two rules being compared must be alike up to a renaming of those nodes as
// well as of their variables: one container of the new rule corresponds to one
// container of the rule in force at every place, and two to two. Paired at each
// location individually, one set of the new rule could represent two distinct
// sets of the rule in force, or two for one, and a guard `L != M` or a join
// across one set served to distinguish the rules after the second had been
// rolled back. A collection that holds a variable is written as the set
// constant of its members, which is the same node for two templates, so two
// rules over it may stay two even when one would suffice.
TEST_CASE("rule identity: a container in a term stands for one container of the other rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("two sets of the new rule are not one set of the rule in force")
        {
            // The generator writes a rule over a single bound set at two
            // places; the typed rule following it possesses two sets of its
            // own.
            process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(C => (e f g)) => ((X s Y) => (C noted X) (C seen X))
u s v
)");
            interactive.run(true, false, false);
            interactive.process("(X s Y) => ((a p b, c q d) noted X) ((a p b, c q d) seen X)");
            interactive.process("(L noted X, M seen X, L != M) => (X distinct yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S distinct O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u distinct yes"});
        }
        SUBCASE("one set at two places is not two sets, in either order of entry")
        {
            // The rule built by `one` records `X` regarding a single set of its
            // own at two places, the rule built by `two` concerning two sets.
            process_lines(interactive, R"js(
u q k
%(defn two [] (def x (zelph/var "X")) (def s1 (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "r" "d"))) (zelph/fact s1 "~" "conjunction") (def s2 (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "r" "d"))) (zelph/fact s2 "~" "conjunction") (zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact s1 "noted" x) (zelph/fact s2 "seen" x)))
%(defn one [] (def x (zelph/var "X")) (def s (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "r" "d"))) (zelph/fact s "~" "conjunction") (zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact s "noted" x) (zelph/fact s "seen" x)))
)js");
            std::string first;
            std::string second;
            SUBCASE("the rule over two sets first")
            {
                first  = "two";
                second = "one";
            }
            SUBCASE("the rule over one set first")
            {
                first  = "one";
                second = "two";
            }
            interactive.process("%(def kept (zelph/dedup-rule " + first + "))");
            collector.clear();
            interactive.process("%(zelph/out (string \"same=\" (= kept (zelph/dedup-rule " + second + "))))");
            CHECK(any_output_contains(collector, "same=false"));
            interactive.process("(L noted X, L seen X) => (X both yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S both O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u both yes"});
        }
        SUBCASE("a generator that rebuilds two sets into one beside a typed rule over two")
        {
            // The generator substitutes `k` into its two sets sharing
            // identical members, and the construction finds the first one
            // it made for the second: its rule notes `X` regarding one set
            // at two distinct places.
            process_lines(interactive, R"(
k is on
u q k
(X q k) => ((X p k, X r Y) noted X) ((X p k, X r Y) seen X)
(K is on) => ((X q K) => ((X p K, X r Y) noted X) ((X p K, X r Y) seen X))
)");
            interactive.run(true, false, false);
            interactive.process("(L noted X, L seen X) => (X both yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S both O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u both yes"});
        }
        SUBCASE("a rule's own collection of fixed members stands for one collection")
        {
            // The typed rule encompasses two collections, while the rule built
            // by `one` contains just one, present at two places.
            process_lines(interactive, R"js(
u q k
(X q k) => (X has @{a}) (X owns @{a})
%(defn one [] (def x (zelph/var "X")) (def c (zelph/collection "a")) (zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact x "has" c) (zelph/fact x "owns" c)))
%(zelph/dedup-rule one)
)js");
            interactive.process("(X has L, X owns L) => (X same yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S same O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u same yes"});
        }
        SUBCASE("a set both rules hold stands for itself")
        {
            // The rule in force records `X` regarding the set made outside the
            // rules at both places; the new rule notes it about a set of its own
            // and the one made outside. Paired with the set made outside at the
            // first place, the new rule's own set took the image that the set
            // made outside has for itself at the second. The set made outside
            // constitutes the conditions of a rule of its own, thus a firing
            // writes it via its node, just as it writes the new rule's own set
            // (ReadAt::Written), and only the pairing tells the two apart.
            process_lines(interactive, R"js(
u q k
%(def f (zelph/collection (zelph/fact "a" "p" "b")))
%(zelph/fact f "~" "conjunction")
%(zelph/fact f "=>" (zelph/fact "c" "d" "e"))
%(def kept (zelph/dedup-rule (fn [] (def x (zelph/var "X")) (zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact f "noted" x) (zelph/fact f "seen" x)))))
)js");
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "same=" (= kept (zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def s (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact s "~" "conjunction") (zelph/fact (zelph/fact x "q" "k") "=>" (zelph/fact s "noted" x) (zelph/fact f "seen" x))))))) )js");
            CHECK(any_output_contains(collector, "same=false"));
            interactive.process("(L noted X, M seen X, L != M) => (X distinct yes)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S distinct O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"u distinct yes"});
        } });
}

// A program may place one set of conditions at a rule's subject and within its
// consequence, where a firing writes a fact concerning the set itself. The rule
// over one such set is not the rule over two sets containing identical members,
// one positioned at each place: a set stands for a single set of the other rule
// at every place, including the rule's own subject, and a set containing one
// condition that also stands in the consequence is not read as that condition
// (read_container, ReadAt::OwnSubject). Compared at the subject outside the
// pairing, or read there as its condition, the set let the rule over one set
// pass for the rule over two; with the rule over one set entered first,
// `u seen yes` was lost, because a firing of that rule writes nothing the
// observer meets. Only a program puts one set node at both locations.
TEST_CASE("rule identity: a set that is a rule's conditions and a term of its consequence is one set at both places")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        // `one` notes `X` regarding its own set of conditions, `two` concerning
        // a second set composed of the same members; `facts` are the conditions,
        // over `x` and `y`.
        const auto define = [&](const std::string& facts)
        {
            interactive.process(R"js(%(defn one [] (def x (zelph/var "X")) (def y (zelph/var "Y")) (def s (zelph/collection )js" + facts + R"js()) (zelph/fact s "~" "conjunction") (zelph/fact s "=>" (zelph/fact s "noted" x))))js");
            interactive.process(R"js(%(defn two [] (def x (zelph/var "X")) (def y (zelph/var "Y")) (def d (zelph/collection )js" + facts + R"js()) (zelph/fact d "~" "conjunction") (def e (zelph/collection )js" + facts
                                + R"js()) (zelph/fact e "~" "conjunction") (zelph/fact d "=>" (zelph/fact e "noted" x))))js");
        };
        process_lines(interactive, R"(
u p v
u r v
)");
        std::string first;
        std::string second;
        SUBCASE("two conditions")
        {
            define(R"js((zelph/fact x "p" y) (zelph/fact x "r" y))js");
            SUBCASE("the rule over one set first")
            {
                first  = "one";
                second = "two";
            }
            SUBCASE("the rule over two sets first")
            {
                first  = "two";
                second = "one";
            }
        }
        SUBCASE("one condition")
        {
            define(R"js((zelph/fact x "p" y))js");
            SUBCASE("the rule over one set first")
            {
                first  = "one";
                second = "two";
            }
            SUBCASE("the rule over two sets first")
            {
                first  = "two";
                second = "one";
            }
        }
        interactive.process("%(def kept (zelph/dedup-rule " + first + "))");
        collector.clear();
        interactive.process("%(zelph/out (string \"same=\" (= kept (zelph/dedup-rule " + second + "))))");
        CHECK(any_output_contains(collector, "same=false"));
        interactive.process("(L noted X) => (X seen yes)");
        CHECK(rules() == 3);
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S seen O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"u seen yes"}); });
}

// A rule that a rule writes is a term of the rule that writes it: it becomes a
// rule solely when a construction builds it, and the construction then reads
// its conditions as the firing does. Its set of conditions is compared as every
// set of conditions in a term is: as the node it is, and by its members only
// when the new rule's statement made it and neither set is named; a set of one
// condition is not that condition there. Read as conditions at any depth, two
// generators whose rules a rule pattern, a variable bound at a rule's subject
// or the construction tells apart were taken for one rule, and the generator
// rolled back lost what it derived. Each subcase except the first holds data
// for both generators; the first holds only the count, for the reason it gives.
TEST_CASE("rule identity: a rule that a rule writes is read as a term")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        // Sorted: in parallel mode, the order of the answers varies across
        // runs.
        const auto answers = [&](const std::string& query)
        {
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(query);
            std::vector<std::string> result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        std::string first;
        std::string second;
        SUBCASE("named sets two levels down")
        {
            // sa and sb are two terms of the generators, located two levels
            // down, thus the generators are two rules. No fact indicates this:
            // when switched on, both write the identical rule
            // `(a is up) => (S => (x p h))`, S being whichever of the two sets
            // the construction meets first (find_conjunction_set), and that
            // rule's firing puts no rule over S in force: the ground rule it
            // would write is the node its own text holds, which stays a
            // mention. Thus, the count alone holds the reading.
            process_lines(interactive, R"(
(a r d) in sa
(b r e) in sa
sa ~ conjunction
(a r d) in sb
(b r e) in sb
sb ~ conjunction
(G go H) => ((a is up) => (sa => (x p H)))
(G go H) => ((a is up) => (sb => (x p H)))
)");
            CHECK(rules() == 2);
        }
        SUBCASE("a set of one condition and that condition, one level down")
        {
            // The generators ground H, thus the rules they write are ground,
            // and the rule pattern below meets `(x p h) => (y q h)`
            // exclusively. Read as its condition, the Janet generator's set
            // made the typed generator its duplicate, and with the Janet
            // generator entered first `h hit yes` was lost.
            process_lines(interactive, R"(
x p h
g go h
((x p h) => (y q X)) => (X hit yes)
)");
            const std::string janet = R"js(%(let [g (zelph/var "G") h (zelph/var "H") condition (zelph/collection (zelph/fact "x" "p" h))] (zelph/fact condition "~" "conjunction") (zelph/dedup-rule (fn [] (zelph/fact (zelph/fact g "go" h) "=>" (zelph/fact condition "=>" (zelph/fact "y" "q" h)))))))js";
            const std::string typed = "(G go H) => ((x p H) => (y q H))";
            SUBCASE("the Janet generator first")
            {
                first  = janet;
                second = typed;
            }
            SUBCASE("the typed generator first")
            {
                first  = typed;
                second = janet;
            }
            interactive.process(first);
            interactive.process(second);
            interactive.run(true, false, false);
            CHECK(rules() == 5);
            CHECK(answers("S hit O") == std::vector<std::string>{"h hit yes"});
            CHECK(answers("y q O") == std::vector<std::string>{"y q h"});
        }
        SUBCASE("a switch over a set of one condition and over that condition")
        {
            // The two switches hold two terms, a set containing a single
            // condition and that condition itself, thus constituting two
            // rules. Each one puts its rule in force, and the two rules they
            // write are one and the same (ReadAt::OwnSubject), so `e f u` is
            // derived regardless of which switch the identity keeps, and only
            // the count holds the case. While rule_variables read the set via
            // its tag alone, the rule associated with the switch over the set
            // stayed a mention, and with the set spelling entered first, the
            // switch over the condition was rolled back as a duplicate:
            // `e f u` was never derived.
            process_lines(interactive, R"(
k is on
a p b
)");
            const std::string set       = "(K is on) => ({(X p b)} => (e f u))";
            const std::string condition = "(K is on) => ((X p b) => (e f u))";
            SUBCASE("the set first")
            {
                first  = set;
                second = condition;
            }
            SUBCASE("the condition first")
            {
                first  = condition;
                second = set;
            }
            interactive.process(first);
            interactive.process(second);
            interactive.run(true, false, false);
            CHECK(rules() == 3);
            CHECK(answers("e f O") == std::vector<std::string>{"e f u"});
        }
        SUBCASE("a generator over a membership in a set of one condition and over that membership")
        {
            // As above, with membership in a collection the generator binds:
            // with the set spelling entered first, `k ok k` and `z ok k` were
            // never derived. Each generator now derives them alone, so, as
            // above, the counts hold the case. Both generators stay, and they
            // write one rule: the construction rebuilds the set's member at
            // the place of a condition, and a rule over a set consisting of a
            // single condition that holds a variable is that condition
            // (ReadAt::OwnSubject), so the second rule built is the first
            // one's twin. Rebuilt as a term, the set held `(X in @{k z X})`, X
            // still a member of the collection, and its rule stood beside the
            // other generator's.
            interactive.process("k is on");
            const std::string set       = "(K is on) => ({(X in @{K z})} => (X ok K))";
            const std::string condition = "(K is on) => ((X in @{K z}) => (X ok K))";
            SUBCASE("the set first")
            {
                first  = set;
                second = condition;
            }
            SUBCASE("the condition first")
            {
                first  = condition;
                second = set;
            }
            interactive.process(first);
            interactive.process(second);
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            CHECK(count_outputs_containing(collector, "(K is on) =>") == 2);
            CHECK(rules() == 3);
            CHECK(answers("S ok O").size() == 2);
            CHECK(answers_contain(collector, "k ok k"));
            CHECK(answers_contain(collector, "z ok k"));
            CHECK(rules() == 3);
        } });
}

// A condition matches a set constant through its node, and the subject of a
// rule through its members only when both are sets of conditions
// (unify_condition): a set containing a single condition is not equivalent to
// that condition there, and two untagged containers are two distinct nodes.
// Read as conditions, these subjects made the two rules of each subcase into
// one rule, the rule rolled back derived nothing, and behind a negation the
// program derived a wrong fact. The comparison reads every rule within a rule
// as a term, a rule that a rule writes being included (see "a rule that a rule
// writes is read as a term"). Each subcase holds data for both rules, so a rule
// missing in either order of entry becomes apparent.
TEST_CASE("rule identity: a rule that a condition names or quotes is read as a term")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        // Sorted: in parallel mode, the order of the answers varies across
        // runs.
        const auto answers = [&](const std::string& query)
        {
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(query);
            std::vector<std::string> result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        std::string first;
        std::string second;
        SUBCASE("a rule in a set constant, over two named sets of the same members")
        {
            process_lines(interactive, R"(
(a r d) in sa
(b r e) in sa
sa ~ conjunction
(a r d) in sb
(b r e) in sb
sb ~ conjunction
u has {(sb => (e f k))}
w has {(sa => (e f k))}
)");
            const std::string over_sa = "(X has {(sa => (e f k))}) => (X hit yes)";
            const std::string over_sb = "(X has {(sb => (e f k))}) => (X hit yes)";
            SUBCASE("the rule over sa first")
            {
                first  = over_sa;
                second = over_sb;
            }
            SUBCASE("the rule over sb first")
            {
                first  = over_sb;
                second = over_sa;
            }
            interactive.process(first);
            interactive.process(second);
            interactive.process("(X has Y, ¬(X hit yes)) => (X miss yes)");
            CHECK(rules() == 3);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes", "w hit yes"});
            collector.clear();
            interactive.process("S miss O");
            CHECK(collect_answers(collector).empty());
        }
        SUBCASE("a set of one condition and that condition, in a rule a condition names")
        {
            const std::string set = R"js(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def c (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact c "~" "conjunction") (zelph/fact (zelph/fact c "=>" (zelph/fact "e" "f" x)) "=>" (zelph/fact x "hit" "yes")))) )js";
            const std::string condition = "((a p b) => (e f X)) => (X hit yes)";
            SUBCASE("the set first")
            {
                first  = set;
                second = condition;
            }
            SUBCASE("the condition first")
            {
                first  = condition;
                second = set;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"js(
(a p b) => (e f u)
%(let [c (zelph/collection (zelph/fact "a" "p" "b"))] (zelph/fact c "~" "conjunction") (zelph/fact c "=>" (zelph/fact "e" "f" "w")))
)js");
            CHECK(rules() == 4);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes", "w hit yes"});
        }
        SUBCASE("a set of one condition and that condition, in a rule a condition quotes")
        {
            const std::string set = R"js(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def c (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact c "~" "conjunction") (zelph/fact (zelph/fact (zelph/fact c "=>" (zelph/fact "e" "f" x)) "noted" x) "=>" (zelph/fact x "hit" "yes")))) )js";
            const std::string condition = "(((a p b) => (e f X)) noted X) => (X hit yes)";
            SUBCASE("the set first")
            {
                first  = set;
                second = condition;
            }
            SUBCASE("the condition first")
            {
                first  = condition;
                second = set;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"js(
((a p b) => (e f u)) noted u
%(let [c (zelph/collection (zelph/fact "a" "p" "b"))] (zelph/fact c "~" "conjunction") (zelph/fact (zelph/fact c "=>" (zelph/fact "e" "f" "w")) "noted" "w"))
)js");
            CHECK(rules() == 2);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes", "w hit yes"});
        }
        SUBCASE("two untagged containers of the same members, in a rule a condition quotes")
        {
            process_lines(interactive, R"js(
%(def c1 (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "q" "d")))
%(def c2 (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "q" "d")))
)js");
            const std::string over_c1 = R"js(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (zelph/fact (zelph/fact (zelph/fact c1 "=>" (zelph/fact "e" "f" x)) "noted" x) "=>" (zelph/fact x "hit" "yes")))) )js";
            const std::string over_c2 = R"js(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (zelph/fact (zelph/fact (zelph/fact c2 "=>" (zelph/fact "e" "f" x)) "noted" x) "=>" (zelph/fact x "hit" "yes")))) )js";
            SUBCASE("the rule over c1 first")
            {
                first  = over_c1;
                second = over_c2;
            }
            SUBCASE("the rule over c2 first")
            {
                first  = over_c2;
                second = over_c1;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"js(
%(zelph/fact (zelph/fact c2 "=>" (zelph/fact "e" "f" "u")) "noted" "u")
%(zelph/fact (zelph/fact c1 "=>" (zelph/fact "e" "f" "w")) "noted" "w")
)js");
            CHECK(rules() == 2);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes", "w hit yes"});
        } });
}

// When the reasoner matches a set of conditions through its members, it pairs
// them just as it does with any term (unify_members): a member named set is the
// node it is, and a set containing a single condition is not equivalent to that
// condition. Read as conditions there, the members made the two rules of each
// subcase into a single rule, and the rule rolled back derived nothing. Each
// subcase holds data for both rules.
TEST_CASE("rule identity: the members of a set of conditions in a term are read as terms")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        // Sorted: in parallel mode, the order of the answers varies across
        // runs.
        const auto answers = [&](const std::string& query)
        {
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(query);
            std::vector<std::string> result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        const char* const named = R"(
(c q d) in n1
(g h i) in n1
n1 ~ conjunction
(c q d) in n2
(g h i) in n2
n2 ~ conjunction
)";
        std::string first;
        std::string second;
        SUBCASE("a named set among the conditions of a rule a condition names")
        {
            process_lines(interactive, named);
            const std::string over_n1 = "((a p b, n1) => (e f X)) => (X hit yes)";
            const std::string over_n2 = "((a p b, n2) => (e f X)) => (X hit yes)";
            SUBCASE("the rule over n1 first")
            {
                first  = over_n1;
                second = over_n2;
            }
            SUBCASE("the rule over n2 first")
            {
                first  = over_n2;
                second = over_n1;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"(
(a p b, n2) => (e f u)
(a p b, n1) => (e f w)
)");
            CHECK(rules() == 4);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes", "w hit yes"});
        }
        SUBCASE("a named set among the conditions of a rule a condition quotes")
        {
            process_lines(interactive, named);
            const std::string over_n1 = "(((a p b, n1) => (e f X)) noted X) => (X hit yes)";
            const std::string over_n2 = "(((a p b, n2) => (e f X)) noted X) => (X hit yes)";
            SUBCASE("the rule over n1 first")
            {
                first  = over_n1;
                second = over_n2;
            }
            SUBCASE("the rule over n2 first")
            {
                first  = over_n2;
                second = over_n1;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"(
((a p b, n2) => (e f u)) noted u
((a p b, n1) => (e f w)) noted w
)");
            CHECK(rules() == 2);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes", "w hit yes"});
        }
        SUBCASE("a named set among the conditions of a rule quoted in a consequence")
        {
            process_lines(interactive, named);
            const std::string over_n1 = "(X s Y) => (X noted ((a p b, n1) => (e f k)))";
            const std::string over_n2 = "(X s Y) => (X noted ((a p b, n2) => (e f k)))";
            SUBCASE("the rule over n1 first")
            {
                first  = over_n1;
                second = over_n2;
            }
            SUBCASE("the rule over n2 first")
            {
                first  = over_n2;
                second = over_n1;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"(
u s v
(X noted ((a p b, n1) => (e f k))) => (X hit n1)
(X noted ((a p b, n2) => (e f k))) => (X hit n2)
)");
            CHECK(rules() == 4);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit n1", "u hit n2"});
        }
        SUBCASE("a set of one condition among the conditions of a rule a condition names")
        {
            process_lines(interactive, R"js(
%(def inner (zelph/collection (zelph/fact "c" "q" "d")))
%(zelph/fact inner "~" "conjunction")
)js");
            const std::string set = R"js(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def s (zelph/collection (zelph/fact "a" "p" "b") inner)) (zelph/fact s "~" "conjunction") (zelph/fact (zelph/fact s "=>" (zelph/fact "e" "f" x)) "=>" (zelph/fact x "hit" "yes")))) )js";
            const std::string condition = "((a p b, c q d) => (e f X)) => (X hit yes)";
            SUBCASE("the set first")
            {
                first  = set;
                second = condition;
            }
            SUBCASE("the condition first")
            {
                first  = condition;
                second = set;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"js(
(a p b, c q d) => (e f u)
%(let [s (zelph/collection (zelph/fact "a" "p" "b") inner)] (zelph/fact s "~" "conjunction") (zelph/fact s "=>" (zelph/fact "e" "f" "w")))
)js");
            CHECK(rules() == 4);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes", "w hit yes"});
        } });
}

// A rule's own set of conditions is compared just like any other set of
// conditions: as the node it is, and through its members only when the new
// rule's statement made it and neither set bears a name, each set standing for
// one set of the other rule. The firing reads the set via its members, yet three
// additional readers see the node: a rule pattern pairs the members of a rule's
// set by node (unify_members), a variable bound at the subject of a ground rule
// binds the set itself, and the membership of a ground member answers a query.
// Read as the firing reads it, the set made one rule from two rules these
// readers distinguish, and the rule rolled back lost what it had derived. Each
// subcase holds data for both rules and includes an observer for the reader that
// tells them apart, so a rule missing in either entry order becomes apparent.
TEST_CASE("rule identity: a rule's own set of conditions is a term, and so are its members")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        // Sorted: in parallel mode, the order of the answers varies across
        // runs.
        const auto answers = [&](const std::string& query)
        {
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(query);
            std::vector<std::string> result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        const char* const named = R"(
(c q d) in n1
(g h i) in n1
n1 ~ conjunction
(c q d) in n2
(g h i) in n2
n2 ~ conjunction
)";
        std::string first;
        std::string second;
        SUBCASE("a named set among the rule's own conditions")
        {
            // The rule pattern meets the members of the rule's set by node, and
            // the membership of n2 within the set answers a query, so both
            // observers meet the rule over n2 exclusively. Read by its members,
            // n2 was n1: the rule over n2, entered after the rule over n1, was
            // rolled back, and neither observer derived anything.
            process_lines(interactive, named);
            process_lines(interactive, R"(
((a p b, n2) => (e f X)) => (X quoted yes)
(S => (e f X), n2 in S) => (X held yes)
)");
            const std::string over_n1 = "(a p b, n1) => (e f u)";
            const std::string over_n2 = "(a p b, n2) => (e f u)";
            SUBCASE("the rule over n1 first")
            {
                first  = over_n1;
                second = over_n2;
            }
            SUBCASE("the rule over n2 first")
            {
                first  = over_n2;
                second = over_n1;
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(rules() == 4);
            CHECK(answers("S quoted O") == std::vector<std::string>{"u quoted yes"});
            CHECK(answers("S held O") == std::vector<std::string>{"u held yes"});
        }
        SUBCASE("a named set as the whole of the rule's conditions")
        {
            // A variable bound at the subject of a ground rule binds the node
            // that stands there, so the observer notes n1 and n2 as separate.
            // Read through their members, the two sets made the second rule a
            // duplicate of the first, and what the observer had noted about
            // it was lost.
            process_lines(interactive, named);
            interactive.process("(S => (e f X)) => (S noted X)");
            SUBCASE("the rule over n1 first")
            {
                first  = "n1 => (e f u)";
                second = "n2 => (e f u)";
            }
            SUBCASE("the rule over n2 first")
            {
                first  = "n2 => (e f u)";
                second = "n1 => (e f u)";
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(rules() == 3);
            CHECK(answers("S noted O") == std::vector<std::string>{"n1 noted u", "n2 noted u"});
        }
        SUBCASE("two untagged collections of the same members as the whole of the rule's conditions")
        {
            // An untagged collection of several conditions created by a
            // program is not a set of conditions: a rule over it never fires,
            // and `.list-rules` does not list it, so only the answer is
            // counted. A rule pattern meets it by node. Read via their
            // members, c1 and c2 made the rule over c2 the duplicate of the
            // rule over c1, and the observer over c2 derived nothing. Only a
            // program makes such a collection.
            process_lines(interactive, R"js(
%(def c1 (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "q" "d")))
%(def c2 (zelph/collection (zelph/fact "a" "p" "b") (zelph/fact "c" "q" "d")))
%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (zelph/fact (zelph/fact c2 "=>" (zelph/fact "e" "f" x)) "=>" (zelph/fact x "hit" "yes"))))
)js");
            const std::string over_c1 = R"js(%(zelph/dedup-rule (fn [] (zelph/fact c1 "=>" (zelph/fact "e" "f" "u")))))js";
            const std::string over_c2 = R"js(%(zelph/dedup-rule (fn [] (zelph/fact c2 "=>" (zelph/fact "e" "f" "u")))))js";
            SUBCASE("the rule over c1 first")
            {
                first  = over_c1;
                second = over_c2;
            }
            SUBCASE("the rule over c2 first")
            {
                first  = over_c2;
                second = over_c1;
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("a typed twin entered before a generator stays beside the generator's rule")
        {
            // A construction does not regard any set it made as the new rule's
            // own (NewRuleNodes): it might have found the set via its members.
            // Thus, the rule a generator builds over its own set of conditions
            // is not a twin of the typed rule entered before it, as it would
            // not be a twin if the set were positioned within a term. Typed
            // after the generator, the typed rule's set belongs to the rule's
            // statement itself, and the typed rule is recognized.
            process_lines(interactive, R"(
k is on
u p k
u r k
)");
            const std::string typed     = "(X p k, X r k) => (X s k)";
            const std::string generator = "(K is on) => ((X p K, X r K) => (X s K))";
            std::size_t       expected  = 0;
            SUBCASE("typed before")
            {
                interactive.process(typed);
                interactive.process(generator);
                interactive.run(true, false, false);
                expected = 3;
            }
            SUBCASE("typed after")
            {
                interactive.process(generator);
                interactive.run(true, false, false);
                interactive.process(typed);
                expected = 2;
            }
            CHECK(rules() == expected);
            CHECK(answers("S s O") == std::vector<std::string>{"u s k"});
        }
        SUBCASE("a set of ground conditions written out and its comma list are two rules")
        {
            // `(*{(a p b) (c q d)} ~ conjunction)` builds a set of ground
            // conditions as a set constant, and the comma list of those
            // identical conditions builds a collection that the rule's
            // statement makes. The firing reads both through their members,
            // yet the variable bound at the subject of a ground rule binds
            // either one node or the other; the condition
            // `{(a p b) (c q d)} noted V` names the set constant, and the
            // rule pattern whose subject is a set pattern meets exclusively
            // the rule over the set constant (unify_nodes matches a set
            // pattern only against a set constant). Read through its members,
            // the set constant was the comma list: the rule over it, entered
            // after the rule over the comma list, was rolled back as its
            // twin, and `u via yes` and `u hit yes` were lost. The observers
            // follow the rules, because the set constant is hash-consed:
            // constructed before them, by the observer that names it, it was
            // not a node the statement made, and the rules stayed two.
            const std::string constant = "(*{(a p b) (c q d)} ~ conjunction) => (e f u)";
            const std::string comma    = "(a p b, c q d) => (e f u)";
            SUBCASE("the set constant first")
            {
                first  = constant;
                second = comma;
            }
            SUBCASE("the comma list first")
            {
                first  = comma;
                second = constant;
            }
            SUBCASE("the comma list first, with the fact stores off")
            {
                // Without the store that says whether a node holds a variable
                // (variables_in), the identity performs a walk over the set
                // constant, which returns the same result as the store would.
                interactive.process(".fact-stores off");
                first  = comma;
                second = constant;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"(
(S => (e f X)) => (S noted X)
({(a p b) (c q d)} noted V) => (V via yes)
({(A p B) (C q D)} => (e f X)) => (X hit yes)
a p b
c q d
)");
            CHECK(rules() == 5);
            CHECK(answers("e f O") == std::vector<std::string>{"e f u"});
            CHECK(answers("S via O") == std::vector<std::string>{"u via yes"});
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes"});
        }
        SUBCASE("a rule over a set of ground conditions written out prints as the line that builds it")
        {
            // The rule over the set constant prints in the explicit form,
            // which builds the identical set constant and thus the same
            // rule. When printed as the comma list, the line would build a
            // collection, and with it a second rule beside the first,
            // because the identity reads the set constant through its node.
            // The rule over a comma list prints as the comma list, which
            // builds a collection that the identity compares by its members.
            // Each line printed and re-entered preserves the rules
            // unchanged.
            process_lines(interactive, R"(
(*{(a p b) (c q d)} ~ conjunction) => (e f u)
(a p b, c q d) => (e f v)
)");
            collector.clear();
            interactive.process(".list-rules");
            std::vector<std::string> printed;
            for (const auto& e : collector.events())
                if (e.text.find("=>") != std::string::npos) printed.push_back(normalize(e.text));
            std::ranges::sort(printed);
            CHECK(printed == std::vector<std::string>{"((c q d), (a p b)) => (e f v)", "(*{(c q d) (a p b)} ~ conjunction) => (e f u)"});
            for (const std::string& line : printed)
                interactive.process(line);
            CHECK(rules() == 2);
        }
        SUBCASE("a rule over a collection of one ground condition prints as the line that builds it")
        {
            // A program builds a rule with a single condition over a tagged
            // collection of that condition (janet.md), and the typed
            // `(*@{(a p b)} ~ conjunction)` builds the identical result. The
            // brace in the explicit form builds a collection solely from a
            // condition that holds a variable; from a ground one it builds
            // the set constant of the condition, another node, which the
            // identity reads by its node (two subcases above). Printed using
            // the brace, as `(*{(a p b)} ~ conjunction) => (e f u)`, the rule
            // over the collection re-entered as a second rule beside itself,
            // which fired in the same manner. It prints using the
            // collection's marker, and each printed line re-enters, leaving
            // the rules unchanged.
            const auto printed_rules = [&]
            {
                collector.clear();
                interactive.process(".list-rules");
                std::vector<std::string> printed;
                for (const auto& e : collector.events())
                    if (e.text.find("=>") != std::string::npos) printed.push_back(normalize(e.text));
                std::ranges::sort(printed);
                return printed;
            };
            const std::string marked = "(*@{(a p b)} ~ conjunction) => (e f u)";
            const std::string braced = "(*{(a p b)} ~ conjunction) => (e f u)";
            SUBCASE("built by a program")
            {
                interactive.process(R"(%(zelph/dedup-rule (fn [] (let [c (zelph/collection (zelph/fact "a" "p" "b"))] (zelph/fact c "~" "conjunction") (zelph/fact c "=>" (zelph/fact "e" "f" "u"))))))");
            }
            SUBCASE("typed")
            {
                interactive.process(marked);
            }
            CHECK(printed_rules() == std::vector<std::string>{marked});
            interactive.process(marked);
            CHECK(rules() == 1);

            // The spelling using the brace builds the rule over the set
            // constant, a standalone rule, which prints with the brace:
            // each of the two printed lines is the rule that printed it.
            interactive.process(braced);
            const std::vector<std::string> printed = printed_rules();
            CHECK(printed == std::vector<std::string>{marked, braced});
            for (const std::string& line : printed)
                interactive.process(line);
            CHECK(rules() == 2);

            interactive.process("a p b");
            CHECK(answers("e f O") == std::vector<std::string>{"e f u"});
        }
        SUBCASE("a rule that writes a rule, over a collection of one ground condition, prints as the line that builds it")
        {
            // The shape of a conditional where the condition is a ground fact
            // and the consequence is a rule containing a single condition: both
            // rules stand over a tagged collection consisting of one condition.
            // The outer condition is ground and prints using the marker; the
            // inner one holds a variable, and its brace builds a collection, so
            // it keeps the brace. Printed with the brace at both places, the
            // line re-entered as a second generator over the set constant of the
            // outer condition.
            interactive.process(R"(%(zelph/dedup-rule (fn [] (def x (zelph/var "X")) (def outer (zelph/collection (zelph/fact "a" "p" "b"))) (zelph/fact outer "~" "conjunction") (def inner (zelph/collection (zelph/fact x "~" "man"))) (zelph/fact inner "~" "conjunction") (zelph/fact outer "=>" (zelph/fact inner "=>" (zelph/fact x "sleeps" "nil"))))))");
            const std::string generator = "(*@{(a p b)} ~ conjunction) => ((*{(X ~ man)} ~ conjunction) => (X sleeps nil))";
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, generator));
            interactive.process(generator);
            CHECK(rules() == 1);

            process_lines(interactive, R"(
Bill ~ man
a p b
)");
            CHECK(answers("S sleeps nil") == std::vector<std::string>{"Bill sleeps nil"});
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
            CHECK(any_output_contains(collector, generator));
        }
        SUBCASE("a rule that a generator writes over a collection of one condition its binding makes ground prints as the line that builds it")
        {
            // A construction rebuilds a tagged set consisting of a single
            // condition as a collection under the binding, where typing the
            // instance with the brace builds the set constant of the ground
            // condition. The written rule prints with the marker, and the
            // printed line, typed, is the rule the generator wrote.
            process_lines(interactive, R"(
(K is on) => ((*{(a p K)} ~ conjunction) => (x r K))
k1 is on
)");
            interactive.run(true, false, false);
            const std::string written = "(*@{(a p k1)} ~ conjunction) => (x r k1)";
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
            CHECK(any_output_contains(collector, written));
            interactive.process(written);
            CHECK(rules() == 2);

            interactive.process("a p k1");
            CHECK(answers("x r O") == std::vector<std::string>{"x r k1"});
        }
        SUBCASE("a set of ground conditions written out and its comma list, in a rule a condition names")
        {
            // The same two spellings at the subject of a rule that a condition
            // names, which is a term. A condition matches two sets of
            // conditions via their members (unify_condition), so each observer
            // meets the rules over every spelling. The identity reads the set
            // constant by its node there as well, just as it does wherever the
            // set constant stands, and the two observers stay two rules that
            // derive alike: a duplicate, which loses nothing.
            const std::string over_constant = "((*{(a p b) (c q d)} ~ conjunction) => (e f X)) => (X hit yes)";
            const std::string over_comma    = "(((c q d), (a p b)) => (e f X)) => (X hit yes)";
            SUBCASE("the set constant first")
            {
                first  = over_constant;
                second = over_comma;
            }
            SUBCASE("the comma list first")
            {
                first  = over_comma;
                second = over_constant;
            }
            interactive.process(first);
            interactive.process(second);
            process_lines(interactive, R"(
(a p b, c q d) => (e f u)
(*{(a p b) (c q d)} ~ conjunction) => (e f w)
(c q d, a p b) => (e f v)
)");
            CHECK(rules() == 5);
            CHECK(answers("S hit O") == std::vector<std::string>{"u hit yes", "v hit yes", "w hit yes"});
        } });
}

TEST_CASE("rule identity: a set of conditions with one member is that condition")
{
    // A rule written in Janet, in the manner that janet.md builds one, holds
    // a tagged condition set consisting of a single member, and the engine
    // reads it as that specific condition. `.list-rules` prints it as
    // `(*{(X p Y)} ~ conjunction) => ...`; previously, it was printed as
    // `((X p Y)) => ...`, which the parser reads as the single condition
    // `(X p Y)` without a set. Read as two distinct rules, that printed line
    // re-entered as a second rule, and likewise the plain one. The line it
    // now prints, the line it previously printed, and the plain one are all
    // that rule. The condition holds a variable, so no reader other than the
    // firing sees the set, except for its tag (see the next test case).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(let [condition (zelph/collection (zelph/fact 'X "p" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/fact "k" "q" 'Y)))))");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(*{(X p Y)} ~ conjunction) => (X likes (k q Y))"));
        interactive.process("(*{(X p Y)} ~ conjunction) => (X likes (k q Y))");
        interactive.process("((X p Y)) => (X likes (k q Y))");
        interactive.process("(X p Y) => (X likes (k q Y))");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);

        interactive.process("a p m");
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector) == std::vector<std::string>{"a likes (k q m)"}); });
}

// At a rule's own subject, a set containing a single condition reads as that
// condition where only the firing reads the set, apart from a tag it carries:
// its condition holds a variable, or is a rule's set holding one, and the set
// does not appear elsewhere in the rule (read_container, ReadAt::OwnSubject). A
// rule holding a variable is not eligible for a rule pattern, nor for a
// variable bound at a rule's subject, and the membership of a member holding a
// variable answers nothing, so the untagged spellings derive alike across every
// reader. The tag of the tagged spelling functions as a reader too, since it
// answers a query: what a rule over the tag derives from that spelling is lost
// when it is entered after another (read_container), and no subcase here holds
// a rule over the tag. When the condition is ground, three readers tell a set
// of it from the condition, and the two spellings represent two rules that fire
// alike: read as its condition, the set made them into one rule, and in either
// order of entry a fact over named atoms was lost. Each subcase holds data for
// both rules.
TEST_CASE("rule identity: a set of one condition at a rule's own subject is that condition only where the firing alone reads it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        // Sorted: in parallel mode, the order of the answers varies across
        // runs.
        const auto answers = [&](const std::string& query)
        {
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(query);
            std::vector<std::string> result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        std::string first;
        std::string second;
        SUBCASE("a collection of one ground condition and that condition")
        {
            // The rule pattern quoting `(a p b) => (e f X)` meets the bare
            // rule exclusively, the variable bound at the subject binds
            // either `(a p b)` or the collection, and the membership of
            // `(a p b)` within the collection answers a query: each of the
            // three observers below derives its fact from one of the two
            // rules alone.
            process_lines(interactive, R"(
a p b
((a p b) => (e f X)) => (X quoted yes)
(S => (e f X)) => (S noted X)
((a p b) noted X) => (X via yes)
((a p b) in S, S => (e f X)) => (X held yes)
)");
            const std::string set       = "@{(a p b)} => (e f u)";
            const std::string condition = "(a p b) => (e f u)";
            SUBCASE("the collection first")
            {
                first  = set;
                second = condition;
            }
            SUBCASE("the condition first")
            {
                first  = condition;
                second = set;
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(rules() == 6);
            CHECK(answers("S quoted O") == std::vector<std::string>{"u quoted yes"});
            CHECK(answers("S via O") == std::vector<std::string>{"u via yes"});
            CHECK(answers("S held O") == std::vector<std::string>{"u held yes"});
        }
        SUBCASE("a set constant of one ground condition and that condition")
        {
            // Written as a set constant, a ground set consisting of a single
            // condition also functions as an independent rule alongside the
            // bare rule: the rule pattern meets only the bare rule.
            process_lines(interactive, R"(
a p b
((a p b) => (c q X)) => (X hit yes)
)");
            const std::string set       = "{(a p b)} => (c q d)";
            const std::string condition = "(a p b) => (c q d)";
            SUBCASE("the set constant first")
            {
                first  = set;
                second = condition;
            }
            SUBCASE("the condition first")
            {
                first  = condition;
                second = set;
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(rules() == 3);
            CHECK(answers("S hit O") == std::vector<std::string>{"d hit yes"});
        }
        SUBCASE("the four spellings of one condition with a variable are one rule")
        {
            // The spellings documented in logic.md: the condition holds a
            // variable, so only the firing reads the set, apart from the tag
            // of the tagged spelling, and it reads the set as its condition.
            // Read as sets, just like every other set of conditions, the
            // spellings containing a set would stand beside the plain one as
            // independent rules. Nothing here reads the tag; a rule over it
            // would derive nothing from the tagged spelling in the order where
            // that spelling comes last.
            interactive.process("a p b");
            const std::vector<std::string> plain_first  = {"(X p Y) => (X q Y)", "{(X p Y)} => (X q Y)", "*{(X p Y)} => (X q Y)", "(*{(X p Y)} ~ conjunction) => (X q Y)"};
            const std::vector<std::string> tagged_first = {"(*{(X p Y)} ~ conjunction) => (X q Y)", "{(X p Y)} => (X q Y)", "*{(X p Y)} => (X q Y)", "(X p Y) => (X q Y)"};
            std::vector<std::string>       spellings;
            SUBCASE("the plain spelling first")
            {
                spellings = plain_first;
            }
            SUBCASE("the tagged spelling first")
            {
                spellings = tagged_first;
            }
            for (const std::string& spelling : spellings)
                interactive.process(spelling);
            CHECK(rules() == 1);
            CHECK(answers("S q O") == std::vector<std::string>{"a q b"});
        }
        SUBCASE("a set of one set of conditions and that set")
        {
            // The single member constitutes a rule's set of conditions holding
            // a variable, whose membership answers nothing
            // (is_engine_bookkeeping in unification.cpp), so the firing alone
            // reads the outer set, and it reads it as the inner one.
            process_lines(interactive, R"(
a p b
a r b
)");
            const std::string set        = "{(X p Y, X r Y)} => (X q Y)";
            const std::string conditions = "(X p Y, X r Y) => (X q Y)";
            SUBCASE("the outer set first")
            {
                first  = set;
                second = conditions;
            }
            SUBCASE("the comma list first")
            {
                first  = conditions;
                second = set;
            }
            interactive.process(first);
            interactive.process(second);
            CHECK(rules() == 1);
            CHECK(answers("S q O") == std::vector<std::string>{"a q b"});
        } });
}

TEST_CASE("rule identity: the rollback leaves no debris")
{
    // The duplicate is built before it can be recognised, so the check has
    // to undo the construction -- patterns, conjunction set, AND the
    // variables they are made of. Anything left behind would show up as
    // graph growth for a statement that changed nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        collector.clear();
        interactive.process(".stat");
        const std::string before = node_count(collector);
        REQUIRE_FALSE(before.empty());

        interactive.process("(R is transitive, A R B, B R C) => (A R C)");
        collector.clear();
        interactive.process(".stat");
        CHECK(node_count(collector) == before); });
}

TEST_CASE("rule identity: the rollback of a rule with the focus operator leaves no debris")
{
    // With a focus, the duplicate is rolled back solely after the part of the
    // kept rule has been retrieved (network::corresponding_part), meaning the
    // lookup runs while two copies of the rule exist within the graph. Whatever
    // it builds must not persist beyond the rollback.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto twice = [&](const std::string& first, const std::string& second)
        {
            interactive.process(first);
            collector.clear();
            interactive.process(".stat");
            const std::string before = node_count(collector);
            REQUIRE_FALSE(before.empty());

            interactive.process(second);
            collector.clear();
            interactive.process(".stat");
            CHECK(node_count(collector) == before);
        };

        SUBCASE("the condition set")
        {
            twice("*{(X p Y)} => (X q Y)", "*{(A p B)} => (A q B)");
        }
        SUBCASE("a consequence among several")
        {
            twice("(X p Y) => *(X q Y) (Y q X)", "(A p B) => (B q A) *(A q B)");
        } });
}

TEST_CASE("rule identity: different rules stay different")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a different predicate")
        {
            interactive.process("(X p Y) => (X q Y)");
            interactive.process("(X p Y) => (X r Y)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a different sharing pattern between the variables")
        {
            // Same shape, different rule: the first joins on Y, the second
            // does not. A fingerprint alone cannot tell them apart -- only
            // the bijection can.
            interactive.process("(X p Y, Y p Z) => (X q Z)");
            interactive.process("(X p Y, Z p W) => (X q W)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a single-condition rule is not a set")
        {
            // Two rules with ONE condition each and the same consequence
            // SHAPE. Assuming the => subject is always a condition set
            // makes both look like "{} => (v lcmp v)" and collapses them --
            // which silently deleted a recursion rule of common-arithmetic.
            interactive.process("((A cons R) lcmp (B cons S)) => (R lcmp S)");
            interactive.process("(N cmp M) => (N lcmp M)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("negation is part of the rule")
        {
            interactive.process("(A is yellow, ¬(A is green)) => (A mark yellow)");
            interactive.process("(A is yellow, A is green) => (A mark yellow)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "¬"));
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("negation is part of a rule nested in a rule")
        {
            // Once derived, the inner rule becomes a rule, so its condition
            // is compared as a condition. When the negation was attached to
            // the pattern, the subject of a standard fact was compared
            // without that negation, and the second statement was discarded
            // due to being a duplicate of the first; the negated condition
            // stands as an independent node.
            interactive.process("(G go H) => (¬(X p H) => (X q H))");
            interactive.process("(G go H) => ((X p H) => (X q H))");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "¬"));
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("negation is part of a conjunction nested in a rule")
        {
            // The same applies to a member of the inner rule's
            // conjunction, compared by its members as conditions.
            interactive.process("(G go H) => (((X q H), ¬(X p H)) => !)");
            interactive.process("(G go H) => (((X q H), (X p H)) => !)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "¬"));
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a set constant around a collection is not a collection around it")
        {
            // A set constant featuring a rule's own collection beneath it
            // is compared by its members, just as the collection itself is,
            // and `{@{c}}` along with `@{@{c}}` possess identical members.
            // They remain two distinct types of term, and the two rules
            // derive two different facts.
            interactive.process("(X p Y) => (X likes {@{c}})");
            interactive.process("(X p Y) => (X likes @{@{c}})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);

            interactive.process("b p d");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("b likes S");
            CHECK(answers_contain(collector, "b likes {@{c}}"));
            CHECK(answers_contain(collector, "b likes @{@{c}}"));
        }
        SUBCASE("a != guard is part of the rule")
        {
            interactive.process("(A p X, A p Y, X != Y) => (A pair X Y)");
            interactive.process("(A p X, A p Y) => (A pair X Y)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        } });
}

TEST_CASE("rule identity: a rule built inside a cluster stays in that cluster")
{
    // The check runs the construction in a scratch cluster of its own. What
    // survives has to be handed back to the cluster the user activated,
    // otherwise .cluster-drop would no longer roll the rule back.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".cluster exp");
        interactive.process("(X cp Y) => (X cq Y)");
        interactive.process(".cluster-drop exp");
        collector.clear();
        interactive.process(".list-rules");
        CHECK_FALSE(any_output_contains(collector, "(X cq Y)")); });
}

TEST_CASE("rule identity: a rule is not built while another one is being built")
{
    // zelph/dedup-rule builds its rule within a scratch cluster and marks
    // the ground patterns that the scratch recorded. A second build nested
    // within the first one activated the same scratch, merged it into itself
    // upon completion, and lost the recordings made by the outer build, so
    // the outer rule's patterns were never marked. It is rejected before
    // activating anything, and the outer build hands the scratch back as it
    // does in response to any error.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        CHECK_THROWS_WITH(interactive.process(R"js(%(zelph/dedup-rule (fn [] (zelph/dedup-rule (fn [] (zelph/fact (zelph/fact 'X "p" 'Y) "=>" (zelph/fact 'X "q" 'Y)))))))js"),
                          doctest::Contains("a rule cannot be built while another rule is being built"));

        collector.clear();
        interactive.process(R"js(%(zelph/out (string "active=" (or (zelph/cluster) "none"))))js");
        CHECK(any_output_contains(collector, "active=none")); });
}

TEST_CASE("rule identity: talking about a rule does not assert it")
{
    // A fact node exists exactly when its edges exist, and the edges of
    // `((X p Y) => (X q Y)) is nice` include those of the rule it mentions.
    // So the rule FIRED: entering `a p b` derived `a q b`, although nobody
    // had claimed the rule -- only that it is nice. Statements about
    // statements are what zelph leads with, and this made a statement about
    // a RULE impossible to write.
    //
    // An asserted rule is a part of nothing; a mentioned one is the subject,
    // the predicate or an object of the statement that mentions it. That is
    // decidable from the graph, so nothing has to be remembered and a
    // save/load round trip cannot lose it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("as the subject of an ordinary fact")
        {
            interactive.process("((X p Y) => (X q Y)) is nice");
            collector.clear();
            interactive.process(".list-rules");
            CHECK_FALSE(any_output_contains(collector, "=> (X q Y)"));

            interactive.process("a p b");
            collector.clear();
            interactive.process("a q Z");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("as the conclusion of another rule")
        {
            // The inner rule must not hold for EVERY predicate just because
            // the outer rule mentions it -- `foo` was never declared
            // transitive.
            interactive.process("(R is transitive) => ((X R Y, Y R Z) => (X R Z))");
            process_lines(interactive, R"(
a foo b
b foo c
)");
            collector.clear();
            interactive.process("a foo Z");
            CHECK(answers_contain(collector, "a foo b"));
            CHECK_FALSE(any_output_contains(collector, "a foo c"));
        }
        SUBCASE("a rule that was actually asserted still fires")
        {
            interactive.process("(X p Y) => (X q Y)");
            interactive.process("a p b");
            collector.clear();
            interactive.process("a q Z");
            CHECK(answers_contain(collector, "a q b"));
        }
        SUBCASE("mentioning a rule with variables leaves the asserted one alone")
        {
            // Each statement names its own variables, so the mentioned rule
            // is a different node from the asserted one and both keep their
            // meaning.
            interactive.process("(X p Y) => (X q Y)");
            interactive.process("((X p Y) => (X q Y)) is nice");
            interactive.process("a p b");
            collector.clear();
            interactive.process("a q Z");
            CHECK(answers_contain(collector, "a q b"));
        }
        SUBCASE("a statement about a rule with variables is structure, not data")
        {
            // The statement is stored, yet no condition ever matches it:
            // Unification::extract_bindings dismisses each fact containing a
            // variable anywhere inside it as a rule template, since binding a
            // pattern's variable to a rule's own variable would build
            // partially instantiated, unusable nodes. Consequently, neither a
            // query nor a rule perceives the information previously stated
            // about the rule, and logic.md names this as the sole deviation
            // from "rules are data". Adopting HiLog-style quoting of rules
            // would introduce a capability designed to intentionally modify
            // this test.
            interactive.process("((X p Y) => (X q Y)) is nice");
            interactive.process("(R is nice) => (R flagged yes)");
            interactive.process(".run");
            collector.clear();
            interactive.process("S is nice");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
            collector.clear();
            interactive.process("S flagged yes");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("a statement about an ordinary fact is data")
        {
            // The control for the case above: the same statement concerning a
            // fact, devoid of variables, answers the query and fires the
            // rule, thus the exclusion of the rule stems from its variables,
            // not from the nesting structure.
            interactive.process("(a p b) is nice");
            interactive.process("(R is nice) => (R flagged yes)");
            interactive.process(".run");
            collector.clear();
            interactive.process("S is nice");
            CHECK(answers_contain(collector, "(a p b) is nice"));
            collector.clear();
            interactive.process("S flagged yes");
            CHECK(answers_contain(collector, "(a p b) flagged yes"));
        } });
}

TEST_CASE("rule identity: zelph/rules lists the rules in force with their parts")
{
    // A front end that rejects a question when a rule currently in force
    // could derive what the question supposes has to read the rules --
    // including their negated conditions and guards. A query using `=>`
    // finds only ground rules, as any rule text containing a variable can
    // never serve as a match candidate, and .list-rules outputs the rule
    // text. zelph/rules returns the rules in force as data, with conditions
    // sorted by kind exactly as the engine itself reads them. Auto-run is
    // disabled, meaning that adding a rule runs nothing.
    static const std::string parts =
        R"js(%(defn parts [sub] (def r (find |(string/find sub (string ($ :rule))) (zelph/rules))) (zelph/out (string sub " -> " (string/join (map (fn [k] (string k "=" (string/join (map string (r k)) "; "))) [:conditions :negated :guards :paths :neural :heads]) " ")))))js";
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".auto-run");
        interactive.process(parts);
        SUBCASE("a rule with a variable")
        {
            interactive.process("(X p Y) => (X q Y)");
            collector.clear();
            interactive.process(R"js(%(parts "(X q Y)"))js");
            CHECK(any_output_contains(collector, "(X q Y) -> conditions=X p Y negated= guards= paths= neural= heads=X q Y"));
        }
        SUBCASE("a ground rule")
        {
            interactive.process("(a p b) => (c q d)");
            collector.clear();
            interactive.process(R"js(%(parts "(c q d)"))js");
            CHECK(any_output_contains(collector, "(c q d) -> conditions=a p b negated= guards= paths= neural= heads=c q d"));
        }
        SUBCASE("a rule with a negated condition lists the pattern it negates")
        {
            // `¬(P)` is the fact `((P) ~ negation)`, and the rule names that
            // fact as its condition. :negated lists P, the pattern a caller
            // compares with what other rules derive.
            interactive.process("(X ~ man, ¬(X ~ married)) => (X ~ bachelor)");
            collector.clear();
            interactive.process(R"js(%(parts "bachelor"))js");
            CHECK(any_output_contains(collector, "bachelor -> conditions=X ~ man negated=X ~ married guards= paths= neural= heads=X ~ bachelor"));
        }
        SUBCASE("a denial")
        {
            interactive.process("(X ~ man, X ~ woman) => !");
            collector.clear();
            interactive.process(R"js(%(parts "woman"))js");
            CHECK(any_output_contains(collector, " negated= guards= paths= neural= heads=!"));
            CHECK(any_output_contains(collector, "X ~ man"));
            CHECK(any_output_contains(collector, "X ~ woman"));
        }
        SUBCASE("a guard, a path and a neural condition each have their own part, printed as typed")
        {
            // This case pinned the prior printing of the path and the neural
            // condition as the facts their operators build,
            // `(S P279 T) closure one-or-more` and `(A relG B) nn gnet`. That
            // spelling names `closure` and `nn` within the current language,
            // and outside the zelph language the line re-entered as a different
            // statement. The node now prints as the condition is typed.
            interactive.process("(X p Y, X != Y) => (X differs Y)");
            interactive.process("(S pair T, S P279⁺ T) => (S under T)");
            interactive.process("(A relG B, ≈gnet(A relG B)) => (A verifiedG B)");
            collector.clear();
            interactive.process(R"js(%(parts "differs"))js");
            interactive.process(R"js(%(parts "under"))js");
            interactive.process(R"js(%(parts "verifiedG"))js");
            CHECK(any_output_contains(collector, "differs -> conditions=X p Y negated= guards=X != Y paths= neural= heads=X differs Y"));
            CHECK(any_output_contains(collector, "under -> conditions=S pair T negated= guards= paths=S P279⁺ T neural= heads=S under T"));
            CHECK(any_output_contains(collector, "verifiedG -> conditions=A relG B negated= guards= paths= neural=≈gnet(A relG B) heads=A verifiedG B"));
        }
        SUBCASE("a rule a statement only mentions is not listed")
        {
            // A statement concerning a rule mentions it without asserting
            // it, thus it is not in force (as explained in "talking about a
            // rule does not assert it" above), and zelph/rules lists only
            // the rule typed as a rule.
            interactive.process("((X p Y) => (X r Y)) is nice");
            interactive.process("(a p b) => (c q d)");
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "rules=" (length (zelph/rules)) " " (string (((zelph/rules) 0) :rule)))))js");
            CHECK(any_output_contains(collector, "rules=1 (a p b) => (c q d)"));
        }
        SUBCASE("the rules come sorted by node, and nothing is created")
        {
            // Reading the rules does not generate a node, which .stat
            // counts; the sequence follows the order of the rule nodes,
            // consistent across every call.
            process_lines(interactive, R"(
(X p Y) => (X q Y)
(X ~ man, ¬(X ~ married)) => (X ~ bachelor)
(a p b) => (c q d)
)");
            const std::size_t before = node_count(collector, interactive);
            collector.clear();
            interactive.process(R"js(%(zelph/out (string "rules=" (length (zelph/rules)) " sorted=" (apply < (map |($ :rule) (zelph/rules))))))js");
            CHECK(any_output_contains(collector, "rules=3 sorted=true"));
            CHECK(node_count(collector, interactive) == before);
        } });
}

TEST_CASE("rule identity: a rule a statement only mentions is no twin of the rule typed after it")
{
    // A switch mentions the rule it writes once it is active. Typed
    // independently, the identical rule was treated as a duplicate of that
    // mention and rolled back -- since a mention is not in force, nothing
    // derived anything. Only a rule in force serves as a twin; naming the
    // mention is what pattern resolution is for (.explain, the .prune
    // commands).
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("with a collection literal")
        {
            interactive.process("(K is on) => ((X p Y) => (X q @{c}))");
            interactive.process("(X p Y) => (X q @{c})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);

            interactive.process("a p b");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S q O");
            CHECK(answers_contain(collector, "a q @{c}"));
        }
        SUBCASE("without one")
        {
            interactive.process("(K is on) => ((A r B) => (A s B))");
            interactive.process("(X r Y) => (X s Y)");
            interactive.process("c r d");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S s O");
            CHECK(answers_contain(collector, "c s d"));
        } });
}

TEST_CASE("rule identity: a mentioned rule survives a save/load round trip as a mention")
{
    // The distinction is a property of the graph, not of anything the
    // session remembers, so it does not have to be written to the .bin --
    // and cannot be lost by not writing it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const std::filesystem::path out =
            std::filesystem::temp_directory_path() / "zelph_test_mentioned_rule.bin";
        std::filesystem::remove(out);

        interactive.process("((X p Y) => (X q Y)) is nice");
        interactive.process(".save " + out.string());
        interactive.process(".new");
        interactive.process(".load " + out.string());
        interactive.process(".auto-run");

        interactive.process("a p b");
        collector.clear();
        interactive.process("a q Z");
        CHECK_FALSE(any_output_starts_with(collector, "Answer:"));

        std::filesystem::remove(out); });
}

TEST_CASE("rule identity: two rules differing only in their container node are one rule")
{
    // A container is not hash-consed, so two spellings of `@{Y}` are two
    // NODES -- and a node with no fact structure of its own used to be
    // compared by identity, which made the two rules different. That is not a
    // cosmetic problem: rebuild_rule alpha-renames an inner rule and rebuilds
    // its container with the renamed variable, so a rule GENERATOR produced
    // another copy of its rule on every run and the fixpoint never arrived.
    //
    // The container is now read by its members, exactly as a conjunction set
    // is. Both directions are checked: same shape collapses, different
    // members stay apart.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X p Y) => (X likes {Y})");
        collector.clear();
        interactive.process(".list-rules");
        REQUIRE(listed_rules(collector) == 1);

        // Alpha-equivalent, and its container is a different node.
        interactive.process("(A p B) => (A likes {B})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);

        // A container with a DIFFERENT member is a different rule.
        interactive.process("(A p B) => (A likes {A})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 2);

        // Two members are not one member either.
        interactive.process("(A p B) => (A likes {A B})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 3); });
}

TEST_CASE("rule identity: a rule's own collection is compared by its members, any other collection by its node")
{
    // Whether a collection is a rule's own is determined at the moment it
    // is written: one that the rule's text wrote is part of that text and
    // compared by its members, thereby identifying a twin whose collection
    // is a new node. Any other collection constitutes data named by the
    // rule, and is the same term only as the same node: a rule operating on
    // a data collection is not equivalent to a typed rule over a literal
    // sharing the same members.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("in a consequence")
        {
            interactive.process("(X p Y) => (X likes @{c})");
            collector.clear();
            interactive.process(".stat");
            const std::string before = node_count(collector);
            REQUIRE_FALSE(before.empty());

            interactive.process("(X p Y) => (X likes @{c})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
            collector.clear();
            interactive.process(".stat");
            CHECK(node_count(collector) == before);

            interactive.process("a p b");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S likes O");
            CHECK(answers_contain(collector, "a likes @{c}"));
        }
        SUBCASE("in a condition")
        {
            // The condition's collection is written with the rule and named
            // by nothing else, thus a data collection possessing identical
            // members fails to match it.
            interactive.process("(X in @{a b}) => (X flagged yes)");
            interactive.process("(X in @{a b}) => (X flagged yes)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);

            interactive.process("c in @{a b}");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S flagged O");
            CHECK_FALSE(any_output_starts_with(collector, "Answer:"));
        }
        SUBCASE("written by a switch, then typed")
        {
            interactive.process("(K is on) => ((X p Y) => (X q @{c d}))");
            interactive.process("k is on");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            REQUIRE(listed_rules(collector) == 2);

            interactive.process("(A p B) => (A q @{c d})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a rule over a data collection is no twin of a typed literal")
        {
            // The generator binds C to the data collection `@{x}` and writes
            // `(X r Y) => (X in C)`; the typed rule's `@{k}` has the members
            // that the collection acquired, and is its own all the same.
            process_lines(interactive, R"(
d has @{x}
(S has C) => ((k in C) => (S flagged yes))
(S has C) => ((X r Y) => (X in C))
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            const std::size_t before = listed_rules(collector);

            interactive.process("(X r Y) => (X in @{k})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == before + 1);

            interactive.process("a r b");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S in O");
            CHECK(answers_contain(collector, "a in @{k a}"));
        }
        SUBCASE("a rule over a data collection with the literal's members is no twin of it")
        {
            // `@{k}` is data that the generated rule names and writes into;
            // the typed rule's `@{k}` is its own, containing identical members
            // after the generated rule has written X into the data. Compared
            // by members, just as each collection was, the typed rule was
            // deemed a duplicate and rolled back, and the rule a user wrote
            // was gone.
            process_lines(interactive, R"(
d has @{k}
(S has C) => ((X r Y) => (X in C))
)");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(".list-rules");
            const std::size_t before = listed_rules(collector);

            interactive.process("(X r Y) => (X in @{k})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == before + 1);
        } });
}

TEST_CASE("rule identity: a set constant is decided by identity unless a rule's own collection hangs below it")
{
    // A set constant hash-conses over its members, so `{a b}` written twice
    // constitutes a single node, and identity settles it.
    //
    // Every statement contains a node of its own representing the collection
    // a rule writes, and likewise, the set constant around it forms its own
    // node: `{@{c}}` is compared by its members, just as the collection
    // `@{c}` by itself is. Compared by its node, the second statement of the
    // rule was a second rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("only atoms below it")
        {
            interactive.process("(X in {a b}) => (X flagged yes)");
            collector.clear();
            interactive.process(".list-rules");
            REQUIRE(listed_rules(collector) == 1);

            interactive.process("(A in {a b}) => (A flagged yes)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);

            interactive.process("(A in {a c}) => (A flagged yes)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        }
        SUBCASE("a collection below it")
        {
            interactive.process("(X p Y) => (X likes {@{c}})");
            collector.clear();
            interactive.process(".stat");
            const std::string before = node_count(collector);
            REQUIRE_FALSE(before.empty());

            interactive.process("(X p Y) => (X likes {@{c}})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 1);
            collector.clear();
            interactive.process(".stat");
            CHECK(node_count(collector) == before);

            // A collection that includes a different member constitutes
            // a different rule.
            interactive.process("(X p Y) => (X likes {@{d}})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == 2);
        } });
}

// A set constant over a rule's own collection is compared by its members,
// because each statement makes its own collection and, with it, its own set
// constant. A named one remains the constant it is: the instantiation keeps
// it as written (container_plan), so a rule writes that node and no other,
// and two rules writing two such constants over collections with identical
// members write two different facts. Compared by its members, the rule
// writing `k has s2` was taken for the rule writing `k has s1` and rolled
// back, and `k has s2` was lost. The constants are named as a user names a
// node lacking a name: `.node` lists its id, and `.name` names it.
TEST_CASE("rule identity: a named set constant over a rule's own collection is the constant it is")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto rules = [&]
        {
            collector.clear();
            interactive.process(".list-rules");
            return listed_rules(collector);
        };
        // Sorted: in parallel mode, the order of the answers varies across
        // runs.
        const auto answers = [&](const std::string& query)
        {
            interactive.run(true, false, false);
            collector.clear();
            interactive.process(query);
            std::vector<std::string> result = collect_answers(collector);
            std::ranges::sort(result);
            return result;
        };
        // The identifier that `.node` lists for the neighbour of `node`
        // displayed as `label`, or an empty string.
        const auto listed_id = [&](const std::string& node, const std::string& label)
        {
            collector.clear();
            interactive.process(".node " + node);
            const std::string prefix = "- " + label + " (ID ";
            for (const auto& e : collector.events())
                if (const std::size_t at = e.text.find(prefix); at != std::string::npos)
                    return e.text.substr(at + prefix.size(), e.text.find(')', at + prefix.size()) - at - prefix.size());
            return std::string{};
        };
        // Names the set constant `{@{c}}` of the consequence
        // `<subject> has {@{c}}`.
        const auto name_constant = [&](const std::string& subject, const std::string& name)
        {
            const std::string consequence = listed_id(subject, subject + " has {@{c}}");
            REQUIRE_FALSE(consequence.empty());
            const std::string constant = listed_id(consequence, "{@{c}}");
            REQUIRE_FALSE(constant.empty());
            interactive.process(".name " + constant + " " + name);
        };
        process_lines(interactive, R"(
(Z r W) => (k1 has {@{c}})
(Z r W) => (k2 has {@{c}})
)");
        name_constant("k1", "s1");
        name_constant("k2", "s2");

        const std::string over_s1 = "(X q k) => (k has s1)";
        const std::string over_s2 = "(X q k) => (k has s2)";
        std::string       first;
        std::string       second;
        SUBCASE("the rule over s1 first")
        {
            first  = over_s1;
            second = over_s2;
        }
        SUBCASE("the rule over s2 first")
        {
            first  = over_s2;
            second = over_s1;
        }
        interactive.process(first);
        interactive.process(second);
        interactive.process("u q k");
        CHECK(rules() == 4);
        CHECK(answers("k has O") == std::vector<std::string>{"k has s1", "k has s2"}); });
}

TEST_CASE("rule identity: a rule sharing a set constant survives its duplicate's rollback")
{
    // zelph/dedup-rule builds every parsed rule in a scratch cluster and drops
    // that scratch again once it finds an alpha-equivalent twin. So the twin
    // branch performs a REMOVAL, on nodes the duplicate has in common with the
    // rule already in the graph -- and a set constant is shared by
    // construction, since the literal hash-conses.
    //
    // Removing a PartOf fact dooms its container, which is right for an
    // element and wrong for the variable of a pattern. Getting that wrong
    // deleted the ORIGINAL rule and left "No rules found" (002dcbc), and this
    // is the side that made it reachable without any removal command at all:
    // typing the same rule twice was enough.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in {a b}) => (X flagged yes)");
        interactive.process("(A in {a b}) => (A flagged yes)");

        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == 1);
        CHECK_FALSE(any_output_contains(collector, "No rules found"));

        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "a flagged yes"));
        CHECK(answers_contain(collector, "b flagged yes")); });
}

TEST_CASE("rule identity: a rule whose text changed is found by its text as it stands")
{
    // The fingerprint of a rule is calculated just once, upon completion, and
    // stored within an index (Zelph::find_equivalent_rule). A name merge
    // changes its text afterwards without creating a new rule node: it
    // rewrites the members of its collection. This merge marks the index
    // stale, prompting the subsequent lookup to recalculate the fingerprint
    // for every rule. Under the old fingerprint, the rule typed with its
    // current text was a second rule beside it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // bug1 is merged into bug2: the two membership facts combine into a
        // single one, the rule node remains.
        process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
(X filed Y) => (Y in @{bug1 bug2})
.name bug1 en z
.name bug2 en z
)");

        collector.clear();
        interactive.process(".list-rules");
        const std::size_t before = listed_rules(collector);

        interactive.process("(X reported Y) => (Y in @{bug2})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == before); });
}

TEST_CASE("rule identity: a statement that binds a rule's collection or condition set leaves the rule as written")
{
    // A rule over rule structure binds a ground rule's collection or its
    // condition set, and states a membership within it. The membership is
    // directed toward the data term of what it bound, so the ground rule's
    // text remains the original text it was written with: when typed again,
    // it is the same rule, it prints as written, and it fires. If the
    // member had been written directly into the rule's text, the rule would
    // express something not authored by anyone -- `(a p b) => (c in @{d c
    // zz})`, a condition `x y z` that no fact matches -- and the rule, when
    // retyped, would become a distinct second rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a member written into the collection")
        {
            process_lines(interactive, R"(
(a p b) => (c in @{d})
a p b
(G => (S in C)) => (zz in C)
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".list-rules");
            const std::size_t before = listed_rules(collector);
            CHECK(any_output_contains(collector, "(a p b) => (c in @{d c})"));

            interactive.process("(a p b) => (c in @{d})");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == before);

            // The data term holds the members of the rule and the content
            // written by the rule over rules.
            collector.clear();
            interactive.process("S in O");
            auto answers = collect_answers(collector);
            std::ranges::sort(answers);
            CHECK(answers == std::vector<std::string>{"c in @{d c zz}", "d in @{d c zz}", "zz in @{d c zz}"});
        }
        SUBCASE("a condition written into the condition set")
        {
            process_lines(interactive, R"(
(a p b, c q d) => (e r f)
(G => H) => ((x y z) in G)
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process(".list-rules");
            const std::size_t before = listed_rules(collector);

            interactive.process("(a p b, c q d) => (e r f)");
            collector.clear();
            interactive.process(".list-rules");
            CHECK(listed_rules(collector) == before);

            process_lines(interactive, "a p b\nc q d");
            interactive.run(true, false, false);
            collector.clear();
            interactive.process("S r O");
            CHECK(collect_answers(collector) == std::vector<std::string>{"e r f"});
        } });
}

TEST_CASE("rule identity: a generated rule a name merge rebuilt is found as a twin")
{
    // A name merge rebuilds the rules that hold a merged node using fresh
    // identifiers, which fact() never queued for the index. The rule typed
    // with the rebuilt rule's text is that rule, not a second one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(G go H) => ((X p H) => (X G @{c}))
likes go k
)");
        interactive.run(true, false, false);
        process_lines(interactive, R"(
m is here
.name k en x
.name m en x
)");
        collector.clear();
        interactive.process(".list-rules");
        const std::size_t before = listed_rules(collector);
        REQUIRE(before == 2);

        interactive.process("(X p m) => (X likes @{c})");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(listed_rules(collector) == before); });
}
