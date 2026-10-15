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

TEST_CASE("rule identity: a set of conditions is compared by its members wherever it stands")
{
    // The engine reads a conjunction set via its tag, and within a term
    // position -- either the conditions of a rule nested in a consequence, or
    // a set that a consequence refers to -- it remains rule text regardless.
    // Compared there by its node, a counter that each statement builds anew,
    // a nested rule whose conditions constitute such a set never matched its
    // twin, and each statement of the rule added one.
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

TEST_CASE("rule identity: a set of conditions with one member is that condition")
{
    // A rule written in Janet, in the manner that janet.md builds one,
    // holds a condition set consisting of a single member, and the engine
    // reads it as that specific condition. It prints as `((X p Y)) => ...`,
    // which the parser reads as the single condition `(X p Y)` without a
    // set: as two distinct rules, the printed line re-entered as a second
    // rule, and likewise the plain one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(R"(%(let [condition (zelph/collection (zelph/fact 'X "p" 'Y))] (zelph/fact condition "~" "conjunction") (zelph/fact condition "=>" (zelph/fact 'X "likes" (zelph/fact "k" "q" 'Y)))))");
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
