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

// Set constants and collections.
//
// zelph used to have one brace literal doing both jobs, and the two are
// incompatible. A mathematical SET is determined by its members -- the axiom
// of extensionality -- so `{a b}` written twice denotes one and the same set,
// and there is no such operation as adding an element to it; you form a new
// set. A CONTAINER has an identity of its own, and putting something into it
// is exactly what one does with it.
//
// Conflating them cost both: `{a b}` built a fresh container every time, so a
// set literal in a rule condition could never match data written with the same
// literal, and asserting `x in {a b}` silently extended the very set it named,
// leaving a node whose identity said {a b} while it rendered {a b x}.
//
// The two are now separate literals. `{...}` is the set constant, keeping the
// mathematical convention; `@{...}` is the collection, following Janet -- the
// language zelph embeds -- where `{...}` is the immutable struct and `@{...}`
// the mutable table. The marker costs no reserved character: `@` stays an
// ordinary name character and only `@{` is special.

#include "test_helpers.hpp"

#include "network/reasoning.hpp"

#include <algorithm>
#include <filesystem>

using namespace zelph::test;

namespace
{
    // The answers to `query`, sorted.
    std::vector<std::string> sorted_answers(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& query)
    {
        collector.clear();
        interactive.process(query);
        auto answers = collect_answers(collector);
        std::ranges::sort(answers);
        return answers;
    }
}

TEST_CASE("sets: a set constant is its members, so the same literal is the same node")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        // Two occurrences of the literal denote ONE node, which is what a
        // rule needs: the set in its condition has to be the set in the data.
        process_lines(interactive, R"(
p q {a b}
r s {a b}
)");
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "p q {a b}"));

        collector.clear();
        interactive.process("S s O");
        CHECK(answers_contain(collector, "r s {a b}"));

        // Order does not matter -- a set is not a list.
        interactive.process("t u {b a}");
        collector.clear();
        interactive.process("t u O");
        CHECK(answers_contain(collector, "t u {a b}")); });
}

TEST_CASE("sets: a collection has its own identity, so two literals are two containers")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x in @{a b}
y in @{a b}
)");
        // Each literal built its own container, and each took its own member.
        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "x in @{a b x}"));
        CHECK(answers_contain(collector, "y in @{a b y}")); });
}

TEST_CASE("sets: a set constant cannot be extended, and the message says what to write")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        (void)collector;
        CHECK_THROWS_WITH_AS(interactive.process("x in {a b}"),
                             doctest::Contains("set constant cannot be extended"),
                             std::runtime_error);
        CHECK_THROWS_WITH_AS(interactive.process("x in {a b}"),
                             doctest::Contains("@{...}"),
                             std::runtime_error);

        // Stating what already holds is not an extension: `a in {a b}` is
        // true by construction, so it is a no-op rather than an error.
        interactive.process("a in {a b}"); });
}

TEST_CASE("sets: a rule quantifies over the members of a set constant")
{
    // The payoff. This shape derived nothing at all while every literal built
    // its own container -- the rule's set was one nothing else referred to.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X in {a b}) => (X flagged yes)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S flagged yes");
        CHECK(answers_contain(collector, "a flagged yes"));
        CHECK(answers_contain(collector, "b flagged yes"));
        CHECK(collect_answers(collector).size() == 2); });
}

// A name merge, a load, and `.fact-stores off` disarm the store that says
// which nodes hold a variable, and the walk that answers in its stead read
// the membership `A in {b c}`, which the question itself writes, as a
// variable within the set's text. The set then counted as a rule template,
// and each membership within it was dismissed: `A in {b c}` yielded no result
// while `A in O` listed both memberships, and a rule over the set typed
// afterwards derived nothing.
TEST_CASE("sets: a set constant answers for its members after a name merge, a load, or with the fact stores off")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_set_members_disarmed.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process("x has {b c}");
        SUBCASE("a name merge")
        {
            interactive.process("foo q r");
            interactive.process("bar z w");
            interactive.process(".name foo bar");
        }
        SUBCASE(".save and .load")
        {
            interactive.process(".save " + path);
            interactive.process(".load " + path);
            interactive.process(".auto-run");
        }
        SUBCASE(".fact-stores off")
        {
            interactive.process(".fact-stores off");
        }
        REQUIRE_FALSE(interactive.graph()->fact_stores_enabled());

        CHECK(sorted_answers(collector, interactive, "A in {b c}") == std::vector<std::string>{"b in {b c}", "c in {b c}"});
        interactive.process("(X in {b c}) => (X flagged yes)");
        CHECK(sorted_answers(collector, interactive, "S flagged yes") == std::vector<std::string>{"b flagged yes", "c flagged yes"}); });
    std::filesystem::remove(path);
}

// A statement that holds a variable quantifies over the members of a set
// constant just as a variable does: `(X p z) in {(x p z) c}` inquires which
// member matches the form `X p z`, and a literal constructed from such a
// member creates a set pattern, not a set constant (Zelph::set), meaning the
// membership expressed by the question or the rule is never an actual member
// of the set. Read as one, it broke the set's identity, and while the
// membership was present, the set was perceived as a collection: the question
// echoed `(X p z) in @{c (x p z) (X p z)}` and responded with `(x p z) in @{c
// (x p z) (x p z)}`, the rule printed the set in the same way, and as long as
// it existed, so did every statement regarding the set, and `.remove X` took
// the set and `{c (x p z)} label foo` alongside the rule.
TEST_CASE("sets: a membership of a statement that holds a variable quantifies over a set constant without becoming a member")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "x p z\n{(x p z) c} label foo");
        SUBCASE("a question")
        {
            const std::size_t before = node_count(collector, interactive);
            collector.clear();
            interactive.process("(X p z) in {(x p z) c}");
            CHECK(any_output_contains(collector, "(X p z) in {c (x p z)}"));
            CHECK(collect_answers(collector) == std::vector<std::string>{"(x p z) in {c (x p z)}"});
            CHECK(node_count(collector, interactive) == before);
        }
        SUBCASE("a question with the fact stores off")
        {
            interactive.process(".fact-stores off");
            CHECK(sorted_answers(collector, interactive, "(X p z) in {(x p z) c}") == std::vector<std::string>{"(x p z) in {c (x p z)}"});
        }
        SUBCASE("a rule")
        {
            interactive.process("((X p z) in {(x p z) c}) => (X ok yes)");
            CHECK(sorted_answers(collector, interactive, "S ok yes") == std::vector<std::string>{"x ok yes"});
            collector.clear();
            interactive.process(".list-rules");
            CHECK(any_output_contains(collector, "((X p z) in {c (x p z)}) => (X ok yes)"));
        }
        SUBCASE("a rule whose variable is removed")
        {
            interactive.process("((X p z) in {(x p z) c}) => (X ok yes)");
            interactive.process(".remove X");
            collector.clear();
            interactive.process(".list-rules");
            REQUIRE(any_output_contains(collector, "No rules found"));
        }

        CHECK(sorted_answers(collector, interactive, "S label foo") == std::vector<std::string>{"{c (x p z)} label foo"});
        CHECK(sorted_answers(collector, interactive, "A in {(x p z) c}") == std::vector<std::string>{"(x p z) in {c (x p z)}", "c in {c (x p z)}"}); });
}

// Beside a rule whose condition quantifies over the set with a statement, a
// firing neither reconstructs the set nor extends it: it read the rule's
// membership as one of the elements, thus `(X q Y) => (z has {(x p z) c})`
// derived `z has @{(X p z)}`, and `(X q Y) => ((X p z) in {(x p z) c})`
// derived `(a p z) in @{(a p z) (X p z) (X p z)}`, a member written into a set
// constant.
TEST_CASE("sets: a firing neither rebuilds nor extends a set constant that a rule quantifies over with a statement")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "x p z\n((X p z) in {(x p z) c}) => (X ok yes)\na q b");
        SUBCASE("a consequence that names the set")
        {
            interactive.process("(X q Y) => (z has {(x p z) c})");
            CHECK(sorted_answers(collector, interactive, "S has O") == std::vector<std::string>{"z has {c (x p z)}"});
        }
        SUBCASE("a consequence that writes a member into the set")
        {
            collector.clear();
            interactive.process("(X q Y) => ((X p z) in {(x p z) c})");
            CHECK(any_output_contains(collector, "refused: a set constant cannot be extended -- {c (x p z)} IS its members"));
        }
        CHECK(sorted_answers(collector, interactive, "A in {(x p z) c}") == std::vector<std::string>{"(x p z) in {c (x p z)}", "c in {c (x p z)}"});
        CHECK(sorted_answers(collector, interactive, "S ok yes") == std::vector<std::string>{"x ok yes"}); });
}

TEST_CASE("sets: a rule fills a collection while it runs")
{
    // The counterpart: a container is what a rule can insert items INTO.
    // The derived facts name one collection for every firing, using the
    // rule's own term, and it grows.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c p d
(X p Y) => (X in @{Y})
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        // Both subjects landed in one container -- and NOT the rule's pattern
        // variables, which used to be printed as members: `a in {a c Y X}`.
        CHECK(answers_contain(collector, "a in @{a c}"));
        CHECK(answers_contain(collector, "c in @{a c}")); });
}

TEST_CASE("sets: a literal carrying a variable is a container, not a constant")
{
    // Extensionality needs KNOWN members. `{Y}` denotes a different set for
    // every binding of Y, so it cannot be hash-consed and is the container a
    // pattern can be -- which is also what keeps the engine's own conjunction
    // sugar `*{(A rel B) (B rel C)} ~ conjunction` working.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
(X p Y) => (X in {Y})
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "a in @{a}")); });
}

TEST_CASE("sets: a literal carrying a variable in a pattern matches a set constant by its members")
{
    // This literal functions as a container (as shown above), and the pattern
    // compared it against the data through identity, treating it as an
    // atomic unit: `({(X enters C) (C ~ card)} can nil) => (X ~ able)` was
    // never fired, and the query likewise failed to activate, since no
    // statement names the container that a rule or a query writes. Under a
    // binding, the literal denotes the set formed by its substituted members,
    // thus matching a set constant whose members correspond one-to-one with
    // its own. A set constant that differs in a single member, or holds one
    // more or one fewer member, constitutes a distinct set. The rule's
    // condition, the query, a statement that holds the set within a
    // condition, and a condition whose subject holds it all follow the
    // identical path in both anchor modes: the anchors took the container as
    // a node that every candidate contains, and took a subject, whose other
    // variables were bound by an earlier condition, as the single node it
    // denotes, which no statement actually holds.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("anchors on") {}
        SUBCASE("anchors off") { interactive.process(".anchors off"); }

        process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
{(bob enters card2) (card2 ~ ticket)} can nil
{(eve enters card3) (card3 ~ card) (card3 ~ red)} can nil
{(dan enters card4)} can nil
mary believes ({(john enters card1) (card1 ~ card)} can nil)
({(john enters card1) (card1 ~ card)} can nil) noted k
sel is nil
({(X enters C) (C ~ card)} can nil) => (X ~ able)
(Z believes ({(X enters C) (C ~ card)} can nil)) => (Z expects X)
(({(X enters C) (C ~ card)} can nil) noted k) => (X ~ noted)
(sel is Y, ({(X enters C) (C ~ card)} can Y) noted k) => (X ~ found)
)");
        interactive.run(true, false, false);

        CHECK(sorted_answers(collector, interactive, "S ~ able") == std::vector<std::string>{"john ~ able"});
        CHECK(sorted_answers(collector, interactive, "S expects O") == std::vector<std::string>{"mary expects john"});
        CHECK(sorted_answers(collector, interactive, "S ~ noted") == std::vector<std::string>{"john ~ noted"});
        CHECK(sorted_answers(collector, interactive, "S ~ found") == std::vector<std::string>{"john ~ found"});
        CHECK(sorted_answers(collector, interactive, "{(X enters C) (C ~ card)} can nil")
              == std::vector<std::string>{"{(john enters card1) (card1 ~ card)} can nil"});
        CHECK(sorted_answers(collector, interactive, "({(X enters C) (C ~ card)} can nil) noted O")
              == std::vector<std::string>{"({(john enters card1) (card1 ~ card)} can nil) noted k"}); });
}

TEST_CASE("sets: a query echoes its set pattern as the literal that builds it again")
{
    // The query's literal printed as a collection, `@{...}`: typed back, it
    // constructs a collection that matches by its identity, and answered
    // nothing. It outputs exactly the set literal it was originally typed as.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("{(john enters card1) (card1 ~ card)} can nil");

        const auto echo = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            for (const auto& e : collector.events())
                if (e.channel == zelph::io::OutputChannel::Out && normalize(e.text).rfind("Answer:", 0) != 0) return normalize(e.text);
            return std::string{};
        };

        const std::string printed = echo("{(X enters C) (C ~ card)} can nil");
        CHECK(printed.rfind("{(", 0) == 0);
        REQUIRE(collect_answers(collector) == std::vector<std::string>{"{(john enters card1) (card1 ~ card)} can nil"});

        // It is the same query typed back; its members, fresh variables,
        // can appear in a different sequence.
        const auto members = [](const std::string& line)
        {
            std::vector<std::string> out;
            for (std::size_t at = line.find('('); at != std::string::npos; at = line.find('(', at + 1))
                out.push_back(line.substr(at, line.find(')', at) - at + 1));
            std::sort(out.begin(), out.end());
            return out;
        };
        CHECK(members(echo(printed)) == members(printed));
        CHECK(collect_answers(collector) == std::vector<std::string>{"{(john enters card1) (card1 ~ card)} can nil"}); });
}

// A set pattern rendered as data lost a member. In the presence of a ground
// member, a bare variable fails to constitute an element within a data
// collection, thus `{a X} can nil` echoed `{a} can nil`, which re-entered as
// an assertion; furthermore, a ground member within a pattern under `¬` does
// not hold, hence `X go Y, ¬({Y a} can nil)` echoed `¬({Y} can nil), X go Y`,
// a query yielding different answers. A set pattern outputs each member that
// its literal has written.
TEST_CASE("sets: a query echoes every member of its set pattern, a bare variable among them")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "{a b} can nil\n{d e} can nil\nt go k\nu go m\n{m a} can nil");

        // The conditions of a comma list, and of each answer to such a
        // list, are compared in no particular order.
        const auto conditions = [](const std::string& line)
        {
            std::vector<std::string> parts;
            for (std::size_t from = 0;;)
            {
                const std::size_t at = line.find(", ", from);
                parts.push_back(line.substr(from, at - from));
                if (at == std::string::npos) break;
                from = at + 2;
            }
            std::ranges::sort(parts);
            return parts;
        };
        const auto answered = [&]
        {
            std::vector<std::vector<std::string>> out;
            for (const std::string& answer : collect_answers(collector))
                out.push_back(conditions(answer));
            std::ranges::sort(out);
            return out;
        };
        const auto echo = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            for (const auto& e : collector.events())
                if (e.channel == zelph::io::OutputChannel::Out && normalize(e.text).rfind("Answer:", 0) != 0) return normalize(e.text);
            return std::string{};
        };
        // The echo typed back is identical to the original query: it
        // prints as it was printed, gets the same answers, and asserts
        // nothing.
        const auto round_trip = [&](const std::string& query)
        {
            const std::string printed = echo(query);
            const auto        answers = answered();
            CHECK(conditions(echo(printed)) == conditions(printed));
            CHECK(answered() == answers);
            CHECK(sorted_answers(collector, interactive, "S can nil") == std::vector<std::string>{"{a b} can nil", "{a m} can nil", "{d e} can nil"});
            return printed;
        };

        SUBCASE("a ground member beside a bare variable")
        {
            const std::string printed = round_trip("{a X} can nil");
            CHECK((printed == "{a X} can nil" || printed == "{X a} can nil"));
        }
        SUBCASE("a set pattern under a negation")
        {
            const std::string printed = round_trip("X go Y, ¬({Y a} can nil)");
            CHECK(conditions(printed) == std::vector<std::string>{"X go Y", printed.find("{a Y}") != std::string::npos ? "¬({a Y} can nil)" : "¬({Y a} can nil)"});
            collector.clear();
            interactive.process(printed);
            CHECK(collect_answers(collector).size() == 1);
        }
        SUBCASE("a statement that holds the set pattern")
        {
            const std::string printed = round_trip("k has {X a}");
            CHECK((printed == "k has {a X}" || printed == "k has {X a}"));
        }
        // A set pattern among the members of a different set prints through
        // the same path: rendered as data, `{b X}` printed as `{b}`, and the
        // echo `{a {b}} can nil` re-entered as a ground line that asserted
        // itself.
        SUBCASE("a set pattern that is a member of a set pattern")
        {
            const std::string printed = round_trip("{a {b X}} can nil");
            CHECK((printed.find("{b X}") != std::string::npos || printed.find("{X b}") != std::string::npos));
        } });
}

TEST_CASE("sets: a query echoes a collection literal with its marker")
{
    // This marks the boundary of the case above. Within a query, `@{...}`
    // builds a collection. Since a collection is matched based on its identity,
    // the query yields no result in this instance. The echo keeps the `@`
    // marker, which triggers collection creation once more when the echo is
    // re-entered. Had the echo printed as `{...}`, it would be interpreted as
    // the set pattern upon being read back, constituting a different query that
    // does produce an answer.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("{(john enters card1) (card1 ~ card)} can nil");

        collector.clear();
        interactive.process("@{(X enters C) (C ~ card)} can nil");
        std::string printed;
        for (const auto& e : collector.events())
            if (e.channel == zelph::io::OutputChannel::Out && printed.empty()) printed = normalize(e.text);
        CHECK(printed.rfind("@{(", 0) == 0);
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process(printed);
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("sets: a set pattern that a Janet program builds matches as the typed one does")
{
    // `zelph/set` constructs the container a typed literal builds: within a
    // rule's construction, as part of the rule's text, and outside any rule,
    // as a value that a query nonetheless reads as a pattern. The rule has
    // the form that a rule with a single condition takes when its condition
    // list is a tagged set containing exactly one member.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
{(bob enters card2) (card2 ~ ticket)} can nil
)");
        interactive.process(R"js(%(defn one [c] (let [s (zelph/collection c)] (zelph/fact s "~" "conjunction") s)))js");
        interactive.process(R"js(%(zelph/dedup-rule (fn [] (let [X (zelph/var "X") C (zelph/var "C")] (zelph/fact (one (zelph/fact (zelph/set (zelph/fact X "enters" C) (zelph/fact C "~" "card")) "can" "nil")) "=>" (zelph/fact X "~" "able"))))))js");
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, "S ~ able") == std::vector<std::string>{"john ~ able"});

        collector.clear();
        interactive.process(R"js(%(each r (zelph/query (zelph/fact (zelph/set (zelph/fact 'X "enters" 'C) (zelph/fact 'C "~" "card")) "can" "nil")) (zelph/out (string "found=" (zelph/name (get r 'X)) "/" (zelph/name (get r 'C))))))js");
        CHECK(count_outputs_containing(collector, "found=") == 1);
        CHECK(any_output_contains(collector, "found=john/card1")); });
}

TEST_CASE("sets: the members of a set pattern pair with a set constant's in every way that fits")
{
    // `{(X p Y) (X p Z)}` matches `{(a p b) (a p c)}` through two
    // distinct pairings, and the rest of a rule may apply to just one of
    // these pairings: a guard is evaluated following the condition, thus it
    // cannot choose the pairing beforehand. Every pairing constitutes an
    // individual match, just as every pairing among a fact's objects does.
    // Which pairing appears first is determined by node identifiers, hence
    // each of the two guards needs one of them.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x says {(a p b) (a p c)}
(S says {(X p Y) (X p Z)}) => (S pairs Y)
(S says {(X p Y) (X p Z)}, Y != b) => (S picks Y)
(S says {(X p Y) (X p Z)}, Y != c) => (S takes Y)
)");
        interactive.run(true, false, false);

        CHECK(sorted_answers(collector, interactive, "S pairs O") == std::vector<std::string>{"x pairs b", "x pairs c"});
        CHECK(sorted_answers(collector, interactive, "S picks O") == std::vector<std::string>{"x picks c"});
        CHECK(sorted_answers(collector, interactive, "S takes O") == std::vector<std::string>{"x takes b"}); });
}

TEST_CASE("sets: a set pattern holds for the set constant, and a variable for the node it is bound to")
{
    // Only a set constant is matched by its members: it IS its members and
    // cannot be extended. A collection may expand, so a match against its
    // current contents would not remain valid, and `dan has @{(a p b)}` is
    // not an instance of `S has {(X p Y)}`. A literal without a variable is
    // matched through identity, as previously: a ground collection of the
    // rule's text is not a pattern. A variable represents the node it is
    // bound to. bob's literal is a set pattern whose variables all reside
    // within the rule it holds, so the statement over it is data and V binds
    // that node (first: `owns` has the fewest facts); carl's set constant
    // containing a member of a matching shape is a different node. The bound
    // set used to be one that a Janet program wrote with `zelph/set` over
    // `(X p Y)`, but a statement over such a set now holds its variables and
    // binds nothing ("sets: what a query with a set pattern leaves in the
    // graph answers no question").
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
bob owns {((A r B) => (c q d))}
carl has {((a r b) => (c q d))}
carl has {(a p b)}
dan has @{(a p b)}
e has {a b}
(S has {(X p Y)}) => (S fits X)
(S has @{a b}) => (S boxed k)
(bob owns V, carl has V) => (V shared carl)
)");
        interactive.run(true, false, false);

        CHECK(sorted_answers(collector, interactive, "S fits O") == std::vector<std::string>{"carl fits a"});
        CHECK(sorted_answers(collector, interactive, "S boxed O").empty());
        CHECK(sorted_answers(collector, interactive, "S shared O").empty()); });
}

TEST_CASE("sets: a set pattern inside a set constant is matched by its members too")
{
    // A set constant derived from a literal holding a literal with a
    // variable, `{a @{Y}}`, encloses a collection of the rule's text and
    // also retains the rule's variable. It is compared member by member,
    // just as the inner one is.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x has {{b} a}
y has {{b} c}
(S has {{Y} a}) => (S inner Y)
)");
        interactive.run(true, false, false);

        CHECK(sorted_answers(collector, interactive, "S inner O") == std::vector<std::string>{"x inner b"}); });
}

// A set pattern nested within another set pattern, `{{X} a}` as typed or as
// a program constructs it via `zelph/set`, is matched by its members at
// every level, just as the rule's `{{Y} a}` is in the case above. The outer
// literal previously functioned as a set constant over the inner collection:
// compared by identity, it matched only itself, so beside
// `{{b} a} can nil` the query `{{X} a} can nil` answered nothing, and beside
// `k has {a {b q}}` the query `k has {a {b X}}` answered nothing.
TEST_CASE("sets: a set pattern inside a set pattern of a query is matched by its members")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_nested_set_pattern.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
{{b} a} can nil
{{b} c} can nil
k has {a {b q}}
k has {a {b}}
)");
        CHECK(sorted_answers(collector, interactive, "{{X} a} can nil") == std::vector<std::string>{"{a {b}} can nil"});
        CHECK(sorted_answers(collector, interactive, "k has {a {b X}}") == std::vector<std::string>{"k has {a {b q}}"});

        // A program that builds the literal using `zelph/set` asks the same
        // question as the typed line. This question previously found nothing,
        // and the statement built by the program answered the subsequent
        // question as data: `S can nil` produced `{a {X}} can nil` in addition
        // to the data.
        collector.clear();
        interactive.process(R"js(%(zelph/out (string "rows=" (length (zelph/query (zelph/fact (zelph/set (zelph/set (zelph/var "X")) "a") "can" "nil"))))))js");
        CHECK(any_output_contains(collector, "rows=1"));
        CHECK(sorted_answers(collector, interactive, "S can nil") == std::vector<std::string>{"{a {b}} can nil", "{c {b}} can nil"});

        interactive.process(".save " + path);
        interactive.process(".load " + path);
        CHECK(sorted_answers(collector, interactive, "S can nil") == std::vector<std::string>{"{a {b}} can nil", "{c {b}} can nil"}); });
    std::filesystem::remove(path);
}

// The inner literal's variable may be bound by another condition before
// the outer literal is matched, which then denotes a single set for that
// binding. As a set constant over the inner collection, the outer literal
// held no variable, so the statement over it did not depend on Y: with
// `{{b} a} can nil` as the only such fact, the comma list also answered
// `q go c`, and the negation also answered `p go b`.
TEST_CASE("sets: a set pattern inside a set pattern takes the binding another condition gives its variable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
p go b
q go c
{{b} a} can nil
)");
        SUBCASE("as a condition")
        {
            CHECK(sorted_answers(collector, interactive, "X go Y, {{Y} a} can nil").size() == 1);
            CHECK(any_output_contains(collector, "p go b"));
        }
        SUBCASE("under a negation")
        {
            CHECK(sorted_answers(collector, interactive, "X go Y, ¬({{Y} a} can nil)").size() == 1);
            CHECK(any_output_contains(collector, "q go c"));
        } });
}

TEST_CASE("sets: a membership condition puts its subject into the literal it names")
{
    // `(Z in {Y})` writes Z into the literal, just as `Y in @{X}` does within
    // a consequence, and the rule prints `@{Z Y}`. The literal subsequently
    // represents the set containing precisely Z and Y: it matches every
    // membership of a set constant comprising two members, and none of a set
    // constant with a single member, because the members pair one to one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
b in {a b}
c in {c}
(Z in {Y}) => (Z other Y)
)");
        interactive.run(true, false, false);

        CHECK(sorted_answers(collector, interactive, "S other O") == std::vector<std::string>{"a other b", "b other a"}); });
}

TEST_CASE("sets: .explain and .prune-facts reach the fact a set pattern matched")
{
    // The rule's condition holds its own container, so the fact that its
    // instance under the bindings denotes is the one over the set constant
    // the match read. The justification prints this fact, `.explain` finds it
    // as the premise, and `.prune-facts` removes it when applied to a set
    // pattern. When printed with the bindings instantiated, the justification
    // named a collection that the graph does not hold, and read back, the
    // line built one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process(".deductions all");
        collector.clear();
        process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
{(bob enters card2) (card2 ~ ticket)} can nil
({(X enters C) (C ~ card)} can nil) => (X ~ able)
)");
        interactive.run(true, false, false);
        CHECK(any_output_contains(collector, "(john ~ able) ⇐ ({(john enters card1) (card1 ~ card)} can nil)"));

        collector.clear();
        interactive.process(".explain (john ~ able)");
        CHECK(any_output_contains(collector, "└─ {(john enters card1) (card1 ~ card)} can nil  [axiom]"));
        CHECK_FALSE(any_output_contains(collector, "no derivation found"));

        collector.clear();
        interactive.process(".prune-facts {(X enters C) (C ~ card)} can nil");
        CHECK(any_output_contains(collector, "Pruned 1 matching facts."));
        CHECK(sorted_answers(collector, interactive, "{(X enters C) (C ~ card)} can nil").empty());
        CHECK(sorted_answers(collector, interactive, "{(X enters C) (C ~ ticket)} can nil")
              == std::vector<std::string>{"{(bob enters card2) (card2 ~ ticket)} can nil"}); });
}

// `.prune-nodes` removes the element that a single variable binds. When the
// variable resides within a set pattern, the named form found none:
// "the pattern has no variable X ... Its variables are: none", since it
// processed them via collect_variables, which bypassed the set. The form
// lacking a name is the case presented below.
TEST_CASE("sets: .prune-nodes deletes what a variable inside a set pattern binds once it is named")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
{(bob enters card2) (card2 ~ ticket)} can nil
{(eve enters card3)} holds k
)");
        SUBCASE("the variable named")
        {
            collector.clear();
            interactive.process(".prune-nodes X ({(X enters C) (C ~ card)} can nil)");
            CHECK(any_output_contains(collector, "and 1 nodes."));
            CHECK(sorted_answers(collector, interactive, "X enters Y") == std::vector<std::string>{"bob enters card2", "eve enters card3"});
        }
        // The outer literal, built as a set constant over the inner
        // collection, contained no variable, and the command answered
        // "Its variables are: none".
        SUBCASE("the variable named inside a set pattern within a set pattern")
        {
            interactive.process("{{john} a} can nil");
            collector.clear();
            interactive.process(".prune-nodes X ({{X} a} can nil)");
            CHECK(any_output_contains(collector, "and 1 nodes."));
            CHECK(sorted_answers(collector, interactive, "X enters Y") == std::vector<std::string>{"bob enters card2", "eve enters card3"});
        }
        // A removal switches the variable store off, after which the walk
        // through the pattern's structure reads the variables.
        SUBCASE("the variable named after a removal")
        {
            process_lines(interactive, "zz p q\n.remove zz");
            collector.clear();
            interactive.process(".prune-nodes X ({(X enters C) (C ~ card)} can nil)");
            CHECK(any_output_contains(collector, "and 1 nodes."));
            CHECK(sorted_answers(collector, interactive, "X enters Y") == std::vector<std::string>{"bob enters card2", "eve enters card3"});
        } });
}

// Replaces the SUBCASE "the variable not named" from the preceding case,
// which pinned a refusal: the form lacking a name had assigned the single
// variable of `{(X enters card3)} holds k` to neither subject nor object role
// and required it to be named. Before that refusal, it deleted k, the
// object. The form lacking a name now deletes the binding of the pattern's
// sole variable regardless of its nesting level, whether within a set or a
// fact ("pruning: .prune-nodes deletes what its one variable binds, wherever
// the pattern nests it"), so eve is deleted and k stays.
TEST_CASE("sets: .prune-nodes without a name deletes what the one variable inside a set pattern binds")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
{(eve enters card3)} holds k
{(bob enters card2)} holds m
)");
        collector.clear();
        interactive.process(".prune-nodes {(X enters card3)} holds k");
        CHECK(any_output_contains(collector, "and 1 nodes."));
        CHECK(sorted_answers(collector, interactive, "X enters Y") == std::vector<std::string>{"bob enters card2"});
        CHECK(sorted_answers(collector, interactive, "S holds O") == std::vector<std::string>{"{(bob enters card2)} holds m"});
        CHECK_NOTHROW(interactive.process(".node k")); });
}

// A set in which every variable resides within a rule it holds qualifies as
// a container, and a pattern defined over it possesses no variable capable
// of indicating what is removed. The refusal required the variable to be
// given a name, and the named form subsequently answered that the pattern
// lacks such a variable.
TEST_CASE("sets: .prune-nodes over a set whose variables all sit in a rule does not ask for a name")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("x holds {((A r B) => (c q d))}");
        CHECK_THROWS_WITH_AS(interactive.process(".prune-nodes x holds {((A r B) => (c q d))}"),
                             doctest::Contains("no variable outside the rules it mentions"),
                             std::runtime_error);
        CHECK(sorted_answers(collector, interactive, "S holds O").size() == 1); });
}

// A query constitutes a statement that the engine evaluates, and its
// variables transform it into a pattern. Once all of them reside within the
// members of a set pattern, no structure accesses them: following the query
// `{(X enters C) (C ~ card)} can nil`, the inquiry `S can nil` responded with
// `@{(C ~ card) (X enters C)} can nil` beside the two facts, just as it did
// after a query or a statement constructed by a Janet program using
// `zelph/set`. The variables of such a set are now the statement's. A removal
// switches the variable store off for the duration of the session, so
// afterwards the reading is the walk over the graph, which must yield the
// same outcome.
//
// The record that named a set pattern was session state, and following a
// .save and a .load, nothing distinguished the typed query's statement from
// a data collection: it responded once more. A typed query now leaves
// nothing that holds its variables (Zelph::drop_query_text), hence nothing of
// it is saved. What a program writes with zelph/fact remains the program's
// own and stays, and so did the answer after a load: its statement, and a
// typed statement over variables the program binds, `{(,x enters ,c) ...}`,
// answered as data once more. A set pattern is now defined by its id class,
// which a save keeps.
TEST_CASE("sets: what a query with a set pattern leaves in the graph answers no question")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_set_query.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
{(bob enters card2) (card2 ~ ticket)} can nil
)");
        SUBCASE("a typed query") { interactive.process("{(X enters C) (C ~ card)} can nil"); }
        SUBCASE("a typed query after a removal")
        {
            process_lines(interactive, "zz p q\n.remove zz");
            interactive.process("{(X enters C) (C ~ card)} can nil");
        }
        SUBCASE("a typed query, then .save and .load")
        {
            interactive.process("{(X enters C) (C ~ card)} can nil");
            interactive.process(".save " + path);
            interactive.process(".load " + path);
        }
        SUBCASE("a query a program runs")
        {
            interactive.process(R"js(%(zelph/query (zelph/fact (zelph/set (zelph/fact 'X "enters" 'C) (zelph/fact 'C "~" "card")) "can" "nil")))js");
        }
        SUBCASE("a statement a program writes")
        {
            interactive.process(R"js(%(zelph/fact (zelph/set (zelph/fact 'X "enters" 'C) (zelph/fact 'C "~" "card")) "can" "nil"))js");
        }
        SUBCASE("a query a program runs, then .save and .load")
        {
            interactive.process(R"js(%(zelph/query (zelph/fact (zelph/set (zelph/fact 'X "enters" 'C) (zelph/fact 'C "~" "card")) "can" "nil")))js");
            interactive.process(".save " + path);
            interactive.process(".load " + path);
        }
        SUBCASE("a statement a program writes, then .save and .load")
        {
            interactive.process(R"js(%(zelph/fact (zelph/set (zelph/fact 'X "enters" 'C) (zelph/fact 'C "~" "card")) "can" "nil"))js");
            interactive.process(".save " + path);
            interactive.process(".load " + path);
        }
        SUBCASE("a typed statement over variables a program binds")
        {
            process_lines(interactive, "%(def x (zelph/var \"X\"))\n%(def c (zelph/var \"C\"))\n{(,x enters ,c) (,c ~ card)} can nil");
        }
        SUBCASE("a typed statement over variables a program binds, then .save and .load")
        {
            process_lines(interactive, "%(def x (zelph/var \"X\"))\n%(def c (zelph/var \"C\"))\n{(,x enters ,c) (,c ~ card)} can nil");
            interactive.process(".save " + path);
            interactive.process(".load " + path);
        }

        CHECK(sorted_answers(collector, interactive, "S can nil")
              == std::vector<std::string>{"{(bob enters card2) (card2 ~ ticket)} can nil", "{(john enters card1) (card1 ~ card)} can nil"}); });
    std::filesystem::remove(path);
}

// The record of set patterns is what the gate for each new fact and each
// candidate check reads, so an entry that a query's literal left behind kept
// the gate open for the rest of the session. The literal goes with the
// query, and its entry accompanies it.
TEST_CASE("sets: a typed query leaves no record of its set pattern behind")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("{(john enters card1) (card1 ~ card)} can nil");
        REQUIRE_FALSE(interactive.graph()->may_hold_set_patterns());

        collector.clear();
        interactive.process("{(X enters C) (C ~ card)} can nil");
        CHECK(collect_answers(collector) == std::vector<std::string>{"{(john enters card1) (card1 ~ card)} can nil"});
        CHECK_FALSE(interactive.graph()->may_hold_set_patterns()); });
}

// A program that poses every question within a dedicated cluster and drops
// the cluster afterwards, just as `ace` does, removes the question's set
// pattern via the fast path of the drop, and that path left the pattern's
// entry within the record: the gate stayed open for each subsequent fact.
// The fast path now forgets the entry, as the general path has always done.
TEST_CASE("sets: a question asked in a cluster that is then dropped leaves no record of its set pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("{(john enters card1) (card1 ~ card)} can nil");
        REQUIRE_FALSE(interactive.graph()->may_hold_set_patterns());

        collector.clear();
        interactive.process(R"js(%(do (zelph/cluster "question")
                                      (def answers (zelph/query (zelph/fact (zelph/set (zelph/fact 'X "enters" 'C) (zelph/fact 'C "~" "card")) "can" "nil")))
                                      (zelph/cluster "default")
                                      (zelph/cluster-drop "question")
                                      (string "ANSWERS-" (length answers))))js");
        CHECK(any_output_contains(collector, "ANSWERS-1"));
        CHECK_FALSE(interactive.graph()->may_hold_set_patterns()); });
}

// A set can occupy a predicate position, and the first fact over it
// establishes it as a relation type. A query featuring a set pattern in that
// location produces a statement that holds the pattern's variables, because
// Zelph::fact enters the predicate too, yet the candidate verification read
// solely the subject and the objects: a predicate variable bound the set
// pattern, and `a R b` answered `a (@{(X p Y)}) b` in addition to the fact.
TEST_CASE("sets: what a query with a set pattern as its predicate leaves answers no question")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a {(c p d)} b");
        SUBCASE("the stores on") {}
        SUBCASE("after a removal") { process_lines(interactive, "zz p q\n.remove zz"); }

        interactive.process("a {(X p Y)} b");
        CHECK(sorted_answers(collector, interactive, "a R b") == std::vector<std::string>{"a {(c p d)} b"}); });
}

// A set pattern in the predicate position holds the query's variables, just as
// the declaration that its first use authored does: it has been part of the
// query's text from the beginning and accompanies it. The case pins this next
// to the alternative forms a query may assign to its predicate (refer to
// test_stratified.cpp, "an answered query takes back the relation-type
// declaration it wrote for an older predicate").
TEST_CASE("sets: a query with a set pattern as its predicate leaves no declaration of it")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a {(c p d)} b");
        const auto declarations = [&]
        {
            collector.clear();
            interactive.process("P ~ ->");
            std::vector<std::string> answers = collect_answers(collector);
            std::sort(answers.begin(), answers.end());
            return answers;
        };
        const std::size_t              before   = node_count(collector, interactive);
        const std::vector<std::string> declared = declarations();

        interactive.process("a {(X p Y)} b");
        CHECK(node_count(collector, interactive) == before);
        CHECK(declarations() == declared); });
}

// A rule reaches its candidates via a run, not via a question: the run
// initiated immediately after a query is issued, and the full pass for a
// rule written once a removal has switched the variable store off. Over what
// a query with a set pattern left within the graph, `(S can nil) => (S ok y)`
// derived `@{(C ~ card) (X enters C)} ok y`, a fact concerning a pattern that
// subsequently answered as data. The statement, the candidate check, and the
// variable reading each exclude it; it passes only when all three are missing
// at once.
TEST_CASE("sets: a rule does not fire on what a query with a set pattern left in the graph")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("{(john enters card1) (card1 ~ card)} can nil");
        SUBCASE("the run the query sets off")
        {
            interactive.process("(S can nil) => (S ok y)");
            interactive.process("{(X enters C) (C ~ card)} can nil");
        }
        SUBCASE("a rule written after a removal")
        {
            interactive.process("{(X enters C) (C ~ card)} can nil");
            process_lines(interactive, "zz p q\n.remove zz");
            interactive.process("(S can nil) => (S ok y)");
        }

        CHECK(sorted_answers(collector, interactive, "S ok y") == std::vector<std::string>{"{(john enters card1) (card1 ~ card)} ok y"}); });
}

// A set pattern may reside within a member of another literal,
// `{(w says {(X p Y)})}`. The member holds the inner pattern's variables,
// making the outer literal a pattern too: it is paired with the set constant
// having the same shape, and the statement the query leaves holds the
// variables of both. Previously, the outer literal functioned as a set
// constant over the inner one, which answered nothing, and its statement
// answered `S can nil` as data.
TEST_CASE("sets: a set pattern inside a member makes the literal that holds it a pattern")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("{(w says {(a p b)})} can nil");
        SUBCASE("the stores on") {}
        SUBCASE("after a removal") { process_lines(interactive, "zz p q\n.remove zz"); }

        CHECK(sorted_answers(collector, interactive, "{(w says {(X p Y)})} can nil") == std::vector<std::string>{"{(w says {(a p b)})} can nil"});
        CHECK(sorted_answers(collector, interactive, "S can nil") == std::vector<std::string>{"{(w says {(a p b)})} can nil"}); });
}

// A set pattern within a set (`{{X} a}`) and a variable serving as a member
// (`{X a}`) make the query's literal a container that no structure reads its
// variables from. It remained in the graph along with its statement and
// memberships: `S can nil` answered `{a {X}} can nil` and `S in O` returned
// `a in {a}` in addition to the data, and following a .save and a .load, the
// variable in `{X a}` was also lost. What holds the query's variables now
// goes with the query itself, so both questions now answer using only the
// data, in the session and after a round trip.
TEST_CASE("sets: a query's literal with its variable in a member leaves nothing that answers")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_set_member_query.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process("{{b} a} can nil");
        const auto query_answers_only_the_data = [&](const std::string& query)
        {
            interactive.process(query);
            SUBCASE("in the session") {}
            SUBCASE("after .save and .load")
            {
                interactive.process(".save " + path);
                interactive.process(".load " + path);
            }
            CHECK(sorted_answers(collector, interactive, "S can nil") == std::vector<std::string>{"{a {b}} can nil"});
            CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"a in {a {b}}", "b in {b}", "{b} in {a {b}}"});
        };

        SUBCASE("a set pattern inside the set") { query_answers_only_the_data("{{X} a} can nil"); }
        SUBCASE("a variable as a member") { query_answers_only_the_data("{X a} can nil"); } });
    std::filesystem::remove(path);
}

// A collection literal within a query builds a new collection, which no
// fact names, and a ground one held no variable, thus it stayed with its
// membership as data: each asking of "X says @{k}", "(X f Y) => @{k}" or
// the latter, when held by a statement, added two nodes, and `S in O`
// answered every one of them. The collection is the query's text and goes
// with it.
TEST_CASE("sets: a query's collection literal without a variable leaves nothing behind")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "x says @{k}\nx notes ((a f b) => @{k})");
        std::string query;
        SUBCASE("in a statement") { query = "X says @{k}"; }
        SUBCASE("as the consequence of a question about implications") { query = "(X f Y) => @{k}"; }
        SUBCASE("in such a question held by a statement") { query = "x notes ((X f Y) => @{k})"; }

        // A query names what it mentions, `f` being one of them, thus the
        // count is determined following the initial asking.
        interactive.process(query);
        const std::size_t before = node_count(collector, interactive);
        for (int asked = 0; asked < 2; ++asked)
        {
            interactive.process(query);
            CHECK(node_count(collector, interactive) == before);
        }
        CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"k in @{k}"}); });
}

// On the opposite side: a set constant consists of its members, the
// identical node at each instance of asking, and whatever a query writes
// without a variable outside its operators is asserted, just as it is
// anywhere else.
TEST_CASE("sets: a query's set literal without a variable stays as the set it is")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("X says {k}");
        const std::size_t before = node_count(collector, interactive);
        interactive.process("X says {k}");
        CHECK(node_count(collector, interactive) == before);
        CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"k in {k}"}); });
}

// `.node` and `.explain` invoked without an argument take the node associated
// with the most recent answer, which corresponds to the query's statement
// under the answer's bindings. That statement held no variable which a ground
// statement reading could identify, thus it resolved to itself, and `.explain`
// explained `@{(C ~ card) (X enters C)} can nil` as an axiom. Under the
// bindings, a set pattern denotes the set constant it was paired with, and
// the statement denotes the fact over it -- in the session that wrote it and
// following a removal that has switched the variable store off.
TEST_CASE("sets: .node and .explain without an argument address the fact a set pattern answered")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
{(bob enters card2) (card2 ~ ticket)} can nil
)");
        SUBCASE("the stores on") {}
        SUBCASE("after a removal") { process_lines(interactive, "zz p q\n.remove zz"); }

        interactive.process("{(X enters C) (C ~ card)} can nil");
        collector.clear();
        interactive.process(".explain");
        CHECK(any_output_contains(collector, "{(john enters card1) (card1 ~ card)} can nil  [axiom]"));
        CHECK_FALSE(any_output_contains(collector, "@{"));

        interactive.process("{(X enters C) (C ~ card)} can nil");
        collector.clear();
        interactive.process(".node");
        CHECK(any_output_contains(collector, "Representation: {(john enters card1) (card1 ~ card)} can nil"));
        CHECK_FALSE(any_output_contains(collector, "@{")); });
}

// A rule as a member: all its variables can reside within its conditions,
// which are attached to a container -- no hash involved, so a closure over
// subject, predicate, and objects never accesses them. `var_in_closure`
// interprets a `=>` fact's conditions as the rule's text, meaning such a
// rule carries its variables just like its one-condition twin, where the
// condition is the subject itself. The two cases below maintain consistency
// between the twins, both in data and in a rule that fires.
namespace
{
    // Each answer to `query`, required to be `count` in quantity,
    // identifies the set literal it was written as, `{...}`.
    void check_answers_name_collections(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& query, const size_t count)
    {
        collector.clear();
        interactive.process(query);
        const auto answers = collect_answers(collector);
        CHECK(answers.size() == count);
        for (const auto& answer : answers)
            CHECK(answer.find(" {(") != std::string::npos);
    }
}

TEST_CASE("sets: a literal holding a rule whose variables sit in its conditions is a set pattern")
{
    // `{((A r B) => (c q d))}` is a container: the element it holds
    // contains variables. The twin with two conditions was a set constant,
    // and `S in O` answered its membership -- a rule no one asserted,
    // interpreted as data with A and B unbound -- whereas the one-condition
    // literal yielded no result.
    //
    // This case pinned that the answers name a collection, `@{...}`, as proof
    // the literal is not a set constant. The literal functions as a set
    // pattern, the container `{...}` builds, and it prints as that literal:
    // `@{...}` typed back built a collection of a different kind, which matches
    // by its identity. That it is not a set constant is demonstrated by
    // `S in O`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x has {((A r B, A s B) => (c q d))}
y has {((A r B) => (c q d))}
)");

        collector.clear();
        interactive.process("S has O");
        const auto answers = collect_answers(collector);
        CHECK(answers.size() == 2);
        for (const auto& answer : answers)
            CHECK(answer.find(" has {(") != std::string::npos);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("sets: such a literal in a consequence derives nothing for either twin")
{
    // The identical pair of literals within a rule's consequence. A firing
    // builds a value for the literal, and the two-condition instance
    // reached the data as a set constant whose membership `S in O`
    // answered. Either value would hold a rule that keeps A and B, which no
    // condition binds, so neither twin is derived ("derived rules: a nested
    // rule a firing writes into a collection or a mention is not
    // answered").
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes {((A r B) => (c q d))})
(X p Y) => (X hates {((A r B, A s B) => (c q d))})
a p b
e p f
)");
        interactive.run(true, false, false);

        check_answers_name_collections(collector, interactive, "S likes O", 0);
        check_answers_name_collections(collector, interactive, "S hates O", 0);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty()); });
}

// A statement over a set pattern holds the variables of its members outside
// every rule they mention ("sets: what a query with a set pattern leaves in
// the graph answers no question"): the variables of a rule belong exclusively
// to it, and a line whose variables reside solely within the rules it
// mentions constitutes a statement, not a query. Thus, a set holding a
// statement regarding a rule stays the container that the set holding the
// rule itself is (the two cases above), for either twin. This was true before
// that modification as well; it defines the boundary where the change stops.
TEST_CASE("sets: a literal holding a statement about a rule is a container too")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x has {(w says ((A r B, A s B) => (c q d)))}
y has {(w says ((A r B) => (c q d)))}
)");

        check_answers_name_collections(collector, interactive, "S has O", 2); });
}

TEST_CASE("sets: an empty literal of either kind is nil")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
p q {}
p r @{}
)");
        collector.clear();
        interactive.process("p q O");
        CHECK(answers_contain(collector, "p q nil"));

        collector.clear();
        interactive.process("p r O");
        CHECK(answers_contain(collector, "p r nil"));

        // The calls that the two literals stand for, without an element,
        // return that identical node, as stated by janet.md regarding
        // zelph/collection: two calls give one node, and it is nil.
        CHECK(janet_value(collector, interactive, R"((= (zelph/collection) (zelph/collection) (zelph/set) (zelph/lookup "nil")))") == "true"); });
}

TEST_CASE("sets: both kinds survive .save and .load and still print apart")
{
    const std::string path =
        (std::filesystem::temp_directory_path() / "zelph_test_sets.bin").string();

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        process_lines(interactive, R"(
p q {a b}
x in @{c d}
)");
        interactive.process(".save " + path);
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + path);
    interactive.process(".auto-run");

    collector.clear();
    interactive.process("S q O");
    CHECK(answers_contain(collector, "p q {a b}"));

    collector.clear();
    interactive.process("S in O");
    CHECK(answers_contain(collector, "x in @{c d x}"));

    // The identity survives too: the literal still lands on the loaded node
    // rather than building a second set.
    CHECK_THROWS_AS(interactive.process("z in {a b}"), std::runtime_error);

    std::filesystem::remove(path);
}

// A set pattern in which all variables reside within the rule it holds is a
// container, and a statement concerning it constitutes data ("sets: a
// literal holding a rule whose variables sit in its conditions is a set
// pattern"). It prints as the literal that builds it, `{...}`. Following a
// .save and a .load, it printed as a collection, `@{...}`, because the
// record that named it a set pattern did not survive the load.
TEST_CASE("sets: a set pattern still prints as one after .save and .load")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_set_pattern_print.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process("y has {((A r B) => (c q d))}");
        interactive.process(".save " + path);
        interactive.process(".load " + path);
        CHECK(sorted_answers(collector, interactive, "S has O") == std::vector<std::string>{"y has {((A r B) => (c q d))}"}); });
    std::filesystem::remove(path);
}

TEST_CASE("sets: a set pattern in a rule survives .save and .load")
{
    // A rule's set pattern is a collection of its text, as its id says, and a
    // load keeps the id. What the load does not keep is the store specifying
    // which facts hold a variable, hence the members are read from the
    // graph: the loaded rule matches data entered after the load.
    const std::string path =
        (std::filesystem::temp_directory_path() / "zelph_test_set_pattern.bin").string();

    {
        zelph::io::OutputCollector  collector;
        zelph::console::Interactive interactive(collector.sink());
        interactive.process("({(X enters C) (C ~ card)} can nil) => (X ~ able)");
        interactive.process(".save " + path);
    }

    zelph::io::OutputCollector  collector;
    zelph::console::Interactive interactive(collector.sink());
    interactive.process(".load " + path);
    interactive.process(".auto-run");
    interactive.process("{(john enters card1) (card1 ~ card)} can nil");
    interactive.run(true, false, false);

    CHECK(sorted_answers(collector, interactive, "S ~ able") == std::vector<std::string>{"john ~ able"});

    std::filesystem::remove(path);
}

// A set pattern that a program constructs via `zelph/set` outside a rule
// constitutes a value, and its id says so, just as a rule's id says that its
// own is a collection of its text. The record that previously stated it
// instead was lost with each load: the program's question, asked again after
// a .save and a .load, compared the set by identity and found nothing.
TEST_CASE("sets: a set pattern a program built is matched by its members after .save and .load")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_program_set_pattern.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
{(john enters card1) (card1 ~ card)} can nil
{(bob enters card2) (card2 ~ ticket)} can nil
)");
        interactive.process(R"js(%(def q (zelph/fact (zelph/set (zelph/fact 'X "enters" 'C) (zelph/fact 'C "~" "card")) "can" "nil")))js");
        interactive.process(".save " + path);
        interactive.process(".load " + path);

        collector.clear();
        interactive.process(R"js(%(each r (zelph/query q) (zelph/out (string "found=" (zelph/name (get r 'X)) "/" (zelph/name (get r 'C))))))js");
        CHECK(count_outputs_containing(collector, "found=") == 1);
        CHECK(any_output_contains(collector, "found=john/card1")); });
    std::filesystem::remove(path);
}

// A set pattern functions as a collection, and a program may write a
// membership into it, including its own node: the engine accepts
// `(zelph/fact s1 "in" s1)` just as it accepts any other member. Following a
// .save and a .load, the variables of the pattern are read from its members,
// which in turn lead back to the pattern, and the reading looped
// indefinitely until the stack overflowed. Before the pattern kept its kind
// through a load, the statement over it answered as data instead.
TEST_CASE("sets: a set pattern that a program writes into itself is read after .save and .load")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_set_pattern_cycle.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "b first nil\nc second nil");
        interactive.process(R"js(%(def s1 (zelph/set 'X "a")))js");
        SUBCASE("a member of itself")
        {
            interactive.process(R"js(%(zelph/fact s1 "in" s1))js");
        }
        // Read through s1, s2 meets s1 again and has no variable of its own,
        // thus the outcome of that reading does not represent s2's variables:
        // recorded, it would make the statement over s2 read as data.
        SUBCASE("a member of a set pattern it holds")
        {
            interactive.process(R"js(%(def s2 (zelph/set s1 "b")))js");
            interactive.process(R"js(%(zelph/fact s2 "in" s1))js");
            interactive.process(R"js(%(zelph/fact s2 "second" "nil"))js");
        }
        interactive.process(R"js(%(zelph/fact s1 "first" "nil"))js");
        interactive.process(".save " + path);
        interactive.process(".load " + path);

        CHECK(sorted_answers(collector, interactive, "S first nil") == std::vector<std::string>{"b first nil"});
        CHECK(sorted_answers(collector, interactive, "S second nil") == std::vector<std::string>{"c second nil"}); });
    std::filesystem::remove(path);
}

// A program can write a membership into a set pattern, including its own
// node, just as shown in the case above. A statement concerning such a
// pattern, where all variables reside within the rule it holds, answers as
// data, and printing the answer resolved the pattern via its members, which
// lead back to the original: the stack overflowed and the engine crashed.
// The member met again prints as `?`, just as it does in a collection that
// holds itself.
TEST_CASE("sets: a statement over a set pattern that is a member of itself prints the member as ?")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, "y has {((A r B) => (c q d))}\nz has {((C r D) => (e q f))}");
        interactive.process(R"js(%(def c1 (get (first (zelph/query (zelph/fact "y" "has" 'S))) 'S)))js");
        std::vector<std::string> expected;
        SUBCASE("a member of itself")
        {
            interactive.process(R"js(%(zelph/fact c1 "in" c1))js");
            expected = {"y has {? ((A r B) => (c q d))}", "z has {((C r D) => (e q f))}"};
        }
        SUBCASE("a member of a set pattern that is a member of it")
        {
            interactive.process(R"js(%(def c2 (get (first (zelph/query (zelph/fact "z" "has" 'S))) 'S)))js");
            interactive.process(R"js(%(zelph/fact c1 "in" c2))js");
            interactive.process(R"js(%(zelph/fact c2 "in" c1))js");
            expected = {"y has {{? ((C r D) => (e q f))} ((A r B) => (c q d))}",
                        "z has {{? ((A r B) => (c q d))} ((C r D) => (e q f))}"};
        }
        CHECK(sorted_answers(collector, interactive, "S has O") == expected); });
}

// A rule that a program builds over a set pattern it built beforehand refers
// to that pattern as a value and matches it through its members, just as a
// query would. Following a .save and a .load, the record that had named the
// pattern no longer existed: the rule compared the set by identity and did
// not fire on what was written after the load. A set pattern nested within
// the program's set pattern functioned as a set constant over the inner
// collection, meaning the rule's condition held no variable and fired
// immediately, with X unbound: `?? ok y`.
TEST_CASE("sets: a rule over a set pattern a program built fires on what is written after .save and .load")
{
    const std::string path = (std::filesystem::temp_directory_path() / "zelph_test_program_set_rule.bin").string();
    run_both_modes([&](auto& collector, auto& interactive)
                   {
        interactive.process(R"js(%(def x (zelph/var "X")))js");
        std::string              before;
        std::string              after;
        std::string              first;
        std::vector<std::string> fired;
        SUBCASE("a set pattern")
        {
            interactive.process(R"js(%(def c (zelph/var "C")))js");
            interactive.process(R"js(%(def s (zelph/set (zelph/fact x "enters" c) (zelph/fact c "~" "card"))))js");
            before = "{(john enters card1) (card1 ~ card)} can nil";
            after  = "{(eve enters card3) (card3 ~ card)} can nil";
            first  = "john ok y";
            fired  = {"eve ok y", "john ok y"};
        }
        SUBCASE("a set pattern inside a set pattern")
        {
            interactive.process(R"js(%(def s (zelph/set (zelph/set x) "a")))js");
            before = "{{b} a} can nil";
            after  = "{{c} a} can nil";
            first  = "b ok y";
            fired  = {"b ok y", "c ok y"};
        }
        interactive.process(R"js(%(zelph/rule* [(zelph/fact s "can" "nil")] (zelph/fact x "ok" "y")))js");
        interactive.process(before);
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, "S ok O") == std::vector<std::string>{first});

        interactive.process(".save " + path);
        interactive.process(".load " + path);
        interactive.process(".auto-run");
        interactive.process(after);
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, "S ok O") == fired); });
    std::filesystem::remove(path);
}

TEST_CASE("sets: what is printed reads back as what was printed")
{
    // The round trip, for both kinds and nested inside a list.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
p q <@{a b} c>
r s {a b}
)");
        collector.clear();
        interactive.process("S q O");
        CHECK(answers_contain(collector, "p q <@{a b} c>"));

        collector.clear();
        interactive.process("S s O");
        CHECK(answers_contain(collector, "r s {a b}")); });
}

TEST_CASE("sets: a membership fact keeps its own member in the printed container")
{
    // The same node printed two ways depending on which command asked for it.
    // A container left the membership fact it was rendered FROM out of its own
    // element list, so `a in {a b}` came back as `a in {a}` -- and a set
    // constant is its members, so `{a}` is a different node. The query path
    // never had it, because there the container hangs off a pattern rather
    // than off the fact itself.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("x rel {a b}");

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "a in {a b}"));
        CHECK(answers_contain(collector, "b in {a b}"));

        collector.clear();
        interactive.process(".node a in {a b}");
        CHECK(any_output_contains(collector, "Representation: a in {a b}"));
        CHECK_FALSE(any_output_contains(collector, "Representation: a in {a}")); });
}

TEST_CASE("sets: a set constant keeps a member whose membership only a rule's text states")
{
    // The membership `a in {a b}` within the rule presented beneath
    // constitutes a ground pattern in the rule's text until something
    // claims it. A data collection hides such a membership. A set
    // constant is its members, thus retaining this one: printed without
    // it, the rule, its deduction, and the answer named `{b}`, which is
    // a distinct set. The firing's conclusion holds the membership outside
    // a `=>` and claims it, just as the identical statement typed does;
    // previously, it remained a pattern that `S in O` overlooked.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(X p Y) => ((a in {a b}) noted X)");
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X p Y) => ((a in {a b}) noted X)"));
        CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"b in {a b}"});

        collector.clear();
        interactive.process("k p k");
        CHECK(any_output_contains(collector, "((a in {a b}) noted k) ⇐"));
        CHECK(sorted_answers(collector, interactive, "S noted O") == std::vector<std::string>{"(a in {a b}) noted k"});
        CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"a in {a b}", "b in {a b}"}); });
}

TEST_CASE("sets: a membership claimed not to hold is no member of the printed collection")
{
    // `zelph/refute` records a membership as known not to hold, which no
    // query answers. The collection still listed the refuted member,
    // `box has @{d m}`, and the refutation read `¬(m in @{d m})`: the
    // collection that holds m does not hold m. A collection prints the
    // members whose membership holds, regardless of whether a statement
    // wrote it or a firing built it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto refute_m_in = [&](const std::string& subject, const std::string& predicate)
        {
            collector.clear();
            interactive.process("%(zelph/refute \"m\" \"in\" (first (zelph/targets \"" + subject + "\" \"" + predicate + "\")))");
        };
        SUBCASE("a data collection")
        {
            interactive.process("box has @{d}");
            refute_m_in("box", "has");
            CHECK(any_output_contains(collector, "¬(m in @{d})"));
            CHECK(sorted_answers(collector, interactive, "box has O") == std::vector<std::string>{"box has @{d}"});
            CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"d in @{d}"});
        }
        SUBCASE("a collection a firing built")
        {
            process_lines(interactive, R"(
(a p b) => (c q @{d})
a p b
)");
            interactive.run(true, false, false);
            refute_m_in("c", "q");
            CHECK(any_output_contains(collector, "¬(m in @{d})"));
            CHECK(sorted_answers(collector, interactive, "c q O") == std::vector<std::string>{"c q @{d}"});
            CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"d in @{d}"});
        }
        SUBCASE("a typed refutation")
        {
            interactive.process("¬(m in @{d})");
            CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"d in @{d}"});
        } });
}

TEST_CASE("sets: substitution rebuilds a container in a consequence as a set constant")
{
    // instantiate_fact substitutes into FACTS, and a container is not one --
    // its members hang off it as separate PartOf facts, so it has no fact
    // structure to recurse into and came back unchanged. Every derived fact
    // therefore named the RULE's own container and the substituted member
    // never arrived: `(X p Y) => (X likes {Y})` derived `a likes @{Y}` and
    // `c likes @{Y}`, with the rule's template variable in place of the value
    // and ONE object shared by both bindings.
    //
    // The rebuild produces a set constant, because `{Y}` is unable to
    // determine its kind prior to Y being bound: a set constant employs
    // hash-consing, thus re-deriving leads to the identical node and the
    // fixpoint is achieved.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c p d
(X p Y) => (X likes {Y})
(X p Y) => (X holds <{Y} Y>)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(answers_contain(collector, "a likes {b}"));
        CHECK(answers_contain(collector, "c likes {d}"));
        CHECK(collect_answers(collector).size() == 2);

        // Nested inside a list, which is where the recursion has to reach.
        collector.clear();
        interactive.process("S holds O");
        CHECK(answers_contain(collector, "a holds <{b} b>"));
        CHECK(answers_contain(collector, "c holds <{d} d>"));

        // A second run derives nothing new: the set constants are the same
        // nodes, so the fixpoint is reached.
        interactive.run(true, false, false);
        collector.clear();
        interactive.process("S likes O");
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("sets: the kind of a rebuilt literal is read in the rule's text, not in the value bound into it")
{
    // `{C}` with C bound to the data collection D cannot know its kind
    // through the rule, so the determination hinges on whether an instance
    // holds a variable -- within the rule's text, which D, a value, is no
    // part of. A generator over D writes the membership `X in D` into it. A
    // walk that entered D would find X there, and thereafter reconstruct the
    // literal as a collection beside the set constant from earlier:
    // `d likes @{@{x}}` beside `d likes {@{x}}`, and a generated rule over
    // `@{(k in C)}` written again, as `@{(k in D)}` beside `{(k in D)}`,
    // answering `a likes` twice.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a literal in an ordinary rule")
        {
            process_lines(interactive, R"(
d has @{x}
(A has C) => (A likes {C})
)");
            CHECK(sorted_answers(collector, interactive, "S likes O") == std::vector<std::string>{"d likes {@{x}}"});

            interactive.process("(S has C) => ((X r Y) => (X in C))");
            interactive.process(".semi-naive off");
            interactive.run(true, false, false);
            CHECK(sorted_answers(collector, interactive, "S likes O") == std::vector<std::string>{"d likes {@{x}}"});
        }
        SUBCASE("a literal in a generated rule")
        {
            process_lines(interactive, R"(
d has @{x}
(S has C) => ((X q Y) => (X likes @{(k in C)}))
(S has C) => ((X r Y) => (X in C))
)");
            // The membership `k in C` belongs to the literal of the rule that
            // was generated, forming a pattern within its text, so the data
            // collection excludes `k` until a firing claims it: what the
            // firing derives holds the membership outside a `=>`, just as the
            // identical statement typed would, and previously it remained a
            // pattern. The literal stays a set constant regardless.
            CHECK(sorted_answers(collector, interactive, "d has O") == std::vector<std::string>{"d has @{x}"});
            interactive.process("a q b");
            CHECK(sorted_answers(collector, interactive, "S likes O") == std::vector<std::string>{"a likes {(k in @{x k})}"});
        } });
}

TEST_CASE("sets: a ground literal in a consequence is not rebuilt")
{
    // Nothing to substitute: a set constant consists of its members and keeps
    // the node it originally held. A collection that the rule writes into is
    // one node for every firing, the term of the rule's own collection, and
    // it accumulates.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c p d
(X p Y) => (X likes {red green})
(X p Y) => (X in @{bucket})
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(answers_contain(collector, "a likes {red green}"));
        CHECK(answers_contain(collector, "c likes {red green}"));

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "a in @{a c bucket}"));
        CHECK(answers_contain(collector, "c in @{a c bucket}")); });
}

TEST_CASE("sets: writing into a container fills one container that every binding shares")
{
    // `Y in @{X}` says something ABOUT the container, hence its identity
    // must endure substitution -- reconstructing it for each binding would
    // turn the single bucket referenced by the rule into a separate
    // container for each derived fact. Each firing writes into the bucket's
    // term instead, a single node that the data identifies. This serves as
    // the control for the PartOf position in deduce().
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
alice reported bug1
bob reported bug2
(X reported Y) => (Y in @{X})
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "bug1 in @{bug1 bug2}"));
        CHECK(answers_contain(collector, "bug2 in @{bug1 bug2}"));
        CHECK(collect_answers(collector).size() == 2); });
}

TEST_CASE("sets: a rule that would extend a set constant says why, not just `!`")
{
    // Extending a set constant is refused wherever it is attempted, and a rule
    // consequence is the one place where the refusal arrives during reasoning
    // rather than at parse time -- the pattern `X in {a b}` is legitimate, only
    // its instances are not. deduce() turned every fact() refusal into a bare
    // contradiction, so the user was told the knowledge base contradicts itself
    // and got neither the shape that was refused nor the literal to use instead.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
z rel {a b}
q p r
(X p Y) => (X in {a b})
)");
        interactive.run(true, false, false);

        CHECK(has_contradiction(collector));
        CHECK(any_output_contains(collector, "refused: a set constant cannot be extended"));
        CHECK(any_output_contains(collector, "@{...}"));

        // The set itself came through untouched.
        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "a in {a b}"));
        CHECK(answers_contain(collector, "b in {a b}"));
        CHECK(collect_answers(collector).size() == 2); });
}

// The set that a consequence names as a value, while the rule's text also
// holds a membership of a variable within it. The firing counted the variable
// as a member and rebuilt the set under the binding: `a p b` and
// `(X p Y) => (z has {c d}) (X in {c d})` derived `z has {a c d}`, a set that
// no one authored, along with its three memberships, and then refused the
// sibling consequence `a in {c d}`. A negated condition `¬(X in {c d})`
// performed the identical action without any refusal. The rule's text names
// `{c d}`, and that is the set the firing writes, much like a construction
// does already (test_derived_rules.cpp). Whether a firing writes its other
// consequences beside a refused one is left open here: an atomic firing,
// which writes none of them, is a prerequisite zelph has not yet fulfilled,
// thus scenarios involving a refusal merely demand that no extended set
// appears.
TEST_CASE("sets: a firing keeps a set constant as written beside a membership of a variable")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            std::vector<std::string> found = collect_answers(collector);
            std::ranges::sort(found);
            return found;
        };
        interactive.process("a p b");
        SUBCASE("a sibling consequence that is refused")
        {
            collector.clear();
            CHECK_NOTHROW(interactive.process("(X p Y) => (z has {c d}) (X in {c d})"));
            CHECK(any_output_contains(collector, "refused: a set constant cannot be extended"));
            collector.clear();
            interactive.process("S has O");
            CHECK_FALSE(any_output_contains(collector, "{a c d}"));
        }
        SUBCASE("a refused sibling consequence beside the set nested in a value")
        {
            collector.clear();
            CHECK_NOTHROW(interactive.process("(X p Y) => (z says (w has {c d})) (X in {c d})"));
            CHECK(any_output_contains(collector, "refused: a set constant cannot be extended"));
            collector.clear();
            interactive.process("S says O");
            CHECK_FALSE(any_output_contains(collector, "{a c d}"));
        }
        SUBCASE("a negated condition")
        {
            collector.clear();
            CHECK_NOTHROW(interactive.process("(X p Y, ¬(X in {c d})) => (z has {c d})"));
            CHECK_FALSE(has_contradiction(collector));
            CHECK(answers("S has O") == std::vector<std::string>{"z has {c d}"});
        }
        CHECK(answers("S in O") == std::vector<std::string>{"c in {c d}", "d in {c d}"}); });
}

// The identical rejection at the next lower level. A consequence that nests
// the extension, `z says (X in {c d})`, caused it to exit the run rather than
// issue a report: the rule's line failed due to the error, and with auto-run,
// each subsequent input line failed similarly, including `m r n` and `S r O`.
// The refusal is communicated in the same manner as the one previously
// described, and the session continues. A rule that writes a membership
// relation involving a set constant within a statement constitutes a common
// error.
TEST_CASE("sets: a rule whose consequence nests an extension of a set constant says why, and the session goes on")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("a p b");
        std::string rule;
        SUBCASE("nested in the object") { rule = "(X p Y) => (z says (X in {c d}))"; }
        SUBCASE("nested in the subject") { rule = "(X p Y) => ((X in {c d}) says z)"; }
        // A composite predicate is instantiated through a call of its own,
        // positioned after the subject and before the objects.
        SUBCASE("nested in the predicate") { rule = "(X p Y) => (z (X in {c d}) w)"; }
        collector.clear();
        CHECK_NOTHROW(interactive.process(rule));
        CHECK(has_contradiction(collector));
        CHECK(any_output_contains(collector, "refused: a set constant cannot be extended"));

        collector.clear();
        interactive.process("S says O");
        CHECK(collect_answers(collector).empty());

        CHECK_NOTHROW(interactive.process("m r n"));
        collector.clear();
        CHECK_NOTHROW(interactive.process("S r O"));
        CHECK(collect_answers(collector) == std::vector<std::string>{"m r n"});

        collector.clear();
        CHECK_NOTHROW(interactive.process(".run"));
        CHECK(any_output_contains(collector, "refused: a set constant cannot be extended")); });
}

// A refused consequence leaves the graph unchanged. The firing builds what
// the consequence nests before the fact that holds it, thus the refusal of
// the nested membership left `a q b` behind as data that no derivation line
// named: `.explain` called it an axiom, and the rule governing `q` fired on
// it during the same run, producing `a ok b`. A fresh witness contained
// within the refused consequence remained as well, along with the statement
// holding it, and each run added one additional instance of both. The rule
// is the mistake described above.
TEST_CASE("sets: a consequence refused for a nested extension leaves nothing it built")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers = [&](const std::string& query)
        {
            collector.clear();
            interactive.process(query);
            return collect_answers(collector);
        };
        process_lines(interactive, "a p b\n(X q Y) => (X ok Y)\n");

        SUBCASE("the statement it nests")
        {
            collector.clear();
            interactive.process("(X p Y) => ((X q Y) says (X in {c d}))");
            CHECK(any_output_contains(collector, "refused: a set constant cannot be extended"));
            CHECK(answers("S q O").empty());
            CHECK(answers("S ok O").empty());
            CHECK(answers("S says O").empty());
            collector.clear();
            interactive.process(".explain a q b");
            CHECK(any_output_contains(collector, "Fact is not asserted"));
        }
        SUBCASE("a fresh witness it nests on every run")
        {
            interactive.process("(X p Y) => ((Z q X) says (X in {c d}))");
            const std::size_t before = node_count(collector, interactive);
            interactive.process(".run");
            interactive.process(".run");
            CHECK(node_count(collector, interactive) == before);
            CHECK(answers("S q O").empty());
        }
        // The refused firing may be the first of the run to write into the
        // rule's own collection, after which it fills the collection's term
        // (bucket_term). Its member `a` remained within the term that the
        // kept firing had written into. The drop takes the term back, and the
        // next firing fills it anew; `k s m` arrives one iteration later,
        // ensuring the refused firing comes first.
        SUBCASE("a term it filled first and a later firing fills anew")
        {
            process_lines(interactive, ".auto-run\na s {e f}\nk go m\n(X go Y) => (X s Y)\n(X s Y) => ((X in @{c d}) says (X in Y))\n");
            collector.clear();
            interactive.process(".run");
            CHECK(any_output_contains(collector, "refused: a set constant cannot be extended"));
            CHECK(answers("S says O") == std::vector<std::string>{"(k in @{k c d}) says (k in m)"});
        } });
}

TEST_CASE("sets: a printed collection literal says why it cannot be pasted back")
{
    // The round trip that CANNOT hold, and the one place it matters most: a
    // collection is built fresh by every literal, so "@{a b x}" inside a
    // command pattern denotes a new container, never the one the answer line
    // came from. .explain then said "Fact is not asserted" and .prune-facts
    // "Pruned 0" about data the query had just printed -- which reads as the
    // engine contradicting its own output. Nothing can make the literal
    // resolve; what was missing is the sentence that says so.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("x in @{a b}");
        collector.clear();
        interactive.process("S in O");
        REQUIRE(answers_contain(collector, "a in @{a b x}"));

        collector.clear();
        interactive.process(".explain (a in @{a b x})");
        CHECK(any_output_contains(collector, "Fact is not asserted"));
        CHECK(any_event_contains(collector, "builds a NEW container"));

        collector.clear();
        interactive.process(".prune-facts (a in @{a b x})");
        CHECK(any_output_contains(collector, "Pruned 0"));
        CHECK(any_event_contains(collector, "builds a NEW container"));

        // The fact is untouched, and the route the message names works.
        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "a in @{a b x}"));
        collector.clear();
        interactive.process(".explain");
        CHECK(any_output_contains(collector, "[axiom]"));

        // An ordinary absent fact keeps the plain message: the hint is about
        // the literal, not about every failure.
        collector.clear();
        interactive.process(".explain (q nosuchrel r)");
        CHECK(any_output_contains(collector, "Fact is not asserted"));
        CHECK_FALSE(any_event_contains(collector, "builds a NEW container"));

        // A SET CONSTANT is addressable by its literal -- its identity is its
        // members -- so it neither needs the hint nor gets it.
        interactive.process("y rel {c d}");
        collector.clear();
        interactive.process(".explain (c in {c d})");
        CHECK(any_output_contains(collector, "[axiom]"));
        CHECK_FALSE(any_event_contains(collector, "builds a NEW container")); });
}

TEST_CASE("sets: a rule's collection does not drift into the data it gathered")
{
    // A rule and the facts it derived were once stored within ONE
    // container, compelling the renderer to decide which of its members
    // constitute the STATEMENT. It gave priority to the ground members
    // whenever any were present, causing the printed rule to shift in
    // appearance as execution progressed:
    //
    //     (X reported Y) => (Y in @{X})
    //     .list-rules  ->  (X reported Y) => (Y in @{Y X})
    //     alice reported bug1 ... bob reported bug2
    //     .list-rules  ->  (X reported Y) => (Y in @{bug1 bug2})
    //
    // The final line ceases to describe the function of the rule and
    // re-enters as a DIFFERENT rule, with its container initially containing
    // two bug reports. The data now refers to the bucket's term, meaning no
    // firing writes into the rule's own collection, and the printed rule
    // remains unchanged; the data prints the members of the term.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        collector.clear();
        interactive.process("(X reported Y) => (Y in @{X})");
        CHECK(any_output_contains(collector, "(X reported Y) => (Y in @{Y X})"));

        process_lines(interactive, R"(
alice reported bug1
bob reported bug2
)");
        interactive.run(true, false, false);

        // The rule is a FIXPOINT of the rendering: it says the same after
        // firing as before.
        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(X reported Y) => (Y in @{Y X})"));
        CHECK_FALSE(any_output_contains(collector, "@{bug1 bug2})"));

        // ... and the DATA still names what the container holds, which is the
        // half that must not be lost to the fix.
        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "bug1 in @{bug1 bug2}"));
        CHECK(answers_contain(collector, "bug2 in @{bug1 bug2}"));

        // A SET CONSTANT is exempt: its identity is its members, and
        // "(X in {a b})" is how a rule quantifies over them -- the variable
        // the condition adds is the artifact there.
        collector.clear();
        interactive.process("(X in {a b}) => (X flagged yes)");
        CHECK(any_output_contains(collector, "(X in {a b}) => (X flagged yes)"));
        CHECK_FALSE(any_output_contains(collector, "{X}")); });
}

// Within a rule, the rule's own collection outputs each member its text gave
// it: the members the literal lists, whether ground or variable, and the
// subject of a membership that the rule states within it -- the Y in
// `(X p Y) => (Y in @{X})`. While the rule and its data shared a single node,
// the printed rule omitted the ground members whenever a variable was
// present, ensuring the gathered data did not appear in its text, and the
// line was re-entered as a distinct rule: `(X p Y) => (X likes @{Y k})`
// printed `@{Y}`. Firings now write terms and never the rule's own
// collection, so the members it retains are those written by the rule's text,
// and the printed line represents the rule itself. A statement that binds the
// collection via rule structure can still write one of its own members into
// it, and the rule then prints that member too.
namespace
{
    // The rules the `.list-rules` command
    // outputs, arranged in order.
    std::vector<std::string> listed_rule_lines(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive)
    {
        collector.clear();
        interactive.process(".list-rules");
        std::vector<std::string> rules;
        for (const auto& event : collector.events())
        {
            std::istringstream lines(event.text);
            for (std::string line; std::getline(lines, line);)
                if (line.find("=>") != std::string::npos) rules.push_back(normalize(line));
        }
        std::ranges::sort(rules);
        return rules;
    }

    // The rule that `program` writes, or that is typed as `typed`, prints as
    // `echo` upon being written and once more after `data` has made it fire.
    // `echo`, and `typed` as well, when re-entered following the firings add
    // no new rule, and `echo` input into a fresh network using identical
    // `data` answers `query` as the rule did. `query` must possess answers,
    // otherwise that comparison would say nothing.
    void prints_as_written(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& program, const std::string& typed, const std::string& echo, const std::string& data, const std::string& query)
    {
        collector.clear();
        process_lines(interactive, program);
        if (!typed.empty()) interactive.process(typed);
        CHECK(any_output_contains(collector, echo));

        process_lines(interactive, data);
        interactive.run(true, false, false);
        const auto rules = listed_rule_lines(collector, interactive);
        CHECK(std::ranges::count(rules, echo) == 1);
        const auto answers = sorted_answers(collector, interactive, query);
        CHECK_FALSE(answers.empty());

        for (const std::string& line : {echo, typed})
        {
            if (line.empty()) continue;
            CAPTURE(line);
            interactive.process(line);
            CHECK(listed_rule_lines(collector, interactive) == rules);
        }

        interactive.process(".new");
        interactive.process(echo);
        process_lines(interactive, data);
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, query) == answers);
    }
}

TEST_CASE("sets: a rule's own collection prints every member its text wrote, and the line is the rule")
{
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto typed = [&](const std::string& rule, const std::string& echo, const std::string& data, const std::string& query)
        { prints_as_written(collector, interactive, "", rule, echo, data, query); };

        SUBCASE("a ground member beside the statement's variable")
        {
            typed("(X p Y) => (X likes @{Y k})", "(X p Y) => (X likes @{k Y})", "a p b", "S likes O");
        }
        SUBCASE("a fact over the statement's variable beside it")
        {
            typed("(X p Y) => (X likes @{Y (Y q k)})", "(X p Y) => (X likes @{(Y q k) Y})", "a p b", "S likes O");
        }
        SUBCASE("a variable no binding reaches")
        {
            // The term keeps Z, and the data prints it (`b hates @{k Z}`);
            // the rule printed `@{Z}`, a rule without k.
            typed("(X p k) => (X hates @{Z k})", "(X p k) => (X hates @{k Z})", "b p k", "S hates O");
        }
        SUBCASE("a bucket listing a condition's variable and a ground member")
        {
            // `z p k` writes k, a ground member that the term already
            // contains, thus the firing derives nothing new.
            typed("(X p Y) => (Y in @{X k})", "(X p Y) => (Y in @{k Y X})", "z p k", "S in O");
        }
        SUBCASE("a bucket with a ground member only")
        {
            typed("(X is-a-role yes) => (X in @{roles})", "(X is-a-role yes) => (X in @{roles X})", "ann is-a-role yes", "S in O");
        }
        SUBCASE("a collection in a condition")
        {
            // The condition states X's membership, thus X is a member; the
            // rule output as `(X in @{X})`, a different rule. A data
            // collection sharing identical members constitutes a separate
            // node, which the condition fails to match in either network:
            // `S flagged O` answers only `x flagged no`.
            typed("(X in @{a b}) => (X flagged yes)", "(X in @{a b X}) => (X flagged yes)", "c in @{a b}\nx flagged no", "S flagged O");
        }
        SUBCASE("a bucket whose consequence is ground")
        {
            // The consequence is a's membership in the bucket, meaning a
            // qualifies as a member; each firing writes a into the term,
            // which holds it from the start.
            typed("(P q R) => (a in @{R})", "(P q R) => (a in @{a R})", "x q y", "S in O");
        }
        SUBCASE("a bucket of the condition's variable")
        {
            typed("(X reported Y) => (Y in @{X})", "(X reported Y) => (Y in @{Y X})", "alice reported bug1\nbob reported bug2", "S in O");
        }
        SUBCASE("a bucket of ground members")
        {
            typed("(X reported Y) => (Y in @{bug1 bug2})", "(X reported Y) => (Y in @{bug1 bug2 Y})", "alice reported bug3", "S in O");
        }
        SUBCASE("a bucket of the statement's own variable")
        {
            typed("(X reported Y) => (Y in @{Y})", "(X reported Y) => (Y in @{Y})", "alice reported bug3\nbob reported bug4", "S in O");
        }
        SUBCASE("a collection of the rule's text inside a bucket, another rule writing into its term")
        {
            // The generator binds A to the term of the inner collection and
            // writes into it; the rule's own inner collection remains
            // unchanged.
            typed("(X reported Y) => (Y in @{@{c}})", "(X reported Y) => (Y in @{@{c} Y})", "alice reported bug3\n(A in C, A kind box) => ((X p Y) => (X in A))\n(A in C) => (A kind box)\na p k", "S in O");
        }
        SUBCASE("a collection with nothing to substitute, written into by another rule")
        {
            typed("(X p Y) => (X likes @{bucket})", "(X p Y) => (X likes @{bucket})", "(X likes C) => (X in C)\na p b", "S in O");
        } });
}

TEST_CASE("sets: a generated rule's own collection prints every member its construction wrote")
{
    // A construction builds the written rule's collections as its own,
    // thus the generated rule prints them as a typed rule prints its
    // literals, and its line re-enters as that rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto generated = [&](const std::string& program, const std::string& echo, const std::string& data, const std::string& query)
        { prints_as_written(collector, interactive, program, "", echo, data, query); };

        SUBCASE("the generator's binding substituted into a bucket")
        {
            // The `@{H}` from the generator transformed into the bucket
            // `@{k}` of the rule it wrote, which printed `@{Y}`.
            generated("(G go H) => ((X G Y) => (Y in @{H}))\nt go k", "(X t Y) => (Y in @{k Y})", "a t bug1", "S in O");
        }
        SUBCASE("a bucket of the written rule's own variable")
        {
            generated("(G go H) => ((X G Y) => (Y in @{Y}))\nt go k", "(X t Y) => (Y in @{Y})", "a t b1", "S in O");
        }
        SUBCASE("a switch over a bucket rule")
        {
            generated("(K is on) => ((X reported Y) => (Y in @{X}))\nk is on", "(X reported Y) => (Y in @{Y X})", "alice reported bug1", "S in O");
        }
        SUBCASE("the generator's binding substituted inside a collection")
        {
            generated("(G go H) => ((X p H) => (X G @{(Z q H)}))\nlikes go k", "(X p k) => (X likes @{(Z q k)})", "b p k", "S likes O");
        } });
}

TEST_CASE("sets: a pattern printed on its own prints the rule's collection with every member")
{
    // `.node B` lists the membership `B in @{c B}` of the rule below
    // independently, beyond the rule's boundaries. This membership holds a
    // variable, thus constituting a pattern, the rule's text, and the
    // collection prints each member just as it does within the rule; it
    // printed `B in @{B}`, a membership within a collection the rule does
    // not possess.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        interactive.process("(A p B) => (A t @{c B})");
        collector.clear();
        interactive.process(".node B");
        CHECK(any_output_contains(collector, "- B in @{c B} (ID "));
        CHECK_FALSE(any_output_contains(collector, "B in @{B}")); });
}

TEST_CASE("sets: a data collection in a rule's text prints as it does in the data")
{
    // The generated rule names the data collection `@{seed}`, which it
    // reads and modifies, thus the collection holds the rule's X and Y
    // beside the data. These variables are not part of the data: the rule's
    // text printed `@{Y X}` here, denoting a different collection. Printed
    // as the data prints it, the line names the collection based on its
    // contents; re-entering it creates a fresh one, as no literal refers to
    // a pre-existing collection.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
d has @{seed}
(S has C) => ((X in C, X p Y) => (Y in C))
seed p a
a p b
b p c
)");
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"a in @{seed a b c}", "b in @{seed a b c}", "c in @{seed a b c}", "seed in @{seed a b c}"});

        const auto rules = listed_rule_lines(collector, interactive);
        CHECK(std::ranges::count(rules, "((X p Y), (X in @{seed a b c})) => (Y in @{seed a b c})") == 1);
        CHECK(std::ranges::none_of(rules, [](const std::string& rule)
                                   { return rule.find("@{Y X}") != std::string::npos; })); });
}

TEST_CASE("sets: a conjunction set another rule binds keeps the conditions it was written with")
{
    // The rule structure binds the typed rule's conjunction set to G, and
    // the generated rule states X's membership in it. Where a membership is
    // written, the conjunction set represents its data term, meaning neither
    // the construction nor the firing on `k s y` modifies the set: the typed
    // rule prints its two conditions both before and after, while the
    // generated rule writes k into the term. If X and then k had been
    // written directly into the set, they would have functioned as
    // conditions of the typed rule, which no fact matches.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b, c q d) => (e r f)
(G => H) => ((X s y) => (X in G))
)");
        interactive.run(true, false, false);
        const auto rules = listed_rule_lines(collector, interactive);
        CHECK(std::ranges::count(rules, "((c q d), (a p b)) => (e r f)") == 1);
        CHECK(std::ranges::count_if(rules, [](const std::string& rule)
                                    { return rule.starts_with("(X s y) => (X in @{"); })
              == 1);

        interactive.process("k s y");
        interactive.run(true, false, false);
        CHECK(std::ranges::count(listed_rule_lines(collector, interactive), "((c q d), (a p b)) => (e r f)") == 1);
        CHECK(sorted_answers(collector, interactive, "k in O") == std::vector<std::string>{"k in @{k}"}); });
}

TEST_CASE("sets: a conjunction set that is a set constant is bound as the other conjunction sets are")
{
    // The rule's explicit form with ground conditions builds the conjunction
    // set as a set constant: it IS its members, `{(c q d) (a p b)}`. The rule
    // over rules binds it as G, and the generated rule's statement refers to
    // the set's data term, just as with a conjunction set that is a
    // collection: the typed rule keeps its two conditions and fires on them,
    // and the generated rule's firing on `k s y` writes k into the term. Had
    // it been written into the set, X would have served as a third condition
    // of the typed rule.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(*{(a p b) (c q d)} ~ conjunction) => (e r f)
(G => H) => ((X s y) => (X in G))
)");
        interactive.run(true, false, false);
        const auto rules = listed_rule_lines(collector, interactive);
        CHECK(std::ranges::count(rules, "((c q d), (a p b)) => (e r f)") == 1);
        CHECK(std::ranges::count_if(rules, [](const std::string& rule)
                                    { return rule.starts_with("(X s y) => (X in @{"); })
              == 1);

        process_lines(interactive, "k s y\na p b\nc q d");
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, "k in O") == std::vector<std::string>{"k in @{k}"});
        CHECK(sorted_answers(collector, interactive, "S r O") == std::vector<std::string>{"e r f"}); });
}

TEST_CASE("sets: a generated rule's condition on a bound conjunction set reads the set's data term")
{
    // The rule over rules binds the typed rule's conjunction set as C and
    // writes a rule whose condition specifies membership within it. A
    // condition constitutes a membership fact too -- this is how a rule
    // quantifies over the elements -- thus, within the set itself, `X in C`
    // introduced X as a third condition of the typed rule, which no fact
    // matches: the typed rule stopped firing, and the generated rule
    // flagged the typed rule's two conditions. The condition refers to the
    // set's data term instead, the term a second generated rule writes
    // into: the typed rule keeps firing, and the condition reads what is
    // stored in the term.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b, c q d) => (e f g)
(C => (e f g)) => ((X in C) => (X flagged yes))
(C => (e f g)) => ((X r Y) => (X in C))
a p b
c q d
k r k
)");
        interactive.run(true, false, false);
        CHECK(std::ranges::count(listed_rule_lines(collector, interactive), "((c q d), (a p b)) => (e f g)") == 1);
        CHECK(sorted_answers(collector, interactive, "S f O") == std::vector<std::string>{"e f g"});
        CHECK(sorted_answers(collector, interactive, "S flagged O") == std::vector<std::string>{"k flagged yes"}); });
}

TEST_CASE("sets: a firing that writes into a conjunction set writes into its data term")
{
    // The rule over rules binds the conjunction set of the typed rule as C,
    // and the relation of the membership that the generated rule writes is
    // its own variable R: the construction cannot tell that the set is being
    // written into, thus the generated rule holds the set itself. When it
    // fires with R bound to `in`, it writes the membership. Attempting to
    // insert a condition into the set would have altered the typed rule;
    // such a write was denied, as a rule's text is immutable after being
    // written, and this was reported as a contradiction. A firing writes
    // into a conjunction set's data term, just as a statement that binds the
    // set does, and `.explain` finds the firing: when read without a
    // binding, the generated rule's `X R C` might represent a membership, so
    // C stands for its data term as well as for itself.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b, c q d) => (e f g)
in is memrel
m is box
(C => (e f g)) => ((X R Y, R is memrel, Y is box) => (X R C))
k in m
)");
        interactive.run(true, false, false);
        CHECK_FALSE(has_contradiction(collector));
        collector.clear();
        interactive.process(".explain");
        CHECK(last_out_text(collector).starts_with("k in @{k}\n   ├─ "));

        process_lines(interactive, "a p b\nc q d");
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, "k in O") == std::vector<std::string>{"k in @{k}", "k in m"});
        CHECK(sorted_answers(collector, interactive, "S f O") == std::vector<std::string>{"e f g"});
        CHECK(std::ranges::count(listed_rule_lines(collector, interactive), "((c q d), (a p b)) => (e f g)") == 1); });
}

TEST_CASE("sets: a firing that names a conjunction set without writing into it is explained by the rule")
{
    // The identical generated rule, with R bound to `sub`: the firing
    // maintains the set, and derives `k sub` the typed rule's conditions.
    // Read without a binding, `Z R C` can write into the set or name it,
    // and C was matched as the set's data term alone: `.explain` found no
    // rule for the fact and labelled it an axiom. C represents the set as
    // well as its term, and the binding decides which interpretation
    // applies.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b, c q d) => (e f g)
sub is memrel
m is box
(C => (e f g)) => ((X R Y, R is memrel, Y is box, X twin Z) => (Z R C))
j sub m
j twin k
)");
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, "k sub O") == std::vector<std::string>{"k sub {(c q d) (a p b)}"});
        collector.clear();
        interactive.process(".explain");
        CHECK(last_out_text(collector).starts_with("k sub {(c q d) (a p b)}\n   ├─ ")); });
}

TEST_CASE("sets: a firing that names a conjunction set inside a nested statement is explained by the rule")
{
    // As previously described, with the firing's statement nested as the
    // subject of `noted`: (Z R C) noted C. The generated rule's consequence
    // features a statement subject containing a variable predicate, and
    // such a pattern matched nothing, so `.explain` designated the derived
    // fact as an axiom.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(a p b, c q d) => (e f g)
sub is memrel
m is box
(C => (e f g)) => ((X R Y, R is memrel, Y is box, X twin Z) => ((Z R C) noted C))
j sub m
j twin k
)");
        interactive.run(true, false, false);
        CHECK(sorted_answers(collector, interactive, "S noted O") == std::vector<std::string>{"(k sub {(c q d) (a p b)}) noted {(c q d) (a p b)}"});
        collector.clear();
        interactive.process(".explain");
        CHECK(last_out_text(collector).starts_with("(k sub {(c q d) (a p b)}) noted {(c q d) (a p b)}\n   ├─ ")); });
}

TEST_CASE("sets: a rule that reads its own writes into a bound conjunction set comes to an end")
{
    // As previously described, excluding `Y is box`: the generated rule
    // examines each membership, including its own writes. Every attempt to
    // write into the set was refused and recorded as a contradiction, and
    // the next pass re-read that entry as a membership once more, causing
    // nodes to accumulate with each pass and resulting in a never-ending
    // run. Once written into the data term, a pass occurring after the
    // second adds nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto counts = counts_over_passes(collector, interactive, R"(
(a p b, c q d) => (e f g)
in is memrel
(C => (e f g)) => ((X R Y, R is memrel) => (X R C))
k in m
)");
        CAPTURE(counts[0]);
        CAPTURE(counts[1]);
        CHECK(counts[2] == counts[1]);
        CHECK(counts[3] == counts[1]); });
}

// A collection of a rule's own text is a Skolem function symbol: upon firing,
// it is substituted with its corresponding term, a collection whose
// identifier arises from the rule's collection and the binding of the
// statement it stands in. Re-deriving under identical binding leads to the
// same node; a different binding results in a distinct collection; and the
// data never names the rule's own collection.

TEST_CASE("sets: a rule's collection with nothing to substitute is a collection per binding")
{
    // `@{bucket}` appearing in a value position: every firing writes the term
    // associated with its own binding, holding `bucket` as data. It served as
    // the rule's own collection for each binding, thus the two derived facts
    // named one node -- `a shares c` -- and the membership that the data
    // printed answered no query, since it constituted a pattern within the
    // rule's text.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
a p b
c p d
(X p Y) => (X likes @{bucket})
(A likes C, B likes C, A != B) => (A shares B)
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(answers_contain(collector, "a likes @{bucket}"));
        CHECK(answers_contain(collector, "c likes @{bucket}"));

        collector.clear();
        interactive.process("S shares O");
        CHECK(collect_answers(collector).empty());

        collector.clear();
        interactive.process("S in O");
        const auto memberships = collect_answers(collector);
        CHECK(memberships.size() == 2);
        CHECK(std::all_of(memberships.begin(), memberships.end(), [](const std::string& a)
                          { return a == "bucket in @{bucket}"; }));

        const std::size_t before = node_count(collector, interactive);
        interactive.run(true, false, false);
        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("sets: a term that feeds its own rule's binding is a chase that does not end")
{
    // The `@{bucket}` term for `b` transforms, via `(X likes C) => (C p k)`,
    // into the subject of a fresh binding under the same rule, where the
    // term represents yet another collection, and so on: the Skolem chase
    // involving a function symbol applied to its own outcome, which fails to
    // terminate. It diverges as a fresh variable whose witness feeds its own
    // rule does, advancing one level per pass. Each pass introduces an
    // identical count of nodes, so a run toward the fixpoint would never
    // conclude, and four single passes illustrate this. The rule's own
    // collection represented every binding, and the chase stopped after a
    // single step, at the price of the data naming the rule's text.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        for (const std::string collection : {"@{bucket}", "@{(Z q Y)}"})
        {
            CAPTURE(collection);
            interactive.process(".new");
            const auto counts = counts_over_passes(collector, interactive, "(X p Y) => (X likes " + collection + ")\n(X likes C) => (C p k)\nb p k");
            REQUIRE(counts[1] > counts[0]);
            const std::size_t step = counts[1] - counts[0];
            CHECK(counts[2] - counts[1] == step);
            CHECK(counts[3] - counts[2] == step);
        } });
}

TEST_CASE("sets: a firing writes into the term of the rule's bucket, which holds its ground members")
{
    // The rule writes to `@{bug1 bug2}`, its bucket. Each firing writes
    // into the bucket's term, creating a collection for every firing,
    // beginning with the bucket's ground members as data. The rule's own
    // collection received the data before, and printed
    // `bug3 in @{bug1 bug2 bug3}` while `S in O` answered bug3 alone: the
    // literal's memberships were patterns of the rule's text, thus the
    // printed collection held members the system had rejected.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
bob reported bug4
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).size() == 4);
        for (const std::string member : {"bug1", "bug2", "bug3", "bug4"})
            CHECK(answers_contain(collector, member + " in @{bug1 bug2 bug3 bug4}")); });
}

// This test received the name "sets: a rule over marking facts that names a
// bucket leaves its data in one term", and in its first case the rule over
// marking facts bound the typed bucket via the mark of `bug1 in @{bug1 bug2}`
// and wrote `@{bug1 bug2} flagged yes`. A mark represents the engine's
// reference to a pattern and matches no condition because "rule patterns:
// the mark of a pattern answers no query and satisfies no rule", hence this
// rule now binds no bucket, and the bucket's data remains within its term as
// previously.
TEST_CASE("sets: a rule over marking facts binds no bucket, and the bucket's data stays in one term")
{
    // The rule over the engine's marking facts bound C to the bucket via
    // `(bug1 in B) ~ "rule pattern"` and wrote `B flagged yes`, a
    // statement that identifies the rule's own collection. If such a
    // statement were what made a collection data, the bucket would be a
    // value from the subsequent run on, later firings would write into it
    // beside the term, and the bucket's data would be split between the
    // two. The id decides: the firings keep writing into the term, the
    // typed rule re-entered remains the same rule, and a typed bucket
    // behaves identically to a generated one.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("a typed bucket")
        {
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
"rule pattern" kind meta
((M in C) ~ K, K kind meta) => (C flagged yes)
)");
            CHECK(sorted_answers(collector, interactive, "S flagged O").empty());

            interactive.process("bob reported bug4");
            CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"bug1 in @{bug1 bug2 bug3 bug4}", "bug2 in @{bug1 bug2 bug3 bug4}", "bug3 in @{bug1 bug2 bug3 bug4}", "bug4 in @{bug1 bug2 bug3 bug4}"});

            const std::size_t before = node_count(collector, interactive);
            interactive.process("(X reported Y) => (Y in @{bug1 bug2})");
            CHECK(node_count(collector, interactive) == before);
        }
        SUBCASE("a generated bucket and its typed twin")
        {
            process_lines(interactive, R"(
(G go H) => ((X G Y) => (Y in @{H}))
t go k
(X u Y) => (Y in @{m})
a t b1
c u b2
"rule pattern" kind meta
((M in C) ~ K, K kind meta) => (C flagged yes)
a t b3
c u b4
)");
            CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"b1 in @{k b1 b3}", "b2 in @{m b2 b4}", "b3 in @{k b1 b3}", "b4 in @{m b2 b4}", "k in @{k b1 b3}", "m in @{m b2 b4}"});
        } });
}

TEST_CASE("sets: a rule over rule structure that names a bucket leaves it the rule's bucket, in either order")
{
    // `(G => (S in C)) => (C noted yes)` binds C to the bucket of the ground
    // rule and `@{d} noted yes` is recorded. If a statement naming a
    // collection were what transformed it into data, the answers and the
    // rule's echo would depend on whether that statement occurred before or
    // after the rule fired.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        SUBCASE("the rule over rules first")
        {
            process_lines(interactive, R"(
(a p b) => (c in @{d})
(G => (S in C)) => (C noted yes)
a p b
)");
        }
        SUBCASE("the fact first")
        {
            process_lines(interactive, R"(
(a p b) => (c in @{d})
a p b
(G => (S in C)) => (C noted yes)
)");
        }
        CHECK(sorted_answers(collector, interactive, "S in O") == std::vector<std::string>{"c in @{d c}", "d in @{d c}"});
        CHECK(sorted_answers(collector, interactive, "S noted O") == std::vector<std::string>{"@{d c} noted yes"});

        collector.clear();
        interactive.process(".list-rules");
        CHECK(any_output_contains(collector, "(a p b) => (c in @{d c})")); });
}

TEST_CASE("sets: a bucket's data stays in one term whatever names the bucket or its members")
{
    // The term of the bucket is determined exclusively by the bucket's id. A
    // rule over rules that binds the bucket, or the consequence that writes
    // into it, writes a statement that names it; a rule over rules with
    // variables binds nothing within a rule that contains variables of its
    // own; and a name merge between two literal members of the bucket
    // reconstitutes their memberships, not the bucket itself. None of these
    // actions relocates the data from a later firing out of the term.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        const auto answers_of = [&](const std::string& query)
        {
            return sorted_answers(collector, interactive, query);
        };
        SUBCASE("a statement about the bucket, before and after the firings")
        {
            process_lines(interactive, R"(
(a p b) => (c in @{d})
(G => (S in C)) => (C noted yes)
)");
            CHECK(answers_of("S noted O") == std::vector<std::string>{"@{d c} noted yes"});
            interactive.process("a p b");
            CHECK(answers_of("S in O") == std::vector<std::string>{"c in @{d c}", "d in @{d c}"});
            interactive.process("x p y");
            CHECK(answers_of("S in O") == std::vector<std::string>{"c in @{d c}", "d in @{d c}"});
        }
        SUBCASE("a statement about the consequence that writes into the bucket")
        {
            process_lines(interactive, R"(
(a p b) => (c in @{d})
a p b
)");
            CHECK(answers_of("S in O") == std::vector<std::string>{"c in @{d c}", "d in @{d c}"});
            interactive.process("(G => H) => (H noted yes)");
            CHECK(answers_of("S noted O") == std::vector<std::string>{"(c in @{d c}) noted yes"});
            CHECK(answers_of("S in O") == std::vector<std::string>{"c in @{d c}", "d in @{d c}"});
        }
        SUBCASE("a rule over rules after a firing, which binds no rule with variables")
        {
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
(C => (V in B)) => (B watched yes)
)");
            CHECK(answers_of("S watched O").empty());
            interactive.process("bob reported bug4");
            CHECK(answers_of("S in O") == std::vector<std::string>{"bug1 in @{bug1 bug2 bug3 bug4}", "bug2 in @{bug1 bug2 bug3 bug4}", "bug3 in @{bug1 bug2 bug3 bug4}", "bug4 in @{bug1 bug2 bug3 bug4}"});
        }
        SUBCASE("a rule over rules before the first firing")
        {
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
(C => K) => (K seen yes)
)");
            CHECK(answers_of("S seen O").empty());
            interactive.process("alice reported bug3");
            CHECK(answers_of("S in O") == std::vector<std::string>{"bug1 in @{bug1 bug2 bug3}", "bug2 in @{bug1 bug2 bug3}", "bug3 in @{bug1 bug2 bug3}"});
        }
        SUBCASE("a name merge of two literal members")
        {
            process_lines(interactive, R"(
(X reported Y) => (Y in @{bug1 bug2})
alice reported bug3
.name bug1 en z
.name bug2 en z
bob reported bug4
)");
            CHECK(answers_of("S in O") == std::vector<std::string>{"bug2 in @{bug2 bug3 bug4}", "bug3 in @{bug2 bug3 bug4}", "bug4 in @{bug2 bug3 bug4}"});
        } });
}

TEST_CASE("sets: a collection with a variable is one collection per statement instance")
{
    // `@{(Z q Y)}` holds the statement's Y and a Z that remains inaccessible
    // to any binding. The term is identified through the binding associated
    // with the statement -- X and Y -- ensuring that no two statement
    // instances ever share it. The rule's own collection was incorporated
    // into the data with Y left unbound, each binding named it, and a rule
    // writing into it fed the rule's text.
    SUBCASE("two bindings, two collections")
    {
        run_both_modes([](auto& collector, auto& interactive)
                       {
            process_lines(interactive, R"(
(X p Y) => (X likes @{(Z q Y)})
(X likes C, Y likes C, X != Y) => (X shares Y)
a p k
b p k
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector).size() == 2);
            CHECK(answers_contain(collector, "a likes @{(Z q k)}"));
            CHECK(answers_contain(collector, "b likes @{(Z q k)}"));

            collector.clear();
            interactive.process("S shares O");
            CHECK(collect_answers(collector).empty()); });
    }
    SUBCASE("a collection with nothing to substitute, written into by another rule")
    {
        run_both_modes([](auto& collector, auto& interactive)
                       {
            process_lines(interactive, R"(
(X p Y) => (X likes @{(Z q k)})
(X likes C, Y likes C, X != Y) => (X shares Y)
(X likes C) => (X in C)
a p k
b p k
)");
            interactive.run(true, false, false);

            collector.clear();
            interactive.process("S shares O");
            CHECK(collect_answers(collector).empty());

            collector.clear();
            interactive.process("S likes O");
            CHECK(answers_contain(collector, "a likes @{a (Z q k)}"));
            CHECK(answers_contain(collector, "b likes @{b (Z q k)}")); });
    }
    SUBCASE("a value fed back into the collection stays where it was written")
    {
        run_both_modes([](auto& collector, auto& interactive)
                       {
            process_lines(interactive, R"(
(X p Y) => (X likes @{(Z q Y)})
(X likes C) => (X in C)
b p k
)");
            interactive.run(true, false, false);
            const std::size_t before = node_count(collector, interactive);
            interactive.run(true, false, false);
            interactive.run(true, false, false);
            CHECK(node_count(collector, interactive) == before);

            collector.clear();
            interactive.process("S likes O");
            CHECK(collect_answers(collector).size() == 1);
            CHECK(answers_contain(collector, "b likes @{b (Z q k)}")); });
    }
}

TEST_CASE("sets: a collection inside a collection keeps its kind")
{
    // Both the inner and outer literal transform into terms. Since
    // `@{@{Y}}` was written as a collection around a collection, its kind
    // is known; the inner cannot know its kind until Y is bound, and then
    // it takes the form `{k}`.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes @{@{(Z q Y)}})
(X r Y) => (X loves @{@{c}})
(X s Y) => (X wants @{@{Y}})
b p k
b r k
b s k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S likes O");
        CHECK(answers_contain(collector, "b likes @{@{(Z q k)}}"));

        collector.clear();
        interactive.process("S loves O");
        CHECK(answers_contain(collector, "b loves @{@{c}}"));

        collector.clear();
        interactive.process("S wants O");
        CHECK(answers_contain(collector, "b wants @{{k}}")); });
}

TEST_CASE("sets: a bucket that holds only the rule's variable is filled by every firing")
{
    // `@{Y}` is written into by the statement that names it: its term holds
    // what the firings write. The rule's own collection received the reports
    // before, and typing the rule again produced a second rule, as the first
    // one's text had shifted beneath it.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X reported Y) => (Y in @{Y})
alice reported bug3
bob reported bug4
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).size() == 2);
        CHECK(answers_contain(collector, "bug3 in @{bug3 bug4}"));
        CHECK(answers_contain(collector, "bug4 in @{bug3 bug4}"));

        const std::size_t before = node_count(collector, interactive);
        interactive.process("(X reported Y) => (Y in @{Y})");
        CHECK(node_count(collector, interactive) == before); });
}

TEST_CASE("sets: a collection of the rule's text inside a bucket enters its term as a term")
{
    // The bucket's ground members are instantiated in the same manner as
    // any part of a consequence, causing the inner literal to transform
    // into a term in its own right: the bucket's term holds no rule text,
    // and the inner literal's member constitutes data belonging to the
    // inner term. Previously, the inner literal itself was part of the
    // data, where its membership -- a pattern of the rule's text --
    // answered nothing.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X reported Y) => (Y in @{@{c}})
alice reported bug3
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(answers_contain(collector, "bug3 in @{bug3 @{c}}"));
        CHECK(answers_contain(collector, "@{c} in @{bug3 @{c}}"));
        CHECK(answers_contain(collector, "c in @{c}")); });
}

TEST_CASE("sets: a rule with variables inside a bucket stays out of its term")
{
    // A member of the bucket whose text holds a variable -- specifically,
    // the nested rule, with its variables located within its conditions --
    // lacks an instance under the bucket's empty key and remains within the
    // rule's text. In the data, it was answered with A and B unbound.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X reported Y) => (Y in @{((A p B, A r B) => (c q d)) k})
alice reported bug3
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).size() == 2);
        CHECK(answers_contain(collector, "bug3 in @{k bug3}"));
        CHECK(answers_contain(collector, "k in @{k bug3}")); });
}

TEST_CASE("sets: a collection a firing built prints the variable it keeps")
{
    // Z is not a variable within the rule's statement outside the
    // collection, thus no binding extends to it and it stays a member of the
    // term. The term constitutes data and prints what it contains; omitting
    // the variable resulted in a printed collection that holds fewer
    // elements than it actually does.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p k) => (X hates @{Z k})
b p k
)");
        interactive.run(true, false, false);

        collector.clear();
        interactive.process("S hates O");
        CHECK(answers_contain(collector, "b hates @{k Z}")); });
}
