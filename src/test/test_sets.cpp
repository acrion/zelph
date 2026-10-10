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

// A rule as a member: all its variables can reside within its conditions,
// which are attached to a container -- no hash involved, so a closure over
// subject, predicate, and objects never accesses them. `var_in_closure`
// interprets a `=>` fact's conditions as the rule's text, meaning such a
// rule carries its variables just like its one-condition twin, where the
// condition is the subject itself. The two cases below maintain consistency
// between the twins, both in data and in a rule that fires.
namespace
{
    // Each answer to `query`, required to be `count` in number, names
    // a collection literal.
    void check_answers_name_collections(zelph::io::OutputCollector& collector, const zelph::console::Interactive& interactive, const std::string& query, const size_t count)
    {
        collector.clear();
        interactive.process(query);
        const auto answers = collect_answers(collector);
        CHECK(answers.size() == count);
        for (const auto& answer : answers)
            CHECK(answer.find("@{") != std::string::npos);
    }
}

TEST_CASE("sets: a literal holding a rule whose variables sit in its conditions is a container")
{
    // `{((A r B) => (c q d))}` is a container: the element it holds
    // contains variables. The twin with two conditions was a set constant,
    // and `S in O` answered its membership -- a rule no one asserted,
    // interpreted as data with A and B unbound -- whereas the one-condition
    // literal yielded no result.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
x has {((A r B, A s B) => (c q d))}
y has {((A r B) => (c q d))}
)");

        check_answers_name_collections(collector, interactive, "S has O", 2);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty()); });
}

TEST_CASE("sets: such a literal in a consequence is a container in what the rule derives")
{
    // The identical pair of literals within a rule's consequence. A
    // firing writes the literal into the data, and the two-condition
    // instance reached it as a set constant whose membership `S in O`
    // answered.
    run_both_modes([](auto& collector, auto& interactive)
                   {
        process_lines(interactive, R"(
(X p Y) => (X likes {((A r B) => (c q d))})
(X p Y) => (X hates {((A r B, A s B) => (c q d))})
a p b
e p f
)");
        interactive.run(true, false, false);

        check_answers_name_collections(collector, interactive, "S likes O", 2);
        check_answers_name_collections(collector, interactive, "S hates O", 2);

        collector.clear();
        interactive.process("S in O");
        CHECK(collect_answers(collector).empty()); });
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
        CHECK(answers_contain(collector, "p r nil")); });
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
a q b
)");
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

TEST_CASE("sets: a rule over marking facts that names a bucket leaves its data in one term")
{
    // The rule over the engine's marking facts binds C to the bucket via
    // `(bug1 in B) ~ "rule pattern"` and writes `B flagged yes`, a
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
            CHECK(sorted_answers(collector, interactive, "S flagged O") == std::vector<std::string>{"@{bug1 bug2} flagged yes"});

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
